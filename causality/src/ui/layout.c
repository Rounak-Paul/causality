/* layout.c — flexbox-inspired single-axis layout */
#include "layout.h"

/* Compute the laid-out rect of a node given the available space.
   Recurses into children. */
static void layout_node(Ca_Node *node, float x, float y, float avail_w, float avail_h)
{
    /* Determine this node's own size */
    float w = (node->desc.width  > 0.0f) ? node->desc.width  : avail_w;
    float h = (node->desc.height > 0.0f) ? node->desc.height : avail_h;

    if (node->desc.min_w > 0.0f && w < node->desc.min_w) w = node->desc.min_w;
    if (node->desc.max_w > 0.0f && w > node->desc.max_w) w = node->desc.max_w;
    if (node->desc.min_h > 0.0f && h < node->desc.min_h) h = node->desc.min_h;
    if (node->desc.max_h > 0.0f && h > node->desc.max_h) h = node->desc.max_h;

    node->x = x;
    node->y = y;
    node->w = w;
    node->h = h;
    node->dirty &= ~(CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN);

    if (node->child_count == 0) return;

    /* Inner area after padding */
    float inner_x = x + node->desc.padding_left;
    float inner_y = y + node->desc.padding_top;
    float inner_w = w - node->desc.padding_left - node->desc.padding_right;
    float inner_h = h - node->desc.padding_top  - node->desc.padding_bottom;
    if (inner_w < 0.0f) inner_w = 0.0f;
    if (inner_h < 0.0f) inner_h = 0.0f;

    bool  is_row  = (node->desc.direction == CA_DIR_ROW);
    float gap     = node->desc.gap;

    /* Sum up fixed sizes and count auto-sized children */
    float    total_fixed = (node->child_count > 1)
                           ? gap * (float)(node->child_count - 1)
                           : 0.0f;
    uint32_t auto_count  = 0;

    for (uint32_t i = 0; i < node->child_count; ++i) {
        float main_size = is_row ? node->children[i]->desc.width
                                 : node->children[i]->desc.height;
        if (main_size > 0.0f)
            total_fixed += main_size;
        else
            auto_count++;
    }

    float avail_main  = is_row ? inner_w : inner_h;
    float avail_cross = is_row ? inner_h : inner_w;
    float auto_main   = (auto_count > 0)
                        ? (avail_main - total_fixed) / (float)auto_count
                        : 0.0f;
    if (auto_main < 0.0f) auto_main = 0.0f;

    /* Place children */
    float cursor = 0.0f;
    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];

        float cm = is_row ? child->desc.width  : child->desc.height;
        float cc = is_row ? child->desc.height : child->desc.width;
        if (cm <= 0.0f) cm = auto_main;
        if (cc <= 0.0f) cc = avail_cross;

        float cx, cy, cw, ch;
        if (is_row) {
            cx = inner_x + cursor;  cy = inner_y;
            cw = cm;                ch = cc;
        } else {
            cx = inner_x;           cy = inner_y + cursor;
            cw = cc;                ch = cm;
        }

        layout_node(child, cx, cy, cw, ch);
        cursor += cm + gap;
    }
}

void ca_layout_pass(Ca_Window *win)
{
    if (!win->root) return;

    /* Use logical (window) size so widget coordinates stay in the same
       pixel space as user-specified sizes (padding, width, height).
       On Retina / HiDPI, glfwGetFramebufferSize returns 2× logical size
       which would make every widget appear half-sized. */
    int lw, lh;
    glfwGetWindowSize(win->glfw, &lw, &lh);
    layout_node(win->root, 0.0f, 0.0f, (float)lw, (float)lh);
}
