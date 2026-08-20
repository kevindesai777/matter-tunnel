# Examples

The component in `lib/tunnel/` carries *no device-type semantics*. It moves opaque
authenticated bytes; what those bytes mean is the application's business. An example
here is one instance of dropping it in, not the thing itself.

```
lock/         Nordic's Matter door lock, with the tunnel dropped in
light_bulb/   Nordic's Matter light bulb, same component, unchanged
```

The lock is the first example for two reasons. It is the highest-stakes case, since a
hub that can open a door is the sharpest form of the authority problem the component
addresses. And Nordic ships a public Matter door lock application to build it on.

The bulb is the second because it is different in every way that could plausibly
matter: a different device class (On/Off and Level Control against Door Lock), a base
application 21 KB larger, and a Full Thread Device where the lock is a Minimal End
Device. What it costs is on the [main README](../README.md); the short version is that
every flash figure is identical to the byte.

## Porting to another device type

Nothing in `lib/tunnel/` changes. `tunnel_server.cpp` states it explicitly, and it is
checkable: the only occurrences of "lock" in the component are code comments and
Matter's own `LockChipStack()` stack mutex. The bulb port did not touch it.

Five steps, none of them in the component:

1. **Point at a different upstream application.** The example `CMakeLists.txt` takes
   `-DUPSTREAM_APP_DIR=…` and compiles the upstream sources *in place* rather than
   copying them. Any nRF Connect SDK Matter sample works. Match `target_sources()` to
   that sample's own list, which is not always the same length: the lock has four
   sources, the bulb three plus one SDK source it compiles directly.

2. **Take the sample's board overlay and sysbuild config**, not the lock's. The bulb
   needs PWM wired to its brightness LED and a different partitions `.dtsi`. Copying
   these from the sample keeps the build differenced against the right baseline.

3. **Re-derive the data model** against that sample's stock `.zap`:

   ```bash
   python3 zap/add_tunnel_cluster.py --base <sample>/src/default_zap/<sample>.zap \
     --out zap/generated-<name>/<name>-tunnel.zap --zcl ../generated/zcl.json --templates …
   ```

   The script picks the first application endpoint, skipping the root device; pass
   `--endpoint-type N` to override. Give each port its **own** output directory:
   `CHIP_APP_ZAP_DIR` is derived as `<directory of the .zap>/zap-generated`, so two
   ports sharing a directory overwrite each other. `west zap-generate` needs that
   output directory to exist before it will run.

4. **Copy `prj.conf`**, changing only the ZAP path and adding the `PSA_WANT_*` options
   the security layer needs. Leave the product name, product ID and Bluetooth name as
   the sample has them if you intend to difference against it — a renamed product puts
   its own string bytes inside the measurement.

   Match the sample's Thread role rather than the lock's. The lock is a non-sleepy
   Minimal End Device and says so explicitly in `prj.conf`; the bulb is mains-powered
   and its Kconfig makes it a Full Thread Device. Forcing one into the other's role
   measures a device that does not exist.

5. **Give the payload meaning in the application.** Both examples treat the payload as
   an opaque command string. This is the only place device semantics belong.

Flash is the binding constraint on nRF52840. The lock lands at 81.83% with the full
security layer and the bulb at 84.00%, because a Full Thread Device links routing code
a Minimal End Device omits. The component's cost is fixed; the budget it has to fit
into is not, so a port onto a larger stock application may need `prj_release.conf` or a
smaller payload ceiling.

## What the second example demonstrated

That the measured cost belongs to the component rather than to one application. Across
the lock and the bulb, every flash delta is identical:

| Step | Lock | Bulb |
|---|---:|---:|
| base → transport only | +808 B | +808 B |
| transport → signing, replay, binding | +17,812 B | +17,812 B |
| signing → client authentication | +1,592 B | +1,592 B |
| **base → complete** | **+20,212 B** | **+20,212 B** |

RAM matches at every stage but the first, where the region figures differ by 64 B. That
64 B is not in the component: its static RAM is 2,273 B in both images symbol for
symbol, the symbol-level delta is +3,577 B in both, and no symbol grows differently
between the ports. The difference is section alignment in the linker's region
accounting, which is worth knowing if you plan to compare region figures to the byte.
