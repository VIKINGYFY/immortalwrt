'use strict';
'require baseclass';
'require form';
'require uci';

const state = 'fullconenat_sonic';

function syncMasquerading(fw4, before) {
	const enabled = uci.sections('firewall', 'defaults')[0]?.fullcone === '1';
	const zones = uci.sections('firewall', 'zone');

	for (const zone of zones) {
		const sid = zone['.name'];
		const original = before?.[sid];
		// Section IDs change on commit; retain snapshots across zone renames too.
		let saved = uci.sections(state, 'zone').find(s =>
			s.name === (original?.name ?? zone.name))?.['.name'];

		if (enabled && zone.fullcone === '1') {
			if (!saved)
				saved = uci.add(state, 'zone');
			uci.set(state, saved, 'name', zone.name);

			for (const option of fw4 ? [ 'masq', 'masq6' ] : [ 'masq' ]) {
				if (uci.get(state, saved, option) == null) {
					let previous = uci.get('firewall', sid, option);
					// LuCI flags normalize an unchecked 0 to an absent option.
					// Preserve the exact original value unless the user changed it.
					if (original && (original[option] === '1') === (previous === '1'))
						previous = original[option];
					uci.set(state, saved, option, previous ?? 'unset');
				}
				uci.set('firewall', sid, option, '1');
			}
		}
		else if (saved) {
			for (const option of [ 'masq', 'masq6' ]) {
				const previous = uci.get(state, saved, option);
				if (previous === 'unset')
					uci.unset('firewall', sid, option);
				else if (previous != null)
					uci.set('firewall', sid, option, previous);
			}
			uci.remove(state, saved);
		}
	}

	for (const saved of uci.sections(state, 'zone'))
		if (!zones.some(zone => zone.name === saved.name))
			uci.remove(state, saved['.name']);
}

return baseclass.extend({
	load() {
		return uci.load(state);
	},

	attach(map, fw4) {
		map.chain(state);
		const save = map.save.bind(map);
		map.save = function(cb, silent) {
			const before = Object.fromEntries(uci.sections('firewall', 'zone').map(zone =>
				[ zone['.name'], { name: zone.name, masq: zone.masq, masq6: zone.masq6 } ]));
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
				? _('Uses all supported protocols. Enabling Fullcone NAT automatically enables IPv4 and IPv6 masquerading; disabling it restores the previous settings. Usually enable this only on the WAN zone.')
				: _('Uses all supported protocols. Enabling Fullcone NAT automatically enables IPv4 masquerading; disabling it restores the previous settings. Usually enable this only on the WAN zone.'));
		o.editable = true;
		o.default = '0';
		o.rmempty = false;
		section.addModalOptions = modal => this.attach(modal.map, fw4);
		return o;
	}
});
