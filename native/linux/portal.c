#define _POSIX_C_SOURCE 200809L
#include "portal.h"
#include "platform.h"
#include "eis.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static const char *destination = "org.freedesktop.portal.Desktop";
static const char *object = "/org/freedesktop/portal/desktop";
static const char *remote = "org.freedesktop.portal.RemoteDesktop";
static GDBusConnection *bus;
static char *session, *request_path;
static guint devices, response_code, subscription, closed_subscription;
static unsigned long sequence;
static int available, revoked, capture_allowed, complete, eis_started;
static GVariant *response_data;
static const char *failure;
typedef enum { NONE, SETUP_SHOT, CREATE, SELECT, START, CAPTURE } Stage;
static Stage stage;

static GVariant *options(void) {
  return g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
}

static GVariant *call(const char *path, const char *interface,
                      const char *method, GVariant *parameters) {
  GError *error = NULL;
  if (!bus) {
    if (parameters) g_variant_unref(g_variant_ref_sink(parameters));
    return NULL;
  }
  GVariant *result = g_dbus_connection_call_sync(bus, destination, path, interface,
      method, parameters, NULL, G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
  g_clear_error(&error);
  return result;
}

static void closed(GDBusConnection *connection, const gchar *sender,
                    const gchar *path, const gchar *interface,
                    const gchar *signal, GVariant *parameters, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface;
  (void)signal; (void)parameters; (void)data;
  devices = 0;
  revoked = 1;
}

static void response(GDBusConnection *connection, const gchar *sender,
                      const gchar *path, const gchar *interface,
                      const gchar *signal, GVariant *parameters, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface;
  (void)signal; (void)data;
  if (!complete) {
    g_variant_get(parameters, "(u@a{sv})", &response_code, &response_data);
    complete = 1;
  }
}

static void clear_request(int cancel) {
  if (subscription) g_dbus_connection_signal_unsubscribe(bus, subscription);
  subscription = 0;
  if (request_path && cancel) {
    GVariant *result = call(request_path, "org.freedesktop.portal.Request", "Close", NULL);
    if (result) g_variant_unref(result);
  }
  g_clear_pointer(&request_path, g_free);
  g_clear_pointer(&response_data, g_variant_unref);
  complete = 0;
  stage = NONE;
}

static int begin(Stage next) {
  char token[80];
  snprintf(token, sizeof(token), "arara_%ld_%lu", (long)getpid(), ++sequence);
  const char *unique = g_dbus_connection_get_unique_name(bus);
  char *sender = g_strdup(unique + 1);
  for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
  request_path = g_strdup_printf("%s/request/%s/%s", object, sender, token);
  g_free(sender);
  complete = 0;
  response_code = 0;
  stage = next;
  subscription = g_dbus_connection_signal_subscribe(bus, destination,
      "org.freedesktop.portal.Request", "Response", request_path, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, response, NULL, NULL);
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(token));
  GVariant *parameters;
  const char *interface = remote, *method;
  if (next == SETUP_SHOT || next == CAPTURE) {
    interface = "org.freedesktop.portal.Screenshot";
    method = "Screenshot";
    g_variant_builder_add(&builder, "{sv}", "interactive", g_variant_new_boolean(FALSE));
    parameters = g_variant_new("(s@a{sv})", next == SETUP_SHOT ? platform_parent() : "",
                               g_variant_builder_end(&builder));
  } else if (next == CREATE) {
    method = "CreateSession";
    g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(token));
    parameters = g_variant_new("(@a{sv})", g_variant_builder_end(&builder));
  } else if (next == SELECT) {
    method = "SelectDevices";
    g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(3));
    parameters = g_variant_new("(o@a{sv})", session, g_variant_builder_end(&builder));
  } else {
    method = "Start";
    parameters = g_variant_new("(os@a{sv})", session, platform_parent(),
                               g_variant_builder_end(&builder));
  }
  GVariant *result = call(object, interface, method, parameters);
  const char *returned = NULL;
  if (result) g_variant_get(result, "(&o)", &returned);
  int valid = returned && !strcmp(returned, request_path);
  if (result) g_variant_unref(result);
  if (!valid) {
    clear_request(1);
    failure = "portal_unavailable";
  }
  return valid;
}

static void advance(void) {
  if (!complete || stage == NONE) return;
  Stage previous = stage;
  GVariant *data = response_data;
  response_data = NULL;
  guint code = response_code;
  clear_request(0);
  if (code != 0 || revoked) {
    if (data) g_variant_unref(data);
    failure = code == 1 || revoked ? "portal_denied" : "portal_unavailable";
    portal_close();
    revoked = 1;
    return;
  }
  if (previous == SETUP_SHOT || previous == CAPTURE) {
    const char *uri = NULL;
    capture_allowed = g_variant_lookup(data, "uri", "&s", &uri) && platform_read_image(uri);
    if (!capture_allowed) failure = "portal_unavailable";
    else if (previous == SETUP_SHOT) begin(CREATE);
  } else if (previous == CREATE) {
    const char *path = NULL;
    if (g_variant_lookup(data, "session_handle", "&s", &path) && g_variant_is_object_path(path)) {
      session = g_strdup(path);
      closed_subscription = g_dbus_connection_signal_subscribe(bus, destination,
          "org.freedesktop.portal.Session", "Closed", session, NULL,
          G_DBUS_SIGNAL_FLAGS_NONE, closed, NULL, NULL);
      begin(SELECT);
    } else failure = "portal_unavailable";
  } else if (previous == SELECT) begin(START);
  else if (previous == START) {
    g_variant_lookup(data, "devices", "u", &devices);
    if ((devices & 3) != 3) failure = "portal_denied";
    else platform_hide_window();
  }
  g_variant_unref(data);
  if (failure) {
    portal_close();
    revoked = 1;
  }
}

void portal_poll(void) {
  for (int i = 0; i < 100; i++) {
    if (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    else if (!complete) break;
    advance();
  }
  eis_poll();
  if (bus && g_dbus_connection_is_closed(bus)) revoked = 1;
  if (stage != NONE && platform_setup_cancelled()) {
    failure = "portal_denied";
    portal_close();
    revoked = 1;
  }
}

void portal_init(void) {
  if (!getenv("WAYLAND_DISPLAY")) return;
  GError *error = NULL;
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  g_clear_error(&error);
  if (!bus || !platform_init()) return;
  GVariant *result = call(object, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", remote, "AvailableDeviceTypes"));
  if (result) {
    GVariant *value;
    g_variant_get(result, "(v)", &value);
    available = g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) &&
                (g_variant_get_uint32(value) & 3) == 3;
    g_variant_unref(value);
    g_variant_unref(result);
  }
}

int portal_available(void) { return available; }
int portal_pending(void) { return stage != NONE && stage != CAPTURE; }
int portal_input_ready(void) { return session && !revoked && (devices & 3) == 3; }
int portal_capture_ready(void) { return capture_allowed && !revoked; }
int portal_source_count(void) { return portal_capture_ready() ? platform_screen_count() : 0; }
PortalRectangle portal_source_bounds(int index) { return platform_screen_bounds(index); }

const char *portal_setup(int (*interrupted)(void)) {
  portal_poll();
  if (failure) return failure;
  if (portal_input_ready() && portal_capture_ready()) return NULL;
  if (portal_pending()) return "portal_pending";
  if (!available) return "portal_unavailable";
  if (revoked) return "portal_denied";
  if (interrupted()) return "cancelled";
  failure = NULL;
  if (!platform_setup_window() || !begin(SETUP_SHOT)) return "portal_unavailable";
  portal_poll();
  return failure ? failure : portal_pending() ? "portal_pending" : NULL;
}

void portal_close(void) {
  clear_request(1);
  devices = 0;
  capture_allowed = 0;
  eis_close();
  eis_started = 0;
  platform_hide_window();
  if (closed_subscription) g_dbus_connection_signal_unsubscribe(bus, closed_subscription);
  closed_subscription = 0;
  if (session) {
    GVariant *result = call(session, "org.freedesktop.portal.Session", "Close", NULL);
    if (result) g_variant_unref(result);
    g_clear_pointer(&session, g_free);
  }
}

static const char *notify(const char *method, GVariant *parameters) {
  portal_poll();
  if (!portal_input_ready()) {
    g_variant_unref(g_variant_ref_sink(parameters));
    return revoked ? "portal_denied" : "portal_required";
  }
  GVariant *result = call(object, remote, method, parameters);
  if (!result) return "portal_denied";
  g_variant_unref(result);
  return NULL;
}

static int connect_eis(void) {
  if (eis_started) return 1;
  GUnixFDList *descriptors = NULL;
  GError *error = NULL;
  GVariant *result = g_dbus_connection_call_with_unix_fd_list_sync(bus, destination,
      object, remote, "ConnectToEIS", g_variant_new("(o@a{sv})", session, options()),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &descriptors, NULL, &error);
  g_clear_error(&error);
  if (!result) return 0;
  gint index;
  g_variant_get(result, "(h)", &index);
  int fd = descriptors ? g_unix_fd_list_get(descriptors, index, NULL) : -1;
  g_variant_unref(result);
  g_clear_object(&descriptors);
  eis_started = fd >= 0 && eis_connect(fd);
  return eis_started;
}

const char *portal_motion(double x, double y, int relative) {
  if (relative) return notify("NotifyPointerMotion", g_variant_new("(o@a{sv}dd)",
      session ? session : "/", options(), x, y));
  if (!portal_input_ready() || !portal_contains((PortalRectangle){(int)x, (int)y, 1, 1}))
    return "portal_required";
  return connect_eis() && eis_motion(x, y) ? NULL : "portal_unavailable";
}

const char *portal_button(int button, int pressed) {
  if (eis_started) return eis_button(button, pressed) ? NULL : "portal_denied";
  return notify("NotifyPointerButton", g_variant_new("(o@a{sv}iu)",
      session ? session : "/", options(), button, (guint)pressed));
}

const char *portal_key(uint32_t symbol, int pressed) {
  return notify("NotifyKeyboardKeysym", g_variant_new("(o@a{sv}iu)",
      session ? session : "/", options(), (gint)symbol, (guint)pressed));
}

const char *portal_scroll(double dx, double dy, int finish) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "finish", g_variant_new_boolean(finish));
  return notify("NotifyPointerAxis", g_variant_new("(o@a{sv}dd)",
      session ? session : "/", g_variant_builder_end(&builder), dx, dy));
}

int portal_contains(PortalRectangle box) {
  for (int i = 0; i < portal_source_count(); i++) {
    PortalRectangle rect = portal_source_bounds(i);
    if (box.width > 0 && box.height > 0 && box.x >= rect.x && box.y >= rect.y &&
        (long)box.x + box.width <= (long)rect.x + rect.width &&
        (long)box.y + box.height <= (long)rect.y + rect.height) return 1;
  }
  return 0;
}

const char *portal_capture(PortalRectangle box, unsigned char **rgb, int *width,
                           int *height, int (*interrupted)(void)) {
  portal_poll();
  if (!portal_capture_ready() || portal_pending()) return "portal_required";
  failure = NULL;
  if (!begin(CAPTURE)) return "portal_unavailable";
  gint64 deadline = g_get_monotonic_time() + 4 * G_TIME_SPAN_SECOND;
  while (stage == CAPTURE && !interrupted() && g_get_monotonic_time() < deadline) {
    portal_poll();
    g_usleep(1000);
  }
  if (stage == CAPTURE) clear_request(1);
  if (interrupted()) return "cancelled";
  if (failure) return failure;
  return platform_crop(box, rgb, width, height) ? NULL : "portal_unavailable";
}
