// Persist a preset's exact edits before the first UCI commit. Recovery accepts
// only the recorded old/new values, preserving unrelated edits and refusing drift.
import { open, mkdir, rename, unlink, stat } from 'fs';
import { equal, section_values, value_valid } from './snapshot.uc';

export function configuration(uci, names) {
	let result = {};
	for (let config in names ?? [ 'network', 'dhcp', 'firewall', 'wireless', 'router-tool' ])
		result[config] = uci.get_all(config) || {};
	return result;
}

export function transaction(uci, before) {
	let result = { version: 1, edits: [] };
	for (let config, old in before) {
		let after = uci.get_all(config) || {};
		for (let sid in keys(old))
			if (!after[sid] || old[sid]['.type'] != after[sid]['.type']) die('Cannot journal a section removal or type change');
		for (let sid, section in after) {
			let previous = old[sid];
			if (!previous) {
				push(result.edits, { config, sid, type: section['.type'], created: section_values(section) });
				continue;
			}
			let a = section_values(previous), b = section_values(section), edits = [];
			for (let key in uniq([ ...keys(a), ...keys(b) ]))
				if (!equal(a[key], b[key])) push(edits, { key, before: a[key], after: b[key] });
			if (length(edits)) push(result.edits, { config, sid, type: section['.type'], options: edits });
		}
	}
	return result;
}

export function save_transaction(path, state) {
	let directory = substr(path, 0, rindex(path, '/'));
	if (!stat(directory) && !mkdir(directory, 0700)) die('Cannot create transaction directory');
	let temporary = path + '.tmp';
	let file = open(temporary, 'w', 0600);
	let text = sprintf('%J\n', state);
	if (!file || file.write(text) != length(text) || !file.flush() || !file.close() || !rename(temporary, path))
		die('Cannot persist configuration transaction');
}

export function commit_transaction(state, path, before_commit, after_commit, current) {
	if (state?.version != 1 || type(state.edits) != 'array' || !length(state.edits))
		die('Cannot recover invalid configuration transaction');
	let edit_by_section = {};
	let loaded = {};
	function section(config, sid) {
		if (!exists(loaded, config)) loaded[config] = current.get_all(config) || {};
		return loaded[config][sid];
	}
	function refuse_change(message) {
		let applied = false;
		for (let edit in state.edits) {
			let item_section = section(edit.config, edit.sid);
			if (edit.created) {
				if (item_section && item_section['.type'] == edit.type && equal(section_values(item_section), edit.created)) applied = true;
			}
			else for (let option in edit.options)
				if (!equal(option.before, option.after) && equal(item_section?.[option.key], option.after)) applied = true;
		}
		if (!applied && !unlink(path)) die(message + '; unable to clear unapplied transaction');
		die(message);
	}
	// Validate the complete journal before inspecting or staging any edit.
	for (let edit in state.edits) {
		let id = `${edit.config}/${edit.sid}`;
		if (index([ 'network', 'dhcp', 'firewall', 'wireless', 'router-tool' ], edit.config) < 0 ||
			type(edit.sid) != 'string' || !match(edit.sid, /^[a-zA-Z0-9_][a-zA-Z0-9_-]*$/) ||
			type(edit.type) != 'string' || !length(edit.type) || exists(edit_by_section, id) ||
			(type(edit.created) == 'object') == (type(edit.options) == 'array'))
			die('Cannot recover invalid configuration transaction');
		edit_by_section[id] = edit;
		if (edit.created) {
			for (let key, value in edit.created)
				if (!match(key, /^[a-zA-Z0-9_]+$/) || value == null || !value_valid(value))
					die('Cannot recover invalid configuration transaction');
		}
		else {
			let options = {};
			if (!length(edit.options)) die('Cannot recover invalid configuration transaction');
			for (let o in edit.options) {
				if (type(o.key) != 'string' || !match(o.key, /^[a-zA-Z0-9_]+$/) || options[o.key] ||
					!exists(o, 'before') || !exists(o, 'after') || !value_valid(o.before) || !value_valid(o.after))
					die('Cannot recover invalid configuration transaction');
				options[o.key] = true;
			}
		}
	}
	for (let edit in state.edits) {
		let saved = section(edit.config, edit.sid);
		if (edit.created) {
			if (saved && (saved['.type'] != edit.type || !equal(section_values(saved), edit.created)))
				refuse_change(`Transaction recovery refused: user changed ${edit.config}.${edit.sid}`);
		}
		else {
			if (saved?.['.type'] != edit.type)
				refuse_change(`Transaction recovery refused: user changed ${edit.config}.${edit.sid}`);
			for (let option in edit.options) {
				let value = saved?.[option.key];
				if (!equal(value, option.before) && !equal(value, option.after))
					refuse_change(`Transaction recovery refused: user changed ${edit.config}.${edit.sid}.${option.key}`);
			}
		}
	}
	let changed = {};
	for (let edit in state.edits) {
		changed[edit.config] = true;
		if (edit.created) {
			if (!current.get_all(edit.config, edit.sid) && !current.set(edit.config, edit.sid, edit.type))
				die(`Cannot create ${edit.config}.${edit.sid}`);
			for (let key, value in edit.created)
				if (!current.set(edit.config, edit.sid, key, value)) die(`Cannot set ${edit.config}.${edit.sid}.${key}`);
		}
		else for (let option in edit.options) {
			if (equal(current.get(edit.config, edit.sid, option.key), option.after)) continue;
			let ok = option.after == null ? current.delete(edit.config, edit.sid, option.key) : current.set(edit.config, edit.sid, option.key, option.after);
			if (!ok) die(`Cannot set ${edit.config}.${edit.sid}.${option.key}`);
		}
	}
	// Persist ownership state before the corresponding network changes.
	let changed_configs = filter([ 'router-tool', 'firewall', 'dhcp', 'wireless', 'network' ], name => changed[name]);
	for (let config in changed_configs) {
		before_commit(config);
		if (!current.commit(config)) die(`Unable to commit router tool settings in ${config}`);
		after_commit(config);
	}
	if (!unlink(path)) die('Cannot remove completed configuration transaction');
	return changed_configs;
}
