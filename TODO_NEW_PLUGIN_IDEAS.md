# New Plugin Ideas

Potential plugins for fbpanel3, ordered by implementation effort.

---

## Low Effort

### thermal
Display CPU temperature from the kernel's thermal subsystem.
- Read `/sys/class/thermal/thermal_zone*/temp` or `/sys/class/hwmon/hwmon*/temp*_input`
- Show temperature in °C with color coding (green/yellow/red thresholds)
- Could reuse the `meter` plugin class like `battery` and `cpu`

### brightness
Screen backlight level indicator and controller.
- Read/write `/sys/class/backlight/<device>/brightness` and `max_brightness`
- Display current level as percentage
- Scroll wheel to adjust brightness without launching an external tool

### loadavg
1-minute system load average from `/proc/loadavg`.
- Trivial file read, no external libraries
- Display as a number or small graph using the `meter` class
- Useful on servers and multi-core desktops to spot runaway processes

### disk
Disk free space monitor via `statvfs()`.
- Configurable mount point (default `/`)
- Show used/free as percentage or absolute (GB)
- Alert color when space drops below a configurable threshold

---

## Medium Effort

### mpris
Media player control via D-Bus MPRIS2 interface.
- Show current track title/artist from any compliant player (mpv, VLC, Spotify, etc.)
- Play/pause/next/prev buttons
- Requires GDBus (already available via GLib)

### kbdlayout
Keyboard layout indicator using the X Keyboard Extension (Xkb).
- Show current layout abbreviation (e.g., "US", "FR", "RU")
- Click to cycle through configured layouts
- Uses `XkbGetState()` / `XkbLockGroup()` — no extra libraries beyond libX11

### nmstatus
NetworkManager connection status via D-Bus.
- Show active connection name and signal strength (for Wi-Fi)
- Click to launch `nm-connection-editor` or `nmtui`
- Requires GDBus queries to `org.freedesktop.NetworkManager`

---

## Higher Effort

### pipewire / pulseaudio
Better volume control than the current ALSA-only `volume` plugin.
- Control system volume via PipeWire (with PulseAudio compatibility layer)
- Show default sink name and mute state
- Requires `libpipewire` or `libpulse` linkage

### notifybadge
Freedesktop desktop notification count badge.
- Implement a minimal notification daemon that counts unread notifications
- Display badge count; click to show a popup list of recent notifications
- Requires implementing `org.freedesktop.Notifications` D-Bus service

---

## Notes

- Low-effort plugins are good candidates to implement next — they need no extra
  dependencies beyond what fbpanel3 already links against.
- Medium-effort plugins need GDBus (part of GLib/GIO, already a transitive dep)
  but no new shared libraries in the build.
- Higher-effort plugins introduce new link-time dependencies and should be made
  optional (only built if the library is detected at configure time).
