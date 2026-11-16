'use strict';
'require baseclass';
'require rpc';

/* What the airscan service publishes on ubus, and helpers its pages share. */

var callStatus = rpc.declare({ object: 'airscan', method: 'status', expect: { } });
var callSpectrum = rpc.declare({ object: 'airscan', method: 'spectrum', params: [ 'band' ], expect: { } });
var callBss = rpc.declare({ object: 'airscan', method: 'bss', expect: { } });
var callClients = rpc.declare({ object: 'airscan', method: 'clients', expect: { } });
var callEvents = rpc.declare({ object: 'airscan', method: 'events', expect: { } });
var callBle = rpc.declare({ object: 'airscan', method: 'ble', expect: { } });
var callSweep = rpc.declare({ object: 'airscan', method: 'sweep', params: [ 'band' ], expect: { } });
var callScan = rpc.declare({ object: 'airscan', method: 'scan', expect: { } });
var callBleScan = rpc.declare({ object: 'airscan', method: 'ble_scan', expect: { } });

var SVGNS = 'http://www.w3.org/2000/svg';

var BANDS = {
	'2g': _('2.4 GHz'),
	'5g': _('5 GHz'),
	'6g': _('6 GHz')
};

/* known BLE company identifiers (Bluetooth SIG assigned numbers) */
var COMPANIES = {
	0x0006: 'Microsoft', 0x004c: 'Apple', 0x0075: 'Samsung', 0x00e0: 'Google',
	0x0087: 'Garmin', 0x0157: 'Huami', 0x0171: 'Amazon', 0x01da: 'Logitech',
	0x02e5: 'Espressif', 0x0059: 'Nordic Semiconductor', 0x000d: 'Texas Instruments',
	0x038f: 'Xiaomi', 0x0822: 'Tile'
};

return baseclass.extend({
	status: callStatus,
	spectrum: callSpectrum,
	bss: callBss,
	clients: callClients,
	events: callEvents,
	ble: callBle,
	sweep: callSweep,
	scan: callScan,
	bleScan: callBleScan,

	bands: BANDS,

	bandOf: function(freq) {
		if (freq < 3000) return '2g';
		if (freq < 5950) return '5g';
		return '6g';
	},

	channel: function(freq) {
		if (freq == 2484) return 14;
		if (freq < 3000) return (freq - 2407) / 5;
		if (freq < 5950) return (freq - 5000) / 5;
		return (freq - 5950) / 5;
	},

	company: function(id) {
		if (id == null) return '';
		var hex = '0x' + ('0000' + id.toString(16)).slice(-4);
		return COMPANIES[id] ? COMPANIES[id] + ' (' + hex + ')' : hex;
	},

	/* "3 min ago", relative to the time the service reported (its clock,
	   not the browser's) */
	ago: function(t, now) {
		if (!t || !now) return '-';
		var d = Math.max(0, now - t);
		if (d < 90) return _('%d s ago').format(d);
		if (d < 5400) return _('%d min ago').format(Math.round(d / 60));
		return _('%d h ago').format(Math.round(d / 3600));
	},

	time: function(t) {
		return t ? new Date(t * 1000).toLocaleString() : '-';
	},

	/* an SVG element: tag, attributes, children (elements or text) */
	svg: function(tag, attrs, children) {
		var e = document.createElementNS(SVGNS, tag);
		for (var k in attrs || {})
			if (attrs[k] != null)
				e.setAttribute(k, attrs[k]);
		(Array.isArray(children) ? children : (children != null ? [ children ] : [])).forEach(function(c) {
			e.appendChild(typeof(c) == 'object' ? c : document.createTextNode(String(c)));
		});
		return e;
	},

	/* 0..1 -> green, amber, red */
	busyColor: function(v) {
		v = Math.max(0, Math.min(1, v));
		return 'hsl(' + Math.round(120 - 120 * v) + ', 70%, 45%)';
	},

	/* 0..1 -> dark blue, cyan, yellow, red (a spectrum analyser's density scale) */
	heatColor: function(v) {
		v = Math.max(0, Math.min(1, v));
		return 'hsl(' + Math.round(240 - 240 * v) + ', 90%, ' + Math.round(18 + 42 * Math.sqrt(v)) + '%)';
	},

	/* a warning when the service could not be read, or '' (E() would print a
	   null child as "null") */
	unavailable: function(st) {
		if (st && st.phy)
			return '';
		return E('div', { 'class': 'alert-message warning' },
			_('Cannot read the airscan service. Is it running (System > Startup)? After installing this page, log in again.'));
	},

	badge: function(text, color) {
		return E('span', {
			'style': 'display:inline-block;padding:0 .5em;border-radius:.6em;font-size:90%;color:#fff;background:' + color
		}, text);
	}
});
