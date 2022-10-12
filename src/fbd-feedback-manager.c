/*
 * Copyright (C) 2020 Purism SPC
 * SPDX-License-Identifier: GPL-3.0+
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "fbd-feedback-manager"

#include "lfb-names.h"
#include "fbd.h"
#include "fbd-dev-vibra.h"
#include "fbd-dev-leds.h"
#include "fbd-event.h"
#include "fbd-feedback-vibra.h"
#include "fbd-feedback-manager.h"
#include "fbd-feedback-theme.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <gudev/gudev.h>

#define FEEDBACKD_SCHEMA_ID "org.sigxcpu.feedbackd"
#define FEEDBACKD_KEY_PROFILE "profile"
#define FEEDBACKD_THEME_VAR "FEEDBACK_THEME"

#define APP_SCHEMA FEEDBACKD_SCHEMA_ID ".application"
#define APP_PREFIX "/org/sigxcpu/feedbackd/application/"

#define DEVICE_TREE_PATH "/sys/firmware/devicetree/base/compatible"
#define DEVICE_NAME_MAX 1024


/**
 * SECTION:fbd-feedback-manager
 * @short_description: The manager processing incoming events
 * @Title: FbdFeedbackManager
 *
 * The #FbdFeedbackManager listens for DBus messages and triggers feedbacks
 * based on the incoming events.
 */

typedef struct _FbdFeedbackManager {
  LfbGdbusFeedbackSkeleton parent;

  GSettings               *settings;
  FbdFeedbackProfileLevel  level;
  FbdFeedbackTheme        *theme;
  guint                    next_id;

  /* Key: event id, value: event */
  GHashTable              *events;
  /* Key: DBus name, value: watch_id */
  GHashTable              *clients;

  /* Hardware interaction */
  GUdevClient             *client;
  FbdDevVibra             *vibra;
  FbdDevSound             *sound;
  FbdDevLeds              *leds;
} FbdFeedbackManager;

static void fbd_feedback_manager_feedback_iface_init (LfbGdbusFeedbackIface *iface);

G_DEFINE_TYPE_WITH_CODE (FbdFeedbackManager,
                         fbd_feedback_manager,
                         LFB_GDBUS_TYPE_FEEDBACK_SKELETON,
                         G_IMPLEMENT_INTERFACE (
                           LFB_GDBUS_TYPE_FEEDBACK,
                           fbd_feedback_manager_feedback_iface_init));

static void
device_changes (FbdFeedbackManager *self, gchar *action, GUdevDevice *device,
                GUdevClient        *client)
{
  g_debug ("Device changes: action = %s, device = %s",
           action, g_udev_device_get_sysfs_path (device));

  if (g_strcmp0 (action, "remove") == 0 && self->vibra) {
    GUdevDevice *dev = fbd_dev_vibra_get_device (self->vibra);

    if (g_strcmp0 (g_udev_device_get_sysfs_path (dev),
                   g_udev_device_get_sysfs_path (device)) == 0) {
      g_debug ("Vibra device %s got removed", g_udev_device_get_sysfs_path (dev));
      g_clear_object (&self->vibra);
    }
  } else if (g_strcmp0 (action, "add") == 0) {
    if (!g_strcmp0 (g_udev_device_get_property (device, FEEDBACKD_UDEV_ATTR), "vibra")) {
      g_autoptr (GError) err = NULL;

      g_debug ("Found hotplugged vibra device at %s", g_udev_device_get_sysfs_path (device));
      g_clear_object (&self->vibra);
      self->vibra = fbd_dev_vibra_new (device, &err);
      if (!self->vibra)
        g_warning ("Failed to init vibra device: %s", err->message);
    }
  }
}

static gchar *
munge_app_id (const gchar *app_id)
{
  gchar *id = g_strdup (app_id);
  gint i;

  g_strcanon (id,
              "0123456789"
              "abcdefghijklmnopqrstuvwxyz"
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
              "-",
              '-');
  for (i = 0; id[i] != '\0'; i++)
    id[i] = g_ascii_tolower (id[i]);

  return id;
}

static FbdFeedbackProfileLevel
app_get_feedback_level (const gchar *app_id)
{
  g_autofree gchar *profile = NULL;
  g_autofree gchar *munged_app_id = munge_app_id (app_id);
  g_autofree gchar *path = g_strconcat (APP_PREFIX, munged_app_id, "/", NULL);
  g_autoptr (GSettings) setting =  g_settings_new_with_path (APP_SCHEMA, path);

  profile = g_settings_get_string (setting, FEEDBACKD_KEY_PROFILE);
  g_debug ("%s uses app profile %s", app_id, profile);
  return fbd_feedback_profile_level (profile);
}

static void
init_devices (FbdFeedbackManager *self)
{
  GList *l;
  g_autolist (GUdevClient) devices = NULL;
  g_autoptr(GError) err = NULL;

  devices = g_udev_client_query_by_subsystem (self->client, "input");

  for (l = devices; l != NULL; l = l->next) {
    GUdevDevice *dev = l->data;

    if (!g_strcmp0 (g_udev_device_get_property (dev, FEEDBACKD_UDEV_ATTR), "vibra")) {
      g_debug ("Found vibra device");
      self->vibra = fbd_dev_vibra_new (dev, &err);
      if (!self->vibra) {
        g_warning ("Failed to init vibra device: %s", err->message);
        g_clear_error (&err);
      }
    }
  }
  if (!self->vibra)
    g_debug ("No vibra capable device found");

  self->leds = fbd_dev_leds_new (&err);
  if (!self->leds) {
    g_debug ("Failed to init leds device: %s", err->message);
    g_clear_error (&err);
  }

  self->sound = fbd_dev_sound_new (&err);
  if (!self->sound) {
    g_warning ("Failed to init sound device: %s", err->message);
    g_clear_error (&err);
  }
}

static void
on_event_feedbacks_ended (FbdFeedbackManager *self, FbdEvent *event)
{
  guint event_id;

  g_return_if_fail (FBD_IS_FEEDBACK_MANAGER (self));
  g_return_if_fail (FBD_IS_EVENT (event));

  event_id = fbd_event_get_id (event);
  event = g_hash_table_lookup (self->events, GUINT_TO_POINTER (event_id));
  if (!event) {
    g_warning ("Feedback ended for unknown event %d", event_id);
    return;
  }

  g_return_if_fail (fbd_event_get_feedbacks_ended (event));

  lfb_gdbus_feedback_emit_feedback_ended (LFB_GDBUS_FEEDBACK (self), event_id,
                                          fbd_event_get_end_reason (event));

  g_debug ("All feedbacks for event %d finished", event_id);
  g_hash_table_remove (self->events, GUINT_TO_POINTER (event_id));
}

static void
on_profile_changed (FbdFeedbackManager *self, GParamSpec *psepc, gpointer unused)
{
  const gchar *pname;

  g_return_if_fail (FBD_IS_FEEDBACK_MANAGER (self));

  pname = lfb_gdbus_feedback_get_profile (LFB_GDBUS_FEEDBACK (self));

  if (!fbd_feedback_manager_set_profile (self, pname))
    g_warning ("Invalid profile '%s'", pname);

  /* TODO: end running feedbacks that aren't allowed in new profile immediately */
}

static void
on_feedbackd_setting_changed (FbdFeedbackManager *self,
                              const gchar        *key,
                              GSettings          *settings)
{
  g_autofree gchar *profile = NULL;

  g_return_if_fail (FBD_IS_FEEDBACK_MANAGER (self));
  g_return_if_fail (G_IS_SETTINGS (settings));
  g_return_if_fail (!g_strcmp0 (key, FEEDBACKD_KEY_PROFILE));

  profile = g_settings_get_string (settings, key);
  fbd_feedback_manager_set_profile (self, profile);
}

static void
on_client_vanished (GDBusConnection *connection,
		    const gchar     *name,
		    gpointer         user_data)
{
  FbdFeedbackManager *self = FBD_FEEDBACK_MANAGER (user_data);
  GHashTableIter iter;
  gpointer key, value;
  FbdEvent *event;
  GSList *l;
  g_autoptr (GSList) events = NULL;

  g_return_if_fail (name);

  g_debug ("Client %s vanished", name);

  /*
   * Prepare a list of events to end feedback for so we don't modify
   * the hash table in place when 'feedbacks-ended' fires.
   */
  g_hash_table_iter_init (&iter, self->events);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    event = FBD_EVENT (value);
    if (!g_strcmp0 (fbd_event_get_sender (event), name))
      events = g_slist_append (events, event);
  }

  for (l = events; l; l = l->next) {
    event = l->data;
    g_debug ("Ending event %s (%d) since %s vanished",
             fbd_event_get_event (event),
             fbd_event_get_id (event),
             name);
    fbd_event_end_feedbacks (event);
  }

  g_hash_table_remove (self->clients, name);
}

static void
watch_client (FbdFeedbackManager *self, GDBusMethodInvocation *invocation)
{
  guint watch_id;
  GDBusConnection *conn = g_dbus_method_invocation_get_connection (invocation);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  watch_id = g_bus_watch_name_on_connection (conn,
					     sender,
					     G_BUS_NAME_WATCHER_FLAGS_NONE,
					     NULL,
					     on_client_vanished,
					     self,
					     NULL);
  g_hash_table_insert (self->clients, g_strdup (sender), GUINT_TO_POINTER (watch_id));
}

static void
free_client_watch (gpointer data)
{
  guint watch_id = GPOINTER_TO_UINT (data);

  if (watch_id == 0)
    return;
  g_bus_unwatch_name (watch_id);
}

static FbdFeedbackProfileLevel
get_max_level (FbdFeedbackProfileLevel global_level,
               FbdFeedbackProfileLevel app_level,
               FbdFeedbackProfileLevel event_level)
{
  FbdFeedbackProfileLevel level;

  /* Individual events and apps can select lower levels than the global level but not higher ones */
  level = global_level > app_level ? app_level : global_level;
  level = level > event_level ? event_level : level;
  return level;
}

static gboolean
parse_hints (GVariant *hints, FbdFeedbackProfileLevel *level)
{
  const gchar *profile;
  gboolean found;
  g_auto (GVariantDict) dict = G_VARIANT_DICT_INIT (NULL);

  g_variant_dict_init (&dict, hints);
  found = g_variant_dict_lookup (&dict, "profile", "&s", &profile);

  if (level && found)
    *level = fbd_feedback_profile_level (profile);
  return TRUE;
}

static gboolean
fbd_feedback_manager_handle_trigger_feedback (LfbGdbusFeedback      *object,
                                              GDBusMethodInvocation *invocation,
                                              const gchar           *arg_app_id,
                                              const gchar           *arg_event,
                                              GVariant              *arg_hints,
                                              gint                   arg_timeout)
{
  FbdFeedbackManager *self;
  FbdEvent *event;
  GSList *feedbacks, *l;
  guint event_id;
  const gchar *sender;
  FbdFeedbackProfileLevel app_level, level, hint_level = FBD_FEEDBACK_PROFILE_LEVEL_FULL;
  gboolean found_fb = FALSE;

  sender = g_dbus_method_invocation_get_sender (invocation);
  g_debug ("Event '%s' for '%s' from %s", arg_event, arg_app_id, sender);

  g_return_val_if_fail (FBD_IS_FEEDBACK_MANAGER (object), FALSE);
  g_return_val_if_fail (arg_app_id, FALSE);
  g_return_val_if_fail (arg_event, FALSE);

  self = FBD_FEEDBACK_MANAGER (object);
  if (!strlen (arg_app_id)) {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                           G_DBUS_ERROR_INVALID_ARGS,
                                           "Invalid app id %s", arg_app_id);
    return TRUE;
  }

  if (!strlen (arg_event)) {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                           G_DBUS_ERROR_INVALID_ARGS,
                                           "Invalid event %s", arg_event);
    return TRUE;
  }

  if (!parse_hints (arg_hints, &hint_level)) {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                           G_DBUS_ERROR_INVALID_ARGS,
                                           "Invalid hints");
    return TRUE;
  }

  if (arg_timeout < -1)
    arg_timeout = -1;

  event_id = self->next_id++;

  event = fbd_event_new (event_id, arg_app_id, arg_event, arg_timeout, sender);
  g_hash_table_insert (self->events, GUINT_TO_POINTER (event_id), event);

  app_level = app_get_feedback_level (arg_app_id);
  level = get_max_level (self->level, app_level, hint_level);

  feedbacks = fbd_feedback_theme_lookup_feedback (self->theme, level, event);
  if (feedbacks) {
    for (l = feedbacks; l; l = l->next) {
      FbdFeedbackBase *fb = l->data;

      if (fbd_feedback_is_available (FBD_FEEDBACK_BASE (fb))) {
        fbd_event_add_feedback (event, fb);
        found_fb = TRUE;
      }
    }
    g_slist_free_full (feedbacks, g_object_unref);
  } else {
    /* No feedbacks found at all */
    found_fb = FALSE;
  }

  lfb_gdbus_feedback_complete_trigger_feedback (object, invocation, event_id);

  if (found_fb) {
    g_signal_connect_object (event, "feedbacks-ended",
                             (GCallback) on_event_feedbacks_ended,
                             self,
                             G_CONNECT_SWAPPED);
    fbd_event_run_feedbacks (event);
    watch_client (self, invocation);
  } else {
    g_hash_table_remove (self->events, GUINT_TO_POINTER (event_id));
    lfb_gdbus_feedback_emit_feedback_ended (LFB_GDBUS_FEEDBACK (self), event_id,
                                            FBD_EVENT_END_REASON_NOT_FOUND);
  }

  return TRUE;
}

static gboolean
fbd_feedback_manager_handle_end_feedback (LfbGdbusFeedback      *object,
                                          GDBusMethodInvocation *invocation,
                                          guint                  event_id)
{
  FbdFeedbackManager *self;
  FbdEvent *event;

  g_debug ("Ending feedback for event '%d'", event_id);

  g_return_val_if_fail (FBD_IS_FEEDBACK_MANAGER (object), FALSE);
  g_return_val_if_fail (event_id, FALSE);

  self = FBD_FEEDBACK_MANAGER (object);

  event = g_hash_table_lookup (self->events, GUINT_TO_POINTER (event_id));
  if (event) {
    /* The last feedback ending will trigger event disposal via
       `on_fb_ended` */
    fbd_event_end_feedbacks (event);
  } else {
    g_warning ("Tried to end non-existing event %d", event_id);
  }

  lfb_gdbus_feedback_complete_end_feedback (object, invocation);
  return TRUE;
}

static const gchar *
find_themefile (void)
{
  gint i = 0;
  gsize len;
  const gchar *comp;

  g_autoptr (GError) err = NULL;
  g_autofree gchar *user_config_path = NULL;
  gchar **xdg_data_dirs = (gchar **) g_get_system_data_dirs ();
  g_autofree gchar *compatibles = NULL;

  // First look for a default file under $XDG_DATA_HOME
  user_config_path = g_build_filename (g_get_user_config_dir (), "feedbackd",
                                       "themes", "default.json", NULL);
  if (g_file_test (user_config_path, (G_FILE_TEST_EXISTS))) {
    g_debug ("Found user themefile at: %s", user_config_path);
    return g_steal_pointer (&user_config_path);
  }

  // Try to read the device name
  if (g_file_test (DEVICE_TREE_PATH, (G_FILE_TEST_EXISTS))) {
    g_debug ("Found device tree device compatible at %s", DEVICE_TREE_PATH);

    // Check if feedbackd has a proper config available this device
    if (!g_file_get_contents (DEVICE_TREE_PATH, &compatibles, &len, &err))
      g_warning ("Unable to read: %s", err->message);

    comp = compatibles;
    while (comp - compatibles < len) {

      // Iterate over $XDG_DATA_DIRS
      for (i = 0; i < g_strv_length (xdg_data_dirs); i++) {
        g_autofree gchar *config_path = NULL;
        g_autofree gchar *theme_file_name = NULL;

        // We leave it to g_build_filename to add/remove erroneous path separators
        theme_file_name = g_strconcat (comp, ".json", NULL);
        config_path = g_build_filename (xdg_data_dirs[i], "feedbackd", "themes",
                                        theme_file_name, NULL);
        g_debug ("Searching for device specific themefile in %s", config_path);

        // Check if file exist
        if (g_file_test (config_path, (G_FILE_TEST_EXISTS))) {
          g_debug ("Found themefile for this device at: %s", config_path);
          return g_steal_pointer (&config_path);
        }
      }

      // Next compatible
      comp = strchr (comp, 0);
      comp++;
    }
  }else  {
    g_debug ("Device tree path does not exist: %s", DEVICE_TREE_PATH);
  }

  return NULL;
}

static void
fbd_feedback_manager_constructed (GObject *object)
{
  FbdFeedbackManager *self = FBD_FEEDBACK_MANAGER (object);

  G_OBJECT_CLASS (fbd_feedback_manager_parent_class)->constructed (object);

  fbd_feedback_manager_load_theme(self);

  g_signal_connect (self, "notify::profile", (GCallback)on_profile_changed, NULL);

  self->settings = g_settings_new (FEEDBACKD_SCHEMA_ID);
  g_signal_connect_swapped (self->settings, "changed::" FEEDBACKD_KEY_PROFILE,
                            G_CALLBACK (on_feedbackd_setting_changed), self);
  on_feedbackd_setting_changed (self, FEEDBACKD_KEY_PROFILE, self->settings);
}

static void
fbd_feedback_manager_dispose (GObject *object)
{
  FbdFeedbackManager *self = FBD_FEEDBACK_MANAGER (object);

  g_clear_object (&self->settings);
  g_clear_object (&self->theme);
  g_clear_object (&self->sound);
  g_clear_object (&self->vibra);
  g_clear_object (&self->leds);
  g_clear_object (&self->client);
  g_clear_pointer (&self->events, g_hash_table_destroy);
  g_clear_pointer (&self->clients, g_hash_table_destroy);

  G_OBJECT_CLASS (fbd_feedback_manager_parent_class)->dispose (object);
}

static void
fbd_feedback_manager_feedback_iface_init (LfbGdbusFeedbackIface *iface)
{
  iface->handle_trigger_feedback = fbd_feedback_manager_handle_trigger_feedback;
  iface->handle_end_feedback = fbd_feedback_manager_handle_end_feedback;
}

static void
fbd_feedback_manager_class_init (FbdFeedbackManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = fbd_feedback_manager_constructed;
  object_class->dispose = fbd_feedback_manager_dispose;
}

static void
fbd_feedback_manager_init (FbdFeedbackManager *self)
{
  const gchar * const subsystems[] = { "input", NULL };

  self->next_id = 1;
  self->level = FBD_FEEDBACK_PROFILE_LEVEL_UNKNOWN;

  self->client = g_udev_client_new (subsystems);
  g_signal_connect_swapped (G_OBJECT (self->client), "uevent",
                            G_CALLBACK (device_changes), self);
  init_devices (self);

  self->events = g_hash_table_new_full (g_direct_hash,
                                        g_direct_equal,
                                        NULL,
                                        (GDestroyNotify)g_object_unref);
  self->clients = g_hash_table_new_full (g_str_hash,
                                         g_str_equal,
                                         g_free,
                                         free_client_watch);
}

FbdFeedbackManager *
fbd_feedback_manager_get_default (void)
{
  static FbdFeedbackManager *instance;

  if (instance == NULL) {
    instance = g_object_new (FBD_TYPE_FEEDBACK_MANAGER, NULL);
    g_object_add_weak_pointer (G_OBJECT (instance), (gpointer *)&instance);
  }
  return instance;
}

FbdDevVibra *
fbd_feedback_manager_get_dev_vibra (FbdFeedbackManager *self)
{
  g_return_val_if_fail (FBD_IS_FEEDBACK_MANAGER (self), NULL);

  return self->vibra;
}

FbdDevSound *
fbd_feedback_manager_get_dev_sound (FbdFeedbackManager *self)
{
  g_return_val_if_fail (FBD_IS_FEEDBACK_MANAGER (self), NULL);

  return self->sound;
}

FbdDevLeds *
fbd_feedback_manager_get_dev_leds (FbdFeedbackManager *self)
{
  g_return_val_if_fail (FBD_IS_FEEDBACK_MANAGER (self), NULL);

  return self->leds;
}

void fbd_feedback_manager_load_theme (FbdFeedbackManager *self) {
  g_autoptr (GError) err = NULL;
  g_autoptr (FbdFeedbackTheme) theme = NULL;
  const gchar *themefile;

  // Overide themefile with environment variable if requested
  themefile = g_getenv (FEEDBACKD_THEME_VAR);

  // Search for device-specific configuration
  if (!themefile)
    themefile = find_themefile ();

  // Fallback to default configuration if needed
  if (!themefile)
    themefile = FEEDBACKD_THEME_DIR "/default.json";
  g_info ("Using themefile: %s", themefile);

  theme = fbd_feedback_theme_new_from_file (themefile, &err);
  if (theme) {
    g_set_object(&self->theme, theme);
  } else {
    if (self->theme)
      g_warning ("Failed to reload theme: %s", err->message);
    else
      g_error ("Failed to load theme: %s", err->message); // No point to carry on
  }
}

gboolean
fbd_feedback_manager_set_profile (FbdFeedbackManager *self, const gchar *profile)
{
  FbdFeedbackProfileLevel level;

  g_return_val_if_fail (FBD_IS_FEEDBACK_MANAGER (self), FALSE);

  level = fbd_feedback_profile_level (profile);

  if (level == FBD_FEEDBACK_PROFILE_LEVEL_UNKNOWN)
    return FALSE;

  if (level == self->level)
    return TRUE;

  g_debug ("Switching profile to '%s'", profile);
  self->level = level;
  lfb_gdbus_feedback_set_profile (LFB_GDBUS_FEEDBACK (self), profile);
  g_settings_set_string (self->settings, FEEDBACKD_KEY_PROFILE, profile);
  return TRUE;
}
