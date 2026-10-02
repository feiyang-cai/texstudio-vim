#!/usr/bin/env bash
set -euo pipefail
if command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y --no-install-recommends python3 xvfb xauth xdotool scrot \
        libgl1 libegl1 libx11-xcb1 libxcb-cursor0 libxkbcommon-x11-0 \
        libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
        libxcb-xinerama0 libxcb-randr0 libxcb-shape0 libxcb-xfixes0 \
        libfontconfig1 libdbus-1-3 libnss3 fonts-dejavu-core
else
    dnf install -y python3 xorg-x11-server-Xvfb xorg-x11-xauth xdotool scrot \
        mesa-libGL mesa-libEGL libX11 libxcb xcb-util-cursor xcb-util-wm \
        xcb-util-image xcb-util-keysyms xcb-util-renderutil libxkbcommon-x11 \
        fontconfig dbus-libs nss dejavu-sans-fonts
fi
export QT_QPA_PLATFORM=xcb
export XDG_RUNTIME_DIR=/tmp/texstudio-runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
Xvfb :99 -screen 0 1280x800x24 >/tmp/xvfb.log 2>&1 &
export DISPLAY=:99
python3 /work/.github/scripts/smoke-appimage.py
