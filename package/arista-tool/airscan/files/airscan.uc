#!/usr/bin/ucode -S
// airscan: spectrum, BSS and client survey on a dedicated scan radio.
//
// Runs only on the board's dedicated scan radio (Arista C-360: the QCN9072
// multifunction card, PCI 0001:01:00.0, ath11k; Arista C-260: the QCA988x,
// PCI 0000:01:00.0, ath10k). Serving radios are never touched: no
// interface, scan, retune or spectral command is ever issued on another phy.
// The radio is taken out of netifd's control (wifi-device disabled) so that a
// "wifi reload" cannot tear down the interfaces airscan creates on it, and
// gives no country of its own: the serving radios set the AP's.
//
// Jobs, one at a time, all receive-only on the scan radio:
//   spectral  ath10k/ath11k spectral scan, 20 MHz windows on 2.4/5 GHz, 80 MHz
//             on 6 GHz (where the radio has it)
//   bss       passive scan of every channel the radio supports (2.4/5/6 GHz),
//             then of the 6 GHz channels again, long enough on each for a beacon
//   clients   monitor capture, one dwell per channel that has networks on it
//   ble       Bluetooth LE survey on the board's BLE controller (hci0), on its
//             own timer: a different radio, so it never waits for or delays
//             the Wi-Fi jobs (airscan-ble, through the kernel's mgmt socket)
// Detections (events): wired rogue (a BSS that relays our wired ARP probes but
// is not ours), evil twin (our SSID on a BSS that never relays them), own BSSID
// heard on a channel the AP is not using, deauth/disassoc floods.
// Results: ubus object "airscan" and JSON files under /tmp/airscan.
//

'use strict';

import * as fs from 'fs';
import * as uloop from 'uloop';
import * as ubus from 'ubus';
import * as uci from 'uci';
import * as struct from 'struct';
import * as socket from 'socket';
import * as nl80211 from 'nl80211';

const BOARDS = { "arista,c-230": "0000:01:00.0", "arista,c-260": "0000:01:00.0", "arista,c-360": "0001:01:00.0" };
const OUT = "/tmp/airscan";
const MON = "as-mon0", STA = "as-sta0";
const PIDFILE = "/var/run/airscan.pid";
const MYPID = +fs.readlink("/proc/self");
const FFT = "/usr/libexec/airscan-fft";
const BLE = "/usr/libexec/airscan-ble";

const LOAN_MOST = 300;     // seconds the radio is lent for at the most
const HEAL_EVERY = 60;     // seconds between two tries at binding the driver again
const HEAL_MOST = 6;       // tries in an hour at the most
const HEAL_LATER = 3600;   // seconds before another, after one that did not bring the radio back

let conn, cfg, radio = {}, state = "idle", busy = false;
let loan = { until: 0, to: null };
let heal = { count: 0, failed: 0, last: 0, next: 0, check: false, times: [], told: false,
	     unbound: null, probed: 0 };
let results = { spectrum: null, bss: null, clients: {}, ble: {} };
let last = { spectral: 0, bss: 0, clients: 0, ble: 0 };
let next = { spectral: 0, bss: 0, ble: 0 };
let errors = [], events = [], seen_events = {};
let pending = { spectral: null, bss: false };

function log(msg) {
	system(["logger", "-t", "airscan", msg]);
}

// later: fn, once, in ms. A uloop timer that has fired is not freed until it
// is cancelled, nor is all its callback holds; this one cancels itself.
function later(ms, fn) {
	return uloop.timer(ms, function() { this.cancel(); fn(); });
}

function note_error(msg) {
	log(msg);
	push(errors, { time: time(), msg });
	if (length(errors) > 20) shift(errors);
}

// one decimal place; ucode has no rounding builtin
function r1(v) {
	return int(v * 10 + (v < 0 ? -0.5 : 0.5)) / 10.0;
}

function sh(cmd) {
	return system(cmd + " >/dev/null 2>&1");
}

function write_json(name, obj) {
	fs.writefile(`${OUT}/${name}.json.tmp`, sprintf("%J", obj));
	fs.rename(`${OUT}/${name}.json.tmp`, `${OUT}/${name}.json`);
}

// Detections. Each distinct (type, bssid) is reported once per hour.
function add_event(ev) {
	let key = `${ev.type}/${ev.bssid}`;
	if (seen_events[key] && time() - seen_events[key] < 3600) return;
	for (let k, t in seen_events)
		if (time() - t >= 3600) delete seen_events[k];
	seen_events[key] = time();
	ev.time = time();
	push(events, ev);
	if (length(events) > 200) shift(events);
	let more = ev.frames ? ` (${ev.frames} frames` +
		(ev.most_to ? `, ${ev.most_frames} to ${ev.most_to} of ${ev.stations} stations` : "") +
		(ev.reasons && length(ev.reasons) ? `, reasons ${join(" ", map(keys(ev.reasons), (r) => `${r}x${ev.reasons[r]}`))}` : "") +
		")" : "";
	log(`${ev.type}: ${ev.bssid} "${ev.ssid ?? ""}" ${ev.freq} MHz${more}`);
	write_json("events", { time: time(), list: events });
}

function load_config() {
	let c = uci.cursor();
	let s = c.get_all("airscan", "main") || {};
	let list = (v) => (type(v) == "array" ? v : (v ? [ v ] : []));
	return {
		enabled: s.enabled != "0",
		spectral_interval: +(s.spectral_interval || 900),
		spectral_samples: +(s.spectral_samples || 2048),
		spectral_fft: +(s.spectral_fft || 256),
		bss_interval: +(s.bss_interval || 300),
		client_dwell: +(s.client_dwell || 1000),
		client_expire: +(s.client_expire || 900),
		wired_probe: s.wired_probe != "0",
		probe_evidence: +(s.probe_evidence || 5),
		probe_refresh: +(s.probe_refresh || 300),
		trusted_ssid: list(s.trusted_ssid),
		trusted_bssid: map(list(s.trusted_bssid), (b) => lc(b)),
		ble_enabled: s.ble_enabled != "0",
		ble_interval: +(s.ble_interval || 120),
		ble_duration: +(s.ble_duration || 10),
		ble_expire: +(s.ble_expire || 900),
	};
}

// ---------------------------------------------------------------- radio

function board_name() {
	return trim(fs.readfile("/tmp/sysinfo/board_name") || "");
}

function find_phy(pci) {
	for (let p in fs.lsdir("/sys/class/ieee80211") || []) {
		let dev = fs.readlink(`/sys/class/ieee80211/${p}/device`) || "";
		if (substr(dev, -length(pci)) == pci)
			return p;
	}
	return null;
}

// Keep netifd off the scan radio, and mark it as the scan radio (airscan
// '1'), which the init script takes away when airscan is turned off. Only the
// scan radio's own section is touched.
// The scan radio has no country option: netifd sets up a disabled radio too,
// and its country goes to the whole system (iw reg set). One unlike the
// serving radios' (OpenWrt gives a 6 GHz radio '00') switched the AP's
// regulatory domain in the middle of every boot; on a C-360 the IPQ8074
// refused it and the 5 GHz radar check failed. Without one the radio follows
// the country the serving radios set; turned off, airscan gives it theirs.
function release_from_netifd(pci) {
	if (system(["/usr/libexec/airscan-config", "claim", pci]) != 0) {
		log("scan radio configuration busy or failed; radio left with netifd");
		return false;
	}
	let c = uci.cursor();
	c.foreach("wireless", "wifi-device", (s) => {
		if (substr(s.path || "", -length(pci)) == pci && s.airscan == "1")
			radio.section = s[".name"];
	});
	if (!radio.section) return false;
	sh(`ubus call network.wireless down '{"device":"${radio.section}"}'`);
	return true;
}

function resolve_radio() {
	let pci = BOARDS[board_name()];
	if (!pci) return false;
	radio.pci = pci;
	radio.phy = find_phy(pci);
	if (!radio.phy) return false;
	// ath10k takes at most 255 samples a trigger (spectral_count), ath11k more
	radio.driver = "ath11k";
	radio.per_trigger = 0;
	for (let d in [ "ath11k", "ath10k" ])
		if (fs.access(`/sys/kernel/debug/ieee80211/${radio.phy}/${d}`)) radio.driver = d;
	if (radio.driver == "ath10k") radio.per_trigger = 255;
	radio.debugfs = `/sys/kernel/debug/ieee80211/${radio.phy}/${radio.driver}`;
	radio.spectral = !!fs.access(`${radio.debugfs}/spectral_scan_ctl`, "w");
	// ath10k-ct adds 8 dB (2.4 GHz) or 12 dB (5 GHz) to every beacon's level
	// while ofdm_peak_power_rssi is on, its default; off, they read as the
	// neighbours hear this radio. Set before every job, since a driver reload
	// turns it back on. Stock ath10k-ct also anchors a QCA988x's levels to a
	// noise floor it has only from 2 s after each firmware start, so client
	// levels (monitor dwells) read up to 20 dB under beacon levels (the start
	// of a look); the port's ath10k-ct patch (main 0022) anchors both to -95.
	if (radio.driver == "ath10k") {
		let f = `${radio.debugfs}/ofdm_peak_power_rssi`;
		if (fs.access(f, "w") && trim(fs.readfile(f) || "") != "0" && fs.writefile(f, "0\n") != null)
			log(`${radio.phy}: ofdm_peak_power_rssi turned off`);
	}
	return true;
}

function own_ifaces() {
	let out = [];
	for (let i in fs.lsdir(`/sys/class/ieee80211/${radio.phy}/device/net`) || [])
		push(out, i);
	return out;
}

function iface_up(name, kind) {
	if (!fs.access(`/sys/class/net/${name}`)) {
		if (sh(`iw phy ${radio.phy} interface add ${name} type ${kind}`) != 0) return false;
	}
	return sh(`ip link set ${name} up`) == 0;
}

function iface_del(name) {
	if (fs.access(`/sys/class/net/${name}`)) {
		sh(`ip link set ${name} down`);
		sh(`iw dev ${name} del`);
	}
}

// ---------------------------------------------------------------- spectral

function spectral_write(file, val) {
	return fs.writefile(`${radio.debugfs}/${file}`, `${val}\n`) != null;
}

// Whether the radio has 6 GHz channels at all (the C-260's QCA988x has none).
// Its own test: freqs_6g, further down, is not yet declared here.
function has_6g() {
	let p = fs.popen(`iw phy ${radio.phy} channels 2>/dev/null`), yes = false;
	if (!p) return false;
	for (let line in split(p.read("all") || "", "\n")) {
		let m = match(line, /^[ \t]*\* ([0-9]+) MHz/);
		if (m && +m[1] >= 5950) yes = true;
	}
	p.close();
	return yes;
}

// 2.4 GHz and 5 GHz: 20 MHz windows (the C-360 scan radio's 5 GHz band has no
// 40/80 MHz support). 6 GHz, on a radio with 6 GHz channels: the fourteen
// 80 MHz blocks of channels 1-221, the 40 MHz block of 225 and 229, and 233.
function spectral_windows() {
	let w = [];
	let add20 = (band, f) => push(w, { band, ctrl: f, width: 20, center: f });
	for (let f in [ 2412, 2432, 2452, 2462 ]) add20("2g", f);
	for (let r in [ [5180, 5320], [5500, 5720], [5745, 5885] ])
		for (let f = r[0]; f <= r[1]; f += 20) add20("5g", f);
	if (!has_6g()) return w;
	for (let k = 0; k < 14; k++)
		push(w, { band: "6g", ctrl: 5955 + 80 * k, width: 80, center: 5985 + 80 * k });
	push(w, { band: "6g", ctrl: 7075, width: 40, center: 7085 });
	add20("6g", 7115);
	return w;
}

// The FFT size the radio takes: ath10k at most 256 bins.
function fft_size() {
	return radio.driver == "ath10k" ? min(cfg.spectral_fft, 256) : cfg.spectral_fft;
}

function set_freq(win) {
	if (win.width == 20) return sh(`iw dev ${MON} set freq ${win.ctrl}`) == 0;
	return sh(`iw dev ${MON} set freq ${win.ctrl} ${win.width} ${win.center}`) == 0;
}

// Busy share per window: the share of samples whose in-band power is at or
// above the 802.11 preamble-detect level, -82 dBm per 20 MHz (+3 dB per
// doubling of width), from airscan-fft's power histogram (noise + rssi, the
// radio's uncalibrated in-band estimate, 1 dB steps). A floor relative to the
// window misreads the QCN9074: a quiet channel reports either exactly the
// noise value or a tight cluster 12-19 dB above it (gain state, not traffic).
// The histogram stays in the output so other thresholds can be applied.
const BUSY_DBM = -82;

function apply_busy(windows) {
	for (let w in windows) {
		let thr = BUSY_DBM + { "20": 0, "40": 3, "80": 6, "160": 9 }[`${w.width}`];
		let n = 0;
		for (let i = 0; i < length(w.pwr_hist); i++)
			if (w.pwr_lo + i >= thr) n += w.pwr_hist[i];
		w.busy_pct = r1(100.0 * n / w.samples);
	}
}

// Disable -> retune -> settle -> arm -> trigger -> wait -> disable -> drain.
// Spectral is never left armed across a retune: with QCN9074 firmware
// WLAN.HK.2.5.r4 that crashed the card (2026-10-07).
function run_spectral(done, only_band) {
	if (!radio.spectral) { done(); return; }
	if (!fs.access(FFT, "x")) { note_error(`spectral: ${FFT} missing`); done(); return; }
	if (!iface_up(MON, "monitor")) { note_error("spectral: monitor interface failed"); done(); return; }
	let wins = filter(spectral_windows(), (w) => !only_band || w.band == only_band);
	let windows = [], i = 0, skipped = [], total = 0, fft = fft_size();
	spectral_write("spectral_scan_ctl", "disable");
	spectral_write("spectral_bins", fft);
	fs.readfile(`${radio.debugfs}/spectral_scan0`);

	function finish() {
		spectral_write("spectral_scan_ctl", "disable");
		apply_busy(windows);
		for (let w in windows) w.time = time();
		// A sweep of one band replaces only that band's windows.
		let old = results.spectrum;
		// (band_of is declared further down: the bands are worked out here)
		if (only_band && old) {
			windows = [ ...filter(old.windows, (w) => w.band != only_band), ...windows ];
			skipped = [ ...filter(old.skipped || [], (f) => (f < 3000 ? "2g" : f < 5950 ? "5g" : "6g") != only_band), ...skipped ];
		}
		results.spectrum = {
			time: time(), phy: radio.phy, fft, samples_per_window: cfg.spectral_samples,
			busy_dbm_20mhz: BUSY_DBM, skipped, windows: sort(windows, (a, b) => a.center - b.center),
		};
		last.spectral = time();
		write_json("spectrum", results.spectrum);
		log(`spectral sweep${only_band ? ` (${only_band})` : ""}: ${length(wins) - length(skipped)} windows, ${total} samples`);
		// A look after every sweep: it was at a sweep that this radio went deaf.
		pending.bss = true;
		done();
	}

	function step() {
		if (i >= length(wins)) { finish(); return; }
		let w = wins[i++];
		spectral_write("spectral_scan_ctl", "disable");
		if (!set_freq(w)) {
			// 80 or 40 MHz not allowed here (e.g. a disabled edge channel): fall back to 20 MHz
			if (w.width > 20) {
				for (let k = w.width / 20 - 1; k >= 0; k--)
					splice(wins, i, 0, { band: w.band, ctrl: w.ctrl + 20 * k, width: 20, center: w.ctrl + 20 * k });
			}
			else push(skipped, w.ctrl);
			later(10, step);
			return;
		}
		// One trigger for all the window's samples, or bursts of at most
		// radio.per_trigger (ath10k: 255), each run out and disabled before
		// the next: a trigger on top of a running one loses samples (C-260).
		later(300, () => {
			let want = +cfg.spectral_samples, per = radio.per_trigger || want;
			function burst() {
				let n = min(per, want);
				want -= n;
				spectral_write("spectral_scan_ctl", "manual");
				spectral_write("spectral_count", n);
				spectral_write("spectral_scan_ctl", "trigger");
				later(int(n * 0.23) + (radio.per_trigger ? 150 : 250), () => {
					spectral_write("spectral_scan_ctl", "disable");
					if (want > 0) { burst(); return; }
					let tmp = `${OUT}/.win.json`;
					uloop.process("/bin/sh", [ "-c", `exec ${FFT} ${radio.debugfs}/spectral_scan0 > ${tmp}` ], {}, (code) => {
						let got = [];
						if (code == 0)
							try { got = json(fs.readfile(tmp) || "[]"); }
							catch (e) { note_error(`spectral: ${FFT} wrote what is not JSON: ${e.message}`); }
						fs.unlink(tmp);
						for (let wdw in got) { wdw.band = w.band; push(windows, wdw); total += wdw.samples; }
						later(10, step);
					});
				});
			}
			burst();
		});
	}
	step();
}

// ---------------------------------------------------------------- bss

// Own MACs. Interface addresses map to the frequency the interface runs on, so a
// beacon with an own BSSID on another frequency can be told apart (spoofing).
function local_macs() {
	let m = {};
	for (let p in fs.lsdir("/sys/class/ieee80211") || []) {
		let a = trim(fs.readfile(`/sys/class/ieee80211/${p}/macaddress`) || "");
		if (a) m[lc(a)] = true;
		for (let i in fs.lsdir(`/sys/class/ieee80211/${p}/device/net`) || []) {
			let b = lc(trim(fs.readfile(`/sys/class/net/${i}/address`) || ""));
			if (!b || i == MON || i == STA) continue;
			let info = fs.popen(`iw dev ${i} info 2>/dev/null`);
			let f = info ? match(info.read("all") || "", /channel [0-9]+ \(([0-9]+) MHz\)/) : null;
			if (info) info.close();
			m[b] = f ? +f[1] : true;
		}
	}
	return m;
}

// SSIDs the AP itself serves (enabled AP interfaces on the other radios).
function own_ssids() {
	let out = [], c = uci.cursor();
	c.foreach("wireless", "wifi-iface", (s) => {
		if (s.disabled == "1" || s.mode != "ap" || s.device == radio.section || !s.ssid) return;
		if (c.get("wireless", s.device, "disabled") == "1") return;
		if (index(out, s.ssid) < 0) push(out, s.ssid);
	});
	return out;
}

function chan_of(f) {
	if (f < 3000) return f == 2484 ? 14 : int((f - 2407) / 5);
	if (f < 5950) return int((f - 5000) / 5);
	return int((f - 5950) / 5);
}

function parse_iw_scan(text) {
	let list = [], cur = null, sect = "";
	for (let line in split(text, "\n")) {
		let m;
		if ((m = match(line, /^BSS ([0-9a-f:]{17})/))) {
			cur = { bssid: m[1], ssid: "", security: [], gen: "legacy", width: 20 };
			push(list, cur);
			sect = "";
			continue;
		}
		if (!cur) continue;
		let t = trim(line);
		if ((m = match(t, /^freq: ([0-9.]+)/))) { cur.freq = int(+m[1]); cur.channel = chan_of(cur.freq); }
		else if ((m = match(t, /^signal: (-?[0-9.]+) dBm/))) cur.signal = +m[1];
		// the channel the beacon says it is on: its HT operation's primary
		// channel (2.4 and 5 GHz; this image's iw does not print the DS
		// parameter set, a fuller one does)
		else if ((m = match(t, /^DS Parameter set: channel ([0-9]+)/))) cur.says_channel = +m[1];
		else if ((m = match(t, /^\* primary channel: ([0-9]+)/)) && !cur.says_channel) cur.says_channel = +m[1];
		else if ((m = match(t, /^SSID: (.*)$/))) cur.ssid = m[1];
		else if ((m = match(t, /^last seen: ([0-9]+) ms ago/))) cur.age_ms = +m[1];
		else if (match(t, /^RSN:/)) sect = "rsn";
		else if (match(t, /^WPA:/)) { sect = "wpa"; push(cur.security, "WPA"); }
		else if (sect == "rsn" && (m = match(t, /^\* Authentication suites: (.*)$/))) {
			for (let a in split(m[1], " ")) {
				let x = { "PSK": "WPA2-PSK", "SAE": "WPA3-SAE", "IEEE 802.1X": "802.1X", "802.1X": "802.1X", "FT/PSK": "FT-PSK", "FT/SAE": "FT-SAE", "OWE": "OWE" }[a];
				if (x && index(cur.security, x) < 0) push(cur.security, x);
			}
		}
		else if (match(t, /^EHT capabilities:/)) cur.gen = "wifi7";
		else if (match(t, /^HE capabilities:/) && cur.gen != "wifi7") cur.gen = "wifi6";
		else if (match(t, /^VHT capabilities:/) && !(cur.gen in [ "wifi6", "wifi7" ])) cur.gen = "wifi5";
		else if (match(t, /^HT capabilities:/) && cur.gen == "legacy") cur.gen = "wifi4";
		else if (match(t, /^\* secondary channel offset: (above|below)/) && cur.width < 40) cur.width = 40;
		else if ((m = match(t, /^\* channel width: ([0-9]) \(/))) {
			if (+m[1] == 1) cur.width = 80;
			else if (+m[1] >= 2) cur.width = 160;
		}
		else if ((m = match(t, /^\* center freq segment 2: ([0-9]+)/)) && +m[1] > 0 && cur.width == 80) cur.width = 160;
	}
	for (let b in list) if (b.freq >= 5950) b.width = null;   // 6 GHz width is in HE/EHT operation, not decoded by iw
	return list;
}

function band_of(f) {
	return f < 3000 ? "2g" : (f < 5950 ? "5g" : "6g");
}

// A passive scan listens on each 6 GHz channel for 30 ms only (ath11k's
// dwell_time_passive_6g; 150 ms on 2.4 and 5 GHz), and a beacon comes every
// 102.4 ms, so a look heard a 6 GHz network about one time in three
// (2026-10-14). The 6 GHz channels are scanned again, DWELL_6G on each: three
// tries of that each heard a second C-360's 6 GHz network close by.
const DWELL_6G = 120;

// The radio's 6 GHz channels, those not disabled.
function freqs_6g() {
	let out = [], p = fs.popen(`iw phy ${radio.phy} channels 2>/dev/null`);
	if (!p) return out;
	for (let line in split(p.read("all") || "", "\n")) {
		let m = match(line, /^[ \t]*\* ([0-9]+) MHz \[[0-9]+\](.*)$/);
		if (m && +m[1] >= 5950 && !match(m[2], /disabled/)) push(out, +m[1]);
	}
	p.close();
	return out;
}

// ---------------------------------------------------------------- wired probe

// Rogue and evil-twin test (the wired-probe method). The AP sends an ARP probe on every
// L2 domain it is on (each bridge, and each VLAN device or port that is not in
// a bridge), sourced from its own MAC. RFC 5227 probe: sender IP 0.0.0.0, so no
// ARP cache changes; target 169.254.255.<VLAN low byte>, a reserved block that
// nobody holds. A BSS that relays the probe over the air is bridged into our
// LAN: the source address of a FromDS frame is sent in the clear even under
// WPA2/WPA3. Our SSID on a BSS that never relays it (after probe_evidence
// dwells in which the BSS was heard) is an evil twin; a foreign or hidden SSID
// that relays it is a rogue on our wire. Each domain's probe is padded to
// 60 + 16 * (domain index) bytes, so the size of a relayed frame tells which
// VLAN that BSS bridges even when the frame is encrypted.
let wp = { sock: null, domains: [], macs: {}, bss: {}, ch: {} };
const LLC_ARP = "\xaa\xaa\x03\x00\x00\x00\x08\x06";   // LLC/SNAP header of an ARP frame

function l2_domains() {
	let vid = {}, out = [], macs = {};
	for (let l in split(fs.readfile("/proc/net/vlan/config") || "", "\n")) {
		let m = match(l, /^([^ |]+) *\| *([0-9]+) *\|/);
		if (m) vid[m[1]] = +m[2];
	}
	for (let d in fs.lsdir("/sys/class/net") || []) {
		let p = `/sys/class/net/${d}`;
		if (d == "lo" || fs.access(`${p}/phy80211`) || trim(fs.readfile(`${p}/type`) || "") != "1") continue;
		let mac = lc(trim(fs.readfile(`${p}/address`) || ""));
		if (mac) macs[mac] = true;
		if (fs.access(`${p}/brport`)) continue;            // its bridge carries it
		if (fs.access(`${p}/bonding_slave`)) continue;     // its bond carries it
		let st = trim(fs.readfile(`${p}/operstate`) || "");
		if (st != "up" && st != "unknown") continue;
		let v = vid[d] ?? 0;
		if (fs.access(`${p}/bridge`))
			for (let m in fs.lsdir(`${p}/brif`) || []) if (vid[m] != null) { v = vid[m]; break; }
		push(out, { dev: d, mac, vlan: v });
	}
	out = sort(out, (a, b) => (a.dev < b.dev) ? -1 : (a.dev > b.dev));
	// The network can change under us (a bond, a VLAN, a bridge's MAC): look
	// again every round, and say so when the domains change.
	if (sprintf("%J", out) != sprintf("%J", wp.domains))
		log("wired probe domains: " + join(", ", map(out, (x) => `${x.dev} (vlan ${x.vlan}, ${x.mac})`)));
	wp.domains = out;
	wp.macs = macs;
}

function send_probes() {
	l2_domains();
	if (!wp.sock) wp.sock = socket.create(socket.AF_PACKET, socket.SOCK_RAW, 0);
	if (!wp.sock) return;
	for (let k = 0; k < length(wp.domains); k++) {
		let d = wp.domains[k];
		let m = struct.pack("6B", ...map(split(d.mac, ":"), (x) => hex(x)));
		let f = struct.pack("6B", 255, 255, 255, 255, 255, 255) + m + struct.pack(">HHHBBH", 0x0806, 1, 0x0800, 6, 4, 1)
			+ m + struct.pack("4x6x") + struct.pack("4B", 169, 254, 255, d.vlan & 255);
		f += struct.pack(`${60 + 16 * k - length(f)}x`);
		wp.sock.send(f, 0, { family: socket.AF_PACKET, interface: d.dev, protocol: 0x0806, address: "ff:ff:ff:ff:ff:ff" });
	}
}

// Two BSSIDs of one physical AP: same octets 2-3, octet 1 equal unless either is
// locally administered (multi-BSSID schemes vary it), at most 4 bits apart in
// octets 4-6.
function siblings(a, b) {
	let x = map(split(a, ":"), (v) => hex(v)), y = map(split(b, ":"), (v) => hex(v));
	if (x[1] != y[1] || x[2] != y[2]) return false;
	if (!((x[0] | y[0]) & 2) && x[0] != y[0]) return false;
	let bits = 0;
	for (let i = 3; i < 6; i++)
		for (let d = x[i] ^ y[i]; d; d >>= 1) bits += d & 1;
	return bits <= 4;
}

function wired_verdict(bssid) {
	let w = wp.bss[bssid];
	if (!w) return null;
	if (w.hit_windows > 0) return true;
	return w.windows >= cfg.probe_evidence ? false : null;
}

// Whether a dwell on f sends the probe. A dwell is evidence only for the BSSes
// it hears, so a channel whose last dwell heard none still without a verdict
// is left alone, and probed again every probe_refresh seconds: a settled BSS
// can start relaying, and a new one shows in the next dwell's beacons. Each
// probe is a broadcast on every L2 domain, which an AP with multicast_to_unicast
// copies to each of its clients; probing every dwell was 2 a second, all day.
function probe_wanted(f) {
	let pc = wp.ch[f];
	if (!pc || time() - pc.at >= cfg.probe_refresh)
		return true;
	let mine = local_macs();
	for (let b in pc.heard)
		if (!(hex(substr(b, 0, 2)) & 1) && mine[b] == null && wired_verdict(b) == null)
			return true;
	return false;
}

// class: own, trusted, verified (ours, relays our probes), verified_sibling
// (ours, does not relay, but another BSSID of the same AP does: APs often
// filter broadcast ARP on one band), evil_twin, wired_rogue, spoofed (own
// BSSID on a foreign channel), unverified (ours, not enough evidence yet),
// neighbor.
//
// An own BSSID heard on another frequency is spoofed, but not:
// - where its beacon says the channel the AP really serves it on: that is the
//   AP's own beacon, heard off its channel by a scan radio a few centimetres
//   from the serving antennas (the C-260's: channel 11 heard on 6, 36 on 56),
//   not a copy sent on the channel it was heard on;
// - on the frequency the AP had it on when the look began (look_mine): the
//   AP moved that radio during the look (a channel switch);
// - where the entry is older than the look: cfg80211 keeps what it heard for
//   30 s, so a look just after a channel switch still lists the old one.
let look_mine = {}, look_secs = 0;

// trusted_bssid: the BSSID is in trusted_bssid (an entry may be a prefix).
function trusted_bssid(bssid) {
	return length(filter(cfg.trusted_bssid, (p) => substr(bssid, 0, length(p)) == p)) > 0;
}

function classify(list) {
	let mine = local_macs();
	let ssids = [ ...cfg.trusted_ssid, ...own_ssids() ];
	let wired_bssids = filter(keys(wp.bss), (x) => wp.bss[x].hit_windows > 0);
	for (let b in list) {
		let w = wp.bss[b.bssid];
		let ours = !b.hidden && index(ssids, b.ssid) >= 0;
		let at = mine[b.bssid], was = look_mine[b.bssid];
		let freqs = filter([ at, was ], (f) => type(f) == "int");
		let stale = b.age_ms != null && b.age_ms > (look_secs + 2) * 1000;
		b.own = at != null || was != null;
		b.off_channel = b.own && length(freqs) > 0 && !(b.freq in freqs) && b.says_channel != null &&
			length(filter(freqs, (f) => band_of(b.freq) == band_of(f) && b.says_channel == chan_of(f))) > 0;
		b.spoofed = b.own && length(freqs) > 0 && !(b.freq in freqs) && !b.off_channel && !stale;
		b.wired = cfg.wired_probe ? wired_verdict(b.bssid) : null;
		b.probe_windows = w?.windows || 0;
		if (w?.vlans) b.wired_vlans = w.vlans;
		if (b.spoofed) b.class = "spoofed";
		else if (b.own) b.class = "own";
		else if (trusted_bssid(b.bssid)) b.class = "trusted";
		else if (b.wired === true) b.class = ours ? "verified" : "wired_rogue";
		else if (ours) {
			let sib = filter(wired_bssids, (x) => siblings(x, b.bssid))[0];
			if (sib) { b.class = "verified_sibling"; b.sibling_of = sib; }
			else b.class = (b.wired === false || !cfg.wired_probe) ? "evil_twin" : "unverified";
		}
		else b.class = "neighbor";
		b.trusted = b.class in [ "own", "trusted", "verified" ];
		b.rogue_candidate = b.class in [ "spoofed", "evil_twin", "wired_rogue" ];
		if (b.rogue_candidate)
			add_event({ type: b.class, bssid: b.bssid, ssid: b.ssid, freq: b.freq, signal: b.signal, vlans: w?.vlans });
	}
}

function reclassify() {
	if (!results.bss) return;
	let list = results.bss.list;
	classify(list);
	let unlisted = [], mine = local_macs();
	// a BSSID that relays our probes but was not heard in the scan: our own,
	// a trusted one, the sibling of a trusted one heard, or a wired rogue
	for (let b, w in wp.bss) {
		if (!w.hit_windows || length(filter(list, (x) => x.bssid == b))) continue;
		let sib = null, cls;
		if (mine[b] != null) cls = "own";
		else if (trusted_bssid(b)) cls = "trusted";
		else {
			sib = filter(list, (x) => x.trusted && siblings(x.bssid, b))[0];
			cls = sib ? "verified_sibling" : "wired_rogue";
		}
		push(unlisted, { bssid: b, freq: w.freq, vlans: w.vlans, sibling_of: sib?.bssid, class: cls });
		if (cls == "wired_rogue") add_event({ type: "wired_rogue", bssid: b, ssid: null, freq: w.freq, vlans: w.vlans });
	}
	results.bss.unlisted_wired = unlisted;
	write_json("bss", results.bss);
}

// on_air says whether the AP itself is on the air on another radio. Then a
// look that hears nothing, not even that, is a deaf radio and not a quiet
// place.
function on_air() {
	for (let mac, f in local_macs())
		if (type(f) == "int") return true;
	return false;
}

let heal_radio;   // assigned below: it closes the capture, which is declared further down
let busy_looks = 0;   // looks in a row that heard nothing and had a scan pass refused (B142)

function run_bss(done) {
	cfg = load_config();
	iface_del(MON);
	if (!iface_up(STA, "managed")) { note_error("bss: station interface failed"); done(); return; }
	let tmp = `${OUT}/.scan.txt`;
	// Every channel, then the 6 GHz ones again, DWELL_6G on each. What the
	// second pass hears on 6 GHz stands for what the first did.
	let six = freqs_6g(), passes = [ "scan passive" ], heard = {};
	if (length(six)) push(passes, `scan freq ${join(" ", six)} duration ${DWELL_6G} passive`);
	// where the AP's own interfaces were as the look began (classify)
	let began = time(), before = local_macs();

	function looked() {
		busy_looks = 0;
		iface_del(STA);
		look_mine = before;
		look_secs = time() - began;
		let list = values(heard);
		for (let b in list) {
			b.band = band_of(b.freq);
			b.hidden = b.ssid == "" || match(b.ssid, /^(\\x00)+$/) != null;
			if (!length(b.security)) push(b.security, "open");
		}
		// Nothing heard where the last look heard networks, or where the AP
		// itself is on the air: the radio is deaf.
		let deaf = !length(list) && ((results.bss?.count || 0) >= 3 || on_air());
		let was = results.bss?.count || 0;
		l2_domains();
		classify(list);
		results.bss = { time: time(), phy: radio.phy, count: length(list), list: sort(list, (a, b) => (b.signal || -200) - (a.signal || -200)) };
		last.bss = time();
		write_json("bss", results.bss);
		let rogues = filter(list, (b) => b.rogue_candidate);
		log(`bss scan: ${length(list)} networks, ${length(rogues)} rogue candidates`);
		if (heal.check) {
			heal.check = false;
			if (length(list)) { heal.failed = 0; heal.told = false; log(`the scan radio hears again, its driver bound again: ${length(list)} networks`); }
			else { heal.failed++; heal.next = time() + HEAL_LATER; note_error("binding its driver again did not bring the scan radio back"); }
			done();
			return;
		}
		if (deaf) {
			// the count the next look is held against is the last one that heard
			results.bss.count_before = was;
			heal_radio(done);
			return;
		}
		done();
	}

	function pass(i) {
		if (i >= length(passes)) { looked(); return; }
		uloop.process("/bin/sh", [ "-c", `exec iw dev ${STA} ${passes[i]} > ${tmp} 2>&1` ], {}, (code) => {
			let text = fs.readfile(tmp) || "";
			fs.unlink(tmp);
			if (code != 0) {
				note_error(`bss: scan exited ${code}: ${substr(text, 0, 120)}`);
				// A failed 6 GHz pass leaves the look what the first heard. A look
				// that heard nothing and had a pass refused (-EBUSY: a scan still
				// running on the radio, as right after boot) says the radio was busy,
				// not that it is deaf: no verdict from it, unless the next is the same.
				if (i > 0 && (length(heard) || ++busy_looks >= 2)) { looked(); return; }
				iface_del(STA);
				done();
				return;
			}
			for (let b in parse_iw_scan(text))
				if (i == 0 || b.freq >= 5950) heard[b.bssid] = b;
			pass(i + 1);
		});
	}
	pass(0);
}

// ---------------------------------------------------------------- clients

let cap = { sock: null, handle: null, freq: 0, ch_list: [], ch_i: 0, deauth: {}, heard: {}, hit: {}, sig: {}, probed: 0, round_heard: 0, round_dwells: 0 };
const DEAUTH_FLOOD = 20;   // deauth + disassoc frames for one BSSID per second of dwell

// radiotap flags (bit 1): 0x10 = frame carries its FCS, 0x40 = its FCS failed
function radiotap_flags(pkt) {
	let present = struct.unpack("<I", pkt, 4)[0], off = 8, p = present;
	while (p & 0x80000000) { p = struct.unpack("<I", pkt, off)[0]; off += 4; }
	if (!(present & 0x02)) return 0;
	if (present & 0x01) off = int((off + 7) / 8) * 8 + 8;
	return ord(pkt, off);
}

function radiotap_fcs(pkt) {
	return !!(radiotap_flags(pkt) & 0x10);
}

function radiotap_signal(pkt) {
	// walk the first presence word up to bit 5 (dBm antenna signal)
	let present = struct.unpack("<I", pkt, 4)[0], off = 8, p = present;
	while (p & 0x80000000) { p = struct.unpack("<I", pkt, off)[0]; off += 4; }
	if (!(present & 0x20)) return null;
	let sizes = [ [8, 8], [1, 1], [1, 1], [4, 2], [2, 1] ];
	for (let bit = 0; bit < 5; bit++) {
		if (!(present & (1 << bit))) continue;
		let a = sizes[bit][1];
		off = int((off + a - 1) / a) * a;
		off += sizes[bit][0];
	}
	return struct.unpack("b", pkt, off)[0];
}

function mac_at(pkt, off) {
	return join(":", map(struct.unpack("6B", pkt, off), (b) => sprintf("%02x", b)));
}

function on_frame(pkt) {
	if (length(pkt) < 8) return;
	let rlen = struct.unpack("<H", pkt, 2)[0];
	if (length(pkt) < rlen + 24) return;
	// A frame whose FCS failed reaches the monitor too (about 6% of them on
	// a C-360), its addresses as damaged as the rest: each would be a new
	// "client", or a BSSID a bit off a real one.
	if (radiotap_flags(pkt) & 0x40) return;
	let fc = struct.unpack("<H", pkt, rlen)[0];
	let ftype = (fc >> 2) & 3, sub = (fc >> 4) & 15, to_ds = !!(fc & 0x100), from_ds = !!(fc & 0x200);
	let sta = null, bssid = null, probe = null, tx_is_sta = false, probe_req = false;
	if (cap.freq && ftype == 0 && sub == 8)                // beacon: this BSS was audible
		cap.heard[mac_at(pkt, rlen + 16)] = true;
	if (cap.freq && ftype == 2 && from_ds && (!to_ds || length(pkt) >= rlen + 30)) {
		let sa = mac_at(pkt, rlen + (to_ds ? 24 : 16));
		if (wp.macs[sa]) {
			// our wired frame relayed by this BSS; its size names the domain (VLAN):
			// body = LLC/SNAP (8) + Ethernet payload (60 + 16k - 14) + cipher overhead
			let h = rlen + 24 + (to_ds ? 6 : 0) + ((sub & 8) ? 2 : 0) + (((sub & 8) && (fc & 0x8000)) ? 4 : 0);
			let body = length(pkt) - h - (radiotap_fcs(pkt) ? 4 : 0), v = true;
			let overheads = (fc & 0x4000) ? [ 16, 24, 20 ] : [ 0 ];   // CCMP-128, GCMP/CCMP-256, TKIP
			for (let o in overheads) {
				let eth = body - o - 8 + 14;
				if (eth >= 60 && (eth - 60) % 16 == 0 && (eth - 60) / 16 < length(wp.domains)) { v = wp.domains[(eth - 60) / 16].vlan; break; }
			}
			if (!(fc & 0x4000) && substr(pkt, h, 8) != LLC_ARP) v = true;   // open frame, not our ARP
			let bs = mac_at(pkt, rlen + 10);
			if (cap.hit[bs] == null || cap.hit[bs] === true) cap.hit[bs] = v;
		}
	}
	// Deauthentication and disassociation sent as the AP (transmitter = BSSID:
	// the AP itself, or someone spoofing it), first transmissions only. A
	// client leaving sends them too, with its own address, and a frame nobody
	// acknowledged comes again with the Retry bit: neither is a flood. The
	// reason code (when the frame is not protected) and the addressees tell a
	// stuck client (reason 7 to one station, again and again) from an attack.
	if (ftype == 0 && (sub == 10 || sub == 12) && cap.freq && !(fc & 0x0800) &&
	    length(pkt) >= rlen + 24 && mac_at(pkt, rlen + 10) == mac_at(pkt, rlen + 16)) {
		let d = cap.deauth[mac_at(pkt, rlen + 16)] ||= { frames: 0, broadcast: 0, reasons: {}, to: {} };
		let ra = mac_at(pkt, rlen + 4);
		d.frames++;
		if (ra == "ff:ff:ff:ff:ff:ff") d.broadcast++;
		else d.to[ra] = (d.to[ra] || 0) + 1;
		if (!(fc & 0x4000) && length(pkt) >= rlen + 26) {
			let r = `${ord(pkt, rlen + 24) | (ord(pkt, rlen + 25) << 8)}`;
			d.reasons[r] = (d.reasons[r] || 0) + 1;
		}
	}
	if (ftype == 0) {
		if (sub == 4) {                                   // probe request
			sta = mac_at(pkt, rlen + 10); tx_is_sta = true; probe_req = true;
			let ie = rlen + 24;
			if (length(pkt) >= ie + 2 && ord(pkt, ie) == 0) {
				let l = ord(pkt, ie + 1);
				if (l > 0) probe = substr(pkt, ie + 2, l);
			}
		}
		else if (sub in [ 0, 2, 10, 11, 12 ]) {          // (re)assoc req, disassoc, auth, deauth
			let a2 = mac_at(pkt, rlen + 10), a3 = mac_at(pkt, rlen + 16);
			if (a2 != a3) { sta = a2; bssid = a3; tx_is_sta = true; }
		}
	}
	else if (ftype == 2 && to_ds != from_ds) {
		if (to_ds) { sta = mac_at(pkt, rlen + 10); bssid = mac_at(pkt, rlen + 4); tx_is_sta = true; }
		else { sta = mac_at(pkt, rlen + 4); bssid = mac_at(pkt, rlen + 10); }
	}
	if (!sta || (hex(substr(sta, 0, 2)) & 1)) return;    // multicast / broadcast
	let c = results.clients[sta] ||= {
		mac: sta, first_seen: time(), frames: 0, randomized: !!(hex(substr(sta, 0, 2)) & 2), probes: [],
	};
	c.last_seen = time();
	c.frames++;
	if (cap.freq) c.freq = cap.freq;    // between dwells the channel is not one of ours
	if (bssid && !(hex(substr(bssid, 0, 2)) & 1)) c.bssid = bssid;
	if (tx_is_sta && cap.freq) {
		let s = radiotap_signal(pkt);
		// Not measurements: a level above -15 dBm is the AP's own radio on the same channel
		// transmitting into the scan radio's reading (Garage, 2026-11-05: -6 dBm from one frame
		// on its own 5 GHz channel), and -95 is ath11k's default noise floor, which every
		// QCN9074 monitor frame carried before port 0092 (B156).
		if (s != null && s < -15 && !(s == -95 && radio.driver == "ath11k")) {
			let d = cap.sig[sta] ||= { sum: 0, n: 0 };
			d.sum += s;
			d.n++;
		}
	}
	// every probe request counts, wildcard ones too (most phones name no SSID):
	// a client that only probes is looking for a network, connected to none
	if (probe_req) { c.probe_reqs = (c.probe_reqs || 0) + 1; c.last_probe = time(); }
	if (probe && index(c.probes, probe) < 0 && length(c.probes) < 10) push(c.probes, probe);
}

function capture_open() {
	if (cap.sock) return true;
	let s = socket.create(socket.AF_PACKET, socket.SOCK_RAW | socket.SOCK_NONBLOCK, 0x0300);
	if (!s || !s.bind({ family: socket.AF_PACKET, protocol: 3, address: "00:00:00:00:00:00", interface: MON })) {
		note_error(`clients: capture socket failed: ${socket.error()}`);
		return false;
	}
	cap.sock = s;
	cap.handle = uloop.handle(s, () => {
		for (let n = 0; n < 256; n++) {
			let pkt = s.recv(2048);
			if (!pkt) break;
			on_frame(pkt);
		}
	}, uloop.ULOOP_READ);
	return true;
}

function capture_close() {
	if (cap.handle) { cap.handle.delete(); cap.handle = null; }
	if (cap.sock) { cap.sock.close(); cap.sock = null; }
}

function client_channels() {
	let seen = {}, out = [];
	for (let b in (results.bss?.list || []))
		if (b.freq && !seen[b.freq]) { seen[b.freq] = true; push(out, b.freq); }
	if (!length(out)) out = [ 2412, 2437, 2462, 5180, 5745 ];
	return sort(out, (a, b) => a - b);
}

function expire_clients() {
	let now = time();
	for (let m, c in results.clients)
		if (now - c.last_seen > cfg.client_expire) delete results.clients[m];
}

function clients_report() {
	let list = values(results.clients);
	let bssids = {};
	for (let b in (results.bss?.list || [])) bssids[b.bssid] = b.ssid;
	// the network of the BSSID it was last seen with, or none the look knows
	for (let c in list) c.ssid = c.bssid ? bssids[c.bssid] : null;
	return { time: time(), phy: radio.phy, count: length(list),
		probing: length(filter(list, (c) => !c.bssid)),
		list: sort(list, (a, b) => b.last_seen - a.last_seen) };
}

// One dwell on the next channel; the round ends after every channel was visited.
function run_client_slice(done) {
	if (!iface_up(MON, "monitor") || !capture_open()) { done(); return; }
	if (cap.ch_i >= length(cap.ch_list)) { cap.ch_list = client_channels(); cap.ch_i = 0; cap.round_heard = 0; cap.round_dwells = 0; }
	let f = cap.ch_list[cap.ch_i++];
	if (radio.spectral) spectral_write("spectral_scan_ctl", "disable");
	if (sh(`iw dev ${MON} set freq ${f}`) != 0) { later(10, done); return; }
	cap.freq = f;
	cap.deauth = {};
	cap.heard = {};
	cap.hit = {};
	cap.sig = {};
	cap.probed = 0;
	if (cfg.wired_probe && probe_wanted(f))
		for (let t in [ 50, int(cfg.client_dwell / 2) ])
			later(t, () => { if (cap.freq == f) { send_probes(); cap.probed++; } });
	later(cfg.client_dwell, () => {
		cap.freq = 0;
		// A client's level is the mean of one dwell, folded in once a dwell: a
		// QCA9882 sets its receive gain anew on each visit to a channel (up to
		// 5 dB apart at 2437/2462), so a level that followed every frame would
		// be whatever the last visit happened to be.
		for (let m, d in cap.sig) {
			let c = results.clients[m];
			if (c) c.signal = int((c.signal == null ? d.sum / d.n : 0.7 * c.signal + 0.3 * d.sum / d.n) - 0.5);
		}
		cap.sig = {};
		for (let b, d in cap.deauth) {
			if (d.frames < DEAUTH_FLOOD * cfg.client_dwell / 1000.0) continue;
			let known = filter(results.bss?.list || [], (x) => x.bssid == b)[0];
			let to = sort(keys(d.to), (x, y) => d.to[y] - d.to[x]);
			add_event({ type: "deauth_flood", bssid: b, ssid: known?.ssid, freq: f, frames: d.frames, broadcast: d.broadcast,
				reasons: d.reasons, stations: length(to), most_to: to[0], most_frames: to[0] ? d.to[to[0]] : 0 });
		}
		if (cap.probed) {
			let mine = local_macs();
			for (let b in keys({ ...cap.heard, ...cap.hit })) {
				if ((hex(substr(b, 0, 2)) & 1) || mine[b] != null) continue;
				let w = wp.bss[b] ||= { windows: 0, hit_windows: 0 };
				w.windows++;
				w.seen = time();
				if (cap.hit[b] != null) {
					w.hit_windows++;
					w.last_hit = time();
					if (type(cap.hit[b]) == "int") {
						w.vlans ||= [];
						if (index(w.vlans, cap.hit[b]) < 0) push(w.vlans, cap.hit[b]);
						w.vlan = cap.hit[b];
					}
					w.freq = f;
				}
			}
		}
		// what this channel holds, for whether its next dwell probes
		let pc = wp.ch[f] ||= { at: 0 };
		pc.heard = keys(cap.heard);
		if (cap.probed)
			pc.at = time();
		cap.round_heard += length(cap.heard);
		cap.round_dwells++;
		if (cap.ch_i >= length(cap.ch_list)) {
			// A whole round with no beacon on any channel that had networks:
			// a look says whether the radio is deaf.
			if (!cap.round_heard && cap.round_dwells >= length(cap.ch_list) && length(cap.ch_list) >= 3 && (results.bss?.count || 0) >= 3) {
				log("no beacon heard in a whole round of the channels: a look, to see whether the scan radio is deaf");
				pending.bss = true;
			}
			cap.round_heard = 0;
			cap.round_dwells = 0;
			expire_clients();
			for (let b, w in wp.bss) if (time() - w.seen > 3600) delete wp.bss[b];
			reclassify();
			last.clients = time();
			write_json("clients", clients_report());
		}
		done();
	});
}

// ---------------------------------------------------------------- heal

// heal_radio binds the scan radio's driver again, which is what brings a
// deaf one back: the firmware starts afresh. Its phy comes back under a new
// name, which run_next finds by where the radio is. The look that follows
// says whether it worked. A radio that goes deaf again is bound again, but
// not within HEAL_EVERY seconds, not more than HEAL_MOST times an hour, and
// after a try that did not work, not for HEAL_LATER: binding a driver over
// and over helps no one.
heal_radio = function(done) {
	let now = time();
	heal.times = filter(heal.times, (t) => now - t < 3600);
	if (now < heal.next || length(heal.times) >= HEAL_MOST) {
		if (!heal.told) note_error("the scan radio hears nothing, and its driver was bound again too lately to do it once more yet");
		heal.told = true;
		done();
		return;
	}
	let drv = fs.readlink(`/sys/bus/pci/devices/${radio.pci}/driver`);
	if (!drv) { note_error("the scan radio hears nothing, and has no driver to bind again"); heal.next = now + HEAL_LATER; done(); return; }
	let dir = `/sys/bus/pci/drivers/${fs.basename(drv)}`;
	heal.count++;
	heal.last = now;
	heal.next = now + HEAL_EVERY;
	push(heal.times, now);
	heal.check = true;
	note_error("the scan radio hears nothing: binding its driver again");
	state = "healing";
	capture_close();
	if (radio.spectral) spectral_write("spectral_scan_ctl", "disable");
	for (let i in own_ifaces()) iface_del(i);
	fs.writefile(`${dir}/unbind`, radio.pci);
	heal.unbound = dir;
	let tries = 0;
	function wait() {
		// its phy is there once the firmware has started, about ten seconds
		if (find_phy(radio.pci) || ++tries > 40) {
			pending.bss = true;
			later(2000, done);
			return;
		}
		later(1000, wait);
	}
	later(3000, () => {
		fs.writefile(`${dir}/bind`, radio.pci);
		heal.unbound = null;
		later(1000, wait);
	});
};


// ---------------------------------------------------------------- ble

// Devices are merged across runs by address: fields from the latest run
// overwrite older ones (a name only sent in scan responses is kept until a
// newer one arrives); first_seen / last_seen / seen_total span runs; a device
// not heard for ble_expire seconds is dropped. Random resolvable addresses
// rotate (about every 15 minutes on phones), so one phone can appear as
// several entries over time.
let ble = { running: false, timer: null, error: null, rounds: 0, duration: 0, hci: null };

// The board's BLE controller: the lowest-numbered one there is (hci0 unless
// another was there first).
function ble_controller() {
	let n = filter(map(fs.lsdir("/sys/class/bluetooth") || [], (h) => match(h, /^hci([0-9]+)$/)), (m) => m);
	return length(n) ? "hci" + sort(map(n, (m) => +m[1]), (a, b) => a - b)[0] : null;
}

function ble_report() {
	let list = values(results.ble);
	return {
		time: last.ble, controller: ble.hci, error: ble.error, duration: ble.duration,
		count: length(list),
		trackers: length(filter(list, (d) => d.tracker)),
		list: sort(list, (a, b) => (b.rssi ?? -200) - (a.rssi ?? -200)),
	};
}

let run_ble;   // assigned below: ble_schedule and run_ble refer to each other

function ble_schedule(ms) {
	if (ble.timer) ble.timer.cancel();
	next.ble = time() + int(ms / 1000);
	ble.timer = uloop.timer(ms, run_ble);
}

run_ble = function() {
	if (ble.running) return;
	if (!cfg.ble_enabled) { ble.error = "disabled"; ble_schedule(60000); return; }
	ble.hci = ble_controller();
	if (!ble.hci || !fs.access(BLE, "x")) {
		ble.error = "no BLE controller";
		ble_schedule(cfg.ble_interval * 1000);
		return;
	}
	ble.running = true;
	let tmp = `${OUT}/.ble.json`;
	uloop.process("/bin/sh", [ "-c", `exec ${BLE} -i ${substr(ble.hci, 3)} -d ${cfg.ble_duration} > ${tmp} 2>/dev/null` ], {}, (code) => {
		ble.running = false;
		let r = null;
		if (code == 0)
			try { r = json(fs.readfile(tmp) || "null"); }
			catch (e) { note_error(`ble: ${BLE} wrote what is not JSON: ${e.message}`); }
		fs.unlink(tmp);
		let now = time();
		if (!r) {
			ble.error = `airscan-ble exited ${code}`;
		} else {
			ble.error = r.error;
			ble.rounds = r.rounds;
			ble.duration = r.duration;
			for (let d in r.devices || []) {
				let rec = results.ble[d.addr] ||= { first_seen: now, seen_total: 0 };
				for (let k, v in d) rec[k] = v;
				rec.seen_total += d.seen;
				rec.last_seen = now;
			}
		}
		for (let a, d in results.ble)
			if (now - d.last_seen > cfg.ble_expire) delete results.ble[a];
		last.ble = now;
		write_json("ble", ble_report());
		ble_schedule(cfg.ble_interval * 1000);
	});
};

// ---------------------------------------------------------------- scheduler

// ---------------------------------------------------------------- DFS pre-check
//
// precheck: a radar check (CAC) on the scan radio for a 5 GHz chandef a serving
// radio wants, so that it can move there by an announced channel switch (CSA)
// instead of going silent for its own 60 s check. cfg80211 hands a finished
// check to every radio of the same DFS region (regulatory_propagate_dfs_state),
// and the serving radio's hostapd marks the channels available. Under FCC a
// checked channel no radio is beaconing on lapses back to "usable" 2 s later
// (REG_PRE_CAC_EXPIRY_GRACE_MS), so once the check is done the scan radio holds
// the block with a hidden beacon: zero-length SSID, no hostapd behind it, so
// nothing can join. The hold ends after `hold` s, on precheck_done, or when the
// serving radio has moved (it then keeps the block available by beaconing).
// A radar during the check marks the channels unavailable for 30 min on every
// radio, the same way. Each step is a ubus event "airscan.precheck" with the
// caller's id and the chandef: cac, available, radar, ended. The radio is taken
// through the loan machinery, so the jobs pause and resume as for a lend.
// Proven on C360-AP (QCN9072, 80 and 160 MHz) and C-260 (QCA988x, 80 MHz),
// 2026-11-06: build/dfs-precheck/FINDINGS.md.
const PRE = "as-pre0";
const PRE_HOLD_MOST = 120;
const NL_WIDTH = { "20": 1, "40": 2, "80": 3, "160": 5 };  // nl80211_chan_width
let pre = null;

// loan_end takes the radio back: what the borrower left on it is removed. (Here,
// above the pre-check's functions: ucode does not hoist function declarations.)
function loan_end(why) {
	if (pre) {
		// a reclaim or a loan that ran out during a pre-check ends it first
		let p = pre;
		pre = null;
		if (p.timer) p.timer.cancel();
		if (p.held) nl80211.request(nl80211.const.NL80211_CMD_STOP_AP ?? 16, 0, { dev: PRE });
		conn.event("airscan.precheck", { id: p.id, state: "ended", why, freq: p.freq,
			center_freq1: p.center_freq1, width: p.width, phy: radio.phy, time: time() });
	}
	for (let i in own_ifaces())
		if (i != MON && i != STA) iface_del(i);
	log(`the scan radio is back from ${loan.to}: ${why}`);
	loan.until = 0;
	loan.to = null;
}

function pre_freqs(p) {
	let out = [];
	for (let f = p.center_freq1 - p.width / 2 + 10; f < p.center_freq1 + p.width / 2; f += 20)
		push(out, f);
	return out;
}

// the DFS state of each 20 MHz channel of the chandef on this radio (only the
// DFS channels have one), and the longest CAC time among them in p.cac_ms
function pre_states(p) {
	let st = {}, f = 0, want = {};
	for (let c in pre_freqs(p)) want[c] = true;
	let pp = fs.popen(`iw phy ${radio.phy} channels 2>/dev/null`);
	if (!pp) return st;
	for (let line in split(pp.read("all") || "", "\n")) {
		let m = match(line, /^[ \t]*\* ([0-9]+) MHz/);
		if (m) { f = +m[1]; continue; }
		if (!want[f]) continue;
		m = match(line, /DFS state: ([a-z]+)/);
		if (m) st[f] = m[1];
		m = match(line, /DFS CAC time: ([0-9]+) ms/);
		if (m && +m[1] > (p.cac_ms || 0)) p.cac_ms = +m[1];
	}
	pp.close();
	return st;
}

function pre_event(state, extra) {
	let ev = { id: pre.id, state, freq: pre.freq, center_freq1: pre.center_freq1, width: pre.width,
		   phy: radio.phy, time: time(), ...(extra || {}) };
	pre.state = state;
	conn.event("airscan.precheck", ev);
	log(`DFS pre-check ${pre.id}: ${state} (${pre.freq} MHz, ${pre.width} MHz at ${pre.center_freq1})` +
	    (extra?.why ? `: ${extra.why}` : ""));
}

function pre_hold_start() {
	let mac = replace(trim(fs.readfile(`/sys/class/net/${PRE}/address`) || ""), /:/g, "");
	// beacon: frame control 0x0080, duration 0, broadcast DA, SA = BSSID = the
	// scan radio, sequence 0, TSF 0, interval 100 TU, capability ESS, SSID element
	// of length 0, supported rates 6*, 9, 12*, 18, 24*, 36, 48, 54. ucode's nl80211
	// has no "ssid" key and rejects an empty beacon_tail: both are left out.
	let head = hexdec("80000000ffffffffffff" + mac + mac + "0000" + "0000000000000000" +
			  "6400" + "0100" + "0000" + "01088c129824b048606c");
	let r = nl80211.request(nl80211.const.NL80211_CMD_START_AP ?? 15, 0, {
		dev: PRE, beacon_head: head, beacon_interval: 100, dtim_period: 2, hidden_ssid: 1,
		wiphy_freq: pre.freq, channel_width: NL_WIDTH[pre.width], center_freq1: pre.center_freq1 });
	if (r === null) return nl80211.error() || "START_AP failed";
	pre.held = true;
	return null;
}

function pre_end(why, state) {
	if (!pre) return;
	if (pre.timer) { pre.timer.cancel(); pre.timer = null; }
	if (pre.held) nl80211.request(nl80211.const.NL80211_CMD_STOP_AP ?? 16, 0, { dev: PRE });
	iface_del(PRE);
	pre_event(state || "ended", { why });
	pre = null;
	if (loan.until) loan_end(`a DFS pre-check ended: ${why}`);
}

// one step of a pre-check: wait for the radio, start the check, follow it
function pre_step() {
	if (!pre) return;
	let p = pre;
	p.timer = null;
	let again = (ms) => { if (pre === p) p.timer = later(ms, pre_step); };
	if (p.phase == "wait") {
		if (state != "lent") {
			if (time() > p.asked + 10) return pre_end("the scan radio was not free within 10 s");
			return again(200);
		}
		if (!iface_up(PRE, "__ap")) return pre_end("cannot add the check interface");
		let r = nl80211.request(nl80211.const.NL80211_CMD_RADAR_DETECT ?? 94, 0, {
			dev: PRE, wiphy_freq: p.freq, channel_width: NL_WIDTH[p.width], center_freq1: p.center_freq1 });
		if (r === null) return pre_end(`the radar check did not start: ${nl80211.error()}`);
		p.phase = "cac"; p.cac_start = time();
		pre_event("cac", { cac_ms: p.cac_ms });
		return again(2000);
	}
	if (p.phase == "cac") {
		let st = pre_states(p), vals = values(st);
		if (length(filter(vals, (s) => s == "unavailable")))
			return pre_end("radar detected", "radar");
		if (length(vals) && length(filter(vals, (s) => s == "available")) == length(vals)) {
			let err = pre_hold_start();
			p.phase = "hold"; p.hold_until = time() + p.hold;
			pre_event("available", { held: !err, ...(err ? { hold_error: err } : {}) });
			return again(1000);
		}
		if (time() > p.cac_start + p.cac_ms / 1000 + 15)
			return pre_end(`no result ${p.cac_ms / 1000 + 15} s after the check started (states ${sprintf("%J", st)})`);
		// near the end, every second: the hold must start well inside FCC's 2 s
		return again(time() >= p.cac_start + p.cac_ms / 1000 - 3 ? 250 : 2000);
	}
	if (p.phase == "hold") {
		if (time() >= p.hold_until) return pre_end("the hold ran out");
		return again(1000);
	}
}

// probe_driver asks the kernel to bind a driver to a scan radio that has
// none, as one left between a heal's unbind and its bind is (an airscan
// killed there), or one anyone else unbound: no one else would bind it
// again. Once a minute at the most.
function probe_driver() {
	let dev = `/sys/bus/pci/devices/${radio.pci}`;
	if (!radio.pci || !fs.access(dev) || fs.readlink(`${dev}/driver`) || time() < heal.probed + 60)
		return;
	heal.probed = time();
	note_error("the scan radio has no driver: asking the kernel to bind one");
	fs.writefile("/sys/bus/pci/drivers_probe", radio.pci);
}

function run_next() {
	if (busy) return;
	if (loan.until) {
		if (time() < loan.until) {
			if (state != "lent") {
				capture_close();
				if (radio.spectral) spectral_write("spectral_scan_ctl", "disable");
				iface_del(MON);
				iface_del(STA);
				state = "lent";
				log(`the scan radio is lent to ${loan.to}, for ${loan.until - time()} s at the most`);
			}
			later(200, run_next);
			return;
		}
		loan_end("the loan ran out");
	}
	if (!resolve_radio()) {
		state = "waiting for scan radio";
		probe_driver();
		later(5000, run_next);
		return;
	}
	let now = time();
	let done = (job) => () => { busy = false; state = "idle"; later(20, run_next); };
	busy = true;
	if (pending.bss || now >= next.bss) {
		pending.bss = false; next.bss = now + cfg.bss_interval; state = "bss";
		capture_close();
		run_bss(done("bss"));
	}
	else if (pending.spectral != null || now >= next.spectral) {
		let band = pending.spectral || null;
		// a sweep of one band asked for leaves the time of the next full one
		pending.spectral = null; state = "spectral";
		if (!band) next.spectral = now + cfg.spectral_interval;
		capture_close(); iface_del(STA);
		run_spectral(done("spectral"), band);
	}
	else {
		state = "clients";
		run_client_slice(done("clients"));
	}
}

// kill_children stops our helpers still running (a scan, a sweep, a BLE
// round): they must not outlive us. The image has no pkill, so read the
// parent of each process from /proc/<pid>/stat ("pid (comm) state ppid ...").
function kill_children() {
	let kids = [];
	for (let d in fs.lsdir("/proc") || []) {
		if (!match(d, /^[0-9]+$/)) continue;
		let st = fs.readfile(`/proc/${d}/stat`) || "";
		let f = split(substr(st, rindex(st, ")") + 2), " ");
		if (+f[1] == MYPID) push(kids, d);
	}
	if (length(kids)) system(`kill -TERM ${join(" ", kids)} 2>/dev/null`);
}

function cleanup() {
	if (heal.unbound) {
		log("stopping in the middle of a heal: binding the scan radio's driver again");
		fs.writefile(`${heal.unbound}/bind`, radio.pci);
		heal.unbound = null;
	}
	kill_children();
	capture_close();
	if (wp.sock) { wp.sock.close(); wp.sock = null; }
	if (ble.timer) ble.timer.cancel();
	if (radio.spectral) spectral_write("spectral_scan_ctl", "disable");
	if (pre?.held) nl80211.request(nl80211.const.NL80211_CMD_STOP_AP ?? 16, 0, { dev: PRE });
	iface_del(PRE);
	iface_del(MON);
	iface_del(STA);
	if (+trim(fs.readfile(PIDFILE) || "") == MYPID) fs.unlink(PIDFILE);
}

// ---------------------------------------------------------------- ubus

function by_band(sp, band) {
	if (!sp || !band) return sp;
	return { ...sp, windows: filter(sp.windows, (w) => band_of(w.center) == band) };
}

// uhttpd's ubus handler adds ubus_rpc_session to the arguments of every call
// it forwards (from LuCI), and a ucode ubus method rejects an argument it does
// not declare
const SESSION = { ubus_rpc_session: "" };

const methods = {
	status: {
		args: SESSION,
		call: () => ({
			board: board_name(), radio: radio.pci, phy: radio.phy, section: radio.section,
			spectral_supported: radio.spectral, state,
			// what a borrower can count on: "lend", and a deaf radio healed;
			// with no scan radio, the BLE survey only
			features: radio.pci ? [ "lend", "heal", "precheck" ] : [ "ble" ],
			loan: loan.until ? { to: loan.to, until: loan.until } : null,
			precheck: pre ? { id: pre.id, state: pre.state, freq: pre.freq, center_freq1: pre.center_freq1,
					  width: pre.width, hold: pre.hold, held: pre.held } : null,
			heal: { count: heal.count, failed: heal.failed, last: heal.last },
			last, next, config: cfg,
			counts: { bss: results.bss?.count || 0, clients: length(results.clients), windows: length(results.spectrum?.windows || []), events: length(events), ble: length(results.ble) },
			ble: { enabled: cfg.ble_enabled, running: ble.running, error: ble.error, rounds: ble.rounds },
			wired_probe: { enabled: cfg.wired_probe, domains: wp.domains, tracked: length(wp.bss) },
			errors,
		}),
	},
	spectrum: {
		args: { ...SESSION, band: "" },
		call: (req) => by_band(results.spectrum, req.args.band) || { pending: true },
	},
	bss: {
		args: SESSION,
		call: () => results.bss || { pending: true },
	},
	clients: {
		args: SESSION,
		call: () => clients_report(),
	},
	events: {
		args: SESSION,
		call: () => ({ time: time(), list: events }),
	},
	sweep: {
		args: { ...SESSION, band: "" },
		call: (req) => {
			let b = req.args.band || "";
			if (b && !(b in [ "2g", "5g", "6g" ])) return { error: "band must be 2g, 5g or 6g" };
			pending.spectral = b;
			return { queued: "spectral", band: b || "all" };
		},
	},
	scan: {
		args: SESSION,
		call: () => { pending.bss = true; return { queued: "bss" }; },
	},
	// lend: the radio, for seconds, to whoever asks (to, for the log). Only
	// between jobs: during a scan or a sweep the answer is no, with the
	// state, and the borrower asks again. The radio is free once status says
	// "lent", a client dwell later at the most (wait_ms).
	lend: {
		args: { ...SESSION, seconds: 0, to: "" },
		call: (req) => {
			let secs = +req.args.seconds || 60;
			if (secs < 1 || secs > LOAN_MOST) return { error: `seconds must be 1 to ${LOAN_MOST}` };
			if (loan.until) return { lent: false, state, to: loan.to };
			if (!(state in [ "clients", "idle" ]) || !radio.phy) return { lent: false, state };
			loan.until = time() + secs;
			loan.to = req.args.to || "another service";
			return { lent: true, phy: radio.phy, until: loan.until, wait_ms: cfg.client_dwell + 500 };
		},
	},
	// reclaim: the borrower is done, and the radio is airscan's again.
	reclaim: {
		args: SESSION,
		call: () => {
			if (loan.until) loan_end("given back");
			return { lent: false };
		},
	},
	// precheck: a DFS radar check of a 5 GHz chandef on the scan radio, then a
	// hold of `hold` s (default 30, at most 120) in which a serving radio can
	// move there by CSA (see "DFS pre-check" above). freq is the primary
	// channel's, center_freq1 the chandef's centre, width 20/40/80/160 MHz; id is
	// the caller's, echoed in every "airscan.precheck" event. Like lend, only
	// between jobs: otherwise the answer is no, with the state.
	precheck: {
		args: { ...SESSION, freq: 0, center_freq1: 0, width: 0, hold: 0, id: "" },
		call: (req) => {
			let a = req.args, width = +a.width || 80, freq = +a.freq, cf = +a.center_freq1 || freq;
			let hold = +a.hold || 30;
			if (!NL_WIDTH[width]) return { error: "width must be 20, 40, 80 or 160" };
			if (freq < 5160 || freq > 5900 || cf < 5160 || cf > 5900) return { error: "freq and center_freq1 must be 5 GHz" };
			if (freq < cf - width / 2 || freq > cf + width / 2) return { error: "freq is outside the chandef" };
			if (hold < 1 || hold > PRE_HOLD_MOST) return { error: `hold must be 1 to ${PRE_HOLD_MOST}` };
			if (pre) return { started: false, state, id: pre.id, precheck: pre.state };
			if (loan.until) return { started: false, state, to: loan.to };
			if (!(state in [ "clients", "idle" ]) || !radio.phy) return { started: false, state };
			let p = { id: a.id || sprintf("pc%d", time()), freq, center_freq1: cf, width, hold, cac_ms: 0 };
			let st = pre_states(p), vals = values(st);
			if (!length(vals)) return { error: "no DFS channel in that chandef on the scan radio", states: st };
			if (length(filter(vals, (s) => s == "unavailable"))) return { started: false, error: "a channel of it is unavailable (radar in the last 30 min)", states: st };
			// the regulatory CAC time iw shows (60 s in the US, 10 min on ETSI's weather channels)
			if (!p.cac_ms) p.cac_ms = 60000;
			p.phase = "wait"; p.asked = time(); p.state = "asked"; p.held = false;
			pre = p;
			loan.until = time() + p.cac_ms / 1000 + 15 + hold + 10;
			loan.to = `a DFS pre-check (${p.id})`;
			p.timer = later(200, pre_step);
			return { started: true, id: p.id, phy: radio.phy, cac_ms: p.cac_ms, hold, states: st };
		},
	},
	// precheck_done: the caller has moved (or given up); the hold ends now.
	precheck_done: {
		args: { ...SESSION, id: "" },
		call: (req) => {
			if (!pre) return { ended: false, error: "no pre-check running" };
			if (req.args.id && req.args.id != pre.id) return { ended: false, error: `the running pre-check is ${pre.id}` };
			let id = pre.id;
			pre_end("done");
			return { ended: true, id };
		},
	},
	ble: {
		args: SESSION,
		call: () => ble_report(),
	},
	ble_scan: {
		args: SESSION,
		call: () => {
			if (ble.running) return { queued: false, running: true };
			ble_schedule(10);
			return { queued: "ble" };
		},
	},
};

// ---------------------------------------------------------------- main

cfg = load_config();
if (!cfg.enabled) { log("disabled in /etc/config/airscan"); exit(0); }
let pci = BOARDS[board_name()];
if (!pci) { log(`no dedicated scan radio on board "${board_name()}"; not starting`); exit(0); }
radio.pci = pci;
// One airscan at a time. On a restart procd can start us while the previous
// instance is still cleaning up, and its cleanup deletes the scan interfaces;
// wait for it to exit (up to 15 s) before touching the radio.
for (let i = 0; i < 60; i++) {
	let old = +trim(fs.readfile(PIDFILE) || "");
	if (!old || old == MYPID || !fs.access(`/proc/${old}`)) break;
	if (i == 0) log(`waiting for the previous airscan (pid ${old}) to exit`);
	sleep(250);
}
fs.writefile(PIDFILE, `${MYPID}\n`);
fs.mkdir(OUT);
if (!release_from_netifd(pci)) { fs.unlink(PIDFILE); exit(1); }

uloop.init();
// ucode -S starts with its collector off: without it, what refers to itself
// (a job's nested functions, and the sweeps and looks they hold) is never
// freed. It runs once a minute from a timer of its own, never by itself:
// started (gc "start"), it can run inside a callback of filter() or map()
// and free the array they are building, and they answer null (ucode
// 2026.01.16).
function collect() {
	gc("collect");
	later(60000, collect);
}
later(60000, collect);
// An error in a callback is said where it can be read; uloop then ends, and
// procd starts airscan afresh.
uloop.guard((e) => {
	note_error(`stopped by an error: ${e?.message} ${e?.stacktrace?.[0]?.context ?? ""}`);
	uloop.end();
});
conn = ubus.connect();
if (!conn) { log("cannot connect to ubus"); exit(1); }
conn.publish("airscan", methods);
log(`started; scan radio ${pci}`);
later(2000, run_next);
ble_schedule(15000);
uloop.run();
cleanup();
