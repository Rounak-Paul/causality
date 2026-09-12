# Text glyph clipping

`causality/src/ui/paint.c::text_clip_for_node` must preserve glyph ink outside
advance-based label bounds when overflow is visible. Only explicit hidden or
scrolling axes add a self clip; clipping ancestors still bound the result.
Return initialized clip state, including radius, and do not expand ancestor
bounds with fixed pixel padding. Input text explicitly clips to its control.

`causality/tests/ca_text_clip_tests.c` uses bundled FreeType glyph metrics across
8–32 px and five scales to reproduce `w` overhang, plus explicit overflow and
rounded-ancestor/empty-clip checks. The original helper fails the regression;
the replacement passes. Sol's full build and 17 CTest tests pass (2026-09-12).
Live application screenshot verification was unavailable through computer-use.
