#ifndef TERMEMU_LAYOUT_H
#define TERMEMU_LAYOUT_H

/*
 * Tiling layout — binary-tree-based pane splitting.
 *
 * Each node is either:
 *   LAYOUT_LEAF    — holds a pane_id and a pixel rect
 *   LAYOUT_SPLIT_H — horizontal split (two children side by side)
 *   LAYOUT_SPLIT_V — vertical split   (two children top/bottom)
 *
 * Coordinate system: top-left (0,0), x increases right, y increases down.
 */

#include <stdint.h>
#include <stddef.h>

typedef enum {
    LAYOUT_LEAF,     /* leaf node: contains a PTY pane */
    LAYOUT_SPLIT_H,  /* horizontal split: children[0]=left, children[1]=right */
    LAYOUT_SPLIT_V,  /* vertical split:   children[0]=top,  children[1]=bottom */
} layout_node_type_t;

typedef struct { int x, y, w, h; } layout_rect_t;

typedef struct layout_node layout_node_t;
struct layout_node {
    layout_node_type_t  type;
    layout_rect_t       rect;        /* pixel bounds, inclusive */
    float               split_ratio; /* 0-1: fraction for children[0] */
    uint32_t            pane_id;     /* only valid for LAYOUT_LEAF */
    layout_node_t      *children[2]; /* [0]=first, [1]=second for split nodes */
    layout_node_t      *parent;      /* NULL for root */
};

/* ── Construction ────────────────────────────────────────────────────────── */

/* Create a root leaf node occupying the given pixel rect. */
layout_node_t *layout_create_leaf(uint32_t pane_id, int x, int y, int w, int h);

/* Free the entire subtree rooted at node. */
void layout_destroy(layout_node_t *root);

/* ── Splitting ───────────────────────────────────────────────────────────── */

/*
 * Split a leaf node.
 * The original leaf retains its pane_id; a new leaf with new_pane_id is created.
 * For SPLIT_H: original → left child, new → right child.
 * For SPLIT_V: original → top  child, new → bottom child.
 * ratio: fraction of space given to the original pane (0 < ratio < 1).
 *
 * @return the new leaf node (caller can use it to further split), or NULL on OOM.
 */
layout_node_t *layout_split(layout_node_t *leaf,
                              layout_node_type_t dir, float ratio,
                              uint32_t new_pane_id);

/* ── Removal ─────────────────────────────────────────────────────────────── */

/*
 * Remove a leaf from the tree.
 * The sibling of the removed pane takes its parent split node's rect.
 * *root_ptr is updated if the root changes.
 * node and its parent split node are freed; sibling is NOT freed.
 */
void layout_remove(layout_node_t **root_ptr, layout_node_t *node);

/* ── Resize ──────────────────────────────────────────────────────────────── */

/*
 * Resize the entire layout to a new pixel rect.
 * All child rects are recomputed proportionally from stored split_ratios.
 */
void layout_resize_root(layout_node_t *root, int x, int y, int w, int h);

/* ── Queries ─────────────────────────────────────────────────────────────── */

/* Find the leaf node with the given pane_id, or NULL. */
layout_node_t *layout_find_pane(layout_node_t *root, uint32_t pane_id);

/* Iterate all leaf nodes in depth-first order. */
void layout_each_leaf(layout_node_t *root,
                       void (*cb)(layout_node_t *leaf, void *user),
                       void *user);

/* ── 직렬화 ──────────────────────────────────────────────────────────────── */

/*
 * 트리를 preorder 로 직렬화한다.
 * 포맷 (little-endian):
 *   LEAF : u8(0) u32(pane_id)
 *   SPLIT: u8(1=H, 2=V) f32(ratio) <left> <right>
 * @return 작성한 바이트 수. buf_size 부족 시 -1.
 */
int layout_serialize(const layout_node_t *root, uint8_t *buf, size_t buf_size);

/*
 * 위 포맷으로 직렬화된 바이트에서 트리를 복원한다.
 * rect 는 (0,0,0,0) 으로 초기화되며 이후 layout_resize_root 로 채워야 한다.
 * @return 루트 노드 또는 NULL (포맷 오류/메모리 부족).
 */
layout_node_t *layout_deserialize(const uint8_t *buf, size_t buf_size);

#endif /* TERMEMU_LAYOUT_H */
