'use strict';
'require baseclass';
'require rpc';
'require uci';

var callMeshStatus = rpc.declare({
	object: 'roamd',
	method: 'mesh_status',
	expect: { }
});

var callHostHints = rpc.declare({
	object: 'luci-rpc',
	method: 'getHostHints',
	expect: { }
});

var localDomain = 'lan';

function stripDomain(name) {
	var tail = '.' + localDomain;

	if (name.length > tail.length && name.slice(-tail.length).toLowerCase() === tail.toLowerCase())
		return name.slice(0, -tail.length);

	return name;
}

return baseclass.extend({
	meshStatus: function() {
		return callMeshStatus().catch(function() { return {}; });
	},

	hostHints: function() {
		return Promise.all([
			callHostHints().catch(function() { return {}; }),
			uci.load('dhcp').catch(function() { return null; })
		]).then(function(res) {
			var domain = uci.get_first('dhcp', 'dnsmasq', 'domain');

			if (domain)
				localDomain = domain;

			return res[0] || {};
		});
	},

	hostName: function(hints, mac) {
		var h = hints[mac.toUpperCase()] || hints[mac.toLowerCase()];

		return (h && h.name) ? stripDomain(h.name) : '';
	},

	deviceOverrides: function() {
		var map = {};

		(uci.sections('roamd', 'device') || []).forEach(function(s) {
			if (!s.mac)
				return;

			map[s.mac.toLowerCase()] = {
				band: s.band || 'both',
				alias: s.alias || '',
				nodes: s.node ? (Array.isArray(s.node) ? s.node : [ s.node ]) : []
			};
		});

		return map;
	},

	clientLabel: function(hints, overrides, mac) {
		var key = mac.toLowerCase();

		return (overrides[key] && overrides[key].alias) || this.hostName(hints, mac) || '';
	},

	isNode: function(state) {
		return (state || {}).role === 'node';
	},

	controlBanner: function(state) {
		var ctrl = (state || {}).controller || {};
		var name = ctrl.name || ctrl.id || _('unknown');
		var addr = ctrl.addr ? ' (%s)'.format(ctrl.addr) : '';
		var age = ctrl.last_contact || 0;
		var lines = [
			E('p', {}, _('This device is managed by the Mesh controller %s%s. Settings on this page are read-only: they come from the controller. To take the device out of the Wi-Fi system, reset it to factory settings.').format(name, addr))
		];

		if (age > 60)
			lines.push(E('p', {}, _('The controller has not been in touch for %t — settings stay locked until it returns.').format(age)));

		return E('div', { 'class': 'alert-message warning' }, lines);
	}
});
