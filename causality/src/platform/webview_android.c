// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_android.c — Android web view backend. Drives the Java companion class
   org.causality.CausalityWebView (android.webkit.WebView on the main looper) over JNI.

   Threads: Causality's UI thread calls the ca_webview_native_* functions. Java calls back from the
   Android main thread (and WebView's bridge thread) through the native methods registered below,
   which only enqueue records; ca_webview_native_pump drains them on the UI thread. Native objects are
   found by a numeric handle in a registry so late callbacks after destroy are dropped safely. */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <jni.h>

#include "webview_native.h"
#include "android_host.h"

#define JNI(env, fn, ...) ((*(env))->fn((env), __VA_ARGS__))
#define JNI0(env, fn)     ((*(env))->fn((env)))

#define POLICY_TIMEOUT_NS 200000000L
#define NS_PER_SECOND     1000000000L

static const char *const k_class_name = "org/causality/CausalityWebView";

/* Order mirrors the EVENT_* constants of CausalityWebView.java. */
static const Ca_WebViewEventType k_event_types[] = {
    CA_WEBVIEW_EVENT_LOAD_STARTED,     CA_WEBVIEW_EVENT_LOAD_FINISHED,
    CA_WEBVIEW_EVENT_LOAD_FAILED,      CA_WEBVIEW_EVENT_URL_CHANGED,
    CA_WEBVIEW_EVENT_TITLE_CHANGED,    CA_WEBVIEW_EVENT_PROGRESS_CHANGED,
    CA_WEBVIEW_EVENT_HISTORY_CHANGED,  CA_WEBVIEW_EVENT_NEW_WINDOW,
    CA_WEBVIEW_EVENT_PROCESS_TERMINATED,
};

typedef enum EventKind {
    EVENT_STATE,
    EVENT_MESSAGE,
    EVENT_EVAL,
    EVENT_KEY,
    EVENT_POLICY,
} EventKind;

/** Record queued from a Java thread for the UI thread. */
typedef struct Event {
    struct Event    *next;
    EventKind        kind;
    Ca_WebViewEventType type;
    char            *url;
    char            *title;
    char            *detail;
    double           progress;
    bool             loading;
    bool             can_go_back;
    bool             can_go_forward;
    Ca_WebViewEvalFn eval_fn;
    void            *eval_data;
    bool             ok;
    int              key_action;
    bool             done;      /* policy: answered                                      */
    bool             abandoned; /* policy: waiter timed out, queue side owns the record  */
    bool             allowed;   /* policy: answer                                        */
} Event;

/** Eval waiting for its completion value from Java. */
typedef struct PendingEval {
    struct PendingEval *next;
    uint64_t            id;
    Ca_WebViewEvalFn    fn;
    void               *user_data;
} PendingEval;

struct Ca_WebViewNative {
    Ca_WebViewNative *next;       /* registry link, guarded by g_lock                 */
    uint64_t          id;         /* handle given to Java                             */
    unsigned          refs;       /* guarded by g_lock                                */
    Ca_Window        *window;
    Ca_WebView       *owner;
    JavaVM           *vm;
    jobject           java;       /* global ref, UI thread only                       */
    pthread_t         ui_thread;

    pthread_mutex_t   lock;       /* guards the fields below                          */
    pthread_cond_t    policy_cond;
    bool              alive;
    Event            *head;
    Event            *tail;
    PendingEval      *evals;
    uint64_t          next_eval;

    char             *state_url;  /* last reported state, UI thread only              */
    char             *state_title;
    double            state_progress;
    bool              state_loading;
    bool              state_back;
    bool              state_forward;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Ca_WebViewNative *g_natives;
static uint64_t g_next_id = 1;
static bool g_registered;
static JavaVM *g_vm;
static pthread_key_t g_env_key;
static pthread_once_t g_env_once = PTHREAD_ONCE_INIT;

static struct {
    jclass    cls;
    jmethodID create;
    jmethodID set_frame;
    jmethodID load_url;
    jmethodID load_html;
    jmethodID reload;
    jmethodID stop;
    jmethodID go_back;
    jmethodID go_forward;
    jmethodID set_zoom;
    jmethodID eval;
    jmethodID focus;
    jmethodID blur;
    jmethodID has_focus;
    jmethodID destroy;
} g_java;

static const struct {
    jmethodID  *slot;
    const char *name;
    const char *signature;
} k_instance_methods[] = {
    { &g_java.set_frame,  "setFrame",  "(FFFFFFFFFZF)V" },
    { &g_java.load_url,   "loadUrl",   "(Ljava/lang/String;)V" },
    { &g_java.load_html,  "loadHtml",  "(Ljava/lang/String;Ljava/lang/String;)V" },
    { &g_java.reload,     "reload",    "()V" },
    { &g_java.stop,       "stop",      "()V" },
    { &g_java.go_back,    "goBack",    "()V" },
    { &g_java.go_forward, "goForward", "()V" },
    { &g_java.set_zoom,   "setZoom",   "(D)V" },
    { &g_java.eval,       "eval",      "(Ljava/lang/String;J)V" },
    { &g_java.focus,      "focus",     "()V" },
    { &g_java.blur,       "blur",      "()V" },
    { &g_java.has_focus,  "hasFocus",  "()Z" },
    { &g_java.destroy,    "destroy",   "()V" },
};

/* ---- Weak defaults: the library links and reports "unsupported" without a platform layer ---- */

/** Default: no Java VM registered. */
__attribute__((weak)) JavaVM *ca_android_java_vm(void)
{
    return NULL;
}

/** Default: no host view. */
__attribute__((weak)) jobject ca_android_window_host_view(const Ca_Window *window)
{
    (void)window;
    return NULL;
}

/** Default: one device pixel per point. */
__attribute__((weak)) float ca_android_window_density(const Ca_Window *window)
{
    (void)window;
    return 1.0f;
}

/* ---- JNI helpers ---- */

/**
 * Report and clear a pending Java exception.
 *
 * @param env JNI environment of the calling thread.
 * @return true when no exception was pending.
 */
static bool jni_ok(JNIEnv *env)
{
    if (!JNI0(env, ExceptionCheck)) return true;
    JNI0(env, ExceptionDescribe);
    JNI0(env, ExceptionClear);
    return false;
}

/** Thread-exit hook that detaches threads this backend attached. */
static void detach_thread(void *unused)
{
    (void)unused;
    JavaVM *vm = __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE);
    if (vm) (*vm)->DetachCurrentThread(vm);
}

/** Create the thread-exit key once. */
static void env_key_init(void)
{
    pthread_key_create(&g_env_key, detach_thread);
}

/**
 * Get the JNI environment of the calling thread, attaching it when needed. Threads attached here
 * are detached when they exit.
 *
 * @param vm The Java VM.
 * @return The environment, or NULL when attaching failed.
 */
static JNIEnv *thread_env(JavaVM *vm)
{
    JNIEnv *env = NULL;
    jint status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_OK) return env;
    if (status != JNI_EDETACHED) return NULL;
    __atomic_store_n(&g_vm, vm, __ATOMIC_RELEASE);
    pthread_once(&g_env_once, env_key_init);
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
    pthread_setspecific(g_env_key, env);
    return env;
}

/**
 * Convert standard UTF-8 to a Java string. Invalid bytes become U+FFFD. NewStringUTF is avoided
 * because it expects modified UTF-8.
 *
 * @param env  JNI environment.
 * @param text NUL-terminated UTF-8, or NULL.
 * @return Local reference, or NULL for NULL input or on failure (exception possibly pending).
 */
static jstring utf8_to_jstring(JNIEnv *env, const char *text)
{
    if (!text) return NULL;
    size_t length = strlen(text);
    if (length > (size_t)INT32_MAX) return NULL;
    jchar *units = malloc((length + 1u) * sizeof(jchar));
    if (!units) return NULL;

    size_t count = 0;
    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + length;
    while (p < end) {
        unsigned char lead = *p;
        size_t need = 1;
        uint32_t cp = lead;
        if (lead >= 0xC2 && lead <= 0xDF) { need = 2; cp = lead & 0x1Fu; }
        else if ((lead & 0xF0u) == 0xE0u) { need = 3; cp = lead & 0x0Fu; }
        else if (lead >= 0xF0 && lead <= 0xF4) { need = 4; cp = lead & 0x07u; }
        else if (lead >= 0x80) need = 0;

        bool valid = need != 0 && (size_t)(end - p) >= need;
        for (size_t i = 1; valid && i < need; ++i) {
            valid = (p[i] & 0xC0u) == 0x80u;
            cp = (cp << 6) | (p[i] & 0x3Fu);
        }
        if (valid && need == 3) valid = cp >= 0x800u && (cp < 0xD800u || cp > 0xDFFFu);
        if (valid && need == 4) valid = cp >= 0x10000u && cp <= 0x10FFFFu;
        if (!valid) {
            units[count++] = 0xFFFD;
            ++p;
            continue;
        }
        p += need;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            units[count++] = (jchar)(0xD800u + (cp >> 10));
            units[count++] = (jchar)(0xDC00u + (cp & 0x3FFu));
        } else {
            units[count++] = (jchar)cp;
        }
    }
    jstring result = JNI(env, NewString, units, (jsize)count);
    free(units);
    return result;
}

/**
 * Convert a Java string to a heap-allocated standard UTF-8 string. Unpaired surrogates become
 * U+FFFD.
 *
 * @param env  JNI environment.
 * @param text Java string, or NULL.
 * @return malloc'd text the caller frees, or NULL for NULL input or on failure.
 */
static char *jstring_to_utf8(JNIEnv *env, jstring text)
{
    if (!text) return NULL;
    jsize length = JNI(env, GetStringLength, text);
    const jchar *units = JNI(env, GetStringChars, text, NULL);
    if (!units) {
        jni_ok(env);
        return NULL;
    }
    char *out = malloc((size_t)length * 3u + 1u);
    if (out) {
        size_t n = 0;
        for (jsize i = 0; i < length; ++i) {
            uint32_t cp = units[i];
            if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < length &&
                units[i + 1] >= 0xDC00u && units[i + 1] <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (units[i + 1] - 0xDC00u);
                ++i;
            } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
                cp = 0xFFFDu;
            }
            if (cp < 0x80u) {
                out[n++] = (char)cp;
            } else if (cp < 0x800u) {
                out[n++] = (char)(0xC0u | (cp >> 6));
                out[n++] = (char)(0x80u | (cp & 0x3Fu));
            } else if (cp < 0x10000u) {
                out[n++] = (char)(0xE0u | (cp >> 12));
                out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                out[n++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                out[n++] = (char)(0xF0u | (cp >> 18));
                out[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                out[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
        }
        out[n] = '\0';
    }
    JNI(env, ReleaseStringChars, text, units);
    return out;
}

/* ---- Records and native object lifetime ---- */

/** Free an event and its strings. */
static void event_free(Event *event)
{
    if (!event) return;
    free(event->url);
    free(event->title);
    free(event->detail);
    free(event);
}

/**
 * Allocate a zeroed event.
 *
 * @param kind What the event carries.
 * @return The event, or NULL when out of memory.
 */
static Event *event_new(EventKind kind)
{
    Event *event = calloc(1, sizeof *event);
    if (event) event->kind = kind;
    return event;
}

/** Free a list of pending evals. */
static void pending_free(PendingEval *list)
{
    while (list) {
        PendingEval *next = list->next;
        free(list);
        list = next;
    }
}

/**
 * Take a reference to the native object behind a Java handle.
 *
 * @param id Handle passed to Java at creation.
 * @return The object with a reference held, or NULL when it was destroyed.
 */
static Ca_WebViewNative *native_acquire(jlong id)
{
    Ca_WebViewNative *found = NULL;
    pthread_mutex_lock(&g_lock);
    for (Ca_WebViewNative *n = g_natives; n; n = n->next) {
        if (n->id == (uint64_t)id) {
            ++n->refs;
            found = n;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

/** Release a reference; the last one frees the object. */
static void native_release(Ca_WebViewNative *native)
{
    pthread_mutex_lock(&g_lock);
    bool last = --native->refs == 0;
    pthread_mutex_unlock(&g_lock);
    if (!last) return;
    pthread_cond_destroy(&native->policy_cond);
    pthread_mutex_destroy(&native->lock);
    free(native->state_url);
    free(native->state_title);
    free(native);
}

/** Remove a native object from the registry so no new reference can be taken. */
static void native_unlink(Ca_WebViewNative *native)
{
    pthread_mutex_lock(&g_lock);
    for (Ca_WebViewNative **link = &g_natives; *link; link = &(*link)->next) {
        if (*link == native) {
            *link = native->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/**
 * Allocate a native object and enter it in the registry.
 *
 * @param window Host window.
 * @param owner  Receives host callbacks.
 * @param vm     Java VM.
 * @return The object holding the creator's reference, or NULL when out of memory.
 */
static Ca_WebViewNative *native_new(Ca_Window *window, Ca_WebView *owner, JavaVM *vm)
{
    Ca_WebViewNative *native = calloc(1, sizeof *native);
    if (!native) return NULL;
    if (pthread_mutex_init(&native->lock, NULL) != 0) {
        free(native);
        return NULL;
    }
    pthread_condattr_t attr;
    bool cond_ok = pthread_condattr_init(&attr) == 0;
    if (cond_ok) {
        cond_ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
                  pthread_cond_init(&native->policy_cond, &attr) == 0;
        pthread_condattr_destroy(&attr);
    }
    if (!cond_ok) {
        pthread_mutex_destroy(&native->lock);
        free(native);
        return NULL;
    }
    native->window = window;
    native->owner = owner;
    native->vm = vm;
    native->alive = true;
    native->refs = 1;
    native->ui_thread = pthread_self();
    pthread_mutex_lock(&g_lock);
    native->id = g_next_id++;
    native->next = g_natives;
    g_natives = native;
    pthread_mutex_unlock(&g_lock);
    return native;
}

/**
 * Queue an event for the UI thread and wake it. Takes ownership of the event.
 *
 * @param native Target object.
 * @param event  Event to queue; freed when the object is no longer alive.
 */
static void enqueue(Ca_WebViewNative *native, Event *event)
{
    pthread_mutex_lock(&native->lock);
    if (!native->alive) {
        pthread_mutex_unlock(&native->lock);
        event_free(event);
        return;
    }
    if (native->tail) native->tail->next = event;
    else native->head = event;
    native->tail = event;
    pthread_mutex_unlock(&native->lock);
    ca_instance_wake();
}

/**
 * Remove and return the pending eval with the given id.
 *
 * @param native Owning object.
 * @param id     Eval id.
 * @return The record, or NULL when it is not pending.
 */
static PendingEval *pending_take(Ca_WebViewNative *native, uint64_t id)
{
    PendingEval *found = NULL;
    pthread_mutex_lock(&native->lock);
    for (PendingEval **link = &native->evals; *link; link = &(*link)->next) {
        if ((*link)->id == id) {
            found = *link;
            *link = found->next;
            break;
        }
    }
    pthread_mutex_unlock(&native->lock);
    return found;
}

/** Report whether the native object has not been destroyed. */
static bool native_alive(Ca_WebViewNative *native)
{
    pthread_mutex_lock(&native->lock);
    bool alive = native->alive;
    pthread_mutex_unlock(&native->lock);
    return alive;
}

/**
 * Wait for the UI thread to answer a navigation policy question, for at most 200 ms.
 *
 * @param native Object the navigation belongs to.
 * @param url    Target address.
 * @return The answer; true on timeout, failure or destruction.
 */
static bool request_policy(Ca_WebViewNative *native, const char *url)
{
    Event *event = event_new(EVENT_POLICY);
    if (!event) return true;
    event->url = strdup(url);
    if (!event->url) {
        event_free(event);
        return true;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += POLICY_TIMEOUT_NS;
    if (deadline.tv_nsec >= NS_PER_SECOND) {
        deadline.tv_nsec -= NS_PER_SECOND;
        ++deadline.tv_sec;
    }

    pthread_mutex_lock(&native->lock);
    if (!native->alive) {
        pthread_mutex_unlock(&native->lock);
        event_free(event);
        return true;
    }
    if (native->tail) native->tail->next = event;
    else native->head = event;
    native->tail = event;
    ca_instance_wake();
    while (!event->done) {
        if (pthread_cond_timedwait(&native->policy_cond, &native->lock, &deadline) == ETIMEDOUT)
            break;
    }
    bool allowed = true;
    if (event->done) {
        allowed = event->allowed;
        pthread_mutex_unlock(&native->lock);
        event_free(event);
    } else {
        event->abandoned = true;
        pthread_mutex_unlock(&native->lock);
    }
    return allowed;
}

/* ---- Native methods called by Java ---- */

/** Java -> native: a state-bearing event (load, url, title, progress, history, new window). */
static void JNICALL native_event(JNIEnv *env, jclass cls, jlong handle, jint kind, jstring url,
                                 jstring title, jint progress, jboolean loading,
                                 jboolean can_back, jboolean can_forward, jstring detail)
{
    (void)cls;
    if (kind < 0 || (size_t)kind >= sizeof k_event_types / sizeof k_event_types[0]) return;
    Ca_WebViewNative *native = native_acquire(handle);
    if (!native) return;
    Event *event = event_new(EVENT_STATE);
    if (event) {
        event->type = k_event_types[kind];
        event->url = jstring_to_utf8(env, url);
        event->title = jstring_to_utf8(env, title);
        event->detail = jstring_to_utf8(env, detail);
        event->progress = progress <= 0 ? 0.0 : progress >= 100 ? 1.0 : progress / 100.0;
        event->loading = loading != JNI_FALSE;
        event->can_go_back = can_back != JNI_FALSE;
        event->can_go_forward = can_forward != JNI_FALSE;
        if ((url && !event->url) || (title && !event->title) || (detail && !event->detail)) {
            event_free(event);
        } else {
            enqueue(native, event);
        }
    }
    native_release(native);
}

/** Java -> native: a message posted by the page's window.causality bridge. */
static void JNICALL native_message(JNIEnv *env, jclass cls, jlong handle, jstring message)
{
    (void)cls;
    Ca_WebViewNative *native = native_acquire(handle);
    if (!native) return;
    Event *event = event_new(EVENT_MESSAGE);
    if (event) {
        event->detail = jstring_to_utf8(env, message);
        if (event->detail) enqueue(native, event);
        else event_free(event);
    }
    native_release(native);
}

/** Java -> native: completion of an evaluateJavascript call. */
static void JNICALL native_eval_result(JNIEnv *env, jclass cls, jlong handle, jlong id,
                                       jboolean ok, jstring text)
{
    (void)cls;
    Ca_WebViewNative *native = native_acquire(handle);
    if (!native) return;
    PendingEval *pending = pending_take(native, (uint64_t)id);
    if (pending) {
        Event *event = event_new(EVENT_EVAL);
        if (event) {
            event->eval_fn = pending->fn;
            event->eval_data = pending->user_data;
            event->ok = ok != JNI_FALSE;
            event->detail = jstring_to_utf8(env, text);
            if (event->detail) enqueue(native, event);
            else event_free(event);
        }
        free(pending);
    }
    native_release(native);
}

/** Java -> native: a main-frame navigation needs a policy answer; returns whether to allow it. */
static jboolean JNICALL native_navigate(JNIEnv *env, jclass cls, jlong handle, jstring url)
{
    (void)cls;
    Ca_WebViewNative *native = native_acquire(handle);
    if (!native) return JNI_TRUE;
    char *text = jstring_to_utf8(env, url);
    bool allowed = true;
    if (text) {
        if (pthread_equal(pthread_self(), native->ui_thread)) {
            if (native_alive(native)) allowed = ca_webview_host_allow_navigation(native->owner, text);
        } else {
            allowed = request_policy(native, text);
        }
    }
    free(text);
    native_release(native);
    return allowed ? JNI_TRUE : JNI_FALSE;
}

/** Java -> native: the host-reserved F9 key changed state in the page. */
static void JNICALL native_key(JNIEnv *env, jclass cls, jlong handle, jint pressed)
{
    (void)env;
    (void)cls;
    Ca_WebViewNative *native = native_acquire(handle);
    if (!native) return;
    Event *event = event_new(EVENT_KEY);
    if (event) {
        event->key_action = pressed ? CA_PRESS : CA_RELEASE;
        enqueue(native, event);
    }
    native_release(native);
}

static const JNINativeMethod k_native_methods[] = {
    { "nativeEvent",
      "(JILjava/lang/String;Ljava/lang/String;IZZZLjava/lang/String;)V", (void *)native_event },
    { "nativeMessage",    "(JLjava/lang/String;)V",  (void *)native_message },
    { "nativeEvalResult", "(JJZLjava/lang/String;)V", (void *)native_eval_result },
    { "nativeNavigate",   "(JLjava/lang/String;)Z",  (void *)native_navigate },
    { "nativeKey",        "(JI)V",                   (void *)native_key },
};

/* ---- Registration ---- */

/**
 * Resolve the companion class and register its native methods. Caller holds g_lock.
 *
 * @param env   JNI environment.
 * @param local Local reference to the class.
 * @return true on success.
 */
static bool register_class(JNIEnv *env, jclass local)
{
    jclass cls = JNI(env, NewGlobalRef, local);
    if (!cls) {
        jni_ok(env);
        return false;
    }
    bool ok = true;
    jmethodID create = JNI(env, GetStaticMethodID, cls, "create",
        "(JLandroid/view/ViewGroup;Ljava/lang/String;Ljava/lang/String;ZZ)"
        "Lorg/causality/CausalityWebView;");
    ok = jni_ok(env) && create != NULL;
    jmethodID resolved[sizeof k_instance_methods / sizeof k_instance_methods[0]] = {0};
    for (size_t i = 0; ok && i < sizeof resolved / sizeof resolved[0]; ++i) {
        resolved[i] = JNI(env, GetMethodID, cls, k_instance_methods[i].name,
                          k_instance_methods[i].signature);
        ok = jni_ok(env) && resolved[i] != NULL;
    }
    if (ok) {
        ok = JNI(env, RegisterNatives, cls, k_native_methods,
                 (jint)(sizeof k_native_methods / sizeof k_native_methods[0])) == JNI_OK;
        jni_ok(env);
    }
    if (!ok) {
        JNI(env, DeleteGlobalRef, cls);
        return false;
    }
    for (size_t i = 0; i < sizeof resolved / sizeof resolved[0]; ++i)
        *k_instance_methods[i].slot = resolved[i];
    g_java.create = create;
    g_java.cls = cls;
    g_registered = true;
    return true;
}

bool ca_android_webview_register(JNIEnv *env)
{
    if (!env) return false;
    pthread_mutex_lock(&g_lock);
    bool ok = g_registered;
    if (!ok) {
        jclass local = JNI(env, FindClass, k_class_name);
        if (local && jni_ok(env)) {
            ok = register_class(env, local);
            JNI(env, DeleteLocalRef, local);
        } else {
            jni_ok(env);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/**
 * Make sure the companion class is registered, trying the calling thread's class loader.
 *
 * @param vm Java VM.
 * @return true when registered.
 */
static bool ensure_registered(JavaVM *vm)
{
    pthread_mutex_lock(&g_lock);
    bool registered = g_registered;
    pthread_mutex_unlock(&g_lock);
    if (registered) return true;
    JNIEnv *env = thread_env(vm);
    return env && ca_android_webview_register(env);
}

/**
 * Check that a string is an absolute URL the web view may load.
 *
 * @param url Candidate address.
 * @return true when it has a scheme, no whitespace or control characters, and is not javascript:.
 */
static bool url_loadable(const char *url)
{
    if (!url) return false;
    const unsigned char *p = (const unsigned char *)url;
    size_t scheme = 0;
    for (; p[scheme]; ++scheme) {
        unsigned char c = p[scheme];
        bool letter = (c | 0x20u) >= 'a' && (c | 0x20u) <= 'z';
        bool extra = scheme > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.');
        if (!letter && !extra) break;
    }
    if (scheme == 0 || p[scheme] != ':') return false;
    if (scheme == 10 && strncasecmp(url, "javascript", 10) == 0) return false;
    for (const unsigned char *q = p + scheme + 1; *q; ++q)
        if (*q <= 0x20u || *q == 0x7Fu) return false;
    return true;
}

/* ---- Pump ---- */

/**
 * Fail every eval still waiting for Java, used when the web content process died.
 *
 * @param native Owning object.
 */
static void fail_pending_evals(Ca_WebViewNative *native)
{
    pthread_mutex_lock(&native->lock);
    PendingEval *list = native->evals;
    native->evals = NULL;
    pthread_mutex_unlock(&native->lock);
    while (list) {
        PendingEval *next = list->next;
        if (native_alive(native))
            ca_webview_host_eval_done(native->owner, list->fn, list->user_data, false,
                                      "web content process terminated");
        free(list);
        list = next;
    }
}

/** Replace a cached string with the event's, taking ownership of it. */
static void adopt_string(char **slot, char **incoming)
{
    free(*slot);
    *slot = *incoming;
    *incoming = NULL;
}

/**
 * Deliver one queued event on the UI thread and free it.
 *
 * @param native Owning object, kept alive by the caller.
 * @param event  Event removed from the queue.
 */
static void dispatch(Ca_WebViewNative *native, Event *event)
{
    if (event->kind == EVENT_POLICY) {
        bool allowed = true;
        pthread_mutex_lock(&native->lock);
        bool skip = event->abandoned;
        pthread_mutex_unlock(&native->lock);
        if (!skip && native_alive(native))
            allowed = ca_webview_host_allow_navigation(native->owner, event->url);
        pthread_mutex_lock(&native->lock);
        if (event->abandoned) {
            pthread_mutex_unlock(&native->lock);
            event_free(event);
            return;
        }
        event->allowed = allowed;
        event->done = true;
        pthread_cond_broadcast(&native->policy_cond);
        pthread_mutex_unlock(&native->lock);
        return;
    }

    if (native_alive(native)) {
        Ca_WebView *owner = native->owner;
        if (event->kind == EVENT_STATE) {
            adopt_string(&native->state_url, &event->url);
            adopt_string(&native->state_title, &event->title);
            native->state_progress = event->progress;
            native->state_loading = event->loading;
            native->state_back = event->can_go_back;
            native->state_forward = event->can_go_forward;
        }
        if (event->kind == EVENT_STATE || event->kind == EVENT_MESSAGE) {
            Ca_WebViewState state = {
                .url            = native->state_url ? native->state_url : "",
                .title          = native->state_title ? native->state_title : "",
                .progress       = native->state_progress,
                .loading        = native->state_loading,
                .can_go_back    = native->state_back,
                .can_go_forward = native->state_forward,
            };
            Ca_WebViewEventType type =
                event->kind == EVENT_MESSAGE ? CA_WEBVIEW_EVENT_MESSAGE : event->type;
            ca_webview_host_event(owner, type, &state, event->detail);
            if (event->kind == EVENT_STATE && type == CA_WEBVIEW_EVENT_PROCESS_TERMINATED)
                fail_pending_evals(native);
        } else if (event->kind == EVENT_EVAL) {
            ca_webview_host_eval_done(owner, event->eval_fn, event->eval_data, event->ok,
                                      event->detail ? event->detail : "null");
        } else if (event->kind == EVENT_KEY) {
            ca_webview_host_key(owner, CA_KEY_F9, event->key_action);
        }
    }
    event_free(event);
}

/**
 * Deliver every queued event of one object.
 *
 * @param native Object to drain, kept alive by the caller.
 */
static void drain(Ca_WebViewNative *native)
{
    for (;;) {
        pthread_mutex_lock(&native->lock);
        Event *event = native->alive ? native->head : NULL;
        if (event) {
            native->head = event->next;
            if (!native->head) native->tail = NULL;
            event->next = NULL;
        }
        pthread_mutex_unlock(&native->lock);
        if (!event) return;
        dispatch(native, event);
    }
}

/* ---- webview_native.h ---- */

bool ca_webview_native_supported(void)
{
    JavaVM *vm = ca_android_java_vm();
    return vm && ensure_registered(vm);
}

void ca_webview_native_pump(Ca_Window *window)
{
    pthread_mutex_lock(&g_lock);
    size_t count = 0;
    for (Ca_WebViewNative *n = g_natives; n; n = n->next)
        if (n->window == window) ++count;
    Ca_WebViewNative **batch = count ? malloc(count * sizeof *batch) : NULL;
    size_t taken = 0;
    if (batch) {
        for (Ca_WebViewNative *n = g_natives; n && taken < count; n = n->next) {
            if (n->window != window) continue;
            ++n->refs;
            batch[taken++] = n;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (count && !batch) {
        ca_instance_wake();
        return;
    }
    for (size_t i = 0; i < taken; ++i) {
        drain(batch[i]);
        native_release(batch[i]);
    }
    free(batch);
}

Ca_WebViewNative *ca_webview_native_create(Ca_Window *window, Ca_WebView *owner,
                                           const Ca_WebViewDesc *desc)
{
    JavaVM *vm = ca_android_java_vm();
    if (!window || !desc || !vm || !ensure_registered(vm)) return NULL;
    jobject host = ca_android_window_host_view(window);
    JNIEnv *env = host ? thread_env(vm) : NULL;
    if (!env) return NULL;
    Ca_WebViewNative *native = native_new(window, owner, vm);
    if (!native) return NULL;

    bool ok = false;
    if (JNI(env, PushLocalFrame, 16) == 0) {
        jstring agent = utf8_to_jstring(env, desc->user_agent);
        jstring init = utf8_to_jstring(env, desc->init_script);
        bool strings_ok = jni_ok(env) && (!desc->user_agent || agent) &&
                          (!desc->init_script || init);
        if (strings_ok) {
            jobject view = JNI(env, CallStaticObjectMethod, g_java.cls, g_java.create,
                               (jlong)native->id, host, agent, init,
                               (jboolean)(desc->transparent ? JNI_TRUE : JNI_FALSE),
                               (jboolean)(desc->devtools ? JNI_TRUE : JNI_FALSE));
            if (jni_ok(env) && view) {
                native->java = JNI(env, NewGlobalRef, view);
                ok = native->java != NULL;
                jni_ok(env);
            }
        }
        JNI(env, PopLocalFrame, NULL);
    } else {
        jni_ok(env);
    }
    if (!ok) {
        native_unlink(native);
        native_release(native);
        return NULL;
    }
    return native;
}

void ca_webview_native_destroy(Ca_WebViewNative *native)
{
    if (!native) return;
    native_unlink(native);

    pthread_mutex_lock(&native->lock);
    native->alive = false;
    Event *event = native->head;
    native->head = NULL;
    native->tail = NULL;
    while (event) {
        Event *next = event->next;
        if (event->kind == EVENT_POLICY && !event->abandoned) {
            event->allowed = true;
            event->done = true;
        } else {
            event_free(event);
        }
        event = next;
    }
    pending_free(native->evals);
    native->evals = NULL;
    pthread_cond_broadcast(&native->policy_cond);
    pthread_mutex_unlock(&native->lock);

    JNIEnv *env = native->java ? thread_env(native->vm) : NULL;
    if (env) {
        JNI(env, CallVoidMethod, native->java, g_java.destroy);
        jni_ok(env);
        JNI(env, DeleteGlobalRef, native->java);
    }
    native->java = NULL;
    native_release(native);
}

/**
 * Get the environment for a call into Java on the UI thread.
 *
 * @param native Object to call.
 * @return The environment, or NULL when native is NULL, destroyed or the thread cannot attach.
 */
static JNIEnv *java_env(const Ca_WebViewNative *native)
{
    return native && native->java ? thread_env(native->vm) : NULL;
}

void ca_webview_native_set_frame(Ca_WebViewNative *native, const Ca_WebViewFrame *frame)
{
    JNIEnv *env = frame ? java_env(native) : NULL;
    if (!env) return;
    float density = ca_android_window_density(native->window);
    if (!(density > 0.0f)) density = 1.0f;
    JNI(env, CallVoidMethod, native->java, g_java.set_frame,
        frame->x, frame->y, frame->w, frame->h,
        frame->clip_x, frame->clip_y, frame->clip_w, frame->clip_h,
        frame->corner_radius, (jboolean)(frame->visible ? JNI_TRUE : JNI_FALSE), density);
    jni_ok(env);
}

bool ca_webview_native_load_url(Ca_WebViewNative *native, const char *url)
{
    JNIEnv *env = url_loadable(url) ? java_env(native) : NULL;
    if (!env || JNI(env, PushLocalFrame, 4) != 0) return false;
    bool ok = false;
    jstring text = utf8_to_jstring(env, url);
    if (jni_ok(env) && text) {
        JNI(env, CallVoidMethod, native->java, g_java.load_url, text);
        ok = jni_ok(env);
    }
    JNI(env, PopLocalFrame, NULL);
    return ok;
}

bool ca_webview_native_load_html(Ca_WebViewNative *native, const char *html, const char *base_url)
{
    JNIEnv *env = html ? java_env(native) : NULL;
    if (!env || JNI(env, PushLocalFrame, 8) != 0) return false;
    bool ok = false;
    jstring source = utf8_to_jstring(env, html);
    jstring base = base_url && *base_url ? utf8_to_jstring(env, base_url) : NULL;
    if (jni_ok(env) && source) {
        JNI(env, CallVoidMethod, native->java, g_java.load_html, source, base);
        ok = jni_ok(env);
    }
    JNI(env, PopLocalFrame, NULL);
    return ok;
}

/**
 * Call a no-argument void method of the companion object.
 *
 * @param native Target object.
 * @param method Method to call.
 */
static void call_void(Ca_WebViewNative *native, jmethodID method)
{
    JNIEnv *env = java_env(native);
    if (!env) return;
    JNI(env, CallVoidMethod, native->java, method);
    jni_ok(env);
}

void ca_webview_native_reload(Ca_WebViewNative *native)
{
    call_void(native, g_java.reload);
}

void ca_webview_native_stop(Ca_WebViewNative *native)
{
    call_void(native, g_java.stop);
}

void ca_webview_native_go_back(Ca_WebViewNative *native)
{
    call_void(native, g_java.go_back);
}

void ca_webview_native_go_forward(Ca_WebViewNative *native)
{
    call_void(native, g_java.go_forward);
}

void ca_webview_native_set_zoom(Ca_WebViewNative *native, double factor)
{
    JNIEnv *env = java_env(native);
    if (!env) return;
    JNI(env, CallVoidMethod, native->java, g_java.set_zoom, (jdouble)factor);
    jni_ok(env);
}

bool ca_webview_native_eval(Ca_WebViewNative *native, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data)
{
    JNIEnv *env = script ? java_env(native) : NULL;
    if (!env || JNI(env, PushLocalFrame, 4) != 0) return false;

    PendingEval *pending = NULL;
    uint64_t id = 0;
    if (on_result) {
        pending = calloc(1, sizeof *pending);
        if (!pending) {
            JNI(env, PopLocalFrame, NULL);
            return false;
        }
        pending->fn = on_result;
        pending->user_data = user_data;
        pthread_mutex_lock(&native->lock);
        id = ++native->next_eval;
        pending->id = id;
        pending->next = native->evals;
        native->evals = pending;
        pthread_mutex_unlock(&native->lock);
    }

    bool ok = false;
    jstring source = utf8_to_jstring(env, script);
    if (jni_ok(env) && source) {
        JNI(env, CallVoidMethod, native->java, g_java.eval, source, (jlong)id);
        ok = jni_ok(env);
    }
    JNI(env, PopLocalFrame, NULL);
    if (!ok && pending) free(pending_take(native, id));
    return ok;
}

void ca_webview_native_focus(Ca_WebViewNative *native)
{
    call_void(native, g_java.focus);
}

void ca_webview_native_blur(Ca_WebViewNative *native)
{
    call_void(native, g_java.blur);
}

bool ca_webview_native_has_focus(const Ca_WebViewNative *native)
{
    JNIEnv *env = java_env(native);
    if (!env) return false;
    jboolean focused = JNI(env, CallBooleanMethod, native->java, g_java.has_focus);
    return jni_ok(env) && focused != JNI_FALSE;
}
