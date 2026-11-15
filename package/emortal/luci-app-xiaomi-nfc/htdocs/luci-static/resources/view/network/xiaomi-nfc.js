'use strict';
'require view';
'require rpc';
'require ui';

var status = rpc.declare({ object: 'xiaomi.nfc', method: 'status', expect: {} });
var save = rpc.declare({ object: 'xiaomi.nfc', method: 'set', params: ['enabled', 'iface', 'mode', 'text', 'uri', 'manual_ssid', 'manual_key', 'security'], expect: {} });

return view.extend({
	load: function() { return status(); },
	render: function(info) {
		var enabled = E('input', { type: 'checkbox', checked: info.enabled ? '' : null, autocomplete: 'off' });
		var mode = E('select', { 'class': 'cbi-input-select' }, [
			E('option', { value: 'wifi' }, _('Router Wi-Fi')), E('option', { value: 'text' }, _('Text')),
			E('option', { value: 'uri' }, _('Link (URI)')), E('option', { value: 'manual' }, _('Manual Wi-Fi'))
		]);
		mode.value = ['text', 'uri', 'manual'].includes(info.mode) ? info.mode : 'wifi';
		function field(label, input) {
			return E('p', {}, [ E('label', { style: 'display:block;margin-bottom:.5em' }, label), input ]);
		}
		var uri = E('input', { type: 'text', placeholder: 'https://example.com', style: 'width:100%', value: info.uri || '' });
		var uriRow = field(_('URL'), uri);
		var ssid = E('input', { type: 'text', value: info.manual_ssid || '', autocomplete: 'off' });
		var key = E('input', { type: 'password', 'class': 'cbi-input-password', style: 'flex:1;min-width:0', value: info.manual_key || '', autocomplete: 'new-password' });
		var reveal = E('button', {
			'class': 'cbi-button cbi-button-neutral',
			'type': 'button',
			'title': _('Reveal/hide password'),
			'aria-label': _('Reveal/hide password'),
			'click': function(ev) {
				key.type = (key.type === 'password') ? 'text' : 'password';
				ev.preventDefault();
			}
		}, '∗');
		var keyGroup = E('div', { 'class': 'control-group', style: 'display:flex;align-items:center;gap:.25em' }, [ key, reveal ]);
		var security = E('select', {}, [ E('option', { value: 'wpa2' }, 'WPA2-Personal'), E('option', { value: 'none' }, _('No password')) ]);
		security.value = info.security === 'none' ? 'none' : 'wpa2';
		var keyRow = field(_('Password'), keyGroup);
		var manualRow = E('div', {}, [ field(_('Network name (SSID)'), ssid), field(_('Encryption'), security), keyRow ]);
		var text = E('textarea', { rows: 6, style: 'width:100%;box-sizing:border-box', autocomplete: 'off' });
		text.value = info.text || '';
		var counter = E('small');
		var textRow = E('div', {}, [ text, counter ]);
		var select = E('select', { 'class': 'cbi-input-select', style: 'max-width:100%' },
			(info.aps || []).map(function(ap) {
				return E('option', { value: ap.id }, ap.ssid + ' · ' + ap.radio + (ap.disabled ? ' ' + _('(disabled)') : ''));
			}));
		if ((info.aps || []).some(function(ap) { return ap.id === info.iface; })) select.value = info.iface;
		var wifiRow = E('p', {}, [ E('label', { style: 'display:block;margin-bottom:.5em' }, _('Access point')), select ]);
		var note = E('p', { 'class': 'cbi-section-descr' });
		var result = E('p', { role: 'status' }, info.error || '');
		var button = E('button', { 'class': 'cbi-button cbi-button-action', click: ui.createHandlerFn(this, function() {
			button.disabled = true;
			return save(enabled.checked, select.value || '', mode.value, text.value, uri.value, ssid.value, key.value, security.value).then(function(reply) {
				if (!reply.ok) throw new Error(reply.error || _('Failed to write NFC tag.'));
				result.textContent = enabled.checked ? _('Written.') : _('NFC disabled.');
			}).catch(function(err) {
				ui.addNotification(null, E('p', {}, err.message), 'error');
			}).finally(update);
		}) }, _('Save'));
		function update() {
			var isText = mode.value === 'text';
			var bytes = new TextEncoder().encode(text.value).length;
			wifiRow.hidden = mode.value !== 'wifi';
			textRow.hidden = !isText;
			uriRow.hidden = mode.value !== 'uri';
			manualRow.hidden = mode.value !== 'manual';
			keyRow.hidden = security.value === 'none';
			[uri, ssid, key, security].forEach(function(input) { input.disabled = !enabled.checked; });
			reveal.disabled = !enabled.checked;
			counter.textContent = bytes + _(' / 1900 bytes');
			text.disabled = !enabled.checked;
			mode.disabled = !enabled.checked;
			select.disabled = !enabled.checked;
			var size = function(value) { return new TextEncoder().encode(value).length; };
			var invalid = mode.value === 'text' ? !bytes || bytes > 1900 :
				mode.value === 'uri' ? !/^[a-zA-Z][a-zA-Z0-9+.-]*:[^\s\x00-\x1f\x7f]*$/.test(uri.value) || size(uri.value) > 1900 :
				mode.value === 'manual' ? !size(ssid.value) || size(ssid.value) > 32 ||
					(security.value !== 'none' && !((size(key.value) >= 8 && size(key.value) <= 63) || /^[a-fA-F0-9]{64}$/.test(key.value))) : !(info.aps || []).length;
			button.disabled = !info.available || (enabled.checked && invalid);
			var ap = (info.aps || []).find(function(item) { return item.id === select.value; });
			note.textContent = !info.available ? _('NFC hardware unavailable.') :
				!enabled.checked || mode.value !== 'wifi' ? '' :
				ap && ap.disabled ? _('Access point is disabled.') :
				ap && /^sae(?:$|\+|-ext)/.test(ap.encryption) ? '' :
				ap && !ap.compatible ? _('This security type is not supported.') : '';
			note.hidden = !note.textContent;
		}
		enabled.addEventListener('change', update);
		select.addEventListener('change', update);
		mode.addEventListener('change', update);
		text.addEventListener('input', update);
		[uri, ssid, key, security].forEach(function(input) { input.addEventListener('input', update); input.addEventListener('change', update); });
		update();
		return E('div', {}, [
			E('h2', {}, 'NFC'),
			E('p', { 'class': 'cbi-section-descr', style: 'margin-top:-.4em;margin-bottom:1.2em' }, _('Provides NFC support for selected Xiaomi router models. Currently only the Xiaomi BE7000 and BE10000 are supported.')),
			E('div', { 'class': 'cbi-section', style: 'max-width:760px;padding:1.2em' }, [
				E('p', {}, E('label', {}, [ enabled, ' ' + _('Enable NFC') ])),
				E('p', {}, [ E('label', { style: 'display:block;margin-bottom:.5em' }, _('Tag content')), mode ]),
				wifiRow, textRow, uriRow, manualRow,
				note,
				button, result
			])
		]);
	},
	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
