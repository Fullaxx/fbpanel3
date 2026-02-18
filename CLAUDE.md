# fbpanel3 — Claude Code Instructions

## Version and documentation discipline

**Every commit that changes code must:**

1. **Increment the version** in `CMakeLists.txt`:
   ```cmake
   project(fbpanel VERSION X.Y.Z LANGUAGES C)
   ```
   Use semantic versioning:
   - **Patch** (Z): bug fixes, internal cleanup, refactors with no behaviour change
   - **Minor** (Y): new features, new plugins, API additions
   - **Major** (X): breaking changes to plugin API or config file format

2. **Update `CHANGELOG.md`** with a brief entry under the new version heading:
   ```markdown
   ## Version: X.Y.Z
   * Short description of what changed and why
   ```

3. **Check that `FBPANEL_REFACTOR.md`** (in the repo root's parent directory
   `/root/code/mygithub/FBPANEL_REFACTOR.md`) has its status markers updated
   if the commit completes a tracked refactor item (`[ ]` → `[x]`).

Documentation lives in these files — keep them in sync with code:
| File | Purpose |
|------|---------|
| `CHANGELOG.md` | User-visible history of every release |
| `FBPANEL_REFACTOR.md` | Tracks in-progress internal cleanup work |
| `CLAUDE.md` | This file — instructions for Claude Code sessions |

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Zero deprecated-API warnings is the bar. After any change, build and confirm
no new `warning: ... deprecated` lines appear.

## Release workflow

After completing a significant set of changes:
1. Bump version in `CMakeLists.txt`
2. Update `CHANGELOG.md`
3. Commit and tag: `git tag vX.Y.Z`
4. Push tag: `git push origin vX.Y.Z`
5. The GitHub Actions workflow in `fbpanel_builder` picks up the tag,
   builds `.deb` packages for all 6 distros, and publishes the GitHub Release.

## Code style

- C99, GTK3. No GTK2 deprecated APIs.
- `static` on all functions not exported across translation units.
- `DBG()` / `ERR()` from `dbg.h` for debug traces (compiled out in Release).
- No `ENTER;` / `RET(x);` macros — these were removed in v8.3.0.
- No `#if 0` dead-code blocks — use git history instead.
