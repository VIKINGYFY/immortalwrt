'use strict';
'require view';
'require dom';
'require poll';
'require airscan.common as common';

/* The clients the scan radio hears on the channels that have networks. */

var SHOW = 300;

return view.extend({
	load: function() {
		return common.clients();
	},

	count: function(list, key) {
		var by = {};
		list.forEach(function(c) { var k = key(c); by[k] = (by[k] || 0) + 1; });
		return Object.keys(by).map(function(k) { return [ k, by[k] ]; }).sort(function(a, b) { return b[1] - a[1]; });
	},

	summary: function(title, rows) {
		var t = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [ E('th', { 'class': 'th' }, title), E('th', { 'class': 'th' }, _('Clients')) ])
		]);
		rows.slice(0, 20).forEach(function(r, i) {
			t.appendChild(E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1) }, [
				E('td', { 'class': 'td' }, r[0]), E('td', { 'class': 'td' }, r[1])
			]));
		});
		return t;
	},

	draw: function() {
		var d = this.data || {}, list = (d.list || []).slice(), now = Date.now() / 1000;
		list.sort(function(a, b) { return (b.last_seen || 0) - (a.last_seen || 0); });
		var randomized = list.filter(function(c) { return c.randomized; }).length;
		var probing = list.filter(function(c) { return !c.bssid; });
		dom.content(this.info, _('%d clients heard: %d connected to a network, %d not connected (probing); %d with randomized addresses. Last update %s.').format(
			list.length, list.length - probing.length, probing.length, randomized, common.ago(d.time, now)));

		var pt = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [ _('MAC'), _('Probe requests'), _('Looking for'), _('Channel'), _('Signal'), _('First heard'), _('Last heard') ]
				.map(function(h) { return E('th', { 'class': 'th' }, h); }))
		]);
		probing.slice(0, SHOW).forEach(function(c, i) {
			pt.appendChild(E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1) }, [
				E('span', {}, [ E('code', c.mac), c.randomized ? ' ' : '', c.randomized ? common.badge(_('random'), '#888') : '' ]),
				c.probe_reqs || 0,
				(c.probes || []).join(', ') || (c.probe_reqs ? _('any network') : '-'),
				c.freq ? common.channel(c.freq) + ' (' + c.freq + ')' : '-',
				c.signal != null ? c.signal + ' dBm' : '-',
				common.ago(c.first_seen, now),
				common.ago(c.last_seen, now)
			].map(function(v) { return E('td', { 'class': 'td' }, v); })));
		});

		var bySsid = this.count(list, function(c) {
			return c.bssid ? (c.ssid || _('(a network not surveyed)')) : _('(not connected)');
		});
		var byChan = this.count(list, function(c) {
			return c.freq ? common.bands[common.bandOf(c.freq)] + ' ' + common.channel(c.freq) : '-';
		});

		var t = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [ _('MAC'), _('SSID'), _('BSSID'), _('Channel'), _('Frames'), _('Probes for'), _('First heard'), _('Last heard') ]
				.map(function(h) { return E('th', { 'class': 'th' }, h); }))
		]);
		list.slice(0, SHOW).forEach(function(c, i) {
			t.appendChild(E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1) }, [
				E('span', {}, [ E('code', c.mac), c.randomized ? ' ' : '', c.randomized ? common.badge(_('random'), '#888') : '' ]),
				c.ssid || '-',
				c.bssid ? E('code', c.bssid) : '-',
				c.freq ? common.channel(c.freq) + ' (' + c.freq + ')' : '-',
				c.frames || 0,
				(c.probes || []).join(', ') || '-',
				common.ago(c.first_seen, now),
				common.ago(c.last_seen, now)
			].map(function(v) { return E('td', { 'class': 'td' }, v); })));
		});

		dom.content(this.body, [
			E('div', { 'style': 'display:flex;gap:2em;flex-wrap:wrap' }, [
				E('div', { 'class': 'cbi-section', 'style': 'flex:1;min-width:18em' }, [ E('h3', _('By network')), this.summary(_('SSID'), bySsid) ]),
				E('div', { 'class': 'cbi-section', 'style': 'flex:1;min-width:18em' }, [ E('h3', _('By channel')), this.summary(_('Channel'), byChan) ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Not connected (probing)')),
				E('p', { 'class': 'cbi-value-description' },
					_('Devices heard that are connected to no network: phones and laptops looking for one, usually with wildcard probes that name no SSID.')),
				probing.length ? pt : E('p', {}, _('None heard yet.'))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', list.length > SHOW ? _('All clients: the %d heard most recently').format(SHOW) : _('All clients')), t
			])
		]);
	},

	render: function(data) {
		this.data = data || {};
		this.info = E('div', { 'class': 'cbi-map-descr' });
		this.body = E('div');
		var page = E('div', { 'class': 'cbi-map' }, [ E('h2', _('Air Scan: Clients')), common.unavailable(this.data.time ? { phy: 1 } : null), this.info, this.body ]);
		this.draw();
		poll.add(L.bind(function() {
			return common.clients().then(L.bind(function(d) { this.data = d; this.draw(); }, this));
		}, this), 60);
		return page;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
