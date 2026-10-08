// Shared preset data is also used by the LuCI post-parse save callback.
function sections(uci, config, type) {
	let result = [];
	uci.foreach(config, type, section => push(result, section));
	return result;
}

function array(value) {
	return type(value) == 'array' ? value : value == null ? [] : split(value, /\s+/);
}

function subnet(value, netmask) {
	let parts = split('' + value, '/');
	if (length(parts) > 2) return null;
	let octets = split(parts[0], '.');
	if (length(octets) != 4 || length(filter(octets, n => !match(n, /^\d+$/) || int(n) > 255))) return null;
	if (length(parts) > 1 && !match(parts[1], /^\d+$/)) return null;
	let prefix = length(parts) > 1 ? int(parts[1]) : 24;
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

export function synchronize(uci, data, upstream4) {
	let changed = {};
	function ensure(config, type_name, sid) {
		if (!uci.get_all(config, sid)) {
			if (!uci.set(config, sid, type_name)) die(`Cannot create ${config}.${sid}`);
			changed[config] = true;
		}
		return sid;
	}
	function set(config, sid, values) {
		for (let key, value in values) {
			let previous = uci.get(config, sid, key);
			if (value == null && previous == null) continue;
			if (sprintf('%J', value) == sprintf('%J', previous)) continue;
			let ok = value == null ? uci.delete(config, sid, key) : uci.set(config, sid, key, value);
			if (!ok) die(`Cannot set ${config}.${sid}.${key}`);
			changed[config] = true;
		}
	}
	let sid = sections(uci, 'network', 'globals')[0]?.['.name'] || ensure('network', 'globals', 'globals');
	if (uci.get('network', sid, 'router_role_ap') == '1') return [];
	let role = uci.get('network', 'wan', 'proto') == 'pppoe' ? 'main' : 'sub';
	if (uci.get('network', sid, 'router_role_auto') != role) {
		for (let group in [ data.common, data.roles[role] ])
			for (let config in [ 'network', 'dhcp' ])
				for (let name, options in group[config] || {}) {
					ensure(config, config == 'network' ? 'interface' : 'dhcp', name);
					set(config, name, options);
				}
		let zones = {};
		for (let name in [ 'lan', 'wan' ]) {
			let found = filter(sections(uci, 'firewall', 'zone'), section => section.name == name)[0];
			zones[name] = found?.['.name'] || ensure('firewall', 'zone', 'router_role_' + name);
			set('firewall', zones[name], { name, ...data.firewall.zones[name] });
		}
		for (let section in sections(uci, 'firewall', 'zone')) {
			let networks = filter(array(uci.get('firewall', section['.name'], 'network')), name => index([ 'lan', 'wan', 'wan6' ], name) < 0);
			if (section['.name'] == zones.lan) push(networks, 'lan');
			if (section['.name'] == zones.wan) push(networks, 'wan', 'wan6');
			set('firewall', section['.name'], { network: length(networks) ? networks : null });
		}
		for (let section in sections(uci, 'firewall', 'defaults')) set('firewall', section['.name'], { disable_ipv6: null });
		let forward = filter(sections(uci, 'firewall', 'forwarding'), section => section.src == 'lan' && section.dest == 'wan')[0]?.['.name'] || ensure('firewall', 'forwarding', 'router_role_lan_wan');
		set('firewall', forward, { src: 'lan', dest: 'wan', enabled: '1', family: null });
		for (let name, values in data.firewall.rules) {
			let existing = filter(sections(uci, 'firewall', 'rule'), section => section.name == name)[0]?.['.name'];
			let enabled = name != 'Router-Role-DHCPv6-Relay' || role == 'sub';
			if (!enabled && !existing) continue;
			let rule = existing || ensure('firewall', 'rule', 'router_role_' + replace(lc(name), /-/g, '_'));
			set('firewall', rule, enabled ? { name, enabled: '1', src: 'wan', target: 'ACCEPT', src_ip: null, dest_ip: null, ...values } : { enabled: '0' });
		}
		set('network', sid, { router_role_auto: role });
	}
	// A WAN lease may arrive after configuration was applied. Only an actual
	// overlap changes IPv4; preserve WAN protocol/credentials and DHCP pools.
	if (role == 'sub' && uci.get('network', 'lan', 'proto') == 'static') {
		let upstream = filter(map(upstream4 || [], address => subnet(address)), address => address != null);
		let lan = filter(map(array(uci.get('network', 'lan', 'ipaddr')), address => subnet(address, uci.get('network', 'lan', 'netmask'))), address => address != null);
		if (length(filter(lan, a => length(filter(upstream, b => overlaps(a, b)))))) {
			let address = null;
			for (let i = 2; i < 255; i++) {
				let candidate = `192.168.${i}.1/24`;
				if (!length(filter(upstream, b => overlaps(subnet(candidate), b)))) { address = candidate; break; }
			}
			if (!address) die('No non-overlapping IPv4 LAN subnet found');
			set('network', 'lan', { ipaddr: address, netmask: null });
		}
	}
	return keys(changed);
}
