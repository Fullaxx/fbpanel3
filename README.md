# fbpanel3

A lightweight GTK3 panel for the Linux desktop.

## About

fbpanel3 is a continuation of the fbpanel project, ported to GTK3 and maintained
as a merge of two community forks:

- [fbpanel3_berte](https://github.com/berte/fbpanel3) — GTK3 port
- [fbpanel_eleksir](https://github.com/eleksir/fbpanel) — quality-of-life improvements (v7.2)

Version 8.0 combines the GTK3 port with the v7.2 improvements and applies
additional fixes required for modern GTK3 (3.24+).

## Features

- Taskbar with window management (iconify, shade, raise)
- System tray (notification area)
- Application launcher bar
- Text clock and digital clock with calendar popup
- Pager (virtual desktop switcher)
- CPU, network, memory, and battery monitors
- Application menu from `.desktop` files with icon theme support
- Show desktop / minimize-all button
- Transparency and autohide support
- Horizontal and vertical panel orientations

## Screenshot

![screenshot](/data/shot.png)

## Build Requirements

- GTK3 >= 3.0 (development headers)
- GLib2 >= 2.4
- CMake >= 3.5

On Debian/Ubuntu: `sudo apt install cmake libgtk-3-dev`

## Building

```sh
mkdir build
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C build -j$(nproc)
```

## Installing

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C build -j$(nproc)
sudo make -C build install
```

## Configuration

fbpanel reads its configuration from `~/.config/fbpanel/default` (created
automatically on first run). See the `data/` directory for example profiles.

## Links

- [Original fbpanel project](http://aanatoly.github.io/fbpanel/)
- [GitHub repository](https://github.com/Fullaxx/fbpanel3)
