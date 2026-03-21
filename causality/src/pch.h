#pragma once

/* Standard C */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

/* POSIX compat for MSVC */
#ifdef _MSC_VER
  #define strcasecmp _stricmp
#endif

/* Vulkan */
#include <vulkan/vulkan.h>

/* GLFW (no OpenGL, Vulkan surface only) */
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
