// Shared preset data is also used by the LuCI post-parse save callback.
import { read_snapshot, record_option, custom_option, tracked_option, lan_context, equal, finish_snapshot } from './snapshot.uc';

function sections(uci, config, type) {
	let result = [];
	uci.foreach(config, type, section => push(result, section));
	return result;
}

function array(value) {
	return type(value) == 'array' ? value : value == null ? [] : split(value, /\s+/);
}

function wan6_device(uci) {
	let proto = uci.get('network', 'wan', 'proto');
	let device = uci.get('network', 'wan', 'device');
	// Ethernet IPv6 must be available before the IPv4 DHCP lease arrives.
	// PPP and other protocols still need their negotiated L3 device.
	return index([ 'dhcp', 'static' ], proto) >= 0 &&
		type(device) == 'string' && length(device) && !match(device, /\s/) ? device : '@wan';
}

function subnet(value, netmask) {
	let parts = split('' + value, '/');
	if (length(parts) > 2) return null;
	let octets = split(parts[0], '.');
	if (length(octets) != 4 || length(filter(octets, n => !match(n, /^\d+$/) || int(n) > 255))) return null;
	if (length(parts) > 1 && !match(parts[1], /^\d+$/)) return null;
	let prefix = length(parts) > 1 ? int(parts[1]) : 32;
	if (length(parts) < 2 && netmask) {
		let masks = split(netmask, '.');
		if (length(masks) != 4) return null;
		let mask = 0;
		for (let part in masks) {
			if (!match(part, /^\d+$/) || int(part) > 255) return null;
			mask = (mask << 8) | int(part);
		}
		prefix = 0;
		while (prefix < 32 && (mask & (1 << (31 - prefix)))) prefix++;
		if ((mask & 0xffffffff) != (prefix == 0 ? 0 : (0xffffffff << (32 - prefix)) & 0xffffffff)) return null;
	}
	if (prefix == null || prefix < 0 || prefix > 32) return null;
	let address = 0;
	for (let part in octets) address = (address << 8) | int(part);
	return { address: address & 0xffffffff, prefix };
}

function overlaps(a, b) {
	let prefix = min(a.prefix, b.prefix);
	let mask = prefix == 0 ? 0 : (0xffffffff << (32 - prefix)) & 0xffffffff;
	return (a.address & mask) == (b.address & mask);
}

export function synchronize(uci, data, upstream4, target) {
	let changed = {};
	if (uci.get_all('router-tool', 'main')?.['.type'] != 'settings')
		die('Invalid router-tool.main settings section');
	let recorder = null;
	let previous_state = null;
	function ensure(config, type_name, sid) {
		let section = uci.get_all(config, sid);
		if (section && section['.type'] != type_name) die(`Mode switch refused: section type changed at ${config}.${sid}`);
		if (!section) {
			// Preserve an explicit deletion recorded by the ownership snapshot.
			// Recreating an empty managed section loses both user intent and the
			// preset values needed by the new role.
			let owned = length(filter(previous_state?.options || [], o => o.config == config && o.sid == sid)) ||
				length(filter(previous_state?.created || [], s => s.config == config && s.sid == sid));
			if (recorder?.mode != 'ap' && owned) return null;
			if (!uci.set(config, sid, type_name)) die(`Cannot create ${config}.${sid}`);
			changed[config] = true;
			if (recorder) push(recorder.created, { config, sid });
		}
		return sid;
	}
	function find_unique(config, type_name, predicate) {
		let found = filter(sections(uci, config, type_name), predicate);
		if (length(found) > 1) die(`Mode switch refused: ambiguous matching sections in ${config}.${type_name}`);
		return found[0]?.['.name'];
	}
	function reserve(config, type_name, sid) {
		if (uci.get_all(config, sid)) die(`Mode switch refused: reserved section already exists at ${config}.${sid}`);
		return ensure(config, type_name, sid);
	}
	function set(config, sid, values) {
		if (sid == null) return;
		for (let key, value in values) {
			if (recorder) {
				let custom = recorder.mode != 'ap' && custom_option(uci, previous_state, config, sid, key);
				let option = record_option(uci, recorder, config, sid, key);
				if (custom) {
					if (option) option.custom = true;
					continue;
				}
				if (option && recorder.mode != 'ap') option.expected = value;
				else if (!option && recorder.mode != 'ap')
					for (let section in recorder.created)
						if (section.config == config && section.sid == sid && section.values) section.values[key] = value;
			}
			let previous = uci.get(config, sid, key);
			if (value == null && previous == null) continue;
			if (sprintf('%J', value) == sprintf('%J', previous)) continue;
			let ok = value == null ? uci.delete(config, sid, key) : uci.set(config, sid, key, value);
			if (!ok) die(`Cannot set ${config}.${sid}.${key}`);
			changed[config] = true;
		}
	}
	let sid = sections(uci, 'network', 'globals')[0]?.['.name'] || ensure('network', 'globals', 'globals');
	let ap = uci.get('router-tool', 'main', 'ap_mode') == '1';
	let role = uci.get('network', 'wan', 'proto') == 'pppoe' ? 'main' : 'sub';
	let previous = uci.get('router-tool', 'main', 'auto_role');
	let saved = uci.get('router-tool', 'main', 'state');
	let state = previous || saved ? read_snapshot(saved, previous) : null;
	let context = state?.lan;
	if (context && !equal(context, lan_context(uci))) state.manual_lan = true;
	previous_state = state;
	// Apply platform defaults once; preserve explicit user choices on reload
	// and role changes, including disabled RPS/RFS and all-CPU steering.
	let defaults = data.platform_defaults?.[target] || data.platform_defaults?.[split(target || '', '/')[0]];
	if (defaults?.network && uci.get('router-tool', 'main', 'steering_defaults_done') != '1') {
		for (let option, value in defaults.network)
			if (uci.get('network', sid, option) == null)
				set('network', sid, { [option]: value });
		set('router-tool', 'main', { steering_defaults_done: '1' });
	}
	if (defaults?.firewall && uci.get('router-tool', 'main', 'offload_defaults_done') != '1') {
		let firewall = sections(uci, 'firewall', 'defaults')[0]?.['.name'] || ensure('firewall', 'defaults', 'defaults');
		// Initialize the preset even when firewall4 ships its own defaults.
		// The marker preserves later user choices, including LuCI "None",
		// which removes both options.
		set('firewall', firewall, defaults.firewall);
		set('router-tool', 'main', { offload_defaults_done: '1' });
	}
	if (ap) return keys(changed);
	if (previous != role) {
		let context = previous_state?.lan;
		let manual_lan = !!previous_state?.manual_lan || context && !equal(context, lan_context(uci));
		// Carry missing managed sections forward as deletion markers across
		// role changes, instead of recreating an explicitly removed section.
		let deleted = map(filter(previous_state?.options || [], o => !uci.get_all(o.config, o.sid)), option => ({ ...option }));
		state = { version: 1, mode: role, options: deleted, created: [], manual_lan: !!manual_lan,
			subnet_done: role != 'sub' || !!manual_lan, lan: lan_context(uci) };
		recorder = state;
		for (let group in [ data.common, data.roles[role] ])
			for (let config in [ 'network', 'dhcp' ])
				for (let name, options in group[config] || {}) {
					let section = ensure(config, config == 'network' ? 'interface' : 'dhcp', name);
					set(config, section, config == 'network' && name == 'wan6' ?
						{ ...options, device: wan6_device(uci) } : options);
				}
		let zones = {};
		for (let name in [ 'lan', 'wan' ]) {
			let existing = find_unique('firewall', 'zone', section => section.name == name);
			let deleted = length(filter(previous_state?.options || [], o => o.config == 'firewall' && o.key == 'name' && o.expected == name && !uci.get_all('firewall', o.sid)));
			zones[name] = existing || (deleted ? null : reserve('firewall', 'zone', 'router_tool_' + name));
			set('firewall', zones[name], { name, ...data.firewall.zones[name] });
		}
		for (let section in sections(uci, 'firewall', 'zone')) {
			let networks = filter(array(uci.get('firewall', section['.name'], 'network')), name => index([ 'lan', 'wan', 'wan6' ], name) < 0);
			if (section['.name'] == zones.lan) push(networks, 'lan');
			if (section['.name'] == zones.wan) push(networks, 'wan', 'wan6');
			set('firewall', section['.name'], { network: length(networks) ? networks : null });
		}
		for (let section in sections(uci, 'firewall', 'defaults')) set('firewall', section['.name'], { disable_ipv6: null });
		let forward_existing = find_unique('firewall', 'forwarding', section => section.src == 'lan' && section.dest == 'wan');
		let forward_deleted = length(filter(previous_state?.options || [], o => o.config == 'firewall' && o.key == 'src' && o.expected == 'lan' && !uci.get_all('firewall', o.sid) && length(filter(previous_state.options, p => p.config == 'firewall' && p.sid == o.sid && p.key == 'dest' && p.expected == 'wan'))));
		let forward = forward_existing || (forward_deleted ? null : reserve('firewall', 'forwarding', 'router_tool_lan_wan'));
		set('firewall', forward, { src: 'lan', dest: 'wan', enabled: '1', family: null });
		for (let name, values in data.firewall.rules) {
			let existing = find_unique('firewall', 'rule', section => section.name == name);
			let enabled = name != 'Router-Tool-DHCPv6-Relay' || role == 'sub';
			if (!enabled && !existing) continue;
			let deleted = length(filter(previous_state?.options || [], o => o.config == 'firewall' && o.key == 'name' && o.expected == name && !uci.get_all('firewall', o.sid)));
			let rule = existing || (deleted ? null : reserve('firewall', 'rule', 'router_tool_' + replace(lc(name), /-/g, '_')));
			set('firewall', rule, enabled ? { name, enabled: '1', src: 'wan', target: 'ACCEPT', src_ip: null, dest_ip: null, ...values } : { enabled: '0' });
		}
		recorder = null;
		set('router-tool', 'main', { auto_role: role });
	}
	// Await the first lease, then check the LAN subnet once for this mode.
	// Later lease events must not overwrite a user's addressing decisions.
	if (role == 'sub' && !state.subnet_done && length(upstream4 || [])) {
		let context = state.lan;
		if (context && !equal(context, lan_context(uci))) state.manual_lan = true;
		recorder = state;
		let upstream = filter(map(upstream4 || [], address => subnet(address)), address => address != null);
		let lan = filter(map(array(uci.get('network', 'lan', 'ipaddr')), address => subnet(address, uci.get('network', 'lan', 'netmask'))), address => address != null);
		if (!state.manual_lan && uci.get('network', 'lan', 'proto') == 'static' && length(filter(lan, a => length(filter(upstream, b => overlaps(a, b)))))) {
			let occupied = [ ...upstream ];
			for (let section in sections(uci, 'network', 'interface'))
				if (section['.name'] != 'lan')
					for (let address in array(section.ipaddr)) {
						let parsed = subnet(address, section.netmask);
						if (parsed) push(occupied, parsed);
					}
			let address = null;
			for (let i = 2; i < 255; i++) {
				let candidate = `192.168.${i}.1/24`;
				if (!length(filter(occupied, b => overlaps(subnet(candidate), b)))) { address = candidate; break; }
			}
			if (!address) die('No non-overlapping IPv4 LAN subnet found');
			set('network', 'lan', { ipaddr: address, netmask: null });
		}
		recorder = null;
		state.subnet_done = true;
		set('router-tool', 'main', { state: finish_snapshot(uci, state) });
	}
	else if (previous != role) set('router-tool', 'main', { state: finish_snapshot(uci, state) });
	else {
		let protocol = tracked_option(state, 'network', 'wan', 'proto');
		let last_proto = state.proto;
		let device = tracked_option(state, 'network', 'wan6', 'device');
		if (last_proto != uci.get('network', 'wan', 'proto') ||
			device && !device.custom && device.expected != wan6_device(uci) && !custom_option(uci, state, 'network', 'wan6', 'device')) {
			recorder = state;
			set('network', 'wan6', { device: wan6_device(uci) });
			recorder = null;
			if (protocol) protocol.expected = uci.get('network', 'wan', 'proto');
			set('router-tool', 'main', { state: finish_snapshot(uci, state) });
		}
	}
	return keys(changed);
}
