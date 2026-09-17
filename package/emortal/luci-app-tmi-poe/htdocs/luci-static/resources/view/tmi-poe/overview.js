'use strict';
'require form';
'require dom';
'require fs';
'require poll';
'require rpc';
'require uci';
'require ui';
'require view';

const statusCommand = '/usr/libexec/tmi-poe-status';
const logCommand = '/usr/libexec/tmi-poe-log';
const clearLogCommand = '/usr/libexec/tmi-poe-log-clear';
const getBoard = rpc.declare({ object: 'system', method: 'board', expect: { '': {} } });
const boardDefaults = {
	'xiaomi,be3600-pro-wired-p5': { ports: 4, budget: 60000 },
	'xiaomi,be3600-pro-wired-p8': { ports: 7, budget: 105000 }
};

function runStatus() {
	return L.resolveDefault(fs.exec_direct(statusCommand, [], 'json'), null);
}

function powerState(port) {
	if (!port)
		return _('TMI PoE unknown');
	if (port.powered && port.power_good)
		return _('TMI PoE power good');
	if (port.powered || port.power_good)
		return _('TMI PoE power fault');
	if (port.mode === 'shutdown')
		return _('TMI PoE power off');
	return port.connected === false ? _('TMI PoE LAN link disconnected') : _('TMI PoE not powered');
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('tmi-poe'), runStatus(), L.resolveDefault(getBoard(), {}) ]);
	},

	render: function(data) {
		let status = data[1];
		const board = boardDefaults[data[2].board_name];
		let maxBudget = status && status.max_budget_mw || (board ? board.budget : 105000);
		let statusBox, summaryNode, logNode;
		let logGeneration = 0, clearingLog = false;
		let rows = [];
		const ports = board
			? Array.from({ length: board.ports }, (_, i) => ({ port: i + 1 }))
			: status && status.ports || [];
		const m = new form.Map('tmi-poe', _('TMI PoE Control'),
			_('Use Save & Apply to apply TMI PoE configuration changes. Power status shows the current hardware state.'));
		const s = m.section(form.NamedSection, 'main', 'poe');
		s.anonymous = true;

		const enabled = s.option(form.Flag, 'enabled', _('Enable TMI PoE'));
		enabled.rmempty = false;
		enabled.default = '1';
		enabled.onchange = function(ev, section_id, value) {
			statusBox.style.display = value === '1' ? '' : 'none';
		};

		const budget = s.option(form.Value, 'budget_mw', _('TMI PoE power budget (W)'),
			_('Leave empty to use the device default TMI PoE budget.'));
		budget.rmempty = true;
		budget.retain = true;
		budget.depends('enabled', '1');
		budget.validate = function(section_id, value) {
			const mw = Number(value) * 1000;
			if (value === '')
				return true;
			if (!isFinite(mw) || mw < 1000 || mw > maxBudget)
				return _('Enter a TMI PoE budget between 1 and %s W.').format(maxBudget / 1000);
			return true;
		};
		budget.cfgvalue = function(section_id) {
			const value = uci.get('tmi-poe', section_id, 'budget_mw');
			return value ? String(Number(value) / 1000) : '';
		};
		budget.write = function(section_id, value) {
			return uci.set('tmi-poe', section_id, 'budget_mw',
				String(Math.round(Number(value) * 1000)));
		};

		const outputs = ports.map(function(port) {
			const option = 'port_lan%d'.format(port.port);
			const lan = 'lan%d'.format(port.port);
			const o = s.option(form.ListValue, option, _('TMI PoE output'));
			o.value('1', _('TMI PoE output enabled'));
			o.value('0', _('TMI PoE output disabled'));
			o.rmempty = false;
			o.default = '1';
			o.cfgvalue = function(section_id) {
				const disabled = L.toArray(uci.get('tmi-poe', section_id, 'disabled_ports'));
				if (disabled.indexOf(lan) !== -1)
					return '0';
				return uci.get('tmi-poe', section_id, option) || '1';
			};
			o.write = function(section_id, value) {
				// Migrate this port's legacy disable entry only when saving a change.
				const disabled = L.toArray(uci.get('tmi-poe', section_id, 'disabled_ports'));
				if (disabled.indexOf(lan) !== -1) {
					const remaining = disabled.filter(name => name !== lan);
					if (remaining.length)
						uci.set('tmi-poe', section_id, 'disabled_ports', remaining);
					else
						uci.unset('tmi-poe', section_id, 'disabled_ports');
				}
				return uci.set('tmi-poe', section_id, option, value);
			};
			return o;
		});

		const debug = s.option(form.Flag, 'debug', _('TMI PoE debug logging'));
		debug.rmempty = false;
		debug.default = '0';

		function updateLog() {
			const node = logNode, generation = logGeneration;
			if (!node || clearingLog)
				return Promise.resolve();
			return L.resolveDefault(fs.exec_direct(logCommand, []), null).then(function(data) {
				if (data == null || node !== logNode || generation !== logGeneration || clearingLog || node.value === data)
					return;
				const focused = document.activeElement === node;
				const follow = !focused && node.scrollTop + node.clientHeight >= node.scrollHeight - 4;
				const start = node.selectionStart, end = node.selectionEnd, top = node.scrollTop;
				node.value = data;
				if (focused)
					node.setSelectionRange(start, end);
				node.scrollTop = follow ? node.scrollHeight : top;
			});
		}

		function clearLog(ev) {
			const button = ev.currentTarget;
			button.disabled = true;
			clearingLog = true;
			logGeneration++;
			return fs.exec(clearLogCommand, []).then(function(result) {
				if (result.code !== 0)
					throw new Error(result.stderr || _('TMI PoE log clear failed.'));
				logNode.value = '';
			}).catch(function(error) {
				ui.addNotification(null, E('p', {}, _('Unable to clear TMI PoE log: %s').format(error.message)));
			}).finally(function() {
				clearingLog = false;
				button.disabled = m.readonly;
				return updateLog();
			});
		}

		function updateTelemetry() {
			if (!summaryNode)
				return;
			dom.content(summaryNode, [ status
				? _('TMI PoE controller: %s, input voltage: %s V, power budget: %s W').format(
					status.controller || '--',
					status.input_mv == null ? '--' : (status.input_mv / 1000).toFixed(2),
					status.budget_mw == null ? '--' : (status.budget_mw / 1000).toFixed(2))
				: _('Unable to read TMI PoE status. The controller may be unavailable or still initializing.') ]);
			rows.forEach(function(row) {
				const port = status && (status.ports || []).find(p => p.port === row.port);
				row.link.className = 'td' + (port && port.connected === true ? ' tmi-poe-blue' : '');
				const color = !port ? '' : port.powered && port.power_good ? 'green' :
					port.powered || port.power_good ? 'red' : port.mode === 'shutdown' ? 'orange' :
					port.connected === false ? '' : 'blue';
				row.state.className = 'td' + (color ? ' tmi-poe-' + color : '');
				dom.content(row.link, [ port && port.connected === true ? _('TMI PoE LAN link connected') :
					port && port.connected === false ? _('TMI PoE LAN link disconnected') : _('TMI PoE unknown') ]);
				const protocol = port && port.protocol;
				const unconfirmed = protocol === 'unconfirmed' || protocol === 'PoE (unconfirmed)';
				row.protocol.className = 'td' + (protocol ? (unconfirmed ? ' tmi-poe-red' : ' tmi-poe-green') : '');
				dom.content(row.protocol, [ protocol ? (unconfirmed ? _('TMI PoE protocol unconfirmed') : protocol) : '--' ]);
				dom.content(row.state, [ powerState(port) ]);
			});
		}

		// Register every editable widget with the same standard UCI form. Only
		// rendering is customized to keep controls inside the compact status table.
		s.render = function() {
			return Promise.all([
				enabled.render(0, 'main'), budget.render(1, 'main'), debug.render(2, 'main'),
				...outputs.map((o, i) => o.render(i + 3, 'main', true))
			]).then(function(nodes) {
				const headers = [ _('TMI PoE LAN port'), _('TMI PoE LAN link status'), _('TMI PoE output'), _('TMI PoE status'), _('TMI PoE protocol') ]
					.map(title => E('th', { 'class': 'th' }, [ title ]));
				const table = E('table', { 'class': 'table cbi-section-table' }, [
					E('tr', { 'class': 'tr table-titles' }, headers)
				]);
				rows = ports.map(function(port, i) {
					const row = { port: port.port };
					[ [ 'link', _('TMI PoE LAN link status') ], [ 'state', _('TMI PoE status') ],
						[ 'protocol', _('TMI PoE protocol') ] ].forEach(function(cell) {
						row[cell[0]] = E('td', { 'class': 'td', 'data-title': cell[1] });
					});
					table.appendChild(E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td', 'data-title': _('TMI PoE LAN port') }, [ 'LAN%d'.format(port.port) ]),
						row.link, nodes[i + 3], row.state, row.protocol
					]));
					return row;
				});
				summaryNode = E('p');
				statusBox = E('div', { 'class': 'cbi-section',
					'style': uci.get('tmi-poe', 'main', 'enabled') === '0' ? 'display:none' : '' }, [
					E('h3', {}, [ _('TMI PoE status') ]), summaryNode, table
				]);
				logNode = E('textarea', {
					'class': 'cbi-input', 'readonly': '', 'rows': 12, 'spellcheck': 'false',
					'style': 'width:100%;font-family:monospace;resize:vertical'
				});
				const clear = E('button', {
					'type': 'button', 'class': 'cbi-button cbi-button-action',
					'disabled': m.readonly || null, 'click': clearLog
				}, [ _('Clear TMI PoE log') ]);
				nodes[2].classList.add('tmi-poe-debug');
				updateTelemetry();
				updateLog();
				return E('div', { 'data-section-id': 'main' }, [
					E('div', { 'class': 'cbi-section' }, [ nodes[0], nodes[1] ]),
					statusBox,
					E('div', { 'class': 'cbi-section', 'id': 'tmi-poe-log' }, [
						E('div', { 'class': 'tmi-poe-log-header' }, [
							E('h3', {}, [ _('TMI PoE log') ]), clear, nodes[2]
						]), logNode
					])
				]);
			});
		};

		poll.add(function() {
			return Promise.all([ runStatus().then(function(result) {
				status = result;
				maxBudget = status && status.max_budget_mw || maxBudget;
				// Keep form widgets and their unsaved values intact while polling.
				updateTelemetry();
			}), updateLog() ]);
		}, 3);

		return m.render().then(function(node) {
			return E([], [ E('link', { 'rel': 'stylesheet',
				'href': L.resource('tmi-poe/overview.css') }), node ]);
		});
	}
});
