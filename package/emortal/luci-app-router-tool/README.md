# Wi-Fi factory settings

`wifi-scripts` creates the full wireless configuration when it first discovers a
radio. OpenWRT-CI supplies its optional `WRT_SSID` and `WRT_WORD` inputs through
the existing `/etc/config/router-tool` section:

```uci
config settings 'main'

config wireless 'defaults'
	option ssid ''
	option key ''
```

When both values are non-empty, `ssid` produces names such as
`Home-2.4G` and `Home-5.2G`, with the same key on each generic factory AP. An
SSID prefix must contain 1 to 32 bytes without control characters, and a key
must contain 8 to 63 printable ASCII characters. With either input missing or
invalid, the generic AP uses the hostname, vendor, and band for its name and
`12345678` for its key. Names are shortened at a UTF-8 boundary within the
32-byte SSID limit.

The factory generator uses the provisioned country, then the board default,
then `US`. It selects automatic channels for generic 2.4, 5, and 6 GHz APs, with
SAE and required protected management frames on 6 GHz. The ucode backend
plans the usable width and candidate channels from the live PHY during setup.
Existing radios retain their settings on repeated discovery and configuration-
preserving upgrades.

Router Tool displays the running channel plan and handles explicit edits in
the Wireless form. The form reads `channels`, `channel_list`, and `chanlist`
in that order and saves explicit edits to `channels`.

## Saving and applying network presets

Router Tool remembers the committed values when a LuCI session saves its
network form. Checked and unchecked LuCI Apply compare the session's pending
edits with the current values. This includes Apply from the pending changes
dialog. A conflict leaves the draft available to review or revert. Disjoint
edits from other sessions are preserved.

Relay AP mode requests its management address from the upstream router. Use
the assigned address to reconnect. Checked Apply restores the previous
configuration if LuCI cannot confirm the change before the timer expires.

## Configuration migration

Router Tool's early `uci-defaults` script clears the initial ULA `auto` value
before the base system generates a prefix. Explicit ULA prefixes and IPv6
prefix delegation remain available.

`/etc/uci-defaults/90-router-tool-migrate` normalizes historical settings and
snapshots. It removes the unchanged, owned `wan6`/`local` prefix filter,
including its saved routed value when AP mode is active. Custom filters remain
intact.

Regular runtime paths do not repair old snapshot formats. They still record new
sections, restore AP settings, preserve manual edits, and resume interrupted
configuration transactions. These operations are part of normal mode switching.
