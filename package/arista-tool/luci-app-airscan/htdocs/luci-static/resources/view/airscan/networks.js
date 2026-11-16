'use strict';
'require view';
'require dom';
'require poll';
'require ui';
'require airscan.common as common';

/* What the scan radio hears: alerts (wired rogues, evil twins), the channels
   by band, and every network. */

/* airscan's classes (airscan.uc) */
var CLASSES = {
	own:              [ _('this AP'), '#2a7ab0' ],
	trusted:          [ _('trusted'), '#3a9a4a' ],
	verified:         [ _('verified'), '#3a9a4a' ],
	verified_sibling: [ _('verified (sibling)'), '#3a9a4a' ],
	unverified:       [ _('your SSID, not yet verified'), '#b8860b' ],
	neighbor:         [ _('neighbour'), '#777' ],
	wired_rogue:      [ _('wired rogue'), '#c0392b' ],
	evil_twin:        [ _('evil twin'), '#c0392b' ],
	spoofed:          [ _('spoofed BSSID'), '#c0392b' ],
	deauth_flood:     [ _('deauth flood'), '#c0392b' ]
};

var EVENTS = {
	wired_rogue: _('Wired rogue: relays this AP\'s wired probes, but is not one of your SSIDs'),
	evil_twin: _('Evil twin: one of your SSIDs on a network that never relays the wired probes'),
	spoofed: _('Spoofed: this AP\'s own BSSID heard where it is not'),
	deauth_flood: _('Deauthentication flood')
};

function cls(b) {
	var c = CLASSES[b.class] || [ b.class || '-', '#777' ];
	return common.badge(c[0], c[1]);
}

return view.extend({
	load: function() {
		return Promise.all([ common.status(), common.bss(), common.events(), common.clients() ]);
	},

	alerts: function() {
		var ev = (this.ev && this.ev.list) || [], now = Date.now() / 1000;
		if (!ev.length)
			return E('p', {}, _('No alerts.'));
		var t = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [ _('Alert'), _('SSID'), _('BSSID'), _('Channel'), _('Signal'), _('VLANs'), _('Since') ]
				.map(function(h) { return E('th', { 'class': 'th' }, h); }))
		]);
		ev.slice().sort(function(a, b) { return (b.time || 0) - (a.time || 0); }).forEach(function(e, i) {
			t.appendChild(E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1) }, [
				E('td', { 'class': 'td', 'title': EVENTS[e.type] || '' }, common.badge((CLASSES[e.type] || [ e.type ])[0], '#c0392b')),
				E('td', { 'class': 'td' }, e.ssid || _('(hidden)')),
				E('td', { 'class': 'td' }, E('code', e.bssid || '-')),
				E('td', { 'class': 'td' }, e.freq ? common.channel(e.freq) + ' (' + e.freq + ')' : '-'),
				E('td', { 'class': 'td' }, e.signal != null ? e.signal + ' dBm' : '-'),
				E('td', { 'class': 'td' }, (e.vlans || []).join(', ') || '-'),
				E('td', { 'class': 'td' }, common.ago(e.time, now))
			]));
		});
		return E('div', {}, [
			t,
			E('p', { 'class': 'cbi-value-description' },
				_('Other APs of yours relay the probes too: list their SSIDs (or BSSIDs) as trusted under Settings, or they show as wired rogues.'))
		]);
	},

	channels: function() {
		var list = (this.bssData && this.bssData.list) || [], clients = (this.cl && this.cl.list) || [];
		var by = {};
		list.forEach(function(b) {
			var k = b.band + ':' + b.channel;
			(by[k] = by[k] || { band: b.band, channel: b.channel, freq: b.freq, nets: [], clients: 0 }).nets.push(b);
		});
		clients.forEach(function(c) {
			var band = common.bandOf(c.freq), k = band + ':' + common.channel(c.freq);
			if (by[k]) by[k].clients++;
		});
		var out = [];
		Object.keys(common.bands).forEach(function(band) {
			var rows = Object.keys(by).map(function(k) { return by[k]; })
				.filter(function(r) { return r.band == band; })
				.sort(function(a, b) { return a.channel - b.channel; });
			if (!rows.length) return;
			var t = E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [ _('Channel'), _('Networks'), _('Strongest'), _('Widths'), _('Clients heard'), _('SSIDs') ]
					.map(function(h) { return E('th', { 'class': 'th' }, h); }))
			]);
			rows.forEach(function(r, i) {
				var best = Math.max.apply(null, r.nets.map(function(b) { return b.signal; }));
				var widths = r.nets.map(function(b) { return b.width; }).filter(function(v, i, a) { return v && a.indexOf(v) == i; });
				var ssids = r.nets.map(function(b) { return b.ssid || _('(hidden)'); }).filter(function(v, i, a) { return a.indexOf(v) == i; });
				t.appendChild(E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1) }, [
					r.channel + ' (' + r.freq + ')', r.nets.length, best + ' dBm',
					widths.map(function(w) { return w + ' MHz'; }).join(', '), r.clients, ssids.join(', ')
				].map(function(c) { return E('td', { 'class': 'td' }, c); })));
			});
			out.push(E('h4', common.bands[band]), t);
		});
		return out.length ? E('div', {}, out) : E('p', {}, _('No networks heard yet.'));
	},

	networks: function() {
		var list = ((this.bssData && this.bssData.list) || []).slice();
		list.sort(function(a, b) { return (b.signal || -999) - (a.signal || -999); });
		var t = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [ _('SSID'), _('BSSID'), _('Band'), _('Channel'), _('Width'), _('Signal'), _('Security'), _('Generation'), _('Class') ]
				.map(function(h) { return E('th', { 'class': 'th' }, h); }))
		]);
		list.forEach(function(b, i) {
			t.appendChild(E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1) }, [
				b.hidden ? E('em', _('(hidden)')) : b.ssid,
				E('code', b.bssid),
				common.bands[b.band] || b.band,
				b.channel,
				b.width ? b.width + ' MHz' : '-',
				b.signal + ' dBm',
				(b.security || []).join(', ') || _('open'),
				b.gen || '-',
				cls(b)
			].map(function(c) { return E('td', { 'class': 'td' }, c); })));
		});
		return t;
	},

	draw: function() {
		var st = this.statusData || {}, b = this.bssData || {}, now = Date.now() / 1000;
		dom.content(this.info, _('Last network survey %s (%s); %d networks, %d clients heard.').format(
			common.time(b.time), common.ago(b.time, now), (b.list || []).length, ((this.cl && this.cl.list) || []).length));
		dom.content(this.body, [
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Alerts')), this.alerts() ]),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Channels')), this.channels() ]),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Networks')), this.networks() ])
		]);
	},

	refresh: function() {
		return Promise.all([ common.status(), common.bss(), common.events(), common.clients() ]).then(L.bind(function(d) {
			this.statusData = d[0]; this.bssData = d[1]; this.ev = d[2]; this.cl = d[3];
			this.draw();
		}, this));
	},

	render: function(data) {
		this.statusData = data[0] || {}; this.bssData = data[1] || {}; this.ev = data[2] || {}; this.cl = data[3] || {};
		this.info = E('div', { 'class': 'cbi-map-descr' });
		this.body = E('div');
		var scanBtn = E('button', { 'class': 'cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, function() {
			return common.scan().then(function() {
				ui.addNotification(null, E('p', _('A network survey has started; this page updates when it finishes.')), 'info');
			});
		}) }, _('Survey now'));
		var page = E('div', { 'class': 'cbi-map' }, [
			E('h2', _('Air Scan: Networks')),
			common.unavailable(this.statusData),
			this.info,
			E('div', { 'style': 'margin:.5em 0' }, scanBtn),
			this.body
		]);
		this.draw();
		poll.add(L.bind(this.refresh, this), 60);
		return page;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
