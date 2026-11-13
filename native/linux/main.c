#define _POSIX_C_SOURCE 200809L
#include "json.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <dlfcn.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <zlib.h>
typedef struct { Window handle;
 unsigned long pid;
 char id[40];
 } WindowRef;
typedef struct { void *element;
 char uid[40];
 char name[1024];
 int window;
 } NodeRef;
typedef struct { int x, y, width, height;
 } Rectangle;
static Display *display;
static WindowRef windows[100];
static int window_count;
static NodeRef nodes[100];
static int node_count;
static unsigned long sequence;
static volatile sig_atomic_t cancelled;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static char line_queue[64001];
static int queued;
static int closed_input;
static pthread_cond_t available = PTHREAD_COND_INITIALIZER;
static char frame_id[40];
static int frame_window = -1;
static Rectangle frame_bounds;
static time_t frame_time;
static KeyCode held_keys[6];
static int held_key_count;
static int held_button;
static void *atspi_lib;
static void *gobject_lib;
static void *glib_lib;
static int (*atspi_init_fn)(void);
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
static char *(*get_text_fn)(void *, int, int, void *);
static void (*unref_fn)(void *);
static void (*free_fn)(void *);
static void load_atspi(void) {
    atspi_lib = dlopen("libatspi.so.0", RTLD_NOW);
    gobject_lib = dlopen("libgobject-2.0.so.0", RTLD_NOW);
    glib_lib = dlopen("libglib-2.0.so.0", RTLD_NOW);
    if (!atspi_lib || !gobject_lib || !glib_lib) return;
#define LOAD(variable, symbol) do { *(void **)(&(variable)) = dlsym(atspi_lib, symbol);
 if (!(variable)) return;
 } while (0)
    LOAD(atspi_init_fn, "atspi_init");
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
#undef LOAD
    *(void **)(&unref_fn) = dlsym(gobject_lib, "g_object_unref");
    *(void **)(&free_fn) = dlsym(glib_lib, "g_free");
    if (!unref_fn || !free_fn || atspi_init_fn() != 0) desktop_fn = NULL;
}
static void clear_nodes(void) {
    if (unref_fn) for (int i = 0; i < node_count; i++) unref_fn(nodes[i].element);
    node_count = 0;
}
static unsigned long property(Window handle, const char *name) {
    Atom type;
 int format;
 unsigned long count, left;
 unsigned char *data = NULL;
    XGetWindowProperty(display, handle, XInternAtom(display, name, False), 0, 1, False, AnyPropertyType, &type, &format, &count, &left, &data);
    unsigned long value = data && format == 32 && count ? *(unsigned long *)data : 0;
    if (data) XFree(data);
    return value;
}
static int bounds(Window handle, Rectangle *out) {
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display, handle, &attributes) || attributes.map_state != IsViewable) return 0;
    Window child;
 int x, y;
    XTranslateCoordinates(display, handle, DefaultRootWindow(display), 0, 0, &x, &y, &child);
    *out = (Rectangle){x, y, attributes.width, attributes.height};
    return attributes.width > 0 && attributes.height > 0;
}
static int same_bounds(Rectangle a, Rectangle b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
 }
static int active(Window handle) { return property(DefaultRootWindow(display), "_NET_ACTIVE_WINDOW") == handle;
 }
static int select_window(Json request) {
    char id[200];
 text(member(request, "windowId"), id, sizeof(id));
    for (int i = 0; i < window_count; i++) if (!strcmp(id, windows[i].id) && property(windows[i].handle, "_NET_WM_PID") == windows[i].pid) return i;
    return -1;
}
static int safe_window(Window handle) {
    XClassHint hint = {0};
    if (!XGetClassHint(display, handle, &hint)) return 0;
    int safe = hint.res_class && !strstr(hint.res_class, "polkit") && !strstr(hint.res_class, "Polkit") && !strstr(hint.res_class, "keyring") && !strstr(hint.res_class, "Settings");
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return safe;
}
static const char *list_windows(Json request) {
    window_count = 0;
 clear_nodes();
 frame_window = -1;
    Atom type;
 int format;
 unsigned long count, left;
 unsigned char *data = NULL;
    XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, "_NET_CLIENT_LIST", False), 0, 1000, False, XA_WINDOW, &type, &format, &count, &left, &data);
    char selected[200];
 text(member(request, "application"), selected, sizeof(selected));
    printf("{\"windows\":[");
 int comma = 0;
    if (data && format == 32) for (unsigned long n = 0; n < count && window_count < 100; n++) {
        Window handle = ((Window *)data)[n];
 Rectangle rect;
        unsigned long pid = property(handle, "_NET_WM_PID");
        if (!pid || !safe_window(handle) || !bounds(handle, &rect)) continue;
        XClassHint hint = {0};
 XGetClassHint(display, handle, &hint);
        int match = !selected[0] || (hint.res_class && !strcmp(hint.res_class, selected));
        char *name = NULL;
 XFetchName(display, handle, &name);
        if (match) {
            WindowRef *ref = &windows[window_count++];
 ref->handle = handle;
 ref->pid = pid;
 snprintf(ref->id, sizeof(ref->id), "w%lu", ++sequence);
            if (comma++) putchar(',');
 printf("{\"windowId\":");
 quoted(ref->id, 40);
 printf(",\"app\":");
 quoted(hint.res_class, 160);
 printf(",\"title\":");
 quoted(name, 160);
 printf(",\"active\":%s}", active(handle) ? "true" : "false");
        }
        if (hint.res_class) XFree(hint.res_class);
 if (hint.res_name) XFree(hint.res_name);
 if (name) XFree(name);
    }
    if (data) XFree(data);
    printf("],\"truncated\":%s}", window_count >= 100 ? "true" : "false");
 return NULL;
}
static void *root_for(int index) {
    if (!desktop_fn) return NULL;
    void *desktop = desktop_fn(0);
 if (!desktop) return NULL;
    int count = count_fn(desktop, NULL);
 void *result = NULL;
    Rectangle rect;
 if (!bounds(windows[index].handle, &rect)) { unref_fn(desktop);
 return NULL;
 }
    for (int i = 0; i < count && i < 100 && !result; i++) {
        void *app = child_fn(desktop, i, NULL);
 if (!app) continue;
        if (pid_fn(app, NULL) == windows[index].pid) {
            int children = count_fn(app, NULL);
            for (int j = 0; j < children && j < 40 && !result; j++) {
                void *window = child_fn(app, j, NULL);
 if (!window) continue;
                void *component = component_fn(window);
 Rectangle *box = component ? extents_fn(component, 0, NULL) : NULL;
                if (box && abs(box->x - rect.x) < 4 && abs(box->y - rect.y) < 40) result = window;
                else unref_fn(window);
                if (box) free_fn(box);
 if (component) unref_fn(component);
            }
        }
        unref_fn(app);
    }
    unref_fn(desktop);
 return result;
}
static const char *snapshot(int index) {
    clear_nodes();
    void *root = root_for(index);
    if (!root) { printf("{\"nodes\":[],\"truncated\":false,\"accessibility\":false}");
 return NULL;
 }
    void *queue[601];
 int depth[601];
 int head = 0, tail = 1;
 queue[0] = root;
 depth[0] = 0;
    printf("{\"nodes\":[");
 int comma = 0;
    while (head < tail && node_count < 100 && !cancelled) {
        int position = head++;
 void *element = queue[position];
        char *role = role_fn(element, NULL);
 char *name = name_fn(element, NULL);
        if (role && strstr(role, "password")) { if (name) free_fn(name);
 free_fn(role);
 unref_fn(element);
 continue;
 }
        NodeRef *node = &nodes[node_count++];
 node->element = element;
 node->window = index;
 snprintf(node->uid, sizeof(node->uid), "n%lu", ++sequence);
 snprintf(node->name, sizeof(node->name), "%s", name ? name : "");
        void *action = action_fn(element);
 void *editable = editable_fn(element);
 void *component = component_fn(element);
        if (comma++) putchar(',');
 printf("{\"uid\":");
 quoted(node->uid, 40);
 printf(",\"role\":");
 quoted(role, 100);
 printf(",\"name\":");
 quoted(name, 160);
 printf(",\"enabled\":true,\"operations\":[");
 int operation = 0;
        if (action && actions_fn(action, NULL) > 0) { printf("\"press\"");
 operation = 1;
 }
        if (editable) { printf("%s\"set_value\"", operation ? "," : "");
 operation = 1;
 }
        if (component) printf("%s\"focus\"", operation ? "," : "");
 printf("]}");
        int count = count_fn(element, NULL);
        if (depth[position] < 12) for (int n = 0; n < count && n < 100 && tail < 600; n++) { void *child = child_fn(element, n, NULL);
 if (child) { queue[tail] = child;
 depth[tail++] = depth[position] + 1;
 } }
        if (action) unref_fn(action);
 if (editable) unref_fn(editable);
 if (component) unref_fn(component);
 if (role) free_fn(role);
 if (name) free_fn(name);
    }
    int truncated = head < tail;
 while (head < tail) unref_fn(queue[head++]);
    printf("],\"truncated\":%s}", truncated ? "true" : "false");
 return NULL;
}
static void release_input(void) {
    if (!display) return;
    for (int i = 0; i < held_key_count; i++) XTestFakeKeyEvent(display, held_keys[i], False, CurrentTime);
    if (held_button) XTestFakeButtonEvent(display, (unsigned int)held_button, False, CurrentTime);
    held_key_count = 0;
 held_button = 0;
 XFlush(display);
}
static KeySym key_symbol(const char *key) {
    if (!strcmp(key, "ENTER")) return XK_Return;
 if (!strcmp(key, "SPACE")) return XK_space;
 if (!strcmp(key, "SHIFT")) return XK_Shift_L;
 if (!strcmp(key, "CTRL")) return XK_Control_L;
 if (!strcmp(key, "ALT")) return XK_Alt_L;
 if (!strcmp(key, "META")) return XK_Super_L;
    if (!strcmp(key, "ESCAPE")) return XK_Escape;
 if (!strcmp(key, "TAB")) return XK_Tab;
 if (!strcmp(key, "BACKSPACE")) return XK_BackSpace;
    if (!strcmp(key, "UP")) return XK_Up;
 if (!strcmp(key, "DOWN")) return XK_Down;
 if (!strcmp(key, "LEFT")) return XK_Left;
 if (!strcmp(key, "RIGHT")) return XK_Right;
    return strlen(key) == 1 && isalnum((unsigned char)key[0]) ? XStringToKeysym(key) : NoSymbol;
}
static void binary32(unsigned char *out, uint32_t value) { out[0] = (unsigned char)(value >> 24);
 out[1] = (unsigned char)(value >> 16);
 out[2] = (unsigned char)(value >> 8);
 out[3] = (unsigned char)value;
 }
static void chunk(unsigned char *out, const char *type, const unsigned char *data, uint32_t size) { binary32(out, size);
 memcpy(out + 4, type, 4);
 if (size) memcpy(out + 8, data, size);
 binary32(out + 8 + size, (uint32_t)crc32(0, out + 4, size + 4));
 }
static void base64(const unsigned char *data, size_t size) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < size; i += 3) { unsigned int value = (unsigned int)data[i] << 16;
 if (i + 1 < size) value |= (unsigned int)data[i + 1] << 8;
 if (i + 2 < size) value |= data[i + 2];
 putchar(alphabet[value >> 18]);
 putchar(alphabet[(value >> 12) & 63]);
 putchar(i + 1 < size ? alphabet[(value >> 6) & 63] : '=');
 putchar(i + 2 < size ? alphabet[value & 63] : '=');
 }
}
static unsigned char channel(unsigned long value, unsigned long mask) { if (!mask) return 0;
 while (!(mask & 1)) { mask >>= 1;
 value >>= 1;
 } return (unsigned char)((value & mask) * 255 / mask);
 }
static const char *screenshot(int index) {
    Rectangle box;
    if (!active(windows[index].handle) || !bounds(windows[index].handle, &box)) return "focus";
    if (box.width > 16000 || box.height > 16000) return "unsupported";
    XImage *image = XGetImage(display, windows[index].handle, 0, 0, (unsigned int)box.width, (unsigned int)box.height, AllPlanes, ZPixmap);
 if (!image) return "failed";
    int width = box.width > 1280 ? 1280 : box.width;
 int height = box.height * width / box.width;
    size_t raw_size = (size_t)(width * 3 + 1) * (size_t)height;
 unsigned char *raw = calloc(1, raw_size);
 if (!raw) { XDestroyImage(image);
 return "failed";
 }
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) { unsigned long pixel = XGetPixel(image, x * box.width / width, y * box.height / height);
 size_t at = (size_t)y * (size_t)(width * 3 + 1) + (size_t)x * 3 + 1;
 raw[at] = channel(pixel, image->red_mask);
 raw[at + 1] = channel(pixel, image->green_mask);
 raw[at + 2] = channel(pixel, image->blue_mask);
 }
    XDestroyImage(image);
 uLongf compressed_size = compressBound(raw_size);
 unsigned char *compressed = malloc(compressed_size);
 if (!compressed) { free(raw);
 return "failed";
 }
    if (compress2(compressed, &compressed_size, raw, raw_size, 6) != Z_OK || compressed_size > 4000000) { free(raw);
 free(compressed);
 return "failed";
 } free(raw);
    size_t png_size = (size_t)compressed_size + 57;
 unsigned char *png = malloc(png_size);
 if (!png) { free(compressed);
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
 frame_bounds = box;
 frame_time = time(NULL);
 snprintf(frame_id, sizeof(frame_id), "f%lu", ++sequence);
    printf("{\"frameId\":");
 quoted(frame_id, 40);
 printf(",\"width\":%d,\"height\":%d,\"coordinateWidth\":%d,\"coordinateHeight\":%d,\"images\":[{\"mimeType\":\"image/png\",\"data\":\"", width, height, box.width, box.height);
 base64(png, png_size);
 printf("\"}]}");
 free(png);
 return NULL;
}
static const char *input(Json request, int index) {
    char frame[200], operation[200];
 text(member(request, "frameId"), frame, sizeof(frame));
 text(member(request, "operation"), operation, sizeof(operation));
 Rectangle box;
    if (frame_window != index || strcmp(frame, frame_id) || time(NULL) - frame_time > 15 || !bounds(windows[index].handle, &box) || !same_bounds(box, frame_bounds)) return "stale";
    if (!active(windows[index].handle)) return "focus";
    int duration = number(request, "durationMs", 0);
 if (duration < 0 || duration > 5000) return "unsupported";
    cancelled = 0;
 const char *error = NULL;
    if (!strcmp(operation, "move") || !strcmp(operation, "click")) {
        int x = number(request, "x", -1), y = number(request, "y", -1);
 if (x < 0 || y < 0 || x >= box.width || y >= box.height) return "unsupported";
        XTestFakeMotionEvent(display, -1, box.x + x, box.y + y, CurrentTime);
        if (!strcmp(operation, "click")) { char button[20];
 text(member(request, "button"), button, sizeof(button));
 held_button = !button[0] || !strcmp(button, "left") ? 1 : !strcmp(button, "right") ? 3 : !strcmp(button, "middle") ? 2 : 0;
 if (!held_button) return "unsupported";
 XTestFakeButtonEvent(display, (unsigned int)held_button, True, CurrentTime);
 }
    } else if (!strcmp(operation, "relative_move")) {
        int dx = number(request, "dx", 0), dy = number(request, "dy", 0);
 if (labs((long)dx) > 2000 || labs((long)dy) > 2000) return "unsupported";
 XTestFakeRelativeMotionEvent(display, dx, dy, CurrentTime);
    } else if (!strcmp(operation, "keys")) {
        Json keys = member(request, "keys");
 const char *p = skip_space(keys.start);
 if (*p++ != '[') return "unsupported";
        char keymap[32];
 XQueryKeymap(display, keymap);
        while (*(p = skip_space(p)) != ']' && held_key_count < 6) {
            const char *end = skip_value(p, 0);
 if (!end) { error = "unsupported";
 break;
 } char key[30];
 text((Json){p, end}, key, sizeof(key));
 for (int i = 0; key[i]; i++) key[i] = (char)toupper((unsigned char)key[i]);
            KeySym symbol = key_symbol(key);
 KeyCode code = symbol ? XKeysymToKeycode(display, symbol) : 0;
 if (!code || (keymap[code / 8] & (1 << (code % 8)))) { error = "intervention";
 break;
 }
            held_keys[held_key_count++] = code;
 XTestFakeKeyEvent(display, code, True, CurrentTime);
 p = skip_space(end);
 if (*p == ',') p++;
 else if (*p != ']') { error = "unsupported";
 break;
 }
        }
        if (*p != ']' || !held_key_count) error = "unsupported";
    } else return "unsupported";
    XFlush(display);
    for (int elapsed = 0; elapsed < duration && !error; elapsed += 10) { Rectangle current;
 if (cancelled) error = "cancelled";
 else if (!active(windows[index].handle) || !bounds(windows[index].handle, &current) || !same_bounds(current, box)) error = "focus";
 struct timespec delay = {0, 10000000};
 nanosleep(&delay, NULL);
 }
    release_input();
 frame_window = -1;
    if (error) return error;
    printf("{\"performed\":");
 quoted(operation, 40);
 printf(",\"released\":true,\"verified\":false}");
 return NULL;
}
static const char *execute(Json request) {
    char action[200];
 text(member(request, "action"), action, sizeof(action));
    if (number(request, "version", 0) != 1) return "unsupported";
    if (!strcmp(action, "status")) { printf("{\"platform\":\"linux\",\"accessibility\":%s,\"input\":%s,\"screenshots\":%s,\"audio\":false,\"session\":\"%s\"}", desktop_fn ? "true" : "false", display ? "true" : "false", display ? "true" : "false", getenv("WAYLAND_DISPLAY") ? "wayland" : "x11");
 return NULL;
 }
    if (!strcmp(action, "stop")) { release_input();
 frame_window = -1;
 printf("{\"stopped\":true}");
 return NULL;
 }
    if (!display) return "session_unavailable";
    if (!strcmp(action, "launch_app")) {
        char application[200]; text(member(request, "application"), application, sizeof(application));
        if (!application[0] || application[0] == '-') return "unsupported";
        for (int i = 0; application[i]; i++) if (!isalnum((unsigned char)application[i]) && application[i] != '.' && application[i] != '_' && application[i] != '-') return "unsupported";
        pid_t child = fork();
        if (child == 0) { execlp("gtk-launch", "gtk-launch", application, (char *)NULL); _exit(127); }
        if (child < 0) return "failed";
        printf("{\"requested\":true}"); return NULL;
    }
    if (!strcmp(action, "list_windows")) return list_windows(request);
    int index = select_window(request);
 if (index < 0) return "stale";
    if (!safe_window(windows[index].handle)) return "secure";
    if (!strcmp(action, "window_info")) { char *title = NULL;
 XFetchName(display, windows[index].handle, &title);
 XClassHint hint = {0};
 XGetClassHint(display, windows[index].handle, &hint);
 printf("{\"title\":");
 quoted(title, 160);
 printf(",\"application\":");
 quoted(hint.res_class, 160);
 printf("}");
 if (title) XFree(title);
 if (hint.res_class) XFree(hint.res_class);
 if (hint.res_name) XFree(hint.res_name);
 return NULL;
 }
    if (!strcmp(action, "focus_window")) { XEvent event = {0};
 event.xclient.type = ClientMessage;
 event.xclient.window = windows[index].handle;
 event.xclient.message_type = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
 event.xclient.format = 32;
 event.xclient.data.l[0] = 1;
 XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
 XFlush(display);
 clear_nodes();
 printf("{\"requested\":true}");
 return NULL;
 }
    if (!strcmp(action, "take_snapshot")) return snapshot(index);
    if (!strcmp(action, "take_screenshot")) return screenshot(index);
    if (!strcmp(action, "input")) return input(request, index);
    char uid[200];
 text(member(request, "uid"), uid, sizeof(uid));
 NodeRef *node = NULL;
    for (int i = 0; i < node_count; i++) if (nodes[i].window == index && !strcmp(nodes[i].uid, uid)) node = &nodes[i];
    if (!node) return "stale";
    char *name = name_fn(node->element, NULL);
 char *role = role_fn(node->element, NULL);
 int stale = !name || strcmp(name, node->name);
 int secure = role && strstr(role, "password");
 if (name) free_fn(name);
 if (role) free_fn(role);
 if (secure) return "secure";
 if (stale) return "stale";
    if (!strcmp(action, "read")) { void *iface = text_fn(node->element);
 int offset = number(request, "offset", 0);
 if (offset < 0 || offset > 10000) return "unsupported";
 char *value = iface ? get_text_fn(iface, offset, offset + 4000, NULL) : NULL;
 printf("{\"value\":");
 quoted(value ? value : node->name, 16000);
 printf("}");
 if (value) free_fn(value);
 if (iface) unref_fn(iface);
 return NULL;
 }
    if (strcmp(action, "act")) return "unsupported";
    if (!active(windows[index].handle)) return "focus";
    char operation[200];
 text(member(request, "operation"), operation, sizeof(operation));
 int success = 0;
 void *iface = NULL;
    if (!strcmp(operation, "press")) { iface = action_fn(node->element);
 if (iface) success = invoke_fn(iface, 0, NULL);
 }
    else if (!strcmp(operation, "focus")) { iface = component_fn(node->element);
 if (iface) success = focus_fn(iface, NULL);
 }
    else if (!strcmp(operation, "set_value")) { char value[32001];
 text(member(request, "value"), value, sizeof(value));
 iface = editable_fn(node->element);
 if (iface) success = set_text_fn(iface, value, NULL);
 }
    if (iface) unref_fn(iface);
 clear_nodes();
 if (!success) return "unsupported";
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
        if (strlen(line) > 64000) continue;
        const char *end = skip_value(line, 0);
 if (!end || *skip_space(end)) continue;
        Json request = {line, end};
 char action[30];
 text(member(request, "action"), action, sizeof(action));
        if (!strcmp(action, "stop")) cancelled = 1;
        pthread_mutex_lock(&state_lock);
        while (queued) pthread_cond_wait(&available, &state_lock);
        snprintf(line_queue, sizeof(line_queue), "%s", line);
 queued = 1;
 pthread_cond_broadcast(&available);
 pthread_mutex_unlock(&state_lock);
    }
    free(line);
 cancelled = 1;
 pthread_mutex_lock(&state_lock);
 closed_input = 1;
 pthread_cond_broadcast(&available);
 pthread_mutex_unlock(&state_lock);
 return NULL;
}
static void terminate(int sig) { (void)sig;
 cancelled = 1;
 }
static int xerror(Display *connection, XErrorEvent *event) { (void)connection;
 (void)event;
 return 0;
 }
int main(void) {
    signal(SIGTERM, terminate);
 signal(SIGINT, terminate);
 XSetErrorHandler(xerror);
    if (!getenv("WAYLAND_DISPLAY")) display = XOpenDisplay(NULL);
    load_atspi();
 pthread_t input_thread;
 pthread_create(&input_thread, NULL, reader, NULL);
    for (;;) {
        char line[64001];
 pthread_mutex_lock(&state_lock);
 while (!queued && !closed_input) pthread_cond_wait(&available, &state_lock);
        if (!queued && closed_input) { pthread_mutex_unlock(&state_lock);
 break;
 }
        memcpy(line, line_queue, sizeof(line));
 queued = 0;
 pthread_cond_broadcast(&available);
 pthread_mutex_unlock(&state_lock);
        Json request = {line, line + strlen(line)};
        FILE *saved = stdout;
 char *buffer = NULL;
 size_t length = 0;
 FILE *capture = open_memstream(&buffer, &length);
 if (!capture) break;
 stdout = capture;
        const char *error = execute(request);
 fflush(capture);
 stdout = saved;
 fclose(capture);
        if (error) { printf("{\"version\":1,\"error\":");
 quoted(error, 100);
 printf("}\n");
 }
        else printf("{\"version\":1,\"result\":%s}\n", buffer ? buffer : "{}");
        free(buffer);
 fflush(stdout);
    }
    release_input();
 clear_nodes();
 if (display) XCloseDisplay(display);
 pthread_join(input_thread, NULL);
 return 0;
}
