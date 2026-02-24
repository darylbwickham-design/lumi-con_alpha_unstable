const { Plugin } = require("@lumiastream/plugin");

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

// No AbortController (runtime limitation)
async function httpGet(url, timeoutMs) {
  const res = await withTimeout(fetch(url, { method: "GET" }), timeoutMs);
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

function isPlaceholder(s) {
  const v = String(s || "").trim();
  if (!v) return true;
  // common “not substituted” placeholders we’ve seen
  if (v === "$1" || v === "${1}" || v === "\\1") return true;
  if (v === "{{message}}" || v === "{{ message }}") return true;
  return false;
}

// Try many likely places Lumia may store the chat text in Beta 12 payloads
function extractChatText(action, config) {
  const candidates = [];

  // 1) value.message (field)
  candidates.push(action?.value?.message);
  candidates.push(action?.value?.text);
  candidates.push(action?.value?.content);

  // 2) action-level properties
  candidates.push(action?.message);
  candidates.push(action?.text);
  candidates.push(action?.content);

  // 3) event payloads
  const ev = action?.event || action?.trigger || config?.event || config?.trigger;
  candidates.push(ev?.message);
  candidates.push(ev?.text);
  candidates.push(ev?.content);
  candidates.push(ev?.chatMessage);
  candidates.push(ev?.chat?.message);
  candidates.push(ev?.chat?.text);
  candidates.push(ev?.data?.message);
  candidates.push(ev?.data?.text);

  // 4) if message is an object (common)
  const msgObj = ev?.message || ev?.chatMessage || ev?.chat?.message;
  candidates.push(msgObj?.text);
  candidates.push(msgObj?.content);
  candidates.push(msgObj?.message);

  for (const c of candidates) {
    if (typeof c === "string" && c.trim().length) return c.trim();
  }
  return "";
}

module.exports = class ESP8266TFTChatBridge extends Plugin {
  async onload() {
    this.log?.info?.("ESP8266 TFT Chat Bridge loaded");
  }

  async actions(config) {
    const baseUrl = normalizeBaseUrl(this.settings?.baseUrl);
    const timeoutMs = Number(this.settings?.timeoutMs ?? 2000);
    const msgPath = normalizePath(this.settings?.msgPath ?? "/msg");
    const clearPath = normalizePath(this.settings?.clearPath ?? "/clear");

    // What command prefixes to strip (comma-separated in settings optional)
    const prefixesRaw = this.settings?.stripPrefixes;
    const prefixes =
      typeof prefixesRaw === "string" && prefixesRaw.trim().length
        ? prefixesRaw.split(",").map((x) => x.trim()).filter(Boolean)
        : ["!lcd"];

    if (!baseUrl) throw new Error("Invalid setting: ESP Base URL must start with http:// or https://");

    const actions = Array.isArray(config?.actions) ? config.actions : [];
    for (const action of actions) {
      const type = action?.type;

      if (type === "clear_screen") {
        await httpGet(joinUrl(baseUrl, clearPath), timeoutMs);
        continue;
      }

      if (type === "display_message") {
        // Prefer field, but if it’s a literal placeholder, ignore it and use event payload.
        let fieldMsg = action?.value?.message;
        let msg = "";

        if (typeof fieldMsg === "string" && !isPlaceholder(fieldMsg)) {
          msg = fieldMsg.trim();
        } else {
          msg = extractChatText(action, config);
        }

        if (!msg) {
          // Minimal diagnostic only when we truly can’t find anything
          this.log?.error?.("Display Message: could not extract chat text from action payload.");
          continue;
        }

        // If we got the whole line, strip "!lcd " so only payload goes to ESP
        msg = stripCommandPrefix(msg, prefixes);
        if (!msg) continue;

        const url = `${joinUrl(baseUrl, msgPath)}?t=${encodeURIComponent(msg)}`;
        await httpGet(url, timeoutMs);
      }
    }
  }
};