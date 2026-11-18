#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
target="${1:-x86_64-windows-gnu}"
destination="${2:-BateriaSemFio.exe}"
zig_exe="${ZIG:-zig}"
temp_dir="$(mktemp -d)"
trap 'rm -rf "$temp_dir"' EXIT

"$zig_exe" rc /fo "$temp_dir/app.res" app.rc
"$zig_exe" c++ -target "$target" -std=c++20 -Os -DNDEBUG -DUNICODE -D_UNICODE \
    -Wall -Wextra -Werror -static -s -municode -Wl,--subsystem,windows \
    app.cpp battery.cpp usb_battery.cpp taskbar_strip.cpp "$temp_dir/app.res" -o "$destination" \
    -lsetupapi -lcfgmgr32 -lbluetoothapis -lole32 -lshell32 -luser32 -lgdi32 -lhid -lgdiplus -lcomctl32 -ladvapi32 -loleaut32 -ldwmapi
