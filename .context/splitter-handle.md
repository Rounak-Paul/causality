# Causality splitter handle (2026-09-12)

Sol's workspace, sidebar and nested buffer splits use `ca_split_begin`.
Causality owns their paint and input lifecycle.

Root cause: `ca_widget_input_pass` cleared `Ca_Splitter.dragging` without setting
`CA_DIRTY_CONTENT`. Hover transitions only caused CSS re-resolution in `ui.c`,
which did not invalidate paint when resolved CSS stayed the same. Cached active
bar commands consequently survived release or pointer-leave.

Fix: invalidate splitter content on drag start/end and old/new splitter hover
transitions. Existing post-input dirty scans schedule painting in that frame;
no polling, forced continuous rendering or Sol redraw workaround is needed.

Handle appearance is built into Causality `paint.c`: two one-pixel line segments
around five 2.5-pixel circular dots, scaled by UI scale and bounded for short or
narrow gutters. Side-by-side panes use a vertical handle; stacked panes use its
horizontal rotation. Fixed hover color is #8498B3; dragging uses #B8CCE6. The
handle is hidden when neither hovered nor dragged. Release while still hovering
returns to the hover appearance; pointer-leave hides it.

Removed `Ca_SplitDesc.bar_color`, `.bar_hover_color` and their internal/CSS
mappings. `bar_size` remains a layout gutter setting. Sol's obsolete splitter
foreground CSS was removed; CSS may still style the container.

Validation: Causality's cached-paint/input regression covers hover enter/leave,
release without ratio movement, clean idle frames, seven handle shapes, both
orientations, scales 1/1.12/2, and tiny bounds. Removing release invalidation
makes the test fail; restoring it passes. Full build and all 18 CTest tests pass.
A preview generated from actual draw-command geometry was rendered and inspected
(`/tmp/sol-splitter-preview.svg`); this is not a live GPU/application screenshot.
Live Sol computer-use inspection remains unavailable as recorded in
`text-glyph-clipping.md`. No commits or pushes.
