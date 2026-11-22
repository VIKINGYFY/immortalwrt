#!/usr/bin/env ucode
'use strict';
import { readfile, writefile, realpath, glob, basename, unlink, open, rename } from "fs";
import { is_equal } from "/usr/share/hostap/common.uc";
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

function freq_to_channel(freq) {
	if (freq < 1000)
		return 0;
	if (freq == 2484)
		return 14;
	if (freq == 5935)
		return 2;
	if (freq < 2484)
		return (freq - 2407) / 5;
	if (freq >= 4910 && freq <= 4980)
		return (freq - 4000) / 5;
	if (freq < 5950)
		return (freq - 5000) / 5;
	if (freq <= 45000)
		return (freq - 5950) / 5;
	if (freq >= 58320 && freq <= 70200)
		return (freq - 56160) / 2160;
	return 0;
}

function freq_range_match(ranges, freq, width) {
	freq *= 1000;
	let half_width = (width ?? 0) * 500;
	for (let range in ranges) {
		if (freq - half_width >= range[0] && freq + half_width <= range[1])
			return true;
	}
	return false;
}

function usable_channel_blocks(band, freqs, max_width, ranges) {
	let enabled = {};
	for (let freq in freqs) {
		if (freq.disabled || (freq.no_ir && !freq.radar) || freq.no_20mhz ||
		    (ranges && !freq_range_match(ranges, freq.freq, 20)))
			continue;
		enabled[freq.freq] = freq;
	}

	let blocks = {};
	function add_block(start, width) {
		if (ranges && !freq_range_match(ranges, start + width / 2 - 10, width))
			return;

		let channels = [], dfs = false, indoor_only = false, no_he = false, no_eht = false;
		for (let offset = 0; offset < width; offset += 20) {
			let freq = enabled[start + offset];
			if (!freq || (width == 320 && freq.no_320mhz) ||
			    (width == 160 && freq.no_160mhz) || (width == 80 && freq.no_80mhz) ||
			    (width == 40 && (offset ? freq.no_ht40_minus : freq.no_ht40_plus)))
				return;

			dfs ||= !!freq.radar;
			indoor_only ||= !!freq.indoor_only;
			no_he ||= !!freq.no_he;
			no_eht ||= !!freq.no_eht;
			// Auto HT40 on 2.4 GHz uses the lower primary with HT40+.
			if (band != "2G" || !offset)
				push(channels, freq_to_channel(freq.freq));
		}

		blocks[width] ??= [];
		push(blocks[width], { channels, dfs, indoor_only, no_he, no_eht });
	}

	// Lowest 20 MHz center of each aligned 5 GHz block, as used by ACS.
	let starts_5g = {
		"160": [ 5180, 5500, 5745 ],
		"80": [ 5180, 5260, 5500, 5580, 5660, 5745, 5825 ],
		"40": [ 5180, 5220, 5260, 5300, 5500, 5540, 5580, 5620,
		      5660, 5700, 5745, 5785, 5825, 5865 ]
	};

	for (let width in [ 320, 160, 80, 40 ]) {
		if (width > max_width)
			continue;
		switch (band) {
		case "2G":
			if (width == 40)
				for (let start = 2412; start + 20 <= 2472; start += 5)
					add_block(start, width);
			break;
		case "5G":
			for (let start in starts_5g[width])
				add_block(start, width);
			break;
		case "6G":
			// The two 320 MHz operating channel grids are offset by 160 MHz.
			for (let start = 5955; start + width - 20 <= 7115; start += min(width, 160))
				add_block(start, width);
			break;
		}
	}

	for (let freq in freqs)
		if (enabled[freq.freq])
			add_block(freq.freq, 20);

	return blocks;
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
			let band_info = {};
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
			bands[band_name] = band_info;
			if (band.ht_capa > 0)
				band_info.ht = true;
			if (band.vht_capa > 0)
				band_info.vht = true;
			let he_phy_cap = 0;
			let eht_phy_cap = 0;

			for (let ift in band.iftype_data) {
				if (!ift.he_cap_phy)
					continue;

				band_info.he = true;
				he_phy_cap |= ift.he_cap_phy[0];

				if (!ift.eht_cap_phy)
					continue;

				band_info.eht = true;
				eht_phy_cap |= ift.eht_cap_phy[0];
			}

			if (band_name == "6G" && (eht_phy_cap & 2))
				band_info.max_width = 320;
			else if (band_name != "2G" &&
			         ((he_phy_cap & 0x18) || ((band.vht_capa >> 2) & 0x3)))
				band_info.max_width = 160;
			else if (band_name != "2G" &&
			         ((he_phy_cap & 4) || band.vht_capa > 0))
				band_info.max_width = 80;
			else if ((band.ht_capa & 0x2) || (he_phy_cap & 0x2))
				band_info.max_width = 40;
			else
				band_info.max_width = 20;

			let has_channel_blocks = band_name in [ "2G", "5G", "6G" ];
			if (has_channel_blocks)
				band_info.channel_blocks = usable_channel_blocks(band_name, band.freqs,
				                                                 band_info.max_width);

			let modes = band_info.modes = [ "NOHT" ];
			if (band_info.ht)
				push(modes, "HT20");
			if (band_info.vht)
				push(modes, "VHT20");
			if (band_info.he)
				push(modes, "HE20");
			if (band_info.eht)
				push(modes, "EHT20");
			if (band.ht_capa & 0x2) {
				push(modes, "HT40");
				if (band_info.vht)
					push(modes, "VHT40")
			}
			if (he_phy_cap & 2)
				push(modes, "HE40");

			if (eht_phy_cap && he_phy_cap & 2)
				push(modes, "EHT40");

			for (let radio in info.radios) {
				let freq_match = filter(band.freqs,
					(freq) => freq_range_match(radio.freq_ranges, freq.freq)
				);
				if (!length(freq_match))
					continue;

				let radio_band = {};
				radio.bands[band_name] = radio_band;
				if (has_channel_blocks)
					radio_band.channel_blocks = usable_channel_blocks(band_name, freq_match,
					                                                  band_info.max_width,
					                                                  radio.freq_ranges);

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

			if (band_name == "2G")
				continue;

			if (he_phy_cap & 4)
				push(modes, "HE40");
			if (eht_phy_cap && he_phy_cap & 4)
				push(modes, "EHT40");
			if (band_info.vht)
				push(modes, "VHT80");
			if (he_phy_cap & 4)
				push(modes, "HE80");
			if (eht_phy_cap && he_phy_cap & 4)
				push(modes, "EHT80");
			if ((band.vht_capa >> 2) & 0x3)
				push(modes, "VHT160");
			if (he_phy_cap & 0x18)
				push(modes, "HE160");
			if (eht_phy_cap && he_phy_cap & 0x18)
				push(modes, "EHT160");

			if (band_name == "6G" && (eht_phy_cap & 2))
				push(modes, "EHT320");
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
