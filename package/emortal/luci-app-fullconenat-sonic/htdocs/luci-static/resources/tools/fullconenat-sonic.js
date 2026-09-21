'use strict';
'require baseclass';
'require form';
'require uci';

function syncMasquerading(before) {
	for (const zone of uci.sections('firewall', 'zone')) {
		const sid = zone['.name'];
		// A user's IPv4 mask-off choice takes priority within the same save.
		if (before[sid]?.masq === '1' && uci.get('firewall', sid, 'masq') !== '1')
			uci.set('firewall', sid, 'fullcone', '0');
		else if (before[sid]?.fullcone !== '1' && uci.get('firewall', sid, 'fullcone') === '1')
			uci.set('firewall', sid, 'masq', '1');
	}
}

return baseclass.extend({
	attach(map) {
		const save = map.save.bind(map);
		map.save = function(cb, silent) {
			// Read each option through get(): sections() retains deleted options.
			const before = Object.fromEntries(uci.sections('firewall', 'zone').map(zone => {
				const sid = zone['.name'];
				return [ sid, {
					masq: uci.get('firewall', sid, 'masq'),
					fullcone: uci.get('firewall', sid, 'fullcone')
				} ];
			}));
			return save(() => {
				for (const defaults of uci.sections('firewall', 'defaults')) {
					uci.set('firewall', defaults['.name'], 'flow_offloading', '0');
					uci.set('firewall', defaults['.name'], 'flow_offloading_hw', '0');
				}
				syncMasquerading(before);
				return cb ? cb() : undefined;
			}, silent);
		};
	},

	addGlobal(section) {
		const o = section.option(form.Flag, 'fullcone', _('Fullcone NAT'));
		o.default = '1';
		o.rmempty = false;
		return o;
	},

	addZone(section) {
		const o = section.taboption('general', form.Flag, 'fullcone', _('Fullcone NAT'),
			_('Uses all supported protocols. Enabling Fullcone NAT automatically enables IPv4 masquerading. Disabling IPv4 masquerading also disables Fullcone NAT for this zone. Disabling Fullcone NAT leaves masquerading unchanged. IPv6 masquerading is independent. Usually enable this only on the WAN zone.'));
		o.editable = true;
		o.default = '0';
		o.rmempty = false;
		section.addModalOptions = modal => this.attach(modal.map);
		return o;
	}
});
