'use strict';
'require view';
'require form';
'require uci';

return view.extend({
	load() {
		return uci.load('firewall');
	},

	render() {
		let m, s, o;

		m = new form.Map('firewall', _('Fullcone NAT'),
			L.hasSystemFeature('firewall4')
				? _('Enable fullcone NAT for selected firewall zones. IPv4 or IPv6 masquerading must also be enabled in the zone settings.')
				: _('Enable IPv4 fullcone NAT for selected firewall zones. IPv4 masquerading must also be enabled in the zone settings.'));

		s = m.section(form.TypedSection, 'defaults', _('General Settings'));
		s.anonymous = true;
		s.addremove = false;
		o = s.option(form.Flag, 'fullcone', _('Enable Fullcone NAT'));
		o.default = '0';
		o.rmempty = false;

		s = m.section(form.TypedSection, 'zone', _('Zones'));
		s.anonymous = true;
		s.addremove = false;
		s.sectiontitle = section_id => uci.get('firewall', section_id, 'name') || section_id;

		o = s.option(form.Flag, 'fullcone', _('Enable for this zone'));
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.MultiValue, 'fullcone_proto', _('Fullcone protocols'),
			_('Leave empty to enable TCP, UDP, UDP-Lite and SCTP. Other protocols use ordinary masquerading.'));
		o.value('tcp', _('TCP'));
		o.value('udp', _('UDP'));
		o.value('udplite', _('UDP-Lite'));
		o.value('sctp', _('SCTP'));
		o.depends('fullcone', '1');
		o.optional = true;
		o.placeholder = _('All supported protocols');

		return m.render();
	}
});
