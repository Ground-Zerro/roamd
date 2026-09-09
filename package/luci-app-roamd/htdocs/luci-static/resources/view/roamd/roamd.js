'use strict';
'require view';
'require form';
'require rpc';
'require poll';
'require dom';
'require ui';
'require uci';
'require view.roamd.common as common';

var meshState = {};

var callStatus = rpc.declare({
	object: 'roamd',
	method: 'status',
	expect: { }
});

var callHostHints = rpc.declare({
	object: 'luci-rpc',
	method: 'getHostHints',
	expect: { }
});

var statusData = {};
var hostHints = {};

function hostName(mac) {
	var hint = hostHints[mac.toUpperCase()] || hostHints[mac.toLowerCase()];

	if (hint && hint.name)
		return hint.name;

	if (hint && hint.ipaddrs && hint.ipaddrs.length)
		return hint.ipaddrs[0];

	return '';
}

function toSigned(value) {
	if (value === undefined || value === null)
		return 0;

	return (value > 0x7fffffff) ? value - 4294967296 : value;
}

function formatSignal(value) {
	var dbm = toSigned(value);

	return dbm ? '%d dBm'.format(dbm) : '-';
}

function bandLabel(band) {
	return '%s %s'.format(band, _('GHz'));
}

var PAIR_TEXT = {
	not_managed: _('not handled by the service'),
	no_peer: _('no access point of the other band with this name'),
	disabled: _('one of the access points is disabled'),
	encryption: _('encryption types differ'),
	key: _('passwords differ'),
	network: _('the access points are bridged to different networks'),
	no_11k: _('802.11k is off'),
	no_11v: _('802.11v is off'),
	fast_transition: _('802.11r is configured inconsistently'),
	no_neighbor_report: _('neighbour reports not exchanged yet')
};

var PAIR_STATE = {
	ok: { text: _('works'), color: '#2e9e2e' },
	degraded: { text: _('partly'), color: '#c98000' },
	broken: { text: _('no'), color: '#d33' }
};

function roamingCell(iface) {
	var info = iface.roaming || {};
	var state = PAIR_STATE[info.state] || PAIR_STATE.broken;
	var issues = info.issues || [];
	var lines = [
		E('span', { 'style': 'color:%s;font-weight:bold'.format(state.color) }, state.text)
	];

	if (info.peer && !issues.length)
		lines.push(E('small', {}, [ E('br'), _('paired with %s').format(info.peer) ]));

	issues.forEach(function (code) {
		lines.push(E('small', {}, [ E('br'), PAIR_TEXT[code] || code ]));
	});

	return E('span', {}, lines);
}

function lockLabel(lock) {
	if (lock === '2.4')
		return _('only 2.4 GHz');
	if (lock === '5')
		return _('only 5 GHz');

	return _('both bands');
}

function eachClient(callback) {
	var ifaces = statusData.interfaces || {};

	for (var name in ifaces) {
		var clients = ifaces[name].clients || {};

		for (var mac in clients)
			callback(mac, clients[mac], name, ifaces[name]);
	}
}

function renderInterfaces() {
	var ifaces = statusData.interfaces || {};
	var rows = [];

	for (var name in ifaces) {
		var i = ifaces[name];

		rows.push([
			name,
			i.ssid || '-',
			bandLabel(i.band),
			i.channel || '-',
			i.active ? _('running') : _('down'),
			roamingCell(i)
		]);
	}

	var table = E('table', { 'class': 'table cbi-section-table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Interface')),
			E('th', { 'class': 'th' }, _('SSID')),
			E('th', { 'class': 'th' }, _('Band')),
			E('th', { 'class': 'th' }, _('Channel')),
			E('th', { 'class': 'th' }, _('State')),
			E('th', { 'class': 'th' }, _('Roaming'))
		])
	]);

	cbi_update_table(table, rows, E('em', {}, _('No wireless interfaces found')));

	return table;
}

function renderClients() {
	var rows = [];

	eachClient(function (mac, client, ifname, iface) {
		var bands = client.bands || {};
		var perBand = [];

		for (var b in bands) {
			if (bands[b].seen)
				perBand.push('%s: %s'.format(bandLabel(b), formatSignal(bands[b].signal)));
		}

		rows.push([
			E('span', {}, [ E('strong', {}, mac), E('br'), E('small', {}, hostName(mac)) ]),
			'%s (%s)'.format(ifname, bandLabel(iface.band)),
			formatSignal(client.signal),
			'%t'.format(client.connected || 0),
			perBand.length ? perBand.join(', ') : E('em', {}, _('unknown')),
			lockLabel(client.band_lock),
			client.btm ? _('yes') : _('no'),
			client.rrm ? _('yes') : _('no'),
			client.btm_rejected ? _('rejected') : String(client.steer_count || 0)
		]);
	});

	var table = E('table', { 'class': 'table cbi-section-table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Client')),
			E('th', { 'class': 'th' }, _('Connected to')),
			E('th', { 'class': 'th' }, _('Signal')),
			E('th', { 'class': 'th' }, _('Uptime')),
			E('th', { 'class': 'th' }, _('Signal per band')),
			E('th', { 'class': 'th' }, _('Allowed band')),
			E('th', { 'class': 'th' }, '802.11v'),
			E('th', { 'class': 'th' }, '802.11k'),
			E('th', { 'class': 'th' }, _('Steering attempts'))
		])
	]);

	cbi_update_table(table, rows, E('em', {}, _('No connected clients')));

	return table;
}


var WIZARD_NETWORK = 'lan';
var DEFAULT_SSID = 'OpenWrt';

function deviceBand(section) {
	if (section.band)
		return section.band;

	var hwmode = section.hwmode || '';

	if (hwmode.indexOf('a') >= 0)
		return '5g';

	if (hwmode)
		return '2g';

	if (section.channel && parseInt(section.channel) > 14)
		return '5g';

	return section.channel ? '2g' : null;
}

function bandGroup(band) {
	return (band === '2g') ? 'low' : 'high';
}

function bandTitle(band) {
	if (band === '2g')
		return _('2.4 GHz');
	if (band === '6g')
		return _('6 GHz');

	return _('5 GHz');
}

function defaultNetwork() {
	var ifaces = uci.sections('wireless', 'wifi-iface');

	for (var i = 0; i < ifaces.length; i++) {
		if (ifaces[i].mode === 'ap' && ifaces[i].network)
			return ifaces[i].network;
	}

	return WIZARD_NETWORK;
}

function wizardProbe() {
	var devices = uci.sections('wireless', 'wifi-device');
	var radios = [];
	var groups = {};

	if (!L.hasSystemFeature('hostapd'))
		return { ok: false, reason: 'no_hostapd' };

	for (var i = 0; i < devices.length; i++) {
		var band = deviceBand(devices[i]);

		if (!band)
			continue;

		radios.push({
			name: devices[i]['.name'],
			band: band,
			group: bandGroup(band),
			disabled: (devices[i].disabled == '1')
		});

		groups[bandGroup(band)] = true;
	}

	if (!radios.length)
		return { ok: false, reason: 'no_radio' };

	if (!groups.low || !groups.high)
		return { ok: false, reason: 'single_band', radios: radios };

	return {
		ok: true,
		radios: radios,
		network: defaultNetwork(),
		ft: !!L.hasSystemFeature('hostapd', '11r'),
		sae: !!L.hasSystemFeature('hostapd', 'sae')
	};
}

function probeMessage(reason) {
	if (reason === 'no_hostapd')
		return _('This system has no hostapd with ubus support, so a roaming network cannot be created.');

	if (reason === 'no_radio')
		return _('No wireless radios were found on this device.');

	return _('This device has radios of one band only. Roaming between 2.4 GHz and 5 GHz needs at least two radios on different bands.');
}

function buildPlan(probe, ssid) {
	var ifaces = uci.sections('wireless', 'wifi-iface');
	var byDevice = {};
	var plan = [];

	for (var i = 0; i < ifaces.length; i++) {
		var iface = ifaces[i];

		if (iface.mode !== 'ap' || !iface.device)
			continue;

		var reuse = (iface.ssid === ssid) ? 2 :
			(iface.ssid === DEFAULT_SSID && (!iface.encryption || iface.encryption === 'none')) ? 1 : 0;

		if (reuse && (!byDevice[iface.device] || byDevice[iface.device].rank < reuse))
			byDevice[iface.device] = { sid: iface['.name'], rank: reuse };
	}

	probe.radios.forEach(function (radio) {
		var match = byDevice[radio.name];

		plan.push({
			radio: radio,
			sid: match ? match.sid : null,
			reused: !!match
		});
	});

	return plan;
}

function planWarnings(plan) {
	var warnings = [];
	var enabling = plan.filter(function (p) { return p.radio.disabled; });

	if (enabling.length)
		warnings.push(_('The following radios are currently disabled and will be switched on: %s.')
			.format(enabling.map(function (p) { return p.radio.name; }).join(', ')));

	return warnings;
}

function validate(ssid, key) {
	var bytes = unescape(encodeURIComponent(ssid)).length;

	if (bytes < 1 || bytes > 32)
		return _('The network name must be between 1 and 32 bytes long.');

	if (key.length < 8 || key.length > 63)
		return _('The password must be between 8 and 63 characters long.');

	return null;
}

function applyWizard(probe, ssid, encryption, key) {
	var plan = buildPlan(probe, ssid);
	var network = probe.network;

	plan.forEach(function (entry) {
		var sid = entry.sid || uci.add('wireless', 'wifi-iface');

		uci.set('wireless', sid, 'device', entry.radio.name);
		uci.set('wireless', sid, 'mode', 'ap');
		uci.set('wireless', sid, 'network', network);
		uci.set('wireless', sid, 'ssid', ssid);
		uci.set('wireless', sid, 'encryption', encryption);
		uci.set('wireless', sid, 'key', key);
		uci.unset('wireless', sid, 'disabled');
		uci.unset('wireless', entry.radio.name, 'disabled');
	});

	uci.set('roamd', 'global', 'enabled', '1');

	var filter = uci.get('roamd', 'global', 'ssid');

	if (filter && filter !== ssid)
		uci.set('roamd', 'global', 'ssid', '');

	return uci.save().then(function () {
		return ui.changes.apply(true);
	});
}

function showUnsupported(probe) {
	ui.showModal(_('Roaming network wizard'), [
		E('p', { 'class': 'alert-message warning' }, probeMessage(probe.reason)),
		E('p', {}, _('Nothing has been changed.')),
		E('div', { 'class': 'right' }, [
			E('button', {
				'class': 'btn cbi-button',
				'click': ui.hideModal
			}, _('Close'))
		])
	]);
}

function showWizard(probe) {
	var ssidInput = E('input', {
		'type': 'text',
		'class': 'cbi-input-text',
		'maxlength': '32',
		'placeholder': _('for example, Home')
	});

	var keyInput = E('input', {
		'type': 'password',
		'class': 'cbi-input-password',
		'placeholder': _('at least 8 characters')
	});

	var encSelect = E('select', { 'class': 'cbi-input-select' }, [
		E('option', { 'value': 'psk2' }, _('WPA2 (compatible with everything)'))
	]);

	if (probe.sae) {
		encSelect.appendChild(E('option', { 'value': 'sae-mixed' }, _('WPA2/WPA3 (recommended)')));
		encSelect.appendChild(E('option', { 'value': 'sae' }, _('WPA3 only (modern devices)')));
	}

	var errorBox = E('p', { 'class': 'alert-message warning', 'style': 'display:none' });

	var summary = probe.radios.map(function (radio) {
		return E('li', {}, '%s — %s%s'.format(radio.name, bandTitle(radio.band),
			radio.disabled ? ' (%s)'.format(_('currently disabled')) : ''));
	});

	var notes = [];

	if (!probe.ft)
		notes.push(E('p', { 'class': 'alert-message warning' },
			_('This hostapd build has no 802.11r support. Roaming will work through 802.11k and 802.11v, but transitions will be slower.')));

	planWarnings(buildPlan(probe, '')).forEach(function (text) {
		notes.push(E('p', { 'class': 'alert-message warning' }, text));
	});

	function field(title, widget, help) {
		return E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title' }, title),
			E('div', { 'class': 'cbi-value-field' }, [
				widget,
				help ? E('div', { 'class': 'cbi-value-description' }, help) : ''
			])
		]);
	}

	function submit() {
		var ssid = ssidInput.value.trim();
		var key = keyInput.value;
		var encryption = encSelect.value;
		var check = wizardProbe();

		if (!check.ok) {
			ui.hideModal();
			showUnsupported(check);
			return;
		}

		var error = validate(ssid, key);

		if (error) {
			errorBox.textContent = error;
			errorBox.style.display = '';
			return;
		}

		ui.showModal(_('Roaming network wizard'), [
			E('p', { 'class': 'spinning' }, _('Creating the network…'))
		]);

		applyWizard(check, ssid, encryption, key).catch(function (err) {
			ui.showModal(_('Roaming network wizard'), [
				E('p', { 'class': 'alert-message danger' }, '%h'.format(err)),
				E('div', { 'class': 'right' }, [
					E('button', { 'class': 'btn cbi-button', 'click': ui.hideModal }, _('Close'))
				])
			]);
		});
	}

	ui.showModal(_('Roaming network wizard'), [
		E('p', {}, _('Creates one network on every band with the same name and password, and turns roaming on. Channel, width and other details can be adjusted later under Network → Wireless.')),
		E('p', {}, _('Radios that will be used:')),
		E('ul', {}, summary),
		E([], notes),
		E('div', {}, [
			field(_('Network name (SSID)'), ssidInput),
			field(_('Encryption'), encSelect, _('Only the modes that support fast roaming are offered.')),
			field(_('Password'), keyInput)
		]),
		errorBox,
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn cbi-button', 'click': ui.hideModal }, _('Cancel')),
			' ',
			E('button', { 'class': 'btn cbi-button cbi-button-positive', 'click': submit }, _('Create network'))
		])
	]);
}

function renderWizardButton() {
	return E('div', { 'style': 'margin: 1em 0' }, [
		E('button', {
			'class': 'btn cbi-button cbi-button-action',
			'click': function () {
				uci.load([ 'wireless', 'roamd' ]).then(function () {
					var probe = wizardProbe();

					if (probe.ok)
						showWizard(probe);
					else
						showUnsupported(probe);
				});
			}
		}, _('Create a roaming network…')),
		E('div', { 'class': 'cbi-value-description' },
			_('Quick setup: one name and password for both bands, roaming enabled automatically.'))
	]);
}

function renderStatus() {
	return E('div', {}, [
		E('h3', {}, _('Wireless interfaces')),
		E('div', { 'id': 'roamd-ifaces' }, renderInterfaces()),
		common.isNode(meshState) ? '' : renderWizardButton(),
		E('h3', {}, _('Clients')),
		E('div', { 'id': 'roamd-clients' }, renderClients())
	]);
}

var StatusTab = form.DummyValue.extend({
	render: function () {
		return E('div', { 'class': 'cbi-section' }, [
			E('div', { 'id': 'roamd-status' }, renderStatus())
		]);
	},
	load: function () { return null; },
	parse: function () { return Promise.resolve(); },
	cfgvalue: function () { return null; },
	formvalue: function () { return null; },
	write: function () {},
	remove: function () {}
});

return view.extend({
	load: function () {
		return Promise.all([
			callStatus().catch(function () { return {}; }),
			callHostHints().catch(function () { return {}; }),
			common.meshStatus()
		]);
	},

	render: function (data) {
		var m, s, sub, o;

		statusData = data[0] || {};
		hostHints = data[1] || {};
		meshState = data[2] || {};

		poll.add(function () {
			return callStatus().then(function (res) {
				var ifaces = document.getElementById('roamd-ifaces');
				var clients = document.getElementById('roamd-clients');

				statusData = res || {};

				if (ifaces)
					dom.content(ifaces, renderInterfaces());

				if (clients)
					dom.content(clients, renderClients());
			});
		}, 5);

		m = new form.Map('roamd', _('Wi-Fi Roaming'),
			_('Seamless roaming between the 2.4 GHz and 5 GHz networks of this router. Both bands must use the same SSID and the same password.'));

		m.readonly = common.isNode(meshState);

		s = m.section(form.NamedSection, 'global', 'roamd');
		s.addremove = false;

		s.tab('status', _('Roaming status'));
		s.tab('general', _('General'));
		s.tab('policy', _('Switching parameters'));

		s.taboption('status', StatusTab, '_status');

		o = s.taboption('general', form.Flag, 'enabled', _('Enable roaming'),
			_('Start the roaming daemon.'));
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'ssid', _('Network (SSID)'),
			_('Leave empty to manage every access point of this router.'));
		o.placeholder = _('all networks');

		o = s.taboption('general', form.Flag, 'band_steering', _('Band steering'),
			_('Move dual-band clients to the preferred band when its signal is good enough.'));
		o.default = '1';

		o = s.taboption('general', form.ListValue, 'prefer_band', _('Preferred band'));
		o.value('5', _('5 GHz'));
		o.value('2', _('2.4 GHz'));
		o.value('none', _('No preference'));
		o.default = '5';
		o.depends('band_steering', '1');

		o = s.taboption('general', form.Flag, 'fast_transition', _('Fast roaming (802.11r)'),
			_('Fast transition between the bands without a full reauthentication. Requires WPA2/WPA3 with a pre-shared key. Very old clients may fail to connect.'));
		o.default = '1';

		o = s.taboption('general', form.Flag, 'ft_over_ds', _('802.11r over the distribution system'),
			_('Perform the fast transition over the wired side instead of over the air.'));
		o.default = '1';
		o.depends('fast_transition', '1');

		o = s.taboption('general', form.Value, 'mobility_domain', _('Mobility domain'),
			_('Four hexadecimal digits shared by all access points of one roaming domain. Leave empty to derive it from the SSID.'));
		o.datatype = 'and(hexstring,length(4))';
		o.placeholder = _('automatic');
		o.depends('fast_transition', '1');

		o = s.taboption('general', form.Flag, 'neighbor_reports', _('Neighbour reports (802.11k)'),
			_('Tell clients about the other band so they can find it without a full scan.'));
		o.default = '1';

		o = s.taboption('general', form.Flag, 'bss_transition', _('Assisted roaming (802.11v)'),
			_('Ask connected clients to move to the other band.'));
		o.default = '1';

		o = s.taboption('general', form.Flag, 'apply_wireless', _('Configure wireless automatically'),
			_('Write the 802.11k/v/r options into the wireless configuration. Turn this off to manage them by hand.'));
		o.default = '1';

		o = s.taboption('policy', form.SectionValue, '_policy', form.NamedSection, 'policy', 'policy',
			null, _('The defaults suit most homes. Change them only if the behaviour does not suit you.'));
		sub = o.subsection;
		sub.addremove = false;

		o = sub.option(form.Value, 'rssi_good', _('Good signal'),
			_('Above this level the preferred band is considered usable and the client is steered to it.'));
		o.datatype = 'range(-100,-1)';
		o.default = '-55';

		o = sub.option(form.Value, 'rssi_low', _('Weak signal'),
			_('Below this level the preferred band is not offered to the client.'));
		o.datatype = 'range(-100,-1)';
		o.default = '-75';

		o = sub.option(form.Value, 'rssi_diff', _('Band difference'),
			_('Keep the client where it is when the current band is this many dB stronger.'));
		o.datatype = 'range(1,60)';
		o.default = '30';

		o = sub.option(form.Value, 'cross_band_delta', _('Cross-band correction'),
			_('Assumed difference in dB between the bands when no measurement of the other band is available.'));
		o.datatype = 'range(0,30)';
		o.default = '8';

		o = sub.option(form.Value, 'kick_rssi', _('Disconnect threshold'),
			_('A client below this level is moved away even from the preferred band.'));
		o.datatype = 'range(-100,-1)';
		o.default = '-82';

		o = sub.option(form.Flag, 'deny_probe', _('Hide the non-preferred band'),
			_('Do not answer probe and authentication requests on the non-preferred band while the preferred one is good.'));
		o.default = '1';

		o = sub.option(form.Flag, 'allow_kick', _('Force disconnect'),
			_('Disconnect clients that ignore the roaming request. For devices without 802.11v support this is the only way to move them: until it is enabled, such a client stays on a dying link until it decides to leave on its own, which can take minutes. Some clients reconnect to the same band.'));
		o.default = '0';

		o = sub.option(form.Value, 'kick_delay', _('Delay before disconnect'),
			_('Milliseconds between the roaming request and the forced disconnect.'));
		o.datatype = 'uinteger';
		o.default = '5000';
		o.depends('allow_kick', '1');

		o = sub.option(form.Value, 'hold_time', _('Hold time'),
			_('Milliseconds to wait before steering the same client again.'));
		o.datatype = 'uinteger';
		o.default = '30000';

		o = sub.option(form.Value, 'steer_retries', _('Steering attempts'),
			_('Give up after this many unsuccessful attempts.'));
		o.datatype = 'range(1,10)';
		o.default = '3';

		o = sub.option(form.Value, 'deny_time', _('Block duration'),
			_('Milliseconds the non-preferred band stays hidden from one client.'));
		o.datatype = 'uinteger';
		o.default = '15000';

		o = sub.option(form.Value, 'check_time_high', _('5 GHz observation window'),
			_('Milliseconds a 5 GHz measurement stays valid for the connection decision.'));
		o.datatype = 'uinteger';
		o.default = '4000';

		o = sub.option(form.Value, 'check_time_low', _('2.4 GHz observation window'),
			_('Milliseconds a 2.4 GHz measurement stays valid for the connection decision.'));
		o.datatype = 'uinteger';
		o.default = '8000';

		o = sub.option(form.Value, 'age_time', _('Client record lifetime'),
			_('Milliseconds an inactive client is remembered.'));
		o.datatype = 'uinteger';
		o.default = '90000';

		o = sub.option(form.Value, 'poll_interval', _('Poll interval'),
			_('Milliseconds between signal measurements.'));
		o.datatype = 'uinteger';
		o.default = '2000';

		o = sub.option(form.Value, 'beacon_req_interval', _('Beacon request interval'),
			_('Milliseconds between 802.11k measurements of the other band.'));
		o.datatype = 'uinteger';
		o.default = '20000';

		o = sub.option(form.ListValue, 'log_level', _('Log level'));
		o.value('0', _('Errors only'));
		o.value('1', _('Normal'));
		o.value('2', _('Debug'));
		o.default = '1';

		if (!common.isNode(meshState))
			return m.render();

		this.handleSave = null;
		this.handleSaveApply = null;
		this.handleReset = null;

		return m.render().then(function (form) {
			return E('div', {}, [ common.controlBanner(meshState), form ]);
		});
	}
});
