#include <config.h>

#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include <gst/gst.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "portal-test-app.h"
#include "portal-test-win.h"
#include "screenshot-portal.h"
#include "account-portal.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

struct _PortalTestWin
{
  GtkApplicationWindow parent;
  GtkWidget *sandbox_status;
  GtkWidget *network_status;
  GtkWidget *monitor_name;
  GtkWidget *resolver_name;
  GtkWidget *proxies;
  GtkWidget *encoding;
  GtkWidget *ack_image;

  char *window_handle;

  GNetworkMonitor *monitor;
  GProxyResolver *resolver;

  GtkWidget *image;
  XdpScreenshot *screenshot;
  char *screenshot_handle;
  guint screenshot_response_signal_id;

  GtkWidget *inhibit_idle;
  GtkWidget *inhibit_logout;
  GtkWidget *inhibit_suspend;
  GtkWidget *inhibit_switch;
  guint inhibit_cookie;
  GtkApplicationInhibitFlags inhibit_flags;

  XdpAccount *account;
  char *account_handle;
  guint account_response_signal_id;
  GtkWidget *username;
  GtkWidget *realname;
  GtkWidget *avatar;
};

struct _PortalTestWinClass
{
  GtkApplicationWindowClass parent_class;
};

G_DEFINE_TYPE (PortalTestWin, portal_test_win, GTK_TYPE_APPLICATION_WINDOW);

static void
update_network_status (PortalTestWin *win)
{
  g_autoptr(GString) s = g_string_new ("");
  GEnumClass *class;
  GEnumValue *value;

  if (g_network_monitor_get_network_available (win->monitor))
    g_string_append (s, "available");
  if (g_network_monitor_get_network_metered (win->monitor))
    {
      if (s->len > 0)
        g_string_append (s, ", ");
      g_string_append (s, "metered");
    }
  class = g_type_class_ref (G_TYPE_NETWORK_CONNECTIVITY);
  value = g_enum_get_value (class, g_network_monitor_get_connectivity (win->monitor));
  if (s->len > 0)
    g_string_append (s, ", ");
  g_string_append_printf (s, "connectivity=%s", value->value_nick);
  g_type_class_unref (class);

  gtk_label_set_label (GTK_LABEL (win->network_status), s->str);
}

static void
portal_test_win_init (PortalTestWin *win)
{
  const char *status;
  g_auto(GStrv) proxies = NULL;
  g_autofree char *proxy = NULL;
  g_autofree char *path = NULL;

  gtk_widget_init_template (GTK_WIDGET (win));

  path = g_build_filename (g_get_user_runtime_dir (), "flatpak-info", NULL);
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    status = "confined";
  else
    status = "unconfined";
  gtk_label_set_label (GTK_LABEL (win->sandbox_status), status);

  win->monitor = g_network_monitor_get_default ();
  gtk_label_set_label (GTK_LABEL (win->monitor_name), G_OBJECT_TYPE_NAME (win->monitor));
  g_signal_connect_swapped (win->monitor, "notify", G_CALLBACK (update_network_status), win);
  update_network_status (win);

  win->resolver = g_proxy_resolver_get_default ();
  gtk_label_set_label (GTK_LABEL (win->resolver_name), G_OBJECT_TYPE_NAME (win->resolver));

  proxies = g_proxy_resolver_lookup (win->resolver, "http://www.flatpak.org", NULL, NULL);
  proxy = g_strjoinv (", ", proxies);
  gtk_label_set_label (GTK_LABEL (win->proxies), proxy);

  win->screenshot = xdp_screenshot_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                           G_DBUS_PROXY_FLAGS_NONE,
                                                           "org.freedesktop.portal.Desktop",
                                                           "/org/freedesktop/portal/desktop",
                                                           NULL, NULL);
  win->account = xdp_account_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     "org.freedesktop.portal.Desktop",
                                                     "/org/freedesktop/portal/desktop",
                                                     NULL, NULL);
}

static void
open_local (GtkWidget *button, PortalTestWin *win)
{
  g_autoptr(GFile) file = NULL;
  g_autofree char *uri = NULL;

  file = g_file_new_for_path (PKGDATADIR "/test.txt");
  uri = g_file_get_uri (file);

  g_message ("Opening '%s'", uri);

  g_app_info_launch_default_for_uri (uri, NULL, NULL);
}

static void
save_dialog (GtkWidget *button, PortalTestWin *win)
{
  gint res;
  GtkFileChooserNative *dialog;
  GtkFileChooser *chooser;
  GtkWindow *parent;
  const char *options[] = {
    "current",
    "iso8859-15",
    "utf-16",
    NULL
  };
  const char *labels[] = {
    "Current Locale (UTF-8)",
    "Western (ISO-8859-15)",
    "Unicode (UTF-16)",
    NULL,
  };
  const char *encoding;
  const char *label;
  g_autofree char *text = NULL;
  const char *canonicalize;
  int i;

  parent = GTK_WINDOW (gtk_widget_get_toplevel (button));
  dialog = gtk_file_chooser_native_new ("File Chooser Portal",
                                        parent,
                                        GTK_FILE_CHOOSER_ACTION_SAVE,
                                        "_Save",
                                        "_Cancel");
  chooser = GTK_FILE_CHOOSER (dialog);
#if GTK_CHECK_VERSION(3, 21, 4)
  gtk_file_chooser_add_choice (chooser,
                               "encoding", "Character Encoding:",
                               options, labels);
  gtk_file_chooser_set_choice (chooser, "encoding", "current");

  gtk_file_chooser_add_choice (chooser, "canonicalize", "Canonicalize", NULL, NULL);
  gtk_file_chooser_set_choice (chooser, "canonicalize", "true");
#endif

  res = gtk_native_dialog_run (GTK_NATIVE_DIALOG (dialog));
  g_message ("Saving file / Response: %d", res);
  if (res == GTK_RESPONSE_ACCEPT)
    {
      char *filename;
      filename = gtk_file_chooser_get_filename (chooser);
      g_message ("Saving file: '%s'", filename);
      g_free (filename);
    }

#if GTK_CHECK_VERSION(3, 21, 4)
  encoding = gtk_file_chooser_get_choice (chooser, "encoding");
  canonicalize = gtk_file_chooser_get_choice (chooser, "canonicalize");
#endif

  label = "";
  for (i = 0; options[i]; i++)
    {
      if (g_strcmp0 (encoding, options[i]) == 0)
        label = labels[i];
    }

  text = g_strdup_printf ("%s%s", label, g_str_equal (canonicalize, "true") ? " (canon)" : "");
  gtk_label_set_label (GTK_LABEL (win->encoding), text);

  g_object_unref (dialog);
}

static void
screenshot_response (GDBusConnection *connection,
                     const char *sender_name,
                     const char *object_path,
                     const char *interface_name,
                     const char *signal_name,
                     GVariant *parameters,
                     gpointer user_data)
{
  PortalTestWin *win = user_data;
  guint32 response;
  GVariant *options;

  g_variant_get (parameters, "(u@a{sv})", &response, &options);

  if (response == 0)
    {
      g_autoptr(GdkPixbuf) pixbuf = NULL;
      g_autoptr(GError) error = NULL;
      const char *uri;
      g_autofree char *path = NULL;

      g_variant_lookup (options, "uri", "&s", &uri);
      path = g_filename_from_uri (uri, NULL, NULL);

      pixbuf = gdk_pixbuf_new_from_file_at_scale (path, 60, 40, TRUE, &error);
      if (error)
        g_warning ("Failed to load screenshot %s: %s", path, error->message);
      else
        gtk_image_set_from_pixbuf (GTK_IMAGE (win->image), pixbuf);
    }
  else
    g_message ("Screenshot canceled");

  if (win->screenshot_response_signal_id != 0)
    {
      g_dbus_connection_signal_unsubscribe (connection,
                                            win->screenshot_response_signal_id);
      win->screenshot_response_signal_id = 0;
    }
}

static void
screenshot_called (GObject *source,
                   GAsyncResult *result,
                   gpointer data)
{
  PortalTestWin *win = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *handle = NULL;

  if (!xdp_screenshot_call_screenshot_finish (win->screenshot, &handle, result, &error))
    {
      g_warning ("Screenshot error: %s", error->message);
      return;
    }

  win->screenshot_response_signal_id =
    g_dbus_connection_signal_subscribe (g_dbus_proxy_get_connection (G_DBUS_PROXY (win->screenshot)),
                                        "org.freedesktop.portal.Desktop",
                                        "org.freedesktop.portal.Request",
                                        "Response",
                                        handle,
                                        NULL,
                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                        screenshot_response,
                                        win, NULL);
}

static void
take_screenshot (GtkWidget *button, PortalTestWin *win)
{
  GVariantBuilder opt_builder;
  GVariant *options;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "modal", g_variant_new_boolean (TRUE));
  options = g_variant_builder_end (&opt_builder);

  xdp_screenshot_call_screenshot (win->screenshot,
                                  win->window_handle ? win->window_handle : "",
                                  options,
                                  NULL,
                                  screenshot_called,
                                  win);
}

static void
account_response (GDBusConnection *connection,
                  const char *sender_name,
                  const char *object_path,
                  const char *interface_name,
                  const char *signal_name,
                  GVariant *parameters,
                  gpointer user_data)
{
  PortalTestWin *win = user_data;
  guint32 response;
  GVariant *options;

  g_variant_get (parameters, "(u@a{sv})", &response, &options);

  if (response == 0)
    {
      g_autoptr(GdkPixbuf) pixbuf = NULL;
      g_autoptr(GError) error = NULL;
      const char *id;
      const char *name;
      const char *uri;
      g_autofree char *path = NULL;

      g_variant_lookup (options, "id", "&s", &id);
      g_variant_lookup (options, "name", "&s", &name);
      g_variant_lookup (options, "image", "&s", &uri);

      gtk_label_set_label (GTK_LABEL (win->username), id);
      gtk_label_set_label (GTK_LABEL (win->realname), name);

      if (uri && uri[0])
        {
          path = g_filename_from_uri (uri, NULL, NULL);
          pixbuf = gdk_pixbuf_new_from_file_at_scale (path, 60, 40, TRUE, &error);
          if (error)
            g_warning ("Failed to load photo %s: %s", path, error->message);
          else
            gtk_image_set_from_pixbuf (GTK_IMAGE (win->avatar), pixbuf);
       }
    }
  else
    g_message ("Account canceled");

  if (win->account_response_signal_id != 0)
    {
      g_dbus_connection_signal_unsubscribe (connection,
                                            win->account_response_signal_id);
      win->account_response_signal_id = 0;
    }
}

static void
get_user_information_called (GObject *source,
                             GAsyncResult *result,
                             gpointer data)
{
  PortalTestWin *win = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *handle = NULL;

  if (!xdp_account_call_get_user_information_finish (win->account, &handle, result, &error))
    {
      g_warning ("Account error: %s", error->message);
      return;
    }

  win->account_response_signal_id =
    g_dbus_connection_signal_subscribe (g_dbus_proxy_get_connection (G_DBUS_PROXY (win->account)),
                                        "org.freedesktop.portal.Desktop",
                                        "org.freedesktop.portal.Request",
                                        "Response",
                                        handle,
                                        NULL,
                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                        account_response,
                                        win, NULL);
}

static void
get_user_information (GtkWidget *button, PortalTestWin *win)
{
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "reason", g_variant_new_string ("Allows portal-test to test the Account portal."));
  xdp_account_call_get_user_information (win->account,
                                         win->window_handle ? win->window_handle : "",
                                         g_variant_builder_end (&options),
                                         NULL,
                                         get_user_information_called,
                                         win);
}

static void
notify_me (GtkButton *button, PortalTestWin *win)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (win));
  g_autoptr(GNotification) notification = NULL;

  gtk_widget_hide (win->ack_image);

  notification = g_notification_new ("Notify me");
  g_notification_set_body (notification, "Really important information would ordinarily appear here");
  g_notification_add_button (notification, "Yup", "app.ack");

  g_application_send_notification (G_APPLICATION (app), "notification", notification);
}

void
portal_test_win_ack (PortalTestWin *win)
{
  gtk_widget_show (win->ack_image);
}

static GList *active_prints = NULL;

typedef struct {
  char *text;
  PangoLayout *layout;
  GList *page_breaks;
  char *font;
} PrintData;

static void
status_changed_cb (GtkPrintOperation *op,
                   gpointer user_data)
{
  if (gtk_print_operation_is_finished (op))
    {
      active_prints = g_list_remove (active_prints, op);
      g_object_unref (op);
    }
}

static void
begin_print (GtkPrintOperation *operation,
             GtkPrintContext *context,
             PrintData *print_data)
{
  PangoFontDescription *desc;
  PangoLayoutLine *layout_line;
  double width, height;
  double page_height;
  GList *page_breaks;
  int num_lines;
  int line;

  width = gtk_print_context_get_width (context);
  height = gtk_print_context_get_height (context);

  print_data->layout = gtk_print_context_create_pango_layout (context);

  desc = pango_font_description_from_string (print_data->font);
  pango_layout_set_font_description (print_data->layout, desc);
  pango_font_description_free (desc);

  pango_layout_set_width (print_data->layout, width * PANGO_SCALE);
  pango_layout_set_text (print_data->layout, print_data->text, -1);

  num_lines = pango_layout_get_line_count (print_data->layout);

  page_breaks = NULL;
  page_height = 0;

  for (line = 0; line < num_lines; line++)
    {
      PangoRectangle ink_rect, logical_rect;
      double line_height;

      layout_line = pango_layout_get_line (print_data->layout, line);
      pango_layout_line_get_extents (layout_line, &ink_rect, &logical_rect);

      line_height = logical_rect.height / 1024.0;

      if (page_height + line_height > height)
        {
          page_breaks = g_list_prepend (page_breaks, GINT_TO_POINTER (line));
          page_height = 0;
        }

      page_height += line_height;
    }

  page_breaks = g_list_reverse (page_breaks);
  gtk_print_operation_set_n_pages (operation, g_list_length (page_breaks) + 1);

  print_data->page_breaks = page_breaks;
}

static void
draw_page (GtkPrintOperation *operation,
           GtkPrintContext *context,
           int page_nr,
           PrintData *print_data)
{
  cairo_t *cr;
  GList *pagebreak;
  int start, end, i;
  PangoLayoutIter *iter;
  double start_pos;

  if (page_nr == 0)
    start = 0;
  else
    {
      pagebreak = g_list_nth (print_data->page_breaks, page_nr - 1);
      start = GPOINTER_TO_INT (pagebreak->data);
    }

  pagebreak = g_list_nth (print_data->page_breaks, page_nr);
  if (pagebreak == NULL)
    end = pango_layout_get_line_count (print_data->layout);
  else
    end = GPOINTER_TO_INT (pagebreak->data);

  cr = gtk_print_context_get_cairo_context (context);

  cairo_set_source_rgb (cr, 0, 0, 0);

  i = 0;
  start_pos = 0;
  iter = pango_layout_get_iter (print_data->layout);
  do
    {
      PangoRectangle   logical_rect;
      PangoLayoutLine *line;
      int              baseline;

      if (i >= start)
        {
          line = pango_layout_iter_get_line (iter);

          pango_layout_iter_get_line_extents (iter, NULL, &logical_rect);
          baseline = pango_layout_iter_get_baseline (iter);

          if (i == start)
            start_pos = logical_rect.y / 1024.0;

          cairo_move_to (cr, logical_rect.x / 1024.0, baseline / 1024.0 - start_pos);
          pango_cairo_show_layout_line  (cr, line);
        }
      i++;
    }
  while (i < end &&
         pango_layout_iter_next_line (iter));

  pango_layout_iter_free (iter);
}

static void
end_print (GtkPrintOperation *op,
           GtkPrintContext *context,
           PrintData *print_data)
{
  g_list_free (print_data->page_breaks);
  print_data->page_breaks = NULL;
  g_object_unref (print_data->layout);
  print_data->layout = NULL;
}

static void
print_done (GtkPrintOperation *op,
            GtkPrintOperationResult res,
            PrintData *print_data)
{
  GError *error = NULL;

  if (res == GTK_PRINT_OPERATION_RESULT_ERROR)
    {

      GtkWidget *error_dialog;

      gtk_print_operation_get_error (op, &error);

      error_dialog = gtk_message_dialog_new (NULL,
                                             GTK_DIALOG_DESTROY_WITH_PARENT,
                                             GTK_MESSAGE_ERROR,
                                             GTK_BUTTONS_CLOSE,
                                             "Error printing file:\n%s",
                                             error ? error->message : "no details");
      g_signal_connect (error_dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);
      gtk_widget_show (error_dialog);
    }
  else

  g_free (print_data->text);
  g_free (print_data->font);
  g_free (print_data);
  if (!gtk_print_operation_is_finished (op))
    {
      g_object_ref (op);
      active_prints = g_list_append (active_prints, op);

      /* This ref is unref:ed when we get the final state change */
      g_signal_connect (op, "status-changed",
                        G_CALLBACK (status_changed_cb), NULL);
    }
}

static char *
get_text (void)
{
  char *text;
  g_autoptr(GError) error = NULL;

  if (!g_file_get_contents ("portal-test-win.c", &text, NULL, &error))
    {
      g_warning ("Failed to load print text: %s", error->message);
      text = g_strdup (error->message);
    }

  return text;
}

static void
print_cb (GtkButton *button, PortalTestWin *win)
{
  GtkPrintOperation *print;
  PrintData *print_data;

  print_data = g_new0 (PrintData, 1);

  print_data->text = get_text ();
  print_data->font = g_strdup ("Sans 12");

  print = gtk_print_operation_new ();

  g_signal_connect (print, "begin-print", G_CALLBACK (begin_print), print_data);
  g_signal_connect (print, "end-print", G_CALLBACK (end_print), print_data);
  g_signal_connect (print, "draw-page", G_CALLBACK (draw_page), print_data);
  g_signal_connect (print, "done", G_CALLBACK (print_done), print_data);

  //gtk_print_operation_set_allow_async (print, TRUE);
  gtk_print_operation_run (print, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                           GTK_WINDOW (win), NULL);

  g_object_unref (print);
}

static void
inhibit_changed (GtkToggleButton *button, PortalTestWin *win)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (win));
  GtkApplicationInhibitFlags flags = 0;

  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (win->inhibit_logout)))
    flags |= GTK_APPLICATION_INHIBIT_LOGOUT;
  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (win->inhibit_switch)))
    flags |= GTK_APPLICATION_INHIBIT_SWITCH;
  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (win->inhibit_suspend)))
    flags |= GTK_APPLICATION_INHIBIT_SUSPEND;
  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (win->inhibit_idle)))
    flags |= GTK_APPLICATION_INHIBIT_IDLE;

  if (win->inhibit_flags == flags)
    return;

  if (win->inhibit_cookie != 0)
    {
      gtk_application_uninhibit (app, win->inhibit_cookie);
      win->inhibit_cookie = 0;
    }

  win->inhibit_flags = flags;

  if (win->inhibit_flags != 0)
    {
      win->inhibit_cookie = gtk_application_inhibit (app,
                                                     GTK_WINDOW (win),
                                                     win->inhibit_flags,
                                                     "Portal Testing");
    }
}

static gboolean
pipeline_stop (GstElement *pipeline)
{
  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_object_unref (pipeline);

  return FALSE;
}
static void
play_sound (gdouble frequency)
{
  GstElement *source, *sink;
  GstElement *pipeline;

  pipeline = gst_pipeline_new ("note");
  source = gst_element_factory_make ("audiotestsrc", "source");
  sink = gst_element_factory_make ("autoaudiosink", "output");

  g_object_set (source, "freq", frequency, NULL);

  gst_bin_add_many (GST_BIN (pipeline), source, sink, NULL);
  gst_element_link (source, sink);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  g_timeout_add (500, (GSourceFunc) pipeline_stop, pipeline);
}

static void
play_clicked (GtkButton *button, PortalTestWin *win)
{
  play_sound (440.0);
}

static void
handle_obtained (GdkWindow *window,
                 const char *handle,
                 gpointer user_data)
{
  PortalTestWin *win = user_data;

  win->window_handle = g_strdup_printf ("wayland:%s", handle);
}

static gboolean
obtain_handle (gpointer data)
{
  PortalTestWin *win = PORTAL_TEST_WIN (data);
  GdkWindow *window;

  window = gtk_widget_get_window (GTK_WIDGET (win));

  if (GDK_IS_WAYLAND_WINDOW (window))
    gdk_wayland_window_export_handle (window, handle_obtained, win, NULL);
  else if (GDK_IS_X11_WINDOW (window))
    win->window_handle = g_strdup_printf ("x11:%x", (guint32)gdk_x11_window_get_xid (window));

  return G_SOURCE_REMOVE;
}

static void
test_win_realize (GtkWidget *widget)
{
  PortalTestWin *win = PORTAL_TEST_WIN (widget);

  GTK_WIDGET_CLASS (portal_test_win_parent_class)->realize (widget);

  g_idle_add (obtain_handle, win);
}

static void
portal_test_win_class_init (PortalTestWinClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

  widget_class->realize = test_win_realize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gtk/portal-test/portal-test-win.ui");

  gtk_widget_class_bind_template_callback (widget_class, save_dialog);
  gtk_widget_class_bind_template_callback (widget_class, open_local);
  gtk_widget_class_bind_template_callback (widget_class, take_screenshot);
  gtk_widget_class_bind_template_callback (widget_class, notify_me);
  gtk_widget_class_bind_template_callback (widget_class, print_cb);
  gtk_widget_class_bind_template_callback (widget_class, inhibit_changed);
  gtk_widget_class_bind_template_callback (widget_class, play_clicked);
  gtk_widget_class_bind_template_callback (widget_class, get_user_information);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, sandbox_status);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, network_status);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, monitor_name);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, proxies);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, resolver_name);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, image);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, encoding);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, ack_image);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, inhibit_idle);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, inhibit_logout);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, inhibit_suspend);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, inhibit_switch);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, username);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, realname);
  gtk_widget_class_bind_template_child (widget_class, PortalTestWin, avatar);
}

GtkApplicationWindow *
portal_test_win_new (PortalTestApp *app)
{
  return g_object_new (portal_test_win_get_type (), "application", app, NULL);
}
