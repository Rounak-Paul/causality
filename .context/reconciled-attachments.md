# Reconciled context-menu and tooltip attachment

Fixed 2026-10-17 after Flow crashed at `entity_menu + 56` while toggling a rectangle with a group child.

Owner: `causality/src/ui/widget.c`, `ctx_attachment_target(bool parent_fallback)`.

During reconciliation, `parent->child_count` includes retained children not yet emitted by the current builder. Attachments must use the current build frame's `child_cursor - 1`, not `child_count - 1`. The old behavior repeatedly rebound the last retained sibling while leaving earlier menus pointing at freed application callback state. Keyed row reordering exposed the same problem.

Both `ca_context_menu` and `ca_tooltip` now share that target resolution. Tree-node calls retain their header-row convention; context menus retain their empty-container fallback. No public API changes.

Regression: `test_reconciled_context_targets` in `causality/tests/ca_input_capture_tests.c` verifies fresh per-row data after retained rebuild/reorder, tooltip identity, and right-click/menu selection through `ca_widget_input_pass`. The pre-fix implementation fails its menu callback-data assertion.

Validation: original implementation fails the regression; fixed input-capture/style/reactive/storage/dock suites pass under ASan+UBSan. Flow rebuilt successfully and a real-editor harness completed 30 menu-dispatched visibility toggles on the user's rectangle-with-child-group scene copied to scratch, with no crashes or sanitizer findings.
