#!/usr/bin/ucode
// SPDX-License-Identifier: Apache-2.0
// Adapted from zh8416's Xiaomi NFC support, immortalwrt PR #225 (cb01644).

import * as fs from 'fs';
import { cursor } from 'uci';

const CONFIG = '/etc/config/xiaomi-control';
const WIRELESS = '/etc/config/wireless';
const STATUS = '/var/run/xiaomi-nfc.json';
const EMPTY = chr(3, 0, 254, 0);
const FIELDS = { iface: '', mode: 'wifi', text: '', uri: '',
	manual_ssid: '', manual_key: '', security: 'wpa2' };
let work, uci, settings, wireless, original, radio_config, lock, bus;

// LuCI translates status messages in the browser; mark literals for i18n-scan.
function _(message) {
	return message;
}

function transfer(page, data, count) {
	let args = [ '/usr/sbin/i2ctransfer', '-y', '' + bus,
		(data == null ? 'w2' : 'w6') + '@0x54',
		sprintf('0x%02x', page >> 8), sprintf('0x%02x', page & 255) ];
	if (data == null)
		push(args, 'r' + count);
	else
		for (let i = 0; i < length(data); i++)
			push(args, sprintf('0x%02x', ord(data, i)));
	let proc = fs.popen(args, 'r');
	let output = proc ? proc.read('all') : null;
	if (!proc || proc.close() != 0)
		die(_('NFC I2C transfer failed.'));
	if (data != null) {
		sleep(10);
		return;
	}
	let bytes = split(trim(output), /\s+/);
	if (length(bytes) != count || length(filter(bytes, (v) => !match(v, /^0x[0-9a-fA-F]{2}$/))))
		die(_('Invalid NFC read response.'));
	return join('', map(bytes, (v) => chr(int(v, 16))));
}

function discover() {
	let board = trim(fs.readfile('/tmp/sysinfo/board_name') || '');
	if (!match(board, /^xiaomi,be(7000|10000)(-es)?$/))
		return false;
	// Follow the NFC node, so an unrelated device at 0x54 is never probed.
	for (let path in fs.glob('/sys/bus/i2c/devices/*-0054')) {
		if (index(split(fs.readfile(path + '/of_node/compatible') || '', '\x00'), 'miwifi,nfc') < 0)
			continue;
		let id = match(fs.basename(path), /^([0-9]+)-0054$/);
		if (!id || !fs.access('/dev/i2c-' + id[1]))
			continue;
		bus = int(id[1]);
		try {
			let cc = transfer(0, null, 4);
			if (cc == chr(0xe1, 0x43, 0x80, 9) || cc == chr(0xe1, 0x40, 0x80, 9))
				return true;
		}
		catch (e) { /* Try another declared NFC node. */ }
	}
	return false;
}

function snapshot() {
	work = fs.mkdtemp('/var/run/xiaomi-nfc.XXXXXX');
	if (!work)
		die(_('Unable to create NFC configuration snapshot.'));
	// Unique package aliases also bypass libuci's default /tmp/.uci delta path.
	settings = 'nfc_' + substr(fs.basename(work), -6);
	wireless = settings + '_wireless';
	original = fs.readfile(CONFIG);
	radio_config = fs.readfile(WIRELESS) || '';
	if (original == null || fs.writefile(work + '/' + settings, original) == null ||
	    fs.writefile(work + '/' + wireless, radio_config) == null)
		die(_('Unable to read NFC configuration.'));
	uci = cursor(work, work, '');
	if (!uci || !uci.load(settings) || !uci.load(wireless))
		die(_('Unable to parse NFC configuration.'));
}

function ap_info(ap) {
	let disabled = ap.disabled == '1';
	let radios = type(ap.device) == 'array' ? ap.device : split(ap.device || '', /\s+/);
	for (let radio in radios)
		disabled ||= !uci.get_all(wireless, radio) || uci.get(wireless, radio, 'disabled') == '1';
	let enc = ap.encryption || 'none';
	// WSC cannot advertise SAE-only or OWE; transition mode uses WPA2.
	let security = { none: [1, 1], psk: [2, 4], 'psk+tkip': [2, 4],
		psk2: [32, 8], 'psk2+ccmp': [32, 8], 'psk2+aes': [32, 8],
		'psk-mixed': [32, 8], 'psk-mixed+ccmp': [32, 8],
		'sae-mixed': [32, 8], 'sae-mixed+ccmp': [32, 8] }[enc];
	return { id: ap['.name'], ssid: ap.ssid || '', radio: join(' ', radios),
		encryption: ap.encryption || 'none', disabled,
		compatible: !disabled && length(radios) > 0 && security != null, security, key: ap.key || '' };
}

function attribute(tag, data) {
	return chr(tag >> 8, tag & 255, length(data) >> 8, length(data) & 255) + data;
}

function wifi_message(ssid, key, security) {
	if (length(ssid) < 1 || length(ssid) > 32)
		die(_('SSID must contain 1–32 UTF-8 bytes.'));
	if (security[0] == 1)
		key = '';
	else if (!((length(key) >= 8 && length(key) <= 63) || match(key, /^[0-9a-fA-F]{64}$/)))
		die(_('Invalid Wi-Fi key.'));
	return attribute(0x100e, attribute(0x1026, chr(1)) + attribute(0x1045, ssid) +
		attribute(0x1003, chr(0, security[0])) + attribute(0x100f, chr(0, security[1])) +
		attribute(0x1027, key) + attribute(0x1020, chr(255, 255, 255, 255, 255, 255)));
}

function message(cfg) {
	if (!cfg.enabled)
		return EMPTY;
	let kind = 'T', payload, header = 0xd1;
	if (cfg.mode == 'wifi') {
		let ap = uci.get_all(wireless, cfg.iface);
		ap = ap && ap['.type'] == 'wifi-iface' && ap.mode == 'ap' ? ap_info(ap) : null;
		if (!ap || !ap.compatible)
			die(_('Selected AP is unavailable or uses unsupported security.'));
		payload = wifi_message(ap.ssid, ap.key, ap.security);
	}
	else if (cfg.mode == 'manual') {
		if (cfg.security != 'none' && cfg.security != 'wpa2')
			die(_('Unsupported security.'));
		payload = wifi_message(cfg.manual_ssid, cfg.manual_key, cfg.security == 'none' ? [1, 1] : [32, 8]);
	}
	else {
		let text = cfg.mode == 'uri' ? cfg.uri : cfg.text;
		if (!length(text) || length(text) > 1900)
			die(_('Text must contain 1–1900 UTF-8 bytes.'));
		if (cfg.mode == 'uri') {
			if (!match(text, /^[a-zA-Z][a-zA-Z0-9+.-]*:/) || match(text, /[[:space:][:cntrl:]]/))
				die(_('Enter a URI, for example https://example.com'));
			kind = 'U';
			payload = chr(0) + text;
		}
		else
			payload = chr(2) + 'en' + text;
	}
	if (cfg.mode == 'wifi' || cfg.mode == 'manual') {
		kind = 'application/vnd.wfa.wsc';
		header = 0xd2;
	}
	let size = length(payload);
	let ndef = chr(size <= 255 ? header : header & ~0x10, length(kind)) +
		(size <= 255 ? chr(size) : chr(0, 0, size >> 8, size & 255)) + kind + payload;
	size = length(ndef);
	let data = chr(3) + (size < 255 ? chr(size) : chr(255, size >> 8, size & 255)) + ndef + chr(254);
	while (length(data) % 4)
		data += chr(0);
	return data;
}

function read_bytes(size) {
	let data = '';
	for (let i = 0; i < size; i += 128)
		data += transfer(1 + i / 4, null, min(128, size - i));
	return data;
}

function program(data) {
	// Publish the NDEF length only after all remaining pages are complete.
	transfer(1, EMPTY);
	for (let i = 4; i < length(data); i += 4)
		transfer(1 + i / 4, substr(data, i, 4));
	transfer(1, substr(data, 0, 4));
	if (read_bytes(length(data)) != data)
		die(_('NFC verification failed.'));
}

function check_config() {
	if (fs.readfile(CONFIG) != original || (fs.readfile(WIRELESS) || '') != radio_config)
		die(_('Configuration changed during NFC update.'));
}

function apply(cfg) {
	let data = message(cfg);
	check_config();
	let before = read_bytes(length(data));
	try {
		if (before != data)
			program(data);
		check_config();
	}
	catch (e) {
		try { if (before != data) program(before); }
		catch (rollback) { die(e.message + ' NFC rollback failed.'); }
		die(e.message);
	}
}

let status = { available: false }, rc = 0;
try {
	lock = fs.open('/var/lock/xiaomi-nfc.lock', 'a', 0o600);
	if (!lock || !lock.lock('x'))
		die(_('Unable to lock NFC update.'));
	snapshot();
	let saved = uci.get_all(settings, 'settings') || {};
	let cfg = { enabled: saved.nfc_enabled == '1' };
	for (let name, fallback in FIELDS) {
		let value = saved['nfc_' + name] ?? fallback;
		if (type(value) != 'string' || length(value) > 1900 || index(value, chr(0)) >= 0) {
			if (cfg.enabled) die('Invalid NFC option: ' + name);
			value = fallback;
		}
		cfg[name] = value;
	}
	if (index([ 'wifi', 'text', 'uri', 'manual' ], cfg.mode) < 0) {
		if (cfg.enabled) die(_('Unsupported NFC mode.'));
		cfg.mode = 'wifi';
	}
	status = { available: discover(), enabled: cfg.enabled, mode: cfg.mode };
	if (status.available) {
		// A removed/disabled AP must not leave its old credentials advertised.
		let error;
		if (cfg.enabled && cfg.mode == 'wifi') {
			try { message(cfg); }
			catch (e) { error = e.message; cfg.enabled = false; }
		}
		apply(cfg);
		if (error) die(error);
	}
}
catch (e) {
	status.error = e.message;
	rc = 1;
}
if (work) {
	for (let path in fs.glob(work + '/*')) fs.unlink(path);
	fs.rmdir(work);
}
let file = fs.open(STATUS + '.tmp', 'w', 0o600);
if (file) {
	file.write(sprintf('%J\n', status));
	file.close();
	fs.rename(STATUS + '.tmp', STATUS);
}
if (lock) lock.close();
if (status.error) warn(status.error + '\n');
exit(rc);
