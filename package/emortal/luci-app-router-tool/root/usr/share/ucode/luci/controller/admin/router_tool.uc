// SPDX-License-Identifier: GPL-2.0-only

import { open, stat } from 'fs';
import { cursor } from 'uci';
import { equal, section_values } from '/usr/share/router-tool/snapshot.uc';

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
	const lock = open('/var/lock/router-tool.lock', 'a');
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
	if (!can_access(sid, 'router-tool', 'write'))
		return response(403, 'Forbidden', _('Permission denied'));

	const pending = changes(sid);
	const before = baseline(sid);
	const live = {}, values = {};
	for (let name in configs) {
		live[name] = committed(name);
		if (name == 'wireless' && (!live[name] || !can_access(sid, name, 'read'))) {
			delete live[name];
			continue;
		}
		values[name] = rpc('uci', 'get', { ubus_rpc_session: sid, config: name }).values;
		if (!live[name]) die(`Cannot read committed ${name} configuration`);
	}
	// Pending edits keep their original expectations. A new save can refresh
	// the baseline once the existing delta still matches the committed values.
	const changed = length(keys(pending)) ? conflicts(before, live, pending) : [];
	if (length(changed)) return refuse(changed);
	rpc('session', 'set', { ubus_rpc_session: sid, values: { [baseline_key]: sprintf('%J', live) } });
	http.prepare_content('application/json; charset=UTF-8');
	http.write_json(values);
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

return {
	action_prepare_save: () => locked(prepare_save),
	action_apply_rollback: () => locked(() => apply(true)),
	action_apply_unchecked: () => locked(() => apply(false)),
	action_confirm: () => locked(() => {
		const pending = rollback_state();
		upstream.action_confirm();
		// Keep expectations across a timeout, which restores the staged delta.
		if (pending?.session && pending.token == http.formvalue('token') && !rollback_state()?.session)
			clear_if_idle(pending.session);
	}),
	action_revert: () => locked(() => {
		upstream.action_revert();
		clear_if_idle(ctx.authsession);
	})
};
