/**
 * @file xconf.h
 * @brief Lightweight configuration tree for fbpanel.
 *
 * An xconf tree is a parsed representation of the panel config file.
 * Each node has a name, an optional string value, and an ordered list
 * of child nodes.  Block nodes (name = NULL value) hold children; value
 * nodes (non-NULL value) hold a single string setting.
 *
 * Ownership rule for string values:
 *   - xconf_get_str()   → (transfer none)  pointer into the tree; do NOT g_free.
 *   - xconf_get_strdup() → (transfer full)  g_strdup copy; caller MUST g_free.
 *
 * See docs/XCONF_REFERENCE.md for the full API description and the
 * double-free case study (v8.3.23).
 */

#ifndef _XCONF_H_
#define _XCONF_H_

#include <glib.h>
#include <stdio.h>

/**
 * xconf - a single node in the configuration tree.
 *
 * @name:   Node name, always non-NULL (g_strdup'd; freed by xconf_del).
 * @value:  String value for leaf nodes, or NULL for block nodes.
 *          Owned by this node; freed by xconf_del.
 * @sons:   Ordered singly-linked list of child xconf nodes.
 *          Each element is an xconf* owned by this node.
 * @parent: Back-pointer to the parent node; NULL for the root.
 *          Not owned — do not free via this pointer.
 */
typedef struct _xconf
{
    gchar *name;
    gchar *value;
    GSList *sons;
    struct _xconf *parent;
} xconf;

/**
 * xconf_enum - one entry in a string-to-integer enum mapping table.
 *
 * @str:  String representation as it appears in the config file.
 *        The table must be terminated by an entry with str == NULL.
 * @desc: Human-readable description (used by the preferences UI).
 * @num:  Integer value used internally.
 */
typedef struct {
    gchar *str;
    gchar *desc;
    int num;
} xconf_enum;

/**
 * xconf_new - allocate a new isolated xconf node.
 * @name:  Node name; g_strdup'd into the node.
 * @value: String value, or NULL for a block node; g_strdup'd.
 *
 * Returns: (transfer full) newly allocated node; caller must eventually
 *   pass it to xconf_del() or xconf_append() (which transfers ownership).
 */
xconf *xconf_new(gchar *name, gchar *value);

/**
 * xconf_append - add @son as the last child of @parent.
 * @parent: Destination parent node; must not be NULL.
 * @son:    Node to append; must not be NULL.
 *          Ownership of @son transfers to @parent.
 *          Sets son->parent = parent.
 */
void xconf_append(xconf *parent, xconf *son);

/**
 * xconf_append_sons - move all children of @src to the end of @dst's sons.
 * @dst: Destination parent node.
 * @src: Source node whose sons list is drained.
 *       After the call src->sons is NULL; ownership of each son
 *       transfers to @dst.
 */
void xconf_append_sons(xconf *dst, xconf *src);

/**
 * xconf_unlink - remove @x from its parent's sons list.
 * @x: Node to unlink.  Sets x->parent = NULL.
 *
 * After unlinking, ownership of @x returns to the caller.
 * If @x has no parent, this is a no-op.
 */
void xconf_unlink(xconf *x);

/**
 * xconf_del - delete @x and, optionally, its entire subtree.
 * @x:         Node to delete.  May be NULL (no-op).
 * @sons_only: If TRUE, delete only child nodes and leave @x in place
 *             (with an empty sons list).  If FALSE, also unlink @x from
 *             its parent and free @x itself.
 *
 * Frees x->name, x->value, and all descendent nodes recursively.
 * After this call any pointer obtained via XCG(..., str) into this
 * subtree is a dangling pointer.
 */
void xconf_del(xconf *x, gboolean sons_only);

/**
 * xconf_set_value - replace @x's value with a copy of @value.
 * @x:     Target node.
 * @value: New string value; g_strdup'd into the node.
 *
 * Clears all child nodes (calls xconf_del sons_only=TRUE) before setting.
 */
void xconf_set_value(xconf *x, gchar *value);

/**
 * xconf_set_value_ref - replace @x's value with @value directly (no copy).
 * @x:     Target node.
 * @value: New string value; stored as-is (no g_strdup).
 *         @value must be a g_malloc-allocated string; the node takes
 *         ownership and will g_free it when the node is updated or deleted.
 *
 * Clears all child nodes before setting.
 */
void xconf_set_value_ref(xconf *x, gchar *value);

/**
 * xconf_get_value - return the raw string value of @x.
 * @x: Node to query.
 *
 * Returns: (transfer none) x->value; do not g_free.
 *   Returns NULL if @x is a block node.
 */
gchar *xconf_get_value(xconf *x);

/**
 * xconf_prn - serialize the xconf tree to @fp in config-file format.
 * @fp:        Output file.
 * @x:         Root of the subtree to print.
 * @n:         Current indentation level (0 for the root).
 * @sons_only: If TRUE, print only child nodes (omit @x's own name/value line
 *             and the surrounding braces).
 */
void xconf_prn(FILE *fp, xconf *x, int n, gboolean sons_only);

/**
 * xconf_find - find the @no-th child of @x with the given @name.
 * @x:    Parent node to search.  May be NULL (returns NULL).
 * @name: Child name to search for (case-insensitive).
 * @no:   Zero-based index among matching children (0 = first match).
 *
 * Returns: (transfer none) pointer into the tree, or NULL if not found.
 *   The returned pointer is valid as long as the tree is alive.
 *   Do not g_free the returned pointer.
 */
xconf *xconf_find(xconf *x, gchar *name, int no);

/**
 * xconf_dup - deep-copy the subtree rooted at @xc.
 * @xc: Root of the subtree to copy.  May be NULL (returns NULL).
 *
 * Returns: (transfer full) new root node; caller must xconf_del() it.
 */
xconf *xconf_dup(xconf *xc);

/**
 * xconf_cmp - compare two xconf trees for structural equality.
 * @a: First tree root.  May be NULL.
 * @b: Second tree root.  May be NULL.
 *
 * Returns: FALSE if trees are equal (or both NULL).
 *          TRUE  if trees differ in any name, value, or structure.
 *          TRUE  if exactly one is NULL.
 */
gboolean xconf_cmp(xconf *a, xconf *b);

/**
 * xconf_new_from_file - parse a config file into an xconf tree.
 * @fname: Path to the config file.
 * @name:  Name to assign to the root node.
 *
 * Returns: (transfer full) root node, or NULL if the file cannot be opened.
 *   Caller must xconf_del() the returned tree when done.
 */
xconf *xconf_new_from_file(gchar *fname, gchar *name);

/**
 * xconf_save_to_file - serialize an xconf tree to a file.
 * @fname: Path to write.  Created or truncated.
 * @xc:    Tree to serialize.
 */
void xconf_save_to_file(gchar *fname, xconf *xc);

/**
 * xconf_save_to_profile - serialize @xc to the current profile file.
 * @xc: Tree to serialize.
 *
 * Uses panel_get_profile_file() to determine the path.
 */
void xconf_save_to_profile(xconf *xc);

/**
 * xconf_get - find or create the child of @xc named @name.
 * @xc:   Parent node.  May be NULL (returns NULL).
 * @name: Child name to find or create.
 *
 * Returns: (transfer none) existing or newly-created child node.
 *   Ownership stays with the tree; do not xconf_del() the returned pointer.
 */
xconf *xconf_get(xconf *xc, gchar *name);

/**
 * xconf_get_int - read @x's value as a decimal integer.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to strtol(x->value, ...) if non-NULL value.
 *       Left unchanged if @x is NULL or has no value.
 */
void xconf_get_int(xconf *x, int *val);

/**
 * xconf_get_enum - match @x's value against an enum table and store the result.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to the matching xconf_enum::num.
 *       Left unchanged if @x is NULL, has no value, or no match is found.
 * @e:   Enum mapping table, terminated by an entry with str == NULL.
 *       Comparison is case-insensitive.
 */
void xconf_get_enum(xconf *x, int *val, xconf_enum *e);

/**
 * xconf_get_str - read @x's value as a borrowed string pointer.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to x->value (a pointer into the tree).
 *       Left unchanged if @x is NULL or has no value.
 *
 * OWNERSHIP: (transfer none) — *val points into xconf-owned memory.
 * Do NOT call g_free(*val).  The pointer is valid only while the tree
 * node @x is alive.
 */
void xconf_get_str(xconf *x, gchar **val);

/**
 * xconf_get_strdup - read @x's value as an independent copy.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to g_strdup(x->value).
 *       Left unchanged if @x is NULL or has no value.
 *
 * OWNERSHIP: (transfer full) — caller MUST call g_free(*val) when done.
 */
void xconf_get_strdup(xconf *x, gchar **val);

/**
 * xconf_set_int - store an integer as a decimal string in @x.
 * @x:   Target node.
 * @val: Integer to store.
 *
 * Clears child nodes before setting.
 */
void xconf_set_int(xconf *x, int val);

/**
 * xconf_set_enum - store an integer enum value as its string representation.
 * @x:   Target node.
 * @val: Integer value to encode.
 * @e:   Enum mapping table; the matching xconf_enum::str is stored.
 *       If no match is found, @x is unchanged.
 */
void xconf_set_enum(xconf *x, int val, xconf_enum *e);

/**
 * XCG - config get: read a named child value from @xc using type suffix.
 *
 * Expands to: xconf_get_<type>(xconf_find(xc, name, 0), var, ##extra)
 *
 * Common usages:
 *   XCG(xc, "key", &var, str)          // (transfer none): do NOT g_free var
 *   XCG(xc, "key", &var, strdup)       // (transfer full): MUST g_free var
 *   XCG(xc, "key", &var, int)          // parses decimal integer
 *   XCG(xc, "key", &var, enum, table)  // maps string to int via enum table
 *
 * If the key is not found, xconf_find returns NULL and xconf_get_<type>
 * is a no-op, leaving *var at its initial (default) value.
 */
#define XCG(xc, name, var, type, extra...)                      \
    xconf_get_ ## type(xconf_find(xc, name, 0), var, ## extra)

/**
 * XCS - config set: write a value to a named child of @xc using type suffix.
 *
 * Expands to: xconf_set_<type>(xconf_get(xc, name), var, ##extra)
 *
 * xconf_get() creates the child node if it does not exist.
 *
 * Common usages:
 *   XCS(xc, "key", str_val, value)     // stores str_val (no copy)
 *   XCS(xc, "key", int_val, int)       // stores decimal representation
 *   XCS(xc, "key", int_val, enum, tbl) // stores string for enum value
 */
#define XCS(xc, name, var, type, extra...)                \
    xconf_set_ ## type(xconf_get(xc, name), var, ## extra)


#endif
