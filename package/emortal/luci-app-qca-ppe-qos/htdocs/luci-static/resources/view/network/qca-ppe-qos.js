'use strict';
'require view';
'require form';
'require network';
'require uci';
'require rpc';
'require poll';
'require ui';
'require tools.widgets as widgets';

const callStatus = rpc.declare({
	object: 'luci.qca-ppe-qos',
	method: 'status',
	expect: { '': {} }
});

function updateShaperStatus(table, warnings, status) {
	const errors = {
		'invalid-configuration': _('Invalid or duplicate port configuration; existing limits were kept.'),
		'hardware-status-unavailable': _('Hardware status is unavailable; no limit was applied.'),
		'sqm-conflict': _('SQM manages this port. Disable its queue before enabling port limiting.'),
		'qdisc-conflict': _('Another queue configuration manages this port; it was kept.'),
		'ownership-conflict': _('Queue ownership changed; the existing queue was kept.'),
		'qdisc-unavailable': _('Unable to read the port queue.'),
		'qdisc-rejected': _('The port rejected the requested queue settings.'),
		'hardware-rejected': _('Hardware did not accept the limit; the previous settings were restored.'),
		'remove-failed': _('Unable to remove the managed queue.'),
		'hardware-remove-failed': _('Hardware still reports an active limit after removal.'),
		'rollback-failed': _('Unable to restore the previous hardware limit. Check the port before retrying.')
	};
	const rows = (status.ports || []).map((p) => {
		const state = !p.available ? _('Hardware status unavailable')
			: p.verified ? _('Hardware limit active')
			: p.active && !p.managed ? _('Managed by another service')
			: p.active ? _('Hardware state does not match configuration')
			: p.conflict ? _('Another queue configuration is active') : _('No hardware limit');
		return [ p.device, p.carrier ? _('Link up') : _('Link down'), state,
			p.active ? '%s kbit/s'.format(p.rate) : '—',
			p.active ? '%s / %s'.format(p.burst, p.buffers * 256) : '—' ];
	});
	const messages = Object.entries(status.errors || {}).map(([device, error]) =>
		E('p', { 'class': 'alert-message warning' }, '%s: %s'.format(device, errors[error] || error)));
	table.update(rows);
	warnings.replaceChildren(...messages);
}

function renderShapers(status) {
	const m = new form.Map('qca-ppe-qos', _('PPE Hardware QoS'),
		_('Limits all traffic leaving a physical PPE port. A WAN port limit controls upload; a LAN port limit also affects local traffic to that port. This does not provide a shared download limit across LAN and Wi-Fi or per-host fair queueing.'));
	const s = m.section(form.TableSection, 'shaper', _('Port rate limits'),
		_('Only physical PPE ports with hardware status are selectable. Existing SQM and other queue configurations take precedence.') + ' ' +
		_('Hardware rounds the requested queue depth to complete frames and available port capacity.'));
	s.anonymous = true;
	s.addremove = true;
	let o = s.option(form.Flag, 'enabled', _('Enable'));
	o.default = '0';
	o.rmempty = false;
	o = s.option(form.ListValue, 'device', _('Physical port'));
	(status.ports || []).filter((p) => p.available).forEach((p) => o.value(p.device));
	o.rmempty = false;
	o.validate = function(section_id, value) {
		const enabled = s.formvalue(section_id, 'enabled') != '0';
		const duplicate = enabled && s.cfgsections().some((sid) => sid != section_id &&
			s.formvalue(sid, 'enabled') != '0' && this.formvalue(sid) == value);
		return duplicate ? _('Only one enabled limit is allowed per port') : true;
	};
	o = s.option(form.Value, 'rate', _('Rate (kbit/s)'));
	o.datatype = 'and(uinteger,range(64,10000000))';
	o.placeholder = '100000';
	o.rmempty = false;
	o = s.option(form.Value, 'burst', _('Burst (bytes)'));
	o.datatype = 'and(uinteger,range(2048,1048576))';
	o.default = '65536';
	o.rmempty = false;
	o = s.option(form.Value, 'limit', _('Requested queue limit (bytes)'));
	o.datatype = 'and(uinteger,range(4096,4194304))';
	o.default = '131072';
	o.rmempty = false;
	o.validate = function(section_id, value) {
		return +value >= +(s.formvalue(section_id, 'burst') || 65536)
			? true : _('Queue limit must be at least the burst size');
	};
	return m.render().then((formNode) => {
		const table = new ui.Table([
			_('Physical port'), _('Link'), _('Hardware status'), _('Rate'), _('Burst / hardware queue (bytes)')
		], { id: 'qca-ppe-qos-hardware-status' });
		const live = E('div', { 'class': 'cbi-section cbi-tblsection' }, [
			E('h3', _('Hardware status')),
			table.render()
		]);
		const warnings = E('div');
		updateShaperStatus(table, warnings, status);
		poll.add(() => callStatus().then((next) => updateShaperStatus(table, warnings, next)).catch(() => {
			table.update([], E('em', _('Unable to read hardware status.')));
			warnings.replaceChildren();
		}), 5);
		return E('div', [ live, warnings, formNode ]);
	});
}

/* The class keywords the qca-ppe-qos init accepts, with their DSCP values. */
const CLASSES = [
	[ 'cs0',  0,  _('Best effort') ],
	[ 'le',   1,  _('Lower effort') ],
	[ 'cs1',  8,  _('Scavenger, bulk') ],
	[ 'af11', 10, _('High-throughput data') ],
	[ 'af12', 12 ],
	[ 'af13', 14 ],
	[ 'cs2',  16, _('Operations, administration') ],
	[ 'af21', 18, _('Low-latency data') ],
	[ 'af22', 20 ],
	[ 'af23', 22 ],
	[ 'cs3',  24, _('Broadcast video') ],
	[ 'af31', 26, _('Multimedia streaming') ],
	[ 'af32', 28 ],
	[ 'af33', 30 ],
	[ 'cs4',  32, _('Real-time interactive, games') ],
	[ 'af41', 34, _('Multimedia conferencing, calls') ],
	[ 'af42', 36 ],
	[ 'af43', 38 ],
	[ 'cs5',  40, _('Signalling, DNS') ],
	[ 'va',   44, _('Voice admit') ],
	[ 'ef',   46, _('Telephony, voice') ],
	[ 'cs6',  48, _('Network control') ],
	[ 'cs7',  56, _('Reserved') ]
];

/* A shaped port serves priorities 4 to 7 ahead of its bulk band, 0 to 3. */
const PRIO_BAND = 4;

function class_dscp(value) {
	const c = CLASSES.find((c) => c[0] == value);

	if (c)
		return c[1];

	if (/^[0-9]{1,2}$/.test(value) && +value <= 63)
		return +value;

	return null;
}

function class_label(dscp) {
	const c = CLASSES.find((c) => c[1] == dscp);

	return c ? c[0].toUpperCase() : String(dscp);
}

function prio_label(prio) {
	return (prio >= PRIO_BAND)
		? _('%d - priority band').format(prio)
		: _('%d - bulk band').format(prio);
}

function prio_values(o) {
	for (let i = 0; i <= 7; i++)
		o.value(i, prio_label(i));
}

/* The priority the DSCP map gives a mark; null leaves it in the bulk band. */
function dscp_prio(dscp) {
	const s = uci.sections('qca-ppe-qos', 'dscp').find((s) => s.dscp != null && +s.dscp == dscp);

	return s ? +s.prio : null;
}

function vars(values) {
	const nodes = [];

	values.forEach((v, i) => {
		if (i > 0)
			nodes.push(', ');
		nodes.push(E('var', v));
	});

	return nodes;
}

function rule_match_txt(section_id) {
	const get = (opt) => L.toArray(uci.get('qca-ppe-qos', section_id, opt));
	const proto = uci.get('qca-ppe-qos', section_id, 'proto') || 'tcpudp';
	const src_ip = get('src_ip'), src_port = get('src_port');
	const dest_ip = get('dest_ip'), dest_port = get('dest_port');
	const lines = [];

	lines.push(vars(proto == 'tcpudp' ? [ 'TCP', 'UDP' ] : [ proto.toUpperCase() ]));

	if (src_ip.length || src_port.length) {
		const from = [ _('From') + ' ' ];

		from.push(...(src_ip.length ? vars(src_ip) : [ _('any host') ]));
		if (src_port.length)
			from.push(', ' + _('port') + ' ', ...vars(src_port));
		lines.push(from);
	}

	const to = [ _('To') + ' ' ];

	to.push(...(dest_ip.length ? vars(dest_ip) : [ _('any host') ]));
	if (dest_port.length)
		to.push(', ' + _('port') + ' ', ...vars(dest_port));
	lines.push(to);

	return E('small', lines.reduce((acc, l, i) => acc.concat(i ? [ E('br') ] : [], l), []));
}

function rule_class_txt(section_id) {
	const dscp = class_dscp(uci.get('qca-ppe-qos', section_id, 'class'));

	if (dscp == null)
		return E('em', _('Invalid class'));

	const prio = dscp_prio(dscp);

	return E('span', [
		E('var', class_label(dscp)), ' (%d)'.format(dscp), E('br'),
		E('small', (prio != null) ? _('Priority %s').format(prio_label(prio)) : _('Unmapped - bulk band'))
	]);
}

return view.extend({
	load() {
		return Promise.all([
			network.getDevices(),
			uci.load('qca-ppe-qos'),
			L.resolveDefault(callStatus(), {})
		]);
	},

	render([ devices, config, status ]) {
		if (status.platform == 'be')
			return renderShapers(status);
		if (!status.classifier) {
			this.handleSaveApply = this.handleSave = this.handleReset = null;
			return E('p', { 'class': 'alert-message warning' }, _('PPE QoS capabilities are unavailable. Check the service and reload this page.'));
		}
		/* DSA user ports; the conduit is a switch device too, but has no parent. */
		const switch_ports = devices
			.filter((d) => d.getType() == 'switch' && d.getParent())
			.map((d) => d.getName());
		let m, s, o;

		m = new form.Map('qca-ppe-qos', _('PPE Hardware QoS'),
			_('Sorts traffic into the egress queues of the switch. Marking rules put a DSCP on a flow, the DSCP map turns a DSCP into a priority, and priorities 4 to 7 are served ahead of the bulk band of a shaped port. The switch does all of this itself, so it covers flows the PPE has offloaded. Shaper rates belong to SQM.'));

		s = m.section(form.NamedSection, 'global', 'global');
		s.tab('general', _('General Settings'));
		s.tab('advanced', _('Advanced Settings'));

		o = s.taboption('general', form.Flag, 'active', _('Enable hardware QoS'),
			_('Apply marking rules, DSCP maps and small packet priorities.'));
		o.default = o.enabled;
		o.rmempty = false;

		o = s.taboption('general', form.Flag, 'enabled', _('Enable marking rules'),
			_('The DSCP map and the small packet priority stay in effect either way.'));
		o.default = o.enabled;
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'small_pkt_len', _('Small packet length'),
			_('Packets up to this IP length take the small packet priority whatever their marking: ACKs, DNS, VoIP and game traffic. Keep it below a full-size segment. 0 disables.'));
		o.datatype = 'and(uinteger,range(0,1500))';
		o.placeholder = '1000';

		o = s.taboption('general', form.ListValue, 'small_pkt_prio', _('Small packet priority'));
		prio_values(o);
		o.default = '5';

		o = s.taboption('advanced', widgets.DeviceSelect, 'wan_port', _('Uplink switch port'),
			_('The switch port the download half of each rule is installed on. Left empty, it is taken from the WAN interface.'));
		o.noaliases = true;
		o.nocreate = true;
		o.optional = true;
		o.filter = (section_id, name) => switch_ports.includes(name);

		o = s.taboption('advanced', form.Flag, 'mark_upload', _('Mark upload'),
			_('Also installs each rule on every client-facing port. This costs a classifier entry per port, and small upload packets are prioritized without it.'));

		s = m.section(form.GridSection, 'rule', _('Marking Rules'),
			_('A rule marks the download direction of the flows it matches. The switch matches addresses and ports only, not domain names.'));
		s.addremove = true;
		s.anonymous = true;
		s.sortable = true;
		s.cloneable = true;
		s.nodescriptions = true;
		s.tab('general', _('General Settings'));
		s.tab('advanced', _('Advanced Settings'));
		s.modaltitle = (section_id) => _('Marking Rules') + ' » ' + (uci.get('qca-ppe-qos', section_id, 'name') || _('Unnamed rule'));
		s.sectiontitle = (section_id) => uci.get('qca-ppe-qos', section_id, 'name') || _('Unnamed rule');

		o = s.taboption('general', form.Value, 'name', _('Name'));
		o.placeholder = _('Unnamed rule');
		o.modalonly = true;

		o = s.option(form.DummyValue, '_match', _('Match'));
		o.modalonly = false;
		o.textvalue = rule_match_txt;

		o = s.option(form.DummyValue, '_class', _('Class'));
		o.modalonly = false;
		o.textvalue = rule_class_txt;

		o = s.taboption('general', form.Flag, 'enabled', _('Enable'));
		o.default = o.enabled;
		o.rmempty = false;
		o.editable = true;

		o = s.taboption('general', form.ListValue, 'proto', _('Protocol'));
		o.value('tcpudp', 'TCP+UDP');
		o.value('tcp', 'TCP');
		o.value('udp', 'UDP');
		o.default = 'tcpudp';
		o.modalonly = true;

		o = s.taboption('general', form.DynamicList, 'dest_ip', _('Destination address'),
			_('IPv4 and IPv6 addresses or subnets.'));
		o.datatype = 'ipmask';
		o.placeholder = _('any');
		o.modalonly = true;

		o = s.taboption('general', form.DynamicList, 'dest_port', _('Destination port'),
			_('Ports or port ranges, such as 5000-5500.'));
		o.datatype = 'portrange';
		o.placeholder = _('any');
		o.modalonly = true;

		o = s.taboption('general', form.Value, 'class', _('Class'),
			_('The DSCP to mark with, as a class name or a number from 0 to 63. It picks the switch queue through the DSCP map below and the Wi-Fi access category directly.'));
		CLASSES.forEach(([ name, dscp, desc ]) =>
			o.value(name, '%s (%d)%s'.format(name.toUpperCase(), dscp, desc ? ' - ' + desc : '')));
		o.rmempty = false;
		o.modalonly = true;
		o.validate = function(section_id, value) {
			if (class_dscp(value) == null)
				return _('Expecting a class name or a DSCP value from 0 to 63');

			const matches = [ 'src_ip', 'src_port', 'dest_ip', 'dest_port' ]
				.some((opt) => L.toArray(this.section.formvalue(section_id, opt)).length);

			if (!matches)
				return _('A rule needs an address or a port, or it would mark all traffic');

			return true;
		};

		o = s.taboption('advanced', form.DynamicList, 'src_ip', _('Source address'),
			_('IPv4 and IPv6 addresses or subnets.'));
		o.datatype = 'ipmask';
		o.placeholder = _('any');
		o.modalonly = true;

		o = s.taboption('advanced', form.DynamicList, 'src_port', _('Source port'),
			_('Ports or port ranges, such as 5000-5500.'));
		o.datatype = 'portrange';
		o.placeholder = _('any');
		o.modalonly = true;

		s = m.section(form.TableSection, 'dscp', _('DSCP Map'),
			_('What a DSCP is worth once the switch reads it, for traffic these rules mark and traffic that arrives marked alike. Unlisted values stay in the bulk band.'));
		s.addremove = true;
		s.anonymous = true;
		s.nodescriptions = true;

		o = s.option(form.Value, 'dscp', _('DSCP'));
		CLASSES.forEach(([ name, dscp ]) => o.value(String(dscp), '%d (%s)'.format(dscp, name.toUpperCase())));
		o.datatype = 'and(uinteger,range(0,63))';
		o.rmempty = false;
		o.validate = function(section_id, value) {
			const dup = this.section.cfgsections().some((sid) =>
				sid != section_id && value !== '' && this.formvalue(sid) == value);

			return dup ? _('This DSCP is already mapped') : true;
		};

		o = s.option(form.ListValue, 'prio', _('Priority'));
		prio_values(o);
		o.rmempty = false;

		return m.render();
	}
});
