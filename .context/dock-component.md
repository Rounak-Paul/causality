# Dock component — ca_dock.h (2026-10-17, ImGui semantics same day)

Reusable, mechanism-only docking workspace, ported from QuasarEngine's
`Editor/src/ed_dock_model.c` + `ui/ed_dock.c` and generalised.

## Layers
- `Ca_DockLayout` (`src/ui/ca_dock_layout.c`): pure model. Nodes are splits or
  tab groups, one tree per window (0 = host main window). Dynamic storage
  (Ca_DynArray for panels/nodes/windows, per-group tab arrays). No fixed caps;
  only `CA_DOCK_ID_MAX` (64) bounds panel id length.
- `Ca_Dock` (`src/ui/ca_dock.c`): instance-owned view/interaction. Emits splits
  + `ca_tabs` via div builders, drag state machine (PRESSED → REORDER →
  FLOATING ghost window), drop targets/preview overlay, floating OS windows.
- Shared structs: `src/ui/ca_dock_internal.h`.

## Invariants
- Every layout mutation claims nodes/window slots/tab capacity BEFORE touching
  the tree → failure-atomic without snapshot copies. Removal never allocates.
- Callbacks get stable heap `DockBinding {dock, index}` objects (one per node /
  window index, never freed until destroy) because view arrays relocate.
- Structural changes: teardown mounted panels → bump `epoch` signal → host
  builders (read epoch) re-emit next tick. Tab switch rebuilds one group via
  its content div builder.
- Per-tick work runs in a `ca_frame_effect` (`pump`). During drags it calls
  `ca_instance_request_frame_after(1/120 s)` so event-driven hosts keep ticking.
- Floating window creation failure → its tree is docked back into the main
  window (`reclaim_window`), never silently lost.
- `ca_dock_destroy` must run while the main window is alive (call it from the
  main window's on_close). It destroys floating windows, the root div (killing
  its builder effect) and only then the epoch signal.

## ImGui-exact behaviour (checked against imgui docking branch imgui.cpp)
- Central group (`set_central`, main window only, ≤1): never collapses; empty →
  `.ca-dock-empty` drop area (ImGuiCol_DockingEmptyBg). Root central offers only
  centre + outer targets (DockNodePreviewDockSetup). On split the side keeping
  existing content keeps `central` (DockNodeTreeSplit inheritor).
- Undock (drag out) = `ca_dock_layout_remove`: forgets place; empty non-central
  group collapses, sibling takes space (DockNodeRemoveWindow + TreeMerge).
- Close (tab X / close_panel / closing floating window) = `ca_dock_layout_close`:
  panel keeps `home`; group stays hidden (invisible, sibling fills visually);
  reopen → add_tab(home) restores place/size (window->DockId persistence).
  Docking elsewhere clears home and prunes orphaned hidden groups.
- Size policy: splits on the central path pin the non-central pane
  (`size`, logical px, persisted) via new splitter fixed-pane mode; host resize
  only changes the central group (DockNodeTreeUpdatePosSize rule 3).
- Divider drag: panes not touching the divider are pinned for the drag
  (WantLockSizeOnce) via `lock_untouched`; settled sizes written back on drag end.
- Drop size: incoming pane = ghost window size along axis if ≤ half target,
  else half (DockNodeCalcSplitRects); pinned when beside the central path.
- Whole-tree TAB drop: empty target is replaced by the tree (central role →
  tree's first visible group); non-empty target only accepts a tree with one
  visible group (IsCenterAvailable rule); remembered homes re-homed.
- Main-window groups always show a tab; only floating single-panel windows
  hide the strip (title bar used instead).
- Saved layouts without central adopt the default layout's central panel.

## Splitter primitive added (causality.h)
- `Ca_SplitDesc.fixed_pane` (CA_SPLIT_FIXED_NONE/FIRST/SECOND) + `fixed_size`;
  `ca_split_set_fixed` (size<=0 → derive from ratio at next layout),
  `ca_split_get_pane_size`, `ca_split_is_dragging`. Clamped by min/max ratio
  without losing the requested size. `ca_split_set_ratio` resets fixed size
  derivation.

## Differences from Quasar's ed_dock
- No globals, no ED_DOCK_MAX_* caps, no cJSON (own bounded-depth JSON
  reader/writer, locale independent; same on-disk format, so Quasar layouts load).
- Persistence is host-owned: `ca_dock_save` (snprintf semantics) /
  `Ca_DockDesc.saved_layout`; no config_key, no persist countdown.
- API: split/dock_root/dock_window take an incoming `ratio` (<=0 = half);
  new close/home_of/node_visible/set_central/central/set_split_size.
- `hide` panel callback replaced by `Ca_DockDesc.on_event`
  (PANEL_OPENED / CLOSED / ACTIVATED, LAYOUT_CHANGED coalesced per tick).
- `default_layout` receives a CLEARED layout; invalid default → empty layout.
- Panel `build(dock, window, user_data)`; `available` predicate ⇒ transient
  (never saved/restored), as in Quasar.
- Class names `.ca-dock-*` with palette-driven defaults in `style.c`
  system sheet (Quasar used `panel`, `dock-*`, `panel-tab*`).

## Tests
- `tests/ca_dock_layout_tests.c` (`causality_dock_layout_tests`): ops, collapse,
  float/dock_window split+merge, persistence round-trip, tolerant/malicious
  load (depth 2000 rejected), deep copy, allocation-failure atomicity for each
  op (asserts injection happened whenever the op allocates), 20k random ops on
  300 panels with validity + save/load round-trip checks. ASan/UBSan clean.
- View layer verified via a capture harness (renders default 6-panel layout,
  ASan clean). Interactive drag/drop/floating not automatable here.
