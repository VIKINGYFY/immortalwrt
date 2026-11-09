# Router Tool wireless defaults

Build systems may set the optional wireless defaults in `/etc/config/router-tool`:

```uci
config settings 'main'

config wireless 'defaults'
	option ssid ''
	option key ''
```

When both values are non-empty, `ssid` produces names such as
`Home-2.4G` and `Home-5.2G`, with the same key on every band. If either value is
empty, Router Tool uses the system hostname, vendor, and band in the SSID and
`12345678` as the key. Hostname case is preserved and the vendor's first letter
is capitalized. The suffix follows the radio's capabilities (`2.4G`, `5.2G`,
`5.8G`, or `6G`). Names are shortened to 32 bytes without splitting UTF-8
characters. A custom key must contain 8 to 63 printable ASCII characters,
supporting both WPA2 and WPA3-SAE.

The 5 GHz and 6 GHz radios use automatic channel selection. The 2.4 GHz
default remains channel 1. On a wiphy with multiple radios, each radio's
own band information determines its suffix and bandwidth, so a restricted
5.8 GHz radio keeps the `5.8G` suffix and an 80 MHz default.

These settings are initialization defaults. Only unmarked, untouched APs with
`OWRT` / `12345678` / `psk2+ccmp` are initialized. Existing custom credentials,
STA/mesh/WDS/MLO profiles, and radios with `defaults_done=1` are preserved.
Subsequent LuCI edits and upgrades that retain configuration do not reapply the
build defaults. Do not clear the markers to enforce new credentials on an
already configured router; edit its Wireless form instead.

OpenWRT-CI passes `WRT_SSID` and `WRT_WORD` through this section. It must leave
the wifi-scripts generator's untouched-AP values intact, so custom build
credentials do not suppress channel, bandwidth, country, and security defaults.
Keep this configuration private when using a private Wi-Fi password.

## Configuration migration

`/etc/uci-defaults/90-router-tool-migrate` owns historical settings and snapshot
normalization and the initial generated ULA cleanup. It removes only the unchanged, owned `wan6`/`local` prefix
filter, including its saved routed value when AP mode is active. Custom filters
remain intact. It also normalizes older routed snapshots before regular service
reloads and LuCI saves can use them.

Regular runtime paths do not repair old snapshot formats. They still record new
sections, restore AP settings, preserve manual edits, and resume interrupted
configuration transactions. These operations are part of normal mode switching.
