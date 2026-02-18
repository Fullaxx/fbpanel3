# Installation

## Requirements

- GTK3 >= 3.0 (development headers)
- GLib2 >= 2.4
- CMake >= 3.5

On Debian/Ubuntu:
```sh
sudo apt install cmake libgtk-3-dev
```

## System Install

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C build -j$(nproc)
sudo make -C build install
```

## Local Build (no install)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j$(nproc)
```
