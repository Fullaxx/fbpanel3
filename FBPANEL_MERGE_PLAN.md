# fbpanel3 Merge Plan

## Overview

`fbpanel3` is the new unified successor to the defunct `fbpanel` GTK2 panel.
It merges the best work from two active forks into a single maintained project:

- **fbpanel3_berte** — GTK3 port (from https://github.com/berte/fbpanel3.git)
- **fbpanel_eleksir** — GTK2 quality-of-life improvements (from https://github.com/eleksir/fbpanel.git)

The target: a modern, GTK3-native panel with CMake build system, all eleksir
quality improvements, and a clear path for ongoing maintenance.

---

## Source Analysis

### fbpanel (original)
- GTK2, Python2 configure + Make
- Version 7.0
- 26 plugins
- 72 commits; upstream defunct

### fbpanel3_berte
- GTK3 port of fbpanel 7.0
- Python3 configure + Make (same structure as original)
- 5 commits; minimal divergence from fbpanel except GTK3 API changes
- Does NOT include eleksir improvements

### fbpanel_eleksir
- GTK2, version 7.2
- CMake 3.5+ build system (replaces Python2 configure)
- 28 plugins (adds `batterytext`, possibly renames `systray` → `tray`)
- Key improvements: extended launchbar (20→40 buttons), Xinerama rewrite,
  icon drawing in pager, Italian localization, deprecation fixes
- 2 commits (squashed from upstream)

### fbpanel3 (target)
- Currently empty (README only)
- Hosted at git@github.com:Fullaxx/fbpanel3.git

---

## Guiding Principles

1. **GTK3 is non-negotiable.** GTK2 is EOL. The end result must be GTK3.
2. **CMake is the build system.** eleksir already did the hard work; use it.
3. **Preserve all plugins.** Every plugin from both forks should be included.
4. **Prefer eleksir as the code base.** It is version 7.2 and has real
   improvements. Apply berte's GTK3 diffs on top.
5. **Keep changes reviewable.** Use logical git commits so future contributors
   can understand what changed and why.
6. **Version from 8.0.** The merged result is a new major effort, distinct
   from 7.x.

---

## High-Level Strategy

```
fbpanel_eleksir  (GTK2 + CMake + QoL)
       +
fbpanel3_berte   (GTK3 API diff only)
       =
fbpanel3         (GTK3 + CMake + QoL + all plugins)
```

The merge proceeds in phases. Each phase produces a working (or at minimum
compilable) state before moving to the next.

---

## Build Infrastructure: fbpanel_builder

`fbpanel_builder` (https://github.com/Fullaxx/fbpanel_builder) provides
Docker images pre-loaded with all build dependencies. Use these images for
all compile/test steps throughout this plan — do not build on the host.

### Supported distributions

`noble` `jammy` `focal` `trixie` `bookworm` `bullseye`
(Ubuntu 24.04/22.04/20.04 and Debian testing/12/11)

### Images

```
ghcr.io/fullaxx/fbpanel_builder:noble
ghcr.io/fullaxx/fbpanel_builder:jammy
... etc.
```

### Pre-installed build dependencies (all images)

```
build-essential  cmake  meson  ninja-build  doxygen
libgtk2.0-dev   libgtk-3-dev
libgdk-pixbuf-xlib-2.0-dev  (libgdk-pixbuf2.0-dev on focal)
python3-minimal  git  curl
```

### How to add a compile script for fbpanel3

Add `scripts/compile_fullaxx_fbpanel3.sh` to the fbpanel_builder repo:

```bash
#!/bin/bash
set -e

git clone https://github.com/Fullaxx/fbpanel3.git code
cd code/; mkdir build; cd build

cmake \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=lib \
  ..

make -j

mkdir /tmp/fbpanel
make install DESTDIR=/tmp/fbpanel
gzip -9 /tmp/fbpanel/usr/share/man/man1/fbpanel.1
( cd /tmp/fbpanel; tar cvf /tmp/fbpanel.tar * )
```

Also add it to `test_all.sh`:
```bash
docker run -it --rm -v ./scripts:/scripts ${IMAGE} /scripts/compile_fullaxx_fbpanel3.sh
```

### Running a build locally

```bash
# Pull image (or build locally with build_all.sh)
docker pull ghcr.io/fullaxx/fbpanel_builder:bookworm

# Run compile script against fbpanel3
docker run -it --rm \
  -v ./scripts:/scripts \
  ghcr.io/fullaxx/fbpanel_builder:bookworm \
  /scripts/compile_fullaxx_fbpanel3.sh
```

### Output artifact

Each compile script stages the install to `/tmp/fbpanel/` and produces
`/tmp/fbpanel.tar` inside the container. Extract it to inspect the layout:
```
usr/bin/fbpanel
usr/lib/fbpanel/
usr/share/fbpanel/
usr/share/man/man1/fbpanel.1.gz
```

---

## Phase 1 — Foundation: Import eleksir as Starting Point

**Goal:** Get eleksir's code into fbpanel3 with history intact and build
passing on GTK2 as a baseline checkpoint.

### Tasks

1. **Import eleksir source tree into fbpanel3.**
   Options (choose one):
   - `git subtree` / `git merge --allow-unrelated-histories` from
     fbpanel_eleksir remote
   - Copy files and commit with a clear message referencing the source

2. **Verify CMake build succeeds (GTK2 target) using fbpanel_builder.**
   ```bash
   docker run -it --rm \
     -v ./scripts:/scripts \
     ghcr.io/fullaxx/fbpanel_builder:bookworm \
     /scripts/compile_fullaxx_fbpanel3.sh
   ```
   Fix any cmake or compiler errors before proceeding.

3. **Audit the plugin list.**
   Confirm all 28 plugins are present and building:
   - Standard 26: battery, chart, cpu, dclock, deskno, deskno2, genmon,
     icons, image, launchbar, mem, mem2, menu, meter, net, pager,
     separator, space, systray, taskbar, tclock, unstable, user, volume,
     wincmd, (+ any others from fbpanel)
   - eleksir additions: batterytext, tray (if distinct from systray)

4. **Commit message:** `chore: import fbpanel_eleksir 7.2 as starting point`

---

## Phase 2 — GTK3 Port: Apply berte's API Changes

**Goal:** Port the codebase from GTK2 to GTK3, using fbpanel3_berte as the
reference diff.

### Known GTK2 → GTK3 API Changes (from berte)

| GTK2 API | GTK3 Replacement |
|---|---|
| `GdkColor` | `GdkRGBA` |
| `gdk_color_parse()` | `gdk_rgba_parse()` |
| `gtk_widget_shape_combine_mask()` | Deprecated; use cairo regions |
| `gdk_window_lookup()` | `gdk_x11_window_lookup_for_display()` |
| `gtk_box_new()` old signature | Orientation parameter required |
| `gtk_widget_get_mapped()` | Check widget state flags |
| Various `GTK_OBJECT()` casts | `G_OBJECT()` |
| `GtkStyle` direct access | `gtk_style_context_*` API |

### Tasks

1. **Diff fbpanel3_berte against fbpanel to extract only GTK3 changes.**
   ```
   diff -rq --exclude='*.pyc' fbpanel/ fbpanel3_berte/
   ```
   Produce a focused list of all changed lines that are GTK3-specific.

2. **Update CMakeLists.txt to target GTK3.**
   - Change `pkg_check_modules(GTK gtk+-2.0)` → `gtk+-3.0`
   - Update minimum version requirements
   - Add any GTK3-specific compile flags

3. **Apply GTK3 API changes to each source file in `panel/`.**
   Work file by file; commit each logical group of changes separately.

4. **Apply GTK3 API changes to each plugin in `plugins/`.**
   Plugins are the most likely source of breakage; test each one.
   Suggested order (simplest first):
   - separator, space (trivial, little GTK usage)
   - image, icons
   - cpu, mem, mem2, chart, meter, net (data display plugins)
   - battery, batterytext
   - dclock, tclock
   - deskno, deskno2, pager
   - launchbar, wincmd
   - menu, user
   - taskbar (most complex GTK usage)
   - systray / tray (X11 embedding, needs careful GTK3 handling)
   - genmon, volume, unstable

5. **Build and iterate using fbpanel_builder until the GTK3 build is clean.**
   Run the compile script against the in-progress fbpanel3 repo after each
   significant change. The `bookworm` and `noble` images are the best targets
   since they ship GTK3 libraries that match current Debian/Ubuntu stable.

6. **Test across all six distros** once the build is stable:
   ```bash
   # In fbpanel_builder/
   ./test_all.sh   # runs compile_fullaxx_fbpanel3.sh on every image
   ```

7. **Commit strategy:** One commit per major file or plugin group.
   Example: `port(panel): replace GdkColor with GdkRGBA throughout`

---

## Phase 3 — Integration Testing

**Goal:** Confirm fbpanel3 actually runs and core features work.

### Tasks

1. **Manual smoke test on a live X11 session.**
   - Panel loads and displays
   - System tray accepts applications
   - Taskbar shows open windows
   - Clock displays correctly
   - Pager switches desktops

2. **Test each plugin individually** by adding it to a test config and
   confirming no crash on load.

3. **Test eleksir-specific features:**
   - batterytext plugin
   - Extended launchbar (>20 buttons)
   - Xinerama multi-monitor behavior
   - Icon drawing in pager

4. **Test GTK3-specific behavior:**
   - Theme integration (GTK3 themes should apply)
   - HiDPI / scaling
   - Wayland compatibility check (GTK3 + XWayland, not a full port goal yet)

5. **File bugs for anything broken** as GitHub issues on Fullaxx/fbpanel3.

---

## Phase 4 — Cleanup and Modernization

**Goal:** Remove legacy cruft, modernize code style, fix known issues.

### Tasks

1. **Remove Python2/Python3 configure scripts.**
   eleksir already did this; confirm they are gone. Only CMake should remain.

2. **Update README.md.**
   - Project description and goals
   - Build instructions (CMake only)
   - Dependency list (GTK3, libwnck, etc.)
   - Plugin list

3. **Review and update CHANGELOG.**
   Add a section for version 8.0 documenting the merge.

4. **Review `contrib/` directory** from eleksir (Gentoo ebuilds).
   - Keep if useful, move to `packaging/` with better naming
   - Add a note that these are out of date and for reference only

5. **Add or update `.gitignore`** to cover CMake build artifacts.

6. **License audit.**
   Confirm all sources carry compatible licenses (fbpanel is GPL-2).
   Add/update LICENSE file at repo root.

7. **Update version to 8.0** in CMakeLists.txt and any other version files.

8. **Remove `www/` directory** (old project website; not relevant).
   Or archive it in a `legacy/` subdirectory.

---

## Phase 5 — Ongoing Maintenance Structure

**Goal:** Set the project up so it can stay alive.

### Tasks

1. **Wire fbpanel3 into fbpanel_builder's CI pipeline.**

   a. Add `scripts/compile_fullaxx_fbpanel3.sh` (see Build Infrastructure
      section above) to the fbpanel_builder repo.

   b. Add it to `test_all.sh` alongside the existing eleksir and berte scripts.

   c. The existing `docker-build-push.yml` CI in fbpanel_builder already
      handles multi-arch (amd64 + arm64) builds across all six distros and
      pushes to `ghcr.io/fullaxx/fbpanel_builder`. No new CI file needed there.

   d. Add a minimal GitHub Actions workflow to fbpanel3 itself that pulls a
      fbpanel_builder image and runs the compile script on every push/PR:
   ```yaml
   # fbpanel3/.github/workflows/build.yml
   name: build
   on: [push, pull_request]
   jobs:
     build:
       runs-on: ubuntu-latest
       strategy:
         matrix:
           tag: [noble, jammy, bookworm, bullseye]
       steps:
         - uses: actions/checkout@v4
         - name: Build in Docker
           run: |
             docker pull ghcr.io/fullaxx/fbpanel_builder:${{ matrix.tag }}
             docker run --rm \
               -v ${{ github.workspace }}:/src \
               ghcr.io/fullaxx/fbpanel_builder:${{ matrix.tag }} \
               bash -c "cd /src && mkdir build && cd build && cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib .. && make -j"
   ```

2. **Write a CONTRIBUTING.md** covering:
   - How to build
   - How to write a plugin
   - Commit message conventions
   - PR process

3. **Tag v8.0.0** once Phase 3 testing passes.

4. **Track upstream issues** from the original fbpanel issue tracker and
   the eleksir/berte repos; migrate relevant open issues to fbpanel3.

5. **Consider packaging:**
   - Debian/Ubuntu `.deb` (via `cpack` or manual)
   - Arch Linux AUR PKGBUILD
   - Gentoo ebuild (update the one from eleksir's contrib/)

---

## Plugin Inventory (Merge Checklist)

| Plugin | fbpanel | eleksir | berte | fbpanel3 target |
|---|---|---|---|---|
| battery | yes | yes | yes | yes |
| batterytext | no | yes | no | yes |
| chart | yes | yes | yes | yes |
| cpu | yes | yes | yes | yes |
| dclock | yes | yes | yes | yes |
| deskno | yes | yes | yes | yes |
| deskno2 | yes | yes | yes | yes |
| genmon | yes | yes | yes | yes |
| icons | yes | yes | yes | yes |
| image | yes | yes | yes | yes |
| launchbar | yes | yes (40btn) | yes | yes (40btn) |
| mem | yes | yes | yes | yes |
| mem2 | yes | yes | yes | yes |
| menu | yes | yes | yes | yes |
| meter | yes | yes | yes | yes |
| net | yes | yes | yes | yes |
| pager | yes | yes (icons) | yes | yes (icons) |
| separator | yes | yes | yes | yes |
| space | yes | yes | yes | yes |
| systray/tray | yes | yes | yes | yes |
| taskbar | yes | yes | yes | yes |
| tclock | yes | yes | yes | yes |
| unstable | yes | yes | yes | yes |
| user | yes | yes | yes | yes |
| volume | yes | yes | yes | yes |
| wincmd | yes | yes | yes | yes |

---

## Dependency Reference

GTK3 target dependencies (Debian package names).
**All of these are pre-installed in the fbpanel_builder images** — no manual
setup needed when building via Docker.

```
libgtk-3-dev
libwnck-3-dev
libgdk-pixbuf-xlib-2.0-dev   (libgdk-pixbuf2.0-dev on Ubuntu focal)
libx11-dev
libxcomposite-dev
libxml2-dev      (if menu plugin uses it)
cmake (>= 3.5)
build-essential
```

To build natively on a host (without Docker), install the above with:
```bash
sudo apt install build-essential cmake libgtk-3-dev libwnck-3-dev \
  libgdk-pixbuf-xlib-2.0-dev libx11-dev libxcomposite-dev
```

---

## Risk Areas

| Risk | Mitigation |
|---|---|
| systray plugin may not work under GTK3 | systray protocol is X11-level; test carefully. May need `libxembed` workaround |
| Wayland incompatibility | Scope for fbpanel3 is X11/XWayland only for now |
| eleksir and berte both diverged from different commits of fbpanel | Diff carefully; manual merge may be needed for panel/ core |
| batterytext plugin is GTK2-only code | Port it as part of Phase 2 plugin work |
| CMake + GTK3 pkg-config names | Double-check module names: `gtk+-3.0`, `libwnck-3.0` |

---

## Reference Repositories

| Repo | URL |
|---|---|
| fbpanel original | https://github.com/aanatoly/fbpanel |
| fbpanel3_berte source | https://github.com/berte/fbpanel3 (fbpanel-gtk3-dev branch) |
| fbpanel_eleksir source | https://github.com/eleksir/fbpanel (mainline branch) |
| fbpanel3 (this project) | https://github.com/Fullaxx/fbpanel3 |
| fbpanel_builder | https://github.com/Fullaxx/fbpanel_builder |
| fbpanel_builder images | ghcr.io/fullaxx/fbpanel_builder |
| Original GTK3 port issue | https://github.com/fbpanel/fbpanel/issues/8 |
| eleksir upstream PR | https://github.com/aanatoly/fbpanel/pull/63 |

---

## Version History / Milestones

| Version | Description |
|---|---|
| 7.0 | fbpanel original + fbpanel3_berte GTK3 port baseline |
| 7.2 | fbpanel_eleksir improvements (CMake, batterytext, launchbar, etc.) |
| **8.0** | **fbpanel3: GTK3 + CMake + all eleksir improvements (merge target)** |
| 8.x | Ongoing maintenance, bug fixes, new plugins |

---

*Plan written: 2026-02-17*
*Target repo: https://github.com/Fullaxx/fbpanel3*
