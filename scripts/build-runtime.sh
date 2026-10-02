#!/usr/bin/env bash
# Builds the runtime the module ships next to its executable: the libraries
# MAX needs beyond glibc (from the Ubuntu 22.04 packages in RUNTIME), the
# bundled dbus-daemon, DejaVu fonts, the Qt Wayland platform plugin of the
# Qt version MAX uses, configuration files and the packages' licenses.
# Needs dpkg-deb and python3 (venv); on hosts other than Ubuntu 22.04 the
# packages are fetched in an ubuntu:22.04 Docker container.
#   scripts/build-runtime.sh <out-dir>   (CMake runs it when needed)
set -euo pipefail
out="$(mkdir -p "$1" && cd "$1" && pwd)"
root="$(cd "$(dirname "$0")/.." && pwd)"
stamp="$(cat "$root/RUNTIME" "$0" | sha256sum | cut -c1-16)"
if [[ -f "$out/.runtime" && "$(cat "$out/.runtime")" == "$stamp" ]]; then
    echo "runtime $stamp already built in $out"
    exit 0
fi

packages="$(grep -v '^#' "$root/RUNTIME" | grep -v '^qt ' | tr '\n' ' ')"
qt="$(sed -n 's/^qt //p' "$root/RUNTIME")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/debs" "$work/tree"

echo "== Ubuntu 22.04 packages"
if grep -q '^VERSION_CODENAME=jammy' /etc/os-release; then
    (cd "$work/debs" && apt-get download -q $packages)
else
    docker run --rm -v "$work/debs:/w" ubuntu:22.04 sh -c \
        "apt-get update -qq && cd /w && apt-get -o APT::Sandbox::User=root download -q $packages && chmod a+r *.deb"
fi
for deb in "$work"/debs/*.deb; do dpkg-deb -x "$deb" "$work/tree"; done

rm -rf "$out" && mkdir -p "$out"/{lib,bin,fonts,share,etc,licenses,plugins/platforms}
for d in "$work/tree/lib/x86_64-linux-gnu" "$work/tree/usr/lib/x86_64-linux-gnu"; do
    [[ -d "$d" ]] && find "$d" -maxdepth 1 -name '*.so*' -exec cp -a {} "$out/lib/" \;
done
cp "$work/tree/usr/bin/dbus-daemon" "$out/bin/"
cp "$work"/tree/usr/share/fonts/truetype/dejavu/*.ttf "$out/fonts/"
for doc in "$work"/tree/usr/share/doc/*/copyright; do
    cp "$doc" "$out/licenses/$(basename "$(dirname "$doc")").copyright"
done

echo "== Qt $qt Wayland platform plugin"
python3 -m venv "$work/venv"
"$work/venv/bin/pip" install -q 'aqtinstall==3.3.0'
(cd "$work" && "$work/venv/bin/aqt" install-qt linux desktop "$qt" linux_gcc_64 --archives qtbase -O "$work/qt" >"$work/aqt.log" 2>&1) || { cat "$work/aqt.log"; exit 1; }
cp "$work/qt/$qt/gcc_64/plugins/platforms/libqwayland.so" "$out/plugins/platforms/"
cat > "$out/licenses/qt.txt" <<EOF
plugins/platforms/libqwayland.so is the unmodified Wayland platform plugin of
the official Qt $qt binaries (The Qt Company), licensed under the GNU LGPL v3.
Source: https://download.qt.io/official_releases/qt/
EOF

cat > "$out/share/dbus-session.conf" <<'EOF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<!-- The app's private session bus inside the module's container. -->
<busconfig>
  <type>session</type>
  <keep_umask/>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
cat > "$out/etc/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir prefix="relative">../fonts</dir>
  <dir>/usr/share/fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
</fontconfig>
EOF
echo "$stamp" > "$out/.runtime"
echo "runtime built in $out ($(du -sh "$out" | cut -f1))"
