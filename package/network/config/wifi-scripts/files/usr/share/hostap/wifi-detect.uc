#!/usr/bin/env ucode
'use strict';
import { readfile, writefile, realpath, glob, basename, unlink, open, rename } from "fs";
import { is_equal } from "/usr/share/hostap/common.uc";
import { freq_to_channel, freq_range_match, usable_channel_blocks, band_capabilities } from "wifi.channels";
let nl = require("nl80211");

let base_file = "/etc/board-base.json";
let board_file = "/tmp/sysinfo/board.json";
let base_data = readfile(base_file);
if (!base_data)
	exit(1);

let prev_data = readfile(board_file);
let prev_board_data = prev_data ? json(prev_data) : null;
let board_data = json(base_data);

function phy_idx(name) {
	return +rtrim(readfile(`/sys/class/ieee80211/${name}/device_index`) ??
		readfile(`/sys/class/ieee80211/${name}/index`));
}

function phy_path(name) {
	let devpath = realpath(`/sys/class/ieee80211/${name}/device`);

	devpath = replace(devpath, /^\/sys\/devices\//, "");
	if (match(devpath, /^platform\/.*\/pci/))
		devpath = replace(devpath, /^platform\//, "");
	let dev_phys = map(glob(`/sys/class/ieee80211/${name}/device/ieee80211/*`), basename);
	sort(dev_phys, (a, b) => phy_idx(a) - phy_idx(b));

	let ofs = index(dev_phys, name);
	if (ofs > 0)
		devpath += `+${ofs}`;

	return devpath;
}

function wiphy_path_match(entry_path, path) {
	if (type(entry_path) == "array")
		return index(entry_path, path) >= 0;

	return entry_path == path;
}

function wiphy_get_entry(phy, path) {
	board_data.wlan ??= {};

	let wlan = board_data.wlan;
	for (let name in wlan)
		if (wiphy_path_match(wlan[name].path, path))
			return wlan[name];

	wlan[phy] = {
		path: path
	};

	return wlan[phy];
}

function wiphy_detect() {
	let phys = nl.request(nl.const.NL80211_CMD_GET_WIPHY, nl.const.NLM_F_DUMP, { split_wiphy_dump: true });
	if (!phys)
		return;

	for (let phy in phys) {
		if (!phy)
			continue;

		let name = phy.wiphy_name;
		let path = phy_path(name);
		let info = {
			antenna_rx: phy.wiphy_antenna_avail_rx,
			macaddr: trim(readfile(`/sys/class/ieee80211/${name}/macaddress`)),
			antenna_tx: phy.wiphy_antenna_avail_tx,
			bands: {},
			radios: []
		};

		for (let radio in phy.radios) {
			// S1G is not supported yet
			radio.freq_ranges = filter(radio.freq_ranges,
				(range) => range.end > 2000000
			);

			if (!length(radio.freq_ranges))
				continue;

			push(info.radios, {
				index: radio.index,
				freq_ranges: map(radio.freq_ranges,
					(range) => [ range.start, range.end ]
				),
				bands: {}
			});
		}

		let bands = info.bands;
		for (let band in phy.wiphy_bands) {
			if (!band || !band.freqs)
				continue;
			let freq = band.freqs[0].freq;
			let band_name;
			if (freq > 50000)
				band_name = "60G";
			else if (freq > 5900)
				band_name = "6G";
			else if (freq > 4000)
				band_name = "5G";
			else if (freq > 2000)
				band_name = "2G";
			else
				continue;
			let band_info = bands[band_name] = band_capabilities(band, band_name);
			let ap = band_info.ap = band_capabilities(band, band_name, true);
			let has_channel_blocks = band_name in [ "2G", "5G", "6G" ];
			if (has_channel_blocks) {
				band_info.channel_blocks = usable_channel_blocks(band_name, band.freqs, band_info.max_width);
				ap.channel_blocks = usable_channel_blocks(band_name, band.freqs, ap.max_width);
			}

			for (let radio in info.radios) {
				let freq_match = filter(band.freqs,
					(freq) => freq_range_match(radio.freq_ranges, freq.freq)
				);
				if (!length(freq_match))
					continue;

				let radio_band = {};
				radio.bands[band_name] = radio_band;
				if (has_channel_blocks) {
					radio_band.channel_blocks = usable_channel_blocks(band_name, freq_match,
					                                                   band_info.max_width, radio.freq_ranges);
					radio_band.ap_channel_blocks = usable_channel_blocks(band_name, freq_match,
					                                                      ap.max_width, radio.freq_ranges);
				}

				freq_match = filter(freq_match,
					(freq) => !freq.disabled
				);

				let freq = freq_match[0];
				if (freq)
					radio_band.default_channel = freq_to_channel(freq.freq);
			}

			for (let freq in band.freqs) {
				if (freq.disabled)
					continue;
				let chan = freq_to_channel(freq.freq);
				if (!chan)
					continue;
				band_info.default_channel = chan;
				break;
			}
		}

		let entry = wiphy_get_entry(name, path);
		entry.info = info;
	}
}

wiphy_detect();
if (!is_equal(prev_board_data, board_data)) {
	let new_file = board_file + ".new";
	unlink(new_file);
	let f = open(new_file, "wx");
	if (!f)
		exit(1);
	f.write(sprintf("%.J\n", board_data));
	f.close();
	rename(new_file, board_file);
}
