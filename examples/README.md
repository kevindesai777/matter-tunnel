# Examples

The component in `lib/tunnel/` carries **no device-type semantics**. It moves opaque
authenticated bytes; what those bytes mean is the application's business. An example
here is one instance of dropping it in, not the thing itself.

```
lock/    Nordic's Matter door lock, with the tunnel dropped in
```

The lock is the first example because it is the highest-stakes case — a hub that can
open a door is the sharpest form of the authority problem the component addresses —
and because Nordic ships a public Matter door lock application to build it on.

## Porting to another device type

Nothing in `lib/tunnel/` changes. `tunnel_server.cpp` states it explicitly, and it is
checkable: the only occurrences of "lock" in the component are code comments and
Matter's own `LockChipStack()` stack mutex.

A port is four steps, none of them in the component:

1. **Point at a different upstream application.** `examples/lock/CMakeLists.txt` takes
   `-DUPSTREAM_APP_DIR=…` and compiles the upstream sources *in place* rather than
   copying them. Any nRF Connect SDK Matter sample works — `template`, `light_bulb`
   and `light_switch` all support `nrf52840dk/nrf52840`. Change the four upstream
   source filenames in `target_sources()` to that sample's.

2. **Re-derive the data model** against that sample's stock `.zap`:

   ```bash
   python3 zap/add_tunnel_cluster.py --base <sample>/src/default_zap/<sample>.zap \
     --out zap/generated/<sample>-tunnel.zap --zcl zap/generated/zcl.json --templates …
   ```

   The script exists precisely so this is a re-run rather than a hand edit, and so the
   stock application is never modified in place.

3. **Copy `prj.conf`**, changing the product name, the product ID and the ZAP path.
   Keep `CONFIG_OPENTHREAD_MTD_SED` and `CONFIG_CHIP_ENABLE_ICD_SUPPORT` unset if you
   intend to compare latency against the numbers in the top-level README — a sleepy
   end device has a ~2.3 s polling floor and is a different device under test.

4. **Give the payload meaning in the application.** The lock example treats the
   payload as an opaque command string; a different device type will want a different
   one. This is the only place device semantics belong.

Flash is the binding constraint on nRF52840 — the lock example lands at 81.83% with
the full security layer — so a port onto a larger stock application may need
`prj_release.conf` or a smaller payload ceiling.

## What a second example would demonstrate

That the measured costs are properties of the component rather than of one
application: `+612 B` of flash for the transport and `+19,652 B` for the security
layer should hold, within the noise of a different base application, wherever the
component is dropped in. Reporting the same two numbers against a second device type
is what turns "portable by construction" into "portable, demonstrated".
