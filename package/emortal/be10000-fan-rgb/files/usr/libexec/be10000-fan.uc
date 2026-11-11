#!/usr/bin/ucode

import * as fs from 'fs';
import * as uloop from 'uloop';
import { cursor } from 'uci';

const STATUS_FILE = '/var/run/fancontrol.status';
const MIN_RUNNING_PWM = 51;
const PROFILES = {
auto: {
start: 45,
full: 85,
minimum: 51,
maximum: 255,
hysteresis: 3,
interval: 5
},
quiet: {
start: 52,
full: 80,
minimum: 51,
maximum: 255,
hysteresis: 4,
interval: 5
},
balanced: {
start: 45,
full: 75,
minimum: 51,
maximum: 255,
hysteresis: 3,
interval: 5
},
performance: {
start: 38,
full: 65,
minimum: 64,
maximum: 255,
hysteresis: 2,
interval: 3
}
};

let uci = cursor();
let timer;
let config = {};
let profile_name = 'auto';
let profile = PROFILES.auto;
let fan_file;
let rpm_file;
let current_pwm = -1;
let fan_active = false;
let kicking = false;
let kick_target = 0;
let last_sample = null;
let last_status = '';

function read_number(path) {
let raw = fs.readfile(path);
if (raw == null)
return null;

raw = trim(raw);
if (!match(raw, /^-?[0-9]+$/))
return null;

return int(raw);
}

function valid_sensor(type) {
return match(type, /^(nss|ubi|cpuss|cpu[0-9]|wcss|top-glue|soc|ath11k|ath12k)/) != null ||
match(type, /^phy[0-9]+$/) != null;
}

function read_max_temperature() {
let hottest = null;
let hottest_sensor = '';
let count = 0;

for (let base in fs.glob('/sys/class/thermal/thermal_zone*')) {
let type = fs.readfile(base + '/type');
let value = read_number(base + '/temp');

if (type == null || value == null)
continue;

type = trim(type);
if (!valid_sensor(type))
continue;
if (value < 1000 || value > 150000)
continue;

count++;
if (hottest == null || value > hottest) {
hottest = value;
hottest_sensor = type;
}
}

if (hottest == null)
return null;

return {
temperature: hottest,
sensor: hottest_sensor,
count: count
};
}

function discover_fan() {
fan_file = null;
rpm_file = null;

/* Find the EMC230x client regardless of the I2C bus number.
 * The bus index differs between board DTS revisions (0-002f / 1-002f). */
for (let client in fs.glob('/sys/bus/i2c/devices/*-002f')) {
for (let base in fs.glob(client + '/hwmon/hwmon*')) {
let candidate = base + '/pwm1';

if (!fs.access(candidate))
continue;

fan_file = candidate;
if (fs.access(base + '/fan1_input'))
rpm_file = base + '/fan1_input';
return true;
}
}

return false;
}

function quantize_pwm(value) {
if (value <= 0)
return 0;
if (value >= 255)
return 255;

let step = int((value * 10 + 127) / 255);
if (step < 1)
step = 1;
let quantized = int((step * 255 + 5) / 10);
return quantized < MIN_RUNNING_PWM ? MIN_RUNNING_PWM : quantized;
}

function write_pwm(value) {
value = int(value);
if (value < 0)
value = 0;
if (value > 255)
value = 255;
value = quantize_pwm(value);

if (fan_file == null || !fs.access(fan_file)) {
if (!discover_fan())
return false;
}

let actual = read_number(fan_file);
if (actual == value) {
current_pwm = actual;
return true;
}

if (!fs.writefile(fan_file, '' + value + '\n'))
return false;

actual = read_number(fan_file);
if (actual == null || actual != value)
return false;

current_pwm = actual;
return true;
}

function reload_config() {
uci.unload('fancontrol');
config = uci.get_all('fancontrol', 'settings') || {};
profile_name = config.profile || 'auto';
profile = PROFILES[profile_name];

if (profile == null) {
profile_name = 'auto';
profile = PROFILES.auto;
}
}

function calculate_pwm(temperature) {
let start = profile.start * 1000;
let stop = (profile.start - profile.hysteresis) * 1000;
let full = profile.full * 1000;

if ((!fan_active && temperature < start) ||
    (fan_active && temperature <= stop))
return 0;
if (temperature <= start)
return profile.minimum;
if (temperature >= full)
return profile.maximum;

return int(profile.minimum +
(profile.maximum - profile.minimum) *
(temperature - start) / (full - start));
}

function save_status(sample, pwm, error, detail) {
let rpm = rpm_file == null ? null : read_number(rpm_file);
let value = sprintf(
'running=1\n' +
'fan_active=%d\n' +
'temperature_mc=%d\n' +
'pwm=%d\n' +
'rpm=%d\n' +
'thermal_file=max-thermal-zones\n' +
'fan_file=%s\n' +
'profile=%s\n' +
'hottest_sensor=%s\n' +
'sensor_count=%d\n' +
'start_temp=%d\n' +
'full_speed_temp=%d\n' +
'error=%s\n' +
'detail=%s\n',
fan_active ? 1 : 0,
sample == null ? -1 : sample.temperature,
pwm,
rpm == null ? -1 : rpm,
fan_file || '',
profile_name,
sample == null ? '' : sample.sensor,
sample == null ? 0 : sample.count,
profile.start,
profile.full,
error || '',
detail || ''
);

if (value != last_status) {
if (fs.writefile(STATUS_FILE + '.tmp', value) != null &&
    fs.rename(STATUS_FILE + '.tmp', STATUS_FILE))
last_status = value;
}
}

function update() {
reload_config();

if (kicking) {
kicking = false;
/* The old target was computed before the spin-up pulse. Never lower PWM
 * after a newly failed sensor or a temperature spike during that interval. */
last_sample = read_max_temperature();
if (last_sample == null) {
let failsafe_ok = write_pwm(255);
fan_active = true;
save_status(null, failsafe_ok ? current_pwm : (read_number(fan_file) ?? -1),
    'temperature-read-failed', failsafe_ok ? '' : 'failsafe-pwm-unconfirmed');
timer.set(1000);
return;
}
kick_target = calculate_pwm(last_sample.temperature);
if (!write_pwm(kick_target)) {
fan_active = true;
save_status(last_sample, read_number(fan_file) ?? -1, 'pwm-write-failed', '');
timer.set(1000);
return;
}

fan_active = current_pwm > 0;
save_status(last_sample, current_pwm, '', '');
timer.set(profile.interval * 1000);
return;
}

if (fan_file == null || !fs.access(fan_file)) {
if (!discover_fan()) {
fan_active = true;
save_status(null, -1, 'pwm-device-not-found', '');
timer.set(1000);
return;
}
current_pwm = read_number(fan_file);
fan_active = current_pwm != null && current_pwm > 0;
}

let sample = read_max_temperature();
last_sample = sample;

if (sample == null) {
let failsafe_ok = write_pwm(255);
fan_active = true;
save_status(null, failsafe_ok ? current_pwm : (read_number(fan_file) ?? -1),
    'temperature-read-failed', failsafe_ok ? '' : 'failsafe-pwm-unconfirmed');
timer.set(1000);
return;
}

let target = calculate_pwm(sample.temperature);

let measured_rpm = rpm_file == null ? null : read_number(rpm_file);
if (target > 0 && fan_active && current_pwm > 0 && measured_rpm != null && measured_rpm <= 0) {
if (!write_pwm(255)) {
fan_active = true;
save_status(sample, read_number(fan_file) ?? -1, 'pwm-write-failed', 'fan-stall');
timer.set(1000);
return;
}

fan_active = true;
kicking = true;
kick_target = target;
save_status(sample, current_pwm, 'fan-stall-recovery', 'rpm-zero-kick');
timer.set(500);
return;
}

if (target > 0 && !fan_active) {
if (!write_pwm(255)) {
fan_active = true;
save_status(sample, read_number(fan_file) ?? -1, 'pwm-write-failed', '');
timer.set(1000);
return;
}

fan_active = true;
kicking = true;
kick_target = target;
save_status(sample, current_pwm, '', 'startup-kick');
timer.set(500);
return;
}

if (!write_pwm(target)) {
fan_active = true;
save_status(sample, read_number(fan_file) ?? -1, 'pwm-write-failed', '');
timer.set(1000);
return;
}

fan_active = current_pwm > 0;
save_status(sample, current_pwm, '', '');
timer.set(profile.interval * 1000);
}

reload_config();
discover_fan();
if (fan_file != null) {
current_pwm = read_number(fan_file);
fan_active = current_pwm != null && current_pwm > 0;
}

uloop.init();
timer = uloop.timer(1, update);
uloop.run();
