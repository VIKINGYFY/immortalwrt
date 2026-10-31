// Project defaults for untouched APs. The caller owns the UCI transaction.
function array(value) {
	if (type(value) == 'array') return value;
	if (type(value) == 'object') {
		let result = [];
		for (let key, item in value) push(result, item);
		return result;
	}
	return value == null ? [] : [ value ];
}

function capabilities(board, radio, band) {
	for (let name, phy in board?.wlan || {}) {
		if (!(radio.phy && name == radio.phy) &&
			!(radio.path && index(array(phy.path), radio.path) >= 0)) continue;
		let info = phy.info;
		let caps = info?.bands?.[band];
		if (type(caps) != 'object') continue;
		if (radio.radio != null && '' + radio.radio != '' && type(info.radios) == 'array') {
			let match = filter(info.radios, entry => '' + entry.index == '' + radio.radio)[0];
			if (type(match?.bands?.[band]) != 'object') return null;
		}
		let modes = array(caps.modes);
		if (!length(modes)) return null;
		let width = match('' + caps.max_width, /^\d+$/) ? int(caps.max_width) : 20;
		return { modes, width };
	}
	return null;
}

function htmode(caps, requested) {
	for (let width in [ 160, 80, 40, 20 ]) {
		if (width > requested || width > caps.width) continue;
		for (let mode in [ 'EHT', 'HE', 'VHT', 'HT' ])
			if (index(caps.modes, mode + width) >= 0) return mode + width;
	}
	return null;
}

function default_ssid(uci, board, suffix, custom_ssid) {
	let hostname = custom_ssid || uci.get_first('system', 'system', 'hostname') || 'OWRT';
	let tail = suffix;
	if (!custom_ssid) {
		let vendor = split(board?.model?.id, ',')?.[0] || 'unknown';
		vendor = uc(substr(vendor, 0, 1)) + substr(vendor, 1);
		// Keep the vendor and band visible within the 32-byte SSID limit.
		tail = substr(vendor, 0, 32 - length(suffix) - 3) + '-' + suffix;
	}
	let size = min(length(hostname), 31 - length(tail));
	// Ucode string offsets count bytes. Avoid cutting a UTF-8 code point.
	while (size > 0 && (ord(hostname, size) & 0xc0) == 0x80) size--;
	return substr(hostname, 0, size) + '-' + tail;
}

export function synchronize_wireless(uci, board) {
	if (type(board?.wlan) != 'object') return false;
	let defaults_loaded = false, custom_ssid, custom_key;
	let radios = [], interfaces = [], changed = {};
	function load_defaults() {
		if (defaults_loaded) return;
		defaults_loaded = true;
		let defaults = uci.get_all('router-tool', 'defaults');
		if (defaults && defaults['.type'] != 'wireless') die('Invalid router-tool.defaults wireless section');
		custom_ssid = defaults?.ssid;
		custom_key = defaults?.key;
		let use_custom = type(custom_ssid) == 'string' && type(custom_key) == 'string' &&
			length(custom_ssid) && length(custom_key);
		if (use_custom && (length(custom_ssid) > 32 || match(custom_ssid, /[[:cntrl:]]/)))
			die('Wireless SSID must contain 1 to 32 bytes without control characters');
		if (use_custom && (length(custom_key) < 8 || length(custom_key) > 63 || match(custom_key, /[^ -~]/)))
			die('Wireless password must contain 8 to 63 printable ASCII characters');
		if (!use_custom) {
			custom_ssid = null;
			custom_key = '12345678';
		}
	}
	uci.foreach('wireless', 'wifi-device', section => push(radios, section));
	uci.foreach('wireless', 'wifi-iface', section => push(interfaces, section));
	function set(config, sid, values) {
		for (let option, value in values) {
			let old = uci.get(config, sid, option);
			if (sprintf('%J', old) == sprintf('%J', value)) continue;
			let ok = value == null ? uci.delete(config, sid, option) : uci.set(config, sid, option, value);
			if (!ok) die(`Cannot set ${config}.${sid}.${option}`);
			changed[config] = true;
		}
	}
	function mark_done(sid) {
		let saved = uci.get_all('router-tool', sid);
		if (saved && saved['.type'] != 'radio') die(`Cannot store wireless defaults marker at router-tool.${sid}`);
		if (!saved && !uci.set('router-tool', sid, 'radio')) die(`Cannot create router-tool.${sid}`);
		if (!saved) changed['router-tool'] = true;
		set('router-tool', sid, { defaults_done: '1' });
	}
	for (let radio in radios) {
		if (uci.get('router-tool', radio['.name'], 'defaults_done') == '1') continue;
		let attached = filter(interfaces, iface => index(array(iface.device), radio['.name']) >= 0);
		// Decide once as soon as a radio has interfaces. Custom SSIDs and
		// station/mesh/WDS backhauls are explicit user configuration.
		if (!length(attached)) continue;
		if (length(filter(attached, iface => iface.mode != 'ap' || iface.wds == '1' || length(array(iface.device)) != 1 || iface.mlo == '1'))) {
			mark_done(radio['.name']);
			continue;
		}
		let aps = attached;
		if (length(filter(aps, iface => iface.ssid != 'OWRT' || iface.key != '12345678' || iface.encryption != 'psk2+ccmp'))) {
			mark_done(radio['.name']);
			continue;
		}
		let band = { '2g': '2G', '5g': '5G', '6g': '6G' }[radio.band];
		if (!band) { mark_done(radio['.name']); continue; }
		let caps = capabilities(board, radio, band);
		// PHY capabilities may arrive after the first boot event. Retry when
		// they are available instead of permanently skipping the defaults.
		if (!caps) continue;
		let suffix, channel, width;
		if (band == '2G') { suffix = '2.4G'; channel = '1'; width = 20; }
		else if (band == '6G') { suffix = '6G'; channel = '33'; width = 160; }
		else if (caps.width == 80) { suffix = '5.8G'; channel = '149'; width = 80; }
		else { suffix = '5.2G'; channel = '36'; width = 160; }
		let mode = htmode(caps, width);
		if (!mode) { mark_done(radio['.name']); continue; }
		load_defaults();
		set('wireless', radio['.name'], { country: 'US', channel, htmode: mode });
		for (let ap in aps)
			set('wireless', ap['.name'], { ssid: default_ssid(uci, board, suffix, custom_ssid), key: custom_key, encryption: band == '6G' ? 'sae+ccmp' : 'psk2+ccmp', ieee80211w: band == '6G' ? '2' : null });
		mark_done(radio['.name']);
	}
	let result = keys(changed);
	return length(result) ? result : false;
}
