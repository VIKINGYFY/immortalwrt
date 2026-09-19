'use strict';
'require baseclass';
'require form';
'require uci';

function syncMasquerading(fw4, before) {
	const enabled = uci.sections('firewall', 'defaults')[0]?.fullcone === '1';
	const maskOptions = fw4 ? [ 'masq', 'masq6' ] : [ 'masq' ];

	for (const zone of uci.sections('firewall', 'zone')) {
		const sid = zone['.name'];
		// A user's mask-off choice takes priority over automatic enablement.
		const maskDisabled = maskOptions.some(option =>
			before[sid]?.[option] === '1' && uci.get('firewall', sid, option) !== '1');

		if (maskDisabled)
			uci.set('firewall', sid, 'fullcone', '0');
		else if (enabled && zone.fullcone === '1')
			for (const option of maskOptions)
				uci.set('firewall', sid, option, '1');
	}
}

return baseclass.extend({
	attach(map, fw4) {
		const save = map.save.bind(map);
		map.save = function(cb, silent) {
			// Read each option through get(): sections() retains deleted options.
			const before = Object.fromEntries(uci.sections('firewall', 'zone').map(zone => {
				const sid = zone['.name'];
				return [ sid, {
					masq: uci.get('firewall', sid, 'masq'),
					masq6: uci.get('firewall', sid, 'masq6')
				} ];
			}));
			return save(() => {
				syncMasquerading(fw4, before);
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

	addZone(section, fw4) {
		const o = section.taboption('general', form.Flag, 'fullcone', _('Fullcone NAT'),
			fw4
				? _('Uses all supported protocols. Enabling Fullcone NAT automatically enables IPv4 and IPv6 masquerading. Disabling either masquerading option also disables Fullcone NAT for this zone. Disabling Fullcone NAT leaves masquerading unchanged. Usually enable this only on the WAN zone.')
				: _('Uses all supported protocols. Enabling Fullcone NAT automatically enables IPv4 masquerading. Disabling masquerading also disables Fullcone NAT for this zone. Disabling Fullcone NAT leaves masquerading unchanged. Usually enable this only on the WAN zone.'));
		o.editable = true;
		o.default = '0';
		o.rmempty = false;
		section.addModalOptions = modal => this.attach(modal.map, fw4);
		return o;
	}
});
