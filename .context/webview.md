# Web view widget (ca_webview.h) — 2026-11-24

Platform-native web engine hosted as a child view over a layout node, like Tauri/wry. Not rendered through Vulkan.

## Layout
- Public API: `include/ca_webview.h` (included by `ca_internal.h`). Creation `ca_webview()` lives in `widget.c` (needs static `claim_child`); everything else in `src/ui/webview.c`.
- Backend contract: `src/platform/webview_backend.h`. One implementation linked per platform:
  - macOS: `src/platform/webview_cocoa.m` (WKWebView, ARC, PCH skipped — ARC mismatch; `#undef CA_MALLOC` after AppKit imports, QuartzCore clashes).
  - Everything else (Windows, Linux, iOS): `webview_unsupported.c`; `ca_webview_supported()` is false and `ca_webview()` returns NULL. Adding WebView2 / WebKitGTK = one new file implementing `ca_webview_native_*` plus a CMake switch.
- `Ca_WebView` pool + `webview_count` on `Ca_Window`; widget type `CA_WIDGET_WEBVIEW`. Node release (`node.c`) and window shutdown (`ca_webview_release_all`) destroy the native view.

## Behaviour
- Frame sync: `ca_webview_sync_window` runs at the end of each window's pass in `ca_ui_update`. Resolves node rect, clip (ancestors with overflow != visible), hidden ancestors, and "visible modal elsewhere" → hide. Only calls native when the frame changed (0.01 epsilon).
- macOS view tree: `CaWebViewClip` (flipped, masksToBounds, corner radius only when unclipped) → `CaWebViewPage` (WKWebView). Clip is a sibling above the GLFW content view (inside the window frame view): the content view is layer-hosting (MoltenVK CAMetalLayer) so its subviews never composite. Rects are converted with `convertRect:fromView:`.
- Reconciled rebuilds keep the page; only callbacks are refreshed. `url`/`html` in the desc apply on creation only.
- Events are delivered synchronously from the engine callbacks (UI thread, inside event pump). `dispatch_depth`/`release_pending` defer node removal triggered from inside a callback (signal writes flush builders synchronously). `CaWebViewHost publish:` also holds a precise-lifetime self guard.
- JS bridge: `window.causality.postMessage(v)` (main frame only, string or JSON) → `CA_WEBVIEW_EVENT_MESSAGE`; `ca_webview_post_message` → `causality` CustomEvent on window. Message literal escaper handles quotes, control chars, U+2028/9.
- Focus: pointer press elsewhere (`mouse_click_this_frame` outside the clip) returns first responder to the content view; while the web view is first responder Causality's `focused_node` is cleared. `CaWebViewPage` forwards Cmd+C/X/V/A/Z because GLFW's menu has no Edit menu.
- JS dialogs (alert/confirm/prompt) and `<input type=file>` use sheets on the host window. `window.open`/`target=_blank` → `CA_WEBVIEW_EVENT_NEW_WINDOW`; no window is created.

## Limits
- Native overlay: always above Causality paint within its rect; popups/menus/tooltips cannot cover it (modals hide non-contained web views).
- Pointer input inside the rect never reaches Causality.

## Tests
`tests/ca_webview_tests.c` (`causality_webview_tests`, real window + WKWebView): load, title/URL/progress state, eval JSON/error, bridge round trip with hostile string, policy cancel, history, new-window, load failure (file URL; HTTP failure not verifiable in the agent sandbox — no network), geometry vs node rect, hide/show, zoom/focus guards, rebuild retention, removal and removal from inside the event callback. ASan+UBSan clean; all 15 Causality suites pass.
