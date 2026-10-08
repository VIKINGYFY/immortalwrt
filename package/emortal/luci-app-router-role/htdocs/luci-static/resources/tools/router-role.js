'use strict';
'require baseclass';
'require form';
'require fs';
'require network';
'require uci';

let recorder = null;

function setOptions(config, sid, values) {
	for (const [ key, value ] of Object.entries(values)) {
		if (recorder && !recorder.created.some(s => s.config == config && s.sid == sid)) {
			const id = `${config}/${sid}/${key}`;
			if (!recorder.options.some(option => option.id == id))
				recorder.options.push({ id, config, sid, key, value: uci.get(config, sid, key) ?? null });
		}
		if (value == null) uci.unset(config, sid, key);
		else uci.set(config, sid, key, value);
	}
}

function ensure(config, type, sid) {
	if (uci.get(config, sid) == null) {
		uci.add(config, type, sid);
		if (recorder) recorder.created.push({ config, sid });
	}
	return sid;
}

function globals() {
	return uci.sections('network', 'globals')[0]?.['.name'];
}

function apState(sid) {
	try { return JSON.parse(uci.get('network', sid, 'router_role_ap_state') || 'null'); }
	catch (e) { return null; }
}

function boardPorts(board, name) {
	return L.toArray(board?.network?.[name]?.ports || board?.network?.[name]?.device);
}

function topology(context) {
	const device = uci.get('network', 'lan', 'device') || 'br-lan';
	const bridge = uci.sections('network', 'device').find(s => s.name == device && s.type == 'bridge');
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
	if (uci.sections('network', 'switch').length || uci.sections('network', 'switch_vlan').length ||
		uci.sections('network', 'bridge-vlan').some(s => s.device == topo.bridgeName) ||
		topo.ports.some(port => /[.@:]/.test(port) || port == topo.bridgeName || /^(br-|wlan|phy)/.test(port)))
		throw new Error(_('Automatic AP conversion requires plain Ethernet ports. Configure VLAN or switch topologies manually.'));
}

function zone(name) {
	return uci.sections('firewall', 'zone').find(s => s.name == name)?.['.name'] ||
		ensure('firewall', 'zone', `router_role_${name}`);
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
	const forwarding = uci.sections('firewall', 'forwarding').find(s => s.src == 'lan' && s.dest == 'wan')?.['.name'] ||
		ensure('firewall', 'forwarding', 'router_role_lan_wan');
	setOptions('firewall', forwarding, { src: 'lan', dest: 'wan', enabled: role == 'relay_ap' ? '0' : '1', family: null });
	for (const [ name, values ] of Object.entries(data.rules)) {
		const existing = uci.sections('firewall', 'rule').find(s => s.name == name)?.['.name'];
		const enabled = role != 'relay_ap' && (name != 'Router-Role-DHCPv6-Relay' || role == 'sub');
		if (!enabled && !existing) continue;
		const sid = existing || ensure('firewall', 'rule', 'router_role_' + name.toLowerCase().replaceAll('-', '_'));
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
	let prefix = cidr != null ? Number(cidr) : 24;
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
	for (let i = 2; i < 255; i++) {
		const address = `192.168.${i}.1/24`;
		if (upstream.every(b => !overlaps(subnet(address), b))) {
			setOptions('network', 'lan', { ipaddr: address, netmask: null });
			return;
		}
	}
	throw new Error(_('No free LAN subnet was found. Configure a non-overlapping IPv4 subnet manually.'));
}

function synchronize(sid, context) {
	if (uci.get('network', sid, 'router_role_ap') == '1') return;
	const role = uci.get('network', 'wan', 'proto') == 'pppoe' ? 'main' : 'sub';
	if (uci.get('network', sid, 'router_role_auto') != role) {
		for (const group of [ context.presets.common, context.presets.roles[role] ])
			for (const config of [ 'network', 'dhcp' ])
				for (const [ name, options ] of Object.entries(group[config] || {})) {
					ensure(config, config == 'network' ? 'interface' : 'dhcp', name);
					setOptions(config, name, options);
				}
		configureFirewall(role, context.presets.firewall);
		uci.set('network', sid, 'router_role_auto', role);
	}
	if (role == 'sub') avoidSubnetConflict(context);
}

function enableAp(sid, context) {
	const topo = topology(context);
	validateAp(topo, context);
	const state = { options: [], created: [] };
	recorder = state;
	try {
		for (const name of [ 'lan', 'wan', 'wan6' ]) ensure('network', 'interface', name);
		const bridge = topo.bridge?.['.name'] || ensure('network', 'device', 'router_role_bridge');
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
		for (const section of uci.sections('wireless', 'wifi-iface'))
			if ([ 'ap', 'ap-wds' ].includes(uci.get('wireless', section['.name'], 'mode')))
				setOptions('wireless', section['.name'], { network: [ 'lan' ] });
		configureFirewall('relay_ap', context.presets.firewall);
	}
	finally { recorder = null; }
	uci.set('network', sid, 'router_role_ap_state', JSON.stringify(state));
	uci.set('network', sid, 'router_role_ap', '1');
}

function disableAp(sid) {
	const state = apState(sid);
	if (!state)
		throw new Error(_('The saved configuration is missing. Restore LAN/WAN settings manually before disabling AP mode.'));
	for (const option of state.options.slice().reverse())
		if (uci.get(option.config, option.sid) != null)
			setOptions(option.config, option.sid, { [option.key]: option.value });
	for (const section of state.created.slice().reverse()) uci.remove(section.config, section.sid);
	uci.unset('network', sid, 'router_role_ap_state');
	uci.set('network', sid, 'router_role_ap', '0');
}

return baseclass.extend({
	attach(map) {
		if (map._routerRoleAttached) return;
		map._routerRoleAttached = true;
		for (const config of [ 'network', 'dhcp', 'firewall', 'wireless' ]) map.chain(config);
		const context = { board: null, upstream4: [], presets: null };
		const load = map.load.bind(map);
		map.load = function() {
			return Promise.all([ load(), L.resolveDefault(fs.read('/etc/board.json'), '{}'),
				fs.read('/usr/share/router-role/auto.json'), network.getNetworks() ]).then(data => {
				try { context.board = JSON.parse(data[1]); } catch (e) { context.board = null; }
				context.presets = JSON.parse(data[2]);
				const upstream = uci.get('network', 'lan', 'proto') == 'dhcp' ? 'lan' : 'wan';
				context.upstream4 = data[3].find(n => n.getName() == upstream)?.getIPAddrs() || [];
			});
		};
		const save = map.save.bind(map);
		map.save = function(cb, silent) {
			const sid = globals();
			const option = map._routerRoleOption;
			const enabled = uci.get('network', sid, 'router_role_ap') == '1';
			const selected = option ? option.formvalue(sid) == '1' : enabled;
			// Only the standard map save/apply flow persists these changes.
			return save(() => {
				if (sid && selected != enabled) {
					if (selected) enableAp(sid, context);
					else disableAp(sid);
				}
				// Cancelling AP must restore the snapshot exactly. A staged AP
				// save can still have the old live LAN address; it is not a WAN
				// lease. The backend checks the real WAN after applying changes.
				if (sid && !selected && !enabled) synchronize(sid, context);
				return cb ? cb() : undefined;
			}, silent);
		};
	},

	addGlobal(section) {
		const o = section.option(form.Flag, 'router_role_ap', _('Relay AP mode'),
			_('Manually enable to bridge all Ethernet ports and obtain IPv4/IPv6 from the upstream router; disable to restore the previous settings. When disabled, PPPoE automatically uses main-router mode and other WAN protocols use sub-router mode.'));
		o.default = '0';
		o.rmempty = false;
		o.write = function() {};
		section.map._routerRoleOption = o;
		return o;
	}
});
