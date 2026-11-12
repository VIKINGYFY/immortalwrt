#!/usr/bin/ucode
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from zh8416's BE10000 fan/RGB controller, immortalwrt PR #223.

import * as fs from 'fs';
import * as uloop from 'uloop';
import { cursor } from 'uci';
import { connect } from 'ubus';

const STATUS = '/var/run/xiaomi-control.json';
// start temperature, full-speed temperature, hysteresis, minimum PWM
const PROFILES = {
	auto: [45, 85, 3, 51],
	quiet: [52, 80, 4, 51],
	balanced: [45, 75, 3, 51],
	performance: [38, 65, 2, 64]
};
const MODES = [ 'static', 'breathe', 'duotone', 'rainbow', 'temperature', 'cpu', 'traffic' ];
const CHANNELS = [ 'red', 'green', 'blue' ];
const WHEEL = [ [255, 0, 0], [255, 255, 0], [0, 255, 0],
	[0, 255, 255], [0, 0, 255], [255, 0, 255], [255, 0, 0] ];

let uci = cursor();
let cfg = uci.get_all('xiaomi-control', 'settings') || {};
let profile_name = PROFILES[cfg.profile] ? cfg.profile : 'auto';
let profile = PROFILES[profile_name];
let mode = index(MODES, cfg.mode) >= 0 ? cfg.mode : 'breathe';
let fan_path, fan_timer, led_timer, status_timer, bus;
let sample, last_cpu, last_traffic, last_rgb;
let phase = 0;
let kick = false;
let fan = {};
let led = {};

function clamp(value, low, high) {
	return min(high, max(low, int(value)));
}

function number_option(name, fallback, low, high) {
	let value = cfg[name];
	return clamp(match('' + value, /^-?[0-9]+$/) ? value : fallback, low, high);
}

function enabled(name, fallback) {
	return cfg[name] == null ? fallback : index([ '1', 'true', 'yes', 'on' ], cfg[name]) >= 0;
}

function read_number(path) {
	let raw = path ? fs.readfile(path) : null;
	return raw != null && match(trim(raw), /^-?[0-9]+$/) ? int(trim(raw)) : null;
}

function read_temperature() {
	let hottest = null;
	let sensor = '';
	let count = 0;

	for (let path in fs.glob('/sys/class/thermal/thermal_zone*')) {
		let value = read_number(path + '/temp');
		if (value == null || value < 0 || value > 150000)
			continue;
		count++;
		if (hottest == null || value > hottest) {
			hottest = value;
			sensor = trim(fs.readfile(path + '/type') || '');
		}
	}

	return { temperature: hottest, sensor, count };
}

function discover_fan() {
	for (let path in fs.glob('/sys/bus/i2c/devices/*-002f/hwmon/hwmon*')) {
		if (trim(fs.readfile(path + '/name') || '') == 'emc2305' && fs.access(path + '/pwm1'))
			return path;
	}
	return null;
}

function set_pwm(value) {
	if (!fan_path || !fs.access(fan_path + '/pwm1'))
		fan_path = discover_fan();
	if (!fan_path)
		return false;

	// CONFIG_THERMAL quantizes the EMC2305 to ten cooling states.
	let step = value > 0 ? max(2, int((value * 10 + 127) / 255)) : 0;
	let target = int((step * 255 + 5) / 10);
	let path = fan_path + '/pwm1';
	if (read_number(path) != target && fs.writefile(path, target + '\n') == null)
		return false;
	let actual = read_number(path);
	// A kernel thermal cooling request may legitimately raise the output.
	return actual != null && actual >= target && actual <= 255;
}

function fan_target(temperature, active) {
	if (temperature < profile[0] * 1000 &&
	    (!active || temperature <= (profile[0] - profile[2]) * 1000))
		return 0;
	if (temperature >= profile[1] * 1000)
		return 255;
	return max(profile[3], int(profile[3] + (255 - profile[3]) *
		(temperature - profile[0] * 1000) / ((profile[1] - profile[0]) * 1000)));
}

function update_fan() {
	sample = read_temperature();
	if (!fan_path || !fs.access(fan_path + '/pwm1'))
		fan_path = discover_fan();
	let pwm = fan_path ? read_number(fan_path + '/pwm1') : null;
	let rpm = fan_path ? read_number(fan_path + '/fan1_input') : null;
	let delay = profile_name == 'performance' ? 3000 : 5000;
	let error = '';
	let target = 255;

	if (!fan_path)
		error = 'fan-unavailable';
	else if (sample.temperature == null)
		error = 'temperature-unavailable';
	else if (pwm == null || pwm < 0 || pwm > 255)
		error = 'pwm-read-failed';
	else {
		target = fan_target(sample.temperature, pwm > 0);
		if (target > 0 && (pwm == 0 || (rpm != null && rpm <= 0))) {
			target = 255;
			error = kick ? 'fan-stalled' : '';
			delay = kick ? 1000 : 500;
			kick = true;
		}
		else {
			kick = false;
		}
	}

	if (!set_pwm(target)) {
		error = error || 'pwm-write-failed';
		set_pwm(255);
	}
	if (error)
		delay = 1000;
	fan = { available: fan_path != null, profile: profile_name,
		temperature: sample.temperature, sensor: sample.sensor, sensors: sample.count,
		pwm: fan_path ? read_number(fan_path + '/pwm1') : null, rpm, error };
	fan_timer.set(delay);
}

function color(value, fallback) {
	if (type(value) != 'string' || !match(value, /^#[0-9a-fA-F]{6}$/))
		value = fallback;
	return map([ 1, 3, 5 ], (offset) => int(substr(value, offset, 2), 16));
}

function mix(a, b, amount) {
	amount = clamp(amount, 0, 1000);
	return map([ 0, 1, 2 ], (i) => int((a[i] * (1000 - amount) + b[i] * amount) / 1000));
}

function write_rgb(rgb, level) {
	let scaled = map(rgb, (v) => clamp((v * level + 127) / 255, 0, 255));
	let ok = true;
	for (let i, channel in CHANNELS) {
		let path = '/sys/class/leds/' + channel + ':ambient/brightness';
		if (last_rgb && last_rgb[i] == scaled[i] && fs.access(path))
			continue;
		if (fs.writefile(path, scaled[i] + '\n') == null)
			ok = false;
	}
	last_rgb = ok ? scaled : null;
	led.error = ok ? '' : 'led-write-failed';
}

function minute(value, fallback) {
	if (type(value) != 'string' || !match(value, /^([01][0-9]|2[0-3]):[0-5][0-9]$/))
		value = fallback;
	let parts = split(value, ':');
	return int(parts[0]) * 60 + int(parts[1]);
}

function in_window(start, end, now) {
	return start == end || (start < end ? now >= start && now < end : now >= start || now < end);
}

function brightness() {
	if (!enabled('enabled', true))
		return 0;
	let now = localtime();
	let clock = now.hour * 60 + now.min;
	if (enabled('schedule_enabled', false) &&
	    !in_window(minute(cfg.on_time, '07:00'), minute(cfg.off_time, '23:30'), clock))
		return 0;
	let level = number_option('brightness', 96, 0, 255);
	if (enabled('night_enabled', false) &&
	    in_window(minute(cfg.night_start, '22:00'), minute(cfg.night_end, '07:00'), clock))
		level = min(level, number_option('night_brightness', 24, 0, 255));
	return level;
}

function cpu_usage() {
	let fields = split(trim(split(fs.readfile('/proc/stat') || '', '\n')[0]), /\s+/);
	if (length(fields) < 9)
		return 0;
	let total = 0;
	// guest and guest_nice are already counted in user and nice.
	for (let i = 1; i <= 8; i++)
		total += int(fields[i]);
	let current = [ total, int(fields[4]) + int(fields[5]) ];
	let previous = last_cpu;
	last_cpu = current;
	return previous && total > previous[0]
		? clamp((total - previous[0] - current[1] + previous[1]) * 1000 / (total - previous[0]), 0, 1000) : 0;
}

function traffic_usage() {
	let device = cfg.traffic_interface || 'wan';
	if (!match(device, /^[A-Za-z0-9_+.:@-]{1,15}$/))
		return 0;
	// Resolve logical interfaces at runtime, including PPPoE and VLAN devices.
	try {
		bus = bus || connect();
		let info = bus ? bus.call('network.interface.' + device, 'status', {}) : null;
		device = info?.l3_device || info?.device || device;
	}
	catch (e) {
		bus = null;
	}
	if (!match(device, /^[A-Za-z0-9_+.:@-]{1,15}$/))
		return 0;
	let path = '/sys/class/net/' + device + '/statistics/';
	let rx = read_number(path + 'rx_bytes');
	let tx = read_number(path + 'tx_bytes');
	if (rx == null || tx == null) {
		last_traffic = null;
		return 0;
	}
	let current = { bytes: rx + tx, time: time(), device };
	let previous = last_traffic;
	last_traffic = current;
	if (!previous || previous.device != device || current.time <= previous.time)
		return 0;
	return clamp((current.bytes - previous.bytes) * 1000 / (current.time - previous.time) /
		(12500000 / number_option('sensitivity', 5, 1, 10)), 0, 1000);
}

function update_led() {
	let level = brightness();
	let rgb = color(cfg.color, '#0000ff');
	let amount = phase <= 1000 ? phase : 2000 - phase;
	let sensitivity = number_option('sensitivity', 5, 1, 10);
	led = { mode, brightness: level, error: '' };

	if (level > 0) {
		switch (mode) {
		case 'breathe':
			let low = number_option('minimum_brightness', 8, 0, level);
			level = low + int((level - low) * amount / 1000);
			break;
		case 'duotone':
			rgb = mix(rgb, color(cfg.second_color, '#ff00aa'), amount);
			break;
		case 'rainbow':
			let position = phase * 3;
			let segment = int(position / 1000);
			rgb = mix(WHEEL[segment], WHEEL[segment + 1], position % 1000);
			break;
		case 'temperature':
			let temp = sample?.temperature;
			if (temp == null)
				rgb = [0, 64, 255];
			else {
				temp = int(temp / 1000) + (sensitivity - 5) * 2;
				rgb = temp <= 55 ? mix([0, 255, 80], [255, 128, 0], (temp - 35) * 50)
					: mix([255, 128, 0], [255, 0, 0], (temp - 55) * 50);
			}
			break;
		case 'cpu':
		case 'traffic':
			let usage = mode == 'cpu' ? clamp(cpu_usage() * (50 + sensitivity * 10) / 100, 0, 1000) : traffic_usage();
			rgb = usage <= 500 ? mix([0, 180, 80], [255, 180, 0], usage * 2)
				: mix([255, 180, 0], [255, 0, 0], (usage - 500) * 2);
			break;
		}
	}

	write_rgb(rgb, level);
	phase = (phase + number_option('speed', 5, 1, 10) * 4) % 2000;
	led_timer.set(level > 0 && index([ 'breathe', 'duotone', 'rainbow' ], mode) >= 0 ? 100 : 1000);
}

function save_status() {
	if (fs.writefile(STATUS + '.tmp', sprintf('%J\n', { fan, led })) != null)
		fs.rename(STATUS + '.tmp', STATUS);
	status_timer.set(1000);
}

function shutdown() {
	set_pwm(255);
	last_rgb = null;
	write_rgb([0, 0, 0], 0);
	fs.unlink(STATUS);
	fs.unlink(STATUS + '.tmp');
	uloop.end();
}

if (index([ 'xiaomi,be10000', 'xiaomi,be10000-es' ], trim(fs.readfile('/tmp/sysinfo/board_name') || '')) < 0)
	exit(0);

uloop.init();
fan_timer = uloop.timer(1, () => {
	try { update_fan(); }
	catch (e) {
		set_pwm(255);
		fan.error = 'fan-control-failed';
		warn('xiaomi-control: ' + e + '\n');
		fan_timer.set(1000);
	}
});
led_timer = uloop.timer(1, () => {
	try { update_led(); }
	catch (e) {
		led.error = 'led-control-failed';
		warn('xiaomi-control: ' + e + '\n');
		led_timer.set(1000);
	}
});
status_timer = uloop.timer(10, save_status);
let signals = [ uloop.signal('SIGTERM', shutdown), uloop.signal('SIGINT', shutdown) ];
try { uloop.run(); }
catch (e) {
	shutdown();
	die(e);
}
shutdown();
