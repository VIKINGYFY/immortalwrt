#!/usr/bin/env ucode
import { readfile, open, stat, glob, realpath, unlink } from "fs";
import * as uci from 'uci';
import { connect, error as ubus_error } from 'ubus';
import { find_phy, json_file_read } from 'wifi.utils';
import { maximum_channel_plan } from 'wifi.channels';

const bands_order = [ "60G", "6G", "5G", "2G" ];
const htmode_order = [ "EHT", "HE", "VHT", "HT" ];

const DEFERRED = 75;
const notify_file = '/var/run/wifi-config.notify';

function retry_hotplug(bus) {
	let devpath = getenv('DEVPATH');
	let pos = index(devpath, '/ieee80211/');
	let device = pos >= 0 ? realpath('/sys' + substr(devpath, 0, pos)) : null;
	if (!device) {
		bus.call('network.wireless', 'retry', {});
		return;
	}

	let status = bus.call('network.wireless', 'status', {});
	let devices = [];
	for (let name, state in status) {
		if (state.up || state.disabled || !state.autostart)
			continue;
		let phy = find_phy(state.config, true);
		if (phy && realpath(`/sys/class/ieee80211/${phy}/device`) == device)
			push(devices, name);
	}
	if (length(devices))
		bus.call('network.wireless', 'retry', { devices });
}

function provision_country_get() {
	let provision;

	try {
		provision = import("provision");
	} catch (e) {
		return;
	}

	let ctx = provision.open();
	if (!ctx)
		return;

	ctx.init();

	return ctx.get("wireless.country");
}

function generate() {
	if (length(glob('/var/run/rpcd/snapshot-files/*')) || stat('/tmp/.uci/wireless')?.size)
		return DEFERRED;
	if (system([ 'ucode', '/usr/share/hostap/wifi-detect.uc' ]))
		return 1;
	let board = json_file_read('/etc/board.json');
	let wlan = board?.wlan;
	if (!wlan)
		return 0;

	let provision_country = provision_country_get();
	let live_plan = stat('/usr/share/schema/wireless.wifi-device.json');

	let idx = 0;
	let commit;

	let path = '/etc/config/wireless';
	let cfg = uci.cursor();
	if (!stat(path) && !open(path, 'a')?.close())
		die('Cannot create wireless configuration');
	// Absolute loading excludes CLI and rpcd deltas. Further lookups find the
	// already-loaded package by its UCI name, including at commit time.
	let config = cfg.get_all(path);
	if (!config) die('Cannot read wireless configuration');

	// The optional build-time input is still provided by OpenWRT-CI.
	let preset = cfg.get_all('/etc/config/router-tool')?.defaults;
	if (preset?.['.type'] != 'wireless' || !preset.ssid || !preset.key) {
		preset = null;
	} else if (type(preset.ssid) != 'string' || length(preset.ssid) > 32 ||
	           match(preset.ssid, /[[:cntrl:]]/) || type(preset.key) != 'string' ||
	           !match(preset.key, /^[ -~]{8,63}$/)) {
		warn('Ignoring invalid Wi-Fi factory SSID or key override\n');
		preset = null;
	}

	function default_ssid(band, first_channel) {
		let hostname = preset?.ssid || cfg.get_first('system', 'system', 'hostname') || 'OWRT';
		let suffix = { '2G': '2.4G', '5G': first_channel >= 100 ? '5.8G' : '5.2G', '6G': '6G' }[band];
		if (!preset) {
			let vendor = split(board.model?.id || 'unknown', ',')[0];
			vendor = uc(substr(vendor, 0, 1)) + substr(vendor, 1);
			suffix = substr(vendor, 0, 32 - length(suffix) - 3) + '-' + suffix;
		}
		let size = min(length(hostname), 31 - length(suffix));
		// Ucode offsets count bytes, so trim the prefix at a UTF-8 boundary.
		while (size > 0 && (ord(hostname, size) & 0xc0) == 0x80) size--;
		return substr(hostname, 0, size) + '-' + suffix;
	}

	function set_section(name, type_name, options) {
		if (!cfg.set('wireless', name, type_name)) die(`Cannot create wireless.${name}`);
		for (let key, value in options)
			if (value != null && !cfg.set('wireless', name, key, value))
				die(`Cannot set wireless.${name}.${key}`);
	}

	function radio_exists(path, macaddr, phy, radio) {
		for (let name, s in config) {
			if (s[".type"] != "wifi-device")
				continue;
			if (radio != null && int(s.radio) != radio)
				continue;
			if (s.macaddr && lc(s.macaddr) == lc(macaddr))
				return true;
			if (s.phy == phy)
				return true;
			if (!s.path || !path)
				continue;
			if (substr(s.path, -length(path)) == path)
				return true;
		}
	}

	for (let phy_name, phy in wlan) {
		let info = phy.info;
		if (!info || !length(info.bands))
			continue;

		let radios = length(info.radios) > 0 ? info.radios : [{ bands: info.bands }];
		for (let radio in radios) {
			while (config[`radio${idx}`] || config[`default_radio${idx}`])
				idx++;
			let name = "radio" + idx;

			let iface = "default_" + name;

			let band_name = filter(bands_order, (b) => radio.bands[b])[0];
			if (!band_name)
				continue;

			let band = info.bands[band_name];
			let rband = radio.bands[band_name];
			let channel = rband.default_channel ?? "auto";
			let lower_band = lc(band_name);

			let width = band.max_width;
			if (band_name == "2G")
				width = 20;
			else if (width > 80)
				width = 80;

			let htmode = filter(htmode_order, (m) => band[lc(m)])[0];
			if (htmode)
				htmode += width;
			else
				htmode = "NOHT";

			if (!phy.path)
				continue;

			let macaddr = trim(readfile(`/sys/class/ieee80211/${phy_name}/macaddress`));
			if (radio_exists(phy.path, macaddr, phy_name, radio.index))
				continue;

			let id = match(phy_name, /^phy[0-9]/) ? { path: phy.path } : { phy: phy_name };
			let configured_country = provision_country || wlan.defaults?.country;
			let country = uc(configured_country || 'US');
			let ssids = wlan.defaults?.ssids;
			let defaults = ssids?.[lower_band]?.ssid ? ssids[lower_band] : ssids?.all;

			if (!configured_country && lower_band != '2g')
				defaults = null;

			let needs_country = band_name == '6G' && !match(country, /^[A-Z]{2}$/);
			let ap_defaults = !defaults?.ssid && band_name in [ '2G', '5G', '6G' ];
			let plan, auto_htmode, channels;
			if (ap_defaults) {
				let ap = band.ap ?? band;
				let blocks = radio.index == null ? ap.channel_blocks : (rband.ap_channel_blocks ?? rband.channel_blocks);
				plan = maximum_channel_plan(ap.modes, blocks, {});
				if (live_plan || plan) {
					channel = 'auto';
					htmode = plan?.htmode ?? htmode;
					// Regdom may still be changing when the first PHY is detected.
					if (live_plan) auto_htmode = htmode;
					else channels = join(' ', plan.channels);
				}
			}
			let sae = ap_defaults && band_name == '6G';
			set_section(name, 'wifi-device', {
				type: 'mac80211', ...id, band: lower_band, channel: '' + channel,
				htmode, auto_htmode, channels, country,
				radio: radio.index == null ? null : '' + radio.index,
				num_global_macaddr: '' + (ssids?.[lower_band]?.mac_count || '')
			});
			set_section(iface, 'wifi-iface', {
				device: name, network: 'lan', mode: 'ap',
				ssid: defaults?.ssid || (ap_defaults ? default_ssid(band_name, rband.default_channel ?? band.default_channel) : 'OWRT'),
				encryption: defaults?.encryption || (sae ? 'sae+ccmp' : 'psk2+ccmp'),
				ieee80211w: sae ? '2' : null,
				key: defaults?.key || (ap_defaults && preset ? preset.key : '12345678'), disabled: needs_country ? '1' : '0'
			});
			config[name] = {};
			commit = true;
		}
	}

	if (commit) {
		let previously_pending = stat(notify_file);
		if (!previously_pending && !open(notify_file, 'a')?.close())
			die('Cannot record pending wireless configuration notification');
		if (!cfg.commit('wireless')) {
			if (!previously_pending)
				unlink(notify_file);
			die('Cannot commit newly discovered radios');
		}
	}

	return 0;
}

let worker;
if (ARGV[0] == '--deferred') {
	worker = open('/var/lock/wifi-config-retry.lock', 'a');
	if (!worker || !worker.lock('xn'))
		exit(0);
}

for (let delay = 1; ; delay = min(delay * 2, 16)) {
	let lock = open('/var/lock/network-config.lock', 'a');
	if (!lock || !lock.lock('x'))
		die('Cannot lock wireless configuration');

	let result = generate();
	let bus = connect();
	if (!result && stat(notify_file)) {
		// A PHY can appear after netifd has loaded the wireless configuration.
		if (bus) {
			ubus_error();
			bus.call('service', 'event', { type: 'config.change', data: { package: 'wireless' } });
			if (!ubus_error(true))
				unlink(notify_file);
		}
		if (stat(notify_file))
			result = DEFERRED;
	}
	if (result != DEFERRED)
		worker?.close();
	lock.close();

	if (bus && ARGV[0] == '--hotplug')
		retry_hotplug(bus);
	bus?.disconnect();
	if (result != DEFERRED || !worker)
		exit(result);
	sleep(delay * 1000);
}
