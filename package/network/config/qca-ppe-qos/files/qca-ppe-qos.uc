#!/usr/bin/ucode
// SPDX-License-Identifier: GPL-2.0-only
// BE port shaping shares the configuration and service with the AX classifier.
import * as fs from 'fs';
import { cursor } from 'uci';

const STATE = '/var/run/qca-ppe-qos-tbf.json';
const DEBUG = '/sys/kernel/debug/ppe/tbf_queues';
const HANDLE = '7ffe:';
const HW_HANDLE = '0x7ffe0000';
let uci = cursor();

function read_json(path) {
	try { return json(fs.readfile(path) || '{}'); }
	catch (e) { return {}; }
}

function quote(value) {
	return "'" + replace('' + value, "'", "'\\''") + "'";
}

function command(args) {
	let pipe = fs.popen(join(' ', map(args, quote)) + ' 2>&1', 'r');
	if (!pipe)
		return { ok: false, output: 'command-unavailable' };
	let output = trim(pipe.read('all') || '');
	return { ok: pipe.close() == 0, output };
}

function hardware() {
	let ports = {};
	let lines = split(trim(fs.readfile(DEBUG) || ''), '\n');
	let header = split(lines[0] || '', /\s+/);
	for (let line in slice(lines, 1)) {
		if (!match(line, /^[0-9]+ /))
			continue;
		let values = split(line, /\s+/);
		let row = {};
		for (let i = 0; i < length(header); i++)
			row[header[i]] = values[i];
		ports[row.port] = row;
	}
	return ports;
}

function devices() {
	let ports = [];
	for (let path in fs.glob('/sys/class/net/*')) {
		// EDMA exposes a physical PPE port directly. DSA downlinks and virtual
		// devices must not accidentally shape an entire switch trunk.
		if (fs.basename(fs.readlink(path + '/device/driver') || '') != 'qcom_ppe')
			continue;
		let name = fs.basename(path);
		let reg = fs.readfile(path + '/of_node/reg');
		if (!match(name, /^[a-zA-Z0-9_][a-zA-Z0-9_.-]{0,14}$/) || length(reg) != 4)
			continue;
		let port = 0;
		for (let i = 0; i < 4; i++)
			port = port * 256 + ord(reg, i);
		if (port > 0 && port <= 7)
			push(ports, { device: name, port, carrier: trim(fs.readfile(path + '/carrier') || '') == '1' });
	}
	return ports;
}

function root_qdisc(device, dump) {
	let result = dump != null ? { ok: true, output: dump } :
		command([ 'tc', 'qdisc', 'show', 'dev', device ]);
	if (!result.ok)
		return null;
	let root = null;
	let custom_child = false;
	for (let line in split(result.output, '\n')) {
		let fields = split(line, /\s+/);
		if (fields[0] != 'qdisc')
			continue;
		if (dump != null && fields[index(fields, 'dev') + 1] != device)
			continue;
		if (index(fields, 'root') >= 0)
			root = { kind: fields[1], handle: fields[2] };
		else if (fields[2] != '0:')
			custom_child = true;
	}
	if (root)
		root.custom_child = custom_child;
	return root;
}

function owned(root, record) {
	return record != null && root?.kind == 'tbf' && root?.handle == HANDLE;
}

function accepted(port, settings, rows) {
	let row = (rows || hardware())['' + port];
	return row?.active == '1' && row?.handle == HW_HANDLE &&
		int(row.rate_bytes_ps) == settings.rate * 125 &&
		int(row.burst_bytes) == settings.burst &&
		int(row.requested_bytes) == settings.limit && int(row.last_result) == 0;
}

function install(device, settings, replace_existing) {
	return command([ 'tc', 'qdisc', replace_existing ? 'replace' : 'add', 'dev', device,
		'root', 'handle', HANDLE, 'tbf', 'rate', settings.rate + 'kbit',
		'burst', settings.burst, 'limit', settings.limit ]);
}

function remove(device, record) {
	let root = root_qdisc(device);
	if (!root)
		return fs.access('/sys/class/net/' + device) ? 'qdisc-unavailable' : null;
	if (!owned(root, record)) {
		let row = hardware()['' + record.port];
		if (row?.active == '1' && row?.handle == HW_HANDLE)
			return 'hardware-remove-failed';
		return root.handle == HANDLE ? 'ownership-conflict' : null;
	}
	let result = command([ 'tc', 'qdisc', 'del', 'dev', device, 'root', 'handle', HANDLE ]);
	if (!result.ok)
		return 'remove-failed';
	let row = hardware()['' + record.port];
	return row?.active == '1' && row?.handle == HW_HANDLE ? 'hardware-remove-failed' : null;
}

function integer(value, fallback, low, high) {
	if (value == null || value == '')
		return fallback;
	return match('' + value, /^[0-9]+$/) && int(value) >= low && int(value) <= high ? int(value) : null;
}

function enabled(value) {
	return index([ '1', 'true', 'yes', 'on' ], value) >= 0;
}

function save(state) {
	if (fs.writefile(STATE + '.new', sprintf('%J\n', state)) == null)
		die('Cannot write PPE QoS state\n');
	fs.chmod(STATE + '.new', 0o600);
	if (!fs.rename(STATE + '.new', STATE))
		die('Cannot save PPE QoS state\n');
}

function apply(stop) {
	let state = read_json(STATE);
	let previous = state.owned || {};
	let ports = devices();
	let desired = {};
	let errors = {};
	let present = {};
	for (let port in ports)
		present[port.device] = port;
	if (!stop) {
		uci.foreach('qca-ppe-qos', 'shaper', (section) => {
			if (!enabled(section.enabled))
				return;
			let device = section.device || '';
			let port = present[device];
			let rate = integer(section.rate, null, 64, 10000000);
			let burst = integer(section.burst, 65536, 2048, 1048576);
			let limit = integer(section.limit, 131072, 4096, 4194304);
			if (!port || desired[device] || rate == null || burst == null || limit == null || limit < burst) {
				errors[section['.name']] = 'invalid-configuration';
				return;
			}
			if (!hardware()['' + port.port]) {
				errors[device] = 'hardware-status-unavailable';
				return;
			}
			desired[device] = { port: port.port, rate, burst, limit };
		});
		// Validate the complete request before changing or removing any shaper.
		if (length(errors)) {
			save({ owned: previous, errors, updated: time() });
			return false;
		}
	}
	for (let device, record in previous) {
		if (desired[device])
			continue;
		let error = remove(device, record);
		if (error)
			errors[device] = error;
		else
			delete previous[device];
	}
	let sqm_ports = {};
	uci.foreach('sqm', 'queue', (section) => {
		if (enabled(section.enabled))
			sqm_ports[split(section.interface || '', '.')[0]] = true;
	});
	for (let device, settings in desired) {
		let before = previous[device];
		let root = root_qdisc(device);
		let ours = owned(root, before);
		if (sqm_ports[device]) {
			errors[device] = 'sqm-conflict';
			continue;
		}
		if (!root || (!ours && (root.kind != 'mq' || root.handle != '0:' || root.custom_child))) {
			errors[device] = 'qdisc-conflict';
			continue;
		}
		if (ours && sprintf('%J', before) == sprintf('%J', settings) && accepted(settings.port, settings))
			continue;
		let result = install(device, settings, ours);
		if (result.ok && accepted(settings.port, settings)) {
			previous[device] = settings;
			continue;
		}
		errors[device] = result.ok ? 'hardware-rejected' : 'qdisc-rejected';
		// A successful tc exit alone may describe a software-only qdisc. Do
		// not leave it installed or claim that offloaded flows are shaped.
		if (ours && before) {
			let restored = install(device, before, true);
			if (!restored.ok || !accepted(before.port, before))
				errors[device] = 'rollback-failed';
		}
		else {
			let error = remove(device, settings);
			if (error)
				errors[device] = error;
		}
	}
	save({ owned: previous, errors, updated: time() });
	for (let device, error in errors)
		warn('PPE QoS: ' + device + ': ' + error + '\n');
	return !length(errors);
}

function status() {
	let ports = devices();
	let rows = hardware();
	let qdiscs = command([ 'tc', 'qdisc', 'show' ]);
	let state = read_json(STATE);
	let ax = fs.access('/sys/module/qca_ppe/parameters/small_pkt_len');
	for (let port in ports) {
		let row = rows['' + port.port];
		let root = qdiscs.ok ? root_qdisc(port.device, qdiscs.output) : null;
		port.available = row != null;
		port.active = row?.active == '1';
		port.managed = owned(root, state.owned?.[port.device]);
		port.rate = int(row?.rate_bytes_ps || '0') / 125;
		port.burst = int(row?.burst_bytes || '0');
		port.limit = int(row?.requested_bytes || '0');
		port.buffers = int(row?.total_buffers || '0');
		port.handle = row?.handle || '';
		port.last_result = int(row?.last_result || '0');
		port.verified = port.managed && accepted(port.port, state.owned[port.device], rows);
		port.qdisc = root?.kind || '';
		port.conflict = !port.managed && (root?.kind != 'mq' || root?.handle != '0:' || root?.custom_child);
	}
	return { platform: ax ? 'ax' : length(ports) ? 'be' : 'unsupported',
		classifier: !!ax, tbf: length(ports) > 0 && length(rows) > 0,
		ports, errors: state.errors || {}, updated: state.updated || 0 };
}

switch (ARGV[0]) {
case 'status':
	printf('%J\n', status());
	break;
case 'apply':
case 'stop':
	let lock = fs.open('/var/lock/qca-ppe-qos.lock', 'w', 0o600);
	if (!lock || lock.lock('x') !== true)
		die('Cannot lock PPE QoS configuration\n');
	exit(apply(ARGV[0] == 'stop') ? 0 : 1);
default:
	die('Usage: qca-ppe-qos.uc status|apply|stop\n');
}
