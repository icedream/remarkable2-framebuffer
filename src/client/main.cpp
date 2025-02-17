#include <dlfcn.h>
#include <iostream>
#include <libgen.h>
#include <linux/fb.h>
#include <linux/ioctl.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <fstream>
#include <unistd.h>
#include <time.h>

#include <vector>
#include <QByteArray>
#include <QRect>

#include "../shared/ipc.cpp"
#include "../shared/config.h"

#ifndef NO_XOCHITL
#include "frida/frida-gum.h"
#endif

#define SEM_WAIT_TIMEOUT 200000000 /* 200 * 1000 * 1000, e.g. 200ms */

constexpr auto msg_q_id = 0x2257c;
swtfb::ipc::Queue MSGQ(msg_q_id);

uint16_t *SHARED_BUF = nullptr;

constexpr auto BYTES_PER_PIXEL = sizeof(*SHARED_BUF);

bool IN_XOCHITL = false;
bool DO_WAIT_IOCTL = true;
bool ON_RM2 = false;

extern "C" {

__attribute__((constructor))
void init() {
  std::ios_base::Init i;

  std::ifstream device_id_file{"/sys/devices/soc0/machine"};
  std::string device_id;
  std::getline(device_id_file, device_id);

  if (device_id == "reMarkable 2.0") {
    SHARED_BUF = swtfb::ipc::get_shared_buffer();
    ON_RM2 = true;

    constexpr auto VERSION = "0.1";
    setenv("RM2FB_SHIM", VERSION, true);

    if (getenv("RM2FB_ACTIVE") != nullptr) {
        setenv("RM2FB_NESTED", "1", true);
    } else {
        setenv("RM2FB_ACTIVE", "1", true);
    }

    if (getenv("RM2FB_NO_WAIT_IOCTL") != nullptr) {
      DO_WAIT_IOCTL = false;
    }
  }
}

__attribute__((visibility("default")))
void _ZN6QImageC1EiiNS_6FormatE(void *that, int x, int y, int f) {
  static bool FIRST_ALLOC = true;
  static const auto qImageCtor = (void (*)(void *, int, int, int))dlsym(
      RTLD_NEXT, "_ZN6QImageC1EiiNS_6FormatE");
  static const auto qImageCtorWithBuffer = (void (*)(
      void *, uint8_t *, int32_t, int32_t, int32_t, int, void (*)(void *),
      void *))dlsym(RTLD_NEXT, "_ZN6QImageC1EPhiiiNS_6FormatEPFvPvES2_");

  if (ON_RM2 && IN_XOCHITL && x == swtfb::WIDTH && y == swtfb::HEIGHT && FIRST_ALLOC) {
    fprintf(stderr, "REPLACING THE IMAGE with shared memory\n");

    FIRST_ALLOC = false;
    qImageCtorWithBuffer(that, (uint8_t *)SHARED_BUF, swtfb::WIDTH,
                         swtfb::HEIGHT, swtfb::WIDTH * BYTES_PER_PIXEL, f,
                         nullptr, nullptr);
    return;
  }
  qImageCtor(that, x, y, f);
}

__attribute__((visibility("default")))
int open64(const char *pathname, int flags, mode_t mode = 0) {
  if (ON_RM2 && !IN_XOCHITL) {
    if (pathname == std::string("/dev/fb0")) {
      return swtfb::ipc::SWTFB_FD;
    }
  }

  static const auto func_open = (int (*)(const char *, int, mode_t))
    dlsym(RTLD_NEXT, "open64");

  return func_open(pathname, flags, mode);
}

__attribute__((visibility("default")))
int open(const char *pathname, int flags, mode_t mode = 0) {
  if (ON_RM2 && !IN_XOCHITL) {
    if (pathname == std::string("/dev/fb0")) {
      return swtfb::ipc::SWTFB_FD;
    }
  }

  static const auto func_open = (int (*)(const char *, int, mode_t))
    dlsym(RTLD_NEXT, "open");

  return func_open(pathname, flags, mode);
}

__attribute__((visibility("default")))
int close(int fd) {
  if (ON_RM2 && fd == swtfb::ipc::SWTFB_FD) {
    return 0;
  }

  static const auto func_close = (int (*)(int))dlsym(RTLD_NEXT, "close");

  return func_close(fd);
}

__attribute__((visibility("default")))
int ioctl(int fd, unsigned long request, char *ptr) {
  if (ON_RM2 && !IN_XOCHITL && fd == swtfb::ipc::SWTFB_FD) {
    if (request == MXCFB_SEND_UPDATE) {

      mxcfb_update_data *update = (mxcfb_update_data *)ptr;
      MSGQ.send(*update);
      return 0;
    } else if (request == MXCFB_SET_AUTO_UPDATE_MODE) {

      return 0;
    } else if (request == MXCFB_WAIT_FOR_UPDATE_COMPLETE) {
#ifdef DEBUG
      std::cerr << "CLIENT: sync" << std::endl;
#endif

      if (!DO_WAIT_IOCTL) {
        return 0;
      }

      // for wait ioctl, we drop a WAIT_t message into the queue.  the server
      // then uses that message to signal the semaphore we just opened. this
      // can take as little as 0.5ms for small updates. one difference is that
      // the ioctl now waits for all pending updates, not just the requested
      // scheduled one.
      swtfb::ClockWatch cz;
      swtfb::wait_sem_data update;
      std::string sem_name = std::string("/rm2fb.wait.");
      sem_name += std::to_string(getpid());

      memcpy(update.sem_name, sem_name.c_str(), sem_name.size());
      update.sem_name[sem_name.size()] = 0;

      MSGQ.send(update);

      sem_t *sem = sem_open(update.sem_name, O_CREAT, 0644, 0);
      struct timespec timeout;
      if (clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
        // Probably unnecessary fallback
        timeout = {0, 0};
#ifdef DEBUG
        std::cerr << "clock_gettime failed" << std::endl;
#endif
      }

      timeout.tv_nsec += SEM_WAIT_TIMEOUT;
      // Move overflow ns to secs
      timeout.tv_sec += timeout.tv_nsec / (long) 1e9;
      timeout.tv_nsec %= (long) 1e9;

      sem_timedwait(sem, &timeout);

      // on linux, unlink will delete the semaphore once all processes using
      // it are closed. the idea here is that the client removes the semaphore
      // as soon as possible
      // TODO: validate this assumption
      sem_unlink(update.sem_name);

#ifdef DEBUG
      std::cerr << "FINISHED WAIT IOCTL " << cz.elapsed() << std::endl;
#endif
      return 0;
    }

    else if (request == FBIOGET_VSCREENINFO) {

      fb_var_screeninfo *screeninfo = (fb_var_screeninfo *)ptr;
      screeninfo->xres = swtfb::WIDTH;
      screeninfo->yres = swtfb::HEIGHT;
      screeninfo->grayscale = 0;
      screeninfo->bits_per_pixel = 8 * BYTES_PER_PIXEL;
      screeninfo->xres_virtual = swtfb::WIDTH;
      screeninfo->yres_virtual = swtfb::HEIGHT;

      //set to RGB565
      screeninfo->red.offset = 11;
      screeninfo->red.length = 5;
      screeninfo->green.offset = 5;
      screeninfo->green.length = 6;
      screeninfo->blue.offset = 0;
      screeninfo->blue.length = 5;
      return 0;
    }

    else if (request == FBIOPUT_VSCREENINFO) {

      return 0;
    } else if (request == FBIOGET_FSCREENINFO) {

      fb_fix_screeninfo *screeninfo = (fb_fix_screeninfo *)ptr;
      screeninfo->smem_len = swtfb::ipc::BUF_SIZE;
      screeninfo->smem_start = (unsigned long)SHARED_BUF;
      screeninfo->line_length = swtfb::WIDTH * BYTES_PER_PIXEL;
      constexpr char fb_id[] = "mxcfb";
      memcpy(screeninfo->id, fb_id, sizeof(fb_id));
      return 0;
    } else {
      std::cerr << "UNHANDLED IOCTL" << ' ' << request << std::endl;
      return 0;
    }
  }

  static auto func_ioctl =
      (int (*)(int, unsigned long request, ...))dlsym(RTLD_NEXT, "ioctl");

  return func_ioctl(fd, request, ptr);
}

__attribute__((visibility("default")))
bool _Z7qputenvPKcRK10QByteArray(const char *name, const QByteArray &val) {
  static const auto touchArgs = QByteArray("rotate=180:invertx");
  static const auto orig_fn = (bool (*)(const char *, const QByteArray &))dlsym(
      RTLD_NEXT, "_Z7qputenvPKcRK10QByteArray");

  if (ON_RM2 && strcmp(name, "QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS") == 0) {
    return orig_fn(name, touchArgs);
  }

  return orig_fn(name, val);
}

// called when the framebuffer is updated
typedef uint32_t (*NotifyFunc)(void*, void*);
NotifyFunc f_notify = 0;
void new_update_int4(void* arg, int x1, int y1, int x2, int y2, int waveform, int flags) {
#ifdef DEBUG
  std::cerr << "UPDATE HOOK CALLED" << std::endl;
  std::cerr << "x " << x1 << " " << x2 << std::endl;
  std::cerr << "y " << y1 << " " << y2 << std::endl;
  std::cerr << "wav " << waveform << " flags " << flags << std::endl;
#endif

  swtfb::xochitl_data data;
  data.x1 = x1;
  data.x2 = x2;
  data.y1 = y1;
  data.y2 = y2;
  data.waveform = waveform;
  data.flags = flags;
  MSGQ.send(data);

  if (f_notify != 0) {
    QRect someRect(x1,y1, x2-x1, y2-y1) ;
    f_notify(arg, &someRect);
  }
}

void new_update_QRect(void* arg, QRect& rect, int waveform, bool flags) {
  new_update_int4(
    arg,
    rect.x(), rect.y(),
    rect.x() + rect.width(),
    rect.y() + rect.height(),
    waveform, flags
  );
}

int new_create_threads(char*, void*) {
  std::cerr << "create threads called" << std::endl;
  return 0;
}

int new_wait() {
  std::cerr << "wait clear func called" << std::endl;
  return 0;
}

int new_shutdown() {
  std::cerr << "shutdown called" << std::endl;
  return 0;
}

std::string readlink_string(const char* link_path) {
  char buffer[PATH_MAX];
  ssize_t len = readlink(link_path, buffer, sizeof(buffer) - 1);

  if (len == -1) {
    return "";
  }

  buffer[len] = '\0';
  return buffer;
}

#ifndef NO_XOCHITL

// exits if it fails since we won't get far with xochitl
// without these funcs stubbed out
void replace_func(
  GumInterceptor* interceptor,
  const Config& config,
  std::string func_name,
  void* new_func
) {
  auto search = config.find(func_name);

  if (search == config.end()) {
    std::cerr << "Missing address for function '" << func_name << "'\n"
      "PLEASE SEE https://github.com/ddvk/remarkable2-framebuffer/issues/18\n";
    std::exit(-1);
  }

  void* old_func = std::get<void*>(search->second);
  std::cerr << "Replacing '" << func_name << "' (at " << old_func << "): ";

  if (gum_interceptor_replace(
        interceptor, old_func, new_func, nullptr) != GUM_REPLACE_OK) {
    std::cerr << "ERR\n";
    std::exit(-1);
  } else {
    std::cerr << "OK\n";
  }
}

void intercept_xochitl(const Config& config) {
  gum_init_embedded();
  GumInterceptor *interceptor = gum_interceptor_obtain();
  const auto update_type = config.find("updateType");
  replace_func(
    interceptor, config, "update",
    (update_type == config.end()
      || std::get<std::string>(update_type->second) == "int4")
      ? (void*) new_update_int4
      : (void*) new_update_QRect
  );
  replace_func(interceptor, config, "create", (void*) new_create_threads);
  replace_func(interceptor, config, "shutdown", (void*) new_shutdown);
  replace_func(interceptor, config, "wait", (void*) new_wait);
  
  auto search = config.find("notify");
  if (search == config.end()) {
    std::cerr << "missing notify function, screenshare won't work" << std::endl;
  } else {
    f_notify = (NotifyFunc) std::get<void*>(search->second);
  }
}

__attribute__((visibility("default")))
int __libc_start_main(int (*_main)(int, char **, char **), int argc,
                      char **argv, int (*init)(int, char **, char **),
                      void (*fini)(void), void (*rtld_fini)(void),
                      void *stack_end) {
  const auto config = read_config();

#ifdef DEBUG
  std::cerr << "Final config:\n";
  for (const auto& [key, value] : config) {
    std::cerr << key;
    if (std::holds_alternative<std::string>(value)) {
      std::cerr << " str " << std::get<std::string>(value) << '\n';
    } else {
      std::cerr << " addr " << std::get<void*>(value) << '\n';
    }
  }
#endif

  if (ON_RM2) {
    auto binary_path = readlink_string("/proc/self/exe");

    if (binary_path.empty()) {
      std::cerr << "Unable to find current binary path\n";
      return -1;
    }

    if (binary_path == "/usr/bin/xochitl") {
      IN_XOCHITL = true;
      intercept_xochitl(config);
    }
  }

  auto func_main =
      (decltype(&__libc_start_main))dlsym(RTLD_NEXT, "__libc_start_main");

  return func_main(_main, argc, argv, init, fini, rtld_fini, stack_end);
}

#endif // NO_XOCHITL

} // extern "C"
