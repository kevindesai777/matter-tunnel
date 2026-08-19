#!/usr/bin/env bash
# Does an ecosystem release the device-side fabric when it removes a device it
# genuinely cannot reach?
#
# Motivation: an earlier version of this test was invalid. Moving the device to a
# different Thread network did NOT make it unreachable — Matter is IP-based, and
# our own border router relayed the ecosystem's traffic to it (measured). Genuine
# unreachability therefore requires stopping the border router, so that no IP path
# to the device exists at all.
#
#   ./fabric-unreachable-test.sh before   # baseline, then cut the IP path
#   ...remove the device in the ecosystem app, noting the exact wording...
#   ./fabric-unreachable-test.sh after    # restore, re-read fabric table, compare
#
#
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail

# --- configuration; override in the environment -----------------------------
PI="${PI_HOST:?set PI_HOST to the border-router host, e.g. pi@raspberrypi.local}"
CT="${CHIP_TOOL:-$HOME/ncs-door-lock/project-workspace/modules/lib/matter/examples/chip-tool/out/host/chip-tool}"
ST="${CHIP_TOOL_STORAGE:-$HOME/ncs-door-lock/chip-tool-storage}"
OUT="${OUT_DIR:-$HOME/matter-tunnel/evidence/device-logs}"
NODE="${NODE_ID:-1}"
# Device extended address, from `ot-ctl child table` on the border router.
DEV_EXTADDR="${DEV_EXTADDR:?set DEV_EXTADDR to the device extended address}"
# VendorId of the ecosystem under test.
ECO_VID="${ECO_VID:-0x110A}"
# Serial port on the border-router host where the device console enumerates.
DEV_TTY="${DEV_TTY:-/dev/ttyACM1}"

# zsh does not word-split an unquoted $VAR, so this must be a function rather
# than a variable holding "ssh -o ...".
p() { ssh -o ConnectTimeout=60 -o BatchMode=yes "$PI" "$@"; }

# §6.3's controller-free instrument: the device emits one record per stored
# fabric at boot, giving identity as well as count. A plain reset preserves NVS.
boot_fabrics() {
  p "nohup python3 ~/serial_cap.py $DEV_TTY 35 > ~/fab.log 2>&1 &
     sleep 2
     sudo openocd -f interface/jlink.cfg -c 'transport select swd' \
       -f target/nordic/nrf52.cfg -c 'init; reset run; exit' >/dev/null 2>&1
     sleep 35
     grep -E 'Fabric index .* was retrieved from storage' ~/fab.log"
}

case "${1:-}" in
before)
  mkdir -p "$OUT"
  echo "== BASELINE $(date -Iseconds) =="
  boot_fabrics | tee "$OUT/fabric-before.txt"
  echo
  echo "-- CommissionedFabrics --"
  "$CT" operationalcredentials read commissioned-fabrics "$NODE" 0 --storage-directory "$ST" --timeout 30 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -oE "CommissionedFabrics: [0-9]+" | tail -1 | tee -a "$OUT/fabric-before.txt"
  echo
  echo "== CUTTING THE IP PATH (stopping border router) =="
  p 'sudo systemctl stop otbr-agent'; sleep 10
  ADDR=$(p 'sudo ot-ctl childip' 2>/dev/null | tr -d '\r' | grep -oE 'fd55[0-9a-f:]+' | head -1)
  printf "  reachability check: "
  ping6 -c 3 -i 0.4 "${ADDR:-fd55::1}" 2>&1 | grep -oE "[0-9.]+% packet loss" | tail -1
  echo
  echo "NOW: remove the device in the ecosystem app."
  echo "Record the EXACT wording of any warning or force-remove prompt — that is"
  echo "evidence for whether the user is told the device may retain the fabric."
  echo "Then run:  $0 after"
  ;;
after)
  echo "== RESTORING border router =="
  p 'sudo systemctl start otbr-agent'; sleep 10
  for i in $(seq 1 15); do
    p "sudo ot-ctl child table 2>/dev/null | grep -qF $DEV_EXTADDR" && { echo "  re-attached (t=$((i*6))s)"; break; }
    sleep 6
  done
  echo
  echo "== AFTER $(date -Iseconds) =="
  boot_fabrics | tee "$OUT/fabric-after.txt"
  echo
  echo "-- CommissionedFabrics --"
  "$CT" operationalcredentials read commissioned-fabrics "$NODE" 0 --storage-directory "$ST" --timeout 40 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -oE "CommissionedFabrics: [0-9]+" | tail -1 | tee -a "$OUT/fabric-after.txt"
  echo
  # 🔴 The boot log alone is NOT sufficient. Measured 2026-08-18: the ecosystem
  # defers the removal and delivers it once the device is reachable again, so a
  # boot capture taken seconds after re-attach still shows the fabric while the
  # attribute already reports it gone. Re-read AFTER a settling delay.
  echo
  echo "== settling (deferred removal may still be in flight) =="
  sleep 60
  echo "-- CommissionedFabrics after settling --"
  FINAL=$("$CT" operationalcredentials read commissioned-fabrics "$NODE" 0 --storage-directory "$ST" --timeout 40 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -oE "CommissionedFabrics: [0-9]+" | tail -1)
  echo "  $FINAL"
  echo "-- fresh boot capture (authoritative: what is actually in NVS) --"
  boot_fabrics | tee "$OUT/fabric-final.txt"

  echo "== VERDICT =="
  if grep -q "$ECO_VID" "$OUT/fabric-final.txt" 2>/dev/null; then
    echo "  🔴 FABRIC ORPHANED — VendorId $ECO_VID still present on the device."
    echo "     The ecosystem cleaned up its own records; the slot is consumed with"
    echo "     no administrator able to release it. Irreversible short of factory reset."
  else
    echo "  ✅ RELEASED — VendorId $ECO_VID is gone despite the device being unreachable"
    echo "     at removal time. Worth establishing HOW: queued for later delivery, or"
    echo "     removed on the device's next reachable moment?"
  fi
  diff "$OUT/fabric-before.txt" "$OUT/fabric-after.txt" && echo "  (no change at all)"
  ;;
*) echo "usage: $0 before|after"; exit 1 ;;
esac
