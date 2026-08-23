// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

/* ---- Symbol visibility ----
 * causality is always built STATIC (see causality/CMakeLists.txt) — it is
 * never linked as its own DLL/shared object. Its static archive is instead
 * folded directly into Quasar's shared library (Quasar.dll / libQuasar.so),
 * which is the thing applications (Editor, Runtime, plugins) actually link
 * against. causality_EXPORTS is defined unconditionally when compiling
 * causality's own sources (see causality/CMakeLists.txt), so CA_API always
 * resolves to "export" there — dllexport on Windows so those symbols land
 * in Quasar.dll's export table once linked in, default visibility on
 * GCC/Clang overriding causality's own -fvisibility=hidden. Consumers that
 * merely declare (never define) these functions — i.e. every #include of
 * causality.h outside causality's own build — see causality_EXPORTS
 * undefined, so CA_API resolves to "import": dllimport on Windows, so
 * Editor/Runtime resolve these symbols through Quasar.dll's single shared
 * instance instead of linking their own separate copy (a separate copy
 * would mean a separate, out-of-sync widget-build g_ctx — the exact bug
 * this contract exists to prevent). On non-Windows, ELF/Mach-O give every
 * process-wide symbol a single definition regardless, so CA_API is a no-op
 * there once causality_EXPORTS is unset.
 */
#ifndef CA_API
#  if defined(causality_EXPORTS)
#    if defined(_WIN32) || defined(__CYGWIN__)
#      define CA_API __declspec(dllexport)
#    elif defined(__GNUC__) || defined(__clang__)
#      define CA_API __attribute__((visibility("default")))
#    else
#      define CA_API
#    endif
#  else
#    if defined(_WIN32) || defined(__CYGWIN__)
#      define CA_API __declspec(dllimport)
#    else
#      define CA_API
#    endif
#  endif
#endif
