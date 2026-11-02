'use strict';
'require baseclass';
'require form';
'require fs';
'require rpc';
'require ui';
'require uci';

let recorder = null;
let previousSnapshot = null;

const callBoardJSON = rpc.declare({
	object: 'luci-rpc',
	method: 'getBoardJSON',
	expect: { '': {} }
});

const callInterfaces = rpc.declare({
	object: 'network.interface',
	method: 'dump',
	expect: { 'interface': [] }
});

const callLiveConfig = rpc.declare({
	object: 'uci',
	method: 'get',
	params: [ 'config' ],
	expect: { values: {} },
	reject: true
});

const copy = value => value == null ? null : JSON.parse(JSON.stringify(value));
function equal(a, b) {
	if (a == null || b == null) return a == null && b == null;
	if (typeof a != typeof b || Array.isArray(a) != Array.isArray(b)) return false;
	if (typeof a != 'object') return a === b;
	const keys = Object.keys(a);
	return keys.length == Object.keys(b).length && keys.every(key => Object.hasOwn(b, key) && equal(a[key], b[key]));
}

function optionEqual(option, a, b) {
	// Firewall's unchecked masquerading flags are omitted by its LuCI view.
	const optional = option.config == 'firewall' && option.type == 'zone' && [ 'masq', 'masq6' ].includes(option.key);
	return equal(optional ? a ?? '0' : a, optional ? b ?? '0' : b);
}

function sectionValues(config, sid) {
	const section = uci.sections(config).find(s => s['.name'] == sid);
	return section && Object.fromEntries(Object.keys(section).filter(k => k[0] != '.').map(k => [ k, copy(uci.get(config, sid, k)) ]).filter(v => v[1] != null));
}

function refuse(message) {
	console.warn('[router-tool] ' + message);
	const error = new Error(message);
	error.routerToolConflict = true;
	throw error;
}

function recordOption(state, config, sid, key) {
	if (state.created.some(s => s.config == config && s.sid == sid)) return null;
	let option = state.options.find(o => o.config == config && o.sid == sid && o.key == key);
	if (option) return option;
	option = { config, sid, key, type: uci.get(config, sid)?.['.type'] };
	if (state.mode == 'ap') option.value = copy(uci.get(config, sid, key));
	state.options.push(option);
	return option;
}

function trackedOption(state, config, sid, key) {
	const option = state?.options.find(o => o.config == config && o.sid == sid && o.key == key);
	if (option) return option;
	const section = state?.created.find(s => s.config == config && s.sid == sid && Object.hasOwn(s.values, key));
	return section && { config, sid, key, type: section.type, expected: section.values[key] };
}

function customOption(state, config, sid, key) {
	const option = trackedOption(state, config, sid, key);
	return option && (option.custom || !optionEqual(option, uci.get(config, sid, key), option.expected));
}

function lanContext() {
	return Object.fromEntries([ 'proto', 'ipaddr', 'netmask' ].map(key => [ key, copy(uci.get('network', 'lan', key)) ]));
}

function finishSnapshot(state) {
	for (const option of state.options) {
		const value = copy(uci.get(option.config, option.sid, option.key));
		if (state.mode != 'ap' && Object.hasOwn(option, 'expected') && !optionEqual(option, value, option.expected)) option.custom = true;
		option.expected = value;
	}
	for (const section of state.created) {
		const current = sectionValues(section.config, section.sid);
		if (!current) continue;
		if (state.mode != 'ap') {
			for (const key of Object.keys(current)) {
				const option = { config: section.config, sid: section.sid, type: uci.get(section.config, section.sid)['.type'], key, expected: copy(current[key]) };
				state.options.push(option);
			}
			continue;
		}
		section.type = uci.get(section.config, section.sid)?.['.type'];
		section.values = current;
	}
	if (state.mode != 'ap') {
		state.created = [];
		state.lan = lanContext();
		state.proto = uci.get('network', 'wan', 'proto');
	}
	return JSON.stringify(state);
}

function setOptions(config, sid, values) {
	if (sid == null) return;
	for (const [ key, value ] of Object.entries(values)) {
		if (recorder) {
			const custom = recorder.mode != 'ap' && customOption(previousSnapshot, config, sid, key);
			const option = recordOption(recorder, config, sid, key);
			if (custom) {
				if (option) option.custom = true;
				continue;
			}
			if (option && recorder.mode != 'ap') option.expected = copy(value);
			else if (!option && recorder.mode != 'ap')
				for (const section of recorder.created)
					if (section.config == config && section.sid == sid && section.values) section.values[key] = copy(value);
		}
		if (equal(uci.get(config, sid, key), value)) continue;
		if (value == null) uci.unset(config, sid, key);
		else uci.set(config, sid, key, value);
	}
}

function ensure(config, type, sid) {
	const section = uci.get(config, sid);
	if (section && section['.type'] != type) refuse(_('Mode switch refused: section type changed at') + ` ${config}.${sid}`);
	if (section == null) {
		// A user deletion is an explicit ownership decision. Do not replace a
		// previously managed section with an empty shell on the next role switch.
		if (recorder?.mode != 'ap' && previousSnapshot?.options?.some(o => o.config == config && o.sid == sid) ||
			recorder?.mode != 'ap' && previousSnapshot?.created?.some(s => s.config == config && s.sid == sid)) return null;
		uci.add(config, type, sid);
		if (recorder) recorder.created.push({ config, sid });
	}
	return sid;
}

function findUnique(config, type, predicate) {
	const found = uci.sections(config, type).filter(predicate);
	if (found.length > 1) refuse(_('Mode switch refused: ambiguous matching configuration sections:') + ` ${config}.${type}`);
	return found[0]?.['.name'];
}

function reserve(config, type, sid) {
	if (uci.get(config, sid)) refuse(_('Mode switch refused: reserved section already exists at') + ` ${config}.${sid}`);
	return ensure(config, type, sid);
}

function globals() {
	return uci.sections('network', 'globals')[0]?.['.name'];
}

function wan6Device() {
	const proto = uci.get('network', 'wan', 'proto');
	const device = uci.get('network', 'wan', 'device');
	// Ethernet IPv6 must be available before the IPv4 DHCP lease arrives.
	// PPP and other protocols still need their negotiated L3 device.
	return [ 'dhcp', 'static' ].includes(proto) &&
		typeof device == 'string' && device.length && !/\s/.test(device) ? device : '@wan';
}

function readSnapshot(sid, option, mode) {
	try {
		const state = JSON.parse(uci.get('router-tool', 'main', option) || 'null');
		const validValue = v => v == null || typeof v == 'string' || Array.isArray(v) && v.every(x => typeof x == 'string');
		const validSection = s => s && [ 'network', 'dhcp', 'firewall', 'wireless' ].includes(s.config) && typeof s.sid == 'string' && s.sid.length && typeof s.type == 'string' && s.type.length;
		const seen = new Set();
		if (state?.version != 1 || ![ 'main', 'sub', 'ap' ].includes(mode) || state.mode != mode || typeof state.subnet_done != 'boolean' ||
			(Object.hasOwn(state, 'manual_lan') && typeof state.manual_lan != 'boolean') ||
			(Object.hasOwn(state, 'dhcpv4_defaults') && typeof state.dhcpv4_defaults != 'boolean') ||
			(Object.hasOwn(state, 'proto') && state.proto != null && typeof state.proto != 'string') ||
			(Object.hasOwn(state, 'lan') && (!state.lan || Array.isArray(state.lan) || typeof state.lan != 'object' ||
				Object.keys(state.lan).some(key => ![ 'proto', 'ipaddr', 'netmask' ].includes(key)) || !Object.values(state.lan).every(validValue))) ||
			!Array.isArray(state.options) || !Array.isArray(state.created) || !state.options.length && !state.created.length) return null;
		if (Object.hasOwn(state, 'dhcpv4_defaults') || mode != 'ap' &&
			(!state.lan || typeof state.lan != 'object' || !Object.hasOwn(state, 'proto') || typeof state.manual_lan != 'boolean' || state.created.length)) return null;
		for (const s of state.created) {
			const id = `${s.config}/${s.sid}`;
			if (!validSection(s) || !s.values || Array.isArray(s.values) || typeof s.values != 'object' || !Object.values(s.values).every(validValue) || seen.has(id)) return null;
			seen.add(id);
		}
		for (const o of state.options) {
			const id = `${o.config}/${o.sid}/${o.key}`;
			if (!validSection(o) || typeof o.key != 'string' || !o.key.length || o.key[0] == '.' || (mode == 'ap' && (!Object.hasOwn(o, 'value') || !validValue(o.value))) || !Object.hasOwn(o, 'expected') ||
				(Object.hasOwn(o, 'custom') && typeof o.custom != 'boolean') ||
				!validValue(o.expected) || seen.has(id) || seen.has(`${o.config}/${o.sid}`)) return null;
			seen.add(id);
		}
		return state;
	}
	catch (e) { return null; }
}

function checkSnapshot(state, live) {
	if (!state) refuse(_('Mode switch refused: saved configuration snapshot is missing or invalid.'));
	const get = (config, sid, key) => {
		if (key == null) return (live ? live[config]?.[sid] : uci.get(config, sid))?.['.type'];
		return live ? live[config]?.[sid]?.[key] : uci.get(config, sid, key);
	};
	const conflicts = [];
	for (const o of state.options) {
		if (get(o.config, o.sid) != null && get(o.config, o.sid) != o.type)
			conflicts.push(`${o.config}.${o.sid}`);
	}
	for (const s of state.created) {
		if (get(s.config, s.sid) != null && get(s.config, s.sid) != s.type) conflicts.push(`${s.config}.${s.sid}`);
	}
	if (conflicts.length) refuse(_('Mode switch refused: user changed related options:') + ' ' + conflicts.join(', '));
}

function verifyLiveSnapshot(sid, selected, enabled, before) {
	const configs = [ 'network', 'dhcp', 'firewall', 'router-tool' ];
	// NO WIFI images have no wireless configuration. Only recheck it when
	// the form loaded it; errors for existing configurations must still fail.
	if (Object.hasOwn(before, 'wireless')) configs.push('wireless');
	return Promise.all(configs.map(config => callLiveConfig(config))).then(values => {
		const live = Object.fromEntries(configs.map((config, i) => [ config, values[i] ]));
		if (!enabled && live['router-tool']?.main?.ap_state)
			refuse(_('Mode switch refused: an AP snapshot exists while AP mode is disabled. Restore AP mode and use LuCI to switch back.'));
		for (const key of [ 'auto_role', 'ap_mode', 'state', 'ap_state' ])
			if (!equal(live['router-tool']?.main?.[key], before['router-tool']?.main?.[key]))
				refuse(_('Mode switch refused: configuration changed while this page was open. Reload the page.'));
		const state = readSnapshot(sid, enabled ? 'ap_state' : 'state', enabled ? 'ap' : uci.get('router-tool', 'main', 'auto_role'));
		if ((enabled || uci.get('router-tool', 'main', 'auto_role') || uci.get('router-tool', 'main', 'state')) && !state)
			checkSnapshot(state);
		if (enabled && !selected) checkSnapshot(state);
		// Compare this page's edits with its load-time values, not a preset
		// saved by an earlier operation. Rebase untouched options onto live UCI.
		const plain = section => section && Object.fromEntries(Object.entries(section).filter(([ key ]) => key[0] != '.'));
		function discard(config, name, key) {
			const changes = uci.state.changes[config]?.[name];
			if (changes) {
				delete changes[key];
				if (!Object.keys(changes).length) delete uci.state.changes[config][name];
			}
			const removed = uci.state.deletes[config]?.[name];
			// LuCI records option deletions as object keys; true removes the
			// whole section. Drop an option already removed by another editor.
			if (removed && removed !== true) {
				delete removed[key];
				if (!Object.keys(removed).length) delete uci.state.deletes[config][name];
			}
		}
		for (const config of configs) {
			const loaded = before[config] || {}, current = live[config] || {};
			for (const name of new Set(Object.keys(loaded).concat(uci.sections(config).map(s => s['.name'])))) {
				const old = loaded[name], next = uci.get(config, name), remote = current[name];
				const conflict = key => refuse(_('Mode switch refused: user changed related options:') + ` ${config}.${name}${key ? '.' + key : ''}`);
				if (!old) {
					if (remote && next) conflict();
					continue;
				}
				if (!next) {
					if (remote && (!equal(plain(old), plain(remote)) || old['.type'] != remote['.type'])) conflict();
					if (!remote && uci.state.deletes[config]) delete uci.state.deletes[config][name];
					continue;
				}
				if (!remote || old['.type'] != remote['.type']) {
					if (!equal(plain(old), plain(next)) || old['.type'] != next['.type']) conflict();
					if (uci.state.changes[config]) delete uci.state.changes[config][name];
					if (uci.state.deletes[config]) delete uci.state.deletes[config][name];
					continue;
				}
				for (const key of new Set(Object.keys(old).concat(Object.keys(next)).filter(k => k[0] != '.'))) {
					const option = { config, sid: name, type: old['.type'], key };
					if (optionEqual(option, next[key], old[key]) || optionEqual(option, next[key], remote[key])) discard(config, name, key);
					else if (!optionEqual(option, remote[key], old[key])) conflict(key);
				}
			}
			uci.state.values[config] = copy(current);
		}
	});
}

function retainedOptions(map, sid, enabled) {
	const state = readSnapshot(sid, enabled ? 'ap_state' : 'state', enabled ? 'ap' : uci.get('router-tool', 'main', 'auto_role'));
	const options = (state?.options || []).concat((state?.created || []).flatMap(s =>
		Object.entries(s.values).map(([ key, expected ]) => ({ config: s.config, sid: s.sid, key, expected }))));
	const retained = options.filter(o => {
		if (o.expected == null || !equal(o.expected, uci.get(o.config, o.sid, o.key))) return false;
		const field = map.lookupOption?.(o.key, o.sid, o.config)?.[0];
		if (!field || !field.isActive(o.sid)) return true;
		// LuCI represents disabled odhcpd services with an empty choice.
		const value = o.config == 'dhcp' && [ 'dhcpv4', 'ra', 'dhcpv6', 'ndp' ].includes(o.key) && o.expected == 'disabled' ? '' : o.expected;
		return (equal(value, field.default) || value === '') && equal(value, field.formvalue(o.sid));
	});
	// LuCI hides the old protocol's fields before parsing the switch. Keep
	// common interface settings which also apply to the new protocol.
	for (const name of [ 'lan', 'wan' ]) {
		const protocol = map.lookupOption?.('proto', name, 'network')?.[0];
		const selected = protocol?.formvalue(name);
		if (typeof selected != 'string' || selected == uci.get('network', name, 'proto')) continue;
		for (const key of [ 'defaultroute', 'peerdns', 'dns', 'dns_metric', 'metric', 'multipath',
			'ip4table', 'ip6table', 'sourcefilter', 'delegate', 'ip6assign', 'ip6hint', 'ip6class', 'ip6ifaceid', 'ip6weight' ]) {
			const field = map.lookupOption(key, name, 'network')?.[0];
			const value = uci.get('network', name, key);
			if (value != null && field && !field.isActive(name))
				retained.push({ config: 'network', sid: name, key, expected: copy(value) });
		}
	}
	return retained;
}

function boardPorts(board, name) {
	return L.toArray(board?.network?.[name]?.ports || board?.network?.[name]?.device);
}

function topology(context) {
	const device = uci.get('network', 'lan', 'device') || 'br-lan';
	const bridgeId = findUnique('network', 'device', s => s.name == device && s.type == 'bridge');
	const bridge = bridgeId ? uci.get('network', bridgeId) : null;
	const bridgeName = bridge ? device : 'br-lan';
	const wan = uci.get('network', 'wan', 'device') || boardPorts(context.board, 'wan')[0];
	const ports = bridge ? L.toArray(uci.get('network', bridge['.name'], 'ports')) :
		boardPorts(context.board, 'lan').concat(device != bridgeName ? [ device ] : []);
	const physical = Object.keys(context.board?.network || {}).flatMap(name => boardPorts(context.board, name));
	return { bridge, bridgeName, wan, ports: Array.from(new Set(ports.concat(physical, wan || []))) };
}

function validateAp(topo, context) {
	if (!Object.keys(context.board?.network || {}).length)
		throw new Error(_('Cannot read board port assignments. Configure AP ports manually.'));
	if (!topo.wan || topo.wan.startsWith('@'))
		throw new Error(_('Cannot identify the Ethernet WAN device. Configure LAN/WAN devices first.'));
	const devices = uci.sections('network', 'device');
	if (uci.sections('network', 'switch').length || uci.sections('network', 'switch_vlan').length ||
		uci.sections('network', 'bridge-vlan').some(s => s.device == topo.bridgeName) ||
		devices.some(s => s.name != topo.bridgeName && L.toArray(s.ports).some(port => topo.ports.includes(port))) ||
		devices.some(s => topo.ports.includes(s.name) && s.type && ![ '8023', 'ethernet' ].includes(s.type)) ||
		topo.ports.some(port => /[.@:]/.test(port) || port == topo.bridgeName || /^(br-|wlan|phy)/.test(port)))
		throw new Error(_('Automatic AP conversion requires plain Ethernet ports. Configure VLAN or switch topologies manually.'));
}

function zone(name) {
	const existing = findUnique('firewall', 'zone', s => s.name == name);
	if (existing) return existing;
	const deleted = previousSnapshot?.options?.some(o => o.config == 'firewall' && o.key == 'name' && o.expected == name && !uci.get('firewall', o.sid));
	return deleted ? null : reserve('firewall', 'zone', `router_tool_${name}`);
}

function configureFirewall(role, data) {
	const lan = zone('lan'), wan = zone('wan');
	for (const section of uci.sections('firewall', 'zone')) {
		const networks = L.toArray(uci.get('firewall', section['.name'], 'network')).filter(n => ![ 'lan', 'wan', 'wan6' ].includes(n));
		if (section['.name'] == lan) networks.push('lan', ...(role == 'relay_ap' ? [ 'wan6' ] : []));
		if (section['.name'] == wan && role != 'relay_ap') networks.push('wan', 'wan6');
		setOptions('firewall', section['.name'], { network: networks.length ? networks : null });
	}
	for (const [ name, sid ] of [ [ 'lan', lan ], [ 'wan', wan ] ])
		setOptions('firewall', sid, { name, ...data.zones[name], ...(role == 'relay_ap' ? { masq: '0' } : {}) });
	for (const section of uci.sections('firewall', 'defaults'))
		setOptions('firewall', section['.name'], { disable_ipv6: null });
	const forwardingExisting = findUnique('firewall', 'forwarding', s => s.src == 'lan' && s.dest == 'wan');
	const forwardingDeleted = previousSnapshot?.options?.some(o => o.config == 'firewall' &&
		[ 'src', 'dest' ].includes(o.key) && !uci.get('firewall', o.sid) &&
		previousSnapshot.options.some(p => p.config == 'firewall' && p.sid == o.sid && p.key == 'src' && p.expected == 'lan') &&
		previousSnapshot.options.some(p => p.config == 'firewall' && p.sid == o.sid && p.key == 'dest' && p.expected == 'wan'));
	const forwarding = forwardingExisting || (forwardingDeleted ? null : reserve('firewall', 'forwarding', 'router_tool_lan_wan'));
	setOptions('firewall', forwarding, { src: 'lan', dest: 'wan', enabled: role == 'relay_ap' ? '0' : '1', family: null });
	for (const [ name, values ] of Object.entries(data.rules)) {
		const existing = findUnique('firewall', 'rule', s => s.name == name);
		const enabled = role != 'relay_ap' && (name != 'Router-Tool-DHCPv6-Relay' || role == 'sub');
		if (!enabled && !existing) continue;
		const deleted = previousSnapshot?.options?.some(o => o.config == 'firewall' && o.key == 'name' && o.expected == name && !uci.get('firewall', o.sid));
		const sid = existing || (deleted ? null : reserve('firewall', 'rule', 'router_tool_' + name.toLowerCase().replaceAll('-', '_')));
		if (!enabled) setOptions('firewall', sid, { enabled: '0' });
		else setOptions('firewall', sid, { name, enabled: '1', src: 'wan', target: 'ACCEPT', src_ip: null, dest_ip: null, ...values });
	}
}

function subnet(value, netmask) {
	const parts = String(value).split('/');
	if (parts.length > 2) return null;
	const [ address, cidr ] = parts;
	const octets = address.split('.');
	if (octets.length != 4 || octets.some(n => !/^\d+$/.test(n) || Number(n) > 255)) return null;
	if (cidr != null && !/^\d+$/.test(cidr)) return null;
	let prefix = cidr != null ? Number(cidr) : 32;
	if (cidr == null && netmask) {
		const masks = String(netmask).split('.');
		if (masks.length != 4 || masks.some(n => !/^\d+$/.test(n) || Number(n) > 255)) return null;
		const bits = masks.map(n => Number(n).toString(2).padStart(8, '0')).join('');
		if (!/^1*0*$/.test(bits) || bits.length != 32) return null;
		prefix = bits.indexOf('0') == -1 ? 32 : bits.indexOf('0');
	}
	if (!Number.isInteger(prefix) || prefix < 0 || prefix > 32) return null;
	return { address: octets.reduce((n, v) => (n << 8) | Number(v), 0) >>> 0, prefix };
}

function avoidSubnetConflict(context) {
	if (uci.get('network', 'lan', 'proto') != 'static') return;
	const upstream = context.upstream4.map(address => subnet(address)).filter(Boolean);
	const lan = L.toArray(uci.get('network', 'lan', 'ipaddr')).map(address => subnet(address, uci.get('network', 'lan', 'netmask'))).filter(Boolean);
	const overlaps = (a, b) => {
		const prefix = Math.min(a.prefix, b.prefix);
		const mask = prefix == 0 ? 0 : (0xffffffff << (32 - prefix)) >>> 0;
		return (a.address & mask) == (b.address & mask);
	};
	if (!lan.some(a => upstream.some(b => overlaps(a, b)))) return;
	const occupied = upstream.slice();
	for (const section of uci.sections('network', 'interface'))
		if (section['.name'] != 'lan')
			occupied.push(...L.toArray(section.ipaddr).map(address => subnet(address, section.netmask)).filter(Boolean));
	for (let i = 2; i < 255; i++) {
		const address = `192.168.${i}.1/24`;
		if (occupied.every(b => !overlaps(subnet(address), b))) {
			console.info(`[router-tool] LAN subnet overlaps upstream; staging ${address}`);
			setOptions('network', 'lan', { ipaddr: address, netmask: null });
			return;
		}
	}
	throw new Error(_('No free LAN subnet was found. Configure a non-overlapping IPv4 subnet manually.'));
}

function synchronize(sid, context) {
	if (uci.get('router-tool', 'main', 'ap_mode') == '1') return;
	const role = uci.get('network', 'wan', 'proto') == 'pppoe' ? 'main' : 'sub';
	previousSnapshot = readSnapshot(sid, 'state', uci.get('router-tool', 'main', 'auto_role'));
	const beforeLan = previousSnapshot?.lan;
	if (beforeLan && !equal(beforeLan, lanContext())) previousSnapshot.manual_lan = true;
	if (uci.get('router-tool', 'main', 'auto_role') != role) {
		console.info(`[router-tool] Preparing ${role} router presets`);
		const manualLan = !!previousSnapshot?.manual_lan || !!beforeLan && !equal(beforeLan, lanContext());
		// Carry missing managed sections forward as deletion markers. Without
		// this, switching away and back would recreate an explicit deletion.
		const deleted = (previousSnapshot?.options || []).filter(o => !uci.get(o.config, o.sid)).map(copy);
		const state = { version: 1, mode: role, options: deleted, created: [], manual_lan: manualLan,
			subnet_done: role != 'sub' || manualLan, lan: lanContext() };
		recorder = state;
		try {
			for (const group of [ context.presets.common, context.presets.roles[role] ])
				for (const config of [ 'network', 'dhcp' ])
					for (const [ name, options ] of Object.entries(group[config] || {})) {
						const section = ensure(config, config == 'network' ? 'interface' : 'dhcp', name);
						setOptions(config, section, config == 'network' && name == 'wan6' ?
							{ ...options, device: wan6Device() } : options);
					}
			configureFirewall(role, context.presets.firewall);
			if (role == 'sub' && !state.subnet_done && context.upstream4.length && context.upstreamProtocol == uci.get('network', 'wan', 'proto')) {
				avoidSubnetConflict(context);
				state.subnet_done = true;
			}
		}
		finally { recorder = null; }
		uci.set('router-tool', 'main', 'auto_role', role);
		uci.set('router-tool', 'main', 'state', finishSnapshot(state));
	}
	else {
		const state = previousSnapshot;
		const option = trackedOption(state, 'network', 'wan', 'proto');
		const device = trackedOption(state, 'network', 'wan6', 'device');
		if (state?.proto != uci.get('network', 'wan', 'proto') ||
			device && !device.custom && device.expected != wan6Device() && !customOption(state, 'network', 'wan6', 'device')) {
			recorder = state;
			try { setOptions('network', 'wan6', { device: wan6Device() }); }
			finally { recorder = null; }
			if (option) option.expected = uci.get('network', 'wan', 'proto');
			uci.set('router-tool', 'main', 'state', finishSnapshot(state));
		}
	}
}

function enableAp(sid, context) {
	const topo = topology(context);
	validateAp(topo, context);
	console.info(`[router-tool] Preparing AP mode on ${topo.bridgeName}; Ethernet ports: ${topo.ports.join(' ')}`);
	const state = { version: 1, mode: 'ap', options: [], created: [], subnet_done: true };
	recorder = state;
	try {
		for (const name of [ 'lan', 'wan', 'wan6' ]) ensure('network', 'interface', name);
		const bridge = topo.bridge?.['.name'] || ensure('network', 'device', 'router_tool_bridge');
		setOptions('network', bridge, { name: topo.bridgeName, type: 'bridge', ports: topo.ports });
		setOptions('network', 'wan', { proto: 'none', device: null, disabled: '1', auto: '0' });
		for (const section of uci.sections('network', 'interface'))
			if (![ 'lan', 'wan', 'wan6' ].includes(section['.name']) && topo.ports.includes(uci.get('network', section['.name'], 'device')))
				setOptions('network', section['.name'], { disabled: '1', auto: '0' });
		setOptions('network', 'lan', { device: topo.bridgeName, proto: 'dhcp', ipaddr: null, netmask: null, gateway: null, dns: null,
			ip6addr: null, ip6gw: null, ip6assign: null, ip6class: null, ip6ifaceid: null, ipv6: '0', delegate: '0', disabled: null, peerdns: '1', defaultroute: '1', auto: '1' });
		setOptions('network', 'wan6', { proto: 'dhcpv6', device: topo.bridgeName, reqaddress: 'try', reqprefix: 'no', delegate: '0',
			defaultroute: '1', peerdns: '1', noslaaconly: '0', forceprefix: '0', disabled: null, auto: '1' });
		for (const name of [ 'lan', 'wan', 'wan6' ]) {
			ensure('dhcp', 'dhcp', name);
			setOptions('dhcp', name, { interface: name, ignore: '1', dhcpv4: 'disabled', ra: 'disabled', dhcpv6: 'disabled', ndp: 'disabled', master: '0', ndproxy_routing: null });
		}
		for (const section of uci.sections('wireless', 'wifi-iface')) {
			const networks = L.toArray(uci.get('wireless', section['.name'], 'network'));
			if ([ 'ap', 'ap-wds' ].includes(section.mode) && networks.some(name => [ 'lan', 'wan', 'wan6' ].includes(name)))
				setOptions('wireless', section['.name'], { network: Array.from(new Set(networks.map(name => [ 'wan', 'wan6' ].includes(name) ? 'lan' : name))) });
		}
		configureFirewall('relay_ap', context.presets.firewall);
	}
	finally { recorder = null; }
	uci.set('router-tool', 'main', 'ap_state', finishSnapshot(state));
	uci.set('router-tool', 'main', 'ap_mode', '1');
	console.info(`[router-tool] AP configuration staged; saved ${state.options.length} options and ${state.created.length} new sections; awaiting Save & Apply`);
}

function disableAp(sid) {
	const state = readSnapshot(sid, 'ap_state', 'ap');
	checkSnapshot(state);
	for (const option of state.options.slice().reverse())
		if (uci.get(option.config, option.sid) != null && optionEqual(option, uci.get(option.config, option.sid, option.key), option.expected))
			setOptions(option.config, option.sid, { [option.key]: option.value });
	for (const section of state.created.slice().reverse()) {
		const current = sectionValues(section.config, section.sid);
		if (current && equal(current, section.values)) uci.remove(section.config, section.sid);
	}
	const wan = uci.get('network', 'wan', 'device'), lan = uci.get('network', 'lan', 'device');
	const bridge = findUnique('network', 'device', s => s.name == lan && s.type == 'bridge');
	if (wan && ![ null, 'none' ].includes(uci.get('network', 'wan', 'proto')) &&
		(wan == lan || bridge && L.toArray(uci.get('network', bridge, 'ports')).includes(wan)))
		refuse(_('Mode switch refused: the active WAN device is still part of the LAN bridge. Review device assignments.'));
	uci.unset('router-tool', 'main', 'ap_state');
	uci.set('router-tool', 'main', 'ap_mode', '0');
	console.info(`[router-tool] Previous configuration restored from AP snapshot; ${state.options.length} options; awaiting Save & Apply`);
}

return baseclass.extend({
	attach(map) {
		if (map._routerToolAttached) return;
		map._routerToolAttached = true;
		for (const config of [ 'network', 'dhcp', 'firewall', 'wireless', 'router-tool' ]) map.chain(config);
		const context = { board: null, upstream4: [], upstreamProtocol: null, presets: null };
		const load = map.load.bind(map);
		map.load = function() {
			return Promise.all([ load(), callBoardJSON().catch(e => {
				console.warn('[router-tool] Board port assignments could not be loaded: ' + e.message);
				return {};
			}),
				fs.read('/usr/share/router-tool/auto.json'), callInterfaces().catch(() => []) ]).then(data => {
				if (uci.get('router-tool', 'main')?.['.type'] != 'settings')
					refuse(_('Mode switch refused: section type changed at') + ' router-tool.main');
				context.board = data[1];
				context.presets = JSON.parse(data[2]);
				const upstream = data[3].find(n => n.interface == 'wan') || {};
				context.upstreamProtocol = upstream.proto;
				context.upstream4 = upstream.up ? (upstream['ipv4-address'] || []).map(a => `${a.address}/${a.mask}`) : [];
			}).catch(e => {
				console.error('[router-tool] Failed to load role settings: ' + e.message);
				throw e;
			});
		};
		const save = map.save.bind(map);
		map.save = function(cb, silent) {
			let previous = null;
			const sid = globals();
			const option = map._routerToolOption;
			const enabled = uci.get('router-tool', 'main', 'ap_mode') == '1';
			const selected = option ? option.formvalue(sid) == '1' : enabled;
			const retained = retainedOptions(map, sid, enabled);
			// Preserve the page's base before form parsing. The save callback
			// rebases unrelated external edits and rejects overlapping changes.
			const before = copy(uci.state.values);
			// Only the standard map save/apply flow persists these changes.
			return save(() => {
				previous = copy(uci.state);
				// Preserve unchanged owned values which LuCI removes as default
				// or inactive fields. Explicit active edits remain user changes.
				for (const o of retained)
					if (uci.get(o.config, o.sid, o.key) == null) uci.set(o.config, o.sid, o.key, copy(o.expected));
				return verifyLiveSnapshot(sid, selected, enabled, before).then(() => {
				if (sid && selected != enabled) {
					if (selected) enableAp(sid, context);
					else disableAp(sid);
				}
				// Leaving AP restores unchanged preset values and keeps manual
				// edits. Only a changed WAN protocol needs a new routed preset.
				const role = uci.get('network', 'wan', 'proto') == 'pppoe' ? 'main' : 'sub';
				if (sid && !selected && (!enabled || role != uci.get('router-tool', 'main', 'auto_role'))) synchronize(sid, context);
				return cb ? cb() : undefined;
				});
			}, silent).catch(e => {
				if (previous) uci.state = previous;
				console.error('[router-tool] Failed to save role settings: ' + e.message);
				if (e.routerToolConflict) {
					ui.showModal(null, [
						E('p', {}, e.message),
						E('div', { 'class': 'right' }, [
							E('button', { 'class': 'cbi-button', 'click': ui.hideModal }, [ _('Dismiss') ])
						])
					]);
					return fs.exec('/usr/bin/logger', [ '-t', 'router-tool', '-p', 'daemon.warning', e.message ]).catch(error => {
						console.warn('[router-tool] Could not write system log: ' + error.message);
					}).then(() => { throw e; });
				}
				throw e;
			});
		};
	},

	addGlobal(section) {
		const o = section.option(form.Flag, 'router_tool_ap', _('Relay AP mode'),
			_('Manually enable to bridge all Ethernet ports and obtain IPv4/IPv6 from the upstream router; disable to restore the previous settings. When disabled, PPPoE automatically uses main-router mode and other WAN protocols use sub-router mode.') + ' ' +
			_('Manual changes are preserved. Concurrent edits to the same settings or incompatible device assignments stop the switch.'));
		o.default = '0';
		o.rmempty = false;
		o.uciconfig = 'router-tool';
		o.ucisection = 'main';
		o.ucioption = 'ap_mode';
		o.load = function() {
			return uci.get('router-tool', 'main', 'ap_mode');
		};
		// Apply the flag with its snapshot after parsing and conflict checks.
		o.write = function() {};
		o.remove = function() {};
		section.map._routerToolOption = o;
		return o;
	}
});
