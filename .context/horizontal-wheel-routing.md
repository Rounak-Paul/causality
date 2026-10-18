# Horizontal wheel routing

`causality/src/ui/widget.c` routes standard wheel input by its dominant axis to the nearest enabled overflow ancestor. A vertical gesture falls back to horizontal scrolling when no vertical owner exists, making horizontal-only strips usable with ordinary wheels. Horizontal gestures do not fall back to vertical scrolling. Custom callbacks retain first refusal; stacking/clip hit tests remain unchanged. Both axes clamp to their resolved content range; only vertical offsets publish the existing vertical scroll signal.

Regression: `test_nested_axis_scrolling` in `causality/tests/ca_input_capture_tests.c` verifies a horizontal owner around a vertical child, axis separation, header fallback and endpoint clamping. Input capture suite passes under ASan/UBSan.

`ca_get_scroll_x_signal` observes horizontal wheel, native scrollbar drag, tabbar reveal/scroll and layout-clamp changes; node destruction releases it. `ca_scroll_wheel_x` forwards horizontal input to a named scroller. Both are horizontal counterparts of existing vertical APIs. Input capture regression checks native X offset publication, forwarding and missing IDs.

`layout.c` includes actual child cross-axis extents (plus margins) in scroll content size for both flex directions. This allows a single fixed-size viewport to own both axes while its child overflows in width and height. Flow's timeline uses this to pin the right/bottom scrollbars and reactively align a separate clipped header.
