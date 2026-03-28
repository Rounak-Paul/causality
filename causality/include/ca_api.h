#pragma once

/* ---- Symbol visibility ---- */
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
