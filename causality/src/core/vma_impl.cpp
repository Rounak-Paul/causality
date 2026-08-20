// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* vma_impl.cpp — sole translation unit compiling the Vulkan Memory Allocator
   implementation. VMA_IMPLEMENTATION pulls in the full definitions (C++
   internally: templates, STL containers) behind the extern-"C"-declared,
   C-ABI-compatible API in <vk_mem_alloc.h>. Every other file in this tree,
   including plain-C callers in gpu.c and Quasar's qs_gpu.c, includes that
   header for declarations only and links against the symbols defined here. */

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
