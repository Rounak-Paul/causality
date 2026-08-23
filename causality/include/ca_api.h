// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

/* ---- Symbol visibility ----
 * causality is always built STATIC (see causality/CMakeLists.txt); it is
 * never linked as a DLL/shared object, so no dllexport/dllimport is needed.
 * causality_EXPORTS only raises GCC/Clang visibility from hidden back to
 * default for the public API surface when the static archive is later
 * embedded in a shared object (e.g. a plugin) that wants to re-export it.
 */
#ifndef CA_API
#  if defined(causality_EXPORTS) && (defined(__GNUC__) || defined(__clang__))
#    define CA_API __attribute__((visibility("default")))
#  else
#    define CA_API
#  endif
#endif
