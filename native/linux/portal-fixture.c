#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *root_path = "/org/freedesktop/portal/desktop";
static const char *mode;
static GMainLoop *loop;
static GDBusNodeInfo *description;
static const char *xml =
  "<node>"
  "<interface name='org.freedesktop.portal.RemoteDesktop'>"
  "<property name='AvailableDeviceTypes' type='u' access='read'/>"
  "<method name='CreateSession'><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
  "<method name='SelectDevices'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
  "<method name='Start'><arg type='o' direction='in'/><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
  "</interface>"
  "<interface name='org.freedesktop.portal.Screenshot'>"
  "<method name='Screenshot'><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
  "</interface>"
  "<interface name='org.freedesktop.portal.Session'><method name='Close'/><signal name='Closed'/></interface>"
  "<interface name='org.freedesktop.portal.Request'><method name='Close'/><signal name='Response'><arg type='u'/><arg type='a{sv}'/></signal></interface>"
  "</node>";

static GVariant *properties(GDBusConnection *connection, const gchar *sender,
                             const gchar *path, const gchar *interface,
                             const gchar *property, GError **error, gpointer data) {
  (void)connection;
  (void)sender;
  (void)path;
  (void)interface;
  (void)property;
  (void)error;
  (void)data;
  return g_variant_new_uint32(3);
}

static void method(GDBusConnection *connection, const gchar *sender,
                    const gchar *path, const gchar *interface,
                    const gchar *name, GVariant *parameters,
                    GDBusMethodInvocation *invocation, gpointer data);
static const GDBusInterfaceVTable methods = {
  .method_call = method, .get_property = properties
};
typedef struct {
  GDBusConnection *connection;
  char *sender;
  char *path;
  GVariant *parameters;
} DelayedResponse;

static gboolean delayed_response(gpointer data) {
  DelayedResponse *reply = data;
  g_dbus_connection_emit_signal(reply->connection, reply->sender, reply->path,
      "org.freedesktop.portal.Request", "Response", reply->parameters, NULL);
  g_object_unref(reply->connection);
  g_variant_unref(reply->parameters);
  g_free(reply->sender);
  g_free(reply->path);
  g_free(reply);
  return G_SOURCE_REMOVE;
}

static void register_interface(GDBusConnection *connection, const char *path,
                                int index) {
  GError *error = NULL;
  g_dbus_connection_register_object(connection, path,
      description->interfaces[index], &methods, NULL, NULL, &error);
  if (error)
    g_error("Fixture registration failed: %s", error->message);
}

static void method(GDBusConnection *connection, const gchar *sender,
                    const gchar *path, const gchar *interface,
                    const gchar *name, GVariant *parameters,
                    GDBusMethodInvocation *invocation, gpointer data) {
  (void)interface;
  (void)data;
  printf("METHOD %s\n", name);
  fflush(stdout);
  if (!strcmp(name, "Close")) {
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  if (!strcmp(name, "OpenPipeWireRemote")) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.portal.Error.Failed", "Isolated fixture has no capture device");
    return;
  }
  GVariant *options = g_variant_get_child_value(parameters,
      g_variant_n_children(parameters) - 1);
  const char *token = NULL;
  g_variant_lookup(options, "handle_token", "&s", &token);
  if (!strcmp(name, "Screenshot")) {
    gboolean interactive = TRUE;
    g_variant_lookup(options, "interactive", "b", &interactive);
    printf("SCREENSHOT interactive=%d\n", interactive);
    fflush(stdout);
  }
  char *owner = g_strdup(sender + 1);
  for (char *p = owner; *p; p++)
    if (*p == '.')
      *p = '_';
  char *request = g_strdup_printf("%s/request/%s/%s", root_path, owner, token);
  register_interface(connection, request, 3);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request));
  GVariantBuilder results;
  g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
  guint response = 0;
  if (!strcmp(name, "CreateSession")) {
    char *session = g_strdup_printf("%s/session/%s/fixture", root_path, owner);
    register_interface(connection, session, 2);
    g_variant_builder_add(&results, "{sv}", "session_handle",
                          g_variant_new_string(session));
    g_free(session);
  } else if (!strcmp(name, "Start")) {
    response = !strcmp(mode, "denied") ? 1 : 0;
    g_variant_builder_add(&results, "{sv}", "devices",
                          g_variant_new_uint32(!strcmp(mode, "partial") ? 1 : 3));
  } else if (!strcmp(name, "Screenshot")) {
    if (strcmp(mode, "empty")) {
      char *uri = !strcmp(mode, "invalid") ? g_strdup("https://invalid.example/image.png")
          : g_filename_to_uri(getenv("PORTAL_FIXTURE_IMAGE"), NULL, NULL);
      g_variant_builder_add(&results, "{sv}", "uri", g_variant_new_string(uri));
      g_free(uri);
    }
  }
  if (!strcmp(mode, "pending") && !strcmp(name, "Start"))
    g_variant_builder_clear(&results);
  else if (!strcmp(mode, "delayed") && !strcmp(name, "Start")) {
    DelayedResponse *reply = g_new0(DelayedResponse, 1);
    reply->connection = g_object_ref(connection);
    reply->sender = g_strdup(sender);
    reply->path = g_strdup(request);
    reply->parameters = g_variant_ref_sink(
        g_variant_new("(u@a{sv})", response, g_variant_builder_end(&results)));
    g_timeout_add(1600, delayed_response, reply);
  }
  else
    g_dbus_connection_emit_signal(connection, sender, request,
        "org.freedesktop.portal.Request", "Response",
        g_variant_new("(u@a{sv})", response, g_variant_builder_end(&results)), NULL);
  g_free(request);
  g_free(owner);
  g_variant_unref(options);
  (void)path;
}

static void acquired(GDBusConnection *connection, const gchar *name, gpointer data) {
  (void)name;
  (void)data;
  register_interface(connection, root_path, 0);
  register_interface(connection, root_path, 1);
}

static void ready(GDBusConnection *connection, const gchar *name, gpointer data) {
  (void)connection;
  (void)name;
  (void)data;
  puts("READY");
  fflush(stdout);
}

int main(int argc, char **argv) {
  mode = argc > 1 ? argv[1] : "ready";
  description = g_dbus_node_info_new_for_xml(xml, NULL);
  loop = g_main_loop_new(NULL, FALSE);
  g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.portal.Desktop",
      G_BUS_NAME_OWNER_FLAGS_NONE, acquired, ready, NULL, NULL, NULL);
  g_main_loop_run(loop);
  return 0;
}
