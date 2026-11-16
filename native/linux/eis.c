#define _POSIX_C_SOURCE 200809L
#include "eis.h"
#include <libei.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

static struct ei *context;
static struct ei_device *pointer;
static int resumed;
static unsigned int emulation_sequence;
static void *library;
#define EI_FUNCTIONS(X) \
  X(ei_new_sender) X(ei_unref) X(ei_setup_backend_fd) X(ei_dispatch) \
  X(ei_get_event) X(ei_event_get_type) X(ei_event_get_seat) \
  X(ei_event_get_device) X(ei_event_unref) X(ei_seat_bind_capabilities) \
  X(ei_device_has_capability) X(ei_device_ref) X(ei_device_unref) \
  X(ei_device_start_emulating) X(ei_device_pointer_motion_absolute) \
  X(ei_device_button_button) X(ei_device_frame) X(ei_now)
#define DECLARE_FUNCTION(name) static __typeof__(&name) native_##name;
EI_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION

int eis_available(void) {
  if (library)
    return 1;
  void *candidate = dlopen("libei.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!candidate)
    return 0;
#define LOAD_FUNCTION(name) \
  *(void **)(&native_##name) = dlsym(candidate, #name); \
  if (!native_##name) { dlclose(candidate); return 0; }
  EI_FUNCTIONS(LOAD_FUNCTION)
#undef LOAD_FUNCTION
  library = candidate;
  return 1;
}

void eis_poll(void) {
  if (!context)
    return;
  native_ei_dispatch(context);
  struct ei_event *event;
  for (int i = 0; i < 100 && (event = native_ei_get_event(context)); i++) {
    enum ei_event_type type = native_ei_event_get_type(event);
    struct ei_device *device = native_ei_event_get_device(event);
    if (type == EI_EVENT_SEAT_ADDED)
      native_ei_seat_bind_capabilities(native_ei_event_get_seat(event),
          EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON, NULL);
    if (type == EI_EVENT_DEVICE_RESUMED && device &&
        native_ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) &&
        native_ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON)) {
      if (pointer)
        native_ei_device_unref(pointer);
      pointer = native_ei_device_ref(device);
      native_ei_device_start_emulating(pointer, ++emulation_sequence);
      resumed = 1;
    }
    if (type == EI_EVENT_DISCONNECT ||
        ((type == EI_EVENT_DEVICE_PAUSED || type == EI_EVENT_DEVICE_REMOVED) &&
         device == pointer))
      resumed = 0;
    native_ei_event_unref(event);
  }
}

int eis_connect(int fd) {
  if (!eis_available()) {
    close(fd);
    return 0;
  }
  context = native_ei_new_sender(NULL);
  if (!context || native_ei_setup_backend_fd(context, fd) < 0) {
    if (!context)
      close(fd);
    eis_close();
    return 0;
  }
  for (int i = 0; i < 100 && !resumed; i++) {
    eis_poll();
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
  }
  return resumed;
}

int eis_motion(double x, double y) {
  eis_poll();
  if (!resumed)
    return 0;
  native_ei_device_pointer_motion_absolute(pointer, x, y);
  native_ei_device_frame(pointer, native_ei_now(context));
  native_ei_dispatch(context);
  return 1;
}

int eis_button(int button, int pressed) {
  eis_poll();
  if (!resumed)
    return 0;
  native_ei_device_button_button(pointer, (unsigned int)button, pressed != 0);
  native_ei_device_frame(pointer, native_ei_now(context));
  native_ei_dispatch(context);
  return 1;
}

void eis_close(void) {
  if (pointer)
    native_ei_device_unref(pointer);
  if (context)
    native_ei_unref(context);
  pointer = NULL;
  context = NULL;
  resumed = 0;
}
