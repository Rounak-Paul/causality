# Android web view backend

`src/platform/webview_android.c` drives `android.webkit.WebView` through the Java companion
`java/org/causality/CausalityWebView.java`. CMake compiles the C file when targeting Android; the
host app must compile the Java file (minSdk 24, compileSdk 26+).

Host app integration:

1. Add `android/java` to the app's Java source set.
2. Call `ca_android_webview_register(env)` from `JNI_OnLoad` (declared in
   `src/platform/android_host.h`). The thread must see app classes, otherwise lookup fails and web
   views stay unsupported.
3. Provide strong definitions of `ca_android_java_vm`, `ca_android_window_host_view` (a global ref
   to a focusable `FrameLayout` holding the render surface) and `ca_android_window_density`.
   Without them the weak defaults make `ca_webview_supported()` false.
4. Forward `Activity.onActivityResult` to `CausalityWebView.handleActivityResult` so `<input
   type=file>` works.
5. Keep the class in release builds: `-keep class org.causality.CausalityWebView { *; }` and
   `-keep class org.causality.CausalityWebView$Bridge { *; }`.

Limitations:

- There are no document-start scripts without androidx.webkit: the bridge and `init_script` are
  evaluated at page start/commit/finish (main frame only) and page scripts may run first.
- Navigation policy is asked for page-initiated main-frame navigations, not for programmatic loads.
  When Causality's UI thread is not the Android main thread the answer is awaited for at most
  200 ms, then the navigation is allowed.
- `evaluateJavascript` cannot report script errors; a throwing script completes with `null`.
- Page zoom uses CSS `zoom` on the document element; sub-frames are not zoomed.
- A dead renderer process is replaced by a fresh blank page; history is lost.
