#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include "game.h"
#include "utils/logger.h"

#include <stdarg.h>

static jobject string_value(const char *value) {
    return jni->NewStringUTF(&jni, value);
}

static jobject current_language(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("eng");
}

static jobject get_class_loader(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    /*
     * There is no Java VM on Vita. Return FalsoJNI's opaque object token so
     * native class-loader setup can continue through the compatibility layer.
     */
    l_info("Providing the Vita JNI class-loader shim.");
    return (jobject)0x42424242;
}

static jobject load_class(jmethodID id, va_list args) {
    (void)id;
    jstring name = va_arg(args, jstring);
    const char *text = name
        ? jni->GetStringUTFChars(&jni, name, NULL) : NULL;
    l_debug("JNI class-loader shim accepted class %s.",
            text ? text : "(null)");
    if (name && text) {
        jni->ReleaseStringUTFChars(&jni, name, (char *)text);
    }
    return (jobject)0x42424242;
}

static jboolean start_movie(jmethodID id, va_list args) {
    (void)id;
    jstring movie = va_arg(args, jstring);
    (void)va_arg(args, jint);
    (void)va_arg(args, jstring);
    const char *name = movie
        ? jni->GetStringUTFChars(&jni, movie, NULL) : "(null)";
    l_warn("FMV request is not implemented: %s.", name ? name : "(null)");
    if (movie && name) {
        jni->ReleaseStringUTFChars(&jni, movie, (char *)name);
    }
    return JNI_FALSE;
}

static void shutdown_app(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    l_info("Fusion requested application shutdown.");
    ninjago_game_request_exit();
}

static void stop_movie(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    l_info("Ignoring stopMoviePlayback: no Vita FMV backend is active.");
}

static void movie_attributes(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, double);
    l_debug("Ignoring Java movie playback attribute update.");
}

static void subtitle_info(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    (void)va_arg(args, double);
    l_debug("Subtitle timing update received without an FMV backend.");
}

static void add_subtitle(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jstring);
    (void)va_arg(args, double);
    (void)va_arg(args, double);
    l_debug("Subtitle text received without an FMV backend.");
}

static void open_browser(jmethodID id, va_list args) {
    (void)id;
    jstring url = va_arg(args, jstring);
    const char *text = url ? jni->GetStringUTFChars(&jni, url, NULL) : NULL;
    l_warn("Browser request ignored on Vita: %s.", text ? text : "(null)");
    if (url && text) {
        jni->ReleaseStringUTFChars(&jni, url, (char *)text);
    }
}

static void telemetry_event(jmethodID id, va_list args) {
    jstring event = va_arg(args, jstring);
    const char *text = event
        ? jni->GetStringUTFChars(&jni, event, NULL) : NULL;
    l_info("Offline telemetry event %d ignored: %s.", (int)id,
           text ? text : "(null)");
    if (event && text) {
        jni->ReleaseStringUTFChars(&jni, event, (char *)text);
    }
}

NameToMethodID nameToMethodId[] = {
    {100, "GetCurrentLanguageAsISO3Letter", METHOD_TYPE_OBJECT},
    {101, "getClassLoader", METHOD_TYPE_OBJECT},
    {102, "loadClass", METHOD_TYPE_OBJECT},
    {200, "startMoviePlayback", METHOD_TYPE_BOOLEAN},
    {300, "shutdownApp", METHOD_TYPE_VOID},
    {301, "stopMoviePlayback", METHOD_TYPE_VOID},
    {302, "setMoviePlaybackAttributes", METHOD_TYPE_VOID},
    {303, "setNewSubTitleInfo", METHOD_TYPE_VOID},
    {304, "addSubtitle", METHOD_TYPE_VOID},
    {305, "openBrowser", METHOD_TYPE_VOID},
    {306, "Flurry_LogEvent", METHOD_TYPE_VOID},
    {307, "Flurry_LogEventParam1", METHOD_TYPE_VOID},
};

MethodsBoolean methodsBoolean[] = {{200, start_movie}};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    {100, current_language},
    {101, get_class_loader},
    {102, load_class},
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    {300, shutdown_app},
    {301, stop_movie},
    {302, movie_attributes},
    {303, subtitle_info},
    {304, add_subtitle},
    {305, open_browser},
    {306, telemetry_event},
    {307, telemetry_event},
};

char WINDOW_SERVICE[] = "window";
const int SDK_INT = 21;
const int EGL_CONFIG = 1;

NameToFieldID nameToFieldId[] = {
    {0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT},
    {1, "SDK_INT", FIELD_TYPE_INT},
    {2, "mEGLConfig", FIELD_TYPE_INT},
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {{1, SDK_INT}, {2, EGL_CONFIG}};
FieldsObject fieldsObject[] = {{0, WINDOW_SERVICE}};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
