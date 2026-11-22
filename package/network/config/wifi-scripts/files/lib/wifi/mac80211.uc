#!/usr/bin/env ucode
import { readfile, open, stat, glob } from "fs";
import * as uci from 'uci';
import { board_wlan } from 'wifi.utils';

const bands_order = [ "60G", "6G", "5G", "2G" ];
const htmode_order = [ "EHT", "HE", "VHT", "HT" ];

let lock = open('/var/lock/network-config.lock', 'a');
if (!lock || !lock.lock('x'))
	die('Cannot lock wireless configuration');
if (system([ 'ucode', '/usr/share/hostap/wifi-detect.uc' ]))
	exit(1);
if (length(glob('/var/run/rpcd/snapshot-files/*')) || stat('/tmp/.uci/wireless')?.size)
	exit(0);
let wlan = board_wlan();
if (!wlan)
	exit(0);

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

let provision_country = provision_country_get();

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
		band_name = lc(band_name);
		let country = band_name == '6g' ? '00' : null;
		let defaults, num_global_macaddr;
		if (wlan.defaults) {
			defaults = wlan.defaults.ssids?.[band_name]?.ssid ? wlan.defaults.ssids?.[band_name] : wlan.defaults.ssids?.all;
			country = wlan.defaults.country;
			num_global_macaddr = wlan.defaults.ssids?.[band_name]?.mac_count;
		}

		if (provision_country)
			country = provision_country;

		if (!country && band_name != '2g')
			defaults = null;

		set_section(name, 'wifi-device', {
			type: 'mac80211', ...id, band: band_name, channel: '' + channel,
			htmode, country: country || 'CN',
			radio: radio.index == null ? null : '' + radio.index,
			num_global_macaddr: '' + (num_global_macaddr || '')
		});
		set_section(iface, 'wifi-iface', {
			device: name, network: 'lan', mode: 'ap', ssid: defaults?.ssid || 'OWRT',
			encryption: defaults?.encryption || 'psk2+ccmp',
			key: defaults?.key || '12345678', disabled: '0'
		});
		config[name] = {};
		commit = true;
	}
}

if (commit && !cfg.commit('wireless'))
	die('Cannot commit newly discovered radios');
