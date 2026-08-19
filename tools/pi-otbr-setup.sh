#!/usr/bin/env bash
# Provision a Raspberry Pi as an OpenThread Border Router, using the
# nRF52840 Dongle (flashed with ot-rcp) as its radio.
#
# This is the experimental CONTROL condition: a border router we own, so that
# measured latency can be attributed to the hub rather than to Thread.
#
# Run ON THE PI, not on the Mac.  It changes system network configuration
# (IPv6 forwarding, firewall rules, mDNS) -- that is the Pi's whole purpose here.
#
#   ./pi-otbr-setup.sh            # install + form a network
#   ./pi-otbr-setup.sh --verify   # re-check an existing install, form nothing
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

CHANNEL="${CHANNEL:-15}"          # RECORD THIS -- the paper needs it for RF context
PANID="${PANID:-0x1234}"
NETNAME="${NETNAME:-mtunnel-ctl}"
INFRA_IF="${INFRA_IF:-eth0}"
RADIO_URL="${RADIO_URL:-spinel+hdlc+uart:///dev/ttyACM0}"
OUT="${OUT:-$HOME/otbr-dataset.txt}"

log() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

log "0. Sanity checks"
[ "$(uname -s)" = "Linux" ] || { echo "FATAL: run this on the Pi, not the Mac."; exit 1; }
echo "host   : $(uname -srm)"
echo "os     : $(. /etc/os-release && echo "$PRETTY_NAME")"
echo "infra  : $INFRA_IF -> $(ip -4 addr show "$INFRA_IF" 2>/dev/null | awk '/inet /{print $2}' || echo 'NOT FOUND')"

# The RCP must be present *before* setup, or otbr-agent installs against nothing.
DEV="${RADIO_URL##*uart://}"; DEV="${DEV%%\?*}"
if [ ! -e "$DEV" ]; then
  echo "FATAL: no RCP at $DEV. Plug the flashed dongle into the Pi."
  echo "       Present ACM devices: $(ls /dev/ttyACM* 2>/dev/null || echo none)"
  exit 1
fi
echo "rcp    : $DEV"
# Confirm it is our co-processor and not some other CDC device.
if command -v udevadm >/dev/null; then
  udevadm info -q property -n "$DEV" 2>/dev/null | grep -E "ID_MODEL=|ID_VENDOR_ID=|ID_MODEL_ID=" || true
fi

if [ "${1:-}" = "--verify" ]; then
  log "Verify only"
  sudo ot-ctl state || true
  sudo ot-ctl channel; sudo ot-ctl panid; sudo ot-ctl dataset active -x
  exit 0
fi

log "1. Build and install ot-br-posix"
sudo apt-get update
sudo apt-get install -y git build-essential ninja-build cmake python3-venv
[ -d "$HOME/ot-br-posix" ] || git clone --depth 1 https://github.com/openthread/ot-br-posix "$HOME/ot-br-posix"
cd "$HOME/ot-br-posix"
git rev-parse HEAD | tee "$HOME/otbr-commit.txt"     # reproducibility datum

./script/bootstrap
# NAT64 off: it adds a translation hop that would pollute the latency we attribute
# to the hub.  Keep the control condition as plain as possible.
INFRA_IF_NAME="$INFRA_IF" NAT64=0 DNS64=0 NETWORK_MANAGER=0 ./script/setup

log "2. Point otbr-agent at the dongle"
sudo sed -i "s|^OTBR_AGENT_OPTS=.*|OTBR_AGENT_OPTS=\"-I wpan0 -B $INFRA_IF $RADIO_URL trel://$INFRA_IF\"|" \
  /etc/default/otbr-agent
grep OTBR_AGENT_OPTS /etc/default/otbr-agent
sudo systemctl restart otbr-agent
sleep 3
systemctl is-active otbr-agent || { sudo journalctl -u otbr-agent -n 40 --no-pager; exit 1; }

log "3. Form the Thread network (channel $CHANNEL, panid $PANID)"
sudo ot-ctl dataset init new
sudo ot-ctl dataset channel "$CHANNEL"
sudo ot-ctl dataset panid "$PANID"
sudo ot-ctl dataset networkname "$NETNAME"
sudo ot-ctl dataset commit active
sudo ot-ctl ifconfig up
sudo ot-ctl thread start
sleep 10

log "4. Record credentials -- THE PAPER NEEDS THESE"
{
  echo "captured   : $(date -Iseconds)"
  echo "otbr-commit: $(cat "$HOME/otbr-commit.txt")"
  echo "state      : $(sudo ot-ctl state       | head -1)"
  echo "channel    : $(sudo ot-ctl channel     | head -1)"
  echo "panid      : $(sudo ot-ctl panid       | head -1)"
  echo "extpanid   : $(sudo ot-ctl extpanid    | head -1)"
  echo "networkname: $(sudo ot-ctl networkname | head -1)"
  echo "networkkey : $(sudo ot-ctl networkkey  | head -1)"
  echo "eui64      : $(sudo ot-ctl eui64       | head -1)"
  echo "dataset-hex: $(sudo ot-ctl dataset active -x | head -1)"
} | tee "$OUT"

log "Done"
echo "State should read 'leader'. Dataset saved to $OUT"
echo
echo "The dataset-hex line is what commissions the lock onto this network:"
echo "  chip-tool pairing code-thread <node-id> hex:<dataset-hex> <pairing-code> \\"
echo "      --storage-directory ~/ncs-door-lock/chip-tool-storage"
