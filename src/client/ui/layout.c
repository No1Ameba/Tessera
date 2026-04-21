#include "layout.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static layout_node_t *alloc_node(layout_node_type_t type, uint32_t pane_id,
                                  layout_rect_t rect)
{
    layout_node_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type    = type;
    n->rect    = rect;
    n->pane_id = pane_id;
    return n;
}

/*
 * Recompute children[0] and children[1] rects from node->rect and split_ratio.
 * Recurse into any split children.
 */
static void recompute_rects(layout_node_t *node)
{
    if (!node || node->type == LAYOUT_LEAF) return;

    layout_rect_t r = node->rect;
    layout_node_t *a = node->children[0];
    layout_node_t *b = node->children[1];

    if (node->type == LAYOUT_SPLIT_H) {
        int aw = (int)(r.w * node->split_ratio);
        a->rect = (layout_rect_t){ r.x,      r.y, aw,       r.h };
        b->rect = (layout_rect_t){ r.x + aw, r.y, r.w - aw, r.h };
    } else { /* LAYOUT_SPLIT_V */
        int ah = (int)(r.h * node->split_ratio);
        a->rect = (layout_rect_t){ r.x, r.y,      r.w, ah       };
        b->rect = (layout_rect_t){ r.x, r.y + ah, r.w, r.h - ah };
    }

    recompute_rects(a);
    recompute_rects(b);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

layout_node_t *layout_create_leaf(uint32_t pane_id,
                                   int x, int y, int w, int h)
{
    return alloc_node(LAYOUT_LEAF, pane_id, (layout_rect_t){x, y, w, h});
}

void layout_destroy(layout_node_t *root)
{
    if (!root) return;
    layout_destroy(root->children[0]);
    layout_destroy(root->children[1]);
    free(root);
}

layout_node_t *layout_split(layout_node_t *leaf,
                              layout_node_type_t dir, float ratio,
                              uint32_t new_pane_id)
{
    if (!leaf || leaf->type != LAYOUT_LEAF) return NULL;
    if (ratio <= 0.0f || ratio >= 1.0f) return NULL;

    /* Create the split node with the leaf's current rect. */
    layout_node_t *split = alloc_node(dir, 0, leaf->rect);
    if (!split) return NULL;
    split->split_ratio = ratio;
    split->parent = leaf->parent;

    /* Create the new leaf (second child). */
    layout_node_t *new_leaf = alloc_node(LAYOUT_LEAF, new_pane_id,
                                          (layout_rect_t){0,0,0,0});
    if (!new_leaf) { free(split); return NULL; }

    /* Wire: split → (existing leaf, new leaf). */
    split->children[0] = leaf;
    split->children[1] = new_leaf;
    leaf->parent        = split;
    new_leaf->parent    = split;

    /* Replace leaf in grandparent. */
    layout_node_t *gp = split->parent;
    if (gp) {
        if      (gp->children[0] == leaf) gp->children[0] = split;
        else if (gp->children[1] == leaf) gp->children[1] = split;
    }

    /* Compute rects for both children. */
    recompute_rects(split);

    return new_leaf;
}

void layout_remove(layout_node_t **root_ptr, layout_node_t *node)
{
    if (!node || node->type != LAYOUT_LEAF) return;

    layout_node_t *parent = node->parent;
    if (!parent) {
        /* node is the sole root leaf. */
        free(node);
        if (root_ptr) *root_ptr = NULL;
        return;
    }

    /* Find sibling. */
    layout_node_t *sibling =
        (parent->children[0] == node) ? parent->children[1]
                                       : parent->children[0];

    /* Sibling inherits parent's rect and parent's position in grandparent. */
    sibling->rect   = parent->rect;
    sibling->parent = parent->parent;

    layout_node_t *gp = parent->parent;
    if (gp) {
        if      (gp->children[0] == parent) gp->children[0] = sibling;
        else if (gp->children[1] == parent) gp->children[1] = sibling;
    } else if (root_ptr) {
        *root_ptr = sibling;
    }

    /* Recompute sibling's subtree with its new rect. */
    recompute_rects(sibling);

    /* Free the removed leaf and parent split node. */
    free(node);
    free(parent);
}

void layout_resize_root(layout_node_t *root, int x, int y, int w, int h)
{
    if (!root) return;
    root->rect = (layout_rect_t){ x, y, w, h };
    recompute_rects(root);
}

layout_node_t *layout_find_pane(layout_node_t *root, uint32_t pane_id)
{
    if (!root) return NULL;
    if (root->type == LAYOUT_LEAF)
        return root->pane_id == pane_id ? root : NULL;
    layout_node_t *r = layout_find_pane(root->children[0], pane_id);
    return r ? r : layout_find_pane(root->children[1], pane_id);
}

void layout_each_leaf(layout_node_t *root,
                       void (*cb)(layout_node_t *, void *), void *user)
{
    if (!root) return;
    if (root->type == LAYOUT_LEAF) { cb(root, user); return; }
    layout_each_leaf(root->children[0], cb, user);
    layout_each_leaf(root->children[1], cb, user);
}

/* ── Serialization ───────────────────────────────────────────────────────── */

static int ser_rec(const layout_node_t *n, uint8_t *buf, size_t cap, size_t *off)
{
    if (!n) return -1;
    if (n->type == LAYOUT_LEAF) {
        if (*off + 1 + 4 > cap) return -1;
        buf[(*off)++] = 0;
        memcpy(buf + *off, &n->pane_id, 4); *off += 4;
        return 0;
    }
    if (*off + 1 + 4 > cap) return -1;
    buf[(*off)++] = (n->type == LAYOUT_SPLIT_H) ? 1 : 2;
    float r = n->split_ratio;
    memcpy(buf + *off, &r, 4); *off += 4;
    if (ser_rec(n->children[0], buf, cap, off) < 0) return -1;
    if (ser_rec(n->children[1], buf, cap, off) < 0) return -1;
    return 0;
}

int layout_serialize(const layout_node_t *root, uint8_t *buf, size_t buf_size)
{
    if (!root || !buf) return -1;
    size_t off = 0;
    if (ser_rec(root, buf, buf_size, &off) < 0) return -1;
    return (int)off;
}

static layout_node_t *deser_rec(const uint8_t *buf, size_t cap, size_t *off,
                                 layout_node_t *parent)
{
    if (*off + 1 > cap) return NULL;
    uint8_t type = buf[(*off)++];
    if (type == 0) {
        if (*off + 4 > cap) return NULL;
        uint32_t pid;
        memcpy(&pid, buf + *off, 4); *off += 4;
        layout_node_t *n = calloc(1, sizeof *n);
        if (!n) return NULL;
        n->type = LAYOUT_LEAF; n->pane_id = pid; n->parent = parent;
        return n;
    }
    if (type != 1 && type != 2) return NULL;
    if (*off + 4 > cap) return NULL;
    float ratio;
    memcpy(&ratio, buf + *off, 4); *off += 4;
    layout_node_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type = (type == 1) ? LAYOUT_SPLIT_H : LAYOUT_SPLIT_V;
    n->split_ratio = ratio;
    n->parent = parent;
    n->children[0] = deser_rec(buf, cap, off, n);
    n->children[1] = deser_rec(buf, cap, off, n);
    if (!n->children[0] || !n->children[1]) {
        layout_destroy(n);
        return NULL;
    }
    return n;
}

layout_node_t *layout_deserialize(const uint8_t *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return NULL;
    size_t off = 0;
    return deser_rec(buf, buf_size, &off, NULL);
}
