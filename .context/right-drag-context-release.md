# Right-button drags and release context menus

- Public `Ca_DivDesc.drag_buttons`: bit 0 left, bit 1 right; zero keeps default left-only behavior. `Ca_DragEvent.button` identifies the captured button. No changes to other widget descriptor defaults.
- `widget.c` owns shared drag hit testing/dispatch. Left callbacks start on press. Right capture waits for movement beyond 4 logical pixels scaled by UI scale, then dispatches start with the original press coordinates and continues/ends using the captured button.
- All Causality context menus open on right-button release. Right gestures record whether the threshold was ever exceeded, so dragging out and back never opens a menu. The released receiver must contain both press and release points and remain enabled/visible. Menu targeting is resolved at release; no borrowed menu pointer survives the gesture.
- `window.c` records right-button press coordinates and a per-tick flag, covering press/release in the same event-poll batch. Generic right captures never create drag previews. Existing left drag transfer previews remain unchanged.
- Tests: `test_right_drag_context_release` and updated `test_reconciled_context_targets` in `ca_input_capture_tests.c`; all 13 Causality suites pass. Flow enables both bits on the Scene stage, using right drag for camera pan and left for editing.
