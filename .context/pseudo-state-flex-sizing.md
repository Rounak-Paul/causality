# Preserve flex sizing during pseudo-state styling

2026-10-23. `causality/src/ui/widget.c:ca_widget_reapply_css` preserves the current descriptor's flex_grow, flex_shrink, flex_basis, flex_order and flex_wrap while reapplying visual CSS, matching its existing preservation of sizing/spacing. Zero is an intentional inline/widget layout override; zero-fill CSS application must not restore class flex defaults on hover/focus.

Confirmed in Flow's native manuscript test: `.fl-property-input { flex-grow: 1 }` plus inline `flex-grow: 0; flex-shrink: 0` changed a fixed title field's rendered height on hover and compressed its sibling narration editor. Native repeated hover/focus sizing assertions fail before the fix and pass after it. Existing pseudo-state layout transitions continue through `ca_motion_follow_layout`.
