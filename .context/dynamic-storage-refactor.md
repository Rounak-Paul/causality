# Dynamic storage refactor

## Scope

Causality must own its dynamic-array implementation because it is an
independent reusable library. Replace compile-time capacity pools and queues
with allocator-backed contiguous storage while preserving fixed-size data that
represents ABI, GPU frame cardinality, vector/matrix shape, or a deliberately
bounded text value.

## Invariants

- All allocations use the configured `CA_*` allocator hooks.
- Growth is overflow-checked and failure-atomic: allocation failure leaves the
  existing array and published owner state unchanged.
- Stable public handles may not point into relocatable registry storage.
- Per-frame hot iteration remains contiguous and does not allocate after
  retained capacity is sufficient.
- Logical removal invokes the owning subsystem's cleanup before storage is
  reused or released.
- Shutdown releases every dynamic owner in reverse dependency order.
- The process-global widget build context retains storage only until its
  owning instance begins teardown; it is released before window destruction
  so a subsequent instance cannot inherit a stale allocator pointer.
- No capacity macro remains as a runtime correctness limit.

## Implemented ownership model

- `Ca_DynArray` is the public, Causality-owned contiguous container. It is
  overflow checked, alias safe, failure atomic, allocator-hook aware, and
  covered by deterministic allocation-failure tests.
- `Ca_Pool` is the internal stable-address owner for public pointer handles.
  Windows, UI nodes/widgets, images, signals, and effects grow in 16 KiB-ish
  chunks and reuse released slots without relocating live objects.
- Windows own retained dynamic draw, sort, paint-cache, layout, geometry,
  keyboard, character, and focus-navigation buffers. Per-node child and
  transition storage also grows on demand.
- The reactive runtime owns dynamic dependency/subscriber lists, pending and
  frame-effect queues, tracking stacks, and stable signal/effect pools.
- Events use double-buffered dynamic queues, so producers may post during
  dispatch without a fixed event-loss ceiling.
- Menus, popups, tabs, selects, tables, node-graph state, and application menu
  trees use demand-grown owned storage with deep cleanup.
- Stylesheets allocate rules, selector lists, selector-chain parts, classes,
  pseudo-classes, declarations, variables, interned strings, and
  cache-classification data according to parsed content.
- Font pages retain their physical atlas cardinality, but extra glyph maps and
  dirty upload rectangles grow according to actual glyph use.
- Vulkan swapchain image arrays use the driver-reported count. Sampled-image
  and SSBO descriptor pools grow in reusable chunks, with each live descriptor
  retaining its owning pool for safe release. Instance buffers grow from the
  actual draw count rather than reserving the historical maximum. Instance
  extension and validation-layer discovery use query-sized dynamic storage.

## Fixed cardinalities that remain inline

- Frames in flight, font atlas page geometry, shader vectors, and enum-indexed
  handler tables are protocol or physical-layout cardinalities rather than
  application-size limits.
- Short identity/style/text fields retain explicit API truncation semantics.
- Mouse buttons, native resize edges, and cursor kinds are platform protocol
  sets.

## Validation target

- Unit-test array overflow, aliasing, failure preservation, growth, mutation,
  and ownership operations.
- `causality_array_tests` covers array and pool lifecycle, aliasing, overflow,
  stable addresses, slot reuse, and allocation-failure preservation.
- `causality_storage_tests` grows node-graph state to 2,048 nodes and parses
  1,101 CSS rules, 40 selectors, 120 declarations, and a selector with 12
  chain parts, 12 classes per part, and 6 pseudo-classes per part, all beyond
  the removed historical ceilings.
- Root Quasar Debug build and CTest validate Causality plus Editor, Runtime,
  scene storage, and both dynamic-array implementations.

## Integration note

Quasar's viewport callback trampoline stores heap-owned callback states behind
a `Ca_DynArray` of pointers. This preserves callback data addresses while
removing the old window-count-derived static array. Editor project settings
uses the new node-graph state access/add/destroy API and does not inspect
relocatable storage.

Both integrations must close their ownership loops explicitly. Editor calls
`ed_project_settings_shutdown()` while its project is still live so the node
graph can persist positions and destroy every `Ca_NgNodeState`. Quasar releases
an empty viewport registry immediately and performs a final registry shutdown
before destroying the Causality instance, covering normal removal and partial
initialization/teardown paths.
