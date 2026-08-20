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
# One ecosystem may hold more than one fabric. Set ECO_VIDS to every VendorId it
# owns and the verdict is reported per fabric, because "released" and "retained"
# are not the only outcomes once there are two: an ecosystem can release one and
# keep the other, which consumes a slot no administrator is left to free.
#
# Set LABEL to namespace the output files. Without it this overwrites whatever
# the previous run wrote, which for a published measurement is destructive.
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
# Identifier(s) of the ecosystem's fabric(s), space separated, matched against the
# boot capture. Prefer Compressed FabricId over VendorId: the device emits VendorId
# LAST on the line, and a concurrent log line can interleave and truncate it, which
# would read as a released fabric. Compressed FabricId sits early in the line.
ECO_VIDS="${ECO_VIDS:-${ECO_VID:-0x110A}}"
# Namespaces the output files, e.g. LABEL=apple -> fabric-apple-before.txt.
LABEL="${LABEL:-}"
PFX="fabric${LABEL:+-$LABEL}"
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
  boot_fabrics | tee "$OUT/$PFX-before.txt"
  echo
  echo "-- CommissionedFabrics --"
  "$CT" operationalcredentials read commissioned-fabrics "$NODE" 0 --storage-directory "$ST" --timeout 30 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -oE "CommissionedFabrics: [0-9]+" | tail -1 | tee -a "$OUT/$PFX-before.txt"
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
  boot_fabrics | tee "$OUT/$PFX-after.txt"
  echo
  echo "-- CommissionedFabrics --"
  "$CT" operationalcredentials read commissioned-fabrics "$NODE" 0 --storage-directory "$ST" --timeout 40 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -oE "CommissionedFabrics: [0-9]+" | tail -1 | tee -a "$OUT/$PFX-after.txt"
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
  boot_fabrics | tee "$OUT/$PFX-final.txt"

  echo "== CAPTURE INTEGRITY =="
  # A verdict is only as good as the capture. The device interleaves log lines, so
  # a fabric record can be truncated mid-field; a truncated record that happens to
  # cut before the matched identifier reads exactly like an absent fabric. Refuse to
  # issue a verdict when the two instruments disagree on how many fabrics exist.
  # grep -c exits 1 on a zero count, so `|| echo 0` would append a second line and
  # the arithmetic test below would fail on "0\n0". Force a single value.
  nrec=$(grep -c "was retrieved from storage" "$OUT/$PFX-final.txt" 2>/dev/null); nrec=${nrec:-0}
  ntrunc=$(grep "was retrieved from storage" "$OUT/$PFX-final.txt" 2>/dev/null | grep -vc "VendorId 0x[0-9A-Fa-f]"); ntrunc=${ntrunc:-0}
  ncf=$(echo "$FINAL" | grep -oE "[0-9]+$")
  echo "  fabric records in boot capture: $nrec"
  echo "  CommissionedFabrics attribute:  ${ncf:-unknown}"
  echo "  records with a truncated tail:  $ntrunc"
  if [ -n "$ncf" ] && [ "$nrec" != "$ncf" ]; then
    echo "  ⚠️  INSTRUMENTS DISAGREE. Either the removal is still in flight, or the"
    echo "     capture dropped a record. Re-run the capture before believing anything."
  fi
  [ "$ntrunc" -gt 0 ] && echo "  ⚠️  $ntrunc record(s) truncated. Match on Compressed FabricId, not VendorId."

  echo
  echo "== VERDICT =="
  # The fresh boot capture is authoritative: it reads NVS rather than asking the
  # ecosystem-facing attribute, and the two disagree during the deferral window.
  held=0; freed=0
  for vid in $ECO_VIDS; do
    if grep -qi "$vid" "$OUT/$PFX-final.txt" 2>/dev/null; then
      echo "  🔴 $vid RETAINED — still in NVS after removal and settling."
      held=$((held+1))
    else
      echo "  ✅ $vid RELEASED — gone despite the device being unreachable at removal."
      freed=$((freed+1))
    fi
  done
  echo
  if [ "$held" -gt 0 ] && [ "$freed" -gt 0 ]; then
    echo "  🔴 PARTIAL RELEASE: $freed released, $held retained. The ecosystem freed"
    echo "     some of its fabrics and kept others. Every retained slot is consumed"
    echo "     with no administrator left to release it, and the app reports the"
    echo "     device as removed either way. This is the sharpest form of the"
    echo "     exhaustion failure mode."
  elif [ "$held" -gt 0 ]; then
    echo "  🔴 RETAINED AT T+60s: all $held fabric(s) still in NVS. The ecosystem"
    echo "     cleaned up its own records; the slots are consumed for now."
    echo "     ⚠️  This is NOT yet 'orphaned'. A previous ecosystem deferred removal and"
    echo "     delivered it ~40 s after reachability returned, so a 60 s settle only"
    echo "     rules out a SHORT deferral. Re-read the attribute over hours before"
    echo "     claiming the slot is permanently lost. Declaring orphaned here is the"
    echo "     same error as the original boot-log-only verdict, inverted."
  else
    echo "  ✅ FULLY RELEASED: all $freed fabric(s) freed. Establish HOW — queued for"
    echo "     later delivery, or removed on the device's next reachable moment?"
  fi
  echo
  echo "-- boot capture: before vs after re-attach (deferral window) --"
  diff "$OUT/$PFX-before.txt" "$OUT/$PFX-after.txt" && echo "  (identical: removal had not yet been delivered)"
  echo "-- boot capture: before vs final (settled) --"
  diff "$OUT/$PFX-before.txt" "$OUT/$PFX-final.txt" && echo "  (identical: nothing was ever removed)"
  ;;
*) echo "usage: $0 before|after"; exit 1 ;;
esac
