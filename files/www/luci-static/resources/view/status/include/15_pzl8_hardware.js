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

/* Bytes per interval to bits per second. A negative delta means the counter
 * was reset - an interface going down - so the rate is unknown, not negative.
 */
function rate(deltaBytes, deltaMs) {
	if (deltaBytes == null || deltaBytes < 0 || !(deltaMs > 0))
		return null;

	return deltaBytes * 8000 / deltaMs;
}

function bits(v) {
	if (v == null)
		return '-';
	if (v >= 1e9)
		return (v / 1e9).toFixed(2) + ' Gbit/s';
	if (v >= 1e6)
		return (v / 1e6).toFixed(1) + ' Mbit/s';
	if (v >= 1e3)
		return (v / 1e3).toFixed(1) + ' kbit/s';

	return Math.round(v) + ' bit/s';
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

		/* Both numbers in one cell rather than a sentence. A bare "peak" as a
		 * msgid would translate that word everywhere else in LuCI too, which
		 * is not this file's business.
		 *
		 * NSS, not NSS/PPE: the packet processing engine is an IPQ807x /
		 * IPQ60xx / IPQ95xx block. IPQ5018 does not have one. */
		if (data.nss)
			rows.push(_('NSS utilisation (average / peak)'),
				data.nss.avg + ' % / ' + data.nss.max + ' %');

		if (data.ecm != null)
			rows.push(_('Accelerated connections'), String(data.ecm));

		/* Throughput needs two samples and the wall time between them. The
		 * poll interval is not fixed, so it is measured rather than assumed. */
		var now = Date.now(),
		    prevNet = this.prevNet,
		    nowNet = {};

		(data.net || []).forEach(function(iface) {
			nowNet[iface.dev] = iface;
		});

		this.prevNet = { t: now, dev: nowNet };

		(data.net || []).forEach(function(iface) {
			var was = prevNet ? prevNet.dev[iface.dev] : null,
			    ms = prevNet ? now - prevNet.t : 0,
			    rx = was ? rate(iface.rx - was.rx, ms) : null,
			    tx = was ? rate(iface.tx - was.tx, ms) : null;

			/* Labelled by whatever netifd calls the port, with the device name as
			 * the fallback. Nothing here assumes a box has a wan, or a lan, or
			 * exactly two ports. */
			var tag = iface.iface ? iface.iface + ' / ' + iface.dev : iface.dev;

			rows.push(_('Port throughput') + ' (' + tag + ')',
				(rx == null && tx == null)
					? _('Collecting data...')
					: '↓ ' + bits(rx) + ' ↑ ' + bits(tx));
		});

		var table = E('table', { 'class': 'table' });

		for (var i = 0; i < rows.length; i += 2)
			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, [ rows[i] ]),
				E('td', { 'class': 'td left' }, [ rows[i + 1] ])
			]));

		return table;
	}
});
