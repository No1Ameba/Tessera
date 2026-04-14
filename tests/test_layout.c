/*
 * test_layout.c — Unit tests for the tiling layout tree.
 * No display or OpenGL required.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../src/client/ui/layout.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) { g_pass++; }                            \
        else {                                              \
            fprintf(stderr, "FAIL [%s:%d] %s\n",           \
                    __FILE__, __LINE__, msg);               \
            g_fail++;                                       \
        }                                                   \
    } while (0)

/* ── Helper: count leaves via each_leaf ──────────────────────────────────── */

static int leaf_count;
static void count_cb(layout_node_t *n, void *u)
{
    (void)n; (void)u; leaf_count++;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_create_leaf(void)
{
    layout_node_t *root = layout_create_leaf(1, 0, 0, 800, 600);
    CHECK(root != NULL,              "create_leaf returns non-NULL");
    CHECK(root->type == LAYOUT_LEAF, "type is LEAF");
    CHECK(root->pane_id == 1,        "pane_id set correctly");
    CHECK(root->rect.x == 0,         "rect.x == 0");
    CHECK(root->rect.y == 0,         "rect.y == 0");
    CHECK(root->rect.w == 800,       "rect.w == 800");
    CHECK(root->rect.h == 600,       "rect.h == 600");
    CHECK(root->parent == NULL,      "root has no parent");
    layout_destroy(root);
}

static void test_split_horizontal(void)
{
    layout_node_t *root = layout_create_leaf(1, 0, 0, 800, 600);
    layout_node_t *new_leaf = layout_split(root, LAYOUT_SPLIT_H, 0.5f, 2);

    CHECK(new_leaf != NULL,                 "split returns new leaf");
    CHECK(root->parent != NULL,             "original leaf has a parent now");
    CHECK(root->parent == new_leaf->parent, "both share the same parent");

    layout_node_t *split = root->parent;
    CHECK(split->type == LAYOUT_SPLIT_H, "parent is SPLIT_H");
    CHECK(split->children[0] == root,    "children[0] is original");
    CHECK(split->children[1] == new_leaf,"children[1] is new leaf");

    /* Left child: x=0, y=0, w=400, h=600 */
    CHECK(root->rect.x == 0,   "left child x==0");
    CHECK(root->rect.y == 0,   "left child y==0");
    CHECK(root->rect.w == 400, "left child w==400");
    CHECK(root->rect.h == 600, "left child h==600");

    /* Right child: x=400, y=0, w=400, h=600 */
    CHECK(new_leaf->rect.x == 400, "right child x==400");
    CHECK(new_leaf->rect.y == 0,   "right child y==0");
    CHECK(new_leaf->rect.w == 400, "right child w==400");
    CHECK(new_leaf->rect.h == 600, "right child h==600");

    layout_destroy(split);
}

static void test_split_vertical(void)
{
    layout_node_t *root = layout_create_leaf(1, 0, 0, 800, 600);
    layout_node_t *bot  = layout_split(root, LAYOUT_SPLIT_V, 0.5f, 2);

    CHECK(bot != NULL, "vertical split returns new leaf");

    /* Top: x=0, y=0, w=800, h=300 */
    CHECK(root->rect.x == 0,   "top x==0");
    CHECK(root->rect.y == 0,   "top y==0");
    CHECK(root->rect.w == 800, "top w==800");
    CHECK(root->rect.h == 300, "top h==300");

    /* Bottom: x=0, y=300, w=800, h=300 */
    CHECK(bot->rect.x == 0,   "bottom x==0");
    CHECK(bot->rect.y == 300, "bottom y==300");
    CHECK(bot->rect.w == 800, "bottom w==800");
    CHECK(bot->rect.h == 300, "bottom h==300");

    layout_destroy(root->parent);
}

static void test_three_panes(void)
{
    /* Create 800x600 root, split horizontally → left(pane 1) + right(pane 2) */
    layout_node_t *root  = layout_create_leaf(1, 0, 0, 800, 600);
    layout_node_t *right = layout_split(root, LAYOUT_SPLIT_H, 0.5f, 2);
    CHECK(right != NULL, "first split succeeds");

    /* Split the right pane vertically → top-right(pane 2) + bottom-right(pane 3) */
    layout_node_t *bot_right = layout_split(right, LAYOUT_SPLIT_V, 0.5f, 3);
    CHECK(bot_right != NULL, "second split succeeds");

    /* The tree root is the horizontal split node. */
    layout_node_t *tree_root = root->parent; /* SPLIT_H */
    CHECK(tree_root->type == LAYOUT_SPLIT_H, "tree root is SPLIT_H");

    /* Count leaves. */
    leaf_count = 0;
    layout_each_leaf(tree_root, count_cb, NULL);
    CHECK(leaf_count == 3, "three panes in layout");

    /* Find each pane. */
    CHECK(layout_find_pane(tree_root, 1) == root,      "find pane 1");
    CHECK(layout_find_pane(tree_root, 2) == right,     "find pane 2");
    CHECK(layout_find_pane(tree_root, 3) == bot_right, "find pane 3");
    CHECK(layout_find_pane(tree_root, 99) == NULL,     "find unknown → NULL");

    layout_destroy(tree_root);
}

static void test_remove_pane(void)
{
    /*
     * Build:  SPLIT_H
     *           ├─ leaf(pane=1)   [left]
     *           └─ leaf(pane=2)   [right]
     * Remove leaf(pane=1) → root becomes leaf(pane=2) with the split's rect.
     */
    layout_node_t *root  = layout_create_leaf(1, 0, 0, 800, 600);
    layout_node_t *right = layout_split(root, LAYOUT_SPLIT_H, 0.5f, 2);
    layout_node_t *split = root->parent;

    /* Remove left leaf. */
    layout_remove(&split, root); /* split pointer updated to sibling = right */

    CHECK(split == right, "after remove, root ptr → sibling");
    CHECK(split->type == LAYOUT_LEAF, "sibling is now root and is LEAF");
    CHECK(split->pane_id == 2, "sibling pane_id == 2");
    /* Sibling should have inherited the split node's rect (0,0,800,600). */
    CHECK(split->rect.x == 0,   "sibling rect.x==0");
    CHECK(split->rect.w == 800, "sibling rect.w==800");
    CHECK(split->rect.h == 600, "sibling rect.h==600");
    CHECK(split->parent == NULL, "sibling's parent is NULL (it's root)");

    layout_destroy(split);
}

static void test_resize(void)
{
    layout_node_t *root  = layout_create_leaf(1, 0, 0, 800, 600);
    layout_node_t *right = layout_split(root, LAYOUT_SPLIT_H, 0.5f, 2);
    layout_node_t *split = root->parent;

    /* Resize to 1200x800. */
    layout_resize_root(split, 0, 0, 1200, 800);

    CHECK(split->rect.w == 1200, "root rect.w == 1200 after resize");
    CHECK(split->rect.h == 800,  "root rect.h == 800 after resize");
    /* ratio=0.5, so each child gets 600 wide. */
    CHECK(root->rect.w == 600,   "left child w==600 after resize");
    CHECK(right->rect.x == 600,  "right child x==600 after resize");
    CHECK(right->rect.w == 600,  "right child w==600 after resize");
    CHECK(root->rect.h == 800,   "left child h==800 after resize");
    CHECK(right->rect.h == 800,  "right child h==800 after resize");

    layout_destroy(split);
}

static void test_split_bad_ratio(void)
{
    layout_node_t *root = layout_create_leaf(1, 0, 0, 800, 600);
    layout_node_t *r1   = layout_split(root, LAYOUT_SPLIT_H, 0.0f, 2);
    layout_node_t *r2   = layout_split(root, LAYOUT_SPLIT_H, 1.0f, 3);
    CHECK(r1 == NULL, "ratio=0.0 rejected");
    CHECK(r2 == NULL, "ratio=1.0 rejected");
    layout_destroy(root);
}

static void test_split_non_leaf_ignored(void)
{
    layout_node_t *root  = layout_create_leaf(1, 0, 0, 800, 600);
    layout_node_t *right = layout_split(root, LAYOUT_SPLIT_H, 0.5f, 2);
    (void)right;
    layout_node_t *split = root->parent;
    /* Splitting a non-leaf should return NULL. */
    layout_node_t *bad = layout_split(split, LAYOUT_SPLIT_V, 0.5f, 3);
    CHECK(bad == NULL, "splitting a non-leaf returns NULL");
    layout_destroy(split);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    test_create_leaf();
    test_split_horizontal();
    test_split_vertical();
    test_three_panes();
    test_remove_pane();
    test_resize();
    test_split_bad_ratio();
    test_split_non_leaf_ignored();

    printf("layout: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
