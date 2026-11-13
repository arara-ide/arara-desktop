#include <gtk/gtk.h>
#include <stdio.h>
#include <time.h>
static GtkWidget *field;
static GtkWidget *result;
static void record(const char *kind) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  printf("%s:%.6f\n", kind, now.tv_sec + now.tv_nsec / 1000000000.0);
  fflush(stdout);
}
static void apply(GtkButton *button, gpointer data) {
  (void)button;
  (void)data;
  char value[200];
  snprintf(value, sizeof(value), "Applied: %s",
           gtk_entry_get_text(GTK_ENTRY(field)));
  gtk_label_set_text(GTK_LABEL(result), value);
}
static gboolean mouse(GtkWidget *widget, GdkEventButton *event, gpointer data) {
  (void)data;
  gtk_widget_grab_focus(widget);
  record(event->type == GDK_BUTTON_PRESS ? "mouse_down" : "mouse_up");
  return TRUE;
}
static gboolean key(GtkWidget *widget, GdkEventKey *event, gpointer data) {
  (void)widget;
  (void)data;
  record(event->type == GDK_KEY_PRESS ? "key_down" : "key_up");
  return TRUE;
}
int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  gdk_set_program_class("AraraDesktopFixture");
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Arara Desktop Fixture");
  gtk_window_set_default_size(GTK_WINDOW(window), 420, 420);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_add(GTK_CONTAINER(window), box);
  field = gtk_entry_new();
  atk_object_set_name(gtk_widget_get_accessible(field), "Fixture input");
  GtkWidget *button = gtk_button_new_with_label("Apply fixture");
  GtkWidget *password = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(password), FALSE);
  gtk_entry_set_text(GTK_ENTRY(password), "fixture-secret");
  result = gtk_label_new("Waiting");
  GtkWidget *canvas = gtk_drawing_area_new();
  gtk_widget_set_can_focus(canvas, TRUE);
  gtk_widget_add_events(canvas, GDK_BUTTON_PRESS_MASK |
                                    GDK_BUTTON_RELEASE_MASK |
                                    GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
  for (int n = 0; n < 5; n++) {
    GtkWidget *child = n == 0   ? field
                       : n == 1 ? button
                       : n == 2 ? password
                       : n == 3 ? result
                                : canvas;
    gtk_box_pack_start(GTK_BOX(box), child, n == 4, n == 4, 0);
  }
  g_signal_connect(button, "clicked", G_CALLBACK(apply), NULL);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  g_signal_connect(canvas, "button-press-event", G_CALLBACK(mouse), NULL);
  g_signal_connect(canvas, "button-release-event", G_CALLBACK(mouse), NULL);
  g_signal_connect(canvas, "key-press-event", G_CALLBACK(key), NULL);
  g_signal_connect(canvas, "key-release-event", G_CALLBACK(key), NULL);
  gtk_widget_show_all(window);
  gtk_main();
  return 0;
}
