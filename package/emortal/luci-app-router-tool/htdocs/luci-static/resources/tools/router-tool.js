'use strict';
'require baseclass';
'require form';
'require fs';
'require rpc';
'require request';
'require ui';
'require uci';

let recorder = null;
let presets;
let saveQueue = Promise.resolve();

function queueSave(callback) {
	const pending = saveQueue.then(callback);
	saveQueue = pending.catch(() => {});
	return pending;
}

const callBoardJSON = rpc.declare({
	object: 'luci-rpc',
	method: 'getBoardJSON',
	expect: { '': {} }
});

function loadPresets() {
	return presets ??= fs.read('/usr/share/router-tool/auto.json').then(JSON.parse).catch(error => {
		presets = null;
		throw error;
	});
}

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

function sectionValues(config, sid, section = uci.sections(config).find(s => s['.name'] == sid)) {
	return section && Object.fromEntries(Object.keys(section).filter(k => k[0] != '.').map(k => [ k, copy(uci.get(config, sid, k)) ]).filter(v => v[1] != null));
}

const routedConfigs = [ 'network', 'dhcp', 'firewall', 'router-tool' ];
const wirelessConfigs = [ 'wireless', 'network', 'firewall' ];

function values(configs) {
	return Object.fromEntries(configs.filter(config => uci.state.values[config]).map(config =>
		[ config, Object.fromEntries(uci.sections(config).map(section => {
			const sid = section['.name'];
			return [ sid, {
				...Object.fromEntries(Object.entries(section).filter(([ key ]) => key[0] == '.')),
				...sectionValues(config, sid, section)
			} ];
		})) ]));
}

function controller(action, content, query) {
	return request.post(L.url('admin', 'uci', action), content, {
		query: { token: L.env.token, ...query },
		headers: content ? { 'Content-Type': 'application/x-www-form-urlencoded' } : undefined
	}).then(response => {
		if (response.status == 409) refuse(response.responseText);
		if (!response.ok) throw new Error(response.responseText || _('Unable to check saved settings. Reload the page and try again.'));
		return response.json();
	});
}

function refuse(message) {
	console.warn('[router-tool] ' + message);
	const error = new Error(message);
	error.routerToolConflict = true;
	throw error;
}

function recordOption(state, config, sid, key) {
	if (state.created.some(s => s.config == config && s.sid == sid) ||
	    state.options.some(o => o.config == config && o.sid == sid && o.key == key)) return;
	state.options.push({
		config, sid, key, type: uci.get(config, sid, '.type'), value: copy(uci.get(config, sid, key))
	});
}

function finishSnapshot(state) {
	for (const option of state.options)
		option.expected = copy(uci.get(option.config, option.sid, option.key));
	for (const section of state.created) {
		section.type = uci.get(section.config, section.sid, '.type');
		section.values = sectionValues(section.config, section.sid);
	}
	return JSON.stringify(state);
}

function setOptions(config, sid, values) {
	for (const [ key, value ] of Object.entries(values)) {
		if (recorder) recordOption(recorder, config, sid, key);
		if (!equal(uci.get(config, sid, key), value)) uci.set(config, sid, key, value);
	}
}

function ensure(config, type, sid) {
	const section = uci.get(config, sid);
	if (section && section['.type'] != type) refuse(_('Mode switch refused: section type changed at') + ` ${config}.${sid}`);
	if (section == null) {
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

function readSnapshot(option, mode) {
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

function checkSnapshot(state) {
	if (!state) refuse(_('Mode switch refused: saved configuration snapshot is missing or invalid.'));
	const conflicts = [];
	for (const entry of state.options.concat(state.created)) {
		const type = uci.get(entry.config, entry.sid)?.['.type'];
		if (type != null && type != entry.type) conflicts.push(`${entry.config}.${entry.sid}`);
	}
	if (conflicts.length) refuse(_('Mode switch refused: user changed related options:') + ' ' + conflicts.join(', '));
}

function verifyLiveSnapshot(selected, enabled, before) {
	return controller('router_tool_prepare').then(live => {
		if (!enabled && live['router-tool']?.main?.ap_state)
			refuse(_('Mode switch refused: an AP snapshot exists while AP mode is disabled. Restore AP mode and use LuCI to switch back.'));
		for (const key of [ 'auto_role', 'ap_mode', 'state', 'ap_state' ])
			if (!equal(live['router-tool']?.main?.[key], before['router-tool']?.main?.[key]))
				refuse(_('Mode switch refused: configuration changed while this page was open. Reload the page.'));
		const state = readSnapshot(enabled ? 'ap_state' : 'state', enabled ? 'ap' : uci.get('router-tool', 'main', 'auto_role'));
		if ((enabled || uci.get('router-tool', 'main', 'auto_role') || uci.get('router-tool', 'main', 'state')) && !state)
			checkSnapshot(state);
		if (enabled && !selected) checkSnapshot(state);
		rebase(before, live);
	});
}

function rebase(before, live) {
	const draft = values(Object.keys(before));
	const plain = section => section && Object.fromEntries(Object.entries(section).filter(([ key ]) => key[0] != '.'));
	for (const [ config, loaded ] of Object.entries(before)) {
		if (!Object.hasOwn(live, config)) continue;
		const current = live[config] || {};
		const edited = draft[config] || {};
		const edits = [];
		const conflict = path => refuse(_('Mode switch refused: user changed related options:') + ` ${config}${path ? '.' + path : ''}`);
		const order = uci.state.reorder[config] && Object.keys(edited);
		if (order && !equal(Object.keys(loaded), Object.keys(current))) conflict();
		for (const name of new Set(Object.keys(loaded).concat(Object.keys(edited)))) {
			const old = loaded[name], next = edited[name], remote = current[name];
			if (!old) {
				if (remote && next) conflict(name);
				continue;
			}
			if (!next) {
				if (remote && (!equal(plain(old), plain(remote)) || old['.type'] != remote['.type'])) conflict(name);
				if (remote) edits.push(() => uci.remove(config, name));
				continue;
			}
			if (!remote || old['.type'] != remote['.type']) {
				if (!equal(plain(old), plain(next)) || old['.type'] != next['.type']) conflict(name);
				continue;
			}
			for (const key of new Set(Object.keys(plain(old)).concat(Object.keys(plain(next))))) {
				const option = { config, sid: name, type: old['.type'], key };
				if (optionEqual(option, next[key], old[key]) || optionEqual(option, next[key], remote[key])) continue;
				if (!optionEqual(option, remote[key], old[key])) conflict(name + '.' + key);
				edits.push(() => uci.set(config, name, key, next[key] ?? null));
			}
		}
		// Install the refreshed base, then replay only the page's disjoint edits.
		// Newly created sections remain in LuCI's original staged section table.
		uci.state.values[config] = copy(current);
		delete uci.state.changes[config];
		delete uci.state.deletes[config];
		edits.forEach(edit => edit());
		if (order)
			order.forEach((name, index) => {
				const section = uci.state.creates[config]?.[name] || uci.state.values[config][name];
				if (section) section['.index'] = index;
			});
	}
}

function retainedOptions(map, enabled) {
	const state = readSnapshot(enabled ? 'ap_state' : 'state', enabled ? 'ap' : uci.get('router-tool', 'main', 'auto_role'));
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
	const managed = topo.ports.concat(topo.bridgeName);
	const vlanOnManagedDevice = name => typeof name == 'string' && managed.some(port => name.startsWith(port + '.'));
	const links = (type, option) => uci.sections('network', type).flatMap(section =>
		L.toArray(uci.get('network', section['.name'], option)).map(device => [ section['.name'], device ]));
	const dependent = links('interface', 'device').concat(links('interface', 'ifname'))
		.find(([ name, device ]) => vlanOnManagedDevice(device) ||
			![ 'lan', 'wan', 'wan6' ].includes(name) && [ '@lan', '@wan', '@wan6' ].includes(device));
	const vlan = devices.find(section => [ '8021q', '8021ad', 'vlan' ].includes(section.type) &&
		[ section.ifname, section.device ].some(device => managed.includes(device) || vlanOnManagedDevice(device)));
	if (dependent || vlan)
		throw new Error(_('Automatic AP conversion conflicts with network section:') + ' ' + (dependent?.[0] ?? vlan['.name']));
	if (uci.sections('network', 'switch').length || uci.sections('network', 'switch_vlan').length ||
		uci.sections('network', 'bridge-vlan').some(s => s.device == topo.bridgeName) ||
		devices.some(s => s.name != topo.bridgeName && L.toArray(s.ports).some(port => topo.ports.includes(port))) ||
		devices.some(s => topo.ports.includes(s.name) && s.type && ![ '8023', 'ethernet' ].includes(s.type)) ||
		topo.ports.some(port => /[.@:]/.test(port) || port == topo.bridgeName || /^(br-|wlan|phy)/.test(port)))
		throw new Error(_('Automatic AP conversion requires plain Ethernet ports. Configure VLAN or switch topologies manually.'));
}

function configureApFirewall(data) {
	const managed = [ 'lan', 'wan', 'wan6' ];
	const zones = Object.fromEntries([ 'lan', 'wan' ].map(name => [
		name, findUnique('firewall', 'zone', s => s.name == name) ?? reserve('firewall', 'zone', `router_tool_${name}`)
	]));
	for (const section of uci.sections('firewall', 'zone')) {
		const sid = section['.name'];
		const networks = L.toArray(uci.get('firewall', sid, 'network')).filter(n => !managed.includes(n));
		if (sid == zones.lan) networks.push('lan', 'wan6');
		setOptions('firewall', sid, { network: networks.length ? networks : null });
	}
	for (const [ name, sid ] of Object.entries(zones))
		setOptions('firewall', sid, { name, ...data.zones[name], masq: '0' });
	for (const section of uci.sections('firewall', 'defaults'))
		setOptions('firewall', section['.name'], { disable_ipv6: null });

	const forwarding = findUnique('firewall', 'forwarding', s => s.src == 'lan' && s.dest == 'wan') ??
		reserve('firewall', 'forwarding', 'router_tool_lan_wan');
	setOptions('firewall', forwarding, { src: 'lan', dest: 'wan', enabled: '0', family: null });
	for (const name of Object.keys(data.rules)) {
		const sid = findUnique('firewall', 'rule', s => s.name == name);
		if (sid) setOptions('firewall', sid, { enabled: '0' });
	}
}

function synchronize() {
	const input = new URLSearchParams({ values: JSON.stringify(values(routedConfigs)) });
	return controller('router_tool_plan', input.toString()).then(edits => {
		for (const edit of edits) {
			if (edit.created) {
				uci.add(edit.config, edit.type, edit.sid);
				setOptions(edit.config, edit.sid, edit.created);
			}
			else {
				for (const option of edit.options)
					uci.set(edit.config, edit.sid, option.key, option.after ?? null);
			}
		}
	});
}

function enableAp(context) {
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
		const managed = new Set([ 'lan', 'wan', 'wan6' ]);
		for (const section of uci.sections('network', 'interface')) {
			const name = section['.name'];
			const devices = L.toArray(uci.get('network', name, 'device') || uci.get('network', name, 'ifname'));
			if (!managed.has(name) && devices.some(device => topo.ports.includes(device))) {
				setOptions('network', name, { disabled: '1', auto: '0' });
				managed.add(name);
			}
		}
		setOptions('network', 'lan', { device: topo.bridgeName, proto: 'dhcp', ipaddr: null, netmask: null, gateway: null, dns: null,
			ip6addr: null, ip6gw: null, ip6assign: null, ip6class: null, ip6ifaceid: null, ipv6: '0', delegate: '0', disabled: null, peerdns: '1', defaultroute: '1', auto: '1' });
		setOptions('network', 'wan6', { proto: 'dhcpv6', device: topo.bridgeName, reqaddress: 'try', reqprefix: 'no', delegate: '0',
			defaultroute: '1', peerdns: '1', noslaaconly: '0', forceprefix: '0', disabled: null, auto: '1' });
		for (const section of uci.sections('dhcp', 'dhcp')) {
			const name = section['.name'];
			if (managed.has(uci.get('dhcp', name, 'interface') || name))
				setOptions('dhcp', name, { ignore: '1', dhcpv4: 'disabled', ra: 'disabled', dhcpv6: 'disabled', ndp: 'disabled', master: '0', ndproxy_routing: null });
		}
		for (const section of uci.sections('wireless', 'wifi-iface')) {
			const networks = L.toArray(uci.get('wireless', section['.name'], 'network'));
			if ([ 'ap', 'ap-wds' ].includes(section.mode) && networks.some(name => [ 'lan', 'wan', 'wan6' ].includes(name)))
				setOptions('wireless', section['.name'], { network: Array.from(new Set(networks.map(name => [ 'wan', 'wan6' ].includes(name) ? 'lan' : name))) });
		}
		configureApFirewall(context.presets.firewall);
	}
	finally { recorder = null; }
	uci.set('router-tool', 'main', 'ap_state', finishSnapshot(state));
	uci.set('router-tool', 'main', 'ap_mode', '1');
	console.info(`[router-tool] AP configuration staged; saved ${state.options.length} options and ${state.created.length} new sections; awaiting Save & Apply`);
}

function disableAp() {
	const state = readSnapshot('ap_state', 'ap');
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

function wirelessChanges(sections) {
	const changes = ui.changes.changes?.wireless;
	return Array.isArray(changes) ? changes.filter(([, id]) => sections.includes(id)).length : 0;
}

function wirelessNetworkInfo(wifi) {
	const mode = wifi.getActiveMode();
	const bssid = wifi.getActiveBSSID();
	const disabled = wifi.get('disabled') == '1' || uci.get('wireless', wifi.getWifiDeviceName(), 'disabled') == '1';
	const associated = !disabled && !!bssid && bssid != '00:00:00:00:00:00' && !!wifi.getChannel() && mode != 'Unknown';
	return { mode, bssid, disabled, associated, pending: wirelessChanges([wifi.getName()]) };
}

function wirelessStatusText(state) {
	const { status, error } = state;
	if (state.disabled)
		return _('Wireless is disabled');
	if (error || state.failed || status == 'DISABLED')
		return _('Wireless configuration failed') + (error ? ' (' + error + ')' : '');
	if (state.cac_active || status == 'DFS')
		return state.cac ? _('DFS availability check (%d s remaining)').format(state.cac) : _('DFS availability check');
	if (status == 'ACS') return _('Selecting a channel');
	if (status == 'WAITING_STA') return _('Waiting for the station uplink');
	if (state.pending || status == 'STARTING' || status == 'HT_SCAN' || status == 'COUNTRY_UPDATE')
		return _('Starting wireless');
	return null;
}

function configuredBandwidth(htmode) {
	return htmode == 'NOHT' ? '20' : htmode?.match(/^(?:EHT|HE|VHT|HT)(\d+(?:\+\d+)?)[+-]?$/)?.[1];
}

function wirelessAdjustmentReasons(data, ap, automatic) {
	const reasons = [];
	const channel = +ap.channel;
	if (channel > 0 && +data.channel > 0 && channel != +data.channel)
		reasons.push(_('Channel changed (reason not reported)'));
	else if (channel > 0 && automatic && data.candidates?.length && !data.candidates.includes(String(channel)))
		reasons.push(_('Operating channel outside the candidate list'));

	const requested = data.chanbw > 0 ? String(data.chanbw) : configuredBandwidth(data.htmode);
	const actual = ap.width ? String(ap.width) : null;
	if (requested && actual && actual != requested) {
		const planned = configuredBandwidth(data.planned_htmode);
		if (actual == planned)
			reasons.push(_('Bandwidth selected from available channel blocks'));
		else if (data.band == '2g' && !data.noscan && planned == '40' && actual == '20')
			reasons.push(_('Possible 20/40 MHz coexistence limit'));
		else
			reasons.push(_('Bandwidth adjusted (reason not reported)'));
	}
	return reasons;
}

return baseclass.extend({
	wirelessNetworkInfo,

	wirelessStatus() {
		return request.get(L.url('admin', 'network', 'router_tool_wireless_status')).then(response =>
			response.ok ? response.json() : {}).catch(() => ({}));
	},

	renderWirelessDetails(data, wifi) {
		const details = data?.details?.[wifi.getName()];
		if (!details || !wifi.isUp() || wirelessStatusText(data)) return null;
		const { standard, country, power } = details;
		const fields = [];
		if (standard) fields.push(_('Wireless standard') + ' ' + standard);
		if (country) fields.push(_('Current region') + ' ' + country);
		if (Number.isFinite(power)) fields.push(_('Current power') + ' ' + power + ' dBm');
		return fields.length ? E('small', { 'class': 'router-tool-wireless-details' }, fields.join(' · ')) : null;
	},

	renderWirelessState(data, radio, networks) {
		data ??= {};
		const active = Object.values(data.aps || {}).filter(ap => !ap.disabled);
		const enabled = active.filter(ap => ap.status == 'ENABLED' && !ap.error);
		const dfs = active.find(ap => ap.cac_active || ap.status == 'DFS');
		const status = data.status ?? (enabled[0] ?? active[0])?.status;
		const disabled = data.disabled || radio.get('disabled') == '1';
		const shared = wirelessStatusText({ ...data, status, disabled, cac_active: !!dfs, cac: dfs?.cac });
		const operating = !shared && status == 'ENABLED' ? enabled.find(ap => ap.width) ?? enabled[0] : null;

		const details = new Map();
		if (!disabled) {
			for (const wifi of networks) {
				const info = wirelessNetworkInfo(wifi);
				const ap = data.aps?.[wifi.getName()];
				const message = wirelessStatusText({ ...ap, disabled: info.disabled || ap?.disabled, cac: dfs?.cac }) ??
					(!shared && !info.associated && ap?.status != 'ENABLED' ? _('Wireless is not associated') : null);
				if (!message || message == shared) continue;
				const name = (wifi.getMode() == 'mesh' ? wifi.getMeshID() : wifi.getSSID()) || wifi.getName();
				if (!details.has(message)) details.set(message, new Set());
				details.get(message).add(name);
			}
		}

		const fields = [];
		if (shared)
			fields.push(shared);
		else if (status == 'ENABLED')
			fields.push(operating?.width ? _('Operating bandwidth') + ' ' + operating.width + ' MHz' : _('AP enabled'));
		else if (!radio.isUp() && !details.size)
			fields.push(_('Device is not active'));
		if (data.htmode) {
			fields.push(_('Configured bandwidth') + ' ' + data.htmode);
			if (data.planned_htmode && data.planned_htmode != data.htmode)
				fields.push(_('Selected bandwidth') + ' ' + data.planned_htmode);
		}
		const automatic = data.channel == 'auto' || data.channel == '0';
		const reasons = operating ? wirelessAdjustmentReasons(data, operating, automatic) : [];
		if (reasons.length)
			fields.push(...reasons);
		else if (automatic && data.candidates?.length)
			fields.push(_('%d eligible channels').format(data.candidates.length));
		for (const [message, names] of details)
			fields.push(`${Array.from(names).join(', ')}: ${message}`);

		const pending = wirelessChanges([radio.getName(), ...networks.map(wifi => wifi.getName())]);
		if (pending)
			fields.push(E('a', { href: '#', click: L.bind(ui.changes.displayChanges, ui.changes) },
				_('%d pending wireless changes').format(pending)));

		return fields.length ? E('small', { 'class': 'router-tool-wireless-state' },
			fields.flatMap((field, index) => index ? [' · ', field] : [field])) : null;
	},

	addAcsChannelList(section) {
		const options = [ 'channels', 'channel_list', 'chanlist' ];
		const list = section.taboption('advanced', form.Value, 'channels', _('ACS channel list'),
			_('Separate channels with spaces. Use a hyphen for a range. Leave empty to use every channel compatible with the selected bandwidth.'));
		list.placeholder = '36 40 44 48';
		list.load = sid => {
			const value = options.map(name => uci.get('wireless', sid, name)).find(value => value != null);
			return value == null ? null : L.toArray(value).join(' ').trim().replace(/[ ,\t]+/g, ' ');
		};
		list.write = (sid, value) => {
			uci.set('wireless', sid, 'channels', value.trim());
			for (const name of options.slice(1)) uci.unset('wireless', sid, name);
		};
		list.remove = sid => {
			if (list.cfgvalue(sid))
				for (const name of options) uci.unset('wireless', sid, name);
		};
		list.validate = (section_id, value) => {
			const text = value?.trim();
			if (!text) return true;
			const valid = text.split(/ +/).every(token => {
				const range = /^(\d{1,3})(?:-(\d{1,3}))?$/.exec(token);
				if (!range) return false;
				const first = Number(range[1]);
				const last = Number(range[2] ?? range[1]);
				return first >= 1 && last >= first && last <= 233;
			});
			return valid || _('Enter channel numbers from 1 to 233 or ascending ranges, separated by spaces.');
		};
	},

	trackWireless(map) {
		if (map._routerToolWireless) return;
		map._routerToolWireless = true;
		for (const config of wirelessConfigs) map.chain(config);
		let before;
		const load = map.load.bind(map), save = map.save.bind(map);
		map.load = function() {
			return load().then(() => { before = values(wirelessConfigs); });
		};
		map.save = function(cb, silent) {
			return queueSave(() => save(async () => {
				const live = await controller('router_tool_prepare', null, { config: 'wireless' });
				const previous = copy(uci.state);
				try {
					rebase(before, live);
					return cb ? await cb() : undefined;
				}
				catch (error) {
					uci.state = previous;
					throw error;
				}
			}, silent));
		};
	},

	attach(map) {
		if (map._routerToolAttached) return;
		map._routerToolAttached = true;
		for (const config of [ 'network', 'dhcp', 'firewall', 'wireless', 'router-tool' ]) map.chain(config);
		let before;
		const load = map.load.bind(map);
		map.load = function() {
			return load().then(() => {
				before = values(routedConfigs.concat('wireless'));
				if (before['router-tool']?.main?.['.type'] != 'settings')
					refuse(_('Mode switch refused: section type changed at') + ' router-tool.main');
			});
		};
		const save = map.save.bind(map);
		map.save = (cb, silent) => queueSave(() => {
			let previous = null;
			const sid = globals();
			const option = map._routerToolOption;
			const enabled = uci.get('router-tool', 'main', 'ap_mode') == '1';
			const selected = option ? option.formvalue(sid) == '1' : enabled;
			const retained = retainedOptions(map, enabled);
			// Only the standard map save/apply flow persists these changes.
			return save(() => {
				previous = copy(uci.state);
				// Preserve unchanged owned values which LuCI removes as default
				// or inactive fields. Explicit active edits remain user changes.
				for (const o of retained)
					if (uci.get(o.config, o.sid, o.key) == null) uci.set(o.config, o.sid, o.key, copy(o.expected));
				return verifyLiveSnapshot(selected, enabled, before).then(async () => {
					if (sid && selected != enabled) {
						if (selected) {
							const [ board, presets ] = await Promise.all([ callBoardJSON(), loadPresets() ]);
							enableAp({ board, presets });
						}
						else disableAp();
					}
					const role = uci.get('network', 'wan', 'proto') == 'pppoe' ? 'main' : 'sub';
					if (sid && !selected && (!enabled || role != uci.get('router-tool', 'main', 'auto_role')))
						await synchronize();
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
		});
	},

	addGlobal(section) {
		const o = section.option(form.Flag, 'router_tool_ap', _('Relay AP mode'),
			_('Manually enable to bridge all Ethernet ports and obtain IPv4/IPv6 from the upstream router; disable to restore the previous settings. When disabled, PPPoE automatically uses main-router mode and other WAN protocols use sub-router mode.') + ' ' +
			_('Manual changes are preserved. Concurrent edits to the same settings or incompatible device assignments stop the switch.') + ' ' +
			_('The upstream router may assign a new management address. With checked apply, LuCI rolls back if it cannot confirm the change before the timer expires.'));
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
