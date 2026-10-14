#!/bin/sh
# Local build only; deliberately no staging/SSH/device actions.
set -eu
[ "$#" = 1 ] || { echo 'usage: build-bt-guard.sh OUTPUT_DIR'; exit 64; }
S=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
O=$1
mkdir -p "$O"
CC=${CC:-arm-linux-gnueabihf-gcc}
flags='-std=gnu11 -Os -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -Wall -Wextra -Werror'
$CC $flags -fPIC -shared "$S/kindle_bt_idle_guard.c" -o "$O/guard.so" -ldl
$CC $flags "$S/kindle_bt_guard_session.c" -o "$O/session"
cp "$S/scripts/bt-guard" "$O/bt-guard"
cp "$S/scripts/Enable Bluetooth Guard.sh" "$O/Enable Bluetooth Guard.sh"
cp "$S/BT_GUARD.md" "$O/BT_GUARD.md"
chmod 755 "$O/session" "$O/bt-guard" "$O/Enable Bluetooth Guard.sh"
chmod 644 "$O/guard.so" "$O/BT_GUARD.md"
(cd "$O" && sha256sum guard.so session bt-guard 'Enable Bluetooth Guard.sh' BT_GUARD.md > SHA256SUMS)
