// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_win32.cpp — Windows web view backend. Hosts a WebView2 controller inside a clipping
   child window of the GLFW window's HWND. */

#ifndef CAUSALITY_HAVE_WEBVIEW2
#error "webview_win32.cpp requires the WebView2 SDK (define CAUSALITY_HAVE_WEBVIEW2)."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "webview_native.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#endif

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kContainerClass[] = L"CausalityWebViewContainer";
constexpr const char *kHtmlDataPrefix = "data:text/html";
constexpr double kStartProgress = 0.1;
constexpr double kFinishProgress = 1.0;

constexpr wchar_t kBridgeScript[] =
    L"(function(){if(window.top!==window)return;"
    L"Object.defineProperty(window,'causality',{value:Object.freeze({"
    L"postMessage:function(m){var s=typeof m==='string'?m:JSON.stringify(m);"
    L"if(typeof s==='string')window.chrome.webview.postMessage(s);}}),configurable:false});})();";

/** Owns a string allocated by WebView2 with CoTaskMemAlloc and frees it on destruction. */
class CoString {
public:
    CoString() = default;
    CoString(const CoString &) = delete;
    CoString &operator=(const CoString &) = delete;
    ~CoString() { CoTaskMemFree(text_); }

    /** Returns the address to receive a new string, freeing any previous one. */
    LPWSTR *put()
    {
        CoTaskMemFree(text_);
        text_ = nullptr;
        return &text_;
    }

    /** Returns the owned string, or NULL when empty. */
    const wchar_t *get() const { return text_; }

private:
    LPWSTR text_ = nullptr;
};

/**
 * Run f and swallow every exception so none crosses a COM or C boundary.
 *
 * @param f Callable to run.
 * @return Always S_OK.
 */
template <class F>
HRESULT contained(F &&f)
{
    try {
        f();
    } catch (...) {
    }
    return S_OK;
}

/**
 * Convert UTF-8 text to UTF-16, rejecting malformed input.
 *
 * @param text NUL-terminated UTF-8 string.
 * @param out  Receives the converted text.
 * @return true when text is non-NULL and valid UTF-8.
 */
bool to_wide(const char *text, std::wstring &out)
{
    out.clear();
    if (!text) return false;
    size_t length = std::strlen(text);
    if (length == 0) return true;
    if (length > INT_MAX) return false;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, nullptr, 0);
    if (count <= 0) return false;
    out.resize((size_t)count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, out.data(),
                               count) == count;
}

/**
 * Convert UTF-16 text to UTF-8.
 *
 * @param text NUL-terminated UTF-16 string, may be NULL.
 * @return The converted text, empty for NULL or on failure.
 */
std::string to_utf8(const wchar_t *text)
{
    if (!text || !*text) return {};
    size_t length = std::wcslen(text);
    if (length > INT_MAX) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out((size_t)count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, (int)length, out.data(), count, nullptr, nullptr);
    return out;
}

/**
 * Reports whether text begins with prefix.
 *
 * @param text   String to test.
 * @param prefix NUL-terminated prefix.
 */
bool starts_with(const std::string &text, const char *prefix)
{
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

/**
 * Format a short failure message that includes the HRESULT.
 *
 * @param what Human readable summary.
 * @param hr   Failure code to append.
 */
std::string describe(const char *what, HRESULT hr)
{
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", (unsigned long)hr);
    return std::string(what) + " (HRESULT " + code + ")";
}

/**
 * Reports whether url starts with a URI scheme of at least two characters. One-letter schemes
 * are rejected so Windows drive paths are not mistaken for URLs.
 *
 * @param url NUL-terminated URL text.
 */
bool has_scheme(const char *url)
{
    size_t length = 0;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!alpha(url[0])) return false;
    while (url[length] &&
           (alpha(url[length]) || (url[length] >= '0' && url[length] <= '9') ||
            url[length] == '+' || url[length] == '-' || url[length] == '.'))
        ++length;
    return length >= 2 && url[length] == ':';
}

/**
 * Describe a navigation failure for LOAD_FAILED.
 *
 * @param status WebView2 error status.
 */
const char *web_error_text(COREWEBVIEW2_WEB_ERROR_STATUS status)
{
    switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT:
        return "The certificate common name is incorrect";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED:
        return "The server certificate has expired";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS:
        return "The client certificate contains errors";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED:
        return "The server certificate has been revoked";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID:
        return "The server certificate is invalid";
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE:
        return "The server is unreachable";
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT:
        return "The connection timed out";
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE:
        return "The server returned an invalid response";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED:
        return "The connection was aborted";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET:
        return "The connection was reset";
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED:
        return "The network is disconnected";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT:
        return "Cannot connect to the server";
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED:
        return "The host name could not be resolved";
    case COREWEBVIEW2_WEB_ERROR_STATUS_REDIRECT_FAILED:
        return "The redirect failed";
    case COREWEBVIEW2_WEB_ERROR_STATUS_UNEXPECTED_ERROR:
        return "An unexpected error occurred";
    default:
        return "The page failed to load";
    }
}

/**
 * Convert a point rectangle to a pixel rectangle with consistent edge rounding.
 *
 * @param x,y,w,h Rectangle in logical points.
 * @param scale   Pixels per logical point.
 */
RECT scaled_rect(float x, float y, float w, float h, double scale)
{
    RECT rect;
    rect.left   = (LONG)std::lround((double)x * scale);
    rect.top    = (LONG)std::lround((double)y * scale);
    rect.right  = (LONG)std::lround(((double)x + (double)w) * scale);
    rect.bottom = (LONG)std::lround(((double)y + (double)h) * scale);
    return rect;
}

/**
 * Child-window pixels per layout point. Causality lays out in GLFW window coordinates, which share
 * one coordinate space with the child windows hosted inside that window at every DPI setting
 * (physical pixels when the process is DPI aware, virtualised units when it is not), so the factor
 * is 1. WebView2 itself follows the monitor's rasterization scale.
 *
 * @param window Window the view is hosted in (unused; kept so a future per-window policy has a home).
 */
double dpi_scale(HWND window)
{
    (void)window;
    return 1.0;
}

/**
 * Initialise COM for the UI thread once, tolerating a thread that already chose another model.
 *
 * @return true when COM is usable on this thread.
 */
bool ensure_com()
{
    static const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

/**
 * Resolve %LOCALAPPDATA%\Causality\WebView2\<exe base name> and create it.
 *
 * @param folder Receives the user data folder.
 * @return true when the folder exists.
 */
bool user_data_folder(std::wstring &folder)
{
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &root))) {
        CoTaskMemFree(root);
        return false;
    }
    std::filesystem::path path(root);
    CoTaskMemFree(root);

    std::wstring module(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, module.data(), (DWORD)module.size());
        if (length == 0) return false;
        if (length < module.size()) {
            module.resize(length);
            break;
        }
        module.resize(module.size() * 2u);
    }
    std::wstring name = std::filesystem::path(module).stem().wstring();
    if (name.empty()) name = L"app";

    path /= L"Causality";
    path /= L"WebView2";
    path /= name;
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (!std::filesystem::is_directory(path, error)) return false;
    folder = path.wstring();
    return true;
}

/**
 * Process-wide WebView2 environment, created lazily. Every web view shares it because a user
 * data folder can only be opened with one configuration per process.
 */
class Environment {
public:
    using Waiter = std::function<void(HRESULT, ICoreWebView2Environment *)>;

    /** Returns the process singleton. */
    static Environment &shared()
    {
        static Environment environment;
        return environment;
    }

    /** Returns the created environment, or an empty pointer while it does not exist yet. */
    ComPtr<ICoreWebView2Environment> current() const { return instance_; }

    /**
     * Queue waiter until the environment exists, starting creation when needed.
     *
     * @param waiter Called on the UI thread with the creation result.
     * @return Failure when creation could not even start; waiter is then not queued.
     */
    HRESULT request(Waiter waiter)
    {
        waiters_.push_back(std::move(waiter));
        if (creating_) return S_OK;
        HRESULT hr = begin();
        if (FAILED(hr)) waiters_.pop_back();
        return hr;
    }

private:
    Environment() = default;

    /** Start asynchronous environment creation. */
    HRESULT begin()
    {
        std::wstring folder;
        if (!user_data_folder(folder)) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        auto handler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                return contained([&] { Environment::shared().complete(result, environment); });
            });
        creating_ = true;
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, folder.c_str(), nullptr,
                                                              handler.Get());
        if (FAILED(hr)) creating_ = false;
        return hr;
    }

    /**
     * Store the created environment and release every waiter.
     *
     * @param result      Creation result.
     * @param environment The environment when result succeeded.
     */
    void complete(HRESULT result, ICoreWebView2Environment *environment)
    {
        creating_ = false;
        if (SUCCEEDED(result) && !environment) result = E_POINTER;
        if (SUCCEEDED(result)) instance_ = environment;
        std::vector<Waiter> waiters = std::move(waiters_);
        waiters_.clear();
        for (Waiter &waiter : waiters) {
            try {
                waiter(result, instance_.Get());
            } catch (...) {
            }
        }
    }

    ComPtr<ICoreWebView2Environment> instance_;
    std::vector<Waiter> waiters_;
    bool creating_ = false;
};

/**
 * Register the clipping container window class once.
 *
 * @return true when the class is available.
 */
bool register_container_class()
{
    static const bool registered = [] {
        WNDCLASSEXW cls = {};
        cls.cbSize        = sizeof cls;
        cls.lpfnWndProc   = DefWindowProcW;
        cls.hInstance     = GetModuleHandleW(nullptr);
        cls.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = kContainerClass;
        return RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

/**
 * Find where a <base> element can be inserted without disturbing the doctype.
 *
 * @param html Document source.
 * @return Offset just after the head tag, else after the html tag, else after a leading doctype.
 */
size_t base_insertion_point(const std::wstring &html)
{
    auto after_tag = [&](const wchar_t *name) -> size_t {
        const size_t length = std::wcslen(name);
        for (size_t i = html.find(L'<'); i != std::wstring::npos; i = html.find(L'<', i + 1)) {
            if (_wcsnicmp(html.c_str() + i + 1, name, length) != 0) continue;
            wchar_t next = html[i + 1 + length];
            if (next != L'>' && next != L'/' && !iswspace(next)) continue;
            size_t close = html.find(L'>', i);
            if (close != std::wstring::npos) return close + 1;
        }
        return std::wstring::npos;
    };
    size_t point = after_tag(L"head");
    if (point == std::wstring::npos) point = after_tag(L"html");
    if (point != std::wstring::npos) return point;
    size_t first = html.find_first_not_of(L" \t\r\n");
    if (first != std::wstring::npos && html.compare(first, 2, L"<!") == 0) {
        size_t close = html.find(L'>', first);
        if (close != std::wstring::npos) return close + 1;
    }
    return 0;
}

/**
 * Add a <base href> to an HTML document so relative links resolve against base.
 *
 * @param html Document source.
 * @param base Base URL.
 * @return The document with the base element inserted.
 */
std::wstring with_base(std::wstring html, const std::wstring &base)
{
    std::wstring escaped;
    for (wchar_t c : base) {
        switch (c) {
        case L'&': escaped += L"&amp;"; break;
        case L'"': escaped += L"&quot;"; break;
        case L'<': escaped += L"&lt;"; break;
        case L'>': escaped += L"&gt;"; break;
        default:   escaped += c;
        }
    }
    html.insert(base_insertion_point(html), L"<base href=\"" + escaped + L"\">");
    return html;
}

} // namespace

/** Native state of one Ca_WebView: window container, WebView2 controller and replay state. */
struct Ca_WebViewNative {
    using Action   = std::function<HRESULT(ICoreWebView2 *)>;
    using Fallback = std::function<void(HRESULT)>;

    /** A command submitted before the engine was ready. */
    struct Queued {
        Action   action;
        Fallback fallback;
    };

    std::shared_ptr<Ca_WebViewNative> self;
    Ca_WebView *owner     = nullptr;
    HWND        parent    = nullptr;
    HWND        container = nullptr;
    bool        alive     = true;
    bool        ready     = false;
    bool        failed    = false;

    std::wstring              user_agent;
    bool                      transparent = false;
    bool                      devtools    = false;
    std::vector<std::wstring> scripts;

    ComPtr<ICoreWebView2Controller>     controller;
    ComPtr<ICoreWebView2>               webview;
    std::vector<std::function<void()>>  unsubscribers;
    std::vector<Queued>                 queue;

    Ca_WebViewFrame frame{};
    bool            frame_valid     = false;
    double          zoom            = 1.0;
    bool            zoom_set        = false;
    bool            focus_requested = false;
    bool            has_focus       = false;

    std::string url;
    std::string title;
    std::string html_alias;
    std::string pending_html_alias;
    double      progress       = 0.0;
    bool        loading        = false;
    bool        can_go_back    = false;
    bool        can_go_forward = false;
    UINT64      navigation_id  = 0;

    /** Returns a weak reference that callbacks use to detect destruction. */
    std::weak_ptr<Ca_WebViewNative> handle() const { return self; }

    /** Reports whether keyboard focus is inside this view. */
    bool owns_focus() const
    {
        if (has_focus) return true;
        HWND focused = GetFocus();
        return container && focused && (focused == container || IsChild(container, focused));
    }

    /**
     * Report an event with a fresh state snapshot.
     *
     * @param type   Event kind.
     * @param detail Optional detail text, may be NULL.
     */
    void publish(Ca_WebViewEventType type, const char *detail = nullptr)
    {
        if (!alive || !owner) return;
        Ca_WebViewState state = {};
        state.url            = url.c_str();
        state.title          = title.c_str();
        state.progress       = progress;
        state.loading        = loading;
        state.can_go_back    = can_go_back;
        state.can_go_forward = can_go_forward;
        ca_webview_host_event(owner, type, &state, detail);
    }

    /** Refresh the cached address from the engine, hiding the internal data URL of HTML loads. */
    void refresh_source()
    {
        CoString source;
        if (!webview || FAILED(webview->get_Source(source.put()))) return;
        std::string raw = to_utf8(source.get());
        url = (!html_alias.empty() && starts_with(raw, kHtmlDataPrefix)) ? html_alias : std::move(raw);
    }

    /** Refresh the cached document title from the engine. */
    void refresh_title()
    {
        CoString text;
        if (webview && SUCCEEDED(webview->get_DocumentTitle(text.put()))) title = to_utf8(text.get());
    }

    /** Refresh the cached back and forward availability from the engine. */
    void refresh_history()
    {
        BOOL back = FALSE, forward = FALSE;
        if (!webview) return;
        webview->get_CanGoBack(&back);
        webview->get_CanGoForward(&forward);
        can_go_back    = back != FALSE;
        can_go_forward = forward != FALSE;
    }

    /**
     * Subscribe a member function to an engine event; the registration is undone on release.
     *
     * @param source Interface that owns the event.
     * @param add    The interface's add_* method.
     * @param remove The interface's matching remove_* method.
     * @param method Member invoked on the UI thread while this object is alive.
     * @return The add_* result.
     */
    template <class Source, class Handler, class Args>
    HRESULT listen(Source *source,
                   HRESULT (STDMETHODCALLTYPE Source::*add)(Handler *, EventRegistrationToken *),
                   HRESULT (STDMETHODCALLTYPE Source::*remove)(EventRegistrationToken),
                   void (Ca_WebViewNative::*method)(Args *))
    {
        std::weak_ptr<Ca_WebViewNative> weak = handle();
        auto handler = Callback<Handler>([weak, method](Source *, Args *args) -> HRESULT {
            std::shared_ptr<Ca_WebViewNative> strong = weak.lock();
            if (!strong || !strong->alive) return S_OK;
            return contained([&] { ((*strong).*method)(args); });
        });
        EventRegistrationToken token = {};
        HRESULT hr = (source->*add)(handler.Get(), &token);
        if (FAILED(hr)) return hr;
        ComPtr<Source> keep(source);
        unsubscribers.push_back([keep, remove, token] { (keep.Get()->*remove)(token); });
        return hr;
    }

    /** Subscribe to every event the backend translates. */
    HRESULT subscribe()
    {
        HRESULT hr = S_OK;
        ICoreWebView2 *view = webview.Get();
        ICoreWebView2Controller *host = controller.Get();
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_NavigationStarting,
                                       &ICoreWebView2::remove_NavigationStarting,
                                       &Ca_WebViewNative::on_navigation_starting);
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_NavigationCompleted,
                                       &ICoreWebView2::remove_NavigationCompleted,
                                       &Ca_WebViewNative::on_navigation_completed);
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_SourceChanged,
                                       &ICoreWebView2::remove_SourceChanged,
                                       &Ca_WebViewNative::on_source_changed);
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_DocumentTitleChanged,
                                       &ICoreWebView2::remove_DocumentTitleChanged,
                                       &Ca_WebViewNative::on_title_changed);
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_HistoryChanged,
                                       &ICoreWebView2::remove_HistoryChanged,
                                       &Ca_WebViewNative::on_history_changed);
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_NewWindowRequested,
                                       &ICoreWebView2::remove_NewWindowRequested,
                                       &Ca_WebViewNative::on_new_window);
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_ProcessFailed,
                                       &ICoreWebView2::remove_ProcessFailed,
                                       &Ca_WebViewNative::on_process_failed);
        if (SUCCEEDED(hr)) hr = listen(view, &ICoreWebView2::add_WebMessageReceived,
                                       &ICoreWebView2::remove_WebMessageReceived,
                                       &Ca_WebViewNative::on_message);
        if (SUCCEEDED(hr)) hr = listen(host, &ICoreWebView2Controller::add_GotFocus,
                                       &ICoreWebView2Controller::remove_GotFocus,
                                       &Ca_WebViewNative::on_got_focus);
        if (SUCCEEDED(hr)) hr = listen(host, &ICoreWebView2Controller::add_LostFocus,
                                       &ICoreWebView2Controller::remove_LostFocus,
                                       &Ca_WebViewNative::on_lost_focus);
        if (SUCCEEDED(hr)) hr = listen(host, &ICoreWebView2Controller::add_AcceleratorKeyPressed,
                                       &ICoreWebView2Controller::remove_AcceleratorKeyPressed,
                                       &Ca_WebViewNative::on_accelerator);
        return hr;
    }

    /** Apply settings from the creation description to the engine. */
    HRESULT configure()
    {
        ComPtr<ICoreWebView2Settings> settings;
        HRESULT hr = webview->get_Settings(&settings);
        if (FAILED(hr)) return hr;
        hr = settings->put_AreDevToolsEnabled(devtools ? TRUE : FALSE);
        if (FAILED(hr)) return hr;
        if (!user_agent.empty()) {
            ComPtr<ICoreWebView2Settings2> extended;
            if (SUCCEEDED(settings.As(&extended))) extended->put_UserAgent(user_agent.c_str());
        }
        if (transparent) {
            ComPtr<ICoreWebView2Controller2> extended;
            COREWEBVIEW2_COLOR clear = {0, 0, 0, 0};
            if (SUCCEEDED(controller.As(&extended))) extended->put_DefaultBackgroundColor(clear);
        }
        return S_OK;
    }

    /**
     * Create the controller inside the container.
     *
     * @param environment Shared WebView2 environment.
     * @return Failure when creation could not start.
     */
    HRESULT create_controller(ICoreWebView2Environment *environment)
    {
        std::weak_ptr<Ca_WebViewNative> weak = handle();
        auto handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Controller *created) -> HRESULT {
                return contained([&] {
                    std::shared_ptr<Ca_WebViewNative> strong = weak.lock();
                    if (!strong || !strong->alive) {
                        if (created) created->Close();
                        return;
                    }
                    strong->on_controller(result, created);
                });
            });
        return environment->CreateCoreWebView2Controller(container, handler.Get());
    }

    /**
     * Begin asynchronous startup.
     *
     * @return Failure when startup could not begin.
     */
    HRESULT start()
    {
        Environment &shared = Environment::shared();
        ComPtr<ICoreWebView2Environment> environment = shared.current();
        if (environment) return create_controller(environment.Get());
        std::weak_ptr<Ca_WebViewNative> weak = handle();
        return shared.request([weak](HRESULT result, ICoreWebView2Environment *created) {
            std::shared_ptr<Ca_WebViewNative> strong = weak.lock();
            if (!strong || !strong->alive) return;
            if (FAILED(result)) {
                strong->fail(result, "WebView2 environment could not be created");
                return;
            }
            HRESULT hr = strong->create_controller(created);
            if (FAILED(hr)) strong->fail(hr, "WebView2 controller could not be created");
        });
    }

    /**
     * Take ownership of the new controller, configure it and start installing scripts.
     *
     * @param result  Creation result.
     * @param created The controller when result succeeded.
     */
    void on_controller(HRESULT result, ICoreWebView2Controller *created)
    {
        if (FAILED(result) || !created) {
            fail(FAILED(result) ? result : E_POINTER, "WebView2 controller could not be created");
            return;
        }
        controller = created;
        HRESULT hr = controller->get_CoreWebView2(&webview);
        if (SUCCEEDED(hr)) hr = configure();
        if (SUCCEEDED(hr)) hr = subscribe();
        if (FAILED(hr)) {
            fail(hr, "WebView2 could not be configured");
            return;
        }
        install_script(0);
    }

    /**
     * Register document-start scripts in order, then become ready.
     *
     * @param index First script still to register.
     */
    void install_script(size_t index)
    {
        if (!alive || failed) return;
        if (index >= scripts.size()) {
            become_ready();
            return;
        }
        std::weak_ptr<Ca_WebViewNative> weak = handle();
        auto handler = Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [weak, index](HRESULT result, PCWSTR) -> HRESULT {
                return contained([&] {
                    std::shared_ptr<Ca_WebViewNative> strong = weak.lock();
                    if (!strong || !strong->alive) return;
                    if (FAILED(result)) strong->fail(result, "WebView2 script could not be installed");
                    else strong->install_script(index + 1u);
                });
            });
        HRESULT hr = webview->AddScriptToExecuteOnDocumentCreated(scripts[index].c_str(), handler.Get());
        if (FAILED(hr)) fail(hr, "WebView2 script could not be installed");
    }

    /** Mark the engine usable and replay the frame, zoom, queued commands and focus request. */
    void become_ready()
    {
        ready = true;
        apply_frame();
        if (zoom_set) controller->put_ZoomFactor(zoom);
        std::vector<Queued> pending = std::move(queue);
        queue.clear();
        for (Queued &entry : pending) {
            if (!alive || !ready) break;
            HRESULT hr = entry.action(webview.Get());
            if (FAILED(hr) && entry.fallback) entry.fallback(hr);
        }
        if (focus_requested) {
            focus_requested = false;
            move_focus_in();
        }
    }

    /**
     * Abandon the engine after a startup or runtime failure and tell the host.
     *
     * @param hr   Failure code.
     * @param what Human readable summary.
     */
    void fail(HRESULT hr, const char *what)
    {
        if (!alive || failed) return;
        failed = true;
        ready = false;
        loading = false;
        release_engine();
        std::vector<Queued> pending = std::move(queue);
        queue.clear();
        for (Queued &entry : pending)
            if (entry.fallback) entry.fallback(E_ABORT);
        publish(CA_WEBVIEW_EVENT_LOAD_FAILED, describe(what, hr).c_str());
    }

    /**
     * Run an action now when the engine is ready, otherwise queue it for replay.
     *
     * @param action   Work to perform against the engine.
     * @param fallback Called when a queued action fails or the engine never starts; may be empty.
     * @return false when the engine is unusable or an immediate action failed.
     */
    bool submit(Action action, Fallback fallback = nullptr)
    {
        if (!alive || failed) return false;
        if (ready) return SUCCEEDED(action(webview.Get()));
        queue.push_back({std::move(action), std::move(fallback)});
        return true;
    }

    /** Returns a fallback that reports a navigation that could not be submitted. */
    Fallback navigation_failure()
    {
        return [this](HRESULT hr) {
            if (!failed) publish(CA_WEBVIEW_EVENT_LOAD_FAILED, describe("Navigation could not start", hr).c_str());
        };
    }

    /**
     * Run a script and report its JSON result to the host.
     *
     * @param source    Script text.
     * @param on_result Optional completion.
     * @param user_data Passed to on_result.
     * @return false when the script could not be submitted.
     */
    bool evaluate(std::wstring source, Ca_WebViewEvalFn on_result, void *user_data)
    {
        std::weak_ptr<Ca_WebViewNative> weak = handle();
        Action action = [weak, source = std::move(source), on_result, user_data](ICoreWebView2 *view) -> HRESULT {
            ComPtr<ICoreWebView2ExecuteScriptCompletedHandler> handler;
            if (on_result) {
                handler = Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                    [weak, on_result, user_data](HRESULT result, PCWSTR json) -> HRESULT {
                        return contained([&] {
                            std::shared_ptr<Ca_WebViewNative> strong = weak.lock();
                            if (!strong || !strong->alive) return;
                            bool ok = SUCCEEDED(result);
                            std::string text = !ok ? describe("Script failed", result)
                                               : (json && *json ? to_utf8(json) : std::string("null"));
                            ca_webview_host_eval_done(strong->owner, on_result, user_data, ok, text.c_str());
                        });
                    });
            }
            return view->ExecuteScript(source.c_str(), handler.Get());
        };
        Fallback fallback = [this, on_result, user_data](HRESULT hr) {
            if (on_result && alive)
                ca_webview_host_eval_done(owner, on_result, user_data, false,
                                          describe("Script could not run", hr).c_str());
        };
        return submit(std::move(action), std::move(fallback));
    }

    /** Position, clip, round and show or hide the container and page from the stored frame. */
    void apply_frame()
    {
        if (!ready || !frame_valid || !container) return;
        if (!frame.visible) {
            hide();
            return;
        }
        const double scale = dpi_scale(parent);
        const RECT clip = scaled_rect(frame.clip_x, frame.clip_y, frame.clip_w, frame.clip_h, scale);
        const RECT page = scaled_rect(frame.x, frame.y, frame.w, frame.h, scale);
        const int width  = std::max<LONG>(clip.right - clip.left, 1);
        const int height = std::max<LONG>(clip.bottom - clip.top, 1);

        SetWindowPos(container, HWND_TOP, clip.left, clip.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        round_corners(EqualRect(&clip, &page) != FALSE, width, height, scale);

        RECT bounds = {page.left - clip.left, page.top - clip.top, page.right - clip.left,
                       page.bottom - clip.top};
        controller->put_Bounds(bounds);
        controller->put_IsVisible(TRUE);
        controller->NotifyParentWindowPositionChanged();
    }

    /**
     * Round the container only when nothing is clipped, otherwise clear its region.
     *
     * @param unclipped Whether the clip equals the page rectangle.
     * @param width,height Container size in pixels.
     * @param scale     Pixels per logical point.
     */
    void round_corners(bool unclipped, int width, int height, double scale)
    {
        if (!unclipped || frame.corner_radius <= 0.5f) {
            SetWindowRgn(container, nullptr, TRUE);
            return;
        }
        const int diameter = (int)std::lround((double)frame.corner_radius * scale * 2.0);
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
        if (!region) return;
        if (!SetWindowRgn(container, region, TRUE)) DeleteObject(region);
    }

    /** Hide the container and page, returning focus to the host window first. */
    void hide()
    {
        if (owns_focus()) {
            has_focus = false;
            SetFocus(parent);
        }
        ShowWindow(container, SW_HIDE);
        controller->put_IsVisible(FALSE);
    }

    /** Give the page keyboard focus when it is on screen. */
    void move_focus_in()
    {
        if (ready && frame_valid && frame.visible)
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

    /** Remove event registrations and close the controller. */
    void release_engine()
    {
        for (auto it = unsubscribers.rbegin(); it != unsubscribers.rend(); ++it) (*it)();
        unsubscribers.clear();
        if (controller) controller->Close();
        webview.Reset();
        controller.Reset();
        has_focus = false;
    }

    /** Destroy everything, return focus to the host window and guarantee no later host call. */
    void teardown()
    {
        if (!alive) return;
        std::shared_ptr<Ca_WebViewNative> keep = std::move(self);
        self.reset();
        const bool focused = owns_focus();
        alive = false;
        owner = nullptr;
        queue.clear();
        release_engine();
        if (container) {
            if (focused && IsWindow(parent)) SetFocus(parent);
            if (IsWindow(container)) DestroyWindow(container);
            container = nullptr;
        }
    }

    /**
     * Apply the host's navigation policy and start tracking the load.
     *
     * @param args Navigation details.
     */
    void on_navigation_starting(ICoreWebView2NavigationStartingEventArgs *args)
    {
        CoString uri;
        if (FAILED(args->get_Uri(uri.put()))) return;
        const std::string target = to_utf8(uri.get());
        const bool html = !pending_html_alias.empty() && starts_with(target, kHtmlDataPrefix);
        const std::string &policy_url = html ? pending_html_alias : target;
        const bool allowed = ca_webview_host_allow_navigation(owner, policy_url.c_str());
        if (!alive) return;
        if (!allowed) {
            args->put_Cancel(TRUE);
            if (html) pending_html_alias.clear();
            return;
        }
        if (html) {
            html_alias = std::move(pending_html_alias);
            pending_html_alias.clear();
        } else {
            html_alias.clear();
        }
        UINT64 id = 0;
        args->get_NavigationId(&id);
        navigation_id = id;
        loading = true;
        progress = kStartProgress;
        publish(CA_WEBVIEW_EVENT_LOAD_STARTED);
        publish(CA_WEBVIEW_EVENT_PROGRESS_CHANGED);
    }

    /**
     * Report the end of the tracked load as finished, failed or silently cancelled.
     *
     * @param args Completion details.
     */
    void on_navigation_completed(ICoreWebView2NavigationCompletedEventArgs *args)
    {
        UINT64 id = 0;
        BOOL success = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        args->get_NavigationId(&id);
        args->get_IsSuccess(&success);
        args->get_WebErrorStatus(&status);
        if (!loading || id != navigation_id) return;
        loading = false;
        progress = kFinishProgress;
        refresh_source();
        refresh_title();
        refresh_history();
        publish(CA_WEBVIEW_EVENT_PROGRESS_CHANGED);
        if (success) {
            publish(CA_WEBVIEW_EVENT_LOAD_FINISHED);
        } else if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
            publish(CA_WEBVIEW_EVENT_LOAD_FAILED, web_error_text(status));
        }
    }

    /** Report an address change. */
    void on_source_changed(ICoreWebView2SourceChangedEventArgs *)
    {
        refresh_source();
        publish(CA_WEBVIEW_EVENT_URL_CHANGED);
    }

    /** Report a document title change. */
    void on_title_changed(IUnknown *)
    {
        refresh_title();
        publish(CA_WEBVIEW_EVENT_TITLE_CHANGED);
    }

    /** Report a change in back or forward availability. */
    void on_history_changed(IUnknown *)
    {
        refresh_history();
        publish(CA_WEBVIEW_EVENT_HISTORY_CHANGED);
    }

    /**
     * Refuse a new window and report the requested address.
     *
     * @param args Request details.
     */
    void on_new_window(ICoreWebView2NewWindowRequestedEventArgs *args)
    {
        args->put_Handled(TRUE);
        CoString uri;
        if (FAILED(args->get_Uri(uri.put()))) return;
        publish(CA_WEBVIEW_EVENT_NEW_WINDOW, to_utf8(uri.get()).c_str());
    }

    /**
     * Report a dead or hung browser or renderer.
     *
     * @param args Failure details.
     */
    void on_process_failed(ICoreWebView2ProcessFailedEventArgs *args)
    {
        COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
        if (FAILED(args->get_ProcessFailedKind(&kind))) return;
        if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED &&
            kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED &&
            kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
            return;
        loading = false;
        publish(CA_WEBVIEW_EVENT_PROCESS_TERMINATED);
    }

    /**
     * Deliver a string message posted through the bridge.
     *
     * @param args Message details.
     */
    void on_message(ICoreWebView2WebMessageReceivedEventArgs *args)
    {
        CoString text;
        if (FAILED(args->TryGetWebMessageAsString(text.put())) || !text.get()) return;
        publish(CA_WEBVIEW_EVENT_MESSAGE, to_utf8(text.get()).c_str());
    }

    /** Track that the page gained keyboard focus. */
    void on_got_focus(IUnknown *) { has_focus = true; }

    /** Track that the page lost keyboard focus. */
    void on_lost_focus(IUnknown *) { has_focus = false; }

    /**
     * Forward F9 to the host window and keep it from the page.
     *
     * @param args Key details.
     */
    void on_accelerator(ICoreWebView2AcceleratorKeyPressedEventArgs *args)
    {
        UINT key = 0;
        COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
        if (FAILED(args->get_VirtualKey(&key)) || key != VK_F9) return;
        if (FAILED(args->get_KeyEventKind(&kind))) return;
        const bool press = kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                           kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
        args->put_Handled(TRUE);
        ca_webview_host_key(owner, CA_KEY_F9, press ? CA_PRESS : CA_RELEASE);
    }
};

extern "C" {

bool ca_webview_native_supported(void)
{
    LPWSTR version = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    bool available = SUCCEEDED(hr) && version && *version;
    CoTaskMemFree(version);
    return available;
}

void ca_webview_native_pump(Ca_Window *window)
{
    (void)window;
}

Ca_WebViewNative *ca_webview_native_create(Ca_Window *window, Ca_WebView *owner,
                                           const Ca_WebViewDesc *desc)
{
    if (!window || !owner || !desc || !ca_webview_native_supported() || !ensure_com() ||
        !register_container_class())
        return nullptr;
    HWND parent = static_cast<HWND>(ca_platform_native_window(window));
    if (!parent || !IsWindow(parent)) return nullptr;

    try {
        auto native = std::make_shared<Ca_WebViewNative>();
        native->self        = native;
        native->owner       = owner;
        native->parent      = parent;
        native->transparent = desc->transparent;
        native->devtools    = desc->devtools;
        native->scripts.emplace_back(kBridgeScript);
        if (desc->user_agent && !to_wide(desc->user_agent, native->user_agent)) {
            native->teardown();
            return nullptr;
        }
        std::wstring init;
        if (desc->init_script) {
            if (!to_wide(desc->init_script, init)) {
                native->teardown();
                return nullptr;
            }
            native->scripts.push_back(std::move(init));
        }

        native->container = CreateWindowExW(
            0, kContainerClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 1, 1,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!native->container || FAILED(native->start())) {
            native->teardown();
            return nullptr;
        }
        return native.get();
    } catch (...) {
        return nullptr;
    }
}

void ca_webview_native_destroy(Ca_WebViewNative *native)
{
    if (native) native->teardown();
}

void ca_webview_native_set_frame(Ca_WebViewNative *native, const Ca_WebViewFrame *frame)
{
    if (!native || !native->alive || !frame) return;
    native->frame       = *frame;
    native->frame_valid = true;
    native->apply_frame();
}

bool ca_webview_native_load_url(Ca_WebViewNative *native, const char *url)
{
    if (!native || !native->alive || !url || !has_scheme(url)) return false;
    try {
        std::wstring target;
        if (!to_wide(url, target)) return false;
        return native->submit(
            [target = std::move(target)](ICoreWebView2 *view) { return view->Navigate(target.c_str()); },
            native->navigation_failure());
    } catch (...) {
        return false;
    }
}

bool ca_webview_native_load_html(Ca_WebViewNative *native, const char *html, const char *base_url)
{
    if (!native || !native->alive || !html) return false;
    try {
        std::wstring document;
        std::wstring base;
        if (!to_wide(html, document)) return false;
        if (base_url && *base_url) {
            if (!to_wide(base_url, base)) return false;
            document = with_base(std::move(document), base);
        }
        std::string alias = (base_url && *base_url) ? base_url : "about:blank";
        Ca_WebViewNative *self = native;
        return native->submit(
            [self, document = std::move(document), alias = std::move(alias)](ICoreWebView2 *view) {
                self->pending_html_alias = alias;
                HRESULT hr = view->NavigateToString(document.c_str());
                if (FAILED(hr)) self->pending_html_alias.clear();
                return hr;
            },
            native->navigation_failure());
    } catch (...) {
        return false;
    }
}

void ca_webview_native_reload(Ca_WebViewNative *native)
{
    if (!native) return;
    try {
        native->submit([](ICoreWebView2 *view) { return view->Reload(); });
    } catch (...) {
    }
}

void ca_webview_native_stop(Ca_WebViewNative *native)
{
    if (!native) return;
    try {
        native->submit([](ICoreWebView2 *view) { return view->Stop(); });
    } catch (...) {
    }
}

void ca_webview_native_go_back(Ca_WebViewNative *native)
{
    if (!native) return;
    try {
        native->submit([](ICoreWebView2 *view) { return view->GoBack(); });
    } catch (...) {
    }
}

void ca_webview_native_go_forward(Ca_WebViewNative *native)
{
    if (!native) return;
    try {
        native->submit([](ICoreWebView2 *view) { return view->GoForward(); });
    } catch (...) {
    }
}

void ca_webview_native_set_zoom(Ca_WebViewNative *native, double factor)
{
    if (!native || !native->alive || native->failed) return;
    native->zoom     = factor;
    native->zoom_set = true;
    if (native->ready) native->controller->put_ZoomFactor(factor);
}

bool ca_webview_native_eval(Ca_WebViewNative *native, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data)
{
    if (!native || !native->alive || !script) return false;
    try {
        std::wstring source;
        if (!to_wide(script, source)) return false;
        return native->evaluate(std::move(source), on_result, user_data);
    } catch (...) {
        return false;
    }
}

void ca_webview_native_focus(Ca_WebViewNative *native)
{
    if (!native || !native->alive || native->failed) return;
    if (native->ready) native->move_focus_in();
    else native->focus_requested = true;
}

void ca_webview_native_blur(Ca_WebViewNative *native)
{
    if (!native || !native->alive || !native->owns_focus()) return;
    native->focus_requested = false;
    native->has_focus = false;
    if (IsWindow(native->parent)) SetFocus(native->parent);
}

bool ca_webview_native_has_focus(const Ca_WebViewNative *native)
{
    return native && native->alive && native->has_focus;
}

} // extern "C"
