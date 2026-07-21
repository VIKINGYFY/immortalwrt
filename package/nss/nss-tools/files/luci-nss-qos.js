'use strict';
'require view';
'require form';

/*
 * NSS QoS marking rules editor. Front end for /etc/config/nssqos, which
 * the nssqos service renders into the standalone nftables table
 * 'inet nssqos'. Marking (and fast-lane steering) applies to NSS
 * accelerated flows too, in both directions.
 */

var CLASSES = [
	['ef',   'EF – Expedited Forwarding (voice)'],
	['va',   'VA – Voice Admit'],
	['cs7',  'CS7 – Network control'],
	['cs6',  'CS6 – Internetwork control'],
	['cs5',  'CS5 – Signaling'],
	['cs4',  'CS4 – Realtime interactive'],
	['af41', 'AF41 – Multimedia conferencing'],
	['af42', 'AF42 – Multimedia conferencing'],
	['af43', 'AF43 – Multimedia conferencing'],
	['cs3',  'CS3 – Broadcast video'],
	['af31', 'AF31 – Multimedia streaming'],
	['af32', 'AF32 – Multimedia streaming'],
	['af33', 'AF33 – Multimedia streaming'],
	['cs2',  'CS2 – OAM'],
	['af21', 'AF21 – Low-latency data'],
	['af22', 'AF22 – Low-latency data'],
	['af23', 'AF23 – Low-latency data'],
	['af11', 'AF11 – High-throughput data'],
	['af12', 'AF12 – High-throughput data'],
	['af13', 'AF13 – High-throughput data'],
	['cs1',  'CS1 – Low priority'],
	['le',   'LE – Least effort (bulk)'],
	['cs0',  'CS0 – Best effort (default)']
];

return view.extend({
	render: function() {
		var m, s, o;

		m = new form.Map('nssqos', _('NSS QoS Marking'),
			_('DSCP marking and prioritization for the NSS-accelerated SQM. ' +
			  'Each rule marks both directions of matching connections; the NSS ' +
			  'firmware applies the mark to accelerated (offloaded) traffic as well. ' +
			  'Fast-laned flows bypass the SQM queues entirely — mark sparse traffic ' +
			  'only (games, calls, DNS), never bulk transfers.'));

		s = m.section(form.NamedSection, 'global', 'global', _('Global'));
		o = s.option(form.Flag, 'enabled', _('Enable'),
			_('Master switch for all marking rules.'));
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.Flag, 'wash_ingress', _('Wash ingress DSCP'),
			_('Reset untrusted DSCP on packets arriving from the ISP to CS0. ' +
			  'Marking rules below still re-mark what they match.'));
		o.default = '0';

		o = s.option(form.Flag, 'wash_egress', _('Wash egress DSCP'),
			_('Send all traffic to the ISP as CS0. Fast-lane prioritization ' +
			  'is unaffected — it rides the internal priority mark, not the wire DSCP.'));
		o.default = '0';

		s = m.section(form.GridSection, 'rule', _('Rules'));
		s.addremove = true;
		s.anonymous = true;
		s.sortable = true;
		s.nodescriptions = true;

		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.default = '0';
		o.editable = true;

		o = s.option(form.Value, 'name', _('Name'));
		o.rmempty = false;

		o = s.option(form.ListValue, 'proto', _('Protocol'));
		o.value('udp', 'UDP');
		o.value('tcp', 'TCP');
		o.value('tcpudp', 'TCP+UDP');
		o.default = 'udp';

		o = s.option(form.Value, 'src_ip', _('Source IP'),
			_('Hosts or CIDRs, space-separated, IPv4 and/or IPv6. Empty matches any.'));
		o.datatype = 'list(or(ipaddr,cidr))';
		o.modalonly = true;

		o = s.option(form.DynamicList, 'src_port', _('Source ports'),
			_("One port or range per entry, e.g. '443' or '27015-27030'."));
		o.datatype = 'or(port,portrange)';
		o.modalonly = true;

		o = s.option(form.Value, 'dest_ip', _('Destination IP'));
		o.datatype = 'list(or(ipaddr,cidr))';
		o.modalonly = true;

		o = s.option(form.DynamicList, 'domain', _('Destination domain'),
			_('DNS names, re-resolved periodically on the router. A CDN whose ' +
			  'per-stream subdomains share the zone\'s front-end addresses is ' +
			  'covered by its apex name. Combines with Destination IP (either matches).'));
		o.datatype = 'hostname';
		o.modalonly = true;

		o = s.option(form.DynamicList, 'dest_port', _('Destination ports'),
			_("One port or range per entry, e.g. '443' or '27015-27030'."));
		o.datatype = 'or(port,portrange)';

		o = s.option(form.ListValue, 'class', _('DSCP class'));
		CLASSES.forEach(function(c) { o.value(c[0], c[1]); });
		o.default = 'cs4';

		o = s.option(form.ListValue, 'fastlane', _('Fast lane'),
			_('Strict-priority lane ahead of the SQM queues, both directions. ' +
			  "'Automatic' enables it for EF, VA and CS4–CS7."));
		o.value('auto', _('Automatic'));
		o.value('1', _('Always'));
		o.value('0', _('Never'));
		o.default = 'auto';

		return m.render();
	}
});
