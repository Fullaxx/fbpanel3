# Contributing to fbpanel3

## Building

See [INSTALL.md](INSTALL.md) for build requirements and instructions.

Quick build:
```sh
cmake -B build
make -C build -j$(nproc)
```

## Testing

Before submitting a PR, please verify the build succeeds on at least one of the
supported distros. The project includes builder Docker images for multi-distro
testing via [fbpanel_builder](https://github.com/Fullaxx/fbpanel_builder).

Supported distros: Ubuntu 20.04 (focal), 22.04 (jammy), 24.04 (noble),
Debian 11 (bullseye), 12 (bookworm), 13 (trixie).

CI runs automatically on push and pull requests via GitHub Actions.

## Submitting Changes

1. Fork the repository and create a branch from `master`
2. Make your changes with clear, focused commits
3. Ensure the build is clean with no new warnings
4. Open a pull request against `master`

## Coding Style

Follow the style of the surrounding code. This is a C codebase using GTK3 and GLib.
- Use GTK3 APIs (not deprecated GTK2 equivalents)
- Keep functions focused and avoid unnecessary abstraction
