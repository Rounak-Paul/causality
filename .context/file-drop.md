# OS file drop

- Public `ca_instance_set_file_drop_handler(instance, fn, data)`; `Ca_FileDropFn(window, paths, count, screen_x, screen_y, data)`. Instance-wide, so floating dock windows also receive drops.
- `src/platform/window.c`: `glfw_drop_cb` registered on every window; cursor read via `ca_window_cursor_screen_pos` (pointer events pause during OS drags). Runs inside event polling on the main thread; handlers must not destroy windows. Posts an empty event so the loop ticks afterwards.
- State lives in `Ca_Instance.file_drop_fn/file_drop_data` (`ca_internal.h`).
- The former `ca_font_source` API was removed: Flow owns its scene fonts (system catalog + bundled Inter) and no longer borrows Causality's UI font.
