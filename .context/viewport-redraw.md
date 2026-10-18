# Viewport redraw scheduling

- `ca_viewport_request_redraw` in `widget.c` marks the image `needs_redraw`, dirties the owning node with `CA_DIRTY_CONTENT`, and requests the next instance frame. NULL is a no-op.
- Image dirtiness alone is insufficient: `renderer.c` skips windows without `needs_render`, and `ui.c` only sets that after painting dirty content. Viewport frame submission is reached through the window compositor.
- Callers should skip unchanged camera updates rather than requesting another frame indefinitely. Flow pan skips zero dx/dy.
- Regression: `test_viewport_redraw_invalidates_content` in `ca_input_capture_tests.c`. All 13 Causality suites pass. Flow runtime harness confirmed repeated viewport renders during a held right-drag, without capture forcing those frames.
