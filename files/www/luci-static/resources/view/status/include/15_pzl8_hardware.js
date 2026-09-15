'use strict';
'require baseclass';
'require rpc';

/* The stock status page shows none of this. The data has always been there -
 * cpufreq and the thermal zones are standard sysfs, ath11k registers a hwmon
 * per radio, and the NSS core reports its own utilisation through debugfs.
 * What was missing was somewhere to put it.
 *
 * One ubus call per poll rather than a file read per field; the plugin lives at
 * /usr/libexec/rpcd/luci.pzl8 and does nothing but read and reformat.
 */
var callHardware = rpc.declare({
	object: 'luci.pzl8',
	method: 'status'
});

/* The hwmon index follows the phy order, and phy numbering is not stable - a
 * `wifi reload` swaps phy0 and phy1 on this board. The device-tree node does
 * not move, so the label is keyed on that, falling back to the phy name on any
 * board this map does not know.
 */
var RADIO_NODE = {
	'c000000.wifi': '2.4 GHz',
	'b00a040.wifi': '5 GHz'
};

function celsius(mdeg) {
	var v = parseInt(mdeg, 10);
	return isNaN(v) ? '-' : (v / 1000).toFixed(1) + ' °C';
}

function parseStat(line) {
	if (typeof line != 'string')
		return null;

	var f = line.replace(/^cpu\s+/, '').trim().split(/\s+/).map(function(n) {
		return parseInt(n, 10);
	});

	for (var i = 0; i < f.length; i++)
		if (isNaN(f[i]))
			return null;

	return f.length >= 7 ? f : null;
}

/* user nice system idle iowait irq softirq - busy over the whole window, which
 * is what the idle field's complement means on a multi-core box.
 */
function busyPercent(prev, cur) {
	if (!prev || !cur)
		return null;

	var d = [];
	for (var i = 0; i < 7; i++)
		d.push((cur[i] || 0) - (prev[i] || 0));

	var busy = d[0] + d[1] + d[2] + d[5] + d[6],
	    total = busy + d[3] + d[4];

	if (total <= 0)
		return null;

	return Math.round(busy * 100 / total);
}

return baseclass.extend({
	title: _('Hardware'),

	load: function() {
		return L.resolveDefault(callHardware(), {});
	},

	render: function(data) {
		if (!data || !data.cpu)
			return null;

		var cur = parseStat(data.stat),
		    load = busyPercent(this.prevStat, cur);

		this.prevStat = cur;

		var cpu = data.cpu,
		    desc = cpu.model || '?';

		if (cpu.cores)
			desc += ' × ' + cpu.cores;

		var qual = [];
		if (cpu.freq)
			qual.push(Math.round(cpu.freq / 1000) + ' MHz');
		if (cpu.governor)
			qual.push(cpu.governor);
		if (qual.length)
			desc += ' (' + qual.join(', ') + ')';

		var rows = [ _('Processor'), desc ];

		/* Needs two samples, so the first poll after loading the page has
		 * nothing to divide yet. */
		rows.push(_('CPU load'),
			load != null ? load + ' %' : _('Collecting data...'));

		var temp = data.temp || {};

		if (temp.cpu != null)
			rows.push(_('CPU temperature'), celsius(temp.cpu));

		(temp.wifi || []).forEach(function(radio) {
			var name = RADIO_NODE[radio.node] || radio.phy || '?';
			rows.push(_('Wi-Fi temperature') + ' (' + name + ')',
				celsius(radio.temp));
		});

		if (data.nss)
			rows.push(_('NSS/PPE utilisation'),
				data.nss.avg + ' % (' + _('peak') + ' ' + data.nss.max + ' %)');

		var table = E('table', { 'class': 'table' });

		for (var i = 0; i < rows.length; i += 2)
			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, [ rows[i] ]),
				E('td', { 'class': 'td left' }, [ rows[i + 1] ])
			]));

		return table;
	}
});
