# Xiaomi TMI PoE control

`tmi-poe` controls the TMI7604R on Xiaomi P5 and the TMI7608R on Xiaomi P8.
It uses the board's QUP I2C controller and the named `poe-reset` GPIO. The
controller is matched by its device-tree node under `/sys/bus/i2c/devices`;
the I2C adapter number is not assumed to be zero.

`/etc/init.d/tmi-poe` starts the service at boot. Startup resets the PSE,
programs and verifies its configuration, then enables automatic af/at and
Class4+ PD detection and classification. It does not force power onto an
undetected device. Chip overcurrent, disconnect and thermal protection remain
in control of the powered ports; the driver does not override classification
current limits to make a PD draw more power.
Stopping the service shuts down all PoE outputs. Reloading a changed power
budget or Class4+ setting briefly shuts down outputs while updating it.

The chip datasheets describe IEEE 802.3af/at and a proprietary Class4+
extension, with up to 50 W advertised for the chip under the latter mode.
They do not claim IEEE 802.3bt support. Compatibility with a bt PD depends on
that PD's fallback/extension behavior; successful boot alone does not validate
bt negotiation or the PD's maximum rated load.

## Power budget

`/etc/config/tmi-poe` accepts `enabled`, `budget_mw`, `class4plus`, `debug`, and a list of
`disabled_ports` (`lan1`, `lan2`, etc.). WAN is never a PoE output.
If `budget_mw` is omitted, the original board budget is used:

| Board | Original supply | Board PoE budget |
| --- | --- | --- |
| P5 | 53 V / 1.25 A | 60,000 mW |
| P8 | 53 V / 2.3 A | 105,000 mW |

The supply ratings were provided for these Xiaomi devices. A smaller
replacement supply needs a lower explicit budget, allowing for the router's
own consumption and conversion losses. For P8 powered by the P5 supply,
30,000 mW is a conservative starting configuration used in the LAN2/C360
test; that test did not validate a continuous 30 W or multiport load.

```sh
uci set tmi-poe.main.budget_mw='30000'
uci commit tmi-poe
/etc/init.d/tmi-poe reload
```

Both supplies have the same nominal voltage. The PSE cannot identify the
adapter's rated current from its input-voltage reading. Its port readings
estimate PoE load, not total router input power or the supply's rated maximum.
No automatic adapter-rating detection is implemented.

`class4plus` defaults to `1`, including upgrades retaining the original UCI
file without this option. Set it to `0` only when restricting the controller
to standard af/at operation. No manual or semiautomatic control is needed.

## Diagnostics

`tmi-poe status` reports requested settings, actual port modes, supply and
power-good state in readable form. It reads hardware without resetting it,
enabling outputs, or consuming event latches. The shared controller lock
prevents it from observing a service's partial configuration/event update.
`tmi-poe status --debug` additionally reports the raw diagnostic snapshot:

- `requested-mask` / `requested-budget-mw`: the current UCI configuration.
- `hw-budget-raw`: the actual threshold register value; `hw-budget-nominal-mw`
  converts it using the factory nominal 53 V encoding, including quantization.
- `hw-detect` / `hw-classify` / `hw-class4plus` and each port's `mode`:
  hardware configuration.
- `powered` / `good`, detection/classification state, voltage and current:
  controller telemetry. Measurements have not been externally calibrated.

For ports with Class4+ enabled, current telemetry reports the raw ADC code
and both documented scales (`current-ma-at-1a`, `current-ma-at-2a`). The PDFs
specify 1.956 and 3.912 mA/LSB but omit the active-range status encoding.
Enabling Class4+ does not prove a particular PD was classified as Class4+;
the utility therefore labels the current scale unresolved and avoids a
misleading single current/power figure. This affects telemetry, not the chip's
automatic classification or hardware protection.

Requested settings can differ from hardware when a service failed to start,
is stopped, or has not reloaded changed UCI settings. `mode=shutdown` and
`powered=0 good=0` describe an off port even when `requested=1`.

Normal logs report controller initialization, connection, reset, verified
configuration, explicit global enable/disable and per-port transitions.
Detection and classification completion are reported once when automatic
classification or confirmed power establishes progress beyond probing. The
repetitive detection-complete latch alone does not advance the normal log.
Powered/PGOOD changes and documented disconnect, startup timeout,
overcurrent and current-limit events are described in words. Repeated
detection on an empty or non-PoE port, unchanged reloads and repeated faults
while waiting for recovery do not repeat normal logs. Continuous voltage and
current fluctuations do not generate normal logs.

The daemon consumes clear-on-read event aliases under its exclusive lock.
Successfully consumed events survive a later sample failure in memory; reads
that may clear an event are never retried. If an I2C transfer fails after the
chip cleared a latch, that event may be unavailable; the error is reported.
Events coalesced by hardware cannot reconstruct every edge or its exact time
between two-second polls. A sampled power-off/reconnect rearms per-port phase
messages. Multiple short changes may be summarized as an interruption already
recovered. Class decoding is not documented: `class=unconfirmed` is intentional,
and `negotiation=completed` requires automatic detection/classification active
and actual powered/PGOOD confirmation. A completion event alone does not prove
successful classification. The Class4+ enable setting is not a negotiated class.

Automatic hardware probing remains enabled while waiting. It is not triggered
by Ethernet carrier: a PD needs power before it can establish link, and carrier
can flap during boot without a PoE disconnect. Disable PoE on a port explicitly
if it must never probe. The chip's automatic protection is unchanged.

Raw registers, event bitmaps, ADC codes and verification expected/actual bytes
are emitted only at debug level when `debug=1`. Normal errors retain the
operation and readable failure reason. Debug-only changes do not interrupt
power:

```sh
uci set tmi-poe.main.debug='1'
uci commit tmi-poe
/etc/init.d/tmi-poe reload
logread -e tmi-poe
```

Set `debug` back to `0` and reload to stop raw diagnostics. A debug snapshot
can be requested with `tmi-poe status --debug` without changing configuration.

The P5/P8 reset pin's electrical defaults are applied with the I2C device's
pinctrl state after the TLMM provider has registered its functions. The
current kernel marks `gpio` as a GPIO function, allowing the userspace line
request while retaining the pin's 8 mA drive and pull-up configuration.
