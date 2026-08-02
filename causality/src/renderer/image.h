// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* image.h — GPU-uploaded RGBA images for ca_image() widget */
#pragma once

#include "ca_internal.h"

/* Create image descriptor pool (called once during renderer init) */
bool ca_image_pool_init(Ca_Instance *inst);
void ca_image_pool_shutdown(Ca_Instance *inst);

/** Allocate a sampled-image descriptor set from growable instance pools. */
bool ca_image_descriptor_allocate(Ca_Instance *inst,
                                  VkDescriptorSetLayout layout,
                                  VkDescriptorSet *out_set,
                                  VkDescriptorPool *out_pool);

/** Release a sampled-image descriptor set to its owning pool. */
void ca_image_descriptor_free(Ca_Instance *inst,
                              VkDescriptorPool pool,
                              VkDescriptorSet set);

/* Public API implementation */
Ca_Image *ca_image_create_impl(Ca_Instance *inst,
                               const uint8_t *pixels, int w, int h);
void ca_image_destroy_impl(Ca_Instance *inst, Ca_Image *img);
