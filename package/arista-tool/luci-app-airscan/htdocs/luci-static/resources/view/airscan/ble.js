'use strict';
'require view';
'require dom';
'require poll';
'require ui';
'require airscan.common as common';

/* The Bluetooth LE devices the board's BLE controller hears. */

return view.extend({
	load: function() {
		return common.ble();
	},

	draw: function() {
		var d = this.data || {}, list = (d.list || []).slice(), now = Date.now() / 1000;
		list.sort(function(a, b) { return (b.rssi_avg || -999) - (a.rssi_avg || -999); });
		dom.content(this.info, d.error
			? E('span', { 'style': 'color:#c0392b' }, _('BLE survey error: %s').format(d.error))
			: _('%d devices heard on %s (%d look like trackers); last scan %s, %d s long.').format(
				list.length, d.controller || 'hci0', d.trackers || 0, common.ago(d.time, now), d.duration || 0));

		var t = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [ _('Address'), _('Type'), _('Name'), _('Signal'), _('Average'), _('Peak'), _('Maker'), _('Services'), _('Seen'), _('Last heard') ]
				.map(function(h) { return E('th', { 'class': 'th' }, h); }))
		]);
		list.forEach(function(b, i) {
			t.appendChild(E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1) }, [
				E('span', {}, [ E('code', b.addr), b.tracker ? ' ' : '',
					b.tracker ? common.badge(String(b.tracker).replace(/_/g, ' '), '#b8860b') : '' ]),
				b.type || '-',
				b.beacon ? [ b.name || '', b.name ? ' ' : '', common.badge(typeof(b.beacon) == 'object' ? (b.beacon.type || _('beacon')) : String(b.beacon), '#2a7ab0') ] : (b.name || '-'),
				b.rssi != null ? b.rssi + ' dBm' : '-',
				b.rssi_avg != null ? Math.round(b.rssi_avg) + ' dBm' : '-',
				b.rssi_max != null ? b.rssi_max + ' dBm' : '-',
				b.company_name || common.company(b.company),
				(b.uuid16 || []).join(', ') || '-',
				b.seen_total || b.seen || 0,
				common.ago(b.last_seen, now)
			].map(function(v) { return E('td', { 'class': 'td' }, v); })));
		});
		dom.content(this.body, list.length ? t : E('p', {}, _('No devices heard yet.')));
	},

	render: function(data) {
		this.data = data || {};
		this.info = E('div', { 'class': 'cbi-map-descr' });
		this.body = E('div');
		var btn = E('button', { 'class': 'cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, function() {
			return common.bleScan().then(function() {
				ui.addNotification(null, E('p', _('A BLE scan has started.')), 'info');
			});
		}) }, _('Scan now'));
		var page = E('div', { 'class': 'cbi-map' }, [
			E('h2', _('Air Scan: Bluetooth LE')),
			common.unavailable(this.data.time || this.data.error ? { phy: 1 } : null),
			this.info,
			E('div', { 'style': 'margin:.5em 0' }, btn),
			E('div', { 'class': 'cbi-section' }, this.body)
		]);
		this.draw();
		poll.add(L.bind(function() {
			return common.ble().then(L.bind(function(d) { this.data = d; this.draw(); }, this));
		}, this), 60);
		return page;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
