# Reactive API contract

`ca_reactive.h` is the authoritative function reference. Reactive objects are
owned by their `Ca_Instance`, use the configured allocator, and run on the main
thread. Callbacks, raw value inputs, and user-data pointers are borrowed.
Instances must outlive calls using them; destroy handles must be discarded.

## Notification and batching

A plain signal write compares the full value byte sequence. Changed bytes or
`ca_signal_notify` invalidate subscribers and the complete derived chain before
any effect runs. Effects drain synchronously before the initiating API returns.
Inside an effect or an untrack scope, notifications wait until the callback
returns. Inside a batch, they wait until its outermost successful end.

An effect never recursively enters itself. A callback that changes one of its
own dependencies queues another run after it returns. Such work must converge;
there is no iteration cutoff that silently discards user work. Subscriber order
is unspecified. Scheduling coalesces duplicate pending invalidations.

`ca_batch_begin` returns whether the batch opened. Match each successful begin
with one end; failed begins do not change nesting. `ca_untrack` reports whether
its callback was invoked. Untracked reads suppress the caller's dependencies
while nested computations and new effects keep their own tracking. Neither
operation partially enters a scope on failure. Unmatched batch ends do nothing.

## Derived values

`ca_computed` computes the initial value once and tracks the signals read by
its callback. Subsequent invalidation marks the derived chain dirty immediately,
without evaluating computations. A dirty value recomputes once when read, even
while a batch defers observer effects. This keeps read-after-write semantics.

Computation callbacks must return the configured number of readable bytes and
must not mutate signal values or create/destroy reactive objects. Dependencies
belong to the same instance and must form an acyclic graph. Cyclic reads are
reported and fail rather than recursing indefinitely. Computed signals are
read-only: set/notify calls do nothing and mutable access returns NULL.
Dependents can rerun on invalidation even if the derived bytes remain equal.

## Frame effects and destruction

A frame effect runs immediately at registration. Each recurring iteration
captures the registrations that existed when it began and preserves their
order. Removing a registration cancels any callback that has not started.
Adding one gives it its immediate run, then recurring runs from the next tick.
Tracked signal changes can also run a frame effect between ticks.

Destroying an effect cancels its pending work and dependencies immediately.
An active execution keeps the effect's storage alive until its call and tracking
references unwind, preventing a replacement from inheriting those references.
The caller must keep user data alive for any callback that is still executing.
Instance destruction belongs outside active callbacks.

## Allocation and failure

Unused tick hooks do not create a runtime. Registration reserves invalidation
and pending-work storage before publishing an effect. Warm notifications and
unchanged dependency lists require no allocations. Tracking a new dependency
or growing callback nesting may allocate.

Creation returns NULL on failure; an initial callback may have executed before
a tracking allocation fails, so its side effects are not rolled back. A live
effect whose tracking fails remains queued for retry on a subsequent tick.
Raw computed reads return NULL on evaluation failure and retain dirty state for
a later read. Typed reads return zero, false, or NULL for a failed read.
