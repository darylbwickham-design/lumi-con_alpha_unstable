const { Plugin } = require("@lumiastream/plugin");
const http = require("http");

const EVENT_MIN = 0;
const EVENT_MAX = 71;
const KEY_COUNT = 36;

function withTimeout(promise, timeoutMs) {
  const ms = Number(timeoutMs) || 2000;
  return Promise.race([
    promise,
    new Promise((_, reject) => setTimeout(() => reject(new Error("Request timed out")), ms)),
  ]);
}

function normalizeBaseUrl(baseUrl) {
  const u = String(baseUrl || "").trim().replace(/\/+$/, "");
  if (!u) return "";
  if (!/^https?:\/\//i.test(u)) return "";
  return u;
}

function normalizePath(p) {
  const s = String(p || "").trim();
  if (!s) return "/";
  return s.startsWith("/") ? s : `/${s}`;
}

function joinUrl(base, path) {
  const b = String(base || "").replace(/\/+$/, "");
  const p = String(path || "");
  return `${b}${p.startsWith("/") ? p : `/${p}`}`;
}

async function httpGet(url, timeoutMs) {
  const res = await withTimeout(fetch(url, { method: "GET" }), timeoutMs);
  const body = await res.text().catch(() => "");
  if (!res.ok) throw new Error(`HTTP ${res.status}${body ? `: ${body}` : ""}`);
  return body;
}

async function httpPostJson(url, obj, timeoutMs) {
  const res = await withTimeout(
    fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(obj),
    }),
    timeoutMs
  );
  const body = await res.text().catch(() => "");
  if (!res.ok) throw new Error(`HTTP ${res.status}${body ? `: ${body}` : ""}`);
  return body;
}


function parseKeyLabels(raw) {
  const text = typeof raw === "string" ? raw : "";
  const lines = text.split(/\r?\n/).map((l) => l.trim());
  // keep empty lines as empty, but cap at 36
  return lines.slice(0, 36);
}

function getKeyLabelFromList(list, keyIndex) {
  const idx = Number(keyIndex);
  if (!Number.isInteger(idx) || idx < 0) return "";
  if (idx >= list.length) return "";
  const label = String(list[idx] ?? "").trim();
  return label;
}

module.exports = class LumiConBridgeIntegratedV2 extends Plugin {
  constructor(manifest, context) {
    super(manifest, context);
    this._server = null;

    // In-memory dedupe: deviceId -> lastSeq processed
    this._lastSeqByDevice = new Map();

    // Cached key labels, 0..35
    this._keyLabelsShort = [];
    this._keyLabelsLong = [];
    this._keyLabelsLegacy = [];

    // Track whether we've seen at least one device event since load
    this._deviceConnected = false;
  }

  async onload() {
    this._refreshKeyLabels();
    await this.lumia.setVariable("device_connected", false);
    if (this._isEnabled()) await this._startServer();
  }

  async onunload() {
    await this._stopServer();
  }

  async onsettingsupdate(settings, previousSettings) {
    this._refreshKeyLabels();
    const enabledChanged = Boolean(settings?.enabled) !== Boolean(previousSettings?.enabled);
    const portChanged = Number(settings?.listenPort) !== Number(previousSettings?.listenPort);

    if (enabledChanged || portChanged) {
      await this._stopServer();
      if (this._isEnabled()) await this._startServer();
    }
  }

  // -------------------------
  // TFT actions (plugin -> ESP)
  // -------------------------
  async actions(config) {
    const baseUrl = normalizeBaseUrl(this.settings?.baseUrl);
    if (!baseUrl) return; // optional (minimal setup)

    const timeoutMs = Number(this.settings?.timeoutMs ?? 2000);

    const uiMode = String(this.settings?.uiMode ?? "legacy_get");
    const msgPath = normalizePath(this.settings?.msgPath ?? "/msg");
    const statusPath = normalizePath(this.settings?.statusPath ?? "/status");
    const clearPath = normalizePath(this.settings?.clearPath ?? "/clear");
    const uiPath = normalizePath(this.settings?.uiPath ?? "/ui");

    const acts = Array.isArray(config?.actions) ? config.actions : [];
    for (const action of acts) {
      const type = action?.type;

      if (type === "clear_screen") {
        if (uiMode === "ui_post") await httpPostJson(joinUrl(baseUrl, uiPath), { channel: "clear" }, timeoutMs);
        else await httpGet(joinUrl(baseUrl, clearPath), timeoutMs);
        continue;
      }

      const msg = String(action?.value?.message ?? "").trim();
      if (!msg) continue;

      if (type === "display_message") {
        if (uiMode === "ui_post") await httpPostJson(joinUrl(baseUrl, uiPath), { channel: "chat", text: msg }, timeoutMs);
        else await httpGet(`${joinUrl(baseUrl, msgPath)}?t=${encodeURIComponent(msg)}`, timeoutMs);
        continue;
      }

      if (type === "status_message") {
        if (uiMode === "ui_post") await httpPostJson(joinUrl(baseUrl, uiPath), { channel: "status", text: msg }, timeoutMs);
        else await httpGet(`${joinUrl(baseUrl, statusPath)}?t=${encodeURIComponent(msg)}`, timeoutMs);
        continue;
      }
    }
  }

  // -------------------------
  // Listener (ESP -> plugin)
  // -------------------------
  _isEnabled() {
    return Boolean(this.settings?.enabled ?? true);
  }

  _getPort() {
    const port = Number(this.settings?.listenPort ?? 8787);
    return Number.isInteger(port) && port > 0 && port <= 65535 ? port : 8787;
  }

  _getSecret() {
    return String(this.settings?.secret ?? "").trim();
  }

_refreshKeyLabels() {
    this._keyLabelsShort = parseKeyLabels(this.settings?.keyLabelsShort);
    this._keyLabelsLong = parseKeyLabels(this.settings?.keyLabelsLong);
    this._keyLabelsLegacy = parseKeyLabels(this.settings?.keyLabels);

    // Backwards compat: if new lists are empty, fall back to legacy list for both
    if (!this._keyLabelsShort.length && this._keyLabelsLegacy.length) {
      this._keyLabelsShort = this._keyLabelsLegacy;
    }
    if (!this._keyLabelsLong.length && this._keyLabelsLegacy.length) {
      this._keyLabelsLong = this._keyLabelsLegacy;
    }
  }

  _getKeyLabel(kind, keyIndex) {
    const list = kind === "long" ? this._keyLabelsLong : this._keyLabelsShort;
    const label = getKeyLabelFromList(list, keyIndex);
    return label || `Key ${keyIndex}`;
  }

    async _startServer() {
    if (this._server) return;

    const port = this._getPort();
    this._server = http.createServer((req, res) => void this._handleRequest(req, res));

    await new Promise((resolve, reject) => {
      this._server.once("error", reject);
      this._server.listen(port, "0.0.0.0", () => resolve());
    });
  }

  async _stopServer() {
    if (!this._server) return;

    const server = this._server;
    this._server = null;

    await new Promise((resolve) => server.close(() => resolve()));
  }

  async _handleRequest(req, res) {
    try {
      if (req.method === "GET" && req.url === "/health") {
        return this._sendJson(res, 200, { ok: true, name: this.manifest?.name ?? "plugin", version: this.manifest?.version ?? "" });
      }

      if (req.method !== "POST" || req.url !== "/event") {
        return this._sendJson(res, 404, { ok: false, error: "Not found" });
      }

      const expectedSecret = this._getSecret();
      if (expectedSecret) {
        const headerValue = String(req.headers["x-matrix-secret"] ?? "").trim();
        if (!headerValue || headerValue !== expectedSecret) {
          return this._sendJson(res, 401, { ok: false, error: "Unauthorized" });
        }
      }

      const body = await this._readJsonBody(req);

      const eventNumber = Number(body?.event);
      const seq = Number.isInteger(Number(body?.seq)) ? Number(body.seq) : null;
      const deviceId = typeof body?.deviceId === "string" ? body.deviceId : "";
      const heldMs = Number.isInteger(Number(body?.heldMs)) ? Number(body.heldMs) : 0;

      // Always ACK with seq when provided
      const ackPayload = { ok: true };
      if (seq !== null) ackPayload.seq = seq;

      if (!Number.isInteger(eventNumber) || eventNumber < EVENT_MIN || eventNumber > EVENT_MAX) {
        return this._sendJson(res, 400, { ok: false, error: "Invalid event number", ...(seq !== null ? { seq } : {}) });
      }

      // Dedupe only when we have deviceId + seq
      if (deviceId && seq !== null) {
        const prev = this._lastSeqByDevice.get(deviceId) ?? 0;
        if (seq <= prev) {
          // already processed (or out-of-order) -> ACK but do not trigger again
          return this._sendJson(res, 200, ackPayload);
        }
      }

      const isLong = eventNumber >= KEY_COUNT;
      const keyIndex = isLong ? (eventNumber - KEY_COUNT) : eventNumber;

      const kind = isLong ? "long" : "short";
      const alertKey = isLong ? "matrix_6x6_long" : "matrix_6x6_short";
      const receivedAt = new Date().toISOString();

      await this.lumia.setVariable("event", keyIndex);
      await this.lumia.setVariable("kind", kind);
      await this.lumia.setVariable("received_at", receivedAt);

      const remoteIp = String(req.socket?.remoteAddress ?? "").replace(/^::ffff:/, "");
      const rssi = Number.isFinite(Number(body?.rssi)) ? Number(body.rssi) : 0;

      if (!this._deviceConnected) {
        this._deviceConnected = true;
        await this.lumia.setVariable("device_connected", true);
      }

      await this.lumia.setVariable("device_id", deviceId);
      await this.lumia.setVariable("device_ip", remoteIp);
      await this.lumia.setVariable("device_last_seen", receivedAt);
      await this.lumia.setVariable("device_rssi", rssi);

      await this.lumia.setVariable("seq", seq ?? 0);
      await this.lumia.setVariable("held_ms", heldMs);

      const keyLabel = this._getKeyLabel(kind, keyIndex);
      await this.lumia.setVariable("key_label", keyLabel);

      await this.lumia.triggerAlert({
        alert: alertKey,
        dynamic: { value: String(keyIndex) },
        extraSettings: {
          event: keyIndex,
          kind,
          received_at: receivedAt,
          device_id: deviceId,
          device_ip: remoteIp,
          device_last_seen: receivedAt,
          device_rssi: rssi,
          device_connected: true,
          seq: seq ?? 0,
          held_ms: heldMs,
          key_label: keyLabel,
        },
      });

      // Update dedupe map after successful processing
      if (deviceId && seq !== null) {
        this._lastSeqByDevice.set(deviceId, seq);
      }

      return this._sendJson(res, 200, ackPayload);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      await this.lumia.log(`[Lumi-Con v2] Error: ${message}`);
      return this._sendJson(res, 500, { ok: false, error: "Server error" });
    }
  }

  _readJsonBody(req) {
    return new Promise((resolve, reject) => {
      let data = "";
      req.on("data", (chunk) => {
        data += chunk;
        if (data.length > 1024 * 16) reject(new Error("Body too large"));
      });
      req.on("end", () => {
        if (!data) return resolve({});
        try { resolve(JSON.parse(data)); } catch { resolve({}); }
      });
      req.on("error", reject);
    });
  }

  _sendJson(res, status, obj) {
    try {
      const payload = JSON.stringify(obj);
      res.writeHead(status, {
        "Content-Type": "application/json",
        "Content-Length": Buffer.byteLength(payload),
      });
      res.end(payload);
    } catch {
      res.writeHead(500);
      res.end();
    }
  }
};
