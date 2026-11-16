'use strict';
'require view';
'require dom';
'require poll';
'require ui';
'require airscan.common as common';

/* The scan radio's spectral survey: how busy each 20 MHz channel is, how the
   power of its samples is spread (the density view of a spectrum analyser),
   and one channel's FFT. */

var W = 960, PAD_L = 44, PAD_R = 8;
var DB_TOP = -20, DB_BOT = -110;

return view.extend({
	load: function() {
		return Promise.all([ common.status(), common.spectrum(), common.bss() ]);
	},

	windows: function() {
		var band = this.band, list = (this.spec.windows || []).slice();
		list.sort(function(a, b) { return a.center - b.center; });
		return band == 'all' ? list : list.filter(function(w) { return w.band == band; });
	},

	/* x position of each window, with a gap between bands */
	layout: function(list) {
		var gaps = 0, prev = null;
		list.forEach(function(w) { if (prev && w.band != prev) gaps++; prev = w.band; });
		var slots = list.length + gaps * 0.8, sw = (W - PAD_L - PAD_R) / Math.max(1, slots);
		var x = PAD_L, out = [];
		prev = null;
		list.forEach(function(w) {
			if (prev && w.band != prev) x += sw * 0.8;
			out.push({ w: w, x: x, sw: sw });
			x += sw;
			prev = w.band;
		});
		return out;
	},

	networksOn: function(w) {
		return (this.bssData.list || []).filter(function(b) {
			return Math.abs(b.freq - w.center) <= (w.width || 20) / 2;
		});
	},

	select: function(w) {
		this.selected = w;
		this.draw();
	},

	axisLabels: function(slots, y) {
		var g = [];
		slots.forEach(function(s, i) {
			var step = slots.length > 30 ? 2 : 1;
			if (i % step) return;
			g.push(common.svg('text', { x: s.x + s.sw / 2, y: y, 'font-size': 10, 'text-anchor': 'middle', fill: 'currentColor' },
				common.channel(s.w.center)));
		});
		return g;
	},

	drawOccupancy: function(slots) {
		var H = 210, top = 10, bot = 180, self = this, g = [];
		[ 0, 25, 50, 75, 100 ].forEach(function(p) {
			var y = bot - (bot - top) * p / 100;
			g.push(common.svg('line', { x1: PAD_L, x2: W - PAD_R, y1: y, y2: y, stroke: 'currentColor', 'stroke-opacity': 0.15 }));
			g.push(common.svg('text', { x: PAD_L - 6, y: y + 3, 'font-size': 10, 'text-anchor': 'end', fill: 'currentColor' }, p + '%'));
		});
		slots.forEach(function(s) {
			var v = (s.w.busy_pct || 0) / 100, h = (bot - top) * v;
			var bar = common.svg('rect', {
				x: s.x + s.sw * 0.12, y: bot - h, width: s.sw * 0.76, height: Math.max(h, 1),
				fill: common.busyColor(v), stroke: self.selected === s.w ? 'currentColor' : null, 'stroke-width': 2,
				style: 'cursor:pointer'
			}, common.svg('title', {}, _('Channel %d (%d MHz): %.1f%% busy, noise %d dBm').format(
				common.channel(s.w.center), s.w.center, s.w.busy_pct, s.w.noise)));
			bar.addEventListener('click', function() { self.select(s.w); });
			g.push(bar);
		});
		g = g.concat(this.axisLabels(slots, bot + 14));
		g.push(common.svg('text', { x: PAD_L, y: H - 2, 'font-size': 10, fill: 'currentColor' },
			_('share of samples above %d dBm per 20 MHz (+3 dB per doubling of a window\'s width)').format(this.spec.busy_dbm_20mhz || -82)));
		return common.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, style: 'width:100%;height:auto' }, g);
	},

	drawDensity: function(slots) {
		var H = 250, top = 8, bot = 220, self = this, g = [];
		var dy = (bot - top) / (DB_TOP - DB_BOT);
		g.push(common.svg('rect', { x: PAD_L, y: top, width: W - PAD_L - PAD_R, height: bot - top, fill: common.heatColor(0) }));
		slots.forEach(function(s) {
			var w = s.w, hist = w.pwr_hist || [], n = w.samples || 1;
			hist.forEach(function(c, i) {
				if (!c) return;
				var dbm = (w.pwr_lo || DB_BOT) + i;
				if (dbm > DB_TOP || dbm < DB_BOT) return;
				/* log scale: one sample in 2048 is still visible */
				var v = Math.log(1 + c) / Math.log(1 + n);
				g.push(common.svg('rect', {
					x: s.x + s.sw * 0.06, y: bot - (dbm - DB_BOT + 1) * dy, width: s.sw * 0.88, height: Math.max(dy, 1),
					fill: common.heatColor(v)
				}, common.svg('title', {}, _('Channel %d: %d samples at %d dBm').format(common.channel(w.center), c, dbm))));
			});
			var hit = common.svg('rect', { x: s.x, y: top, width: s.sw, height: bot - top, fill: 'transparent', style: 'cursor:pointer' });
			hit.addEventListener('click', function() { self.select(w); });
			g.push(hit);
			if (self.selected === w)
				g.push(common.svg('rect', { x: s.x, y: top, width: s.sw, height: bot - top, fill: 'none', stroke: '#fff', 'stroke-width': 1.5 }));
		});
		for (var d = DB_BOT; d <= DB_TOP; d += 10) {
			var y = bot - (d - DB_BOT) * dy;
			g.push(common.svg('text', { x: PAD_L - 6, y: y + 3, 'font-size': 10, 'text-anchor': 'end', fill: 'currentColor' }, d));
		}
		/* the busy threshold of each window: +3 dB per doubling of its width */
		var busy = this.spec.busy_dbm_20mhz || -82;
		slots.forEach(function(s) {
			var thr = busy + ({ 20: 0, 40: 3, 80: 6, 160: 9 }[s.w.width] || 0), by = bot - (thr - DB_BOT) * dy;
			g.push(common.svg('line', { x1: s.x, x2: s.x + s.sw, y1: by, y2: by, stroke: '#fff', 'stroke-dasharray': '4 3', 'stroke-opacity': 0.7 }));
		});
		g = g.concat(this.axisLabels(slots, bot + 14));
		g.push(common.svg('text', { x: PAD_L, y: H - 2, 'font-size': 10, fill: 'currentColor' },
			_('power of each sample (dBm), coloured by how many samples; dashed: the busy threshold')));
		return common.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, style: 'width:100%;height:auto' }, g);
	},

	drawFft: function(w) {
		if (!w)
			return E('p', { 'class': 'cbi-value-description' }, _('Click a channel above to see its spectrum.'));
		var H = 240, top = 10, bot = 210, g = [];
		var lo = -130, hi = -20, n = (w.avg || []).length || 1;
		var x = function(i) { return PAD_L + (W - PAD_L - PAD_R) * i / Math.max(1, n - 1); };
		var y = function(v) { return bot - (bot - top) * (Math.max(lo, Math.min(hi, v)) - lo) / (hi - lo); };
		for (var d = lo; d <= hi; d += 10) {
			g.push(common.svg('line', { x1: PAD_L, x2: W - PAD_R, y1: y(d), y2: y(d), stroke: 'currentColor', 'stroke-opacity': 0.12 }));
			g.push(common.svg('text', { x: PAD_L - 6, y: y(d) + 3, 'font-size': 10, 'text-anchor': 'end', fill: 'currentColor' }, d));
		}
		var path = function(vals) {
			return vals.map(function(v, i) { return (i ? 'L' : 'M') + x(i).toFixed(1) + ' ' + y(v).toFixed(1); }).join(' ');
		};
		if (w.max) g.push(common.svg('path', { d: path(w.max), fill: 'none', stroke: '#e8a33d', 'stroke-width': 1.2 }));
		if (w.avg) g.push(common.svg('path', { d: path(w.avg), fill: 'none', stroke: '#3d8ee8', 'stroke-width': 1.6 }));
		if (w.noise != null)
			g.push(common.svg('line', { x1: PAD_L, x2: W - PAD_R, y1: y(w.noise), y2: y(w.noise), stroke: '#888', 'stroke-dasharray': '4 3' }));
		var half = (w.width || 20) / 2;
		[ -half, -half / 2, 0, half / 2, half ].forEach(function(o) {
			var xi = PAD_L + (W - PAD_L - PAD_R) * (o + half) / (2 * half);
			g.push(common.svg('text', { x: xi, y: bot + 14, 'font-size': 10, 'text-anchor': 'middle', fill: 'currentColor' },
				(w.center + o) + ' MHz'));
		});
		g.push(common.svg('text', { x: PAD_L, y: H - 2, 'font-size': 10, fill: 'currentColor' },
			_('blue: average, amber: peak, grey: noise floor (dBm per FFT bin)')));
		return common.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, style: 'width:100%;height:auto' }, g);
	},

	drawTable: function(list) {
		var self = this;
		var rows = list.map(function(w) {
			var nets = self.networksOn(w), best = null;
			nets.forEach(function(b) { if (best == null || b.signal > best) best = b.signal; });
			return [
				common.channel(w.center), w.center, common.bands[w.band] || w.band,
				E('span', { 'style': 'color:' + common.busyColor((w.busy_pct || 0) / 100) }, '%.1f%%'.format(w.busy_pct || 0)),
				w.noise + ' dBm',
				nets.length,
				best != null ? best + ' dBm' : '-',
				nets.map(function(b) { return b.ssid || _('(hidden)'); }).filter(function(v, i, a) { return a.indexOf(v) == i; }).join(', ')
			];
		});
		var table = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				_('Channel'), _('MHz'), _('Band'), _('Busy'), _('Noise'), _('Networks'), _('Strongest'), _('SSIDs')
			].map(function(t) { return E('th', { 'class': 'th' }, t); }))
		]);
		rows.forEach(function(r, i) {
			var tr = E('tr', { 'class': 'tr cbi-rowstyle-' + (i % 2 + 1), 'style': 'cursor:pointer' },
				r.map(function(c) { return E('td', { 'class': 'td' }, c); }));
			tr.addEventListener('click', function() { self.select(list[i]); });
			table.appendChild(tr);
		});
		return table;
	},

	draw: function() {
		var list = this.windows(), slots = this.layout(list);
		if (this.selected && list.indexOf(this.selected) < 0)
			this.selected = null;
		var st = this.statusData || {}, spec = this.spec || {}, now = Date.now() / 1000;
		var next = st.next && st.next.spectral;
		dom.content(this.info, [
			_('Scan radio %s; last sweep %s (%s), %d channels; next sweep in about %s.').format(
				spec.phy || st.phy || '?', common.time(spec.time), common.ago(spec.time, now),
				(spec.windows || []).length,
				next ? _('%d min').format(Math.max(0, Math.round((next - now) / 60))) : '-')
		]);
		if (!list.length) {
			dom.content(this.charts, E('p', {}, _('No spectral data yet: the first sweep runs a few minutes after airscan starts.')));
			return;
		}
		dom.content(this.charts, [
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Channel occupancy')), this.drawOccupancy(slots) ]),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Power density')), this.drawDensity(slots) ]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', this.selected ? _('Channel %d (%d MHz)').format(common.channel(this.selected.center), this.selected.center) : _('Channel detail')),
				this.drawFft(this.selected)
			]),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Channels')), this.drawTable(list) ])
		]);
	},

	refresh: function() {
		return Promise.all([ common.status(), common.spectrum(), common.bss() ]).then(L.bind(function(d) {
			var sel = this.selected;
			this.statusData = d[0]; this.spec = d[1]; this.bssData = d[2];
			this.selected = null;
			if (sel)
				(this.spec.windows || []).forEach(L.bind(function(w) { if (w.center == sel.center) this.selected = w; }, this));
			this.draw();
		}, this));
	},

	render: function(data) {
		this.statusData = data[0] || {};
		this.spec = data[1] || {};
		this.bssData = data[2] || {};
		this.band = 'all';
		this.selected = null;

		var self = this;
		var bandSel = E('select', { 'class': 'cbi-input-select', 'change': function(ev) {
			self.band = ev.target.value;
			self.draw();
		} }, [ E('option', { 'value': 'all' }, _('All bands')) ].concat(Object.keys(common.bands).map(function(b) {
			return E('option', { 'value': b }, common.bands[b]);
		})));

		var sweepBtn = E('button', { 'class': 'cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, function() {
			/* '' is every band: a null argument is refused */
			return common.sweep(self.band == 'all' ? '' : self.band).then(function(r) {
				if (!r || typeof(r) != 'object' || r.error) {
					ui.addNotification(null, E('p', _('The sweep could not be started: %s').format(r && r.error ? r.error : r)), 'error');
					return;
				}
				ui.addNotification(null, E('p', _('A sweep has started; the charts update when it finishes (about a minute per band).')), 'info');
			});
		}) }, _('Sweep now'));

		this.info = E('div', { 'class': 'cbi-map-descr' });
		this.charts = E('div');

		var page = E('div', { 'class': 'cbi-map' }, [
			E('h2', _('Air Scan: Spectrum')),
			common.unavailable(this.statusData),
			E('div', { 'class': 'cbi-map-descr' },
				_('The dedicated scan radio samples each channel with the spectral scan (20 MHz windows on 2.4 and 5 GHz, wider ones on 6 GHz); the serving radios never leave their channels.')),
			this.info,
			E('div', { 'style': 'display:flex;gap:.5em;align-items:center;margin:.5em 0' }, [ bandSel, sweepBtn ]),
			this.charts
		]);
		this.draw();
		poll.add(L.bind(this.refresh, this), 60);
		return page;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
