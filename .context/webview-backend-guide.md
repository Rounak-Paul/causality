# Writing a web view backend

Each platform links exactly one backend that implements `causality/src/platform/webview_native.h`
(valid C and C++; includes only the public `ca_webview.h`). Applications never see it: they call
`ca_webview()` and the right engine is chosen at build time. Reference implementation (complete and
tested): `webview_cocoa.m` (macOS WKWebView). Host side: `src/ui/webview.c`. Public contract and
semantics: `include/ca_webview.h`.

## What the host does for you
- Calls everything on the UI thread. Never call host functions from another thread.
- Resolves layout, ancestor clipping, hidden ancestors, modals and the F9 overlay into a
  `Ca_WebViewFrame` and calls `ca_webview_native_set_frame` only when it changed. Coordinates are
  logical window points, top-left origin (multiply by the display scale for pixel APIs).
  `x,y,w,h` is the full page; `clip_*` is the visible part (a subset); `visible=false` means hide.
  The page must be positioned at its full rect but only the clip rect may be visible
  (use a clipping container whose frame is the clip rect and place the engine view inside it at
  `(x-clip_x, y-clip_y)`). `corner_radius` rounds the clip only when nothing is clipped.
- Applies initial content after `ca_webview_native_create` (calls load_url or load_html).
- Calls `ca_webview_native_blur` when the user presses elsewhere in the window.
- Defers destroying a widget that is removed from inside one of your callbacks, so the native
  object stays valid until the callback returns. Still do not touch your native object after a host
  call unless you hold a reference.

## Required behaviour (match webview_cocoa.m)
- `create`: hidden native view attached to the window's native surface
  (`ca_platform_native_window` / `ca_platform_native_display`), configured from `Ca_WebViewDesc`:
  `user_agent`, `init_script` (every frame, document start), `transparent`, `devtools`.
  Engines that start asynchronously must queue frame/load/eval calls until ready, then replay.
  Return NULL if the engine cannot start.
- JavaScript bridge, injected at document start into the main frame only, frozen, not configurable:
  `window.causality.postMessage(v)` where strings pass through and other values are
  `JSON.stringify`-ed, delivered as `CA_WEBVIEW_EVENT_MESSAGE` with the text as `detail`. Drop
  messages from non-main frames. `ca_webview_post_message` is built on `eval` by the host, so nothing
  extra is needed for the reverse direction.
- Events via `ca_webview_host_event(owner, type, &state, detail)`; always pass a fresh
  `Ca_WebViewState` (url, title, progress 0..1, loading, can_go_back, can_go_forward; strings only
  need to live for the call): LOAD_STARTED, LOAD_FINISHED, LOAD_FAILED (detail = description; do NOT
  report user/engine cancellations), URL_CHANGED, TITLE_CHANGED, PROGRESS_CHANGED, HISTORY_CHANGED,
  MESSAGE, NEW_WINDOW (never open a window; detail = target url), PROCESS_TERMINATED.
- Navigation policy: before each main-frame navigation call `ca_webview_host_allow_navigation`
  synchronously and cancel when it returns false. Sub-frame navigations are always allowed.
- `eval`: run the script in the main frame; on completion call `ca_webview_host_eval_done(owner,
  fn, user_data, ok, text)` where text is the JSON encoding of the completion value (`"null"` for
  undefined/unsupported values) when ok, else a short error message. If `fn` is NULL skip the
  completion. Drop completions that arrive after destroy.
- `load_url`: require an absolute URL with a scheme, return false for malformed input; `file:` URLs
  must be readable (grant read access to the containing directory where the engine needs it).
  `load_html`: optional base URL. Both return false when the call could not be submitted.
- `set_zoom(factor)` page zoom (already clamped to 0.25..5). `reload/stop/go_back/go_forward`.
- Focus: `focus` gives the engine keyboard focus, `blur` returns focus to the Causality window,
  `has_focus` reports it. Hiding the view while it has focus must return focus to the window.
- F9: engines swallow keys. While the view has focus, detect F9 press/release and forward it with
  `ca_webview_host_key(owner, CA_KEY_F9, CA_PRESS or CA_RELEASE)` and do not deliver it to the page.
  (CA_KEY_F9 is declared in causality.h.)
- JS dialogs (alert/confirm/prompt) and file inputs should work; use the engine's defaults when it
  provides them.
- `destroy`: remove the view, cancel pending work, guarantee no host call happens afterwards.
  `pump` is called every UI tick while the window hosts a web view (see header).
- Never leak: every native object created must be released on destroy and on every failure path.
- Report support honestly: `ca_webview_native_supported()` is true only when the engine can run on
  this machine right now (e.g. runtime installed).

## Build integration
`causality/CMakeLists.txt` picks the backend per platform and falls back to
`webview_unsupported.c`, which keeps `ca_webview_supported()` false. Backend files compile only on
their platform; include `webview_native.h` (not `ca_internal.h`) from C++ files.
