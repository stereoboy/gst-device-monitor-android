#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <gst/gst.h>
#include <pthread.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <locale.h>

#include <gst/gst.h>
#include <gst/gst-i18n-app.h>
#include <gst/math-compat.h>
#include <stdlib.h>
#include <stdio.h>
GST_DEBUG_CATEGORY (devmon_debug);
#define GST_CAT_DEFAULT devmon_debug

GST_DEBUG_CATEGORY_STATIC (debug_category);
#define GST_CAT_DEFAULT debug_category

/*
 * These macros provide a way to store the native pointer to CustomData, which might be 32 or 64 bits, into
 * a jlong, which is always 64 bits, without warnings.
 */
#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData
{
  jobject app;                  /* Application instance, used to call its methods. A global reference is kept. */
  GstElement *pipeline;         /* The running pipeline */
  GMainContext *context;        /* GLib context used to run the main loop */
  GMainLoop *main_loop;         /* GLib main loop */
  gboolean initialized;         /* To avoid informing the UI multiple times about the initialization */
} CustomData;

/* These global variables cache values which are not changing during execution */
static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID custom_data_field_id;
static jmethodID set_message_method_id;
static jmethodID on_gstreamer_initialized_method_id;

/*
 * Private methods
 */

/* Register this thread with the VM */
static JNIEnv *
attach_current_thread (void)
{
  JNIEnv *env;
  JavaVMAttachArgs args;

  GST_DEBUG ("Attaching thread %p", g_thread_self ());
  args.version = JNI_VERSION_1_4;
  args.name = NULL;
  args.group = NULL;

  if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
    GST_ERROR ("Failed to attach current thread");
    return NULL;
  }

  return env;
}

/* Unregister this thread from the VM */
static void
detach_current_thread (void *env)
{
  GST_DEBUG ("Detaching thread %p", g_thread_self ());
  (*java_vm)->DetachCurrentThread (java_vm);
}

/* Retrieve the JNI environment for this thread */
static JNIEnv *
get_jni_env (void)
{
  JNIEnv *env;

  if ((env = pthread_getspecific (current_jni_env)) == NULL) {
    env = attach_current_thread ();
    pthread_setspecific (current_jni_env, env);
  }

  return env;
}

/* Change the content of the UI's TextView */
static void
set_ui_message (const gchar * message, CustomData * data)
{
  JNIEnv *env = get_jni_env ();
  GST_DEBUG ("Setting message to: %s", message);
  jstring jmessage = (*env)->NewStringUTF (env, message);
  (*env)->CallVoidMethod (env, data->app, set_message_method_id, jmessage);
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }
  (*env)->DeleteLocalRef (env, jmessage);
}
#if 0
/* Retrieve errors from the bus and show them on the UI */
static void
error_cb (GstBus * bus, GstMessage * msg, CustomData * data)
{
  GError *err;
  gchar *debug_info;
  gchar *message_string;

  gst_message_parse_error (msg, &err, &debug_info);
  message_string =
      g_strdup_printf ("Error received from element %s: %s",
      GST_OBJECT_NAME (msg->src), err->message);
  g_clear_error (&err);
  g_free (debug_info);
  set_ui_message (message_string, data);
  g_free (message_string);
  gst_element_set_state (data->pipeline, GST_STATE_NULL);
}

/* Notify UI about pipeline state changes */
static void
state_changed_cb (GstBus * bus, GstMessage * msg, CustomData * data)
{
  GstState old_state, new_state, pending_state;
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  /* Only pay attention to messages coming from the pipeline, not its children */
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
    gchar *message = g_strdup_printf ("State changed to %s",
        gst_element_state_get_name (new_state));
    set_ui_message (message, data);
    g_free (message);
  }
}

/* Check if all conditions are met to report GStreamer as initialized.
 * These conditions will change depending on the application */
static void
check_initialization_complete (CustomData * data)
{
  JNIEnv *env = get_jni_env ();
  if (!data->initialized && data->main_loop) {
    GST_DEBUG ("Initialization complete, notifying application. main_loop:%p",
        data->main_loop);
    (*env)->CallVoidMethod (env, data->app, on_gstreamer_initialized_method_id);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to call Java method");
      (*env)->ExceptionClear (env);
    }
    data->initialized = TRUE;
  }
}
#endif

typedef struct
{
    GMainLoop *loop;
    GstDeviceMonitor *monitor;
    guint bus_watch_id;
} DevMonApp;

static gboolean bus_msg_handler (GstBus * bus, GstMessage * msg, gpointer data);

static gchar *
get_launch_line (GstDevice * device)
{
    static const char *const ignored_propnames[] =
            { "name", "parent", "direction", "template", "caps", NULL };
    GString *launch_line;
    GstElement *element;
    GstElement *pureelement;
    GParamSpec **properties, *property;
    GValue value = G_VALUE_INIT;
    GValue pvalue = G_VALUE_INIT;
    guint i, number_of_properties;
    GstElementFactory *factory;

    element = gst_device_create_element (device, NULL);

    if (!element)
        return NULL;

    factory = gst_element_get_factory (element);
    if (!factory) {
        gst_object_unref (element);
        return NULL;
    }

    if (!gst_plugin_feature_get_name (factory)) {
        gst_object_unref (element);
        return NULL;
    }

    launch_line = g_string_new (gst_plugin_feature_get_name (factory));

    pureelement = gst_element_factory_create (factory, NULL);

    /* get paramspecs and show non-default properties */
    properties =
            g_object_class_list_properties (G_OBJECT_GET_CLASS (element),
                                            &number_of_properties);
    if (properties) {
        for (i = 0; i < number_of_properties; i++) {
            gint j;
            gboolean ignore = FALSE;
            property = properties[i];

            /* skip some properties */
            if ((property->flags & G_PARAM_READWRITE) != G_PARAM_READWRITE)
                continue;

            for (j = 0; ignored_propnames[j]; j++)
                if (!g_strcmp0 (ignored_propnames[j], property->name))
                    ignore = TRUE;

            if (ignore)
                continue;

            /* Can't use _param_value_defaults () because sub-classes modify the
             * values already.
             */

            g_value_init (&value, property->value_type);
            g_value_init (&pvalue, property->value_type);
            g_object_get_property (G_OBJECT (element), property->name, &value);
            g_object_get_property (G_OBJECT (pureelement), property->name, &pvalue);
            if (gst_value_compare (&value, &pvalue) != GST_VALUE_EQUAL) {
                gchar *valuestr = gst_value_serialize (&value);

                if (!valuestr) {
                    GST_WARNING ("Could not serialize property %s:%s",
                                 GST_OBJECT_NAME (element), property->name);
                    g_free (valuestr);
                    goto next;
                }

                g_string_append_printf (launch_line, " %s=%s",
                                        property->name, valuestr);
                g_free (valuestr);

            }

            next:
            g_value_unset (&value);
            g_value_unset (&pvalue);
        }
        g_free (properties);
    }

    gst_object_unref (element);
    gst_object_unref (pureelement);

    return g_string_free (launch_line, FALSE);
}


static gboolean
print_structure_field (GQuark field_id, const GValue * value,
                       gpointer user_data)
{
    gchar *val;

    if (G_VALUE_HOLDS_UINT (value)) {
        val = g_strdup_printf ("%u (0x%08x)", g_value_get_uint (value),
                               g_value_get_uint (value));
    } else {
        val = gst_value_serialize (value);
    }

    if (val != NULL)
        GST_INFO ("\n\t\t%s = %s", g_quark_to_string (field_id), val);
    else
        GST_INFO ("\n\t\t%s - could not serialise field of type %s",
                 g_quark_to_string (field_id), G_VALUE_TYPE_NAME (value));

    g_free (val);

    return TRUE;
}

static gboolean
print_field (GQuark field, const GValue * value, gpointer unused)
{
    gchar *str = gst_value_serialize (value);

    GST_INFO (", %s=%s", g_quark_to_string (field), str);
    g_free (str);
    return TRUE;
}

static void
print_device (GstDevice * device, gboolean modified)
{
    gchar *device_class, *str, *name;
    GstCaps *caps;
    GstStructure *props;
    guint i, size = 0;

    caps = gst_device_get_caps (device);
    if (caps != NULL)
        size = gst_caps_get_size (caps);

    name = gst_device_get_display_name (device);
    device_class = gst_device_get_device_class (device);
    props = gst_device_get_properties (device);

    GST_INFO ("\nDevice %s:\n\n", modified ? "modified" : "found");
    GST_INFO ("\tname  : %s\n", name);
    GST_INFO ("\tclass : %s\n", device_class);
    for (i = 0; i < size; ++i) {
        GstStructure *s = gst_caps_get_structure (caps, i);
        GstCapsFeatures *features = gst_caps_get_features (caps, i);

        GST_INFO ("\t%s %s", (i == 0) ? "caps  :" : "       ",
                 gst_structure_get_name (s));
        if (features && (gst_caps_features_is_any (features) ||
                         !gst_caps_features_is_equal (features,
                                                      GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))) {
            gchar *features_string = gst_caps_features_to_string (features);

            GST_INFO ("(%s)", features_string);
            g_free (features_string);
        }
        gst_structure_foreach (s, print_field, NULL);
        GST_INFO ("\n");
    }
    if (props) {
        GST_INFO ("\tproperties:");
        gst_structure_foreach (props, print_structure_field, NULL);
        gst_structure_free (props);
        GST_INFO ("\n");
    }
    str = get_launch_line (device);
    if (gst_device_has_classes (device, "Source"))
        GST_INFO ("\tgst-launch-1.0 %s ! ...\n", str);
    else if (gst_device_has_classes (device, "Sink"))
        GST_INFO ("\tgst-launch-1.0 ... ! %s\n", str);
    else if (gst_device_has_classes (device, "CameraSource")) {
        GST_INFO ("\tgst-launch-1.0 %s.vfsrc name=camerasrc ! ... "
                 "camerasrc.vidsrc ! [video/x-h264] ... \n", str);
    }

    g_free (str);
    GST_INFO ("\n");

    g_free (name);
    g_free (device_class);

    if (caps != NULL)
        gst_caps_unref (caps);
}

static void
device_removed (GstDevice * device)
{
    gchar *name;

    name = gst_device_get_display_name (device);

    GST_INFO ("Device removed:\n");
    GST_INFO ("\tname  : %s\n", name);

    g_free (name);
}

static gboolean
bus_msg_handler (GstBus * bus, GstMessage * msg, gpointer user_data)
{
    GstDevice *device;

    switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_DEVICE_ADDED:
            gst_message_parse_device_added (msg, &device);
            print_device (device, FALSE);
            gst_object_unref (device);
            break;
        case GST_MESSAGE_DEVICE_REMOVED:
            gst_message_parse_device_removed (msg, &device);
            device_removed (device);
            gst_object_unref (device);
            break;
        case GST_MESSAGE_DEVICE_CHANGED:
            gst_message_parse_device_changed (msg, &device, NULL);
            print_device (device, TRUE);
            gst_object_unref (device);
            break;
        default:
            GST_INFO ("%s message\n", GST_MESSAGE_TYPE_NAME (msg));
            break;
    }

    return TRUE;
}

static gboolean
quit_loop (GMainLoop * loop)
{
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

/* Main method for the native code. This is executed on its own thread. */
static void *
app_function (void *userdata)
{
#if 0
  JavaVMAttachArgs args;
  GstBus *bus;
  CustomData *data = (CustomData *) userdata;
  GSource *bus_source;
  GError *error = NULL;

  GST_DEBUG ("Creating pipeline in CustomData at %p", data);

  /* Create our own GLib Main Context and make it the default one */
  data->context = g_main_context_new ();
  g_main_context_push_thread_default (data->context);

  /* Build pipeline */
  data->pipeline =
      gst_parse_launch
      ("audiotestsrc ! audioconvert ! audioresample ! autoaudiosink", &error);
  if (error) {
    gchar *message =
        g_strdup_printf ("Unable to build pipeline: %s", error->message);
    g_clear_error (&error);
    set_ui_message (message, data);
    g_free (message);
    return NULL;
  }

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus (data->pipeline);
  bus_source = gst_bus_create_watch (bus);
  g_source_set_callback (bus_source, (GSourceFunc) gst_bus_async_signal_func,
      NULL, NULL);
  g_source_attach (bus_source, data->context);
  g_source_unref (bus_source);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback) error_cb,
      data);
  g_signal_connect (G_OBJECT (bus), "message::state-changed",
      (GCallback) state_changed_cb, data);
  gst_object_unref (bus);

  /* Create a GLib Main Loop and set it to run */
  GST_DEBUG ("Entering main loop... (CustomData:%p)", data);
  data->main_loop = g_main_loop_new (data->context, FALSE);
  check_initialization_complete (data);
  g_main_loop_run (data->main_loop);
  GST_DEBUG ("Exited main loop");
  g_main_loop_unref (data->main_loop);
  data->main_loop = NULL;

  /* Free resources */
  g_main_context_pop_thread_default (data->context);
  g_main_context_unref (data->context);
  gst_element_set_state (data->pipeline, GST_STATE_NULL);
  gst_object_unref (data->pipeline);

#else
    GST_INFO ("Device Monitor");

    //gst_init(NULL, NULL);

    int argc = 1;
    char *_argv[] = {"./gst-device-monitor"};
    char **argv = _argv;
    gboolean print_version = FALSE;
    GError *err = NULL;
    gchar **arg, **args = NULL;
    gboolean follow = FALSE;
    gboolean include_hidden = FALSE;
    GOptionContext *ctx;
    GOptionEntry options[] = {
            {"version", 0, 0, G_OPTION_ARG_NONE, &print_version,
                                                                         N_("Print version information and exit"), NULL},
            {"follow", 'f', 0, G_OPTION_ARG_NONE, &follow,
                                                                         N_("Don't exit after showing the initial device list, but wait "
                                                                            "for devices to added/removed."), NULL},
            {"include-hidden", 'i', 0, G_OPTION_ARG_NONE, &include_hidden,
                                                                         N_("Include devices from hidden device providers."), NULL},
            {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &args, NULL},
            {NULL}
    };
    GTimer *timer;
    DevMonApp app;
    GstBus *bus;

    setlocale (LC_ALL, "");

#ifdef ENABLE_NLS
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
#endif

    g_set_prgname ("gst-device-monitor-" GST_API_VERSION);

    ctx = g_option_context_new ("[DEVICE_CLASSES[:FILTER_CAPS]] "
                                "[DEVICE_CLASSES[:FILTER_CAPS]] …");
    g_option_context_add_main_entries (ctx, options, GETTEXT_PACKAGE);
    g_option_context_add_group (ctx, gst_init_get_option_group ());
    if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
        GST_ERROR ("Error initializing: %s\n", GST_STR_NULL (err->message));
        g_option_context_free (ctx);
        g_clear_error (&err);
        return 1;
    }
    g_option_context_free (ctx);

    GST_DEBUG_CATEGORY_INIT (devmon_debug, "device-monitor", 0,
                             "gst-device-monitor");

    if (print_version)
    {
        gchar *version_str;

        version_str = gst_version_string ();
        GST_INFO ("%s version %s\n", g_get_prgname (), PACKAGE_VERSION);
        GST_INFO ("%s\n", version_str);
        GST_INFO ("%s\n", GST_PACKAGE_ORIGIN);
        g_free (version_str);

        return 0;
    }

    app.loop = g_main_loop_new (NULL, FALSE);
    app.monitor = gst_device_monitor_new ();
    gst_device_monitor_set_show_all_devices (app.monitor, include_hidden);

    bus = gst_device_monitor_get_bus (app.monitor);
    app.bus_watch_id = gst_bus_add_watch (bus, bus_msg_handler, &app);
    gst_object_unref (bus);

    /* process optional remaining arguments in the form
     * DEVICE_CLASSES or DEVICE_CLASSES:FILTER_CAPS */
    for (arg = args; arg != NULL && *arg != NULL; ++arg) {
        gchar **filters = g_strsplit (*arg, ":", -1);
        if (filters != NULL && filters[0] != NULL) {
            GstCaps *caps = NULL;

            if (filters[1] != NULL) {
                caps = gst_caps_from_string (filters[1]);
                if (caps == NULL)
                    GST_WARNING ("Couldn't parse device filter caps '%s'", filters[1]);
            }
            gst_device_monitor_add_filter (app.monitor, filters[0], caps);
            if (caps)
                gst_caps_unref (caps);
            g_strfreev (filters);
        }
    }
    g_strfreev (args);

    GST_INFO ("Probing devices...\n\n");

    timer = g_timer_new ();

    if (!gst_device_monitor_start (app.monitor)) {
        GST_ERROR ("Failed to start device monitor!\n");
        return -1;
    }

    GST_INFO ("Took %.2f seconds", g_timer_elapsed (timer, NULL));

    if (!follow) {
        /* Consume all the messages pending on the bus and exit */
        g_idle_add ((GSourceFunc) quit_loop, app.loop);
    } else {
        GST_INFO ("Monitoring devices, waiting for devices to be removed or "
                 "new devices to be added...\n");
    }

    g_main_loop_run (app.loop);

    gst_device_monitor_stop (app.monitor);
    gst_object_unref (app.monitor);

    g_source_remove (app.bus_watch_id);
    g_main_loop_unref (app.loop);
    g_timer_destroy (timer);
#endif
  return NULL;
}

/*
 * Java Bindings
 */

/* Instruct the native code to create its internal data structure, pipeline and thread */
static void
gst_native_init (JNIEnv * env, jobject thiz)
{
  CustomData *data = g_new0 (CustomData, 1);
  SET_CUSTOM_DATA (env, thiz, custom_data_field_id, data);
  GST_DEBUG_CATEGORY_INIT (debug_category, "device_monitor", 0,
      "Android tutorial 2");
  // TODO
  gst_debug_set_threshold_for_name ("*", GST_LEVEL_LOG);
  GST_DEBUG ("Created CustomData at %p", data);
  data->app = (*env)->NewGlobalRef (env, thiz);
  GST_DEBUG ("Created GlobalRef for app object at %p", data->app);
  pthread_create (&gst_app_thread, NULL, &app_function, data);
}

/* Quit the main loop, remove the native thread and free resources */
static void
gst_native_finalize (JNIEnv * env, jobject thiz)
{
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data)
    return;
  GST_DEBUG ("Quitting main loop...");
  g_main_loop_quit (data->main_loop);
  GST_DEBUG ("Waiting for thread to finish...");
  pthread_join (gst_app_thread, NULL);
  GST_DEBUG ("Deleting GlobalRef for app object at %p", data->app);
  (*env)->DeleteGlobalRef (env, data->app);
  GST_DEBUG ("Freeing CustomData at %p", data);
  g_free (data);
  SET_CUSTOM_DATA (env, thiz, custom_data_field_id, NULL);
  GST_DEBUG ("Done finalizing");
}

/* Set pipeline to PLAYING state */
static void
gst_native_play (JNIEnv * env, jobject thiz)
{
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data)
    return;
  GST_DEBUG ("Setting state to PLAYING");
  gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
}

/* Set pipeline to PAUSED state */
static void
gst_native_pause (JNIEnv * env, jobject thiz)
{
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data)
    return;
  GST_DEBUG ("Setting state to PAUSED");
  gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
}

/* Static class initializer: retrieve method and field IDs */
static jboolean
gst_native_class_init (JNIEnv * env, jclass klass)
{
  custom_data_field_id =
      (*env)->GetFieldID (env, klass, "native_custom_data", "J");
  set_message_method_id =
      (*env)->GetMethodID (env, klass, "setMessage", "(Ljava/lang/String;)V");
  on_gstreamer_initialized_method_id =
      (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");

  if (!custom_data_field_id || !set_message_method_id
      || !on_gstreamer_initialized_method_id) {
    /* We emit this message through the Android log instead of the GStreamer log because the later
     * has not been initialized yet.
     */
    __android_log_print (ANDROID_LOG_ERROR, "device_monitor",
        "The calling class does not implement all necessary interface methods");
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

/* List of implemented native methods */
static JNINativeMethod native_methods[] = {
  {"nativeInit", "()V", (void *) gst_native_init},
  {"nativeFinalize", "()V", (void *) gst_native_finalize},
  {"nativePlay", "()V", (void *) gst_native_play},
  {"nativePause", "()V", (void *) gst_native_pause},
  {"nativeClassInit", "()Z", (void *) gst_native_class_init}
};

/* Library initializer */
jint
JNI_OnLoad (JavaVM * vm, void *reserved)
{
  JNIEnv *env = NULL;

  //TODO
  //references
  // - https://lists.freedesktop.org/archives/gstreamer-android/2013-April/000448.html
  setenv("GST_DEBUG", "*:5", 1);
  //setenv("GST_DEBUG_NO_COLOR", "1", 1);


  java_vm = vm;

  if ((*vm)->GetEnv (vm, (void **) &env, JNI_VERSION_1_4) != JNI_OK) {
    __android_log_print (ANDROID_LOG_ERROR, "device_monitor",
        "Could not retrieve JNIEnv");
    return 0;
  }
  jclass klass = (*env)->FindClass (env,
      "org/freedesktop/gstreamer/DeviceMonitor");
  (*env)->RegisterNatives (env, klass, native_methods,
      G_N_ELEMENTS (native_methods));

  pthread_key_create (&current_jni_env, detach_current_thread);

  return JNI_VERSION_1_4;
}
