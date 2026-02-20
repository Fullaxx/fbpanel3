# fbpanel3 — xconf Reference

xconf is fbpanel3's lightweight configuration tree.  It stores the parsed
panel config file as an in-memory tree of nodes, each with a name, an
optional string value, and an ordered list of child nodes.

---

## 1. Data Structures

### `xconf` node

```c
typedef struct _xconf {
    gchar          *name;    // node name (g_strdup'd; freed by xconf_del)
    gchar          *value;   // string value, or NULL for block nodes
    GSList         *sons;    // ordered list of child xconf* nodes
    struct _xconf  *parent;  // back-pointer to parent node, or NULL for root
} xconf;
```

A node with `value == NULL` is a **block** node (corresponds to a `name { }` block
in the config file).  A node with `value != NULL` is a **value** node (corresponds
to a `name = value` line).

All strings inside an xconf node are owned by the node and freed when the node
is deleted.  Callers who read string values via `XCG(..., str)` receive a pointer
into xconf-owned memory — they must not free it.

### `xconf_enum` — enum mapping table

```c
typedef struct {
    gchar *str;    // string representation in config file
    gchar *desc;   // human-readable description (for UI)
    int    num;    // integer value used internally
} xconf_enum;
```

Tables of these are defined in `panel.c` for common enum-valued config keys
(`allign_enum`, `edge_enum`, `widthtype_enum`, etc.).  The table is terminated
by an entry with `str == NULL`.

---

## 2. Config File Format

```
# Lines starting with # are comments and are ignored.

Global {
    edge       = bottom
    widthtype  = percent
    width      = 100
    height     = 26
    transparent = 0
    tintcolor  = 0x3f3f3f
}

Plugin {
    type = taskbar
    Config {
        ShowIconNames = 1
    }
}

Plugin {
    type = dclock
    Config {
        ClockFmt = %H:%M
    }
}
```

Rules:
- Block names are alphanumeric.  The `{` must be on the same line.
- `}` must be on its own line.
- Values are everything after `=` to end of line (whitespace-stripped).
- Keys are case-insensitive in `xconf_find()`.
- Maximum line length: 256 characters.

---

## 3. Core API

### Construction and Destruction

```c
// Allocate a new isolated node.  Both name and value are g_strdup'd.
// Returns (transfer full) — caller owns the node.
xconf *xconf_new(gchar *name, gchar *value);

// Parse a config file into a new tree rooted at a node named @name.
// Returns (transfer full) root node, or NULL on open failure.
xconf *xconf_new_from_file(gchar *fname, gchar *name);

// Deep-copy a subtree.
// Returns (transfer full) new root.
xconf *xconf_dup(xconf *xc);

// Delete @x and its entire subtree.  If sons_only=TRUE, deletes only
// child nodes and leaves @x itself in place (with empty sons list).
// Unlinks @x from its parent before freeing.
void xconf_del(xconf *x, gboolean sons_only);
```

### Tree Manipulation

```c
// Append @son as the last child of @parent.
// Sets son->parent = parent.
// Ownership of @son transfers to @parent.
void xconf_append(xconf *parent, xconf *son);

// Append all sons of @src to @dst.  After the call, src->sons is NULL.
// Ownership of each son transfers to @dst.
void xconf_append_sons(xconf *dst, xconf *src);

// Remove @x from its parent's sons list.
// Sets x->parent = NULL.  Caller takes back ownership of @x.
void xconf_unlink(xconf *x);
```

### Search

```c
// Find the @no-th child of @x with the given @name (case-insensitive).
// @no = 0 returns the first match.
// Returns (transfer none) pointer into the tree, or NULL if not found.
xconf *xconf_find(xconf *x, gchar *name, int no);

// Find the child named @name, creating it if it does not exist.
// Returns (transfer none) pointer into the tree.
xconf *xconf_get(xconf *xc, gchar *name);
```

### Value Get/Set

```c
// Return the raw string value of @x.
// Returns (transfer none) — do not g_free.
gchar *xconf_get_value(xconf *x);

// If @x is non-NULL and has a value, set *val = x->value.
// (transfer none): *val points into xconf-owned memory.
void xconf_get_str(xconf *x, gchar **val);

// If @x is non-NULL and has a value, set *val = g_strdup(x->value).
// (transfer full): caller must g_free(*val) when done.
void xconf_get_strdup(xconf *x, gchar **val);

// If @x is non-NULL and has a value, parse it as an integer and set *val.
void xconf_get_int(xconf *x, int *val);

// If @x is non-NULL, match its value against the enum table @e and set *val.
void xconf_get_enum(xconf *x, int *val, xconf_enum *e);

// Replace x->value with a g_strdup of @value (clears x->sons first).
void xconf_set_value(xconf *x, gchar *value);

// Replace x->value with @value directly (takes ownership; no g_strdup).
// @value must be a g_malloc-allocated string; x will g_free it.
void xconf_set_value_ref(xconf *x, gchar *value);

// Replace x->value with a decimal string representation of @i.
void xconf_set_int(xconf *x, int i);

// Encode @val as a string using enum table @e and store in x->value.
void xconf_set_enum(xconf *x, int val, xconf_enum *e);
```

### Serialization

```c
// Print the xconf tree to @fp in config-file format.
// If sons_only=TRUE, print only children (not @x itself).
void xconf_prn(FILE *fp, xconf *x, int n, gboolean sons_only);

// Save the tree to the profile file path.
void xconf_save_to_file(gchar *fname, xconf *xc);
void xconf_save_to_profile(xconf *xc);
```

### Comparison

```c
// Return TRUE if trees @a and @b differ (names, values, or structure).
// Returns FALSE if both NULL.  TRUE if exactly one is NULL.
gboolean xconf_cmp(xconf *a, xconf *b);
```

---

## 4. XCG / XCS Macros

The `XCG` and `XCS` macros provide a concise syntax for reading and writing
config values using the function-suffix naming convention.

### `XCG` — config **g**et

```c
#define XCG(xc, name, var, type, extra...)  \
    xconf_get_ ## type(xconf_find(xc, name, 0), var, ## extra)
```

Usage examples:

```c
gchar *filename = NULL;
XCG(xc, "FileName", &filename, str);    // transfer none: do NOT g_free
// filename == NULL if key not found

gchar *copy = NULL;
XCG(xc, "FileName", &copy, strdup);     // transfer full: MUST g_free(copy)

int val = 0;
XCG(xc, "ShowIcons", &val, int);        // parses decimal integer

int edge = EDGE_BOTTOM;
XCG(xc, "edge", &edge, enum, edge_enum); // maps string to int via enum table
```

If the key is not found, `xconf_find()` returns NULL.  All `xconf_get_*`
functions handle NULL gracefully (they do nothing), so `*var` retains its
initial value — a safe default.

### `XCS` — config **s**et

```c
#define XCS(xc, name, var, type, extra...)  \
    xconf_set_ ## type(xconf_get(xc, name), var, ## extra)
```

`xconf_get()` creates the node if it does not exist.

Usage examples:
```c
XCS(xc, "FileName", filename, value);    // sets node value to filename (no dup)
XCS(xc, "ShowIcons", val, int);          // sets node value to decimal string
XCS(xc, "edge", edge, enum, edge_enum); // sets node value to string for enum
```

---

## 5. Ownership Rule Summary

| Pattern | Who frees | When |
|---|---|---|
| `XCG(..., str)` | **Nobody** — xconf owns it | When `xconf_del()` is called |
| `XCG(..., strdup)` | **Caller** via `g_free()` | When the caller is done |
| `xconf_new()` result | **Caller** (or parent via xconf_append) | `xconf_del()` |
| `xconf_dup()` result | **Caller** | `xconf_del()` |
| `xconf_find()` result | **Nobody** — tree owns it | (transfer none) |
| `xconf_get()` result | **Nobody** — tree owns it | (transfer none) |

**Critical invariant**: Never pass a pointer obtained via `XCG(..., str)` to
`g_free()`.  This is the double-free bug that caused the v8.3.23 crash.
