/**
 * @file xconf.c
 * @brief Lightweight configuration tree — implementation.
 *
 * The config file format is a simple block/key=value text file:
 *
 *   Global {
 *       edge = bottom
 *       width = 100
 *   }
 *   Plugin {
 *       type = taskbar
 *       Config {
 *           ShowIcons = 1
 *       }
 *   }
 *
 * Rules:
 *   - Lines starting with '#' or empty lines are ignored.
 *   - Block: "name {" on one line, "}" on its own line.
 *   - Value: "name = value" (everything after '=' to end-of-line).
 *   - Key names are case-insensitive (xconf_find uses strcasecmp).
 *   - Maximum line length: LINE_LENGTH (256) characters.
 *
 * Ownership: every xconf node owns its name and value strings (g_strdup'd).
 * xconf_del() frees everything recursively.  See xconf.h for the
 * str-vs-strdup rule applied by xconf_get_str / xconf_get_strdup.
 */

#include <stdlib.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

#include "xconf.h"
#include "panel.h"

//#define DEBUGPRN
#include "dbg.h"


/** Parser token types returned by read_line(). */
enum { LINE_NONE, LINE_BLOCK_START, LINE_BLOCK_END, LINE_VAR };

/** Maximum length of a single config file line (including terminator). */
#define LINE_LENGTH 256

/**
 * line - internal parser state for a single config file line.
 * @type: One of LINE_NONE / LINE_BLOCK_START / LINE_BLOCK_END / LINE_VAR.
 * @str:  Raw line buffer (mutated in-place during tokenisation).
 * @t:    Pointers into @str: t[0] = key/block name, t[1] = value string.
 */
typedef struct {
    int type;
    gchar str[LINE_LENGTH];
    gchar *t[2];
} line;


/**
 * xconf_new - allocate a new isolated xconf node.
 * @name:  Node name; g_strdup'd.
 * @value: String value or NULL for a block node; g_strdup'd.
 *
 * Returns: (transfer full) newly allocated node.
 */
xconf *xconf_new(gchar *name, gchar *value)
{
    xconf *x;

    x = g_new0(xconf, 1);
    x->name = g_strdup(name);
    x->value = g_strdup(value);
    return x;
}

/**
 * xconf_append_sons - move all children of @src to the end of @dst's sons.
 * @dst: Destination parent node.
 * @src: Source node whose sons list is drained (src->sons set to NULL).
 *
 * Ownership of each son transfers to @dst.
 * Both @dst and @src may be NULL (no-op).
 */
void xconf_append_sons(xconf *dst, xconf *src)
{
    GSList *e;
    xconf *tmp;

    if (!dst || !src)
        return;
    for (e = src->sons; e; e = g_slist_next(e))
    {
        tmp = e->data;
        tmp->parent = dst;
    }
    dst->sons = g_slist_concat(dst->sons, src->sons);
    src->sons = NULL;
}

/**
 * xconf_append - add @son as the last child of @parent.
 * @parent: Destination parent node; may be NULL (no-op).
 * @son:    Node to append; may be NULL (no-op).
 *          Ownership of @son transfers to @parent.
 *
 * Note: appending requires traversing the full sons list — O(n).
 * Acceptable for config-file sizes; not suitable for hot paths.
 */
void xconf_append(xconf *parent, xconf *son)
{
    if (!parent || !son)
        return;
    son->parent = parent;
    /* appending requires traversing all list to the end, which is not
     * efficient, but for v 1.0 it's ok*/
    parent->sons = g_slist_append(parent->sons, son);
}

/**
 * xconf_unlink - remove @x from its parent's sons list.
 * @x: Node to unlink.  Sets x->parent = NULL.
 *
 * If @x has no parent the call is a no-op.
 * Ownership of @x returns to the caller after unlinking.
 */
void xconf_unlink(xconf *x)
{
    if (x && x->parent)
    {
        x->parent->sons = g_slist_remove(x->parent->sons, x);
        x->parent = NULL;
    }
}

/**
 * xconf_del - delete @x and its subtree.
 * @x:         Node to delete.  May be NULL (no-op).
 * @sons_only: If TRUE, delete only child nodes; leave @x itself alive with
 *             an empty sons list.  If FALSE, also unlink @x from its parent
 *             and free @x, x->name, and x->value.
 *
 * After this call, any pointer obtained via xconf_get_str() / XCG(..., str)
 * into this subtree is a dangling pointer.
 */
void xconf_del(xconf *x, gboolean sons_only)
{
    GSList *s;
    xconf *x2;

    if (!x)
        return;
    DBG("%s %s\n", x->name, x->value);
    for (s = x->sons; s; s = g_slist_delete_link(s, s))
    {
        x2 = s->data;
        x2->parent = NULL;
        xconf_del(x2, FALSE);
    }
    x->sons = NULL;
    if (!sons_only)
    {
        g_free(x->name);
        g_free(x->value);
        xconf_unlink(x);
        g_free(x);
    }
}

/**
 * xconf_set_value - replace @x's value with a copy of @value.
 * @x:     Target node.
 * @value: New string value; g_strdup'd.  The old value and all child nodes
 *         are freed first.
 */
void xconf_set_value(xconf *x, gchar *value)
{
    xconf_del(x, TRUE);
    g_free(x->value);
    x->value = g_strdup(value);

}

/**
 * xconf_set_value_ref - replace @x's value with @value directly (no copy).
 * @x:     Target node.
 * @value: New string value stored as-is (no g_strdup).
 *         Must be a g_malloc-allocated string; the node takes ownership and
 *         will g_free it on the next update or when the node is deleted.
 *
 * The old value and all child nodes are freed first.
 */
void xconf_set_value_ref(xconf *x, gchar *value)
{
    xconf_del(x, TRUE);
    g_free(x->value);
    x->value = value;

}

/**
 * xconf_set_int - store @i as a decimal string in @x.
 * @x: Target node.
 * @i: Integer to store.
 *
 * Clears child nodes before setting.
 */
void xconf_set_int(xconf *x, int i)
{
    xconf_del(x, TRUE);
    g_free(x->value);
    x->value = g_strdup_printf("%d", i);
}

/**
 * xconf_get - find or create the child of @xc named @name.
 * @xc:   Parent node.  May be NULL (returns NULL).
 * @name: Child name to find or create (case-insensitive search).
 *
 * Returns: (transfer none) existing or newly-created child node owned by
 *   the tree.  Do not xconf_del() the returned pointer.
 */
xconf *
xconf_get(xconf *xc, gchar *name)
{
    xconf *ret;

    if (!xc)
        return NULL;
    if ((ret = xconf_find(xc, name, 0)))
        return ret;
    ret = xconf_new(name, NULL);
    xconf_append(xc, ret);
    return ret;
}

/**
 * xconf_get_value - return the raw string value of @x.
 * @x: Node to query.
 *
 * Returns: (transfer none) x->value; do not g_free.
 *   Returns NULL if @x is a block node (no value set).
 */
gchar *xconf_get_value(xconf *x)
{
    return x->value;
}

/**
 * xconf_prn - serialize the xconf tree to @fp in config-file format.
 * @fp:        Output file.
 * @x:         Root of the subtree to print.
 * @n:         Indentation level (0 = outermost; increased for each block).
 * @sons_only: If TRUE, print only child nodes (skip @x's header and braces).
 */
void xconf_prn(FILE *fp, xconf *x, int n, gboolean sons_only)
{
    int i;
    GSList *s;
    xconf *x2;

    if (!sons_only)
    {
        for (i = 0; i < n; i++)
            fprintf(fp, "    ");
        fprintf(fp, "%s", x->name);
        if (x->value)
            fprintf(fp, " = %s\n", x->value);
        else
            fprintf(fp, " {\n");
        n++;
    }
    for (s = x->sons; s; s = g_slist_next(s))
    {
        x2 = s->data;
        xconf_prn(fp, x2, n, FALSE);
    }
    if (!sons_only && !x->value)
    {
        n--;
        for (i = 0; i < n; i++)
            fprintf(fp, "    ");
        fprintf(fp, "}\n");
    }

}

/**
 * xconf_find - find the @no-th child of @x with the given @name.
 * @x:    Parent node.  May be NULL (returns NULL).
 * @name: Child name (case-insensitive comparison via strcasecmp).
 * @no:   Zero-based index of the desired match (0 = first match).
 *
 * Returns: (transfer none) pointer into the tree, or NULL if not found.
 */
xconf *xconf_find(xconf *x, gchar *name, int no)
{
    GSList *s;
    xconf *x2;

    if (!x)
        return NULL;
    for (s = x->sons; s; s = g_slist_next(s))
    {
        x2 = s->data;
        if (!strcasecmp(x2->name, name))
        {
            if (!no)
                return x2;
            no--;
        }
    }
    return NULL;
}


/**
 * xconf_get_str - set *@val to a borrowed pointer into @x's value.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to x->value if non-NULL.
 *       Left unchanged if @x is NULL or has no value.
 *
 * OWNERSHIP: (transfer none) — *val points into xconf-owned memory.
 * Do NOT call g_free(*val).  The pointer becomes invalid when xconf_del()
 * is called on @x or any ancestor node.
 */
void xconf_get_str(xconf *x, gchar **val)
{
    if (x && x->value)
        *val = x->value;
}


/**
 * xconf_get_strdup - set *@val to a g_strdup copy of @x's value.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to g_strdup(x->value) if non-NULL.
 *       Left unchanged if @x is NULL or has no value.
 *
 * OWNERSHIP: (transfer full) — caller MUST call g_free(*val) when done.
 */
void xconf_get_strdup(xconf *x, gchar **val)
{
    if (x && x->value)
        *val = g_strdup(x->value);
}


/**
 * xconf_get_int - parse @x's value as a decimal integer.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to strtol(x->value, NULL, 0).
 *       Left unchanged if @x is NULL or has no value.
 *       Accepts decimal, octal (0-prefix), and hex (0x-prefix).
 */
void xconf_get_int(xconf *x, int *val)
{
    gchar *s;

    if (!x)
        return;
    s = xconf_get_value(x);
    if (!s)
        return;
    *val = strtol(s, NULL, 0);
}

/**
 * xconf_get_enum - match @x's value against an enum table.
 * @x:   Node to read.  May be NULL (no-op).
 * @val: Output parameter; set to the matching xconf_enum::num.
 *       Left unchanged if @x is NULL, has no value, or no match is found.
 * @p:   Enum mapping table terminated by an entry with str == NULL.
 *       Comparison is case-insensitive.
 */
void xconf_get_enum(xconf *x, int *val, xconf_enum *p)
{
    gchar *s;

    if (!x)
        return;
    s = xconf_get_value(x);
    if (!s)
        return;
    while (p && p->str)
    {
        DBG("cmp %s %s\n", p->str, s);
        if (!strcasecmp(p->str, s))
        {
            *val = p->num;
            return;
        }
        p++;
    }
}

/**
 * xconf_set_enum - store the string representation of integer @val in @x.
 * @x:   Target node.
 * @val: Integer value to encode.
 * @p:   Enum mapping table; the matching xconf_enum::str is stored via
 *       xconf_set_value().  If no entry matches, @x is unchanged.
 */
void
xconf_set_enum(xconf *x, int val, xconf_enum *p)
{
    if (!x)
        return;

    while (p && p->str)
    {
        if (val == p->num)
        {
            xconf_set_value(x, p->str);
            return;
        }
        p++;
    }
}

/**
 * read_line - read and tokenise the next meaningful line from @fp.
 * @fp: Input file.  May be NULL (returns LINE_NONE).
 * @s:  Output line struct; s->type, s->t[0], s->t[1] are set on success.
 *
 * Skips blank lines and comment lines (starting with '#').
 * Mutates s->str in place during tokenisation.
 *
 * Returns: the token type (LINE_NONE / LINE_BLOCK_START / LINE_BLOCK_END /
 *   LINE_VAR).
 *
 * Note: on an unrecognised token, logs ERR() and returns LINE_NONE via the
 * loop's implicit fall-through (the loop breaks after one token).
 */
static int
read_line(FILE *fp, line *s)
{
    gchar *tmp, *tmp2;

    s->type = LINE_NONE;
    if (!fp)
        return s->type;
    while (fgets(s->str, LINE_LENGTH, fp)) {
        g_strstrip(s->str);

        if (s->str[0] == '#' || s->str[0] == 0) {
            continue;
        }
        DBG( ">> %s\n", s->str);
        if (!g_ascii_strcasecmp(s->str, "}")) {
            s->type = LINE_BLOCK_END;
            break;
        }

        s->t[0] = s->str;
        for (tmp = s->str; isalnum(*tmp); tmp++);
        for (tmp2 = tmp; isspace(*tmp2); tmp2++);
        if (*tmp2 == '=') {
            for (++tmp2; isspace(*tmp2); tmp2++);
            s->t[1] = tmp2;
            *tmp = 0;
            s->type = LINE_VAR;
        } else if  (*tmp2 == '{') {
            *tmp = 0;
            s->type = LINE_BLOCK_START;
        } else {
            ERR( "parser: unknown token: '%c'\n", *tmp2);
        }
        break;
    }
    return s->type;

}


/**
 * read_block - recursively parse a block from @fp into an xconf subtree.
 * @fp:   Input file positioned just after the opening '{'.
 * @name: Name to assign to the new root node.
 *
 * Reads lines until LINE_BLOCK_END ('}') or EOF.  Each LINE_BLOCK_START
 * triggers a recursive call; each LINE_VAR creates a leaf node.
 *
 * Returns: (transfer full) root node of the parsed block.
 *
 * On an unrecognised token the loop logs an error and breaks, returning
 * the partially-built tree.  The caller will use whatever was parsed.
 */
static xconf *
read_block(FILE *fp, gchar *name)
{
    line s;
    xconf *x, *xs;

    x = xconf_new(name, NULL);
    while (read_line(fp, &s) != LINE_NONE)
    {
        if (s.type == LINE_BLOCK_START)
        {
            xs = read_block(fp, s.t[0]);
            xconf_append(x, xs);
        }
        else if (s.type == LINE_BLOCK_END)
            break;
        else if (s.type == LINE_VAR)
        {
            xs = xconf_new(s.t[0], s.t[1]);
            xconf_append(x, xs);
        }
        else
        {
            ERR("xconf: syntax error in config block '%s' — unknown token type %d\n",
                name, s.type);
            break;
        }
    }
    return x;
}

/**
 * xconf_new_from_file - parse a config file into an xconf tree.
 * @fname: Path to the config file.
 * @name:  Name to assign to the root node.
 *
 * Returns: (transfer full) root node, or NULL if the file cannot be opened.
 *   Caller must xconf_del() the returned tree when done.
 */
xconf *xconf_new_from_file(gchar *fname, gchar *name)
{
    FILE *fp = fopen(fname, "r");
    xconf *ret = NULL;
    if (fp)
    {
        ret = read_block(fp, name);
        fclose(fp);
    }
    //xconf_prn(stdout, ret, 0, FALSE);
    return ret;
}

/**
 * xconf_save_to_file - serialize an xconf tree to a file.
 * @fname: Path to write.  Created or truncated.
 * @xc:    Tree to serialize (sons_only = TRUE: skip the root node header).
 */
void xconf_save_to_file(gchar *fname, xconf *xc)
{
    FILE *fp = fopen(fname, "w");

    if (fp)
    {
        xconf_prn(fp, xc, 0, TRUE);
        fclose(fp);
    }
}

/**
 * xconf_save_to_profile - serialize @xc to the current profile file.
 * @xc: Tree to serialize.
 *
 * The profile file path is determined by panel_get_profile_file().
 */
void
xconf_save_to_profile(xconf *xc)
{
    xconf_save_to_file(panel_get_profile_file(), xc);
}

/**
 * xconf_dup - deep-copy the subtree rooted at @xc.
 * @xc: Root of the subtree to copy.  May be NULL (returns NULL).
 *
 * Returns: (transfer full) new root node with all descendants copied.
 *   Caller must xconf_del() it when done.
 */
xconf *xconf_dup(xconf *xc)
{
    xconf *ret, *son;
    GSList *s;

    if (!xc)
        return NULL;
    ret = xconf_new(xc->name, xc->value);
    for (s = xc->sons; s; s = g_slist_next(s))
    {
        son = s->data;
        xconf_append(ret, xconf_dup(son));
    }
    return ret;
}

/**
 * xconf_cmp - compare two xconf trees for structural equality.
 * @a: First tree root.  May be NULL.
 * @b: Second tree root.  May be NULL.
 *
 * Returns: FALSE if trees are equal (including both-NULL case).
 *          TRUE  if trees differ in any name, value, or child structure.
 *          TRUE  if exactly one argument is NULL.
 *
 * The NULL-handling uses explicit !a && !b / !a || !b tests for clarity.
 */
gboolean
xconf_cmp(xconf *a, xconf *b)
{
    GSList *as, *bs;

    if (!a && !b)
        return FALSE;
    if (!a || !b)
        return TRUE;

    if (g_ascii_strcasecmp(a->name, b->name))
        return TRUE;

    if (g_strcmp0(a->value, b->value))
        return TRUE;
    for (as = a->sons, bs = b->sons; as && bs;
         as = g_slist_next(as), bs = g_slist_next(bs))
    {
        if (xconf_cmp(as->data, bs->data))
            return TRUE;
    }
    return (as != bs);
}
