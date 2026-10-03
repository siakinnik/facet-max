# max — Facet plugin

The [MAX](https://max.ru) messenger on a Facet panel: the official MAX desktop
client for Linux, shown full screen on the module's own screen, with its
notifications as Facet notifications. Runs as an out-of-process plugin of
[facet-core](https://github.com/siakinnik/facet-core) and needs the
[Wayland module](https://github.com/siakinnik/facet-wayland).

## How it works

- **MAX is not part of this module.** On first use the module downloads the
  official package from `download.max.ru` (the same repository `apt` uses),
  checks it against the SHA-256 in the repository index (fetched over
  verified HTTPS) and unpacks it into its own data directory. Updates come from
  the same repository: the module checks twice a day and offers them as a
  notification. Nothing is installed into the host system.
- **It runs inside the module's container** on the Wayland display the
  compositor module shares with this module, and its window is shown full
  screen on the module's screen (a swipe down from the top edge goes back).
  Facet's keyboard opens when a text field is focused.
- MAX's Qt has no Wayland platform plugin, so the module brings the official
  one of the same Qt version (LGPL) and a tiny preload library that turns
  MAX's built-in `-platform xcb` into `wayland`. MAX's files stay untouched.
- MAX needs a D-Bus session: the module runs a private `dbus-daemon` and
  offers two services on it itself:
  - **Secret Service** (`org.freedesktop.secrets`): MAX keeps its login
    through libsecret; it is stored in the module's data directory, which
    only the module's container can read.
  - **Notifications** (`org.freedesktop.Notifications`): MAX's notifications
    become Facet notifications (and a badge on the tile); tapping one opens
    the chat in MAX.
- MAX keeps running in the background (`background` permission) so messages
  arrive while its screen is closed; the compositor does not render it then.

## System requirements

MAX for Linux is published by VK for **x86_64 only**: a `.deb` for
Debian/Ubuntu and an `.rpm` for RHEL/CentOS/AlmaLinux/Oracle Linux 9. MAX
itself needs glibc ≥ 2.28 and bundles Qt 6.11 (with QtWebEngine), OpenSSL and
libstdc++. This module uses the `.deb` and runs on **any x86_64 Linux with
glibc ≥ 2.35** (Ubuntu 22.04+, Debian 12+, Fedora 36+, …): nothing has to be
installed on the host.

Everything else MAX needs is brought by the module in `runtime/lib`, taken
from Ubuntu 22.04 packages (list in `RUNTIME`, licenses in
`runtime/licenses/`):

| Group | Packages | Why |
|---|---|---|
| Graphics | libglvnd (libGLX, libOpenGL, libGL), libegl1, libgbm1, libdrm2, libepoxy0, libwayland-*, libxkbcommon0 | Qt and the web engine link OpenGL/EGL; drawing is done in software |
| X11 client libraries | libx11-6, libxcb-* (incl. libxcb-cursor0), libxext6, libxrandr2, libxdamage1, libxcomposite1, libxtst6, libxss1, … | linked by libGLX and Qt's libraries, although MAX runs on Wayland here |
| Video | libva2, libva-drm2, libva-x11-2, libvdpau1 | video decoding in chats |
| Sound | libpulse0, libasyncns0, libsndfile1 (FLAC, Ogg, Vorbis, Opus, GSM), libasound2 | voice messages and calls |
| GLib/GTK | libglib2.0-0, libnotify4, libgtk-3-0, pango, cairo, harfbuzz, fontconfig, freetype, gdk-pixbuf, … | notifications, file dialogs, text |
| Web engine | libnss3, libnspr4 | Chromium's crypto |
| Session bus | dbus (dbus-daemon), libdbus-1-3 | MAX's secrets and notifications |
| Fonts | fonts-dejavu-core | Latin and Cyrillic on hosts without fonts |
| Qt | `libqwayland.so` from the official Qt 6.11.0 build | MAX ships only the X11 platform plugin |

Only the C library (glibc) comes from the host. MAX's call service
(`max-service`) also wants libraries of older systems (libffi 6, PulseAudio
14); calls are not supported yet.

Needs Facet 0.6 (API 3), the Wayland module, and the permissions `network`,
`wayland.window`, `background`; optionally `notifications` and
`wayland.clipboard`. MAX takes about 310 MB to download and 1.1 GB on disk;
the module itself about 50 MB.

Not yet: calls (camera and microphone while in use), sound, file downloads to
the shared Downloads folder.

## Install

On a device that already runs [Facet](https://github.com/siakinnik/facet-core)
0.6+ with the Wayland module (x86_64):

```bash
curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash -s -- --plugin siakinnik/facet-max
```

Then open the MAX tile and tap *Install MAX*.

## Build from source

Needs a C/C++ compiler, CMake, ninja, the OpenSSL and liblzma development
files and python3 (venv). The first configure builds the runtime from the
Ubuntu 22.04 packages listed in `RUNTIME` (directly on Ubuntu 22.04,
otherwise in an `ubuntu:22.04` Docker container) and the official Qt plugin.

```bash
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build
```

`build/max.plugin/` is a ready plugin directory.

## Releases

Same as the other Facet plugins: CI checks translations, versions, builds,
runs the unit tests and a protocol smoke test. To release, bump the version in
`CMakeLists.txt` (`project(VERSION)`, `MAX_VERSION_SUFFIX`) and
`manifest.json`, push, then Actions → Release → *Run workflow*.

## Files

```
src/installer.*   download, verification, unpacking, version switch
src/https.*       HTTPS client (OpenSSL)
src/package.*     repository index, version order, .deb/tar unpacking (unit-tested)
src/dbus.*        minimal D-Bus wire protocol and connection (unit-tested)
src/secrets.*     Secret Service for libsecret
src/notify.*      desktop notifications -> Facet notifications
src/session.*     private bus and the MAX process
src/main.cpp      plugin glue: screen, tile, notifications, updates
shim/shim.c       preload: Qt platform "xcb" -> "wayland"
RUNTIME           what the runtime contains (Ubuntu packages, Qt version)
```

## Licenses

- This project: GPL-3.0 (LICENSE); its sources are this repository at each
  release tag.
- The release executables are static: glibc (LGPL-2.1-or-later), the GCC runtime, OpenSSL (Apache-2.0) and liblzma are built into them. Their
  licenses are in every release archive under `licenses/` (the packages they
  come from, with exact versions, in `licenses/STATIC`, and the full texts in
  `licenses/common-licenses/`).
- `runtime/` holds libraries, `dbus-daemon` and fonts from Ubuntu 22.04,
  unchanged, each with its license in `runtime/licenses/` (exact versions in
  `runtime/licenses/SOURCES`), and the unmodified Qt Wayland platform plugin
  (LGPL-3, see `runtime/licenses/qt.txt`).
- MAX itself is not part of this module: it is downloaded from its publisher
  on the device.
- Every release has `facet-max-<version>-sources.tar` with the sources of all of
  that, the runtime's source packages and Qt's qtbase sources. GCC's runtime (libstdc++, libgcc) is under the GCC Runtime
  Library Exception, which asks for no sources.
