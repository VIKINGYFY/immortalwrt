// SPDX-License-Identifier: GPL-2.0-only

import { open, stat, readfile } from 'fs';
import { cursor } from 'uci';
import { equal, section_values, value_valid } from '/usr/share/router-tool/snapshot.uc';
import { synchronize } from '/usr/share/router-tool/auto.uc';
import { transaction } from '/usr/share/router-tool/transaction.uc';

const upstream = require('luci.controller.admin.uci');
const configs = [ 'network', 'dhcp', 'firewall', 'wireless', 'router-tool' ];
const baseline_key = 'router_tool_apply_baseline';
const default_session = '00000000000000000000000000000000';

function response(code, status, message) {
	http.status(code, status);
	http.prepare_content('text/plain; charset=UTF-8');
	http.write(message);
}

function rpc(object, method, args) {
	ubus.error();
	const result = ubus.call(object, method, args);
	if (ubus.error(true))
		die(`Router Tool could not ${object}.${method}`);
	return result;
}

function changes(sid) {
	return rpc('uci', 'changes', { ubus_rpc_session: sid }).changes;
}

function baseline(sid) {
	const data = rpc('session', 'get', { ubus_rpc_session: sid, keys: [ baseline_key ] });
	return data.values?.[baseline_key] ? json(data.values[baseline_key]) : null;
}

function clear_if_idle(sid) {
	const pending = ubus.call('uci', 'changes', { ubus_rpc_session: sid })?.changes;
	if (pending && !length(keys(pending)))
		ubus.call('session', 'unset', { ubus_rpc_session: sid, keys: [ baseline_key ] });
}

function committed(name) {
	// An absolute UCI path loads the file without the CLI or LuCI delta.
	const override = `/var/run/uci/${name}`;
	const path = stat(override) ? override : `/etc/config/${name}`;
	return stat(path) ? cursor().get_all(path) : null;
}

function comparable_section(section) {
	return section && [ section['.type'], section_values(section) ];
}

function conflicts(base, live, delta) {
	const paths = [];
	for (let name in configs) {
		const previous = base?.[name];
		if (!previous) continue;
		const current = live[name];
		if (!current) {
			push(paths, name);
			continue;
		}
		for (let change in delta[name] || []) {
			const [ kind, sid, key, value ] = change;
			const old = previous[sid];
			const now = current[sid];
			const option = (kind == 'set' || kind == 'rename') ? length(change) == 4 :
				(kind == 'remove' || kind == 'list-add' || kind == 'list-del') && type(key) == 'string';
			if (kind == 'order') {
				if (!equal(keys(previous), keys(current))) push(paths, name);
				continue;
			}
			if (option) {
				if (!equal(old?.['.type'], now?.['.type']) || !equal(old?.[key], now?.[key]) ||
				    kind == 'rename' && !equal(old?.[value], now?.[value]))
					push(paths, `${name}.${sid}.${key}`);
			}
			else if (!equal(comparable_section(old), comparable_section(now)) ||
			         kind == 'rename' && !equal(comparable_section(previous[key]), comparable_section(current[key]))) {
				push(paths, `${name}.${sid}`);
			}
		}
	}
	return uniq(paths);
}

function refuse(paths) {
	response(409, 'Conflict', _('Other changes overlap with your saved settings. Revert the pending changes and reload the page.') +
		' ' + join(', ', paths));
}

function locked(callback) {
	// Serialize LuCI applies with each other and with Router Tool's event worker.
	const lock = open('/var/lock/network-config.lock', 'a');
	if (!lock || !lock.lock('x'))
		die('Cannot lock Router Tool configuration');
	let result;
	try { result = callback(); }
	catch (error) {
		lock.close();
		die(error);
	}
	lock.close();
	return result;
}

function can_access(sid, name, method) {
	return rpc('session', 'access', {
		ubus_rpc_session: sid, scope: 'uci', object: name, 'function': method
	})?.access;
}

function prepare_save() {
	const sid = ctx.authsession;
	const wireless_only = http.formvalue('config') == 'wireless';
	if (!can_access(sid, wireless_only ? 'wireless' : 'router-tool', 'write'))
		return response(403, 'Forbidden', _('Permission denied'));

	const pending = changes(sid);
	const before = baseline(sid);
	const live = {}, values = {}, next = { ...(before || {}) };
	for (let name in configs) {
		if (wireless_only && name != 'wireless' && !before?.[name]) continue;
		if (!can_access(sid, name, 'read')) continue;
		live[name] = committed(name);
		if (name == 'wireless' && !live[name]) {
			delete live[name];
			continue;
		}
		if (!live[name]) die(`Cannot read committed ${name} configuration`);
		if (wireless_only && name != 'wireless') continue;
		values[name] = rpc('uci', 'get', { ubus_rpc_session: sid, config: name }).values;
		next[name] = live[name];
	}
	// Pending edits keep their original expectations. A new save can refresh
	// the baseline once the existing delta still matches the committed values.
	const changed = length(keys(pending)) ? conflicts(before, live, pending) : [];
	if (length(changed)) return refuse(changed);
	rpc('session', 'set', { ubus_rpc_session: sid, values: { [baseline_key]: sprintf('%J', next) } });
	http.prepare_content('application/json; charset=UTF-8');
	http.write_json(values);
}

function routed_plan() {
	const sid = ctx.authsession;
	const names = [ 'network', 'dhcp', 'firewall', 'router-tool' ];
	for (let name in names)
		if (!can_access(sid, name, 'write'))
			return response(403, 'Forbidden', _('Permission denied'));

	try {
		const input = json(http.formvalue('values') || 'null');
		const before = {};
		for (let name in names) {
			if (type(input?.[name]) != 'object') die('Invalid configuration draft');
			before[name] = {};
			for (let section, data in input[name]) {
				if (!match(section, /^[a-zA-Z0-9_][a-zA-Z0-9_-]*$/) ||
				    type(data?.['.type']) != 'string' || !match(data['.type'], /^[a-zA-Z0-9_-]+$/))
					die('Invalid draft section');
				before[name][section] = { '.name': section, '.type': data['.type'] };
				for (let key, value in section_values(data)) {
					if (!match(key, /^[a-zA-Z0-9_]+$/) || !value_valid(value)) die('Invalid draft option');
					before[name][section][key] = value;
				}
			}
		}

		// A private in-memory cursor shares the worker's planner and the
		// transaction diff format. Native LuCI still stages and applies edits.
		const draft = json(sprintf('%J', before));
		const uci = {
			get_all: (name, section) => section == null ? draft[name] : draft[name]?.[section],
			get: (name, section, key) => draft[name]?.[section]?.[key],
			foreach: (name, section_type, callback) => {
				for (let _, section in draft[name])
					if (section['.type'] == section_type) callback(section);
			},
			set: (name, section, key, value) => {
				if (value == null) draft[name][section] = { '.name': section, '.type': key };
				else draft[name][section][key] = value;
				return true;
			},
			'delete': (name, section, key) => {
				delete draft[name][section][key];
				return true;
			}
		};
		const wan = can_access(sid, 'network', 'read') ? ubus.call('network.interface.wan', 'status', {}) : null;
		const upstream4 = wan?.up && wan.proto == uci.get('network', 'wan', 'proto')
			? map(wan['ipv4-address'] || [], address => `${address.address}/${address.mask}`) : [];
		synchronize(uci, json(readfile('/usr/share/router-tool/auto.json')), upstream4);
		http.prepare_content('application/json; charset=UTF-8');
		http.write_json(transaction(uci, before).edits);
	}
	catch (e) {
		return response(409, 'Conflict', e.message);
	}
}

function apply(checked) {
	const sid = ctx.authsession;
	const before = baseline(sid);
	if (before) {
		const pending = changes(sid);
		const live = {};
		for (let name in configs)
			if (before[name]) live[name] = committed(name);
		const changed = length(keys(pending)) ? conflicts(before, live, pending) : [];
		if (length(changed)) return refuse(changed);
	}
	if (checked)
		upstream.action_apply_rollback();
	else {
		upstream.action_apply_unchecked();
		if (before) clear_if_idle(sid);
	}
}

function rollback_state() {
	return rpc('session', 'get', { ubus_rpc_session: default_session, keys: [ 'rollback' ] }).values?.rollback;
}

function wireless_status() {
	if (!can_access(ctx.authsession, 'wireless', 'read'))
		return response(403, 'Forbidden', _('Permission denied'));
	const radios = ubus.call('network.wireless', 'status', {});
	let widths = {};
	try {
		const nl = import('nl80211');
		const channel_widths = [ 20, 20, 40, 80, '80+80', 160, 5, 10, 1, 2, 4, 8, 16, 320 ];
		const interfaces = nl.request(nl.const.NL80211_CMD_GET_INTERFACE, nl.const.NLM_F_DUMP);
		for (let iface in interfaces)
			if (iface.ifname) widths[iface.ifname] = channel_widths[iface.channel_width];
	}
	catch (e) {
		// Older wireless backends can still display hostapd and netifd state.
	}
	const result = {};
	for (let name, radio in radios) {
		const data = result[name] = {
			band: radio.config?.band,
			channel: radio.config?.channel,
			htmode: radio.config?.htmode,
			chanbw: radio.config?.chanbw,
			noscan: !!+radio.config?.noscan,
			planned_htmode: radio.channel_plan?.htmode,
			candidates: radio.channel_plan?.channels,
			pending: radio.pending,
			disabled: radio.disabled,
			failed: radio.retry_setup_failed,
			error: radio.error,
			aps: {},
			details: {}
		};
		for (let iface in radio.interfaces) {
			const is_ap = iface.config?.mode == 'ap';
			const disabled = iface.config?.disabled == '1';
			const status = is_ap && iface.ifname ? ubus.call('hostapd.' + iface.ifname, 'get_status', {}) : null;
			if (is_ap)
				data.aps[iface.section] = {
					error: iface.error,
					status: status?.status,
					channel: status?.channel,
					width: widths[iface.ifname],
					cac: status?.dfs?.cac_seconds_left,
					cac_active: !!status?.dfs?.cac_active,
					disabled
				};
			if (!radio.up || radio.pending || radio.disabled || disabled || !iface.ifname || iface.error ||
			    (is_ap && (status?.status != 'ENABLED' || status?.dfs?.cac_active)))
				continue;
			const info = ubus.call('iwinfo', 'info', { device: iface.ifname });
			if (info)
				data.details[iface.section] = {
					standard: info.hwmode && info.hwmode != 'unknown' ? uc(info.hwmode) : null,
					country: info.country,
					power: info.txpower
				};
		}
		if (length(data.aps) && radio.phy && !radio.disabled) {
			const args = { phy: radio.phy };
			if (radio.config?.radio != null) args.radio = +radio.config.radio;
			const status = ubus.call('hostapd', 'config_status', args);
			data.status = status?.status;
			data.error ??= status?.error;
		}
	}
	http.header('Cache-Control', 'no-store');
	http.prepare_content('application/json; charset=UTF-8');
	http.write_json(result);
}

return {
	action_wireless_status: wireless_status,
	action_prepare_save: () => locked(prepare_save),
	action_routed_plan: () => locked(routed_plan),
	action_apply_rollback: () => locked(() => apply(true)),
	action_apply_unchecked: () => locked(() => apply(false)),
	action_confirm: () => locked(() => {
		const pending = rollback_state();
		upstream.action_confirm();
		// Keep expectations across a timeout, which restores the staged delta.
		if (pending?.session && pending.token == http.formvalue('token') && !rollback_state()?.session) {
			clear_if_idle(pending.session);
			ubus.call('service', 'event', { type: 'router-tool.apply_done', data: {} });
		}
	}),
	action_revert: () => locked(() => {
		upstream.action_revert();
		clear_if_idle(ctx.authsession);
	})
};
