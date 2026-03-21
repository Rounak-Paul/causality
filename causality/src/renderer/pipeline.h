/* pipeline.h — rect and text drawing pipelines */
#pragma once

#include "ca_internal.h"

/* ---- Shared SSBO descriptor layout (instanced rendering) ---- */

bool ca_ssbo_layout_create(Ca_Instance *inst);
void ca_ssbo_layout_destroy(Ca_Instance *inst);

/* Create per-frame instance buffer + descriptor set for a swapchain frame */
bool ca_instance_buf_create(Ca_Instance *inst, Ca_Frame *f);
void ca_instance_buf_destroy(Ca_Instance *inst, Ca_Frame *f);

/* ---- Rect pipeline ---- */

/* Create the shared rect pipeline using the given swapchain colour format.
   Called once on first window init; result lives in inst->rect_pipeline. */
bool ca_rect_pipeline_create(Ca_Instance *inst, VkFormat color_format);

/* Destroy; called from ca_renderer_shutdown. */
void ca_rect_pipeline_destroy(Ca_Instance *inst);

/* ---- Text pipeline ---- */

/* Create the text pipeline.  Requires that inst->font is already loaded
   (the font atlas image/sampler is bound into the descriptor set here).
   Result lives in inst->text_pipeline. */
bool ca_text_pipeline_create(Ca_Instance *inst, VkFormat color_format);

/* Destroy; called from ca_renderer_shutdown. */
void ca_text_pipeline_destroy(Ca_Instance *inst);

/* Update the descriptor set to point at the current font atlas.
   Call this once after ca_font_create succeeds. */
void ca_text_pipeline_update_font(Ca_Instance *inst);

/* ---- Image pipeline ---- */

/* Create the image pipeline (shares text pipeline layout, RGBA fragment shader).
   Must be called AFTER ca_text_pipeline_create. */
bool ca_image_pipeline_create(Ca_Instance *inst, VkFormat color_format);
void ca_image_pipeline_destroy(Ca_Instance *inst);
