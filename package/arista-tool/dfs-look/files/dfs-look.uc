#!/usr/bin/ucode -S
// dfs-look: optional channel evaluation after local ACS starts a 5 GHz AP.
// A radio already on DFS is left alone. For a radio outside DFS, dfs-look
// then looks at every 5 GHz channel, the DFS ones too, and moves the radio to
// a DFS block only where that is clearly the best: better by MARGIN than the
// block it is on, and than the best block outside DFS. The move costs a
// radar check, a minute in which the radio's networks send nothing, so it is
// weighed once per start, a minute or so in, and not for a small gain.
//
// The look:
//   with a scan radio (airscan's, on the C-360, C-260 and C-230): two
//     of airscan's looks, each a passive scan of every channel and a
//     spectral sweep of 5 GHz. The serving radio does not scan.
//   without a scan radio: the serving radio listens on each 5 GHz channel
//     in turn, a second apart, twice over.
//   Either way, the radio's own block is rated by its own counts: how busy
//     others kept it while it looked, from its survey. A scan radio beside
//     it hears its own sending as busy.
// A channel is rated as Aeolus's radio resource management rates one: 100
// for a channel others keep busy all the time, 2 for each dB of noise above
// -95, 10 for each other network heard strongly there. A block is rated as
// its worst channel.
//
// The move is hostapd's CHAN_SWITCH, on a network's control socket: hostapd
// starts the radio's networks again on the channel, and they send nothing
// until the radar check there is done. (OpenWrt's ubus switch_chan, forced,
// has hostapd pick an automatic channel anew, outside DFS.) A radio not up
// there within its radar check and two minutes did not get there. A DFS block
// a radio left, for radar found there or anything else, or did not get to,
// is left alone for a day. When radar is found, hostapd moves the radio
// itself, as the law has it; dfs-look then looks again from where it went.
//
// Left alone: a radio with its channel set by hand, with acs_exclude_dfs set
// (kept off DFS), or with dfs_look_first '0' (DFS from the start, as before).
//
// dfs-look -n looks and says what it would do, and switches nothing; with a
// scan radio, it looks for a radio on a DFS channel too. -m <points> sets the
// margin, for a test.

'use strict';

import * as fs from 'fs';
import * as uloop from 'uloop';
import * as ubus from 'ubus';
import * as nl from 'nl80211';
import * as socket from 'socket';

const TICK = 1000;            // ms between steps
const SETTLE = 10;            // seconds a radio serves before it is looked at
const LOOKS = 2;              // looks at each channel before a move
const LOOK_MOST = 300;        // seconds a look may take before it is given up
const MARGIN = 20;            // rating points a DFS block must win by
const CAC_MOST = 60;          // seconds of radar check a move may cost
const ARRIVE_MOST = 120;      // seconds after its radar check a radio must be up by
const BARRED_FOR = 86400;     // seconds a DFS block is left alone once left
const SCAN_FLAG_AP = 4;       // NL80211_SCAN_FLAG_AP: scan on an AP's own interface
const REFUSED_MOST = 5;       // refused scans before a look on the radio is given up
const CTL = '/var/run/hostapd/';
const SOCK = '/var/run/dfs-look.sock';
const BARS = '/tmp/dfs-look.json';
const DRY = index(ARGV, '-n') >= 0;
const M = index(ARGV, '-m');
const WIN_BY = M >= 0 && ARGV[M + 1] != null ? +ARGV[M + 1] : MARGIN;

// The 5 GHz channels taken up together at each width, by the lowest and
// highest 20 MHz channel.
const BLOCKS5 = {
	'40': [ [ 36, 40 ], [ 44, 48 ], [ 52, 56 ], [ 60, 64 ], [ 100, 104 ], [ 108, 112 ], [ 116, 120 ], [ 124, 128 ], [ 132, 136 ], [ 140, 144 ], [ 149, 153 ], [ 157, 161 ] ],
	'80': [ [ 36, 48 ], [ 52, 64 ], [ 100, 112 ], [ 116, 128 ], [ 132, 144 ], [ 149, 161 ] ],
	'160': [ [ 36, 64 ], [ 100, 128 ] ],
};

// nl80211's channel widths, in MHz
const WIDTHS = { '0': 20, '1': 20, '2': 40, '3': 80, '4': 160, '5': 160, '13': 320 };

let conn;
let radios = {};   // radio -> what dfs-look knows of its current start
let bars = {};

function log(msg) {
	system([ 'logger', '-t', 'dfs-look', msg ]);
}

// now: seconds on the monotonic clock. An AP's wall clock is set by NTP a
// while after boot, often by minutes, which would end a look early.
function now() {
	return clock(true)[0];
}

// later: fn, once, in ms. A uloop timer that has fired is not freed until it
// is cancelled, nor is all its callback holds; this one cancels itself.
function later(ms, fn) {
	return uloop.timer(ms, function() { this.cancel(); fn(); });
}

function freq(c) {
	return 5000 + 5 * c;
}

function chan(f) {
	return int((f - 5000) / 5);
}

// does the radio's channel list (channels and ranges such as "36-48") take
// in channel c? With no list, every channel is in it.
function listed(list, c) {
	if (!length(list))
		return true;
	for (let v in list)
		for (let w in split(`${v}`, ' ')) {
			let r = split(w, '-');
			if (length(r) == 2 ? (c >= +r[0] && c <= +r[1]) : (+r[0] == c))
				return true;
		}
	return false;
}

function on(v) {
	return v === true || v in [ '1', 'on', 'yes', 'true', 'enabled' ] || v === 1;
}

function off(v) {
	return v === false || v in [ '0', 'off', 'no', 'false', 'disabled' ] || v === 0;
}

// what dfs-look may look after: each 5 GHz radio that is up on an automatic
// channel, has neither acs_exclude_dfs nor dfs_look_first '0', and serves an
// AP network, with the radio's options it needs.
function candidates() {
	let out = {};
	for (let name, r in conn.call('network.wireless', 'status') ?? {}) {
		let c = r.config ?? {};
		if (!r.up || c.band != '5g' || on(c.disabled) || on(c.acs_exclude_dfs) || off(c.dfs_look_first) ||
		    !(c.channel == null || c.channel in [ 'auto', '0', 0 ]))
			continue;
		let ifname = null;
		for (let i in r.interfaces ?? [])
			if (i.ifname && (i.config?.mode ?? 'ap') == 'ap') {
				ifname = i.ifname;
				break;
			}
		let list = c.chanlist ?? c.channels ?? c.channel_list;
		if (ifname)
			out[name] = { ifname, htmode: c.htmode, list: list == null ? [] : (type(list) == 'array' ? list : [ list ]) };
	}
	return out;
}

function ifindex(ifname) {
	return +trim(fs.readfile(`/sys/class/net/${ifname}/ifindex`) || '0');
}

function phy_of(ifname) {
	return trim(fs.readfile(`/sys/class/net/${ifname}/phy80211/name`) || '');
}

// channels lists the phy's 5 GHz channels it may use, each { radar, state,
// cac }: state is Linux's for a channel shared with radar (usable: a radio
// may check it; available: checked; unavailable: radar was found there, and
// it is closed for half an hour); cac is its radar check, in seconds.
function channels(phy) {
	let idx = +trim(fs.readfile(`/sys/class/ieee80211/${phy}/index`) || '-1');
	let w = nl.request(nl.const.NL80211_CMD_GET_WIPHY, nl.const.NLM_F_DUMP, { wiphy: idx, split_wiphy_dump: true });
	let out = {};
	for (let part in (type(w) == 'array' ? w : [ w ]))
		for (let b in part?.wiphy_bands ?? [])
			for (let f in b?.freqs ?? [])
				if (f.freq >= 5150 && f.freq <= 5895 && !f.disabled)
					out[`${chan(f.freq)}`] = {
						radar: !!f.radar,
						state: f.radar ? ([ 'usable', 'unavailable', 'available' ][f.dfs_state] ?? 'unknown') : null,
						cac: f.radar ? (f.dfs_cac_time ?? 60000) / 1000 : 0,
					};
	return out;
}

// where the radio is: its channel and width, from its first network
function where(ifname) {
	let i = nl.request(nl.const.NL80211_CMD_GET_INTERFACE, 0, { dev: ifname });
	if (!i?.wiphy_freq)
		return null;
	return { channel: chan(i.wiphy_freq), width: WIDTHS[`${i.channel_width}`] ?? 20 };
}

// survey: nl80211's survey of the radio, by frequency
function survey(ifname) {
	let out = {};
	for (let x in nl.request(nl.const.NL80211_CMD_GET_SURVEY, nl.const.NLM_F_DUMP, { dev: ifname }) ?? [])
		if (x.survey_info?.frequency)
			out[`${x.survey_info.frequency}`] = x.survey_info;
	return out;
}

// the in-use counts of the radio's own channel, for how busy others kept it
function own_counts(ifname) {
	for (let f, s in survey(ifname))
		if (s.in_use)
			return { time: s.time ?? 0, busy: s.busy ?? 0, tx: s.time_tx ?? 0, noise: s.noise };
	return null;
}

// the MACs of this AP's own wireless interfaces, which are not other networks
function own_macs() {
	let out = {};
	for (let i in nl.request(nl.const.NL80211_CMD_GET_INTERFACE, nl.const.NLM_F_DUMP) ?? [])
		if (i.mac)
			out[lc(i.mac)] = true;
	return out;
}

// weight is how much another network counts against its channel: fully at
// -55 dBm or stronger, not at all at -95 or weaker.
function weight(signal) {
	return min(1, max(0, (signal + 95) / 40.0));
}

// cost rates a channel: lower is better.
function cost(r) {
	let c = 100 * min(1, max(0, r.busy ?? 0));
	if (r.noise != null)
		c += 2 * max(0, r.noise + 95);
	for (let bssid, s in r.signals)
		c += 10 * weight(s);
	return c;
}

function rated(rates, c) {
	return rates[`${c}`] ??= { busy: null, busy_n: 0, noise: null, signals: {} };
}

function add_busy(r, busy) {
	if (busy == null)
		return;
	r.busy = ((r.busy ?? 0) * r.busy_n + busy) / (r.busy_n + 1);
	r.busy_n++;
}

function add_signal(r, bssid, signal) {
	r.signals[bssid] = max(r.signals[bssid] ?? -200, signal);
}

// blocks are what a radio at width may be on: each with its channels
// (members), all of them allowed here and at least one in its channel list.
// That is hostapd's ACS's rule: it takes a block where any of its channels
// is listed and makes that one the primary, so a list of one channel per
// block (as Aeolus writes) still names every block.
function blocks(width, chans, list) {
	let out = [];
	let spans = width <= 20 ? map(sort(map(keys(chans), (c) => +c), (a, b) => a - b), (c) => [ c, c ]) : (BLOCKS5[`${width}`] ?? []);
	for (let b in spans) {
		let members = [];
		for (let c = b[0]; c <= b[1]; c += 4)
			push(members, c);
		if (length(filter(members, (c) => chans[`${c}`])) == length(members) &&
		    length(filter(members, (c) => listed(list, c))))
			push(out, members);
	}
	return out;
}

function barred(phy, members) {
	for (let c in members)
		if ((bars[`${phy} ${c}`] ?? 0) > now())
			return true;
	return false;
}

function bar(phy, members, why) {
	for (let c in members)
		bars[`${phy} ${c}`] = now() + BARRED_FOR;
	fs.writefile(BARS, sprintf('%J', bars));
	log(`${phy}: channels ${join(',', members)} left alone for a day (${why})`);
}

// hostapd_ctl sends one of hostapd's networks a command on its control
// socket and returns its answer: null where none comes within ms
// milliseconds, false where it could not be sent. hostapd answers to the
// asking socket's own path, as its own user, which must be able to write
// there.
function hostapd_ctl(ifname, ask, ms) {
	let k = socket.create(socket.AF_UNIX, socket.SOCK_DGRAM);
	if (!k)
		return false;
	fs.unlink(SOCK);
	let answer = false;
	if (k.bind({ path: SOCK }) && fs.chmod(SOCK, 0o666) && k.connect({ path: CTL + ifname }) && k.send(ask) != null) {
		socket.poll(ms, [ k, socket.POLLIN ]);
		answer = k.recv(8192, socket.MSG_DONTWAIT);
	}
	k.close();
	fs.unlink(SOCK);
	return answer;
}

// chan_switch is hostapd's CHAN_SWITCH for channel c at width, naming each
// generation the radio runs (its htmode, such as HE40): hostapd keeps only
// those the command names.
function chan_switch(c, width, htmode) {
	let fam = match(htmode ?? '', /^(NOHT|HT|VHT|HE|EHT)/)?.[1];
	if (!fam)
		return null;
	let f = freq(c), cf = f, sec = 0;
	if (width > 20)
		for (let b in BLOCKS5[`${width}`] ?? [])
			if (c >= b[0] && c <= b[1]) {
				cf = int((freq(b[0]) + freq(b[1])) / 2);
				sec = int((c - b[0]) / 4) % 2 ? -1 : 1;
			}
	let s = sprintf('CHAN_SWITCH 10 %d', f);
	if (sec)
		s += ` sec_channel_offset=${sec}`;
	s += ` center_freq1=${cf} bandwidth=${width}`;
	if (fam != 'NOHT')
		s += ' ht';
	if (fam in [ 'VHT', 'HE', 'EHT' ])
		s += ' vht';
	if (fam in [ 'HE', 'EHT' ])
		s += ' he';
	if (fam == 'EHT')
		s += ' eht';
	return s;
}

// ---------------------------------------------------------------- the look

// A look with airscan: a passive scan of every channel and a sweep of 5 GHz,
// LOOKS times. It is done when airscan has a scan and a sweep newer than the
// ones it had when they were asked for.
function look_airscan(r, l) {
	let b = conn.call('airscan', 'bss'), s = conn.call('airscan', 'spectrum', { band: '5g' });
	if (!l.asked) {
		l.bss_was = b?.time ?? 0;
		l.sweep_was = s?.time ?? 0;
		conn.call('airscan', 'scan');
		conn.call('airscan', 'sweep', { band: '5g' });
		l.asked = now();
		return false;
	}
	if ((b?.time ?? 0) <= l.bss_was || (s?.time ?? 0) <= l.sweep_was)
		return false;
	for (let e in b.list ?? [])
		if (e.band == '5g' && !e.own && e.class != 'own' && !l.mine[lc(e.bssid ?? '')] && e.signal != null)
			add_signal(rated(l.rates, e.channel ?? chan(e.freq)), lc(e.bssid), e.signal);
	// a 20 MHz window's center is its channel's frequency
	for (let w in s.windows ?? [])
		if (w.band == '5g' && w.width == 20 && w.busy_pct != null)
			add_busy(rated(l.rates, chan(w.center)), w.busy_pct / 100.0);
	l.asked = 0;
	return ++l.done >= LOOKS;
}

// A look by the radio itself: each 5 GHz channel in turn, one at a time,
// LOOKS times. A visit is rated at the next step, from the radio's survey of
// that channel and the networks heard there.
function look_self(r, l) {
	if (l.visit) {
		let f = l.visit, s = survey(r.ifname)[`${f}`];
		if (s?.time && s.busy != null && !s.in_use)
			add_busy(rated(l.rates, chan(f)), (s.busy - (s.time_tx ?? 0)) / (1.0 * s.time));
		if (s?.noise != null && !s.in_use)
			rated(l.rates, chan(f)).noise = s.noise;
		for (let x in nl.request(nl.const.NL80211_CMD_GET_SCAN, nl.const.NLM_F_DUMP, { dev: r.ifname }) ?? []) {
			let e = x.bss;
			if (e?.frequency == f && (e.seen_ms_ago ?? 0) < 3000 && e.bssid && !l.mine[lc(e.bssid)] && e.signal_mbm != null)
				add_signal(rated(l.rates, chan(f)), lc(e.bssid), e.signal_mbm / 100);
		}
		l.visit = null;
	}
	if (l.next >= length(l.queue))
		return true;
	let f = l.queue[l.next++];
	nl.request(nl.const.NL80211_CMD_TRIGGER_SCAN, 0, { dev: r.ifname, scan_frequencies: [ f ], scan_flags: SCAN_FLAG_AP });
	if (nl.error()) {
		if (++l.refused >= REFUSED_MOST)
			l.given_up = `the radio refuses to scan (${nl.error()})`;
		return false;
	}
	l.visit = f;
	return false;
}

function look_start(r) {
	let as = conn.call('airscan', 'status');
	let scan_radio = index(as?.features ?? [], 'lend') >= 0 && as?.spectral_supported;
	let l = { by: scan_radio ? 'airscan' : 'self', began: now(), rates: {}, mine: own_macs(), done: 0, asked: 0,
		next: 0, refused: 0, visit: null, own: own_counts(r.ifname), queue: [] };
	for (let i = 0; i < LOOKS; i++)
		for (let c in sort(map(keys(r.chans), (c) => +c), (a, b) => a - b))
			push(l.queue, freq(c));
	log(`${r.name}: up on channel ${r.channel}${r.chans[`${r.channel}`]?.radar ? '' : ' outside DFS'}; looking at every channel${scan_radio ? ' with the scan radio' : ''}${DRY ? ' (dry run)' : ''}`);
	return l;
}

// ---------------------------------------------------------------- the choice

function describe(b) {
	return b ? `${b.members[0]}${length(b.members) > 1 ? '-' + b.members[length(b.members) - 1] : ''} (${int(b.cost + 0.5)})` : 'none';
}

// choose rates the radio's blocks from its look and says where it goes:
// { to, entry } or null, with what it weighed in its log.
function choose(r, l) {
	let rates = l.rates;
	// The radio's own block, by its own counts while it looked (not named
	// now: that is the clock, which the log line below reads)
	let own = own_counts(r.ifname);
	if (l.own && own && own.time > l.own.time) {
		let busy = ((own.busy - l.own.busy) - (own.tx - l.own.tx)) / (1.0 * (own.time - l.own.time));
		for (let c in r.block) {
			let x = rated(rates, c);
			x.busy = busy;
			x.busy_n = 1;
			if (own.noise != null)
				x.noise = own.noise;
		}
	}
	let cur = null, plain = null, far = null;
	for (let members in blocks(r.width, r.chans, r.list)) {
		if (length(filter(members, (c) => rates[`${c}`]?.busy != null)) != length(members))
			continue;
		let b = { members, cost: 0, dfs: false, entry: null };
		for (let c in members) {
			let x = rates[`${c}`], k = cost(x);
			b.cost = max(b.cost, k);
			if (r.chans[`${c}`].radar)
				b.dfs = true;
			// the primary: a listed channel, as ACS would make it
			if (listed(r.list, c) && (!b.entry || k < cost(rates[`${b.entry}`])))
				b.entry = c;
		}
		if (index(members, r.channel) >= 0)
			cur = b;
		if (!b.dfs && (!plain || b.cost < plain.cost))
			plain = b;
		let clear = length(filter(members, (c) => !r.chans[`${c}`].radar ||
			(r.chans[`${c}`].state in [ 'usable', 'available' ] && r.chans[`${c}`].cac <= CAC_MOST))) == length(members);
		if (b.dfs && clear && !barred(r.phy, members) && (!far || b.cost < far.cost))
			far = b;
	}
	let why = null;
	if (!cur)
		why = 'its own block was not rated';
	else if (!far)
		why = 'no DFS block it may go to was rated';
	else if (far.cost + WIN_BY > cur.cost)
		why = 'no DFS block is clearly better than its own';
	else if (plain && plain !== cur && far.cost + WIN_BY > plain.cost)
		why = 'no DFS block is clearly better than the best outside DFS';
	log(`${r.name}: looked (${l.by}, ${now() - l.began} s): on ${describe(cur)}, best outside DFS ${describe(plain)}, best DFS ${describe(far)}; ` +
		(why ? `stays: ${why}` : `goes to ${far.entry} after a radar check`));
	return why ? null : far;
}

// ---------------------------------------------------------------- each radio

function restart(name) {
	conn.call('network.wireless', 'down', { device: name });
	conn.call('network.wireless', 'up', { device: name });
}

// again: the radio is looked at afresh once it is up outside DFS
function again(r) {
	r.state = 'wait';
	r.since = now();
}

// wait: for the radio to serve SETTLE seconds outside DFS, then look
function on_wait(r, w, up) {
	if (!up) {
		r.since = now();
		return;
	}
	r.chans = channels(r.phy);
	r.channel = w.channel;
	r.width = w.width;
	if (r.chans[`${w.channel}`]?.radar && !DRY) {
		log(`${r.name}: on DFS channel ${w.channel}, from where it can't look elsewhere; left as it is`);
		r.state = 'on-dfs';
		return;
	}
	if (now() - r.since < SETTLE)
		return;
	r.block = filter(blocks(r.width, r.chans, []), (m) => index(m, w.channel) >= 0)[0] ?? [ w.channel ];
	r.look = look_start(r);
	r.state = 'look';
}

// look: one step of the look; at its end, the choice, and the switch
function on_look(r, w, up) {
	if (!up || w.channel != r.channel || w.width != r.width) {
		log(`${r.name}: moved or went down while it looked; it looks again`);
		return again(r);
	}
	let l = r.look;
	let done = l.by == 'airscan' ? look_airscan(r, l) : look_self(r, l);
	if (!done && !l.given_up && now() - l.began > LOOK_MOST)
		l.given_up = `no look within ${LOOK_MOST} s`;
	if (l.given_up) {
		log(`${r.name}: stays outside DFS: ${l.given_up}`);
		r.state = 'done';
		return;
	}
	if (!done)
		return;
	let to = choose(r, l);
	r.look = null;
	r.state = 'done';
	if (!to || DRY)
		return;
	// For a DFS channel hostapd starts the radio again there, and its radar
	// check, before it answers (hostapd_switch_channel_fallback()), so the
	// answer can be late: only a refusal counts as one, and whether the radio
	// gets there tells the rest.
	let cmd = chan_switch(to.entry, r.width, r.htmode);
	let ans = cmd ? hostapd_ctl(r.ifname, cmd, 5000) : false;
	if (ans === false || (ans != null && trim(ans) != 'OK')) {
		log(`${r.name}: hostapd did not take ${cmd ?? 'a channel switch for htmode ' + r.htmode}: ${ans === false ? 'not sent' : trim(ans)}`);
		bar(r.phy, to.members, 'the switch was refused');
		return;
	}
	r.to = to;
	r.sent = now();
	r.cac = 0;
	for (let m in to.members)
		r.cac = max(r.cac, r.chans[`${m}`].cac);
	r.state = 'moving';
	log(`${r.name}: ${cmd}` + (ans == null ? ' (no answer yet; its arrival tells)' : ''));
}

// moving: until the radio is up on the DFS channel, its radar check done
function on_moving(r, w, up, key) {
	if (up && w.channel == r.to.entry) {
		log(`${r.name}: up on channel ${w.channel}, ${now() - r.sent} s after the switch`);
		r.state = 'there';
		r.key = key;
		return;
	}
	if (now() - r.sent < r.cac + ARRIVE_MOST)
		return;
	bar(r.phy, r.to.members, 'the radio did not get there');
	if (!up) {
		log(`${r.name}: not up ${now() - r.sent} s after the switch; it is started again`);
		restart(r.name);
	}
	again(r);
}

// there: on the DFS block it went to, until it leaves it
function on_there(r, w, up) {
	if (!up || index(r.to.members, w.channel) >= 0)
		return;
	let chans = channels(r.phy);
	let radar = filter(r.to.members, (m) => chans[`${m}`]?.state == 'unavailable');
	bar(r.phy, r.to.members, length(radar) ? `radar found on ${join(',', radar)}; hostapd moved the radio to ${w.channel}` : `the radio left for ${w.channel}`);
	again(r);
}

function step(name, c) {
	let key = `${c.ifname}:${ifindex(c.ifname)}`;
	let r = radios[name];
	if (!r || (r.key != key && r.state != 'moving')) {
		r = radios[name] = { name, key, ifname: c.ifname, phy: phy_of(c.ifname) };
		again(r);
	}
	r.htmode = c.htmode;
	r.list = c.list;
	let st = conn.call(`hostapd.${c.ifname}`, 'get_status');
	let w = where(c.ifname);
	let up = st?.status == 'ENABLED' && w != null;

	if (r.state == 'wait')
		on_wait(r, w, up);
	else if (r.state == 'look')
		on_look(r, w, up);
	else if (r.state == 'moving')
		on_moving(r, w, up, key);
	else if (r.state == 'there')
		on_there(r, w, up);
	else if (r.state == 'on-dfs' && up && !channels(r.phy)[`${w.channel}`]?.radar)
		again(r);
}

function tick() {
	let found = candidates();
	for (let name in keys(radios))
		if (!found[name])
			delete radios[name];
	for (let name, c in found)
		step(name, c);
	later(TICK, tick);
}

// ---------------------------------------------------------------- main

uloop.init();
// ucode -S starts with its collector off: without it, what refers to itself
// is never freed. It runs once a minute from a timer of its own.
function collect() {
	gc('collect');
	later(60000, collect);
}
later(60000, collect);
uloop.guard((e) => {
	log(`stopped by an error: ${e?.message} ${e?.stacktrace?.[0]?.context ?? ''}`);
	uloop.end();
});
conn = ubus.connect();
if (!conn) {
	log('cannot connect to ubus');
	exit(1);
}
try {
	bars = json(fs.readfile(BARS) || '{}') ?? {};
} catch (e) {
	bars = {};
}
log('started');
later(TICK, tick);
uloop.run();
