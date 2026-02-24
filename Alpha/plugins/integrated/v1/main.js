const { Plugin } = require("@lumiastream/plugin");
const http = require("http");

const EVENT_MIN = 0;
const EVENT_MAX = 71;
const KEY_COUNT = 36;

// ---------- Helpers (no AbortController) ----------
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

function escapeRegExp(s) {
  return String(s).replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function stripCommandPrefix(fullLine, prefixes) {
  let out = String(fullLine || "").trim();
  if (!out) return "";
  for (const prefix of prefixes) {
    const p = String(prefix || "").trim();
    if (!p) continue;
    const re = new RegExp(`^${escapeRegExp(p)}\\s+`, "i");
    if (re.test(out)) {
      out = out.replace(re, "").trim();
      break;
    }
  }
  return out;
}

// ---------- Plugin ----------
module.exports = class LumiConBridgeIntegrated extends Plugin {
  constructor(manifest, context) {
    super(manifest, context);
    this._server = null;
  }

  // ---- lifecycle ----
  async onload() {
    if (this._isEnabled()) {
      await this._startServer();
    }
  }

  async onunload() {
    await this._stopServer();
  }

  async onsettingsupdate(settings, previousSettings) {
    const enabledChanged = Boolean(settings?.enabled) !== Boolean(previousSettings?.enabled);
    const portChanged = Number(settings?.listenPort) !== Number(previousSettings?.listenPort);

    if (enabledChanged || portChanged) {
      await this._stopServer();
      if (this._isEnabled()) {
        await this._startServer();
      }
    }
  }

  // ---- Actions (TFT) ----
  async actions(config) {
    const baseUrl = normalizeBaseUrl(this.settings?.baseUrl);
    const timeoutMs = Number(this.settings?.timeoutMs ?? 2000);

    const uiMode = String(this.settings?.uiMode ?? "legacy_get");
    const msgPath = normalizePath(this.settings?.msgPath ?? "/msg");
    const statusPath = normalizePath(this.settings?.statusPath ?? "/status");
    const clearPath = normalizePath(this.settings?.clearPath ?? "/clear");
    const uiPath = normalizePath(this.settings?.uiPath ?? "/ui");

    const prefixesRaw = this.settings?.stripPrefixes;
    const prefixes =
      typeof prefixesRaw === "string" && prefixesRaw.trim().length
        ? prefixesRaw.split(",").map((x) => x.trim()).filter(Boolean)
        : [];

    if (!baseUrl) throw new Error("Invalid setting: ESP Base URL must start with http:// or https://");

    const acts = Array.isArray(config?.actions) ? config.actions : [];
    for (const action of acts) {
      const type = action?.type;

      if (type === "clear_screen") {
        if (uiMode === "ui_post") {
          await httpPostJson(joinUrl(baseUrl, uiPath), { channel: "clear" }, timeoutMs);
        } else {
          await httpGet(joinUrl(baseUrl, clearPath), timeoutMs);
        }
        continue;
      }

      let msg = String(action?.value?.message ?? "").trim();
      if (!msg) continue;

      if (prefixes.length) {
        msg = stripCommandPrefix(msg, prefixes);
        if (!msg) continue;
      }

      if (type === "display_message") {
        if (uiMode === "ui_post") {
          await httpPostJson(joinUrl(baseUrl, uiPath), { channel: "chat", text: msg }, timeoutMs);
        } else {
          const url = `${joinUrl(baseUrl, msgPath)}?t=${encodeURIComponent(msg)}`;
          await httpGet(url, timeoutMs);
        }
        continue;
      }

      if (type === "status_message") {
        if (uiMode === "ui_post") {
          await httpPostJson(joinUrl(baseUrl, uiPath), { channel: "status", text: msg }, timeoutMs);
        } else {
          const url = `${joinUrl(baseUrl, statusPath)}?t=${encodeURIComponent(msg)}`;
          await httpGet(url, timeoutMs);
        }
        continue;
      }
    }
  }

  // ---- Listener (ESP -> Lumia) ----
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

  async _startServer() {
    if (this._server) return;

    const port = this._getPort();
    this._server = http.createServer((req, res) => {
      void this._handleRequest(req, res);
    });

    await new Promise((resolve, reject) => {
      this._server.once("error", reject);
      this._server.listen(port, "0.0.0.0", () => resolve());
    });
  }

  async _stopServer() {
    if (!this._server) return;

    const server = this._server;
    this._server = null;

    await new Promise((resolve) => {
      server.close(() => resolve());
    });
  }

  async _handleRequest(req, res) {
    try {
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

      if (!Number.isInteger(eventNumber) || eventNumber < EVENT_MIN || eventNumber > EVENT_MAX) {
        return this._sendJson(res, 400, { ok: false, error: "Invalid event number" });
      }

      const isLong = eventNumber >= KEY_COUNT;
      const keyIndex = isLong ? (eventNumber - KEY_COUNT) : eventNumber;

      const kind = isLong ? "long" : "short";
      const alertKey = isLong ? "matrix_6x6_long" : "matrix_6x6_short";
      const receivedAt = new Date().toISOString();

      await this.lumia.setVariable("event", keyIndex);
      await this.lumia.setVariable("kind", kind);
      await this.lumia.setVariable("received_at", receivedAt);

      // Variations use dynamic.value; pass same info in extraSettings for templating
      await this.lumia.triggerAlert({
        alert: alertKey,
        dynamic: { value: String(keyIndex) },
        extraSettings: { event: keyIndex, kind, received_at: receivedAt }
      });

      return this._sendJson(res, 200, { ok: true });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      await this.lumia.log(`[Lumi-Con Integrated] Error: ${message}`);
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
        try {
          resolve(JSON.parse(data));
        } catch {
          resolve({});
        }
      });
      req.on("error", reject);
    });
  }

  _sendJson(res, status, obj) {
    try {
      const payload = JSON.stringify(obj);
      res.writeHead(status, {
        "Content-Type": "application/json",
        "Content-Length": Buffer.byteLength(payload)
      });
      res.end(payload);
    } catch {
      res.writeHead(500);
      res.end();
    }
  }
};