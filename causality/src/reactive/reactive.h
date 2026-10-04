// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#pragma once

#include "ca_reactive.h"

/** Drains inst's pending effects after active callbacks and batches finish. */
void ca_reactive_flush(Ca_Instance *inst);

/** Runs inst's original frame registrations; mutations apply to the next iteration. */
void ca_reactive_run_frame_effects(Ca_Instance *inst);

/** Releases inst's runtime after all callbacks finish; an unused instance is a no-op. */
void ca_reactive_shutdown(Ca_Instance *inst);
