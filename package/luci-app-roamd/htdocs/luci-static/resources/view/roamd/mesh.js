'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require ui';
'require view.roamd.common as common';
'require uci';

var callMeshStatus = rpc.declare({
	object: 'roamd',
	method: 'mesh_status',
	expect: { }
});

var callMeshClients = rpc.declare({
	object: 'roamd',
	method: 'mesh_clients',
	expect: { clients: [] }
});

var callMeshSelfCheck = rpc.declare({
	object: 'roamd',
	method: 'mesh_self_check',
	expect: { }
});

var callMeshClientForget = rpc.declare({
	object: 'roamd',
	method: 'mesh_client_forget',
	params: [ 'mac' ],
	expect: { }
});

var callMeshClientUpdate = rpc.declare({
	object: 'roamd',
	method: 'mesh_client_update',
	params: [ 'mac', 'band', 'alias', 'nodes' ],
	expect: { }
});

var callMeshDiscover = rpc.declare({
	object: 'roamd',
	method: 'mesh_discover',
	params: [ 'rescan' ],
	expect: { }
});

var callMeshDiscoverStop = rpc.declare({
	object: 'roamd',
	method: 'mesh_discover',
	params: [ 'stop' ],
	expect: { }
});

var callMeshAcquire = rpc.declare({
	object: 'roamd',
	method: 'mesh_acquire',
	params: [ 'addr' ],
	expect: { }
});

var callMeshAcquireStatus = rpc.declare({
	object: 'roamd',
	method: 'mesh_acquire_status',
	params: [ 'task_id' ],
	expect: { }
});

var callMeshMemberRemove = rpc.declare({
	object: 'roamd',
	method: 'mesh_member_remove',
	params: [ 'id', 'reset' ],
	expect: { }
});

var callMeshLog = rpc.declare({
	object: 'roamd',
	method: 'mesh_log',
	expect: { events: [] }
});

var callMeshUpdate = rpc.declare({
	object: 'roamd',
	method: 'mesh_update',
	params: [ 'id' ],
	expect: { }
});

var callMeshSelfUpdate = rpc.declare({
	object: 'roamd',
	method: 'mesh_self_update',
	expect: { }
});

var callMeshMemberUpdate = rpc.declare({
	object: 'roamd',
	method: 'mesh_member_update',
	params: [ 'id', 'name' ],
	expect: { }
});

var callMeshSettings = rpc.declare({
	object: 'roamd',
	method: 'mesh_settings',
	params: [ 'backhaul_enabled', 'backhaul_ssid', 'backhaul_key', 'wifi_shutdown', 'backhaul_delta',
		'backhaul_min_signal', 'auto_update', 'auto_update_every', 'auto_update_unit', 'pkg_url' ],
	expect: { }
});

var callMeshControllerName = rpc.declare({
	object: 'roamd',
	method: 'mesh_settings',
	params: [ 'controller_name' ],
	expect: { }
});

var EVENT_TYPE = {
	connect: _('connected'),
	disconnect: _('disconnected'),
	roam: _('roaming'),
	steer: _('steering'),
	kick: _('forced disconnect')
};

var CONS_ICON = { ok: '', degraded: '⚠', broken: '⛔' };
var ISSUE_TEXT = {
	ssid: _('SSID differs'),
	encryption: _('encryption differs'),
	mobility_domain: _('mobility domain differs'),
	ieee80211r: _('802.11r differs')
};

function consistencyCell(online, m) {
	var state = online ? E('span', {}, _('online')) : E('span', { 'style': 'color:#888' }, _('offline'));

	if (!online || !m.consistency || m.consistency === 'ok')
		return E('div', {}, state);

	var issues = (m.issues || []).map(function(i) { return ISSUE_TEXT[i] || i; }).join(', ');
	var color = m.consistency === 'broken' ? '#c00' : '#c60';
	var label = m.consistency === 'broken' ? _('roaming broken') : _('roaming degraded');

	return E('div', {}, [
		state, E('br'),
		E('small', { 'style': 'color:%s'.format(color), 'title': issues },
			'%s %s'.format(CONS_ICON[m.consistency], label))
	]);
}

var nodeNames = {};

function nodeTitle(node) {
	if (!node || node === 'controller')
		return nodeNames.controller || _('controller');

	return nodeNames[node] || node;
}

function nodeLabel(node, band) {
	var name = nodeTitle(node);
	return band ? '%s · %s %s'.format(name, band, _('GHz')) : name;
}

function nodeCell(node, band) {
	if (!node && !band)
		return '-';

	return E('div', {}, [
		E('span', {}, nodeTitle(node)),
		band ? E('div', {}, E('small', { 'style': 'color:#888' }, '%s %s'.format(band, _('GHz')))) : ''
	]);
}

function timeCell(ts) {
	var d = new Date((ts || 0) * 1000);

	return E('div', {}, [
		E('span', {}, d.toLocaleTimeString()),
		E('div', {}, E('small', { 'style': 'color:#888' }, d.toLocaleDateString()))
	]);
}

var logNames = {};

function clientCell(mac) {
	var name = logNames[(mac || '').toLowerCase()];

	if (!name)
		return mac || '';

	return E('div', {}, [
		E('div', {}, E('strong', {}, name)),
		E('div', {}, E('small', { 'style': 'color:#888' }, mac))
	]);
}

function logRow(ev) {
	return [
		timeCell(ev.ts),
		clientCell(ev.mac),
		nodeCell(ev.from_node, ev.from_band),
		nodeCell(ev.to_node, ev.to_band),
		EVENT_TYPE[ev.type] || ev.type || ''
	];
}

function logCsv(events) {
	var head = [ 'time', 'client', 'from', 'to', 'type' ];
	var rows = events.map(function(ev) {
		return [ new Date((ev.ts || 0) * 1000).toISOString(), ev.mac || '',
			nodeLabel(ev.from_node, ev.from_band), nodeLabel(ev.to_node, ev.to_band),
			ev.type || '' ];
	});

	return [ head ].concat(rows).map(function(r) {
		return r.map(function(c) { return '"' + String(c).replace(/"/g, '""') + '"'; }).join(',');
	}).join('\n');
}

function downloadCsv(events) {
	var blob = new Blob([ logCsv(events) ], { type: 'text/csv' });
	var url = URL.createObjectURL(blob);
	var a = E('a', { 'href': url, 'download': 'mesh-transitions.csv' });

	document.body.appendChild(a);
	a.click();
	document.body.removeChild(a);
	URL.revokeObjectURL(url);
}

var ACQUIRE_ERR = {
	spawn_failed: _('Could not start the capture task.'),
	busy: _('A capture is already in progress.'),
	no_such_task: _('The capture task is gone.'),
	no_address: _('No device address given.')
};

var ACQUIRE_ORDER = [ 'probe', 'install', 'enroll', 'network', 'profile', 'done' ];

var ACQUIRE_MSG = [
	[ /^updating roamd on the node$/, function() { return _('installing roamd on the node'); } ],
	[ /^updated$/, function() { return _('the node is up to date'); } ],
	[ /^checking the repository$/, function() { return _('asking the repository'); } ],
	[ /^the controller is updated$/, function() { return _('the controller is updated'); } ],
	[ /^update installed$/, function() { return _('the update is installed'); } ],
	[ /^no updates$/, function() { return _('no updates available'); } ],
	[ /^some nodes were not updated$/, function() { return _('some nodes were not updated'); } ],
	[ /^node is unreachable$/, function() { return _('the node is unreachable'); } ],
	[ /^unknown OpenWrt version$/, function() { return _('unknown OpenWrt version'); } ],
	[ /^no package for the OpenWrt version of the node$/, function() { return _('no package for the OpenWrt version of the node'); } ],
	[ /^package installation failed on the node$/, function() { return _('the package could not be installed on the node'); } ],
	[ /^curl cannot be installed — no control channel to the node$/, function() { return _('curl could not be installed: without it the controller has no access to the package repository and no control channel to the node'); } ],
	[ /^package source is not trusted: .*$/, function() { return _('the package source is not trusted: the repository must be https and its certificate must be in /etc/roamd/pkg.crt'); } ],
	[ /^installing roamd and interface for (.+)$/, function(m) { return _('packages for OpenWrt %s').format(m[1]); } ],
	[ /^repository unreachable, installed from the controller cache$/, function() { return _('repository unreachable, installed from the cache'); } ],
	[ /^assigning node role$/, function() { return _('assigning the node role'); } ],
	[ /^switching the node to the controller subnet$/, function() { return _('switching to the controller subnet'); } ],
	[ /^checking 802.11v support on the node$/, function() { return _('checking 802.11v support'); } ],
	[ /^waiting for the node to broadcast the network$/, function() { return _('waiting for the network to go on air'); } ],
	[ /^the node broadcasts the network on (\d+) radio\(s\)$/, function(m) { return _('the network is on air on %s radio(s)').format(m[1]); } ],
	[ /^securing the control channel$/, function() { return _('securing the control channel'); } ],
	[ /^installing service key$/, function() { return _('installing the service key'); } ],
	[ /^service key works, password login disabled on the node$/, function() { return _('service key works, password login disabled'); } ],
	[ /^key login does not work, the node restores password login by itself in 90s$/, function() { return _('key login failed, the node restores password login itself'); } ],
	[ /^service key was not installed, password login left enabled on the node$/, function() { return _('service key was not installed, password login left enabled'); } ],
	[ /^resetting the node to factory settings$/, function() { return _('resetting the device to factory settings'); } ],
	[ /^the node is resetting to factory settings$/, function() { return _('the device is resetting to factory settings'); } ],
	[ /^the node did not accept the reset, release it manually$/, function() { return _('the device did not accept the reset — reset it manually'); } ],
	[ /^the node is still reachable, the reset may have failed$/, function() { return _('the device still answers — the reset may have failed'); } ],
	[ /^the node is not in the system$/, function() { return _('the device is not in the system'); } ],
	[ /^installing packages on the controller$/, function() { return _('installing the packages'); } ],
	[ /^package index is not available$/, function() { return _('the package index is not available'); } ],
	[ /^package installation failed$/, function() { return _('the packages could not be installed'); } ],
	[ /^wpad: (.+)$/, function(m) { return m[1]; } ]
];

function acquireMsgText(msg) {
	if (!msg)
		return '';

	for (var i = 0; i < ACQUIRE_MSG.length; i++) {
		var m = ACQUIRE_MSG[i][0].exec(msg);

		if (m)
			return ACQUIRE_MSG[i][1](m);
	}

	return msg;
}

var ACQUIRE_STEP = {
	check: _('checking for updates'),
	probe: _('checking the device'),
	reset: _('resetting the device'),
	update: _('updating packages'),
	compat: _('compatibility'),
	install: _('installing packages'),
	network: _('switching to the single subnet'),
	profile: _('rolling out the profile'),
	enroll: _('enrolling the node'),
	done: _('finished')
};

function acquireErrText(msg) {
	return ACQUIRE_ERR[msg] || msg;
}

function acquireStepText(step) {
	return ACQUIRE_STEP[step] || step;
}

function discoverDialog() {
	var body = E('div', {}, [ E('p', { 'class': 'spinning' }, _('Scanning the local network…')) ]);
	var open = true;

	function close() {
		open = false;
		ui.hideModal();
		callMeshDiscoverStop(true);
	}

	ui.showModal(_('Add a new node'), [
		body,
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': close }, _('Close'))
		])
	]);

	var tick = function(rescan) {
		callMeshDiscover(rescan).then(function(res) {
			if (!open)
				return;

			res = res || {};
			var list = res.candidates || [];

			if (res.scanning) {
				window.setTimeout(function() { tick(false); }, 3000);
				return;
			}

			if (!list.length) {
				dom.content(body, E('p', {}, _('No devices found. Reset the new device to factory settings and make sure it has no password, then try again.')));
				return;
			}

			dom.content(body, list.map(renderCandidate));
		});
	};

	tick(true);
}

var PKG_STATE_TEXT = {
	missing: _('OpenWrt %s (%s) is not supported: the repository has no roamd package for this version and architecture'),
	unreachable: _('Could not check for the roamd package for OpenWrt %s (%s): the package repository is unreachable. Check the internet access of the controller and search again')
};

function renderCandidate(c) {
	var ready = c.pkg === 'ready';
	var lines = [
		E('strong', {}, c.name || c.addr),
		E('br'),
		E('small', {}, '%s · %s · %s · OpenWrt %s'.format(c.addr, c.mac, c.model || '?', c.os || '?'))
	];

	if (!ready)
		lines.push(E('div', { 'style': 'color:#c33' }, E('small', {},
			PKG_STATE_TEXT[c.pkg].format(c.os || '?', c.arch || '?'))));

	var action = E('button', {
		'class': 'btn cbi-button cbi-button-action',
		'disabled': ready ? null : 'disabled',
		'click': function(ev) { startAcquire(c.addr, ev.target); }
	}, _('Capture'));

	return E('div', { 'class': 'cbi-section', 'style': 'display:flex;justify-content:space-between;align-items:center' },
		[ E('div', {}, lines), action ]);
}

function startAcquire(addr, button) {
	var body = E('div', {}, [ E('p', { 'class': 'spinning' }, _('Capturing %s…').format(addr)) ]);

	if (button)
		button.disabled = true;

	ui.showModal(_('Capturing the node'), [ body ]);

	callMeshAcquire(addr).then(function(res) {
		res = res || {};

		if (res.error && !res.task_id) {
			dom.content(body, [
				E('p', { 'class': 'alert-message warning' }, acquireErrText(res.error)),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')))
			]);
			return;
		}

		if (res.error === 'busy')
			dom.content(body, E('p', {}, _('This node is already being captured, showing the progress.')));

		pollAcquire(res.task_id, body, 0, 'acquire');
	});
}

function acquireProgress(steps, kind) {
	var state = {};
	var first = steps.length ? steps[0].step : null;
	var order = (kind === 'release' || first === 'reset') ? [ 'reset', 'done' ]
		: (kind === 'autoupdate' || first === 'check') ? [ 'check', 'update', 'done' ]
		: (kind === 'update' || kind === 'selfupdate' || first === 'update') ? [ 'update', 'done' ]
		: ACQUIRE_ORDER.slice();
	var reached = -1;
	var failed = null;

	steps.forEach(function(s) {
		if (order.indexOf(s.step) < 0)
			order.splice(order.length - 1, 0, s.step);

		state[s.step] = s;

		if (s.status === 'error')
			failed = s.step;

		var i = order.indexOf(s.step);
		if (i > reached)
			reached = i;
	});

	return order.map(function(step, i) {
		var s = state[step] || {};
		var done = s.status === 'ok' || (state[step] && i < reached && s.status !== 'error');
		var mark = s.status === 'error' ? '✕' : (done ? '✓' : (i === reached ? '·' : '·'));
		var text;

		if (s.status === 'error')
			text = acquireErrText(acquireMsgText(s.message));
		else if (!state[step])
			text = _('waiting');
		else if (done && step === 'done')
			text = kind === 'autoupdate' ? acquireMsgText(s.message)
				: kind === 'selfupdate' ? _('the controller is up to date')
				: (kind === 'update' || s.message === 'updated') ? _('the node is up to date')
				: kind === 'release' ? _('the device is released')
				: _('the node is part of the system');
		else
			text = acquireMsgText(s.message) || (done ? _('done') : _('in progress'));

		return E('div', {
			'style': s.status === 'error' ? 'color:#c33'
				: (state[step] ? '' : 'color:#999')
		}, '%s %s — %s'.format(mark, acquireStepText(step), text));
	});
}

function pollAcquire(task, body, misses, kind) {
	callMeshAcquireStatus(task).then(function(res) {
		res = res || {};

		if (res.error) {
			if ((misses || 0) < 10) {
				window.setTimeout(function() { pollAcquire(task, body, (misses || 0) + 1, kind); }, 2000);
				return;
			}

			dom.content(body, [
				E('p', { 'class': 'alert-message warning' }, acquireErrText(res.error)),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': function() { ui.hideModal(); location.reload(); } }, _('Close')))
			]);
			return;
		}

		var steps = res.steps || [];
		var last = steps[steps.length - 1] || {};

		if (res.running) {
			dom.content(body, [
				E('div', {}, acquireProgress(steps, kind)),
				E('p', { 'class': 'spinning' },
					_('The operation is running, it takes a few minutes. You can close this window — the process continues.')),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')))
			]);
			window.setTimeout(function() { pollAcquire(task, body, 0, kind); }, 2000);
			return;
		}

		var ok = last.step === 'done' && last.status === 'ok';
		dom.content(body, [
			E('div', {}, acquireProgress(steps, kind)),
			E('p', { 'class': ok ? '' : 'alert-message warning' },
				kind === 'release' ? (ok ? _('The node has been removed from the system.') : _('The node could not be removed.'))
					: (ok ? _('Finished.') : _('Not finished.'))),
			E('div', { 'class': 'right' }, E('button', { 'class': 'btn cbi-button-positive', 'click': function() { ui.hideModal(); location.reload(); } }, _('Done')))
		]);
	});
}

function renderNodes(state) {
	var intro = E('div', {}, [
		E('p', {}, _('This device works as the Mesh Wi-Fi system controller. Add other OpenWrt devices to build one wireless system with central management and monitoring.')),
		E('p', {}, _('To add a node: reset the new device to factory settings and make sure it has no administrator password.')),
		E('p', {}, E('strong', { 'style': 'color:#c33' },
			_('Connect a LAN port of the controller to a LAN port of the new device — not to its WAN port.'))),
		E('p', {}, _('Then press "Add a new node" and capture the device that appears.')),
		E('p', { 'class': 'cbi-value-description' }, _('If the device is not found or the capture does not finish, reset it to factory settings and try again.'))
	]);

	var deps = state.deps_ready ? '' : E('div', { 'class': 'alert-message warning' }, [
		E('strong', {}, _('Preparing the controller is not finished')),
		E('div', {}, state.deps_state || _('the required packages are not installed yet')),
		E('div', {}, E('small', {}, _('The controller retries by itself every five minutes; make sure it has Internet access.')))
	]);

	var busy = state.acquire && state.acquire.running;

	var addButton = E('button', {
		'class': 'btn cbi-button cbi-button-add',
		'click': discoverDialog,
		'disabled': busy ? 'disabled' : null
	}, _('Add a new node'));

	var kind = busy ? state.acquire.kind : '';
	var kindTitle = kind === 'autoupdate' ? _('Automatic update')
		: kind === 'selfupdate' ? _('Updating the controller')
		: kind === 'update' ? _('Updating the node')
		: kind === 'release' ? _('Remove the node') : _('Capturing the node');
	var kindNote = kind === 'autoupdate' ? _('An automatic update is running now')
		: kind === 'selfupdate' ? _('The controller is being updated now')
		: kind === 'update' ? _('A node is being updated now')
		: kind === 'release' ? _('A node is being removed now') : _('A node is being captured now');

	var running = !busy ? '' : E('div', { 'class': 'alert-message' }, [
		E('strong', {}, kindNote),
		E('div', {}, state.acquire.addr
			? _('Device %s, it takes a few minutes. Do not start another task and do not reset the device.').format(state.acquire.addr)
			: _('It takes a few minutes. Do not start another task and do not reset the device.')),
		E('div', {}, E('button', {
			'class': 'btn cbi-button',
			'click': function() {
				var body = E('div', {}, E('p', { 'class': 'spinning' }, _('Reading the progress…')));

				ui.showModal(kindTitle, [ body ]);
				pollAcquire(state.acquire.task_id, body, 0, kind);
			}
		}, _('Show progress')))
	]);

	var members = state.members || [];
	var table = E('table', { 'class': 'table cbi-section-table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Node name')),
			E('th', { 'class': 'th' }, _('Clients')),
			E('th', { 'class': 'th' }, _('Status')),
			E('th', { 'class': 'th' }, _('Uptime')),
			E('th', { 'class': 'th' }, _('IP address')),
			E('th', { 'class': 'th' }, _('MAC')),
			E('th', { 'class': 'th' }, _('Via')),
			E('th', { 'class': 'th' }, _('Connection')),
			E('th', { 'class': 'th' }, _('Version')),
			E('th', { 'class': 'th' }, '')
		])
	]);
	var rows = [ controllerRow(state) ].concat(members.map(function(m) {
		var srcHint = { repository: _('package taken from the repository'),
				cache: _('repository was unreachable, package taken from the controller cache') };
		var ver = versionCell(m.os_version, m.pkg_version, [
			m.pkg_source === 'cache' ? ' ⚠' : '',
			m.update_available ? E('span', { 'style': 'color:#c60', 'title': _('update available') }, ' ⬆') : ''
		]);

		if (srcHint[m.pkg_source])
			ver.setAttribute('title', srcHint[m.pkg_source]);

		var actions = E('div', {}, [
			state.auto_update ? '' : E('button', {
				'class': 'btn cbi-button cbi-button-apply',
				'style': 'margin-right:.3em',
				'disabled': m.update_available && !busy ? null : 'disabled',
				'title': busy ? _('another task is running') :
					(m.update_available ? _('update available') : _('no updates available')),
				'click': function() { updateMember(m.id, m.name); }
			}, _('Update')),
			E('button', {
				'class': 'btn cbi-button cbi-button-remove',
				'click': function() { removeMember(m.id, m.name); }
			}, _('Remove'))
		]);

		var conn = m.connection === 'wifi'
			? (m.uplink_band ? '%s %s %s'.format(_('Wi-Fi'), m.uplink_band, _('GHz')) : _('Wi-Fi'))
			: (m.connection === 'wired' ? _('Cable') : '-');
		var via = viaLabel(m, state);

		return [ nameCell(m), m.client_count || 0, consistencyCell(m.online, m),
			m.uptime ? '%t'.format(m.uptime) : '-', nodeAddrCell(m), m.mac || '-',
			via, conn, ver, actions ];
	}));

	cbi_update_table(table, rows, E('em', {}, _('No nodes yet')));

	return E('div', {}, [ deps, running, intro, E('div', {}, addButton), E('br'), table ]);
}

function versionCell(os, pkg, marks) {
	var lines = [ os ? 'OpenWrt %s'.format(os) : '-' ];

	if (pkg)
		lines.push(E('br'), 'roamd %s'.format(pkg));

	return E('div', { 'style': 'white-space:nowrap' }, lines.concat(marks || []));
}

function controllerRow(state) {
	var self = state.self || {};
	var name = state.controller_name || self.hostname || _('This device');
	var cell = E('div', {}, [
		E('div', {}, [
			E('strong', {}, name),
			pencil(function() { renameController(state.controller_name || ''); })
		]),
		E('div', {}, E('small', { 'style': 'color:#888' }, _('controller')))
	]);
	var ver = versionCell(self.os_version, self.pkg_version);

	var check = state.auto_update ? '' : E('button', {
		'class': 'btn cbi-button',
		'disabled': readOnly ? 'disabled' : null,
		'click': function() { checkSelfUpdate(); }
	}, _('Check for updates'));

	return [ cell, state.controller_clients || 0,
		E('span', { 'style': 'color:#2e9e2e', 'title': _('this device') }, '●'),
		self.uptime ? '%t'.format(self.uptime) : '-',
		self.addr || '-', self.mac || '-',
		'—', _('this device'), ver, check ];
}

function checkSelfUpdate() {
	var body = E('div', {}, E('p', { 'class': 'spinning' }, _('Checking the repository…')));

	ui.showModal(_('Check for updates'), [ body ]);

	callMeshSelfCheck().then(function(res) {
		res = res || {};

		if (res.error === 'no_repository') {
			dom.content(body, [
				E('p', {}, _('The roamd package repository is not connected on this device. Run install.sh again to add it.')),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')))
			]);
			return;
		}

		if (res.error || !res.packages) {
			dom.content(body, [
				E('p', { 'class': 'alert-message warning' }, _('The repository is unreachable.')),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')))
			]);
			return;
		}

		var rows = res.packages.map(function(p) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, p.name),
				E('td', { 'class': 'td' }, p.installed || '-'),
				E('td', { 'class': 'td' }, p.available || '-'),
				E('td', { 'class': 'td' }, p.outdated ? E('strong', { 'style': 'color:#c60' }, _('update available')) : _('up to date'))
			]);
		});

		var buttons = [ E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')) ];

		if (res.update_available && !readOnly)
			buttons.push(' ', E('button', {
				'class': 'btn cbi-button cbi-button-action',
				'click': function() { startSelfUpdate(body); }
			}, _('Update the controller')));

		dom.content(body, [
			E('p', {}, res.update_available
				? _('A newer version is available. The controller can install it itself.')
				: _('This device runs the newest packages from the repository.')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Package')),
					E('th', { 'class': 'th' }, _('Installed')),
					E('th', { 'class': 'th' }, _('Repository')),
					E('th', { 'class': 'th' }, '')
				])
			].concat(rows)),
			E('div', { 'class': 'right' }, buttons)
		]);
	}, function() {
		dom.content(body, [
			E('p', { 'class': 'alert-message warning' }, _('The check failed.')),
			E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')))
		]);
	});
}

function startSelfUpdate(body) {
	dom.content(body, E('p', { 'class': 'spinning' }, _('Installing the packages…')));

	callMeshSelfUpdate().then(function(res) {
		res = res || {};

		if (res.error && !res.task_id) {
			dom.content(body, [
				E('p', { 'class': 'alert-message warning' }, acquireErrText(res.error)),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')))
			]);
			return;
		}

		pollAcquire(res.task_id, body, 0, 'selfupdate');
	});
}

function renameController(current) {
	nameDialog(_('Controller name'), current, function(name) {
		if (name === current)
			return;

		callMeshControllerName(name).then(refreshNodes);
	});
}

function nodeAddrCell(m) {
	if (!m.addr)
		return '-';

	return E('span', {}, [
		m.addr, ' ',
		E('a', {
			'href': 'http://%s/'.format(m.addr),
			'target': '_blank',
			'rel': 'noreferrer',
			'title': _('Open the node web interface'),
			'style': 'text-decoration:none'
		}, '🌐')
	]);
}

function nameCell(m) {
	var alias = m.name || m.hostname || m.id;
	var sys = (m.hostname && m.hostname !== alias) ? m.hostname : '';

	return E('div', {}, [
		E('div', {}, [
			E('strong', {}, alias),
			pencil(function() { renameMember(m.id, m.name); })
		]),
		sys ? E('div', {}, E('small', { 'style': 'color:#888' }, sys)) : ''
	]);
}

function renameMember(id, current) {
	nameDialog(_('Rename node'), current, function(name) {
		if (name === current)
			return;

		callMeshMemberUpdate(id, name).then(refreshNodes);
	});
}

function refreshNodes() {
	return callMeshStatus().then(function(fresh) {
		var pane = document.getElementById('mesh-nodes-pane');

		fresh = fresh || {};

		if (pane)
			dom.content(pane, renderNodes(fresh));

		settingsSync(fresh);
	});
}

function bssidOwner(bssid, aps) {
	for (var i = 0; i < (aps || []).length; i++)
		if (aps[i].bssid === bssid)
			return true;

	return false;
}

function viaLabel(m, state) {
	if (!m.online || !m.via)
		return '-';

	if (m.via === 'controller' || bssidOwner(m.via, state.self_aps))
		return controllerLabel(state);

	var members = state.members || [];

	for (var i = 0; i < members.length; i++)
		if (bssidOwner(m.via, members[i].aps))
			return members[i].name;

	return m.via;
}

function removeMember(id, name) {
	var reset = E('input', { 'type': 'checkbox', 'checked': 'checked' });

	ui.showModal(_('Remove the node'), [
		E('p', {}, _('Remove the node %s from the Wi-Fi system?').format(name || id)),
		E('label', { 'class': 'cbi-value', 'style': 'display:block' }, [
			reset, ' ', _('Reset the device to factory settings')
		]),
		E('p', { 'class': 'cbi-value-description' },
			_('With the reset the device becomes independent again and can be captured anew. Without it the controller only stops managing the device, and it keeps working with the profile it has already received.')),
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
			' ',
			E('button', {
				'class': 'btn cbi-button-remove',
				'click': function() {
					var wipe = reset.checked;
					var body = E('div', {}, E('p', { 'class': 'spinning' }, _('Removing %s…').format(name || id)));

					ui.hideModal();
					ui.showModal(_('Remove the node'), [ body ]);

					if (!wipe) {
						callMeshMemberRemove(id).then(function() { location.reload(); });
						return;
					}

					callMeshMemberRemove(id, '1').then(function(res) {
						res = res || {};

						if (res.error && !res.task_id) {
							dom.content(body, [
								E('p', { 'class': 'alert-message warning' }, acquireErrText(res.error)),
								E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': function() { ui.hideModal(); location.reload(); } }, _('Close')))
							]);
							return;
						}

						pollAcquire(res.task_id, body, 0, 'release');
					});
				}
			}, _('Remove'))
		])
	]);
}

function updateMember(id, name) {
	var body = E('div', {}, [ E('p', { 'class': 'spinning' }, _('Updating %s…').format(name || id)) ]);
	ui.showModal(_('Updating the node'), [ body ]);

	callMeshUpdate(id).then(function(res) {
		if (res && res.error) {
			dom.content(body, [
				E('p', { 'class': 'alert-message warning' }, acquireErrText(res.error)),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')))
			]);
			return;
		}
		pollAcquire(res.task_id, body, 0, 'update');
	});
}

function renderLog() {
	var filters = { mac: '', from: '', to: '', type: '' };
	var all = [];

	var table = E('table', { 'class': 'table cbi-section-table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Time')),
			E('th', { 'class': 'th' }, _('Client')),
			E('th', { 'class': 'th' }, _('From')),
			E('th', { 'class': 'th' }, _('To')),
			E('th', { 'class': 'th' }, _('Type'))
		])
	]);

	var counter = E('span', { 'class': 'cbi-value-description' }, '');

	function refresh() {
		var shown = all.filter(function(ev) {
			var mac = (ev.mac || '').toLowerCase();
			var name = (logNames[mac] || '').toLowerCase();
			var needle = filters.mac.toLowerCase();

			if (filters.mac && mac.indexOf(needle) < 0 && name.indexOf(needle) < 0) return false;
			if (filters.from && (ev.from_node || '').toLowerCase().indexOf(filters.from.toLowerCase()) < 0) return false;
			if (filters.to && (ev.to_node || '').toLowerCase().indexOf(filters.to.toLowerCase()) < 0) return false;
			if (filters.type && ev.type !== filters.type) return false;
			return true;
		});

		counter.textContent = _('Showing %d of %d records').format(shown.length, all.length);
		cbi_update_table(table, shown.map(logRow), E('em', {}, _('The transition log is empty')));
	}

	function filterInput(key, ph) {
		return E('input', {
			'type': 'text', 'placeholder': ph, 'style': 'width:9em',
			'keyup': function(ev) { filters[key] = ev.target.value; refresh(); }
		});
	}

	var typeSelect = E('select', {
		'change': function(ev) { filters.type = ev.target.value; refresh(); }
	}, [
		E('option', { 'value': '' }, _('all types')),
		E('option', { 'value': 'roam' }, _('roaming')),
		E('option', { 'value': 'steer' }, _('steering')),
		E('option', { 'value': 'connect' }, _('connected')),
		E('option', { 'value': 'disconnect' }, _('disconnected')),
		E('option', { 'value': 'kick' }, _('forced disconnect'))
	]);

	var bar = E('div', { 'style': 'display:flex;gap:.5em;align-items:center;margin-bottom:.5em;flex-wrap:wrap' }, [
		filterInput('mac', _('Client')),
		filterInput('from', _('From')),
		filterInput('to', _('To')),
		typeSelect,
		counter,
		E('button', {
			'class': 'btn cbi-button', 'style': 'margin-left:auto',
			'click': function() { downloadCsv(all); }
		}, _('Download CSV'))
	]);

	var container = E('div', {}, [
		E('p', {}, _('Client transitions between the nodes of the Wi-Fi system are shown here.')),
		bar, table
	]);

	function load() {
		return callMeshLog().then(function(events) {
			all = events || [];
			refresh();
		}, function() {});
	}

	Promise.all([
		common.hostHints(),
		uci.load('roamd').catch(function() { return null; })
	]).then(function(res) {
		var ov = common.deviceOverrides();

		logNames = {};
		for (var mac in (res[0] || {}))
			logNames[mac.toLowerCase()] = common.hostName(res[0], mac);
		for (var m in ov)
			if (ov[m].alias)
				logNames[m] = ov[m].alias;

		return load();
	}).then(function() {
		poll.add(load, 5);
	});

	return container;
}

function humanBytes(n) {
	var units = [ _('B'), _('KB'), _('MB'), _('GB'), _('TB') ];
	var i = 0;

	n = Number(n) || 0;
	while (n >= 1024 && i < units.length - 1) {
		n /= 1024;
		i++;
	}

	return '%s %s'.format(i ? n.toFixed(2) : String(n), units[i]);
}

function hintIp4(hints, mac) {
	var h = hints[mac.toUpperCase()] || hints[mac.toLowerCase()] || {};

	if (h.ipaddrs && h.ipaddrs.length)
		return h.ipaddrs[0];

	return h.ipv4 || '';
}

function hintIp6(hints, mac) {
	var h = hints[mac.toUpperCase()] || hints[mac.toLowerCase()] || {};

	if (h.ip6addrs && h.ip6addrs.length)
		return h.ip6addrs;

	return h.ipv6 ? [ h.ipv6 ] : [];
}

function bandLockLabel(band) {
	if (band === '2')
		return _('only 2.4 GHz');
	if (band === '5')
		return _('only 5 GHz');

	return _('both bands');
}

var readOnly = false;

function pencil(onclick) {
	if (readOnly)
		return E('span', {
			'style': 'margin-left:.4em;color:#ccc',
			'title': _('Managed by the controller')
		}, '✎');

	return E('span', {
		'style': 'cursor:pointer;margin-left:.4em;color:#888',
		'title': _('Edit'),
		'click': onclick
	}, '✎');
}

function nameDialog(title, current, onOk) {
	var input = E('input', {
		'type': 'text', 'class': 'cbi-input-text', 'style': 'width:100%',
		'value': current || '',
		'keydown': function(ev) { if (ev.key === 'Enter') submit(); }
	});

	function submit() {
		ui.hideModal();
		onOk(input.value);
	}

	ui.showModal('', [
		E('h4', { 'style': 'text-align:center;margin-top:0' }, title),
		E('div', { 'class': 'cbi-value' }, input),
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
			' ',
			E('button', { 'class': 'btn cbi-button-positive', 'click': submit }, _('OK'))
		])
	]);

	input.focus();
}

function infoRow(label, value) {
	return E('div', { 'class': 'cbi-value' }, [
		E('label', { 'class': 'cbi-value-title' }, label),
		E('div', { 'class': 'cbi-value-field' }, value)
	]);
}

function bandChoice(current) {
	var opts = [
		{ value: 'both', label: _('both bands') },
		{ value: '2', label: _('only 2.4 GHz') },
		{ value: '5', label: _('only 5 GHz') }
	];

	return opts.map(function(o) {
		var input = E('input', {
			'type': 'radio', 'name': 'roamd-band', 'value': o.value,
			'checked': (current || 'both') === o.value ? 'checked' : null,
			'disabled': readOnly ? 'disabled' : null
		});

		return E('label', { 'style': 'margin-right:1.5em' }, [ input, ' ', o.label ]);
	});
}

function nodeChips(allNodes, allowed) {
	var state = {};

	allNodes.forEach(function(n) {
		state[n.id] = !allowed.length || allowed.indexOf(n.id) >= 0;
	});

	var chips = allNodes.map(function(n) {
		var chip = E('button', {
			'class': 'btn',
			'style': 'margin:0 .4em .4em 0',
			'click': function(ev) {
				ev.preventDefault();
				if (readOnly)
					return;
				state[n.id] = !state[n.id];
				paint(chip, n.id);
			}
		});

		paint(chip, n.id);
		return chip;
	});

	function paint(chip, id) {
		var on = state[id];
		var label = allNodes.filter(function(n) { return n.id === id; })[0].label;

		chip.style.background = on ? '' : '#777';
		chip.style.color = on ? '' : '#fff';
		dom.content(chip, on ? label : [ label, ' ✕' ]);
	}

	return {
		widget: E('div', {}, chips),
		value: function() {
			var on = allNodes.filter(function(n) { return state[n.id]; })
				.map(function(n) { return n.id; });

			return on.length === allNodes.length ? '' : on.join(',');
		},
		reset: function() {
			allNodes.forEach(function(n) { state[n.id] = true; });
			chips.forEach(function(chip, i) { paint(chip, allNodes[i].id); });
		}
	};
}

function clientDialog(c, hints, ov, nodeName, allNodes, refresh) {
	var mac = c.mac.toLowerCase();
	var saved = ov[mac] || { band: 'both', alias: '', nodes: [] };
	var sysName = common.hostName(hints, c.mac) || c.host || '';
	var title = saved.alias || sysName || c.mac;
	var nameInput = E('input', {
		'type': 'text', 'class': 'cbi-input-text', 'style': 'width:100%',
		'value': saved.alias || '',
		'placeholder': sysName,
		'disabled': readOnly ? 'disabled' : null
	});
	var bands = bandChoice(saved.band);
	var chips = nodeChips(allNodes, saved.nodes);
	var ip4 = hintIp4(hints, c.mac);
	var conn = !c.online ? _('offline')
		: c.wired ? _('Cable') : _('Wi-Fi %s GHz').format(c.band || '?');
	var info = [
		infoRow(_('Device name'), sysName || '—'),
		infoRow(_('Connection'), conn),
		infoRow(_('MAC address'), c.mac)
	];

	if (ip4)
		info.push(infoRow(_('IP address'), ip4));
	if (c.rate)
		info.push(infoRow(_('Rate'), '%d %s'.format(Math.round(c.rate / 1000), _('Mbit/s'))));
	if (c.rx_bytes || c.tx_bytes) {
		info.push(infoRow(_('Received by client'), humanBytes(c.rx_bytes)));
		info.push(infoRow(_('Sent by client'), humanBytes(c.tx_bytes)));
	}

	function save() {
		ui.hideModal();
		callMeshClientUpdate(c.mac, bands.filter(function(l) { return l.firstChild.checked; })
			.map(function(l) { return l.firstChild.value; })[0] || 'both',
			nameInput.value, chips.value()).then(refresh);
	}

	function forget() {
		ui.hideModal();
		callMeshClientForget(c.mac).then(refresh);
	}

	var buttons = readOnly
		? [ E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close')) ]
		: [
			E('button', { 'class': 'btn cbi-button-remove', 'style': 'float:left',
				'click': forget }, _('Forget device')),
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
			' ',
			E('button', { 'class': 'btn cbi-button-positive', 'click': save }, _('Save'))
		];

	ui.showModal(title, [
		E('h4', {}, _('Basic settings')),
		E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title' }, _('Client name')),
			E('div', { 'class': 'cbi-value-field' }, nameInput)
		]),
		c.wired ? '' : E('h4', {}, _('Wi-Fi bands allowed for this device')),
		c.wired ? '' : E('div', { 'class': 'cbi-value' }, bands),
		c.wired ? '' : E('h4', {}, _('Wi-Fi system nodes allowed for this device')),
		c.wired ? '' : E('div', { 'class': 'cbi-value' }, chips.widget),
		readOnly || c.wired ? '' : E('div', { 'class': 'cbi-value' }, E('button', {
			'class': 'btn',
			'click': function(ev) {
				ev.preventDefault();
				chips.reset();
				bands.forEach(function(l) { l.firstChild.checked = l.firstChild.value === 'both'; });
			}
		}, _('Restore default roaming'))),
		E('h4', {}, _('Details')),
		E('div', {}, info),
		E('div', { 'class': 'right' }, buttons)
	]);
}

function controllerLabel(state) {
	return (state && state.controller_name) || _('controller');
}

function viaNodeLabel(c, nodeName) {
	return nodeName[c.node || 'controller'] || c.node || _('controller');
}

function clientNameCell(c, hints, ov, nodeName) {
	var mac = c.mac.toLowerCase();
	var alias = common.clientLabel(hints, ov, c.mac) || c.host || c.mac;
	var dot = E('span', {
		'style': 'display:inline-block;width:.6em;height:.6em;border-radius:50%%;margin-right:.5em;background:%s'
			.format(c.online ? '#2e9e2e' : '#bbb')
	});
	var lines = [ E('div', {}, [ dot, E('strong', {}, alias) ]) ];

	if (c.online)
		lines.push(E('div', {}, E('small', { 'style': 'color:#888' },
			_('via %s').format(viaNodeLabel(c, nodeName)))));

	return E('div', {}, lines);
}

function addrCell(c, hints) {
	var lines = [];

	if (c.online) {
		var ip4 = hintIp4(hints, c.mac);

		if (ip4)
			lines.push(E('div', {}, ip4));
		hintIp6(hints, c.mac).forEach(function(a) {
			lines.push(E('div', {}, E('small', { 'style': 'color:#2196a5' }, a)));
		});
	}

	lines.push(E('div', {}, E('small', { 'style': 'color:#888' }, c.mac)));

	return E('div', {}, lines);
}

function connCell(c, ov) {
	var mac = c.mac.toLowerCase();
	var lock = (ov[mac] && ov[mac].band) || 'both';

	if (c.wired)
		return E('div', {}, [
			E('span', {}, _('Cable')),
			c.online ? '' : E('div', {}, E('small', { 'style': 'color:#888' }, _('offline')))
		]);

	if (!c.online)
		return E('div', {}, [
			E('span', {}, bandLockLabel(lock)),
			E('div', {}, E('small', { 'style': 'color:#888' }, _('offline')))
		]);

	return E('div', {}, [
		E('span', {}, bandLockLabel(lock)),
		E('div', {}, E('small', { 'style': 'color:#888' },
			_('Wi-Fi %s GHz').format(c.band || '?')))
	]);
}

function paramCell(c) {
	if (!c.online || !c.rate)
		return '—';

	var phy = c.std || '';
	if (c.nss)
		phy += ' %dx%d'.format(c.nss, c.nss);
	if (c.width)
		phy += ' %d %s'.format(c.width, _('MHz'));

	return E('div', {}, [
		E('div', {}, [ E('strong', {}, '%d %s'.format(Math.round(c.rate / 1000), _('Mbit/s'))),
			c.encryption ? ' ' + c.encryption : '' ]),
		phy ? E('div', {}, E('small', { 'style': 'color:#888' }, phy)) : ''
	]);
}

function trafficCell(c) {
	if (!c.online || (!c.rx_bytes && !c.tx_bytes))
		return '—';

	return E('div', {}, [
		E('div', {}, '↓ %s'.format(humanBytes(c.rx_bytes))),
		E('div', {}, E('small', {}, '↑ %s'.format(humanBytes(c.tx_bytes))))
	]);
}

function clientRow(c, hints, ov, nodeName, allNodes, refresh) {
	var action = E('button', {
		'class': 'btn cbi-button',
		'click': function() { clientDialog(c, hints, ov, nodeName, allNodes, refresh); }
	}, readOnly ? _('Details') : _('Settings'));

	return [
		clientNameCell(c, hints, ov, nodeName),
		addrCell(c, hints),
		connCell(c, ov),
		paramCell(c),
		trafficCell(c),
		action
	];
}

function serviceMacs(members) {
	var set = {};

	(members || []).forEach(function(m) {
		if (!m.mac)
			return;

		var mac = m.mac.toLowerCase();
		var head = parseInt(mac.slice(0, 2), 16);

		set[mac] = true;
		set['%02x%s'.format(head ^ 0x02, mac.slice(2))] = true;
	});

	return set;
}

function clientTable(caption) {
	var table = E('table', { 'class': 'table cbi-section-table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Client')),
			E('th', { 'class': 'th' }, _('Address')),
			E('th', { 'class': 'th' }, _('Connection')),
			E('th', { 'class': 'th' }, _('Parameters')),
			E('th', { 'class': 'th' }, _('Traffic')),
			E('th', { 'class': 'th' }, '')
		])
	]);

	return { title: E('h4', {}, caption), table: table };
}

function renderClients(state) {
	var members = state.members || [];
	var nodeName = {};

	members.forEach(function(m) { nodeName[m.id] = m.name || m.hostname || m.id; });
	nodeName['controller'] = controllerLabel(state);
	nodeNames = Object.assign({}, nodeName);

	var allNodes = [ { id: 'controller', label: controllerLabel(state) } ].concat(
		members.map(function(m) { return { id: m.id, label: nodeName[m.id] }; }));

	var online = clientTable(_('Online'));
	var offline = clientTable(_('Offline'));

	function refresh() {
		uci.unload('roamd');

		return Promise.all([
			callMeshClients().catch(function() { return {}; }),
			common.hostHints(),
			uci.load('roamd').catch(function() { return null; })
		]).then(function(res) {
			var clients = (res[0] && res[0].clients) || res[0] || [];
			var hints = res[1] || {};
			var ov = common.deviceOverrides();
			var service = serviceMacs(members);
			var up = [];
			var down = [];

			clients.forEach(function(c) {
				var mac = c.mac.toLowerCase();

				if (service[mac])
					return;

				(c.online ? up : down).push(clientRow(c, hints, ov, nodeName, allNodes, refresh));
			});

			cbi_update_table(online.table, up, E('em', {}, _('No clients online')));
			cbi_update_table(offline.table, down, E('em', {}, _('No offline clients')));
			offline.title.style.display = down.length ? '' : 'none';
			offline.table.style.display = down.length ? '' : 'none';
		});
	}

	refresh();
	poll.add(refresh, 5);

	return E('div', {}, [
		E('p', {}, _('All devices of the Wi-Fi system: the node they are connected through, addresses, connection type and parameters, traffic. The settings button opens the name, allowed bands and allowed nodes of a device.')),
		online.title, online.table,
		E('br'),
		offline.title, offline.table
	]);
}

var AUTO_UNITS = [
	{ unit: 'hour', label: _('hours'), max: 23 },
	{ unit: 'day', label: _('days'), max: 31 },
	{ unit: 'week', label: _('weeks'), max: 5 },
	{ unit: 'month', label: _('months'), max: 12 }
];

var AUTO_RESULT = {
	updated: _('updated'),
	none: _('no updates'),
	error: _('the check failed')
};

function lastCheckText(state) {
	var pad = function(n) { return (n < 10 ? '0' : '') + n; };
	var d, when, result;

	if (!state.auto_update_last)
		return _('not performed yet');

	d = new Date(state.auto_update_last * 1000);
	when = '%s-%s-%s %s:%s'.format(pad(d.getDate()), pad(d.getMonth() + 1),
		d.getFullYear(), pad(d.getHours()), pad(d.getMinutes()));
	result = AUTO_RESULT[state.auto_update_result];

	return result ? '%s — %s'.format(when, result) : when;
}

var BACKHAUL_FIELDS = [ 'backhaul_ssid', 'backhaul_key' ];
var settingsSync;

function renderSettings(state) {
	var form = {
		backhaul_enabled: !!state.backhaul_enabled,
		backhaul_ssid: state.backhaul_ssid || '',
		backhaul_key: state.backhaul_key || '',
		wifi_shutdown: !!state.wifi_shutdown,
		backhaul_delta: state.backhaul_delta,
		backhaul_min_signal: state.backhaul_min_signal,
		auto_update: !!state.auto_update,
		auto_update_every: state.auto_update_every || 1,
		auto_update_unit: state.auto_update_unit || 'day',
		pkg_url: state.pkg_url === state.pkg_url_default ? '' : (state.pkg_url || '')
	};
	var inputs = {};
	var synced = {};

	BACKHAUL_FIELDS.forEach(function(key) { synced[key] = form[key]; });

	function toggle(key, label, desc, onchange) {
		var input = E('input', {
			'type': 'checkbox', 'checked': form[key] ? 'checked' : null,
			'disabled': readOnly || null,
			'change': function(ev) {
				form[key] = ev.target.checked;
				if (onchange)
					onchange();
			}
		});

		return E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title', 'style': 'min-width:22em' }, label),
			E('div', { 'class': 'cbi-value-field' }, [
				input, desc ? E('div', { 'class': 'cbi-value-description' }, desc) : ''
			])
		]);
	}

	function text(key, label, desc, secret) {
		var input = E('input', {
			'type': secret ? 'password' : 'text', 'class': 'cbi-input-text',
			'value': form[key], 'disabled': readOnly || null,
			'change': function(ev) { form[key] = ev.target.value; }
		});

		inputs[key] = input;

		if (secret) {
			var reveal = E('input', {
				'type': 'checkbox',
				'change': function(ev) { input.type = ev.target.checked ? 'text' : 'password'; }
			});

			return E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title', 'style': 'min-width:22em' }, label),
				E('div', { 'class': 'cbi-value-field' }, [
					input, ' ', reveal, ' ', E('label', {}, _('show')),
					desc ? E('div', { 'class': 'cbi-value-description' }, desc) : ''
				])
			]);
		}

		return E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title', 'style': 'min-width:22em' }, label),
			E('div', { 'class': 'cbi-value-field' }, [
				input, desc ? E('div', { 'class': 'cbi-value-description' }, desc) : ''
			])
		]);
	}

	function number(key, label, desc, min, max) {
		return E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title', 'style': 'min-width:22em' }, label),
			E('div', { 'class': 'cbi-value-field' }, [
				E('input', {
					'type': 'number', 'class': 'cbi-input-text', 'style': 'width:6em',
					'min': min, 'max': max, 'value': form[key], 'disabled': readOnly || null,
					'change': function(ev) { form[key] = ev.target.value; }
				}),
				E('div', { 'class': 'cbi-value-description' }, desc)
			])
		]);
	}

	var status = E('span', { 'style': 'margin-left:1em' }, '');

	function backhaulBands(s) {
		var ssid = s.backhaul_ssid || '';
		var bands = (s.self_aps || []).filter(function(ap) { return ap.ssid === ssid; })
			.map(function(ap) { return '%s %s'.format(ap.band, _('GHz')); });

		return bands.length ? bands.join(', ') : _('not broadcasting');
	}

	function unitMax(unit) {
		for (var i = 0; i < AUTO_UNITS.length; i++)
			if (AUTO_UNITS[i].unit === unit)
				return AUTO_UNITS[i].max;

		return 1;
	}

	function clampEvery(value) {
		var max = unitMax(form.auto_update_unit);
		var n = parseInt(value, 10);

		if (isNaN(n) || n < 1)
			n = 1;

		return n > max ? max : n;
	}

	var everyInput = E('input', {
		'type': 'number', 'class': 'cbi-input-text', 'style': 'width:5em',
		'min': 1, 'max': unitMax(form.auto_update_unit),
		'value': form.auto_update_every, 'disabled': readOnly || null,
		'change': function(ev) {
			form.auto_update_every = clampEvery(ev.target.value);
			ev.target.value = form.auto_update_every;
		}
	});

	var unitSelect = E('select', {
		'class': 'cbi-input-select', 'disabled': readOnly || null,
		'change': function(ev) {
			form.auto_update_unit = ev.target.value;
			everyInput.max = unitMax(form.auto_update_unit);
			form.auto_update_every = clampEvery(everyInput.value);
			everyInput.value = form.auto_update_every;
		}
	}, AUTO_UNITS.map(function(u) {
		return E('option', {
			'value': u.unit,
			'selected': u.unit === form.auto_update_unit ? 'selected' : null
		}, u.label);
	}));

	var intervalRow = E('div', { 'class': 'cbi-value' }, [
		E('label', { 'class': 'cbi-value-title', 'style': 'min-width:22em' }, _('Check for updates every')),
		E('div', { 'class': 'cbi-value-field' }, [
			everyInput, ' ', unitSelect,
			E('div', { 'class': 'cbi-value-description' },
				_('The controller checks the repository on this schedule and installs the update itself — first on the controller, then on the nodes.'))
		])
	]);

	var lastField = E('div', { 'class': 'cbi-value-field' }, lastCheckText(state));
	var lastRow = E('div', { 'class': 'cbi-value' }, [
		E('label', { 'class': 'cbi-value-title', 'style': 'min-width:22em' }, _('Last check')),
		lastField
	]);

	function autoRows() {
		var display = form.auto_update ? '' : 'none';

		intervalRow.style.display = display;
		lastRow.style.display = display;
	}

	var save = E('button', {
		'class': 'btn cbi-button cbi-button-save',
		'disabled': readOnly || null,
		'click': function() {
			status.textContent = _('Saving…');
			callMeshSettings(
				form.backhaul_enabled ? '1' : '0',
				form.backhaul_ssid, form.backhaul_key,
				form.wifi_shutdown ? '1' : '0',
				String(form.backhaul_delta), String(form.backhaul_min_signal),
				form.auto_update ? '1' : '0',
				String(form.auto_update_every), form.auto_update_unit,
				form.pkg_url
			).then(function() {
				BACKHAUL_FIELDS.forEach(function(key) { synced[key] = form[key]; });
				status.textContent = _('Saved.');
			}, function() { status.textContent = _('Save failed.'); });
		}
	}, _('Save'));

	var bandsField = E('div', { 'class': 'cbi-value-field' }, backhaulBands(state));

	settingsSync = function(fresh) {
		dom.content(bandsField, backhaulBands(fresh));
		dom.content(lastField, lastCheckText(fresh));

		BACKHAUL_FIELDS.forEach(function(key) {
			if (inputs[key].value !== synced[key])
				return;

			inputs[key].value = form[key] = synced[key] = fresh[key] || '';
		});
	};

	autoRows();

	return E('div', {}, [
		E('h3', {}, _('Wireless backhaul')),
		E('p', { 'class': 'cbi-value-description' }, _('A hidden Wi-Fi network the nodes use to talk to each other. Leave the name and key empty to generate them automatically.')),
		toggle('backhaul_enabled', _('Wireless backhaul')),
		E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title', 'style': 'min-width:22em' }, _('Broadcast on bands')),
			bandsField
		]),
		number('backhaul_delta', _('5 GHz advantage for the node link'),
			_('dB. A node bringing up its wireless link picks 5 GHz if its signal is weaker than 2.4 GHz by no more than this value.'), 0, 40),
		number('backhaul_min_signal', _('Minimum 5 GHz signal for the node link'),
			_('dBm. A weaker 5 GHz signal is not used for the node link while 2.4 GHz is available.'), -95, -40),
		text('backhaul_ssid', _('Backhaul network name')),
		text('backhaul_key', _('Backhaul key'), null, true),

		E('h3', {}, _('Wi-Fi operation mode')),
		toggle('wifi_shutdown', _('Turn off node access points when the controller is unreachable'),
			_('Nodes stop broadcasting the Wi-Fi network while the controller is not available.')),

		E('h3', {}, _('Automatic update')),
		toggle('auto_update', _('Automatic update'),
			_('roamd is updated automatically on the controller and on all nodes when a newer package is available.'),
			autoRows),
		intervalRow,
		lastRow,
		text('pkg_url', _('Package repository'),
			_('Leave empty to use the project repository. Fill it in only for your own repository: only https:// is accepted, and its certificate must be placed in /etc/roamd/pkg.crt — the delivery installs software on every node.')),

		E('div', { 'class': 'cbi-page-actions' }, [ save, status ])
	]);
}

function renderController(state) {
	var ctrl = state.controller || {};
	var counts = state.counts || {};
	var age = ctrl.last_contact || 0;
	var link = age > 60
		? E('span', { 'style': 'color:#c60' }, _('no contact for %t').format(age))
		: _('in touch');

	var table = E('table', { 'class': 'table cbi-section-table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Controller')),
			E('th', { 'class': 'th' }, _('IP address')),
			E('th', { 'class': 'th' }, _('Link')),
			E('th', { 'class': 'th' }, _('This node')),
			E('th', { 'class': 'th' }, _('Clients'))
		])
	]);

	cbi_update_table(table, [[
		ctrl.name || ctrl.id || _('unknown'), ctrl.addr || '-', link,
		state.member_id || '-', counts.clients || 0
	]]);

	return E('div', {}, [
		E('p', {}, _('This node is part of the Wi-Fi system managed by the controller below. The node knows about the controller and about its own clients; the rest of the system is visible on the controller.')),
		table
	]);
}

return view.extend({
	load: function() {
		return callMeshStatus().catch(function() { return {}; });
	},

	render: function(state) {
		state = state || {};

		var container = E('div', {}, [ E('h2', {}, _('Mesh Wi-Fi system')) ]);

		readOnly = common.isNode(state);

		if (readOnly)
			container.appendChild(common.controlBanner(state));

		container.appendChild(buildTabs(state));

		if (!common.isNode(state))
			poll.add(refreshNodes, 5);

		return container;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});

function buildTabs(state) {
	var isNode = common.isNode(state);

	nodeNames = { controller: controllerLabel(state) };
	(state.members || []).forEach(function(m) {
		nodeNames[m.id] = m.name || m.hostname || m.id;
	});

	var panes = [
		isNode
			? { title: _('Controller'), node: renderController(state) }
			: { title: _('Nodes'), node: E('div', { 'id': 'mesh-nodes-pane' }, renderNodes(state)) },
		{ title: _('Clients'), node: renderClients(state) },
		{ title: _('Transition log'), node: renderLog() },
		{ title: _('Settings'), node: renderSettings(state) }
	];

	var menu = E('ul', { 'class': 'cbi-tabmenu' });
	var content = E('div', { 'class': 'cbi-tabcontainer' });

	function activate(idx) {
		panes.forEach(function(p, i) {
			p.node.style.display = i === idx ? '' : 'none';
			menu.childNodes[i].className = i === idx ? 'cbi-tab' : 'cbi-tab-disabled';
		});
	}

	panes.forEach(function(p, i) {
		menu.appendChild(E('li', {}, E('a', {
			'href': '#',
			'click': function(ev) { ev.preventDefault(); activate(i); }
		}, p.title)));
		content.appendChild(p.node);
	});

	var wrap = E('div', {}, [ menu, content ]);
	activate(0);

	return wrap;
}
