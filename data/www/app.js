/* cymon — Oscilloscope UI — app.js */
'use strict';

function app() {
  return {
    // ── View state ─────────────────────────────────────────────────────────
    view: 'nodes',

    // ── WebSocket ──────────────────────────────────────────────────────────
    ws: null,
    wsConnected: false,

    // ── Nodes view ─────────────────────────────────────────────────────────
    nodes: [],
    nodeRefreshInterval: null,

    // ── Graph view ─────────────────────────────────────────────────────────
    graphCfg: {
      nodeId: 1,
      varId: 0,
      mode: 'rolling',
      depthMs: 1000,
      triggerLevel: 0,
      triggerEdge: 'rising',
    },
    activeSessionId: null,
    triggered: false,
    sampleCount: 0,
    chart: null,
    chartData: [],   // [{t, v}] for latest channel

    // ── Settings view ───────────────────────────────────────────────────────
    wifiCfg: { ssid: '', password: '' },
    wifiStatus: { connected: false, ip: '' },
    apMode: false,
    canCfg: { nominalKbps: 500, dataMbps: 2 },

    // ────────────────────────────────────────────────────────────────────────
    init() {
      this.fetchNodes();
      this.nodeRefreshInterval = setInterval(() => {
        if (this.view === 'nodes') this.fetchNodes();
      }, 2000);
      this.fetchSettings();
      this.connectWs();
    },

    // ── Nodes ────────────────────────────────────────────────────────────────
    async fetchNodes() {
      try {
        const res = await fetch('/api/nodes');
        this.nodes = await res.json();
      } catch (e) {
        console.warn('fetchNodes failed', e);
      }
    },

    // ── Settings ─────────────────────────────────────────────────────────────
    async fetchSettings() {
      try {
        const res = await fetch('/api/settings');
        const s = await res.json();
        this.wifiCfg.ssid = s.ssid || '';
        this.canCfg.nominalKbps = s.nominal_kbps || 500;
        this.canCfg.dataMbps = s.data_mbps || 2;
      } catch (e) { /* ignore — device may not be reachable yet */ }
    },

    async saveWifi() {
      const res = await fetch('/api/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid: this.wifiCfg.ssid, password: this.wifiCfg.password }),
      });
      const r = await res.json();
      alert(r.ok ? 'WiFi settings saved — reconnecting…' : 'Failed');
    },

    async saveCan() {
      const res = await fetch('/api/can', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ nominal_kbps: this.canCfg.nominalKbps, data_mbps: this.canCfg.dataMbps }),
      });
      const r = await res.json();
      alert(r.ok ? 'CAN settings applied.' : 'Failed');
    },

    // ── Graph ─────────────────────────────────────────────────────────────────
    async startSession() {
      if (this.activeSessionId !== null) await this.stopSession();

      const body = {
        channels: [{ node_id: this.graphCfg.nodeId, variable_id: this.graphCfg.varId }],
        mode: this.graphCfg.mode,
        depth_ms: this.graphCfg.depthMs,
      };
      if (this.graphCfg.mode === 'triggered') {
        body.trigger = {
          level: this.graphCfg.triggerLevel,
          rising_edge: this.graphCfg.triggerEdge === 'rising',
          channel_index: 0,
        };
      }

      try {
        const res = await fetch('/api/session', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(body),
        });
        const r = await res.json();
        this.activeSessionId = r.session_id;
        this.initChart();
      } catch (e) {
        console.error('startSession failed', e);
      }
    },

    async stopSession() {
      if (this.activeSessionId === null) return;
      await fetch('/api/session/' + this.activeSessionId, { method: 'DELETE' });
      this.activeSessionId = null;
      this.triggered = false;
    },

    armTrigger() {
      this.triggered = !this.triggered;
      // The arm/disarm state is managed server-side via a future REST endpoint.
      // For now, toggle local flag and reflect on UI.
    },

    initChart() {
      const canvas = document.getElementById('oscCanvas');
      if (this.chart) { this.chart.destroy(); }
      this.chart = new Chart(canvas, {
        type: 'line',
        data: {
          datasets: [{
            label: `Node ${this.graphCfg.nodeId} var${this.graphCfg.varId}`,
            data: [],
            borderColor: '#00bcd4',
            backgroundColor: 'rgba(0,188,212,0.08)',
            borderWidth: 1.5,
            pointRadius: 0,
            tension: 0,   // no spline — exact samples only
          }],
        },
        options: {
          animation: false,
          responsive: true,
          scales: {
            x: {
              type: 'linear',
              title: { display: true, text: 'Time (ms)' },
            },
            y: {
              title: { display: true, text: 'Value' },
            },
          },
          plugins: {
            legend: { display: true },
          },
        },
      });
    },

    updateChart(tArr, vArr) {
      if (!this.chart || !tArr.length) return;
      const t0 = Number(tArr[0]);
      const points = tArr.map((ts, i) => ({ x: (Number(ts) - t0) / 1000, y: vArr[i] }));
      this.chart.data.datasets[0].data = points;
      this.chart.update('none');
      this.sampleCount = points.length;
      this.chartData = points;
    },

    exportCsv() {
      const rows = ['time_ms,value', ...this.chartData.map(p => `${p.x},${p.y}`)];
      const blob = new Blob([rows.join('\n')], { type: 'text/csv' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `cymon_session_${this.activeSessionId}.csv`;
      a.click();
      URL.revokeObjectURL(url);
    },

    // ── WebSocket ─────────────────────────────────────────────────────────────
    connectWs() {
      const url = `ws://${location.host}/ws`;
      this.ws = new WebSocket(url);
      this.ws.binaryType = 'arraybuffer';

      this.ws.onopen = () => {
        this.wsConnected = true;
        console.log('WS connected');
      };

      this.ws.onclose = () => {
        this.wsConnected = false;
        console.log('WS closed — retrying in 3 s');
        setTimeout(() => this.connectWs(), 3000);
      };

      this.ws.onerror = (e) => {
        console.warn('WS error', e);
      };

      this.ws.onmessage = (evt) => {
        try {
          const msg = msgpack.decode(new Uint8Array(evt.data));
          if (msg.s === this.activeSessionId && msg.ch && msg.ch.length > 0) {
            const ch = msg.ch[0];
            this.updateChart(ch.t, ch.v);
          }
        } catch (e) {
          console.warn('WS decode error', e);
        }
      };
    },
  };
}
