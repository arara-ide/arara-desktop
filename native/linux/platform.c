#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "linux-locales.h"
#include <gio/gio.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *gtk_library, *gdk_library, *pixbuf_library;
static void *window, *image;
static char *parent;
static int headless, setup_cancelled;
static PortalRectangle screens[16], atlas;
static int screen_count;
static int (*gtk_init_check_fn)(int *, char ***);
static void *(*window_new_fn)(int);
static void (*window_title_fn)(void *, const char *);
static void (*window_size_fn)(void *, int, int);
static void (*window_present_fn)(void *);
static void *(*label_new_fn)(const char *);
static void (*container_add_fn)(void *, void *);
static void (*show_all_fn)(void *);
static void (*hide_fn)(void *);
static void *(*widget_window_fn)(void *);
static int (*export_fn)(void *, void (*)(void *, const char *, void *), void *, void *);
static void *(*display_fn)(void);
static int (*monitor_count_fn)(void *);
static void *(*monitor_fn)(void *, int);
static void (*geometry_fn)(void *, PortalRectangle *);
static void *(*image_load_fn)(const char *, GError **);
static int (*image_width_fn)(void *), (*image_height_fn)(void *);
static int (*image_channels_fn)(void *), (*image_stride_fn)(void *);
static unsigned char *(*image_pixels_fn)(void *);

static int actual_wayland(void) {
  const char *name = getenv("WAYLAND_DISPLAY");
  if (!name)
    return 0;
  char *path = g_path_is_absolute(name) ? g_strdup(name)
      : g_build_filename(g_get_user_runtime_dir(), name, NULL);
  struct stat info;
  int exists = !stat(path, &info) && S_ISSOCK(info.st_mode);
  g_free(path);
  return exists;
}

void platform_scope(int permissions) {
  if (!actual_wayland())
    return;
  char *current = NULL;
  g_file_get_contents("/proc/self/cgroup", &current, NULL, NULL);
  int scoped = current && strstr(current, "/app-arara-br.app.code.arara.Desktop-");
  g_free(current);
  if (scoped)
    return;
  char executable[4096], unit[160];
  ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length <= 0 || access("/usr/bin/systemd-run", X_OK))
    return;
  executable[length] = 0;
  snprintf(unit, sizeof(unit), "--unit=app-arara-br.app.code.arara.Desktop-%ld", (long)getpid());
  execl("/usr/bin/systemd-run", "systemd-run", "--user", "--scope", "--quiet",
        unit, executable, permissions ? "--permissions" : NULL, (char *)NULL);
}

int platform_init(void) {
  g_set_prgname("br.app.code.arara.Desktop");
  g_set_application_name("Arara Desktop");
  headless = !actual_wayland();
  gtk_library = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_LOCAL);
  gdk_library = dlopen("libgdk-3.so.0", RTLD_NOW | RTLD_LOCAL);
  pixbuf_library = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!gtk_library || !gdk_library || !pixbuf_library)
    return 0;
#define LOAD(library, variable, symbol) \
  *(void **)(&variable) = dlsym(library, symbol); if (!variable) return 0;
  LOAD(gtk_library, gtk_init_check_fn, "gtk_init_check");
  LOAD(gtk_library, window_new_fn, "gtk_window_new");
  LOAD(gtk_library, window_title_fn, "gtk_window_set_title");
  LOAD(gtk_library, window_size_fn, "gtk_window_set_default_size");
  LOAD(gtk_library, window_present_fn, "gtk_window_present");
  LOAD(gtk_library, label_new_fn, "gtk_label_new");
  LOAD(gtk_library, container_add_fn, "gtk_container_add");
  LOAD(gtk_library, show_all_fn, "gtk_widget_show_all");
  LOAD(gtk_library, hide_fn, "gtk_widget_hide");
  LOAD(gtk_library, widget_window_fn, "gtk_widget_get_window");
  LOAD(gdk_library, export_fn, "gdk_wayland_window_export_handle");
  LOAD(gdk_library, display_fn, "gdk_display_get_default");
  LOAD(gdk_library, monitor_count_fn, "gdk_display_get_n_monitors");
  LOAD(gdk_library, monitor_fn, "gdk_display_get_monitor");
  LOAD(gdk_library, geometry_fn, "gdk_monitor_get_geometry");
  LOAD(pixbuf_library, image_load_fn, "gdk_pixbuf_new_from_file");
  LOAD(pixbuf_library, image_width_fn, "gdk_pixbuf_get_width");
  LOAD(pixbuf_library, image_height_fn, "gdk_pixbuf_get_height");
  LOAD(pixbuf_library, image_channels_fn, "gdk_pixbuf_get_n_channels");
  LOAD(pixbuf_library, image_stride_fn, "gdk_pixbuf_get_rowstride");
  LOAD(pixbuf_library, image_pixels_fn, "gdk_pixbuf_get_pixels");
#undef LOAD
  return gtk_init_check_fn(NULL, NULL) || headless;
}

static int register_application(void) {
  char executable[4096];
  ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length <= 0)
    return 0;
  executable[length] = 0;
  GString *command = g_string_new("\"");
  for (const char *p = executable; *p; p++) {
    if (strchr("\\\"`$", *p))
      g_string_append_c(command, '\\');
    if (*p == '%')
      g_string_append_c(command, '%');
    g_string_append_c(command, *p);
  }
  g_string_append(command, "\" --permissions");
  char *directory = g_build_filename(g_get_user_data_dir(), "applications", NULL);
  char *path = g_build_filename(directory, "br.app.code.arara.Desktop.desktop", NULL);
  GKeyFile *entry = g_key_file_new();
  int exists = g_file_test(path, G_FILE_TEST_EXISTS);
  int valid = !exists || (g_key_file_load_from_file(entry, path, G_KEY_FILE_NONE, NULL) &&
      g_key_file_get_boolean(entry, "Desktop Entry", "X-Arara-Desktop-Managed", NULL));
  if (valid) {
    g_key_file_set_string(entry, "Desktop Entry", "Type", "Application");
    g_key_file_set_string(entry, "Desktop Entry", "Name", "Arara Desktop");
    g_key_file_set_string(entry, "Desktop Entry", "Exec", command->str);
    g_key_file_set_boolean(entry, "Desktop Entry", "X-Arara-Desktop-Managed", TRUE);
    valid = !g_mkdir_with_parents(directory, 0700) && g_key_file_save_to_file(entry, path, NULL);
  }
  g_key_file_unref(entry);
  g_string_free(command, TRUE);
  g_free(directory);
  g_free(path);
  return valid;
}

static void exported(void *target, const char *handle, void *data) {
  (void)target;
  (void)data;
  g_free(parent);
  parent = g_strconcat("wayland:", handle, NULL);
}

static gboolean close_window(void *target, void *event, void *data) {
  (void)event;
  (void)data;
  setup_cancelled = 1;
  hide_fn(target);
  return TRUE;
}

int platform_setup_window(void) {
  setup_cancelled = 0;
  if (headless)
    return 1;
  if (!register_application())
    return 0;
  if (!window) {
    window = window_new_fn(0);
    window_title_fn(window, "Arara Desktop");
    window_size_fn(window, 580, 160);
    const char *locale = getenv("LC_ALL");
    if (!locale || !*locale) locale = getenv("LC_MESSAGES");
    if (!locale || !*locale) locale = getenv("LANG");
    int language = locale && !strncmp(locale, "pt", 2) ? 1 :
                   locale && !strncmp(locale, "es", 2) ? 2 : 0;
    container_add_fn(window, label_new_fn(arara_linux_setup_messages[language]));
    g_signal_connect_data(window, "delete-event", G_CALLBACK(close_window), NULL, NULL, 0);
  }
  show_all_fn(window);
  window_present_fn(window);
  if (!parent)
    export_fn(widget_window_fn(window), exported, NULL, NULL);
  gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
  while (!parent && !setup_cancelled && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  return parent != NULL;
}

int platform_setup_cancelled(void) { return setup_cancelled; }
const char *platform_parent(void) { return parent ? parent : ""; }
void platform_hide_window(void) { if (window) hide_fn(window); }
int platform_screen_count(void) { return screen_count; }
PortalRectangle platform_screen_bounds(int index) {
  return index >= 0 && index < screen_count ? screens[index] : (PortalRectangle){0};
}

int platform_read_image(const char *uri) {
  if (!uri || !g_str_has_prefix(uri, "file://"))
    return 0;
  char *path = g_filename_from_uri(uri, NULL, NULL);
  GError *error = NULL;
  void *next = path ? image_load_fn(path, &error) : NULL;
  g_free(path);
  g_clear_error(&error);
  if (!next)
    return 0;
  int width = image_width_fn(next), height = image_height_fn(next);
  int channels = image_channels_fn(next);
  if (width <= 0 || height <= 0 || width > 32000 || height > 32000 ||
      (long)width * height > 100000000 || (channels != 3 && channels != 4)) {
    g_object_unref(next);
    return 0;
  }
  if (image)
    g_object_unref(image);
  image = next;
  screen_count = 0;
  void *display = display_fn();
  int count = display ? monitor_count_fn(display) : 0;
  int left = 100000, top = 100000, right = -100000, bottom = -100000;
  for (int i = 0; i < count && i < 16; i++) {
    PortalRectangle rect;
    geometry_fn(monitor_fn(display, i), &rect);
    if (rect.width <= 0 || rect.height <= 0 || rect.width > 16000 || rect.height > 16000 ||
        labs((long)rect.x) > 100000 || labs((long)rect.y) > 100000)
      return 0;
    screens[screen_count++] = rect;
    if (rect.x < left) left = rect.x;
    if (rect.y < top) top = rect.y;
    if (rect.x + rect.width > right) right = rect.x + rect.width;
    if (rect.y + rect.height > bottom) bottom = rect.y + rect.height;
  }
  if (!screen_count) {
    screens[screen_count++] = (PortalRectangle){0, 0, width, height};
    atlas = screens[0];
  } else
    atlas = (PortalRectangle){left, top, right - left, bottom - top};
  long difference = (long)width * atlas.height - (long)height * atlas.width;
  return atlas.width > 0 && atlas.height > 0 &&
      labs(difference) <= 2L * (atlas.width > atlas.height ? atlas.width : atlas.height);
}

int platform_crop(PortalRectangle box, unsigned char **rgb, int *width, int *height) {
  if (!image || box.x < atlas.x || box.y < atlas.y || box.width <= 0 || box.height <= 0 ||
      (long)box.x + box.width > (long)atlas.x + atlas.width ||
      (long)box.y + box.height > (long)atlas.y + atlas.height)
    return 0;
  *width = box.width > 1280 ? 1280 : box.width;
  *height = box.height * *width / box.width;
  if (*height < 1) *height = 1;
  unsigned char *out = malloc((size_t)*width * (size_t)*height * 3);
  if (!out)
    return 0;
  int source_width = image_width_fn(image), source_height = image_height_fn(image);
  int channels = image_channels_fn(image), stride = image_stride_fn(image);
  unsigned char *pixels = image_pixels_fn(image);
  for (int y = 0; y < *height; y++)
    for (int x = 0; x < *width; x++) {
      int sx = (int)((box.x - atlas.x + (double)x * box.width / *width) * source_width / atlas.width);
      int sy = (int)((box.y - atlas.y + (double)y * box.height / *height) * source_height / atlas.height);
      memcpy(out + ((size_t)y * (size_t)*width + (size_t)x) * 3,
             pixels + (size_t)sy * (size_t)stride + (size_t)sx * (size_t)channels, 3);
    }
  *rgb = out;
  return 1;
}
