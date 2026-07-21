'use strict';
'require view';
'require fs';
'require poll';

/*
 * Live connections viewer (qosmate-inspired). Renders /usr/sbin/nssqos-conns
 * JSON; rates are client-side deltas between polls — valid for NSS
 * accelerated flows too, since ECM syncs their counters into conntrack.
 * The DSCP class of nssqos-marked connections rides in the ct mark
 * (dscp | 0x80). The table can be frozen, and every column is sortable
 * (click the header to toggle ascending/descending) and filterable.
 */

var DSCP_NAMES = {
	0: 'CS0', 8: 'CS1', 16: 'CS2', 24: 'CS3', 32: 'CS4', 40: 'CS5',
	48: 'CS6', 56: 'CS7', 10: 'AF11', 12: 'AF12', 14: 'AF13',
	18: 'AF21', 20: 'AF22', 22: 'AF23', 26: 'AF31', 28: 'AF32',
	30: 'AF33', 34: 'AF41', 36: 'AF42', 38: 'AF43', 46: 'EF',
	44: 'VA', 1: 'LE'
};

function fmtRate(bps) {
	if (bps >= 1e6) return (bps / 1e6).toFixed(1) + ' Mbit/s';
	if (bps >= 1e3) return (bps / 1e3).toFixed(0) + ' kbit/s';
	return bps.toFixed(0) + ' bit/s';
}

function fmtBytes(b) {
	if (b >= 1073741824) return (b / 1073741824).toFixed(1) + ' GB';
	if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
	if (b >= 1024) return (b / 1024).toFixed(0) + ' KB';
	return b + ' B';
}

function dscpLabel(mark) {
	if (!(mark & 0x80)) return '—';
	var d = mark & 0x7f;
	return DSCP_NAMES[d] || ('0x' + d.toString(16));
}

/* Column model: display string + sort value per connection. */
var COLS = [
	{ title: 'Proto',       disp: function(c) { return c.proto.toUpperCase() + (c.family == 'ipv6' ? '6' : ''); } },
	{ title: 'Source',      disp: function(c) { return c.src + ':' + c.sport; } },
	{ title: 'Destination', disp: function(c) { return c.dst + ':' + c.dport; } },
	{ title: 'State',       disp: function(c) { return c.state || '—'; } },
	{ title: 'DSCP',        disp: function(c) { return dscpLabel(c.mark); } },
	{ title: 'PPS',         disp: function(c) { return c._pps.toFixed(0); },      val: function(c) { return c._pps; } },
	{ title: 'Rate',        disp: function(c) { return fmtRate(c._bps); },        val: function(c) { return c._bps; } },
	{ title: 'Total',       disp: function(c) { return fmtBytes(c._tot); },       val: function(c) { return c._tot; } }
];

var prev = {};
var prevTime = 0;
var lastConns = [];
var frozen = false;
var sortCol = 6;      /* Rate */
var sortDir = -1;     /* descending */
var colFilters = COLS.map(function() { return ''; });

function connKey(c) {
	return c.proto + '|' + c.src + '|' + c.sport + '|' + c.dst + '|' + c.dport;
}

function annotate(conns) {
	var now = Date.now();
	var dt = prevTime ? (now - prevTime) / 1000 : 0;

	conns.forEach(function(c) {
		var p = prev[connKey(c)];
		c._tot = c.orig_bytes + c.repl_bytes;
		var totPkts = c.orig_pkts + c.repl_pkts;
		c._bps = 0; c._pps = 0;
		if (p && dt > 0) {
			c._bps = Math.max(0, (c._tot - p.bytes) * 8 / dt);
			c._pps = Math.max(0, (totPkts - p.pkts) / dt);
		}
	});

	prev = {};
	conns.forEach(function(c) {
		prev[connKey(c)] = { bytes: c._tot, pkts: c.orig_pkts + c.repl_pkts };
	});
	prevTime = now;
}

function sortVal(c, i) {
	return COLS[i].val ? COLS[i].val(c) : COLS[i].disp(c).toLowerCase();
}

/* The header and filter rows are built ONCE; only the data rows are
 * replaced on refresh — rebuilding the inputs would steal keyboard focus
 * on every keystroke and every poll. */
var headerCells = [];
var dataBody = null;

function render_rows() {
	if (!dataBody)
		return;

	headerCells.forEach(function(th, i) {
		var arrow = (i == sortCol) ? (sortDir > 0 ? ' ▲' : ' ▼') : '';
		th.textContent = _(COLS[i].title) + arrow;
	});

	var conns = lastConns.slice();
	conns.sort(function(a, b) {
		var x = sortVal(a, sortCol), y = sortVal(b, sortCol);
		if (x < y) return -1 * sortDir;
		if (x > y) return 1 * sortDir;
		return 0;
	});

	while (dataBody.firstChild) dataBody.removeChild(dataBody.firstChild);

	var shown = 0;
	conns.forEach(function(c) {
		var cells = COLS.map(function(col) { return col.disp(c); });
		for (var i = 0; i < COLS.length; i++) {
			var f = colFilters[i].toLowerCase();
			if (f && cells[i].toLowerCase().indexOf(f) < 0)
				return;
		}
		shown++;
		dataBody.appendChild(E('tr', { 'class': 'tr' }, cells.map(function(v, i) {
			// data-title: mobile-stacked rows keep their column labels
			return E('td', { 'class': 'td', 'data-title': _(COLS[i].title) }, v);
		})));
	});

	if (!shown)
		dataBody.appendChild(E('tr', { 'class': 'tr placeholder' },
			E('td', { 'class': 'td', 'colspan': COLS.length }, _('No matching connections'))));
}

function build_table() {
	headerCells = COLS.map(function(col, i) {
		return E('th', {
			'class': 'th',
			'style': 'cursor:pointer; white-space:nowrap',
			'title': _('Click to sort'),
			'click': function() {
				if (sortCol == i) sortDir = -sortDir;
				else { sortCol = i; sortDir = (COLS[i].val ? -1 : 1); }
				render_rows();
			}
		}, _(col.title));
	});

	var filters = E('tr', { 'class': 'tr' }, COLS.map(function(col, i) {
		return E('td', { 'class': 'td' }, E('input', {
			'type': 'text',
			'class': 'cbi-input-text',
			'style': 'width:100%; min-width:3em',
			'placeholder': _('filter'),
			'value': colFilters[i],
			'input': function(ev) {
				colFilters[i] = ev.target.value;
				render_rows();
			}
		}));
	}));

	dataBody = E('tbody');
	return E('div', { 'style': 'overflow-x:auto' }, E('table', { 'class': 'table' }, [
		E('thead', {}, [
			E('tr', { 'class': 'tr table-titles' }, headerCells),
			filters
		]),
		dataBody
	]));
}

return view.extend({
	load: function() {
		return fs.exec('/usr/sbin/nssqos-conns');
	},

	render: function(res) {
		var wrap = E('div');
		var body = E('div', { 'id': 'nssconns' });

		function ingest(r) {
			if (frozen)
				return;
			var conns = [];
			try { conns = JSON.parse(r.stdout || '[]'); } catch (e) {}
			annotate(conns);
			lastConns = conns;
			render_rows();
		}

		var freezeBtn = E('button', {
			'class': 'btn cbi-button',
			'style': 'margin-bottom:10px',
			'click': function(ev) {
				frozen = !frozen;
				ev.target.textContent = frozen ? _('Resume') : _('Freeze');
				ev.target.className = frozen ? 'btn cbi-button cbi-button-positive'
							     : 'btn cbi-button';
				if (!frozen) {
					/* skip one rate window so the long pause
					   does not dilute the next delta */
					prev = {}; prevTime = 0;
				}
			}
		}, _('Freeze'));

		body.appendChild(build_table());
		ingest(res);
		poll.add(function() {
			return fs.exec('/usr/sbin/nssqos-conns').then(ingest);
		}, 3);

		wrap.appendChild(E('h2', {}, _('NSS Connections')));
		wrap.appendChild(E('div', { 'class': 'cbi-map-descr' },
			_('Live connection table with rates computed between polls — ' +
			  'NSS-accelerated flows included (their counters are synced back ' +
			  'from the firmware). The DSCP column shows classes applied by ' +
			  'the QoS marking rules. Click a column header to sort; the row ' +
			  'of boxes filters each column.')));
		wrap.appendChild(freezeBtn);
		wrap.appendChild(body);
		return wrap;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
