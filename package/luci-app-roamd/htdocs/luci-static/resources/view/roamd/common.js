'use strict';
'require baseclass';
'require rpc';

var callMeshStatus = rpc.declare({
	object: 'roamd',
	method: 'mesh_status',
	expect: { }
});

return baseclass.extend({
	meshStatus: function() {
		return callMeshStatus().catch(function() { return {}; });
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
