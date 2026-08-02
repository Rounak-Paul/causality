// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.
//
// causality_config.h — fixed protocol values, text policies, and allocators.

#pragma once

/* ---- Renderer ---- */

#ifndef CA_FRAMES_IN_FLIGHT
#  define CA_FRAMES_IN_FLIGHT 2
#endif

/* ---- Text storage ---- */

#ifndef CA_LABEL_TEXT_MAX
#  define CA_LABEL_TEXT_MAX 256
#endif
#ifndef CA_BUTTON_TEXT_MAX
#  define CA_BUTTON_TEXT_MAX 128
#endif
#ifndef CA_INPUT_TEXT_MAX
#  define CA_INPUT_TEXT_MAX 512
#endif
#ifndef CA_OPTION_TEXT_MAX
#  define CA_OPTION_TEXT_MAX 128
#endif
/* ---- Allocator hooks ---- */
/*
 * All internal Causality heap allocations go through the following function
 * pointers.  Call ca_set_allocator() before ca_instance_create() to redirect
 * them to a custom allocator (e.g. the engine memory system).  Defaults are
 * the standard-library malloc / calloc / realloc / free.
 */

#include <stddef.h>

typedef void *(*Ca_MallocFn) (size_t sz);
typedef void *(*Ca_CallocFn) (size_t n, size_t sz);
typedef void *(*Ca_ReallocFn)(void *ptr, size_t sz);
typedef void  (*Ca_FreeFn)   (void *ptr);

extern Ca_MallocFn  ca_g_malloc;
extern Ca_CallocFn  ca_g_calloc;
extern Ca_ReallocFn ca_g_realloc;
extern Ca_FreeFn    ca_g_free;

#define CA_MALLOC(sz)        ca_g_malloc(sz)
#define CA_CALLOC(n, sz)     ca_g_calloc((n), (sz))
#define CA_REALLOC(ptr, sz)  ca_g_realloc((ptr), (sz))
#define CA_FREE(ptr)         ca_g_free(ptr)
