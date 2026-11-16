'use strict';
'require view';
'require form';

/* /etc/config/airscan */

return view.extend({
	render: function() {
		var m, s, o;

		m = new form.Map('airscan', _('Air Scan: Settings'),
			_('The survey runs on the dedicated scan radio only; the serving radios never scan.'));

		s = m.section(form.NamedSection, 'main', 'airscan');
		s.tab('general', _('General'));
		s.tab('trust', _('Your networks'));
		s.tab('ble', _('Bluetooth LE'));

		o = s.taboption('general', form.Flag, 'enabled', _('Enabled'));
		o.default = '1';

		o = s.taboption('general', form.Value, 'spectral_interval', _('Spectral sweep every'), _('seconds'));
		o.datatype = 'range(60,86400)';
		o.default = '900';

		o = s.taboption('general', form.ListValue, 'spectral_fft', _('FFT size'), _('in-band bins are half of this; a QCA988x scan radio (ath10k, the C-260\'s) takes at most 256'));
		[ '64', '128', '256', '512' ].forEach(function(v) { o.value(v); });
		o.default = '256';

		o = s.taboption('general', form.Value, 'spectral_samples', _('Samples per channel'), _('about 0.23 ms each'));
		o.datatype = 'range(256,16384)';
		o.default = '2048';

		o = s.taboption('general', form.Value, 'bss_interval', _('Network survey every'), _('seconds'));
		o.datatype = 'range(30,86400)';
		o.default = '300';

		o = s.taboption('general', form.Value, 'client_dwell', _('Dwell per channel'), _('milliseconds, for the client survey'));
		o.datatype = 'range(100,10000)';
		o.default = '1000';

		o = s.taboption('general', form.Value, 'client_expire', _('Forget a client after'), _('seconds unheard'));
		o.datatype = 'uinteger';
		o.default = '900';

		o = s.taboption('trust', form.Flag, 'wired_probe', _('Wired probe'),
			_('An ARP probe from this AP on each of its L2 domains: a network that relays it over the air is bridged into your LAN. With one of your SSIDs it is verified; with another SSID it is a wired rogue; one of your SSIDs that never relays it is an evil twin.'));
		o.default = '1';

		o = s.taboption('trust', form.Value, 'probe_evidence', _('Evil twin after'), _('dwells without a relayed probe'));
		o.datatype = 'range(1,100)';
		o.default = '5';
		o.depends('wired_probe', '1');

		o = s.taboption('trust', form.Value, 'probe_refresh', _('Probe settled channels every'),
			_('seconds. A channel is probed while a network heard there has no verdict yet, and otherwise this often: each probe is a broadcast that APs may copy to every client.'));
		o.datatype = 'range(30,3600)';
		o.default = '300';
		o.depends('wired_probe', '1');

		o = s.taboption('trust', form.DynamicList, 'trusted_ssid', _('Your SSIDs'),
			_('SSIDs served by your other APs. The SSIDs this AP serves count by themselves.'));

		o = s.taboption('trust', form.DynamicList, 'trusted_bssid', _('Trusted BSSIDs'),
			_('Always accepted; a prefix such as 00:11:22 matches a whole vendor or AP. For APs that filter broadcast ARP.'));
		o.datatype = 'string';

		o = s.taboption('ble', form.Flag, 'ble_enabled', _('BLE survey'));
		o.default = '1';

		o = s.taboption('ble', form.Value, 'ble_interval', _('Scan every'), _('seconds'));
		o.datatype = 'range(10,86400)';
		o.default = '120';
		o.depends('ble_enabled', '1');

		o = s.taboption('ble', form.Value, 'ble_duration', _('Scan for'), _('seconds'));
		o.datatype = 'range(1,60)';
		o.default = '10';
		o.depends('ble_enabled', '1');

		o = s.taboption('ble', form.Value, 'ble_expire', _('Forget a device after'), _('seconds unheard'));
		o.datatype = 'uinteger';
		o.default = '900';
		o.depends('ble_enabled', '1');

		return m.render();
	}
});
