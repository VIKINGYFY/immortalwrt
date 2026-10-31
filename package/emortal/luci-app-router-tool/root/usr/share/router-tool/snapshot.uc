// Snapshots describe only options owned by a preset, plus complete new sections.
export function section_values(section) {
	let result = {};
	for (let key, value in section || {})
		if (substr(key, 0, 1) != '.') result[key] = value;
	return result;
}

export function equal(a, b) {
	if (type(a) != type(b)) return false;
	if (type(a) == 'array') {
		if (length(a) != length(b)) return false;
		for (let i = 0; i < length(a); i++) if (!equal(a[i], b[i])) return false;
		return true;
	}
	if (type(a) == 'object') {
		if (length(keys(a)) != length(keys(b))) return false;
		for (let key, value in a) if (!exists(b, key) || !equal(value, b[key])) return false;
		return true;
	}
	return a == b;
}

export function value_valid(value) {
	return value == null || type(value) == 'string' ||
		type(value) == 'array' && !length(filter(value, v => type(v) != 'string'));
}

export function option_equal(option, a, b) {
	// The firewall UI omits unchecked masquerading flags; fw4 defaults to off.
	let optional = option.config == 'firewall' && option.type == 'zone' && index([ 'masq', 'masq6' ], option.key) >= 0;
	return equal(optional ? a ?? '0' : a, optional ? b ?? '0' : b);
}

export function read_snapshot(text, mode) {
	let state;
	try { state = json(text || 'null'); } catch (e) {}
	let valid = s => type(s) == 'object' && index([ 'network', 'dhcp', 'firewall', 'wireless' ], s.config) >= 0 &&
		type(s.sid) == 'string' && length(s.sid) && type(s.type) == 'string' && length(s.type);
	if (state?.version != 1 || index([ 'main', 'sub', 'ap' ], mode) < 0 || state.mode != mode || type(state.subnet_done) != 'bool' ||
		(exists(state, 'manual_lan') && type(state.manual_lan) != 'bool') ||
		(exists(state, 'dhcpv4_defaults') && type(state.dhcpv4_defaults) != 'bool') ||
		(exists(state, 'proto') && state.proto != null && type(state.proto) != 'string') ||
		(exists(state, 'lan') && (type(state.lan) != 'object' ||
			length(filter(keys(state.lan), k => index([ 'proto', 'ipaddr', 'netmask' ], k) < 0)) ||
			length(filter(values(state.lan), v => !value_valid(v))))) ||
		type(state.options) != 'array' || type(state.created) != 'array' || !length(state.options) && !length(state.created))
		die('Mode switch refused: saved configuration snapshot is missing or invalid');
	let seen = {};
	for (let s in state.created) {
		let id = `${s.config}/${s.sid}`;
		if (!valid(s) || type(s.values) != 'object' || seen[id] ||
			length(filter(values(s.values), v => !value_valid(v))))
			die('Mode switch refused: saved configuration snapshot is missing or invalid');
		seen[id] = true;
	}
	for (let o in state.options) {
		let id = `${o.config}/${o.sid}/${o.key}`;
		if (!valid(o) || type(o.key) != 'string' || !length(o.key) || substr(o.key, 0, 1) == '.' ||
			(mode == 'ap' && (!exists(o, 'value') || !value_valid(o.value))) || !exists(o, 'expected') || !value_valid(o.expected) ||
			(exists(o, 'custom') && type(o.custom) != 'bool') ||
			seen[id] || seen[`${o.config}/${o.sid}`])
			die('Mode switch refused: saved configuration snapshot is missing or invalid');
		seen[id] = true;
	}
	return state;
}

// A persistent preset is an ownership record, not a lock on user settings.
// Retyped sections invalidate identity; intentionally deleted sections stay deleted.
export function check_snapshot(uci, state) {
	let conflicts = [];
	let configs = {};
	function section(config, sid) {
		if (!exists(configs, config)) configs[config] = uci.get_all(config) || {};
		return configs[config][sid];
	}
	for (let o in state.options) {
		let saved = section(o.config, o.sid);
		if (saved && saved['.type'] != o.type)
			push(conflicts, `${o.config}.${o.sid}`);
	}
	for (let s in state.created) {
		let saved = section(s.config, s.sid);
		if (saved && saved['.type'] != s.type)
			push(conflicts, `${s.config}.${s.sid}`);
	}
	if (length(conflicts)) die('Mode switch refused: user changed ' + join(', ', conflicts));
}

export function record_option(uci, state, config, sid, key) {
	if (length(filter(state.created, s => s.config == config && s.sid == sid))) return null;
	for (let option in state.options)
		if (option.config == config && option.sid == sid && option.key == key) return option;
	let option = { config, sid, key, type: uci.get_all(config, sid)?.['.type'] };
	if (state.mode == 'ap') option.value = uci.get(config, sid, key);
	push(state.options, option);
	return option;
}

export function tracked_option(state, config, sid, key) {
	for (let option in state?.options || [])
		if (option.config == config && option.sid == sid && option.key == key) return option;
	for (let section in state?.created || [])
		if (section.config == config && section.sid == sid && exists(section.values, key))
			return { config, sid, key, type: section.type, expected: section.values[key] };
	return null;
}

export function custom_option(uci, state, config, sid, key) {
	let previous = tracked_option(state, config, sid, key);
	return previous && (previous.custom || !option_equal(previous, uci.get(config, sid, key), previous.expected));
}

export function lan_context(uci) {
	let result = {};
	for (let key in [ 'proto', 'ipaddr', 'netmask' ]) result[key] = uci.get('network', 'lan', key);
	return result;
}

export function saved_lan(state) {
	if (type(state?.lan) == 'object') return state.lan;
	let result = {};
	for (let key in [ 'proto', 'ipaddr', 'netmask' ]) {
		let option = tracked_option(state, 'network', 'lan', key);
		if (!option) return null;
		result[key] = option.expected;
	}
	return result;
}

export function finish_snapshot(uci, state) {
	for (let o in state.options) {
		let value = uci.get(o.config, o.sid, o.key);
		if (state.mode != 'ap' && exists(o, 'expected') && !option_equal(o, value, o.expected)) o.custom = true;
		o.expected = value;
	}
	for (let s in state.created) {
		let section = uci.get_all(s.config, s.sid);
		if (!section) continue;
		if (state.mode != 'ap') {
			// Routed presets need per-option ownership after section creation.
			// Flatten legacy section records without losing later overrides.
			let current = section_values(section);
			for (let key in uniq([ ...keys(s.values || {}), ...keys(current) ])) {
				let option = { config: s.config, sid: s.sid, type: section['.type'], key, expected: current[key] };
				if (s.values && !option_equal(option, s.values[key], current[key])) option.custom = true;
				push(state.options, option);
			}
			continue;
		}
		s.type = section['.type'];
		s.values = section_values(section);
	}
	if (state.mode != 'ap') {
		state.created = [];
		state.lan = lan_context(uci);
		state.proto = uci.get('network', 'wan', 'proto');
	}
	return sprintf('%J', state);
}
