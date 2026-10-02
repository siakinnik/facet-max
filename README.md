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

The module brings the libraries MAX needs beyond glibc (libGLX/libX11, GLib,
libva, …) from Ubuntu 22.04 packages, so it runs on any x86_64 Linux with
glibc ≥ 2.35, plus DejaVu fonts. Their licenses are in `runtime/licenses/`.

Needs Facet 0.6 (API 3), the Wayland module, and the permissions `network`,
`wayland.window`, `background`; optionally `notifications` and
`wayland.clipboard`. MAX takes about 310 MB to download and 1.1 GB on disk.

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
