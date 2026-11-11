#!/usr/bin/ucode

import * as fs from 'fs';
import * as uloop from 'uloop';
import { cursor } from 'uci';

const CHANNELS = {
red: '/sys/class/leds/red:ambient/brightness',
green: '/sys/class/leds/green:ambient/brightness',
blue: '/sys/class/leds/blue:ambient/brightness'
};
const STATUS_FILE = '/var/run/be10000-rgb.status';
const VALID_MODES = {
static: true,
breathe: true,
duotone: true,
rainbow: true,
temperature: true,
cpu: true,
traffic: true
};

let uci = cursor();
let cfg = {};
let timer;
let phase = 0;
let last_cpu;
let last_traffic;
let last_status = '';
let last_rgb = [-1, -1, -1];
let last_config_time = null;
let write_failed = false;

function clamp(value, low, high) {
value = int(value);
if (value < low)
return low;
if (value > high)
return high;
return value;
}

function number_option(name, fallback, low, high) {
let value = cfg[name];
if (value == null || !match('' + value, /^-?[0-9]+$/))
value = fallback;
return clamp(value, low, high);
}

function bool_option(name, fallback) {
let value = cfg[name];
if (value == null)
return fallback;
return value == '1' || value == 'true' || value == 'yes' || value == 'on';
}

function reload_config() {
uci.unload('be10000_rgb');
cfg = uci.get_all('be10000_rgb', 'ambient') || {};
}

function color(value, fallback) {
if (type(value) != 'string' || !match(value, /^#[0-9a-fA-F]{6}$/))
value = fallback;

return [
int(substr(value, 1, 2), 16),
int(substr(value, 3, 2), 16),
int(substr(value, 5, 2), 16)
];
}

function write_rgb(rgb, level) {
level = clamp(level, 0, 255);
let scaled = [
clamp((rgb[0] * level + 127) / 255, 0, 255),
clamp((rgb[1] * level + 127) / 255, 0, 255),
clamp((rgb[2] * level + 127) / 255, 0, 255)
];

if (!write_failed && scaled[0] == last_rgb[0] &&
    scaled[1] == last_rgb[1] &&
    scaled[2] == last_rgb[2])
return scaled;

write_failed = false;
for (let i, name in ['red', 'green', 'blue']) {
if (!fs.writefile(CHANNELS[name], '' + scaled[i] + '\n'))
write_failed = true;
}
/* Retry an unchanged colour if any channel failed; don't cache a false success. */
if (!write_failed)
last_rgb = scaled;
return scaled;
}

function interpolate(a, b, amount) {
amount = clamp(amount, 0, 1000);
return [
clamp((a[0] * (1000 - amount) + b[0] * amount) / 1000, 0, 255),
clamp((a[1] * (1000 - amount) + b[1] * amount) / 1000, 0, 255),
clamp((a[2] * (1000 - amount) + b[2] * amount) / 1000, 0, 255)
];
}

function parse_time(value, fallback) {
if (type(value) != 'string' || !match(value, /^([01][0-9]|2[0-3]):[0-5][0-9]$/))
value = fallback;

let parts = split(value, ':');
let hour = clamp(parts[0], 0, 23);
let minute = clamp(parts[1], 0, 59);
return hour * 60 + minute;
}

function in_window(start, end, now) {
if (start == end)
return true;
if (start < end)
return now >= start && now < end;
return now >= start || now < end;
}

function active_level() {
let now = localtime();
let minute = now.hour * 60 + now.min;

if (bool_option('schedule_enabled', false)) {
let start = parse_time(cfg.on_time, '07:00');
let end = parse_time(cfg.off_time, '23:30');
if (!in_window(start, end, minute))
return 0;
}

let level = number_option('brightness', 96, 0, 255);
if (bool_option('night_enabled', false)) {
let start = parse_time(cfg.night_start, '22:00');
let end = parse_time(cfg.night_end, '07:00');
if (in_window(start, end, minute)) {
let night = number_option('night_brightness', 24, 0, 255);
if (night < level)
level = night;
}
}

return level;
}

function rainbow(position) {
let wheel = [
[255, 0, 0], [255, 255, 0], [0, 255, 0],
[0, 255, 255], [0, 0, 255], [255, 0, 255],
[255, 0, 0]
];
position = position % 6000;
let segment = int(position / 1000);
return interpolate(wheel[segment], wheel[segment + 1], position % 1000);
}

function read_temperature() {
let highest = null;

for (let base in fs.glob('/sys/class/thermal/thermal_zone*')) {
let raw = fs.readfile(base + '/temp');
if (raw == null || !match(trim(raw), /^-?[0-9]+$/))
continue;

let value = int(trim(raw));
if (value < 1000 || value > 150000)
continue;
value = int(value / 1000);
if (highest == null || value > highest)
highest = value;
}

return highest;
}

function temperature_color(value) {
let sensitivity = number_option('sensitivity', 5, 1, 10);
if (value == null)
return [0, 64, 255];

value += (sensitivity - 5) * 2;
if (value <= 35)
return [0, 255, 80];
if (value <= 55)
return interpolate([0, 255, 80], [255, 128, 0], (value - 35) * 50);
if (value <= 75)
return interpolate([255, 128, 0], [255, 0, 0], (value - 55) * 50);
return [255, 0, 0];
}

function cpu_usage() {
let raw = fs.readfile('/proc/stat');
if (raw == null)
return 0;

let fields = split(trim(split(raw, '\n')[0]), /[ \t]+/);
if (length(fields) < 6)
return 0;

let total = 0;
/* guest and guest_nice are already included in user/nice. */
for (let i = 1; i < length(fields) && i <= 8; i++)
total += int(fields[i]);
let idle = int(fields[4]) + int(fields[5]);
let current = [total, idle];

if (last_cpu == null) {
last_cpu = current;
return 0;
}

let delta_total = total - last_cpu[0];
let delta_idle = idle - last_cpu[1];
last_cpu = current;
if (delta_total <= 0)
return 0;

return clamp(((delta_total - delta_idle) * 1000) / delta_total, 0, 1000);
}

function status_color(value) {
value = clamp(value, 0, 1000);
if (value <= 500)
return interpolate([0, 180, 80], [255, 180, 0], value * 2);
return interpolate([255, 180, 0], [255, 0, 0], (value - 500) * 2);
}

function traffic_device() {
let section = cfg.traffic_interface || 'wan';
let device = uci.get('network', section, 'device');
if (device == null)
device = section;
if (!match(device, /^[A-Za-z0-9_+.:@-]{1,15}$/))
return null;
return device;
}

function traffic_usage() {
let device = traffic_device();
if (device == null)
return 0;
let rx = fs.readfile('/sys/class/net/' + device + '/statistics/rx_bytes');
let tx = fs.readfile('/sys/class/net/' + device + '/statistics/tx_bytes');
if (rx == null || tx == null)
return 0;

let current = [int(trim(rx)), int(trim(tx)), time(), device];
if (last_traffic == null || last_traffic[3] != device) {
last_traffic = current;
return 0;
}

let elapsed = current[2] - last_traffic[2];
if (elapsed < 1)
return null;

let bytes = (current[0] - last_traffic[0]) + (current[1] - last_traffic[1]);
last_traffic = current;
if (bytes < 0)
bytes = 0;

let sensitivity = number_option('sensitivity', 5, 1, 10);
let full_scale = 12500000 / sensitivity;
return clamp((bytes / elapsed) * 1000 / full_scale, 0, 1000);
}

function save_status(mode, level, rgb, detail) {
let value = sprintf('%J\n', {
enabled: bool_option('enabled', true),
mode: mode,
level: level,
red: rgb[0],
green: rgb[1],
blue: rgb[2],
detail: write_failed ? 'led-write-failed' : (detail || '')
});

if (value != last_status) {
if (fs.writefile(STATUS_FILE + '.tmp', value) != null &&
    fs.rename(STATUS_FILE + '.tmp', STATUS_FILE))
last_status = value;
}
}

function update() {
let now = time();
if (last_config_time == null || now != last_config_time) {
reload_config();
last_config_time = now;
}

let mode = cfg.mode || 'static';
if (!VALID_MODES[mode])
mode = 'static';

let speed = number_option('speed', 5, 1, 10);
let level = bool_option('enabled', true) ? active_level() : 0;
let rgb = [0, 0, 0];
let detail = '';

if (level > 0) {
switch (mode) {
case 'breathe': {
let minimum = number_option('minimum_brightness', 8, 0, level);
let triangle = phase % 2000;
if (triangle > 1000)
triangle = 2000 - triangle;
let breathe_level = minimum + int((level - minimum) * triangle / 1000);
rgb = write_rgb(color(cfg.color, '#0066ff'), breathe_level);
detail = 'level=' + breathe_level;
break;
}
case 'duotone': {
let amount = phase % 2000;
if (amount > 1000)
amount = 2000 - amount;
rgb = write_rgb(interpolate(
color(cfg.color, '#0066ff'),
color(cfg.second_color, '#ff00aa'),
amount
), level);
break;
}
case 'rainbow':
rgb = write_rgb(rainbow(phase * 3), level);
break;
case 'temperature': {
let temp = read_temperature();
rgb = write_rgb(temperature_color(temp), level);
detail = temp == null ? 'temperature=unknown' : 'temperature=' + temp;
break;
}
case 'cpu': {
let usage = cpu_usage();
let sensitivity = number_option('sensitivity', 5, 1, 10);
usage = clamp(usage * (50 + sensitivity * 10) / 100, 0, 1000);
rgb = write_rgb(status_color(usage), level);
detail = 'cpu=' + int(usage / 10) + '%';
break;
}
case 'traffic': {
let usage = traffic_usage();
if (usage == null)
rgb = last_rgb;
else {
if (usage <= 500)
rgb = write_rgb(interpolate([0, 100, 255], [0, 255, 180], usage * 2), level);
else
rgb = write_rgb(interpolate([0, 255, 180], [255, 0, 180], (usage - 500) * 2), level);
detail = 'traffic=' + int(usage / 10) + '%';
}
break;
}
default:
rgb = write_rgb(color(cfg.color, '#0066ff'), level);
break;
}
} else {
rgb = write_rgb([0, 0, 0], 0);
}

save_status(mode, level, rgb, detail);
phase = (phase + speed * 4) % 120000;
/* Only animation needs 10 Hz. Off/static and telemetry use one tick/second. */
timer.set(level > 0 && (mode == 'breathe' || mode == 'duotone' || mode == 'rainbow') ? 100 : 1000);
}

for (let name in CHANNELS) {
if (!fs.access(CHANNELS[name])) {
warn('be10000-rgb: missing LED channel ' + CHANNELS[name] + '\n');
exit(1);
}
}

reload_config();
uloop.init();
timer = uloop.timer(1, update);
uloop.run();
