# Inline formatting context — Ca_DivDesc.inline_flow (2026-09-21)

Added for the causality-browser project (a separate consumer of this repo,
alongside Sol): real inline text flow, where mixed text + inline elements
(`<b>`, `<a>`, etc. in HTML terms) wrap together on shared lines, the way
a browser's inline formatting context works. Previously there was no such
primitive — only flexbox (`ca_div_begin`'s `direction`), which lays out
each child as its own independent box and cannot wrap mixed content onto
shared lines.

Fully additive: `Ca_DivDesc.inline_flow` (new field, default `false`) is
the only public API surface change. Every existing caller — including
Sol — is unaffected; a zero-initialized or pre-existing `Ca_DivDesc{...}`
behaves exactly as before.

## Usage
```c
ca_div_begin(&(Ca_DivDesc){ .width = 400, .inline_flow = true });
    ca_text(&(Ca_TextDesc){ .text = "Some plain text " });
    ca_text(&(Ca_TextDesc){ .text = "a bold run", .style = "bold-class" });
    ca_text(&(Ca_TextDesc){ .text = " more text that wraps naturally." });
ca_div_end();
```
Direct children of an `inline_flow` div are packed word-by-word onto
lines that wrap at the container's width. `direction`/`gap`/
`align_items`/`justify_content` are ignored on an `inline_flow` div —
line-breaking and per-line baseline alignment replace them entirely.

## Scope of this first implementation
- Only `CA_WIDGET_LABEL` children (i.e. `ca_text` calls) are split into
  word runs. Other in-flow children are placed as one unbreakable inline
  box each (sized by their own explicit width/height), never split
  across lines — there is no nested inline-sub-container support (an
  `inline_flow` div's own children cannot themselves be `inline_flow`
  divs with independent runs; each direct child is either a flat label
  or an opaque box).
- Per-line baseline alignment uses the tallest run's ascent on that line;
  a non-text box run has no baseline of its own, so its height is
  treated as its ascent contribution (grows the line to fit it, doesn't
  try to align anything against it).
- Absolute/fixed children of an `inline_flow` div are unaffected —
  resolved normally via the existing out-of-flow code path in
  `layout_node`.
- No click-target/hit-testing support inside an inline run yet (e.g. a
  clickable link mid-sentence) — text-only flow. Flagged as an explicit
  follow-up in the research that scoped this feature, not an oversight.
- `CA_CSS_PROP_DISPLAY`'s `inline`/`inline-block` keywords are still
  unconsumed (parsed into `Ca_ResolvedStyle.display`, never read) — this
  feature is opt-in via the `inline_flow` field only, not yet
  auto-derived from `display: inline` in a stylesheet. A CSS-driven
  `apply_css` path that sets `nd.inline_flow` from `display: inline` on
  a div is a natural next step but wasn't required for the
  causality-browser DOM bridge's first use (it sets `inline_flow`
  directly from its own HTML tag-classification logic).

## Implementation
- `causality/src/ui/inline_layout.{h,c}` (new files) — `ca_inline_layout`
  walks a container's children, splits label text into word runs by the
  same whitespace/newline rules as the existing single-string word-wrap
  (`layout.c`'s `measure_wrapped_text_height` / `paint.c`'s
  `paint_text_wrapped` — this is a generalization of that same algorithm
  to span multiple sibling nodes, not a new algorithm from scratch),
  packs runs onto lines against an available width, and resolves each
  run's `x`/`line_index`/`baseline_y`. Result cached on
  `Ca_Node.inline_layout` (a lazily-allocated `Ca_InlineLayout*`, freed
  in `node.c`'s `free_subtree` alongside the node's other owned buffers;
  reused across frames rather than freed/realloced every rebuild — the
  run array's capacity persists even as `run_count` resets to 0 each
  layout pass).
- `layout.c`'s `layout_node` branches to `ca_inline_layout` right after
  computing the container's inner (post-padding) width, before the
  normal flex-line-building code; in-flow children are zeroed/dirty-
  cleared (mirroring the existing hidden-node handling) rather than
  independently laid out, since their position comes from the inline
  pass instead. `content_size()` gained a matching branch for the
  height-query case (asking a container's natural height requires
  actually running the line-breaking pass — there's no way to predict
  wrapped line count without it, same reasoning the pre-existing
  flex-wrap-row height-query branch already uses).
- `paint.c` gained `paint_inline_run`/`paint_inline_children` (new
  functions, same file as `paint_text`/`paint_text_wrapped` since they
  need paint.c's file-local `ClipRect` type and draw-command helpers —
  `inline_layout.h` deliberately owns only measurement/layout, no paint
  dependencies, so it has no `ClipRect`-shaped API at all). Hooked into
  `paint_node_content` before the normal `widget_type` switch, and
  `paint_tree_cached`'s child-recursion loop skips recursing into an
  `inline_flow` node's children entirely (they were already fully
  painted as part of the parent's resolved runs — recursing would
  double-paint them at their own, never-computed, independent box).

## Verified
Built clean (causality alone, and the full causality-browser app); ran
via a temporary instrumentation pass (added, verified, reverted — not
left in the tree) confirming: multiple real `inline_flow` containers in
a live demo page produced correct non-zero run/line counts and sane
content heights; a narrow (150px) test container with a 10-word string
wrapped into 3 lines with an internally-consistent `content_h` (≈3×
single-line height); process RSS stable over a multi-second run
(no leak from the per-frame layout-pass reuse pattern).
