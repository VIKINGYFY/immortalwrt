// SPDX-License-Identifier: GPL-2.0-only

export function freq_to_channel(freq) {
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

export function freq_range_match(ranges, freq, width) {
	freq *= 1000;
	let half_width = (width ?? 0) * 500;
	for (let range in ranges) {
		if (freq - half_width >= range[0] && freq + half_width <= range[1])
			return true;
	}
	return false;
}

export function usable_channel_blocks(band, freqs, max_width, ranges) {
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

		let channels = [], members = [], dfs = false, indoor_only = false, no_he = false, no_eht = false;
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
			push(members, freq_to_channel(freq.freq));
			// Auto HT40 on 2.4 GHz uses the lower primary with HT40+.
			if (band != "2G" || !offset)
				push(channels, freq_to_channel(freq.freq));
		}

		blocks[width] ??= [];
		let center = start + width / 2 - 10;
		push(blocks[width], { channels, members, center, center_channel: freq_to_channel(center), dfs, indoor_only, no_he, no_eht });
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

export function band_capabilities(band, band_name, ap_only) {
	let band_info = {};
	if (band.ht_capa > 0)
		band_info.ht = true;
	if (band.vht_capa > 0)
		band_info.vht = true;
	let he_phy_cap = 0;
	let eht_phy_cap = 0;

	for (let ift in band.iftype_data) {
		if (ap_only && !ift.iftypes?.ap)
			continue;
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
			push(modes, "VHT40");
	}
	if (he_phy_cap & 2)
		push(modes, "HE40");

	if (eht_phy_cap && he_phy_cap & 2)
		push(modes, "EHT40");

	if (band_name == "2G")
		return band_info;

	if (he_phy_cap & 4) {
		if (index(modes, "HE40") < 0) push(modes, "HE40");
		if (eht_phy_cap && index(modes, "EHT40") < 0) push(modes, "EHT40");
	}
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
	return band_info;
}

export function eligible_blocks(blocks, htmode, config, automatic) {
	let mode = match(htmode ?? 'NOHT', /^(EHT|HE|VHT|HT)(20|40|80|160|320)([+-])?$/);
	let width = mode ? int(mode[2]) : 20;
	return filter(blocks?.[width] || [], block =>
		!(automatic && +config.acs_exclude_dfs && block.dfs) &&
		!(config.country3 in [ '0x4f', '0x4F', '79' ] && block.indoor_only) &&
		!(mode?.[1] in [ 'HE', 'EHT' ] && block.no_he) &&
		!(mode?.[1] == 'EHT' && block.no_eht));
}

export function automatic_channels(blocks) {
	let channels = [];
	for (let block in blocks)
		push(channels, ...block.channels);
	return map(uniq(channels), channel => '' + channel);
}

export function maximum_channel_plan(modes, blocks, config) {
	for (let width in [ 320, 160, 80, 40, 20 ])
		for (let family in [ 'EHT', 'HE', 'VHT', 'HT', 'NOHT' ]) {
			if (family == 'NOHT' && width != 20) continue;
			let htmode = family == 'NOHT' ? family : family + width;
			if (!(htmode in modes)) continue;
			let channels = automatic_channels(eligible_blocks(blocks, htmode, config, true));
			if (length(channels)) return { htmode, channels };
		}
	return null;
}

export function candidate_contains(list, channel) {
	if (!length(list)) return true;
	for (let item in list)
		for (let entry in split(item, /[ ,\t]+/)) {
			let range = match(entry, /^([0-9]+)(-([0-9]+))?$/);
			if (range && channel >= +range[1] && channel <= +(range[3] ?? range[1]))
				return true;
		}
	return false;
}

export function ap_channel_plan(info, band, config) {
	if (!band?.freqs || !length(band.freqs)) return { error: 'NO_AP_BAND' };
	let band_name = uc(config.band);
	let caps = band_capabilities(band, band_name, true);
	let radio = config.radio == null ? null : filter(info.radios || [], r => r.index == +config.radio)[0];
	if (config.radio != null && length(info.radios) && !radio) return { error: 'NO_AP_RADIO' };
	let ranges = radio ? map(radio.freq_ranges || [], range => [ range.start, range.end ]) : null;
	let blocks = usable_channel_blocks(band_name, band.freqs, caps.max_width, ranges);
	let automatic = !+config.channel || config.channel == 'auto';
	let manual_list = config.chanlist || [];
	let requested = config.htmode ?? 'NOHT';
	if (automatic && !length(manual_list) && config.auto_htmode == requested)
		return maximum_channel_plan(caps.modes, blocks, config) ?? { error: 'NO_USABLE_CHANNEL_BLOCK' };

	let mode = replace(requested, /[+-]$/, '');
	if (!(mode in caps.modes)) return { error: 'AP_BANDWIDTH_UNSUPPORTED' };
	let eligible = eligible_blocks(blocks, requested, config, automatic);
	if (automatic) {
		let channels = filter(automatic_channels(eligible), ch => candidate_contains(manual_list, +ch));
		return length(channels) ? { htmode: requested, channels } : { error: 'NO_USABLE_CHANNEL_BLOCK' };
	}

	let primary = +config.channel;
	let block = filter(eligible, block => {
		if (band_name == '2G' && match(requested, /40[+-]?$/)) {
			let lower = requested == 'HT40+' || (requested != 'HT40-' && primary < 7);
			return primary == block.members[lower ? 0 : 1];
		}
		return primary in block.channels;
	})[0];
	return block ? { htmode: requested, center_channel: block.center_channel } : { error: 'CHANNEL_BANDWIDTH_MISMATCH' };
}
