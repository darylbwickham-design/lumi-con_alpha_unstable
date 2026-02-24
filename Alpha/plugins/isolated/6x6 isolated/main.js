const { Plugin } = require("@lumiastream/plugin");
const http = require("http");

const EVENT_MIN = 0;
const EVENT_MAX = 71;
const KEY_COUNT = 36;

class MatrixRelay6x6Ver extends Plugin {
	constructor(manifest, context) {
		super(manifest, context);
		this._server = null;
	}

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

			// IMPORTANT: Lumia support confirmed variations should use dynamic: { value: "..." }
			await this.lumia.triggerAlert({
				alert: alertKey,
				dynamic: { value: String(keyIndex) },
				extraSettings: {
					event: keyIndex,
					kind,
					received_at: receivedAt
				}
			});

			return this._sendJson(res, 200, { ok: true });
		} catch (error) {
			const message = error instanceof Error ? error.message : String(error);
			await this.lumia.log(`[Matrix Relay (6x6 Ver)] Error: ${message}`);
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
}

module.exports = MatrixRelay6x6Ver;