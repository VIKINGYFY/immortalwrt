'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require rpc';
'require poll';
'require dom';

const callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

const callSystemBoard = rpc.declare({ object: 'system', method: 'board', expect: { '': {} } });

function nfcOptions(section) {
	section.tab('nfc', 'NFC');
	let o = section.taboption('nfc', form.DummyValue, '_nfc_status', _('Runtime status'));
	o.renderWidget = function() { return section.map.nfcStatus; };
	o = section.taboption('nfc', form.Flag, 'nfc_enabled', _('Enable NFC'),
		_('Disabling NFC clears the tag. Saved content is restored at boot and when wireless settings change. WPA3-only networks are not supported.'));
	o.default = '0';
	o.rmempty = false;
	function option(type, name, title, mode) {
		const value = section.taboption('nfc', type, 'nfc_' + name, title);
		value.retain = true;
		value.depends(mode ? { nfc_enabled: '1', nfc_mode: mode } : { nfc_enabled: '1' });
		return value;
	}
	const size = function(value) { return new TextEncoder().encode(value || '').length; };
	o = option(form.ListValue, 'mode', _('Tag content'));
	[ [ 'wifi', _('Router Wi-Fi') ], [ 'text', _('Text') ],
		[ 'uri', _('Link (URI)') ], [ 'manual', _('Manual Wi-Fi') ] ].forEach(function(entry) { o.value(entry[0], entry[1]); });
	o.default = 'wifi';
	o.rmempty = false;
	o = option(form.ListValue, 'iface', _('Access point'), 'wifi');
	uci.sections('wireless', 'wifi-iface').forEach(function(ap) {
		if (ap.mode === 'ap') o.value(ap['.name'], '%s · %s'.format(ap.ssid || ap['.name'], ap.device || ''));
	});
	o.validate = function(section_id, value) {
		const ap = uci.get('wireless', value);
		const radios = ap && (Array.isArray(ap.device) ? ap.device : (ap.device || '').split(/\s+/));
		return !!(ap && ap.mode === 'ap' && ap.disabled !== '1' && radios.length &&
			radios.every(function(id) { return uci.get('wireless', id) && uci.get('wireless', id, 'disabled') !== '1'; }) &&
			/^(none|psk(?:\+tkip)?|psk2(?:\+(?:ccmp|aes))?|(?:psk|sae)-mixed(?:\+ccmp)?)$/.test(ap.encryption || 'none')) ||
			_('Selected AP is unavailable or uses unsupported security.');
	};
	o = option(form.TextValue, 'text', _('Text'), 'text');
	o.rows = 6;
	o.description = _('Maximum 1900 UTF-8 bytes.');
	o.validate = function(section_id, value) { return (size(value) > 0 && size(value) <= 1900) || _('Text must contain 1–1900 UTF-8 bytes.'); };
	o = option(form.Value, 'uri', _('URL'), 'uri');
	o.placeholder = 'https://example.com';
	o.validate = function(section_id, value) {
		return (size(value) <= 1900 && /^[a-zA-Z][a-zA-Z0-9+.-]*:[^\s\x00-\x1f\x7f]*$/.test(value || '')) || _('Enter a valid URI (up to 1900 UTF-8 bytes).');
	};
	o = option(form.Value, 'manual_ssid', _('Network name (SSID)'), 'manual');
	o.validate = function(section_id, value) { return (size(value) > 0 && size(value) <= 32) || _('SSID must contain 1–32 UTF-8 bytes.'); };
	const security = option(form.ListValue, 'security', _('Encryption'), 'manual');
	security.value('wpa2', 'WPA2-Personal');
	security.value('none', _('No password'));
	security.default = 'wpa2';
	security.rmempty = false;
	o = section.taboption('nfc', form.Value, 'nfc_manual_key', _('Password'));
	o.password = true;
	o.retain = true;
	o.depends({ nfc_enabled: '1', nfc_mode: 'manual', nfc_security: 'wpa2' });
	o.validate = function(section_id, value) {
		return ((size(value) >= 8 && size(value) <= 63) || /^[a-fA-F0-9]{64}$/.test(value || '')) || _('Password must contain 8–63 bytes or 64 hexadecimal digits.');
	};
}

const ColorValue = form.Value.extend({
	renderWidget: function(section_id, option_index, cfgvalue) {
		const value = /^#[0-9a-fA-F]{6}$/.test(cfgvalue || '') ? cfgvalue : this.default;
		const widget = this.super('renderWidget', [ section_id, option_index, value ]);
		const input = widget.querySelector('input') || widget;
		const picker = E('input', {
			type: 'color', value: value, title: _('Choose a color'),
			style: 'width:3.5em;margin-left:.5em',
			change: function(ev) {
				input.value = ev.target.value;
				input.dispatchEvent(new Event('change', { bubbles: true }));
			}
		});
		input.addEventListener('input', function() {
			if (/^#[0-9a-fA-F]{6}$/.test(input.value))
				picker.value = input.value;
		});
		return E('div', { style: 'display:flex;align-items:center' }, [ widget, picker ]);
	}
});

function validateColor(section_id, value) {
	return /^#[0-9a-fA-F]{6}$/.test(value || '') || _('Enter a color in #RRGGBB format.');
}

function validateTime(section_id, value) {
	return /^([01][0-9]|2[0-3]):[0-5][0-9]$/.test(value || '') || _('Enter a time in HH:MM format.');
}

function fault(error) {
	const messages = {
		'fan-unavailable': _('Fan controller unavailable'),
		'temperature-unavailable': _('Temperature unavailable; requesting full fan speed'),
		'pwm-read-failed': _('Unable to read fan output; requesting full speed'),
		'pwm-write-failed': _('Unable to confirm fan output; requesting full speed'),
		'fan-stalled': _('Fan stalled; requesting full speed'),
		'fan-control-failed': _('Fan control failed; requesting full speed'),
		'led-write-failed': _('Unable to write RGB lighting output'),
		'led-control-failed': _('RGB lighting control failed')
	};
	return error ? (messages[error] || _('Unknown error')) : _('None');
}

function nfcFault(error) {
	const rollback = ' NFC rollback failed.';
	const invalid = 'Invalid NFC option: ';
	if (error.endsWith(rollback))
		return '%s %s'.format(nfcFault(error.slice(0, -rollback.length)), _('NFC rollback failed.'));
	if (error.startsWith(invalid))
		return _('Invalid NFC option: %s').format(error.slice(invalid.length));
	return _(error);
}

function metric(label, value) {
	return E('div', { 'class': 'table-row' }, [
		E('div', { 'class': 'td left', style: 'width:40%' }, label),
		E('div', { 'class': 'td left' }, value)
	]);
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('xiaomi-control'), uci.load('wireless'), callSystemBoard() ]);
	},

	updateStatus: function() {
		return Promise.all([
			L.resolveDefault(fs.read('/var/run/xiaomi-control.json'), ''),
			L.resolveDefault(callServiceList('xiaomi-control'), {}),
			L.resolveDefault(fs.read('/var/run/xiaomi-nfc.json'), '')
		]).then(L.bind(function(result) {
			const instances = result[1]['xiaomi-control']?.instances || {};
			const running = Object.keys(instances).some(function(key) { return instances[key].running; });
			let status = {};
			try {
				const parsed = JSON.parse(result[0]);
				if (parsed && typeof parsed == 'object') status = parsed;
			} catch (e) { /* Status is not ready. */ }
			const fan = running ? (status.fan || {}) : {};
			const led = running ? (status.led || {}) : {};
			let nfc = {};
			try { nfc = JSON.parse(result[2]); } catch (e) { /* Not ready. */ }
			dom.content(this.nfcStatus, nfc.error ? nfcFault(nfc.error) : !nfc.available ? _('NFC hardware unavailable.') :
				nfc.enabled ? _('Written.') : _('NFC disabled.'));
			const reading = function(value, format) {
				return typeof value == 'number' && Number.isFinite(value) && value >= 0 ? format(value) : '-';
			};

			dom.content(this.fanStatus, E('div', { 'class': 'table' }, [
				metric(_('Service'), running ? _('Running') : _('Stopped')),
				metric(_('Temperature'), reading(fan.temperature, function(v) { return '%.1f °C'.format(v / 1000); })),
				metric(_('Fan speed'), reading(fan.rpm, function(v) { return '%d RPM'.format(v); })),
				metric(_('PWM output'), reading(fan.pwm, function(v) { return '%d / 255 (%d%%)'.format(v, Math.round(v * 100 / 255)); })),
				metric(_('Hottest sensor'), fan.sensor || '-'),
				metric(_('Valid sensors'), reading(fan.sensors, String)),
				metric(_('Fault'), running && !status.fan ? _('Collecting data...') : fault(fan.error))
			]));
			dom.content(this.ledStatus, running ? (!status.led ? _('Collecting data...') :
				(led.error ? fault(led.error) : _('Running'))) : _('Stopped'));
		}, this));
	},

	render: function(data) {
		const m = new form.Map('xiaomi-control', _('Xiaomi Control'),
			_('Fan, RGB lighting and NFC control for supported Xiaomi routers.'));
		const s = m.section(form.NamedSection, 'settings', 'control');
		s.addremove = false;
		this.fanStatus = E('div', {}, _('Collecting data...'));
		this.ledStatus = E('span', {}, _('Collecting data...'));
		this.nfcStatus = m.nfcStatus = E('span', {}, _('Collecting data...'));
		if (/^xiaomi,be10000(?:-es)?$/.test(data[2].board_name || '')) {
			s.tab('fan', _('Fan'));
			s.tab('led', _('RGB lighting'));

			let o = s.taboption('fan', form.DummyValue, '_fan_status', _('Runtime status'));
			o.renderWidget = L.bind(function() { return this.fanStatus; }, this);
			o = s.taboption('fan', form.ListValue, 'profile', _('Fan profile'),
				_('Uses the highest valid thermal-zone temperature, with hysteresis, a startup boost and full-speed recovery on faults.'));
			o.value('auto', _('Automatic (45–85 °C)'));
			o.value('quiet', _('Quiet (52–80 °C)'));
			o.value('balanced', _('Balanced (45–75 °C)'));
			o.value('performance', _('Performance (38–65 °C)'));
			o.default = 'auto';
			o.rmempty = false;

			o = s.taboption('led', form.DummyValue, '_led_status', _('Runtime status'));
			o.renderWidget = L.bind(function() { return this.ledStatus; }, this);
			o = s.taboption('led', form.Flag, 'enabled', _('Enable RGB lighting'));
			o.default = o.enabled;
			o.rmempty = false;
			o = s.taboption('led', form.ListValue, 'mode', _('Lighting mode'));
			[
				[ 'static', _('Static') ], [ 'breathe', _('Breathing') ],
				[ 'duotone', _('Two-color fade') ], [ 'rainbow', _('Rainbow') ],
				[ 'temperature', _('Temperature') ], [ 'cpu', _('CPU load') ], [ 'traffic', _('Network traffic') ]
			].forEach(function(entry) { o.value(entry[0], entry[1]); });
			o.default = 'breathe';
			o.rmempty = false;
			o.depends('enabled', '1');

			function modes(option, values) {
				values.forEach(function(mode) { option.depends({ enabled: '1', mode: mode }); });
			}
			function slider(name, title, value, low, high, values) {
				const option = s.taboption('led', form.RangeSliderValue, name, title);
				option.min = low;
				option.max = high;
				option.step = 1;
				option.default = value;
				option.rmempty = false;
				// Keep default values for these required settings.
				option.formvalue = function(section_id) {
					const element = this.getUIElement(section_id);
					return element ? element.getValue() : null;
				};
				option.datatype = 'range(%d,%d)'.format(low, high);
				if (values) modes(option, values);
				else option.depends('enabled', '1');
				return option;
			}
			function timeOption(name, title, value, flag) {
				const option = s.taboption('led', form.Value, name, title);
				option.default = value;
				option.rmempty = false;
				option.validate = validateTime;
				const dependency = { enabled: '1' };
				dependency[flag] = '1';
				option.depends(dependency);
			}

			o = s.taboption('led', ColorValue, 'color', _('Primary color'));
			o.default = '#0000ff';
			o.rmempty = false;
			o.validate = validateColor;
			modes(o, [ 'static', 'breathe', 'duotone' ]);
			o = s.taboption('led', ColorValue, 'second_color', _('Secondary color'));
			o.default = '#ff00aa';
			o.rmempty = false;
			o.validate = validateColor;
			modes(o, [ 'duotone' ]);
			slider('brightness', _('Maximum brightness'), '96', 0, 255);
			slider('minimum_brightness', _('Minimum brightness'), '8', 0, 255, [ 'breathe' ]);
			slider('speed', _('Animation speed'), '5', 1, 10, [ 'breathe', 'duotone', 'rainbow' ]);
			slider('sensitivity', _('Sensitivity'), '5', 1, 10, [ 'temperature', 'cpu', 'traffic' ]);
			o = s.taboption('led', form.Value, 'traffic_interface', _('Traffic interface'),
				_('Logical interface or network device, for example wan or pppoe-wan.'));
			o.default = 'wan';
			o.rmempty = false;
			o.validate = function(section_id, value) {
				return /^[A-Za-z0-9_+.:@-]{1,15}$/.test(value || '') || _('Enter a valid interface or device name (up to 15 characters).');
			};
			modes(o, [ 'traffic' ]);

			o = s.taboption('led', form.Flag, 'schedule_enabled', _('Scheduled lighting'),
				_('Equal start and end times cover the whole day. Time ranges may cross midnight.'));
			o.rmempty = false;
			o.depends('enabled', '1');
			timeOption('on_time', _('On time'), '07:00', 'schedule_enabled');
			timeOption('off_time', _('Off time'), '23:30', 'schedule_enabled');
			o = s.taboption('led', form.Flag, 'night_enabled', _('Night brightness'));
			o.rmempty = false;
			o.depends('enabled', '1');
			timeOption('night_start', _('Night start'), '22:00', 'night_enabled');
			timeOption('night_end', _('Night end'), '07:00', 'night_enabled');
			o = slider('night_brightness', _('Maximum night brightness'), '24', 0, 255, []);
			o.depends({ enabled: '1', night_enabled: '1' });
		}
		nfcOptions(s);

		return m.render().then(L.bind(function(node) {
			poll.add(L.bind(this.updateStatus, this), 5);
			return this.updateStatus().then(function() { return node; });
		}, this));
	}
});
