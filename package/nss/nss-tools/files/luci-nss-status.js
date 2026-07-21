'use strict';
'require view';
'require fs';
'require poll';
'require dom';

// Renders the output of `nss-status -j` (the single source of truth for
// NSS plane health); this view only formats, it computes nothing itself.

function getStatus() {
	return fs.exec('/usr/sbin/nss-status', ['-j']).then(function(res) {
		if (res.code !== 0)
			throw new Error(res.stderr || 'nss-status failed');
		return JSON.parse(res.stdout);
	});
}

function stateBadge(state) {
	var map = {
		active:  ['#2e7d32', _('Offload active')],
		stalled: ['#c62828', _('Firmware armed but heartbeat silent — check the system log; a reboot returns to the host stack')],
		host:    ['#f9a825', _('Host stack (NSS plane not armed)')]
	};
	var m = map[state] || ['#757575', state];
	return E('div', {
		'style': 'display:inline-block;padding:.3em .8em;border-radius:.3em;color:#fff;font-weight:bold;background:' + m[0]
	}, m[1]);
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'style': 'min-width:30%' }, label),
		E('td', { 'class': 'td left' }, value)
	]);
}

function onoff(v) {
	return v ? _('loaded') : _('not loaded');
}

function offl(pkts, pct) {
	return String(pkts) + ' (' + pct + '%)';
}

function renderStatus(d) {
	var cores = (d.cores || []).map(function(c) {
		return 'core' + c.core + ' ' + c.avg + '%';
	}).join(', ');

	var igs = (d.sqm.igs || []).map(function(i) {
		return i.port + (i.active ? ' ✓' : ' ✗');
	}).join('  ');

	// d.mesh: {configured, fw_capable} - 802.11s mesh only offloads on the
	// 11.4 firmware line; everywhere else it stays on the host path.
	var mesh = d.mesh || {};
	var wifi = d.wifi_offload == 1 ?
	               (mesh.configured ? _('NSS offload active (wifili, 802.11s mesh offloaded)')
	                                : _('NSS offload active (wifili)')) :
	           d.wifi_offload == 0 ?
	               (mesh.configured && mesh.fw_capable ? _('host mode (mesh configured; firmware supports mesh offload)') :
	                mesh.configured ? _('host mode (mesh configured; mesh offload needs the 11.4 NSS firmware)') :
	                                  _('host mode')) :
	           _('ath11k not loaded');

	// data-title lets the theme stack table rows into labeled cards on
	// narrow (mobile) screens.
	var portRows = (d.ports || []).map(function(p) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td', 'data-title': _('Port') }, p.netdev),
			E('td', { 'class': 'td', 'data-title': 'if_num' }, String(p.ifnum)),
			E('td', { 'class': 'td', 'data-title': _('Started') }, p.started ? _('yes') : _('no')),
			E('td', { 'class': 'td', 'data-title': _('TX offloaded') }, offl(p.tx_offloaded, p.tx_offload_pct)),
			E('td', { 'class': 'td', 'data-title': _('TX via host') }, String(p.tx_host_pkts)),
			E('td', { 'class': 'td', 'data-title': _('RX offloaded') }, offl(p.rx_offloaded, p.rx_offload_pct)),
			E('td', { 'class': 'td', 'data-title': _('RX to host') }, String(p.rx_host_pkts))
		]);
	});

	return E('div', {}, [
		E('p', {}, stateBadge(d.state)),
		E('table', { 'class': 'table' }, [
			row(_('Firmware'), d.fw_version + ' (fw_mask ' + d.fw_mask + ')'),
			row(_('Heartbeat'), d.heartbeat_pps + ' ' + _('pkts/s (NSS→host)')),
			row(_('NSS core load'), cores || '-'),
			row(_('ECM accelerated connections'), d.ecm.loaded
				? d.ecm.ipv4_accel + ' IPv4 / ' + d.ecm.ipv6_accel + ' IPv6'
				: _('ECM not loaded')),
			row(_('Bridge offload (bridge-mgr)'), onoff(d.modules.bridge_mgr)),
			row(_('Multicast snooping (qca-mcs)'), onoff(d.modules.mcs)),
			row(_('PPPoE manager'), onoff(d.modules.pppoe)),
			row(_('Wi-Fi data path'), wifi),
			row(_('SQM shaper'), d.sqm.active
				? _('nsstbl on') + ' ' + d.sqm.device + (igs ? ' — ' + _('upload IGS:') + ' ' + igs : '')
				: _('no NSS shaper on') + ' ' + d.sqm.device),
			(d.sqm.fastlane && d.sqm.fastlane.active)
				? row(_('Fast lane'), _('egress') + ' ' + d.sqm.fastlane.egress_pkts + ' pkts / ' +
					_('ingress') + ' ' + d.sqm.fastlane.ingress_pkts + ' pkts — ' +
					(d.sqm.qos_marking_rules || 0) + ' ' + _('marking rules'))
				: E('tr', { 'style': 'display:none' })
		]),
		E('h3', {}, _('Firmware-attached ports')),
		E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Port')),
				E('th', { 'class': 'th' }, 'if_num'),
				E('th', { 'class': 'th' }, _('Started')),
				E('th', { 'class': 'th' }, _('TX offloaded')),
				E('th', { 'class': 'th' }, _('TX via host')),
				E('th', { 'class': 'th' }, _('RX offloaded')),
				E('th', { 'class': 'th' }, _('RX to host'))
			])
		].concat(portRows.length ? portRows : [
			E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'colspan': '7' }, _('No ports attached — host stack'))
			])
		])),
		E('p', {}, E('em', {}, _('Offloaded packets are forwarded by the firmware and never reach the host. "via host" / "to host" is the host↔firmware exception path — what was not offloaded — so low numbers there mean offload is working. Broadcast and multicast discovery, connection setup, short-lived flows and traffic addressed to the router itself always take that path: a flow is only offloaded once it is established, so a quiet port that carries little else shows a low offloaded share. Port totals are on Status → Overview.')))
	]);
}

return view.extend({
	load: getStatus,

	render: function(data) {
		var container = E('div', {}, renderStatus(data));

		poll.add(function() {
			return getStatus().then(function(d) {
				dom.content(container, renderStatus(d));
			});
		}, 10);

		return E('div', {}, [
			E('h2', {}, _('NSS Offload')),
			container
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
