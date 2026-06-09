// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ca_alloc.c — global allocator function pointers for Causality internals. */

#include "causality_config.h"
#include "ca_api.h"

#include <stdlib.h>

Ca_MallocFn  ca_g_malloc  = malloc;
Ca_CallocFn  ca_g_calloc  = calloc;
Ca_ReallocFn ca_g_realloc = realloc;
Ca_FreeFn    ca_g_free    = free;

/*
 * Override the global allocator function pointers used throughout Causality.
 *
 * Passing NULL for any function restores the corresponding libc default
 * (malloc, calloc, realloc, or free).  Must be called before any other
 * Causality API if a custom allocator is desired.
 *
 * mal  Replacement for malloc.
 * cal  Replacement for calloc.
 * ral  Replacement for realloc.
 * fre  Replacement for free.
 */
CA_API void ca_set_allocator(Ca_MallocFn mal, Ca_CallocFn cal,
                              Ca_ReallocFn ral, Ca_FreeFn fre)
{
    ca_g_malloc  = mal ? mal : malloc;
    ca_g_calloc  = cal ? cal : calloc;
    ca_g_realloc = ral ? ral : realloc;
    ca_g_free    = fre ? fre : free;
}
