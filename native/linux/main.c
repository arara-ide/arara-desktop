#define _POSIX_C_SOURCE 200809L
#include "json.h"
#include "portal.h"
#include "platform.h"
#include <gio/gdesktopappinfo.h>
#include <fcntl.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
typedef struct {
  Window handle;
  unsigned long pid;
  char id[40];
  void *accessible;
  int screen;
} WindowRef;
typedef struct {
  void *element;
  char uid[40];
  char name[1024];
  int window;
} NodeRef;
typedef PortalRectangle Rectangle;
static Display *display;
static WindowRef windows[100];
static int window_count;
static NodeRef nodes[100];
static int node_count;
static unsigned long sequence;
static volatile sig_atomic_t cancelled;
static volatile sig_atomic_t generation;
static sig_atomic_t queued_generation;
static sig_atomic_t active_generation;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static char line_queue[64001];
static int queued;
static int closed_input;
static pthread_cond_t available = PTHREAD_COND_INITIALIZER;
static char frame_id[40];
static int frame_window = -1;
static int visual_frame;
static void *frame_foreground;
static unsigned int frame_foreground_pid;
static Rectangle frame_bounds;
static time_t frame_time;
static KeyCode held_keys[6];
static int held_key_count;
static int held_button;
static uint32_t portal_keys[6];
static int portal_key_count;
static int portal_held_button;
static void *atspi_lib;
static void *gobject_lib;
static void *glib_lib;
static int (*atspi_init_fn)(void);
static void (*atspi_timeout_fn)(int, int);
static void *(*desktop_fn)(int);
static int (*count_fn)(void *, void *);
static void *(*child_fn)(void *, int, void *);
static char *(*name_fn)(void *, void *);
static char *(*role_fn)(void *, void *);
static unsigned int (*pid_fn)(void *, void *);
static void *(*component_fn)(void *);
static Rectangle *(*extents_fn)(void *, int, void *);
static int (*focus_fn)(void *, void *);
static void *(*action_fn)(void *);
static int (*actions_fn)(void *, void *);
static int (*invoke_fn)(void *, int, void *);
static void *(*editable_fn)(void *);
static int (*set_text_fn)(void *, const char *, void *);
static void *(*text_fn)(void *);
static void *(*document_fn)(void *);
static char *(*document_attribute_fn)(void *, const char *, void *);
static char *(*get_text_fn)(void *, int, int, void *);
static void *(*ref_fn)(void *);
static void *(*state_fn)(void *);
static int (*contains_fn)(void *, int);
static void (*clear_cache_fn)(void *);
static void (*unref_fn)(void *);
static void (*free_fn)(void *);
static void load_atspi(void) {
  atspi_lib = dlopen("libatspi.so.0", RTLD_NOW);
  gobject_lib = dlopen("libgobject-2.0.so.0", RTLD_NOW);
  glib_lib = dlopen("libglib-2.0.so.0", RTLD_NOW);
  if (!atspi_lib || !gobject_lib || !glib_lib)
    return;
#define LOAD(variable, symbol)                                                 \
  do {                                                                         \
    *(void **)(&(variable)) = dlsym(atspi_lib, symbol);                        \
    if (!(variable))                                                           \
      return;                                                                  \
  } while (0)
  LOAD(atspi_init_fn, "atspi_init");
  *(void **)(&atspi_timeout_fn) = dlsym(atspi_lib, "atspi_set_timeout");
  LOAD(desktop_fn, "atspi_get_desktop");
  LOAD(count_fn, "atspi_accessible_get_child_count");
  LOAD(child_fn, "atspi_accessible_get_child_at_index");
  LOAD(name_fn, "atspi_accessible_get_name");
  LOAD(role_fn, "atspi_accessible_get_role_name");
  LOAD(pid_fn, "atspi_accessible_get_process_id");
  LOAD(component_fn, "atspi_accessible_get_component_iface");
  LOAD(extents_fn, "atspi_component_get_extents");
  LOAD(focus_fn, "atspi_component_grab_focus");
  LOAD(action_fn, "atspi_accessible_get_action_iface");
  LOAD(actions_fn, "atspi_action_get_n_actions");
  LOAD(invoke_fn, "atspi_action_do_action");
  LOAD(editable_fn, "atspi_accessible_get_editable_text_iface");
  LOAD(set_text_fn, "atspi_editable_text_set_text_contents");
  LOAD(text_fn, "atspi_accessible_get_text_iface");
  LOAD(get_text_fn, "atspi_text_get_text");
  *(void **)(&document_fn) =
      dlsym(atspi_lib, "atspi_accessible_get_document_iface");
  *(void **)(&document_attribute_fn) =
      dlsym(atspi_lib, "atspi_document_get_attribute_value");
  LOAD(state_fn, "atspi_accessible_get_state_set");
  LOAD(contains_fn, "atspi_state_set_contains");
  LOAD(clear_cache_fn, "atspi_accessible_clear_cache");
  *(void **)(&unref_fn) = dlsym(gobject_lib, "g_object_unref");
  *(void **)(&free_fn) = dlsym(glib_lib, "g_free");
  *(void **)(&ref_fn) = dlsym(gobject_lib, "g_object_ref");
#undef LOAD
  if (!ref_fn || !unref_fn || !free_fn || atspi_init_fn() != 0)
    desktop_fn = NULL;
  else {
    if (atspi_timeout_fn)
      atspi_timeout_fn(250, 500);
    void *desktop = desktop_fn(0);
    if (desktop)
      unref_fn(desktop);
  }
}
static double monotonic_time(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return value.tv_sec + value.tv_nsec / 1000000000.0;
}
static void clear_nodes(void) {
  if (unref_fn)
    for (int i = 0; i < node_count; i++)
      unref_fn(nodes[i].element);
  node_count = 0;
}
static unsigned long property(Window handle, const char *name) {
  Atom type;
  int format;
  unsigned long count, left;
  unsigned char *data = NULL;
  XGetWindowProperty(display, handle, XInternAtom(display, name, False), 0, 1,
                     False, AnyPropertyType, &type, &format, &count, &left,
                     &data);
  unsigned long value =
      data && format == 32 && count ? *(unsigned long *)data : 0;
  if (data)
    XFree(data);
  return value;
}
static int bounds(Window handle, Rectangle *out) {
  XWindowAttributes attributes;
  if (!XGetWindowAttributes(display, handle, &attributes) ||
      attributes.map_state != IsViewable)
    return 0;
  Window child;
  int x, y;
  XTranslateCoordinates(display, handle, DefaultRootWindow(display), 0, 0, &x,
                        &y, &child);
  *out = (Rectangle){x, y, attributes.width, attributes.height};
  return attributes.width > 0 && attributes.height > 0;
}
static int same_bounds(Rectangle a, Rectangle b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
static int active(Window handle);
static int interrupted(void) {
  return cancelled || generation != active_generation;
}
static int window_bounds(int index, Rectangle *out) {
  if (windows[index].screen) {
    *out = portal_source_bounds(windows[index].screen - 1);
    return out->width > 0 && out->height > 0;
  }
  if (display)
    return bounds(windows[index].handle, out);
  void *element = windows[index].accessible;
  if (!element)
    return 0;
  clear_cache_fn(element);
  void *component = component_fn(element);
  Rectangle *box = component ? extents_fn(component, 0, NULL) : NULL;
  int valid = box && box->width > 0 && box->height > 0 &&
      box->width <= 16000 && box->height <= 16000 &&
      box->x > -100000 && box->x < 100000 &&
      box->y > -100000 && box->y < 100000;
  if (valid)
    *out = *box;
  if (box)
    free_fn(box);
  if (component)
    unref_fn(component);
  return valid;
}
static int window_active(int index) {
  if (windows[index].screen)
    return portal_input_ready();
  if (display)
    return active(windows[index].handle);
  clear_cache_fn(windows[index].accessible);
  void *state = state_fn(windows[index].accessible);
  int focused = state && contains_fn(state, 1);
  if (state)
    unref_fn(state);
  return focused;
}
static int active(Window handle) {
  return property(DefaultRootWindow(display), "_NET_ACTIVE_WINDOW") == handle;
}
static int select_window(Json request) {
  char id[200];
  text(member(request, "windowId"), id, sizeof(id));
  for (int i = 0; i < window_count; i++)
    if (!strcmp(id, windows[i].id) &&
        (windows[i].screen ? windows[i].screen <= portal_source_count() :
         windows[i].accessible
             ? pid_fn(windows[i].accessible, NULL) == windows[i].pid
             : display && property(windows[i].handle, "_NET_WM_PID") ==
                              windows[i].pid))
      return i;
  return -1;
}
static int safe_window(Window handle) {
  XClassHint hint = {0};
  if (!XGetClassHint(display, handle, &hint))
    return 0;
  int safe = hint.res_class && !strstr(hint.res_class, "polkit") &&
             !strstr(hint.res_class, "Polkit") &&
             !strstr(hint.res_class, "keyring") &&
             !strstr(hint.res_class, "Settings");
  if (hint.res_name)
    XFree(hint.res_name);
  if (hint.res_class)
    XFree(hint.res_class);
  return safe;
}
static void clear_windows(void) {
  if (frame_foreground && unref_fn)
    unref_fn(frame_foreground);
  frame_foreground = NULL;
  if (unref_fn)
    for (int i = 0; i < window_count; i++)
      if (windows[i].accessible) {
        unref_fn(windows[i].accessible);
        windows[i].accessible = NULL;
      }
  window_count = 0;
}
static const char *accessible_windows(Json request) {
  clear_nodes();
  clear_windows();
  frame_window = -1;
  void *desktop = desktop_fn ? desktop_fn(0) : NULL;
  if (!desktop && !portal_input_ready())
    return "session_unavailable";
  char selected[200];
  text(member(request, "application"), selected, sizeof(selected));
  int count = desktop ? count_fn(desktop, NULL) : 0, comma = 0;
  printf("{\"windows\":[");
  for (int i = 0; i < count && i < 100 && window_count < 100; i++) {
    void *app = child_fn(desktop, i, NULL);
    if (!app)
      continue;
    char *name = name_fn(app, NULL);
    int allowed = name && (!selected[0] || !strcmp(name, selected)) &&
                  !strstr(name, "Settings") && !strstr(name, "polkit") &&
                  !strstr(name, "Password");
    int children = allowed ? count_fn(app, NULL) : 0;
    for (int n = 0; n < children && n < 40 && window_count < 100; n++) {
      void *element = child_fn(app, n, NULL);
      if (!element)
        continue;
      WindowRef *target = &windows[window_count++];
      target->handle = 0;
      target->screen = 0;
      target->accessible = element;
      target->pid = pid_fn(element, NULL);
      snprintf(target->id, sizeof(target->id), "w%lu", ++sequence);
      char *title = name_fn(element, NULL);
      if (comma++)
        putchar(',');
      printf("{\"windowId\":");
      quoted(target->id, 40);
      printf(",\"app\":");
      quoted(name, 160);
      printf(",\"title\":");
      quoted(title, 160);
      printf(",\"active\":%s}", window_active(window_count - 1) ? "true" : "false");
      if (title)
        free_fn(title);
    }
    if (name)
      free_fn(name);
    unref_fn(app);
  }
  if (desktop)
    unref_fn(desktop);
  if (!selected[0] || !strcmp(selected, "Desktop"))
    for (int i = 0; i < portal_source_count() && window_count < 100; i++) {
      WindowRef *target = &windows[window_count++];
      memset(target, 0, sizeof(*target));
      target->screen = i + 1;
      snprintf(target->id, sizeof(target->id), "w%lu", ++sequence);
      Rectangle rect = portal_source_bounds(i);
      if (comma++)
        putchar(',');
      printf("{\"windowId\":");
      quoted(target->id, 40);
      printf(",\"app\":\"Desktop\",\"title\":\"Shared screen %d\","
          "\"surface\":\"screen\",\"active\":true,\"x\":%d,\"y\":%d,"
          "\"width\":%d,\"height\":%d}", i + 1, rect.x, rect.y,
          rect.width, rect.height);
    }
  printf("],\"truncated\":%s}", window_count >= 100 ? "true" : "false");
  return NULL;
}
static const char *list_windows(Json request) {
  clear_windows();
  clear_nodes();
  frame_window = -1;
  Atom type;
  int format;
  unsigned long count, left;
  unsigned char *data = NULL;
  XGetWindowProperty(display, DefaultRootWindow(display),
                     XInternAtom(display, "_NET_CLIENT_LIST", False), 0, 1000,
                     False, XA_WINDOW, &type, &format, &count, &left, &data);
  char selected[200];
  text(member(request, "application"), selected, sizeof(selected));
  printf("{\"windows\":[");
  int comma = 0;
  if (data && format == 32)
    for (unsigned long n = 0; n < count && window_count < 100; n++) {
      Window handle = ((Window *)data)[n];
      Rectangle rect;
      unsigned long pid = property(handle, "_NET_WM_PID");
      if (!pid || !safe_window(handle) || !bounds(handle, &rect))
        continue;
      XClassHint hint = {0};
      XGetClassHint(display, handle, &hint);
      int match =
          !selected[0] || (hint.res_class && !strcmp(hint.res_class, selected));
      char *name = NULL;
      XFetchName(display, handle, &name);
      if (match) {
        WindowRef *ref = &windows[window_count++];
        ref->handle = handle;
        ref->screen = 0;
        ref->pid = pid;
        snprintf(ref->id, sizeof(ref->id), "w%lu", ++sequence);
        if (comma++)
          putchar(',');
        printf("{\"windowId\":");
        quoted(ref->id, 40);
        printf(",\"app\":");
        quoted(hint.res_class, 160);
        printf(",\"title\":");
        quoted(name, 160);
        printf(",\"active\":%s}", active(handle) ? "true" : "false");
      }
      if (hint.res_class)
        XFree(hint.res_class);
      if (hint.res_name)
        XFree(hint.res_name);
      if (name)
        XFree(name);
    }
  if (data)
    XFree(data);
  printf("],\"truncated\":%s}", window_count >= 100 ? "true" : "false");
  return NULL;
}
static void *root_for(int index) {
  if (windows[index].accessible)
    return ref_fn(windows[index].accessible);
  if (!desktop_fn)
    return NULL;
  void *desktop = desktop_fn(0);
  if (!desktop)
    return NULL;
  int count = count_fn(desktop, NULL);
  void *result = NULL;
  Rectangle rect;
  if (!bounds(windows[index].handle, &rect)) {
    unref_fn(desktop);
    return NULL;
  }
  for (int i = 0; i < count && i < 100 && !result; i++) {
    void *app = child_fn(desktop, i, NULL);
    if (!app)
      continue;
    if (pid_fn(app, NULL) == windows[index].pid) {
      int children = count_fn(app, NULL);
      for (int j = 0; j < children && j < 40 && !result; j++) {
        void *window = child_fn(app, j, NULL);
        if (!window)
          continue;
        void *component = component_fn(window);
        Rectangle *box = component ? extents_fn(component, 0, NULL) : NULL;
        if (box && abs(box->x - rect.x) < 4 && abs(box->y - rect.y) < 40)
          result = window;
        else
          unref_fn(window);
        if (box)
          free_fn(box);
        if (component)
          unref_fn(component);
      }
    }
    unref_fn(app);
  }
  unref_fn(desktop);
  return result;
}
static void document_metadata(int index) {
  if (!document_fn || !document_attribute_fn)
    return;
  void *root = root_for(index);
  if (!root)
    return;
  void *queue[601];
  int depths[601];
  queue[0] = root;
  depths[0] = 0;
  int head = 0, tail = 1;
  double started = monotonic_time();
  while (head < tail && head < 200 && monotonic_time() - started < 1) {
    int position = head++;
    void *element = queue[position];
    void *document = document_fn(element);
    if (document) {
      char *url = document_attribute_fn(document, "DocURL", NULL);
      char *title = name_fn(element, NULL);
      printf(",\"documentTitle\":");
      quoted(title, 160);
      printf(",\"documentUrl\":");
      quoted(url, 2000);
      if (url)
        free_fn(url);
      if (title)
        free_fn(title);
      unref_fn(document);
      unref_fn(element);
      break;
    }
    int count = count_fn(element, NULL);
    if (depths[position] < 12)
      for (int n = 0; n < count && n < 100 && tail < 600; n++) {
        void *child = child_fn(element, n, NULL);
        if (child) {
          queue[tail] = child;
          depths[tail++] = depths[position] + 1;
        }
      }
    unref_fn(element);
  }
  while (head < tail)
    unref_fn(queue[head++]);
}
static const char *snapshot(int index) {
  if (windows[index].screen) {
    Rectangle box;
    if (!window_bounds(index, &box))
      return "stale";
    frame_window = index;
    frame_bounds = box;
    frame_time = time(NULL);
    visual_frame = 0;
    snprintf(frame_id, sizeof(frame_id), "f%lu", ++sequence);
    printf("{\"nodes\":[],\"truncated\":false,\"accessibility\":false,"
           "\"inputFrameId\":");
    quoted(frame_id, 40);
    printf("}");
    return NULL;
  }
  void *root = root_for(index);
  if (!root) {
    clear_nodes();
    printf("{\"nodes\":[],\"truncated\":false,\"accessibility\":false}");
    return NULL;
  }
  NodeRef previous[100];
  int previous_count = node_count;
  memcpy(previous, nodes, sizeof(previous));
  node_count = 0;
  void *queue[601];
  int depth[601];
  int head = 0, tail = 1;
  queue[0] = root;
  depth[0] = 0;
  printf("{\"nodes\":[");
  int comma = 0;
  double started = monotonic_time();
  while (head < tail && node_count < 100 && !cancelled &&
         monotonic_time() - started < 3) {
    int position = head++;
    void *element = queue[position];
    char *role = role_fn(element, NULL);
    char *name = name_fn(element, NULL);
    if (role && strstr(role, "password")) {
      if (name)
        free_fn(name);
      free_fn(role);
      unref_fn(element);
      continue;
    }
    NodeRef *node = &nodes[node_count++];
    node->element = element;
    node->window = index;
    int existing = -1;
    for (int n = 0; n < previous_count; n++)
      if (previous[n].window == index && previous[n].element == element) {
        existing = n;
        break;
      }
    if (existing >= 0)
      snprintf(node->uid, sizeof(node->uid), "%s", previous[existing].uid);
    else
      snprintf(node->uid, sizeof(node->uid), "n%lu", ++sequence);
    snprintf(node->name, sizeof(node->name), "%s", name ? name : "");
    void *action = action_fn(element);
    void *editable = editable_fn(element);
    void *component = component_fn(element);
    if (comma++)
      putchar(',');
    printf("{\"uid\":");
    quoted(node->uid, 40);
    printf(",\"role\":");
    quoted(role, 100);
    printf(",\"name\":");
    quoted(name, 160);
    printf(",\"enabled\":true,\"operations\":[");
    int operation = 0;
    if (action && actions_fn(action, NULL) > 0) {
      printf("\"press\"");
      operation = 1;
    }
    if (editable) {
      printf("%s\"set_value\"", operation ? "," : "");
      operation = 1;
    }
    if (component)
      printf("%s\"focus\"", operation ? "," : "");
    printf("]}");
    int count = count_fn(element, NULL);
    if (depth[position] < 12)
      for (int n = 0; n < count && n < 100 && tail < 600; n++) {
        void *child = child_fn(element, n, NULL);
        if (child) {
          queue[tail] = child;
          depth[tail++] = depth[position] + 1;
        }
      }
    if (action)
      unref_fn(action);
    if (editable)
      unref_fn(editable);
    if (component)
      unref_fn(component);
    if (role)
      free_fn(role);
    if (name)
      free_fn(name);
  }
  int truncated = head < tail;
  while (head < tail)
    unref_fn(queue[head++]);
  for (int n = 0; n < previous_count; n++)
    unref_fn(previous[n].element);
  printf("],\"truncated\":%s", truncated ? "true" : "false");
  Rectangle box;
  if (!display && portal_input_ready() && window_active(index) &&
      window_bounds(index, &box)) {
    frame_window = index;
    frame_bounds = box;
    frame_time = time(NULL);
    visual_frame = 0;
    snprintf(frame_id, sizeof(frame_id), "f%lu", ++sequence);
    printf(",\"inputFrameId\":");
    quoted(frame_id, 40);
  }
  printf("}");
  return NULL;
}
static void release_input(void) {
  while (portal_key_count > 0)
    portal_key(portal_keys[--portal_key_count], 0);
  if (portal_held_button)
    portal_button(portal_held_button, 0);
  portal_held_button = 0;
  if (!display)
    return;
  for (int i = 0; i < held_key_count; i++)
    XTestFakeKeyEvent(display, held_keys[i], False, CurrentTime);
  if (held_button)
    XTestFakeButtonEvent(display, (unsigned int)held_button, False,
                         CurrentTime);
  held_key_count = 0;
  held_button = 0;
  XFlush(display);
}
static KeySym key_symbol(const char *key) {
  if (!strcmp(key, "ENTER"))
    return XK_Return;
  if (!strcmp(key, "SPACE"))
    return XK_space;
  if (!strcmp(key, "SHIFT"))
    return XK_Shift_L;
  if (!strcmp(key, "CTRL"))
    return XK_Control_L;
  if (!strcmp(key, "ALT"))
    return XK_Alt_L;
  if (!strcmp(key, "META"))
    return XK_Super_L;
  if (!strcmp(key, "ESCAPE"))
    return XK_Escape;
  if (!strcmp(key, "TAB"))
    return XK_Tab;
  if (!strcmp(key, "BACKSPACE"))
    return XK_BackSpace;
  if (!strcmp(key, "UP"))
    return XK_Up;
  if (!strcmp(key, "DOWN"))
    return XK_Down;
  if (!strcmp(key, "LEFT"))
    return XK_Left;
  if (!strcmp(key, "RIGHT"))
    return XK_Right;
  return strlen(key) == 1 && isalnum((unsigned char)key[0])
             ? XStringToKeysym(key)
             : NoSymbol;
}
static void binary32(unsigned char *out, uint32_t value) {
  out[0] = (unsigned char)(value >> 24);
  out[1] = (unsigned char)(value >> 16);
  out[2] = (unsigned char)(value >> 8);
  out[3] = (unsigned char)value;
}
static void chunk(unsigned char *out, const char *type,
                  const unsigned char *data, uint32_t size) {
  binary32(out, size);
  memcpy(out + 4, type, 4);
  if (size)
    memcpy(out + 8, data, size);
  binary32(out + 8 + size, (uint32_t)crc32(0, out + 4, size + 4));
}
static void base64(const unsigned char *data, size_t size) {
  const char *alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (size_t i = 0; i < size; i += 3) {
    unsigned int value = (unsigned int)data[i] << 16;
    if (i + 1 < size)
      value |= (unsigned int)data[i + 1] << 8;
    if (i + 2 < size)
      value |= data[i + 2];
    putchar(alphabet[value >> 18]);
    putchar(alphabet[(value >> 12) & 63]);
    putchar(i + 1 < size ? alphabet[(value >> 6) & 63] : '=');
    putchar(i + 2 < size ? alphabet[value & 63] : '=');
  }
}
static unsigned char channel(unsigned long value, unsigned long mask) {
  if (!mask)
    return 0;
  while (!(mask & 1)) {
    mask >>= 1;
    value >>= 1;
  }
  return (unsigned char)((value & mask) * 255 / mask);
}
static const char *screenshot(int index) {
  if (!display && !windows[index].screen)
    return "unsupported";
  Rectangle box;
  if (!window_active(index) || !window_bounds(index, &box))
    return "focus";
  if (box.width > 16000 || box.height > 16000)
    return "unsupported";
  int width = box.width > 1280 ? 1280 : box.width;
  int height = box.height * width / box.width;
  if (height < 1)
    height = 1;
  size_t raw_size = (size_t)(width * 3 + 1) * (size_t)height;
  unsigned char *raw = calloc(1, raw_size);
  if (!raw)
    return "failed";
  if (display) {
    XImage *image = XGetImage(display, windows[index].handle, 0, 0,
        (unsigned int)box.width, (unsigned int)box.height, AllPlanes, ZPixmap);
    if (!image) {
      free(raw);
      return "failed";
    }
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++) {
        unsigned long pixel =
            XGetPixel(image, x * box.width / width, y * box.height / height);
        size_t at = (size_t)y * (size_t)(width * 3 + 1) + (size_t)x * 3 + 1;
        raw[at] = channel(pixel, image->red_mask);
        raw[at + 1] = channel(pixel, image->green_mask);
        raw[at + 2] = channel(pixel, image->blue_mask);
      }
    XDestroyImage(image);
  } else {
    unsigned char *rgb = NULL;
    const char *error = portal_capture(box, &rgb, &width, &height, interrupted);
    if (error) {
      free(raw);
      return error;
    }
    for (int y = 0; y < height; y++)
      memcpy(raw + (size_t)y * (size_t)(width * 3 + 1) + 1,
             rgb + (size_t)y * (size_t)width * 3, (size_t)width * 3);
    free(rgb);
    Rectangle current;
    if (!window_active(index) || !window_bounds(index, &current) ||
        !same_bounds(box, current)) {
      free(raw);
      return "focus";
    }
  }
  uLongf compressed_size = compressBound(raw_size);
  unsigned char *compressed = malloc(compressed_size);
  if (!compressed) {
    free(raw);
    return "failed";
  }
  if (compress2(compressed, &compressed_size, raw, raw_size, 6) != Z_OK ||
      compressed_size > 4000000) {
    free(raw);
    free(compressed);
    return "failed";
  }
  free(raw);
  size_t png_size = (size_t)compressed_size + 57;
  unsigned char *png = malloc(png_size);
  if (!png) {
    free(compressed);
    return "failed";
  }
  memcpy(png, "\x89PNG\r\n\x1a\n", 8);
  unsigned char header[13] = {0};
  binary32(header, (uint32_t)width);
  binary32(header + 4, (uint32_t)height);
  header[8] = 8;
  header[9] = 2;
  chunk(png + 8, "IHDR", header, 13);
  chunk(png + 33, "IDAT", compressed, (uint32_t)compressed_size);
  chunk(png + 45 + compressed_size, "IEND", NULL, 0);
  free(compressed);
  frame_window = index;
  visual_frame = 1;
  frame_bounds = box;
  frame_time = time(NULL);
  snprintf(frame_id, sizeof(frame_id), "f%lu", ++sequence);
  printf("{\"frameId\":");
  quoted(frame_id, 40);
  printf(
      ",\"width\":%d,\"height\":%d,\"coordinateWidth\":%d,\"coordinateHeight\":"
      "%d,\"images\":[{\"mimeType\":\"image/png\",\"data\":\"",
      width, height, box.width, box.height);
  base64(png, png_size);
  printf("\"}]}");
  free(png);
  return NULL;
}
static const char *input_guard(int index, Rectangle box) {
  portal_poll();
  if (interrupted())
    return "cancelled";
  if (!portal_input_ready())
    return "portal_required";
  if (windows[index].screen && frame_foreground) {
    if (pid_fn(frame_foreground, NULL) != frame_foreground_pid)
      return "focus";
    clear_cache_fn(frame_foreground);
    void *state = state_fn(frame_foreground);
    int focused = state && contains_fn(state, 1);
    if (state)
      unref_fn(state);
    if (!focused)
      return "focus";
  }
  Rectangle current;
  if (!window_active(index) || !window_bounds(index, &current) ||
      !same_bounds(current, box))
    return "focus";
  return NULL;
}
static const char *portal_input(Json request, int index, Rectangle box,
                                 const char *operation, int duration) {
  if (!portal_input_ready())
    return "portal_required";
  if (windows[index].screen && !portal_contains(box))
    return "unsupported";
  int camera_x = number(request, "dx", 0), camera_y = number(request, "dy", 0);
  if (labs((long)camera_x) > 2000 || labs((long)camera_y) > 2000)
    return "unsupported";
  if ((!visual_frame || !windows[index].screen) &&
      (strcmp(operation, "keys") && strcmp(operation, "text")))
    return "visual_frame_required";
  if (!visual_frame && (camera_x || camera_y))
    return "visual_frame_required";
  const char *error = input_guard(index, box);
  if (error)
    return error;
  if (!strcmp(operation, "move") || !strcmp(operation, "click")) {
    int x = number(request, "x", -1), y = number(request, "y", -1);
    if (x < 0 || y < 0 || x >= box.width || y >= box.height)
      return "unsupported";
    char button[20];
    text(member(request, "button"), button, sizeof(button));
    int code = !button[0] || !strcmp(button, "left") ? 272 :
        !strcmp(button, "right") ? 273 : !strcmp(button, "middle") ? 274 : 0;
    if (!strcmp(operation, "click") && !code)
      return "unsupported";
    error = portal_motion(box.x + x, box.y + y, 0);
    if (!error && !strcmp(operation, "click")) {
      error = input_guard(index, box);
      if (!error) {
        portal_held_button = code;
        error = portal_button(code, 1);
      }
    }
  } else if (!strcmp(operation, "relative_move")) {
    error = portal_motion(camera_x, camera_y, 1);
  } else if (!strcmp(operation, "scroll")) {
    if (!camera_x && !camera_y)
      return "unsupported";
    error = portal_motion(box.x + box.width / 2, box.y + box.height / 2, 0);
    for (int i = 0; i < 12 && !error; i++) {
      error = input_guard(index, box);
      if (!error)
        error = portal_scroll(camera_x / 12.0, camera_y / 12.0, i == 11);
      g_usleep(10000);
    }
  } else if (!strcmp(operation, "keys")) {
    Json keys = member(request, "keys");
    const char *p = skip_space(keys.start);
    if (*p++ != '[')
      return "unsupported";
    uint32_t symbols[6];
    int count = 0;
    while (*(p = skip_space(p)) != ']' && count < 6) {
      const char *end = skip_value(p, 0);
      if (!end)
        return "unsupported";
      char key[30];
      text((Json){p, end}, key, sizeof(key));
      for (int i = 0; key[i]; i++)
        key[i] = (char)toupper((unsigned char)key[i]);
      KeySym symbol = key_symbol(key);
      if (!symbol)
        return "unsupported";
      for (int i = 0; i < count; i++)
        if (symbols[i] == symbol)
          return "unsupported";
      symbols[count++] = (uint32_t)symbol;
      p = skip_space(end);
      if (*p == ',')
        p++;
      else if (*p != ']')
        return "unsupported";
    }
    if (*p != ']' || !count)
      return "unsupported";
    for (int i = 0; i < count && !error; i++) {
      portal_keys[portal_key_count++] = symbols[i];
      error = portal_key(symbols[i], 1);
    }
  } else if (!strcmp(operation, "text")) {
    char value[32001];
    text(member(request, "value"), value, sizeof(value));
    if (!g_utf8_validate(value, -1, NULL))
      return "unsupported";
    double deadline = monotonic_time() + 5;
    for (char *p = value; *p && !error; p = g_utf8_next_char(p)) {
      error = input_guard(index, box);
      if (!error && monotonic_time() > deadline)
        error = "cancelled";
      if (!error) {
        gunichar character = g_utf8_get_char(p);
        uint32_t symbol = character == '\n' ? XK_Return :
            character == '\t' ? XK_Tab : character <= 255 ? character :
            0x01000000 | character;
        portal_keys[portal_key_count++] = symbol;
        error = portal_key(symbol, 1);
        const char *released = portal_key(symbol, 0);
        portal_key_count = 0;
        if (!error)
          error = released;
      }
    }
  } else
    return "unsupported";
  int moved_x = 0, moved_y = 0;
  double deadline = monotonic_time() + duration / 1000.0;
  while (!error && monotonic_time() < deadline) {
    error = input_guard(index, box);
    if (!error && !strcmp(operation, "keys") && (camera_x || camera_y)) {
      double fraction = 1 - (deadline - monotonic_time()) * 1000 / duration;
      int x = (int)(camera_x * fraction), y = (int)(camera_y * fraction);
      error = portal_motion(x - moved_x, y - moved_y, 1);
      moved_x = x;
      moved_y = y;
    }
    g_usleep(10000);
  }
  release_input();
  clear_nodes();
  frame_window = -1;
  if (error)
    return error;
  printf("{\"performed\":");
  quoted(operation, 40);
  printf(",\"released\":true,\"verified\":false}");
  return NULL;
}
static const char *input(Json request, int index) {
  char frame[200], operation[200];
  text(member(request, "frameId"), frame, sizeof(frame));
  text(member(request, "operation"), operation, sizeof(operation));
  Rectangle box;
  if (frame_window != index || strcmp(frame, frame_id) ||
      time(NULL) - frame_time > 15 || !window_bounds(index, &box) ||
      !same_bounds(box, frame_bounds))
    return "stale";
  if (!window_active(index))
    return "focus";
  int duration = number(request, "durationMs", 0);
  if (duration < 0 || duration > 5000)
    return "unsupported";
  if (!display)
    return portal_input(request, index, box, operation, duration);
  cancelled = 0;
  const char *error = NULL;
  if (!strcmp(operation, "move") || !strcmp(operation, "click")) {
    int x = number(request, "x", -1), y = number(request, "y", -1);
    if (x < 0 || y < 0 || x >= box.width || y >= box.height)
      return "unsupported";
    XTestFakeMotionEvent(display, -1, box.x + x, box.y + y, CurrentTime);
    if (!strcmp(operation, "click")) {
      char button[20];
      text(member(request, "button"), button, sizeof(button));
      held_button = !button[0] || !strcmp(button, "left") ? 1
                    : !strcmp(button, "right")            ? 3
                    : !strcmp(button, "middle")           ? 2
                                                          : 0;
      if (!held_button)
        return "unsupported";
      XTestFakeButtonEvent(display, (unsigned int)held_button, True,
                           CurrentTime);
    }
  } else if (!strcmp(operation, "scroll")) {
    int dx = number(request, "dx", 0), dy = number(request, "dy", 0);
    if (labs((long)dx) > 2000 || labs((long)dy) > 2000 || (!dx && !dy))
      return "unsupported";
    XTestFakeMotionEvent(display, -1, box.x + box.width / 2, box.y + box.height / 2, CurrentTime);
    int horizontal = (abs(dx) + 79) / 80, vertical = (abs(dy) + 79) / 80;
    for (int step = 0; step < horizontal + vertical; step++) {
      Rectangle current;
      if (cancelled || generation != active_generation)
        return "cancelled";
      if (!active(windows[index].handle) || !bounds(windows[index].handle, &current) || !same_bounds(current, box))
        return "focus";
      unsigned int button = step < vertical ? (dy > 0 ? 5 : 4) : (dx > 0 ? 7 : 6);
      XTestFakeButtonEvent(display, button, True, CurrentTime);
      XTestFakeButtonEvent(display, button, False, CurrentTime);
      XFlush(display);
      struct timespec delay = {0, 12000000};
      nanosleep(&delay, NULL);
    }
  } else if (!strcmp(operation, "relative_move")) {
    int dx = number(request, "dx", 0), dy = number(request, "dy", 0);
    if (labs((long)dx) > 2000 || labs((long)dy) > 2000)
      return "unsupported";
    XTestFakeRelativeMotionEvent(display, dx, dy, CurrentTime);
  } else if (!strcmp(operation, "keys")) {
    Json keys = member(request, "keys");
    const char *p = skip_space(keys.start);
    if (*p++ != '[')
      return "unsupported";
    char keymap[32];
    XQueryKeymap(display, keymap);
    while (*(p = skip_space(p)) != ']' && held_key_count < 6) {
      const char *end = skip_value(p, 0);
      if (!end) {
        error = "unsupported";
        break;
      }
      char key[30];
      text((Json){p, end}, key, sizeof(key));
      for (int i = 0; key[i]; i++)
        key[i] = (char)toupper((unsigned char)key[i]);
      KeySym symbol = key_symbol(key);
      KeyCode code = symbol ? XKeysymToKeycode(display, symbol) : 0;
      if (!code || (keymap[code / 8] & (1 << (code % 8)))) {
        error = "intervention";
        break;
      }
      held_keys[held_key_count++] = code;
      XTestFakeKeyEvent(display, code, True, CurrentTime);
      p = skip_space(end);
      if (*p == ',')
        p++;
      else if (*p != ']') {
        error = "unsupported";
        break;
      }
    }
    if (*p != ']' || !held_key_count)
      error = "unsupported";
  } else
    return "unsupported";
  XFlush(display);
  int camera_x = !strcmp(operation, "keys") ? number(request, "dx", 0) : 0;
  int camera_y = !strcmp(operation, "keys") ? number(request, "dy", 0) : 0;
  int moved_x = 0, moved_y = 0;
  if (labs((long)camera_x) > 2000 || labs((long)camera_y) > 2000) {
    release_input();
    return "unsupported";
  }
  for (int elapsed = 0; elapsed < duration && !error; elapsed += 10) {
    Rectangle current;
    if (cancelled || generation != active_generation)
      error = "cancelled";
    else if (!active(windows[index].handle) ||
             !bounds(windows[index].handle, &current) ||
             !same_bounds(current, box))
      error = "focus";
    if (!error && (camera_x || camera_y)) {
      int next_x = camera_x * elapsed / duration,
          next_y = camera_y * elapsed / duration;
      XTestFakeRelativeMotionEvent(display, next_x - moved_x, next_y - moved_y,
                                   CurrentTime);
      XFlush(display);
      moved_x = next_x;
      moved_y = next_y;
    }
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
  }
  release_input();
  clear_nodes();
  frame_window = -1;
  if (error)
    return error;
  printf("{\"performed\":");
  quoted(operation, 40);
  printf(",\"released\":true,\"verified\":false}");
  return NULL;
}
static const char *execute(Json request) {
  portal_poll();
  char action[200];
  text(member(request, "action"), action, sizeof(action));
  if (number(request, "version", 0) != 1)
    return "unsupported";
  if (!strcmp(action, "status")) {
    printf("{\"appVersion\":\"%s\",\"platform\":\"linux\",\"accessibility\":%s,\"input\":%s,"
           "\"screenshots\":%s,\"audio\":false,\"session\":\"%s\","
           "\"portalAvailable\":%s,\"portalRequired\":%s,\"permissionPending\":%s}",
           ARARA_APP_VERSION, desktop_fn ? "true" : "false",
           display || portal_input_ready() ? "true" : "false",
           display || portal_capture_ready() ? "true" : "false",
           getenv("WAYLAND_DISPLAY") ? "wayland" : "x11",
           portal_available() ? "true" : "false",
           !display && portal_available() && !portal_input_ready() ? "true" : "false",
           portal_pending() ? "true" : "false");
    return NULL;
  }
  if (!strcmp(action, "setup")) {
    if (display || !getenv("WAYLAND_DISPLAY"))
      return "unsupported";
    cancelled = 0;
    const char *error = portal_setup(interrupted);
    if (error && !strcmp(error, "portal_pending")) {
      printf("{\"configured\":false,\"permissionPending\":true}");
      return NULL;
    }
    if (error)
      return error;
    frame_window = -1;
    clear_nodes();
    printf("{\"configured\":true}");
    return NULL;
  }
  if (!strcmp(action, "stop")) {
    release_input();
    portal_close();
    frame_window = -1;
    printf("{\"stopped\":true}");
    return NULL;
  }
  if (!display && !strcmp(action, "list_windows"))
    return accessible_windows(request);
  if (!strcmp(action, "launch_app")) {
    char application[200];
    text(member(request, "application"), application, sizeof(application));
    if (!application[0] || application[0] == '-')
      return "unsupported";
    for (int i = 0; application[i]; i++)
      if (!isalnum((unsigned char)application[i]) && application[i] != '.' &&
          application[i] != '_' && application[i] != '-')
        return "unsupported";
    char identifier[220];
    snprintf(identifier, sizeof(identifier), "%s%s", application,
        g_str_has_suffix(application, ".desktop") ? "" : ".desktop");
    GDesktopAppInfo *app = g_desktop_app_info_new(identifier);
    if (!app)
      return "unsupported";
    GError *error = NULL;
    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    int launched = null_fd >= 0 && g_desktop_app_info_launch_uris_as_manager_with_fds(
        app, NULL, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
        null_fd, null_fd, null_fd, &error);
    if (null_fd >= 0)
      close(null_fd);
    g_clear_error(&error);
    g_object_unref(app);
    if (!launched)
      return "failed";
    printf("{\"requested\":true}");
    return NULL;
  }
  if (!strcmp(action, "list_windows"))
    return list_windows(request);
  int index = select_window(request);
  if (index < 0)
    return "stale";
  if (windows[index].screen) {
    if (!strcmp(action, "window_info")) {
      printf("{\"application\":\"Desktop\",\"surface\":\"screen\"}");
      return NULL;
    }
    if (!strcmp(action, "focus_window")) {
      printf("{\"focused\":true}");
      return NULL;
    }
    if (!strcmp(action, "take_snapshot") || !strcmp(action, "take_screenshot")) {
      if (frame_foreground)
        unref_fn(frame_foreground);
      frame_foreground = NULL;
      for (int i = 0; i < window_count && !frame_foreground; i++)
        if (windows[i].accessible && window_active(i)) {
          frame_foreground = ref_fn(windows[i].accessible);
          frame_foreground_pid = (unsigned int)windows[i].pid;
        }
    }
    if (!strcmp(action, "take_snapshot"))
      return snapshot(index);
    if (!strcmp(action, "take_screenshot"))
      return screenshot(index);
    if (!strcmp(action, "input"))
      return input(request, index);
    return "unsupported";
  }
  if (display && !safe_window(windows[index].handle))
    return "secure";
  if (!display && !strcmp(action, "window_info")) {
    char *title = name_fn(windows[index].accessible, NULL);
    char process_path[80], application[200] = {0};
    snprintf(process_path, sizeof(process_path), "/proc/%lu/comm",
             windows[index].pid);
    FILE *process_file = fopen(process_path, "r");
    if (process_file) {
      if (fgets(application, sizeof(application), process_file))
        application[strcspn(application, "\r\n")] = 0;
      fclose(process_file);
    }
    printf("{\"title\":");
    quoted(title, 160);
    printf(",\"application\":");
    quoted(application, 160);
    document_metadata(index);
    printf("}");
    if (title)
      free_fn(title);
    return NULL;
  }
  if (!display && !strcmp(action, "focus_window")) {
    void *component = component_fn(windows[index].accessible);
    int focused = component && focus_fn(component, NULL);
    if (component)
      unref_fn(component);
    if (!focused)
      return "focus";
    printf("{\"focused\":true}");
    return NULL;
  }
  if (!strcmp(action, "window_info")) {
    char *title = NULL;
    XFetchName(display, windows[index].handle, &title);
    XClassHint hint = {0};
    XGetClassHint(display, windows[index].handle, &hint);
    printf("{\"title\":");
    quoted(title, 160);
    printf(",\"application\":");
    quoted(hint.res_class, 160);
    document_metadata(index);
    printf("}");
    if (title)
      XFree(title);
    if (hint.res_class)
      XFree(hint.res_class);
    if (hint.res_name)
      XFree(hint.res_name);
    return NULL;
  }
  if (!strcmp(action, "focus_window")) {
    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.window = windows[index].handle;
    event.xclient.message_type =
        XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    clear_nodes();
    printf("{\"requested\":true}");
    return NULL;
  }
  if (!strcmp(action, "take_snapshot"))
    return snapshot(index);
  if (!strcmp(action, "take_screenshot"))
    return screenshot(index);
  if (!strcmp(action, "input"))
    return input(request, index);
  char uid[200];
  text(member(request, "uid"), uid, sizeof(uid));
  NodeRef *node = NULL;
  for (int i = 0; i < node_count; i++)
    if (nodes[i].window == index && !strcmp(nodes[i].uid, uid))
      node = &nodes[i];
  if (!node)
    return "stale";
  char *name = name_fn(node->element, NULL);
  char *role = role_fn(node->element, NULL);
  int stale = !name || strcmp(name, node->name);
  int secure = role && strstr(role, "password");
  if (name)
    free_fn(name);
  if (role)
    free_fn(role);
  if (secure)
    return "secure";
  if (stale)
    return "stale";
  if (!strcmp(action, "read")) {
    void *iface = text_fn(node->element);
    int offset = number(request, "offset", 0);
    if (offset < 0 || offset > 10000)
      return "unsupported";
    char *value =
        iface ? get_text_fn(iface, offset, offset + 4000, NULL) : NULL;
    printf("{\"value\":");
    quoted(value ? value : node->name, 16000);
    printf("}");
    if (value)
      free_fn(value);
    if (iface)
      unref_fn(iface);
    return NULL;
  }
  if (strcmp(action, "act"))
    return "unsupported";
  if (!window_active(index))
    return "focus";
  char operation[200];
  text(member(request, "operation"), operation, sizeof(operation));
  int success = 0;
  void *iface = NULL;
  if (!strcmp(operation, "press")) {
    iface = action_fn(node->element);
    if (iface)
      success = invoke_fn(iface, 0, NULL);
  } else if (!strcmp(operation, "focus")) {
    iface = component_fn(node->element);
    if (iface)
      success = focus_fn(iface, NULL);
  } else if (!strcmp(operation, "set_value")) {
    char value[32001];
    text(member(request, "value"), value, sizeof(value));
    iface = editable_fn(node->element);
    if (iface)
      success = set_text_fn(iface, value, NULL);
  }
  if (iface)
    unref_fn(iface);
  clear_nodes();
  if (!success)
    return "unsupported";
  printf("{\"performed\":");
  quoted(operation, 40);
  printf(",\"verified\":false}");
  return NULL;
}
static void *reader(void *unused) {
  (void)unused;
  char *line = NULL;
  size_t size = 0;
  while (getline(&line, &size, stdin) >= 0) {
    if (strlen(line) > 64000)
      continue;
    const char *end = skip_value(line, 0);
    if (!end || *skip_space(end))
      continue;
    Json request = {line, end};
    char action[30];
    text(member(request, "action"), action, sizeof(action));
    if (!strcmp(action, "stop"))
      cancelled = 1;
    generation++;
    pthread_mutex_lock(&state_lock);
    while (queued)
      pthread_cond_wait(&available, &state_lock);
    snprintf(line_queue, sizeof(line_queue), "%s", line);
    queued_generation = generation;
    queued = 1;
    pthread_cond_broadcast(&available);
    pthread_mutex_unlock(&state_lock);
  }
  free(line);
  cancelled = 1;
  generation++;
  pthread_mutex_lock(&state_lock);
  closed_input = 1;
  pthread_cond_broadcast(&available);
  pthread_mutex_unlock(&state_lock);
  return NULL;
}
static void terminate(int sig) {
  (void)sig;
  cancelled = 1;
  generation++;
}
static int xerror(Display *connection, XErrorEvent *event) {
  (void)connection;
  (void)event;
  return 0;
}
int main(int argc, char **argv) {
  int permissions = argc == 2 && !strcmp(argv[1], "--permissions");
  if (getenv("WAYLAND_DISPLAY"))
    platform_scope(permissions);
  signal(SIGTERM, terminate);
  signal(SIGINT, terminate);
  XSetErrorHandler(xerror);
  if (!getenv("WAYLAND_DISPLAY"))
    display = XOpenDisplay(NULL);
  load_atspi();
  portal_init();
  if (permissions) {
    const char *error = portal_setup(interrupted);
    while (portal_pending() && !cancelled) {
      portal_poll();
      g_usleep(10000);
    }
    int ready = portal_input_ready() && portal_capture_ready();
    portal_close();
    return ready && (!error || !strcmp(error, "portal_pending")) ? 0 : 1;
  }
  pthread_t input_thread;
  pthread_create(&input_thread, NULL, reader, NULL);
  for (;;) {
    char line[64001];
    pthread_mutex_lock(&state_lock);
    while (!queued && !closed_input) {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 10000000;
      if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&available, &state_lock, &deadline);
      if (!queued && !closed_input) {
        pthread_mutex_unlock(&state_lock);
        portal_poll();
        pthread_mutex_lock(&state_lock);
      }
    }
    if (!queued && closed_input) {
      pthread_mutex_unlock(&state_lock);
      break;
    }
    memcpy(line, line_queue, sizeof(line));
    active_generation = queued_generation;
    queued = 0;
    pthread_cond_broadcast(&available);
    pthread_mutex_unlock(&state_lock);
    Json request = {line, line + strlen(line)};
    FILE *saved = stdout;
    char *buffer = NULL;
    size_t length = 0;
    FILE *capture = open_memstream(&buffer, &length);
    if (!capture)
      break;
    stdout = capture;
    char requested_action[30];
    text(member(request, "action"), requested_action, sizeof(requested_action));
    const char *error = generation != active_generation &&
                                strcmp(requested_action, "status") &&
                                strcmp(requested_action, "stop")
                            ? "cancelled"
                            : execute(request);
    fflush(capture);
    stdout = saved;
    fclose(capture);
    if (error) {
      printf("{\"version\":1,\"error\":");
      quoted(error, 100);
      printf("}\n");
    } else
      printf("{\"version\":1,\"result\":%s}\n", buffer ? buffer : "{}");
    free(buffer);
    fflush(stdout);
  }
  release_input();
  portal_close();
  clear_nodes();
  clear_windows();
  if (display)
    XCloseDisplay(display);
  pthread_join(input_thread, NULL);
  return 0;
}
