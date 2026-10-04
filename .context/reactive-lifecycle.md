# Reactive runtime ownership and contract

Public specification: `docs/reactivity.md` and `causality/include/ca_reactive.h`.
Detailed engine integration evidence: QuasarEngine root
`.context/causality-reactive-lifecycle-2026-10-04.md`.

- `Ca_Instance` owns a lazy opaque runtime; unused tick hooks do not allocate.
- Stable signal/effect pools hold handles. Destroyed active effects retain their
  slots until execution/tracking references unwind, with callbacks cancelled
  immediately. User-data contexts are borrowed; destroyed handles are invalid.
- Frame iteration captures its starting registrations and compacts cancellation
  tombstones stably afterward. New registrations get one immediate run, then
  recurring runs from the next tick. Signal-triggered runs remain available.
- Notification is synchronous outside callbacks/batches/untrack scopes;
  reentrant invalidations drain after the current callback returns.
- Computed signals have one tracking/value path and lazy dirty recomputation.
  Derived dirtiness propagates iteratively before observers, including batches.
  Computed storage is read-only and dependency graphs must be acyclic.
- Untrack suppresses only the caller's tracking, preserving nested computation
  dependencies. `ca_batch_begin` and `ca_untrack` report bool success.
- Registration reserves notification queues. Tracking failure retries live
  effects; initial creation may return NULL after callback side effects.
- Headless regression/fault-injection and focused ASan/UBSan pass; parent-engine
  full CTest is 27/27, with Runtime and Editor lifecycle smoke also passing.
