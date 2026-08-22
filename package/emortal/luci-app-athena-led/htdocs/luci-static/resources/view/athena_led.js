'use strict';
'require dom';
'require form';
'require poll';
'require rpc';
'require ui';
'require view';

var callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

function addChoices(option, choices) {
	for (var i = 0; i < choices.length; i++)
		option.value(choices[i][0], choices[i][1]);

	return option;
}

function addNumberChoices(option, min, max, formatLabel) {
	for (var i = min; i <= max; i++)
		option.value(String(i), formatLabel ? formatLabel(i) : String(i));

	return option;
}

function toStringList(value) {
	var values = L.toArray(value);
	var list = [];

	for (var i = 0; i < values.length; i++) {
		var item = values[i];

		if (item == null)
			continue;

		if (typeof(item) == 'string') {
			item = item.trim();

			if (item == '')
				continue;

			list = list.concat(item.split(/\s+/));
		}
		else {
			list.push(String(item));
		}
	}

	return list;
}

var SpaceSeparatedCheckboxValue = form.MultiValue.extend({
	renderWidget: function(section_id, option_index, cfgvalue) {
		var choices = this.transformChoices();
		var widget = new ui.Select(toStringList((cfgvalue != null) ? cfgvalue : this.default), choices, {
			id: this.cbid(section_id),
			sort: this.keylist,
			multiple: true,
			widget: 'checkbox',
			orientation: this.orientation || 'horizontal',
			disabled: (this.readonly != null) ? this.readonly : this.map.readonly
		});
		var node = widget.render();
		var inputs = node.querySelectorAll('input[type="checkbox"]');
		var items = node.querySelectorAll('.cbi-checkbox');

		node.style.setProperty('display', 'inline-flex', 'important');
		Object.assign(node.style, {
			flexWrap: 'wrap',
			alignItems: 'center',
			columnGap: '1em',
			rowGap: '.25em'
		});

		for (var i = node.childNodes.length - 1; i >= 0; i--)
			if (node.childNodes[i].nodeType == 3)
				node.removeChild(node.childNodes[i]);

		for (var i = 0; i < items.length; i++) {
			items[i].style.setProperty('display', 'inline-flex', 'important');
			Object.assign(items[i].style, {
				alignItems: 'center',
				whiteSpace: 'nowrap'
			});
		}

		widget.getValue = function() {
			var values = [];

			for (var i = 0; i < inputs.length; i++)
				if (inputs[i].checked)
					values.push(inputs[i].value);

			return values;
		};

		widget.setValue = function(value) {
			var values = toStringList(value);

			for (var i = 0; i < inputs.length; i++)
				inputs[i].checked = (values.indexOf(inputs[i].value) > -1);
		};

		for (var i = 0; i < inputs.length; i++) {
			widget.setUpdateEvents(inputs[i], 'change', 'click', 'blur');
			widget.setChangeEvents(inputs[i], 'change');
		}

		return node;
	},

	cfgvalue: function(section_id, set_value) {
		if (arguments.length == 2)
			return this.super('cfgvalue', [ section_id, set_value ]);

		return toStringList(this.super('cfgvalue', [ section_id ]));
	},

	write: function(section_id, value) {
		return this.super('write', [ section_id, toStringList(value).join(' ') ]);
	}
});

function renderStatus(running) {
	return E('span', {
		'style': 'font-weight:bold;font-style:italic;color:%s'.format(running ? 'green' : 'red')
	}, [ _('Athena LED Ctrl'), ': ', running ? _('Running') : _('Not running') ]);
}

function isServiceRunning(res) {
	var service = res && res.athena_led;
	var instances = (service && service.instances) || {};

	for (var name in instances) {
		if (instances[name].running === true)
			return true;
	}

	return false;
}

function getRunningStatus() {
	return L.resolveDefault(callServiceList('athena_led'), {}).then(function(res) {
		return isServiceRunning(res);
	});
}

function updateStatus(node) {
	return getRunningStatus().then(function(running) {
		dom.content(node, renderStatus(running));
	});
}

function renderStatusSection() {
	var node = E('span', { 'id': 'athena-led-service-status' }, [ _('Collecting data...') ]);
	var refresh = L.bind(updateStatus, null, node);

	refresh();
	poll.add(refresh, 3);

	return E('div', { 'class': 'cbi-section' }, [
		E('p', {}, [ node ])
	]);
}

return view.extend({
	render: function() {
		var m, s, o;

		m = new form.Map('athena_led', _('Athena LED Ctrl'), _('JDCloud Athena LED Ctrl'));

		s = m.section(form.TypedSection);
		s.anonymous = true;
		s.render = renderStatusSection;

		s = m.section(form.NamedSection, 'config', 'athena_led', _('Settings'));
		s.anonymous = true;

		o = s.option(form.Flag, 'enable', _('Enabled'));
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.ListValue, 'seconds', _('Display interval time'), _('Enable carousel display and set interval time in seconds'));
		o.default = '5';
		o.rmempty = false;
		addNumberChoices(o, 1, 5, function(value) {
			return _('%d seconds').format(value);
		});

		o = s.option(form.ListValue, 'lightLevel', _('Display light level'), _('Display light level desc'));
		o.default = '5';
		o.rmempty = false;
		addNumberChoices(o, 0, 7);

		o = s.option(SpaceSeparatedCheckboxValue, 'status', _('Side LED status'), _('side led status desc'));
		o.rmempty = true;
		addChoices(o, [
			[ 'time', _('status time') ],
			[ 'medal', _('status medal') ],
			[ 'upload', _('status upload') ],
			[ 'download', _('status download') ]
		]);

		o = s.option(SpaceSeparatedCheckboxValue, 'option', _('Display Type'), _('Select one or more display modes'));
		o.default = 'date timeBlink';
		o.rmempty = false;
		o.required = true;
		addChoices(o, [
			[ 'date', _('Display Type Date') ],
			[ 'time', _('Display Type Time') ],
			[ 'timeBlink', _('Display Type Time Blink') ],
			[ 'temp', _('Display Type temp') ],
			[ 'string', _('Display Type String') ],
			[ 'getByUrl', _('Display Type getByUrl') ]
		]);

		o = s.option(form.Value, 'value', _('Custom Value'), _('Set the custom message to display on the LED screen, Only effective on \'Display Type String\''));
		o.default = 'abcdefghijklmnopqrstuvwxyz0123456789+-*/=.:：℃';
		o.placeholder = _('Enter your message here');
		o.rmempty = false;
		o.retain = true;
		o.depends({ option: 'string', '!contains': true });

		o = s.option(form.Value, 'url', _('Remote text URL'), _('api url for get content des'));
		o.default = 'https://ifconfig.me';
		o.placeholder = _('Enter your api url here');
		o.rmempty = false;
		o.retain = true;
		o.depends({ option: 'getByUrl', '!contains': true });

		o = s.option(SpaceSeparatedCheckboxValue, 'tempFlag', _('tempFlag'), _('Select the temperature sensor, Only effective on \'Display Type temp\''));
		o.default = '4';
		o.rmempty = false;
		o.required = true;
		o.retain = true;
		o.depends({ option: 'temp', '!contains': true });
		addChoices(o, [
			[ '0', _('nss-top') ],
			[ '1', _('nss') ],
			[ '2', _('wcss-phya0') ],
			[ '3', _('wcss-phya1') ],
			[ '4', _('cpu') ],
			[ '5', _('lpass') ],
			[ '6', _('ddrss') ]
		]);

		return m.render();
	}
});
