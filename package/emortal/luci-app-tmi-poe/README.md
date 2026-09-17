# LuCI TMI PoE control

`tmi-poe` controls the TMI7604R on Xiaomi P5 and the TMI7608R on Xiaomi P8.
It uses the board's QUP I2C controller and the named `poe-reset` GPIO. The
controller is matched by its device-tree node under `/sys/bus/i2c/devices`;
the I2C adapter number is not assumed to be zero.

The `luci-app-tmi-poe` package provides the LuCI page under Control and the
`/etc/init.d/tmi-poe` service. Startup resets the PSE,
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

## LuCI configuration

The global switch, budget, port output selectors and debug switch use the
standard LuCI form. Editing does not change device settings. Save stages the
configuration; Save & Apply applies all changes together through the existing
UCI service reload trigger. Reset discards unsaved edits.

The status panel follows the form's global switch. Polling updates telemetry
only and preserves unsaved port selections. The output column is the configured
preference; power state is derived from hardware mode and powered/PGOOD bits.
Disabling the global switch retains the saved budget and per-port preferences.
Changing a port also removes its legacy `disabled_ports` entry at save time.
Board metadata supplies the port list and budget limit even when the initial
controller query fails; recovering telemetry updates the existing rows.
The protocol column displays only the protocol name, `Unconfirmed` when the
controller cannot identify it reliably, or `--` for a non-powered port.
The page intentionally has no per-port power column. The controller exposes
voltage/current ADC registers, but the supplied documentation leaves the
active current scale unresolved for Class4+ and the values are not externally
calibrated. Reporting a calculated wattage would therefore imply precision the
hardware API cannot guarantee. Connected links are blue; power-good, unpowered,
disabled and fault states use green, blue, orange and red respectively, while
retaining their text labels. Confirmed protocol names are green and an
unconfirmed protocol is red.

## Package layout and translations

The package uses the standard `feeds/luci/luci.mk` build rules: `htdocs/` holds
the JavaScript view and scoped CSS, `root/` holds runtime files, and
`src/Makefile` builds and installs the native daemon. The qualcommbe target
already enables the I2C and GPIO character devices required by the daemon.
Runtime service and UCI names remain `tmi-poe`.
The standard package installation starts the service through procd. Tracking
the executable lets procd replace an old daemon after a binary upgrade, while
an unchanged binary does not restart solely for a page or translation update.
Configuration updates use the daemon's validated HUP reload path.

Page and menu strings use English message IDs. `po/templates/tmi-poe.pot` is
the translation template; `po/zh_Hans/tmi-poe.po` supplies Simplified Chinese.
LuCI builds the standard `luci-i18n-tmi-poe-zh-cn` translation package. Install
it alongside the application to use the Chinese page, or select it through
the normal LuCI language settings when building firmware. Native daemon logs
remain diagnostic text and are not rewritten by the page translator.

## Power budget

`/etc/config/tmi-poe` accepts `enabled`, `budget_mw`, `class4plus`, `debug`, per-port
`port_lanN` options, and a list of
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

Normal logs report initialization, I2C adapter readiness, reset sequence,
verified configuration, explicit global enable/disable and per-port transitions.
Opening the adapter does not prove that the PSE responds. Detection and
classification success are reported once automatic mode and powered/PGOOD
confirm successful negotiation. A classification event alone is reported as
an observation with an unconfirmed result. The repetitive detection-complete
latch alone does not advance the normal log.
Debug mode reports a detection-event bit once per observable port state;
identical empty/non-PD probes do not fill the log, including when separated
by polls without events. Port state changes or re-enabling debug rearm it.
Other raw event types remain available in debug mode.
Powered/PGOOD changes and documented disconnect, startup timeout,
overcurrent and current-limit events are described in words. Repeated
detection on an empty or non-PoE port, unchanged reloads and repeated faults
while waiting for recovery do not repeat normal logs. Continuous voltage and
current fluctuations do not generate normal logs. Fault recovery requires a
later powered/PGOOD sample without a newly reported fault. A DC-disconnect
event identifies the controller's load-disconnect observation, not proof of
a physical unplug. Power-off and power-good registers are read separately;
an off output with power-good still set is reported as shutdown unconfirmed.

On failure, the service still attempts output shutdown. An initialization or
reload error and its cleanup result are combined into one error record, keeping
the original stage and reason. Repeated I2C errors do not hide the final output
state: a failed cleanup explicitly reports it as unconfirmed. Normal SIGTERM
or SIGINT cancellation is not labeled a controller failure. Sampling failures
are logged on the first occurrence or when their details change; three
consecutive failures stop monitoring and initiate shutdown.

Each complete sample checks requested modes, detection/classification,
DC-disconnect protection, Class4+ and budget against hardware. A controller
reset or lost setting therefore cannot silently leave the service running
under different settings. A mismatch logs the affected setting and shuts down;
the service does not automatically reset or repower the controller. This adds
one ordinary protection-register read to each sample. Status queries remain
read-only and do not enforce policy. The checks do not reconstruct undocumented
registers or establish that every analog protection circuit is healthy.

The daemon consumes clear-on-read event aliases under its exclusive lock.
Its monitoring sample reuses those consumed events and skips the nine redundant
non-clearing event reads. Read-only status queries still use the non-clearing
aliases and never consume events needed by the daemon.
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
power. Enabling debug emits a baseline on the next successful sample, and a
reload that also changes policy enables requested diagnostics before applying
the hardware update. Other consumed event classes remain visible on successive
samples; repeated detection events follow the suppression described above:

```sh
uci set tmi-poe.main.debug='1'
uci commit tmi-poe
/etc/init.d/tmi-poe reload
```

The LuCI page refreshes the last 200 lines from `/var/log/tmi-poe.log` every
three seconds, preserving text selection and a manually scrolled position.
The Clear log action immediately clears the current and rotated logs. It does
not save pending form edits, reload the service or access the PSE. New events
continue to appear. The same operation is available as `tmi-poe clear-log`.
Clearing and rotation share a log lock; the active file is truncated in place
so the running daemon keeps its valid append descriptor. The daemon
does not send PoE messages to syslog. The file is capped at 128 KiB and rotated
once to `/var/log/tmi-poe.log.1`.

Set `debug` back to `0` and reload to stop raw diagnostics. A debug snapshot
can be requested with `tmi-poe status --debug` without changing configuration.
Failed status queries report their stage and readable cause on stderr and
return a nonzero exit code. Successful JSON remains on stdout. Query failures
do not append to the daemon's log, so page polling cannot fill it with errors.

The P5/P8 reset pin's electrical defaults are applied with the I2C device's
pinctrl state after the TLMM provider has registered its functions. The
current kernel marks `gpio` as a GPIO function, allowing the userspace line
request while retaining the pin's 8 mA drive and pull-up configuration.
