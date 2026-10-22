// SPDX-License-Identifier: AGPL-3.0-or-later
// cymon web UI (Alpine.js + uPlot, no build step).
'use strict';

const SEVERITIES = ['trace', 'debug', 'info', 'notice', 'warning', 'error', 'critical', 'alert'];
const COLORS = ['#4e79a7', '#f28e2b', '#e15759', '#76b7b2', '#59a14f', '#edc948', '#b07aa1', '#ff9da7'];
const CLIENT_ID = Math.random().toString(16).slice(2);

async function api(method, path, body) {
  const res = await fetch(path, {
    method,
    headers: { 'Content-Type': 'application/json', 'X-Cymon-Client': CLIENT_ID },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.detail ? (typeof data.detail === 'string' ? data.detail : JSON.stringify(data.detail)) : res.statusText);
  return data;
}

// Plots live outside Alpine's reactivity: uPlot instances must not be proxied.
const plots = {};
const series = {};

function makePlot(el, labels, height) {
  const width = Math.max(300, el.clientWidth || 800);
  const opts = {
    width, height,
    scales: { x: { time: false } },
    series: [{ label: 't [s]' }, ...labels.map((l, i) => ({ label: l, stroke: COLORS[i % COLORS.length], width: 1.5 }))],
    axes: [{ stroke: '#888' }, { stroke: '#888' }],
    legend: { live: true },
  };
  el.innerHTML = '';
  return new uPlot(opts, [[], ...labels.map(() => [])], el);
}

window.cymon = function () {
  return {
    views: [
      { id: 'nodes', label: 'Nodes' }, { id: 'monitor', label: 'Monitor' }, { id: 'publish', label: 'Publish' },
      { id: 'scope', label: 'Scope' }, { id: 'log', label: 'Log' },
    ],
    view: 'nodes',
    connected: false,
    status: null,
    toast: '',
    nodes: {},
    selected: null,
    ports: [],
    registers: [],
    registerFilter: '',
    loadingRegisters: false,
    customCommand: '',
    types: [],
    subForm: { subject_id: null, type: '' },
    subscriptions: [],
    windowSeconds: 10,
    pubForm: { subject_id: null, type: '', rate_hz: 10, lease_s: 1, value: '{}' },
    publications: [],
    scope: {
      node: '', info: null, error: '', channels: [], period: 100, samples: 200, pretrigger: 20,
      mode: 'immediate', source: 0, level: 0, band: 0, repeat: false, busy: false, state: '', result: null,
    },
    diagnostics: [],
    logLevel: 'info',
    logNode: '',

    async init() {
      this.connect();
      setInterval(() => { if (this.connected) this.ws.send(JSON.stringify({ type: 'renew' })); }, 250);
      try {
        this.status = await api('GET', '/api/status');
        for (const n of await api('GET', '/api/nodes')) this.nodes[n.node_id] = n;
        this.diagnostics = await api('GET', '/api/diagnostics');
        this.publications = (await api('GET', '/api/publishers')).map(p => ({ ...p, edit: JSON.stringify(p.value) }));
        this.types = await api('GET', '/api/types');
      } catch (e) { this.notify(e); }
    },

    connect() {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      this.ws = new WebSocket(`${proto}://${location.host}/ws?client=${CLIENT_ID}`);
      this.ws.onopen = () => { this.connected = true; };
      this.ws.onclose = () => { this.connected = false; setTimeout(() => this.connect(), 1000); };
      this.ws.onmessage = (ev) => this.onEvent(JSON.parse(ev.data));
    },

    onEvent(e) {
      switch (e.kind) {
        case 'hello': this.status = e.status; break;
        case 'node': this.nodes[e.node.node_id] = e.node; break;
        case 'heartbeat': if (this.nodes[e.node_id]) this.nodes[e.node_id].uptime = e.uptime; break;
        case 'diagnostic':
          this.diagnostics.push(e);
          if (this.diagnostics.length > 5000) this.diagnostics.splice(0, 1000);
          break;
        case 'subject': this.onSubject(e); break;
        case 'publisher': this.onPublisher(e); break;
        case 'capture': this.scope.state = e.state; break;
      }
    },

    notify(e) { this.toast = String(e.message ?? e); setTimeout(() => { this.toast = ''; }, 6000); },

    // ── Nodes ──────────────────────────────────────────────────────────────
    nodeList() { return Object.values(this.nodes).sort((a, b) => a.node_id - b.node_id); },
    node() { return this.nodes[this.selected]; },
    selectNode(id) { this.selected = id; this.ports = []; this.registers = []; },
    async loadPorts() {
      try { this.ports = await api('GET', `/api/nodes/${this.selected}/ports`); } catch (e) { this.notify(e); }
    },
    async loadRegisters() {
      this.loadingRegisters = true;
      try {
        this.registers = (await api('GET', `/api/nodes/${this.selected}/registers`)).map(r => ({ ...r, edit: this.fmtValue(r.value) }));
      } catch (e) { this.notify(e); }
      this.loadingRegisters = false;
    },
    filteredRegisters() {
      const f = this.registerFilter.toLowerCase();
      return this.registers.filter(r => r.name.toLowerCase().includes(f));
    },
    parseValue(text, kind) {
      if (kind === 'string' || kind === 'unstructured') return text;
      const items = text.split(/[\s,;]+/).filter(Boolean).map(t => (kind === 'bit' ? t === 'true' || t === '1' : Number(t)));
      if (items.some(v => typeof v === 'number' && Number.isNaN(v))) throw new Error(`Not a number: ${text}`);
      return items;
    },
    async writeRegister(r) {
      try {
        const requested = this.parseValue(r.edit, r.kind);
        const res = await api('PUT', `/api/nodes/${this.selected}/registers/${encodeURIComponent(r.name)}`, { value: requested });
        Object.assign(r, res, { edit: this.fmtValue(res.value) });
        if (this.fmtValue(res.value) !== this.fmtValue(requested)) this.notify(`${r.name}: the node kept ${this.fmtValue(res.value)}`);
      } catch (e) { this.notify(e); }
    },
    async command(cmd) {
      try {
        const res = await api('POST', `/api/nodes/${this.selected}/command`, { command: cmd });
        this.notify(`Command ${res.command}: ${res.status}`);
      } catch (e) { this.notify(e); }
    },
    async estop() {
      try {
        const res = await api('POST', '/api/estop');
        const list = Object.entries(res.nodes).map(([n, s]) => `${n}: ${s}`).join(', ');
        this.notify('E-STOP sent. ' + (list || 'No node online.'));
      } catch (e) { this.notify(e); }
    },

    // ── Monitor ────────────────────────────────────────────────────────────
    async monitor(subjectId, type) {
      try {
        const s = await api('POST', '/api/subjects', { subject_id: subjectId, type });
        if (!this.subscriptions.find(x => x.id === s.id)) {
          this.subscriptions.push({ ...s, fieldNames: [], shown: [], lastValues: {}, paused: false });
          series[s.id] = { t: [], v: {} };
        }
        this.view = 'monitor';
      } catch (e) { this.notify(e); }
    },
    async unmonitor(s) {
      await api('DELETE', `/api/subjects/${s.id}`).catch(e => this.notify(e));
      this.subscriptions = this.subscriptions.filter(x => x.id !== s.id);
      if (plots[s.id]) { plots[s.id].destroy(); delete plots[s.id]; }
      delete series[s.id];
    },
    onSubject(e) {
      const s = this.subscriptions.find(x => x.id === e.id);
      if (!s) return;
      s.count += e.t.length;
      const buf = series[s.id];
      for (const [k, vals] of Object.entries(e.fields)) {
        if (!(k in buf.v)) {
          buf.v[k] = new Array(buf.t.length).fill(null);
          s.fieldNames.push(k);
          if (s.shown.length < 4) s.shown.push(k);
        }
        s.lastValues[k] = vals[vals.length - 1];
      }
      for (let i = 0; i < e.t.length; i++) {
        buf.t.push(e.t[i]);
        for (const k of Object.keys(buf.v)) buf.v[k].push(e.fields[k] ? e.fields[k][i] : null);
      }
      const cutoff = buf.t[buf.t.length - 1] - this.windowSeconds;
      let drop = 0;
      while (drop < buf.t.length && buf.t[drop] < cutoff) drop++;
      if (drop) { buf.t.splice(0, drop); for (const k of Object.keys(buf.v)) buf.v[k].splice(0, drop); }
      if (!s.paused) this.redraw(s);
    },
    toggleField(s, f) {
      s.shown = s.shown.includes(f) ? s.shown.filter(x => x !== f) : [...s.shown, f];
      if (plots[s.id]) { plots[s.id].destroy(); delete plots[s.id]; }
      this.redraw(s);
    },
    redraw(s) {
      const el = document.getElementById('plot-' + s.id);
      if (!el || this.view !== 'monitor') return;
      const shown = [...s.shown];
      if (!plots[s.id] || plots[s.id].series.length !== shown.length + 1) {
        if (plots[s.id]) plots[s.id].destroy();
        plots[s.id] = makePlot(el, shown, 220);
      }
      const buf = series[s.id];
      const t0 = buf.t.length ? buf.t[buf.t.length - 1] : 0;
      plots[s.id].setData([buf.t.map(t => t - t0), ...shown.map(k => buf.v[k])]);
    },

    // ── Publish ────────────────────────────────────────────────────────────
    preparePublish(subjectId, type) {
      this.pubForm.subject_id = subjectId;
      this.pubForm.type = type;
      this.loadTemplate();
      this.view = 'publish';
    },
    async loadTemplate() {
      if (!this.pubForm.type) return;
      try {
        const t = await api('GET', `/api/types/${encodeURIComponent(this.pubForm.type)}`);
        this.pubForm.type = t.type;
        this.pubForm.value = JSON.stringify(t.default, null, 2);
      } catch (e) { this.notify(e); }
    },
    async startPublish() {
      try {
        const f = this.pubForm;
        const p = await api('POST', '/api/publishers', {
          subject_id: f.subject_id, type: f.type, value: JSON.parse(f.value), rate_hz: f.rate_hz, lease_s: f.lease_s,
        });
        if (!this.publications.find(x => x.id === p.id)) this.publications.push({ ...p, edit: JSON.stringify(p.value) });
      } catch (e) { this.notify(e); }
    },
    async updatePublish(p) {
      try { Object.assign(p, await api('PUT', `/api/publishers/${p.id}`, JSON.parse(p.edit))); } catch (e) { this.notify(e); }
    },
    async stopPublish(p) { await api('DELETE', `/api/publishers/${p.id}`).catch(e => this.notify(e)); },
    async stopAllPublish() { await api('POST', '/api/publishers/stop').catch(e => this.notify(e)); },
    onPublisher(e) {
      if (e.action === 'stopped') {
        this.publications = this.publications.filter(p => p.id !== e.id);
        if (e.reason !== 'stopped') this.notify(`Publication ${e.id} on ${e.subject_id} stopped: ${e.reason}`);
      } else if (!this.publications.find(p => p.id === e.id)) {
        this.publications.push({ ...e, edit: JSON.stringify(e.value) });
      }
    },

    // ── Scope ──────────────────────────────────────────────────────────────
    async loadScope() {
      this.scope.info = null;
      this.scope.error = '';
      this.scope.channels = [];
      if (this.scope.node === '') return;
      try {
        this.scope.info = await api('GET', `/api/nodes/${this.scope.node}/scope?refresh=true`);
        const tick = this.scope.info.buffer.tick_period_us;
        if (tick > 0) this.scope.period = tick;
      } catch (e) { this.scope.error = e.message; }
    },
    maxSamples() {
      if (!this.scope.info) return 0;
      return Math.floor(this.scope.info.buffer.max_frames / Math.max(1, this.scope.channels.length));
    },
    async capture() {
      const sc = this.scope;
      sc.busy = true;
      try {
        do {
          sc.state = 'setting up';
          const res = await api('POST', `/api/nodes/${sc.node}/scope/capture`, {
            channels: sc.channels, sample_period_us: sc.period, num_samples: sc.samples, pretrigger_samples: sc.pretrigger,
            trigger_mode: sc.mode, trigger_source: sc.source, trigger_level: sc.level, hysteresis_band: sc.band,
          });
          sc.result = res;
          this.drawScope(res);
          sc.state = `${res.t.length} samples at ${res.sample_period_us} µs`;
        } while (sc.repeat && this.view === 'scope');
      } catch (e) { this.notify(e); sc.state = 'error'; }
      sc.busy = false;
    },
    async disarm() {
      if (this.scope.node === '') return;
      await api('POST', `/api/nodes/${this.scope.node}/scope/disarm`).catch(e => this.notify(e));
    },
    channelLabels(res) {
      const vars = this.scope.info.variables;
      return this.scope.channels.slice(0, res.channels.length).map(id => {
        const v = vars.find(x => x.id === id);
        return v ? `${v.name}${v.unit ? ' [' + v.unit + ']' : ''}` : `#${id}`;
      });
    },
    drawScope(res) {
      const el = document.getElementById('scope-plot');
      if (plots.scope) plots.scope.destroy();
      plots.scope = makePlot(el, this.channelLabels(res), 480);
      plots.scope.setData([res.t, ...res.channels]);
    },
    exportCsv() {
      const res = this.scope.result;
      const header = ['t_s', ...this.channelLabels(res)].join(',');
      const rows = res.t.map((t, i) => [t, ...res.channels.map(c => c[i])].join(','));
      const blob = new Blob([header + '\n' + rows.join('\n') + '\n'], { type: 'text/csv' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `cymon_node${res.node_id}_${new Date().toISOString().replace(/[:.]/g, '-')}.csv`;
      a.click();
    },

    // ── Log ────────────────────────────────────────────────────────────────
    filteredLog() {
      const min = SEVERITIES.indexOf(this.logLevel);
      return this.diagnostics.filter(d => SEVERITIES.indexOf(d.severity) >= min && (this.logNode === '' || d.node_id === this.logNode)).slice(-1000).reverse();
    },

    // ── Formatting ─────────────────────────────────────────────────────────
    fmtValue(v) {
      if (v === null || v === undefined) return '';
      return Array.isArray(v) ? v.map(x => (typeof x === 'number' && !Number.isInteger(x) ? +x.toPrecision(7) : x)).join(', ') : String(v);
    },
    fmtNum(v) { return typeof v === 'number' ? +v.toPrecision(5) : ''; },
    fmtUptime(s) {
      if (s === undefined) return '';
      const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
      return (d ? `${d}d ` : '') + `${h}:${String(m).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
    },
    fmtTime(t) { return new Date(t * 1000).toLocaleTimeString(undefined, { hour12: false }) + '.' + String(Math.floor((t % 1) * 1000)).padStart(3, '0'); },
  };
};
