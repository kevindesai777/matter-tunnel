# Reproducing

Everything here was run on macOS driving an nRF52840-DK, with a Raspberry Pi 5
acting as border router and BLE central. Nothing depends on that arrangement, but
the gotchas at the end are where the time actually went, and several are
platform-specific.

**Read [../SECURITY.md](../SECURITY.md) first if you intend to build on this rather
than only reproduce it.**

---

## 0. What you need

| | |
|---|---|
| Device under test | nRF52840-DK (PCA10056) |
| Border-router radio | nRF52840 Dongle (PCA10059) flashed with `ot-rcp` |
| Border-router host | Raspberry Pi (this used a Pi 5 on Raspberry Pi OS Trixie) |
| Host toolchain | nRF Connect SDK v3.4.0 LTS via `nrfutil sdk-manager` |
| Optional | a commercial Matter hub, for the treatment condition |

The dongle is needed only for the border-router control condition. The BLE floor
needs no dongle, and the on-device benchmark needs nothing but the DK.

**Install `nrfutil` from Nordic's own installer, never from pip or Homebrew.** Those
give you the legacy Python tool, which has no `sdk-manager`. Then
`nrfutil install device`.

Every `west` command below runs inside the toolchain. It accepts arbitrary commands,
not only an interactive shell:

```bash
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir <dir> -- <command>
```

---

## 1. Workspace

The door lock sample no longer exists in NCS v3.4.0: `nrf/samples/matter/lock/`
contains only a `README.rst`. Per the v3.4.0 release notes it was relocated to the
nRF Door Lock and Access Control Add-on, so `west build nrf/samples/matter/lock`
fails at CMake. Use a separate west workspace built from the add-on manifest:

```bash
west init -m https://github.com/nrfconnect/ncs-door-lock-and-access-control --mr main project-workspace
cd project-workspace
west update && west patch apply && west zephyr-export
```

Three things make this safe rather than a compromise: the add-on manifest **pins
`nrf` at v3.4.0** with `import: true`, so the LTS reproducibility pin survives;
`applications/matter-door-lock-app` is the Matter-only application, with
`nrf52840dk/nrf52840` in its supported-platforms table and a real board overlay; and
the manifest's `group-filter` excludes the Qorvo `qm35-aliro-sdk`, `vendor1` and the
extensions repo by default, so there is no UWB dependency to satisfy.

`west zephyr-export` adds a second CMake package registration rather than clobbering
an existing one. CMake keys by path hash and `west build` resolves `ZEPHYR_BASE` from
the workspace topdir, so a `/opt` install and this workspace coexist.

---

## 2. Generate the data model

`zap/generated/zap-generated/` is deliberately not in the repository: it is about
16 MB of Matter SDK output, nearly all of it standard clusters unrelated to this
work, derived from a pinned SDK commit. Generate it in three steps.

```bash
# 1. register the cluster in a ZCL database
west zap-append --clusters zap/VendorTunnelCluster.xml -o zap/generated/zcl.json

# 2. derive an application .zap from the stock base
python3 zap/add_tunnel_cluster.py \
  --base      <workspace>/ncs-door-lock-and-access-control/applications/matter-door-lock-app/src/default_zap/lock.zap \
  --out       zap/generated/lock-tunnel.zap \
  --zcl       zap/generated/zcl.json \
  --templates <workspace>/modules/lib/matter/src/app/zap-templates/app-templates.json

# 3. generate the Matter data model. The -f (full) is not optional, see gotcha 2
west zap-generate -f \
  -z zap/generated/lock-tunnel.zap \
  -j zap/generated/zcl.json \
  -o zap/generated/zap-generated
```

`add_tunnel_cluster.py` is a script rather than a hand-edited `.zap` on purpose: it
is re-runnable against a future SDK bump, it documents exactly what changed, and the
stock application is never modified in place, so its provenance holds.

Step 3 emits conformance warnings about `Identify TriggerEffect`, `Basic Information
ConfigurationVersion`, Thread diagnostics attributes and the Door Lock `USR` feature.
**These originate in the stock application, not in this cluster.**

---

## 3. Build and flash

```bash
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir <workspace> -- \
  west build -b nrf52840dk/nrf52840 --sysbuild -d <build-dir> <repo>/examples/lock
west flash -d <build-dir>
```

Expect 797,604 B of flash (81.83%) and 167,452 B of RAM (63.88%) for the full
signed build. If the flash figure lands near 79.8% you have a *sleepy* device and the
wrong device under test. Check that `CONFIG_OPENTHREAD_MTD_SED` and
`CONFIG_CHIP_ENABLE_ICD_SUPPORT` are both unset in the generated `.config`.

For the BLE floor condition, add the overlay. The image name comes from the
application directory, so the option prefix is `lock_`:

```bash
west build ... -- -Dlock_EXTRA_CONF_FILE=<repo>/examples/lock/ble-floor.conf
```

**Advertising is not automatic.** A bare boot logs `commissioning mode 0` and never
advertises. Start it with Button 1 (short press, under 3 s) or over the device shell,
which needs no physical access:

```
matter device opencommissioningwindow
matter ble adv state
```

Commissioning credentials for the example build: manual code `34970112332`,
QR `MT:8IXS142C00KA0648G00`, PIN `20202021`, discriminator `3840`, VID `0xFFF1`,
PID `0x8006`.

### Client signing key

`client_key.pem` is not in the repository and must not be, because it is a private key
registered against one specific device. Generate your own and register it under
physical presence:

```bash
openssl genpkey -algorithm ed25519 -out client_key.pem
# then press Button 4 on the DK to register the client public key
```

---

## 4. Border router (the control condition)

This is the scientifically load-bearing condition. Without it, the numbers
characterise *one commercial path* rather than *hub overhead*, and the decomposition
in the README is not available at all.

```bash
# on the host: build ot-rcp from the SAME pinned workspace, not a prebuilt
west build -b nrf52840dongle/nrf52840 -d <build>/rcp-dongle nrf/samples/openthread/coprocessor

# package for the dongle's factory bootloader (hex is not accepted, see gotcha 11)
nrfutil install nrf5sdk-tools
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req 0x00 \
  --application <build>/rcp-dongle/coprocessor/zephyr/zephyr.hex \
  --application-version 1 rcp-dongle.zip
nrfutil device program --firmware rcp-dongle.zip

# verify over Spinel before trusting it
python3 tools/spinel_probe.py /dev/tty.usbmodem<...>

# on the Pi
./tools/pi-otbr-setup.sh
```

**Choose the channel by energy scan, not by the Wi-Fi overlap map.** On this bench
the a-priori "clear of Wi-Fi 1/6/11" reasoning would have picked channel 20, the
noisiest channel in the band at −22 dBm. Channel 16 was chosen on *spread* (1 dB)
rather than mean, because the protocol reports p99 and worst-case interference
dominates the tail. `pi-otbr-setup.sh` defaults to channel 15; override `CHANNEL`
with whatever your own scan justifies, and record it. The RF context is part of the
result.

**Link margin dominates the tail.** The control condition was measured twice,
changing only the physical distance between DK and RCP. At −85 dBm the p99 was
179 ms with a 1973 ms maximum; at −26 dBm it was 115 ms with a 300 ms maximum, and
the signing effect size went from 0.8σ to 4.9σ. Both datasets are in `measure/data/`
(`results-otbr.csv` and `results-otbr-final.csv`). The weak-link run is retained as
evidence, not as a result.

---

## 5. Measure

```bash
# Matter conditions
python3 measure/harness.py --samples 1000 --counter-start <above the device floor> \
  --out measure/data/results-<condition>.csv

# BLE floor, run from the Pi (gotcha 6: this cannot be done from macOS)
python3 measure/harness_ble.py --samples 1000 --counter-start <...> --out <...>.csv

# regenerate every published figure
python3 measure/analyse.py
python3 measure/analyse.py data/results-ble.csv --interval 15
```

**Signed samples cannot be repeated.** Each needs a fresh counter and signature or
the replay guard rejects it, correctly. This is why `chip-tool --repeat-count` is
unusable here and every signed sample is generated individually. Start the counter
above the device's current floor; a rejection returns the floor, so recovery is
cheap, but a whole run below it is wasted.

The harness takes latency from chip-tool's own EM-layer timestamps, TX of
`InvokeCommandRequest` to RX of `InvokeCommandResponse`, so its own overhead stays
outside the measurement. Conditions are interleaved rather than blocked, because the effect being
measured (~40 ms) is smaller than the observed spread, and blocking would let RF
drift correlate with the treatment.

### On-device benchmark

`tunnel_bench.cpp` measures sign and verify with the Cortex-M DWT cycle counter,
sidestepping the network entirely. It is the primary cost evidence and the
lowest-noise instrument available here. Build with `bench.conf` / `bench-cc310.conf` from
`evidence/device-logs/` to reproduce the algorithm comparison.

---

## 6. Gotchas that cost real time

1. **Building `chip-tool` in-tree breaks the embedded build.** `scripts/bootstrap.sh`
   overwrites the *tracked* file `build_overrides/pigweed_environment.gni`, and every
   later Zephyr build then fails in GN with a path resolving to filesystem root.
   Recover with `git checkout -- build_overrides/pigweed_environment.gni`; build
   chip-tool from a separate clone.

2. **A manufacturer-specific cluster needs `west zap-generate -f` (full).** Default
   generation emits dispatch tables referencing C++ types it never generates, and the
   error surfaces in a *generated* file, which makes it look like the cluster XML is
   malformed. It is not.

3. **`cluster-objects.cpp` is hardcoded to the SDK copy** in
   `chip_data_model.cmake`, while the adjacent `Accessors.cpp` honours
   `CHIP_APP_ZAP_DIR`. Full generation therefore gives you headers but not
   implementations, and the link fails on undefined `Encode`/`Decode`. The fix is
   `lib/tunnel/tunnel_cluster_objects.cpp`, a four-line translation unit including
   only the `VendorTunnel/*.ipp` fragments. Compiling the full generated copy instead
   collides on every standard cluster.

4. **Image-scoped `-D` options fail silently under the wrong image name.** Ours is
   `lock`, from `examples/lock`, so it is `-Dlock_EXTRA_CONF_FILE=…`. A wrong prefix
   is a no-op, not an error.

5. **AP-Protect may be enabled on a new DK.** `west flash` fails with *"The
   Application core access port is currently closed"*. Fix with
   `west flash --recover`, which erases flash and UICR and unlocks the AP. Nothing
   precious is lost, because this build uses test attestation rather than a production
   DAC.

6. **macOS serial: `stty -f <port>` then `cat <port>` returns garbage.** `stty`
   closes the port, and termios resets the line to 9600 before `cat` reopens it. Use
   `tools/serial_cap.py`, which sets the baud rate on the handle it reads from. The
   console really is 115200.

7. **BLE from Python on macOS is impossible without an app bundle.** `bleak` aborts
   with SIGABRT: TCC requires `NSBluetoothAlwaysUsageDescription` in the calling
   binary's `Info.plist`, so CoreBluetooth never even prompts. Do not retry it. Run the
   BLE harness from the Pi.

8. **`psa_open_key()` was removed in PSA Crypto 1.0.** Probe for an existing
   persistent key with `psa_get_key_attributes()`.

9. **chip-tool log parsing:** strip the whole ANSI CSI family
   (`\x1b\[[0-9;]*[A-Za-z]`), not just colour codes ending in `m`, because `\x1b[0J`
   precedes the timestamp. And chip-tool's own log prefix contains `]`, which
   terminates a naive `\[(.*?)\]` byte-array match early.

10. **`chip-tool interactive start` prints nothing until the first command.** Do not
    block waiting for a readiness banner; issue a warm-up command and discard its
    timing.

11. **`nrfutil device program` will not take a `.hex` for the dongle.** For a
    `nordicDfu` device the only accepted format is an SdfuZip `.zip`. Build it with
    `nrfutil install nrf5sdk-tools` → `nrfutil nrf5sdk-tools pkg generate`. The "not
    signed" banner is expected; the dongle's factory Open Bootloader is
    signature-less. Note also that `nrfutil device program` prints nothing to a pipe,
    so check `nrfutil device list` before concluding a flash failed: a successful flash
    leaves DFU mode, and the *next* call is the one that errors.

12. **The dongle RCP links at `0x1000` and needs no MCUboot.**
    `CONFIG_USE_DT_CODE_PARTITION` is not set, so the board DTS
    `zephyr,code-partition` is ignored and `CONFIG_FLASH_LOAD_OFFSET=0x1000` wins.
    Confirm with `objdump -h`. Sysbuild produces no `merged.hex` for this target;
    do not go looking for one.

13. **The device's `MaxNetworks` is 1.** `AddOrUpdateThreadNetwork` returns
    `networkingStatus: 2` (BoundsExceeded) until you `RemoveNetwork` the existing
    one. Do both inside a single armed fail-safe so the whole move stays revertible.
    Note `chip-tool networkcommissioning` takes Breadcrumb as `--Breadcrumb`, not
    positionally, whereas `generalcommissioning arm-fail-safe` does take it
    positionally. The two clusters differ.

14. **After a live network move, the border router holds a stale address-cache entry
    and silently drops every packet.** The device shows as an attached child with
    plausible RSSI, DNS-SD and SRP records are correct, and yet ICMP and CASE both
    see 100% loss. The tell is `ot-ctl eidcache` showing `<addr> fffe retry` together
    with `counters mac` reporting `TxRetry: 0`. Zero retries means the packets never
    reached the radio, so it is not an RF problem however bad the RSSI looks. Reset
    the device so it re-attaches under a fresh RLOC. Do not chase RSSI, USB-3 noise
    or antenna placement first.

15. **A USB extender can kill the RCP silently, and `systemctl` will still report
    `active`.** The signature of a charge-only cable or a loose seat is a `USB
    disconnect` followed by `RCP device disconnected (EOF)` and then *no further USB
    events at all*, no over-current and no reset attempt. `otbr-agent` is a sysv unit
    with no auto-restart. Nothing is lost when the RCP disappears: Thread credentials
    live on the *host* at `/var/lib/thread/0_<rcp-eui64>.data`, keyed to the dongle's
    EUI64. Replug the same dongle and restart `otbr-agent`.

16. **BLE: the ATT MTU stays at 23 unless the *device* asks.**
    `CONFIG_BT_L2CAP_TX_MTU=247` only sets what the peripheral will accept, and BlueZ
    as central never initiates an exchange. The symptom is misleading: a 7-byte
    unsigned echo round-trips fine while every 79-byte signed sample times out. Call
    `bt_gatt_exchange_mtu()` from the peripheral on connect, which needs
    `CONFIG_BT_GATT_CLIENT=y`.

17. **BLE: connectable advertising does not resume after a disconnect.** Once a
    central connects, advertising stops and stays stopped, so the device becomes
    invisible and the *next* run cannot connect at all without a power cycle.
    Re-insert the advertising request from a `disconnected` callback.

18. **BLE: the connection interval must be re-asserted after CHIP's override
    settles.** A first attempt measured signing at +1.6 ms because BlueZ ran the link
    at a 45 ms interval, so a 40 ms operation fitted inside a single interval and
    vanished. That run was clean, plausible and wrong.

19. **Raspberry Pi OS Trixie is PEP-668 externally managed.** Install pyserial with
    `apt install python3-serial`, not pip. `userconf.txt` creates the account but not
    the `NOPASSWD` sudoers drop-in that the first-boot wizard installs, so add
    `/etc/sudoers.d/010_pi-nopasswd` by hand. `ssh host 'sudo …'` fails with *"a
    terminal is required to read the password"*, so use `ssh -t`. And `lsb_release -i`
    returns `Debian` on Raspberry Pi OS, which is what `ot-br-posix` keys `PLATFORM`
    off, so Trixie needs no special handling.

20. **zsh does not word-split an unquoted `$VAR`.** `SSH="ssh -o … host"; $SSH 'cmd'`
    fails with "command not found: ssh -o …". Use a shell function.

21. **Flashing a DK from a Linux host needs OpenOCD, not `nrfutil`.** `nrfutil
    device` warns *"JLinkARM DLL not found"*, because programming a DK over its
    J-Link OB needs SEGGER's package, whose download sits behind a licence
    acceptance. OpenOCD drives the same J-Link over libusb with no licence:

    ```bash
    sudo openocd -f interface/jlink.cfg -c "transport select swd" \
      -f target/nordic/nrf52.cfg \
      -c "program <image>.hex verify; reset run; exit"
    ```

    The nRF target config lives at `target/nordic/nrf52.cfg`, in a subdirectory, not
    at `target/nrf52.cfg`. Flash `zephyr.signed.hex`, not `zephyr.hex`:
    `runners.yaml` names the signed artifact and MCUboot rejects an unsigned image.
    Check the hex's address range first: it must stay inside slot0 and never touch
    the factory-data or storage partitions.
