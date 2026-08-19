#!/usr/bin/env bash
# Write Raspberry Pi OS to the SD card and pre-seed it for headless SSH login.
# Run on the Mac:   sudo ~/matter-tunnel/tools/flash-pi-sd.sh [diskN]
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

DISK="${1:-disk15}"
IMG="${PI_IMAGE:-$HOME/Downloads/raspios-trixie-arm64-lite.img.xz}"
USERNAME="${PI_USER:-pi}"

# Supply your own crypt(3) hash; never commit one.
#   PI_PWHASH=$(openssl passwd -6)
PWHASH="${PI_PWHASH:?set PI_PWHASH, e.g. export PI_PWHASH=\$(openssl passwd -6)}"

red() { printf '\033[31m%s\033[0m\n' "$*"; }
grn() { printf '\033[32m%s\033[0m\n' "$*"; }
bold(){ printf '\033[1m%s\033[0m\n' "$*"; }

bold "== 1. Safety interlock on /dev/$DISK =="
INFO=$(diskutil info "$DISK")
PROTO=$(echo "$INFO"  | awk -F': *' '/^ *Protocol:/{print $2}'       | xargs)
REMOV=$(echo "$INFO"  | awk -F': *' '/^ *Removable Media:/{print $2}'| xargs)
WHOLE=$(echo "$INFO"  | awk -F': *' '/^ *Whole:/{print $2}'          | xargs)
SIZEB=$(echo "$INFO"  | awk -F'[()]' '/^ *Disk Size:/{print $2}'     | awk '{print $1}')
MEDIA=$(echo "$INFO"  | awk -F': *' '/Device \/ Media Name:/{print $2}' | xargs)

echo "  media     : $MEDIA"
echo "  protocol  : $PROTO"
echo "  removable : $REMOV"
echo "  whole     : $WHOLE"
echo "  size      : $SIZEB bytes"

# Refuse anything that is not a whole, removable SD card. This is the guard that
# makes a mistyped disk number harmless instead of catastrophic.
[ "$PROTO" = "Secure Digital" ] || { red "ABORT: protocol is '$PROTO', not 'Secure Digital'."; exit 1; }
[ "$REMOV" = "Removable" ]      || { red "ABORT: media is not removable."; exit 1; }
[ "$WHOLE" = "Yes" ]            || { red "ABORT: not a whole disk."; exit 1; }
[ "${SIZEB:-0}" -lt 1099511627776 ] || { red "ABORT: larger than 1 TB; refusing."; exit 1; }
grn "  interlock passed"

bold "== 2. Unmounting =="
diskutil unmountDisk "/dev/$DISK"

bold "== 3. Writing image (a few minutes; press Ctrl-T for progress) =="
xz -dc "$IMG" | dd of="/dev/r$DISK" bs=4m
sync

bold "== 4. Waiting for the boot partition to mount =="
BOOT=""
for i in $(seq 1 30); do
  diskutil mountDisk "/dev/$DISK" >/dev/null 2>&1 || true
  for c in /Volumes/bootfs /Volumes/boot; do [ -d "$c" ] && BOOT="$c" && break; done
  [ -n "$BOOT" ] && break
  sleep 2
done
[ -n "$BOOT" ] || { red "ABORT: boot partition never mounted."; exit 1; }
grn "  boot partition at $BOOT"

bold "== 5. Pre-seeding headless access =="
touch "$BOOT/ssh"                                   # enables sshd on first boot
echo "$USERNAME:$PWHASH" > "$BOOT/userconf.txt"    # creates the account
echo "  ssh          -> enabled"
echo "  userconf.txt -> user '$USERNAME'"
sync

bold "== 6. Ejecting =="
diskutil eject "/dev/$DISK"

grn ""
grn "DONE. Put the card in the Pi, plug Ethernet into the ROUTER, power on."
grn "First boot takes ~60-90s (it expands the filesystem and reboots once)."
