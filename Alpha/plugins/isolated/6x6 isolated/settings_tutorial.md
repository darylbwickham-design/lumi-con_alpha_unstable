---
## Matrix Relay (6x6 Ver) — Settings

### 1) Listen Port
- Default is **8787**
- Your ESP8266 should POST to:

`http://<PC-IP>:<PORT>/event`

Example (with your IP):
`http://192.168.1.87:8787/event`

### 2) Shared Secret (optional)
If you set a secret here, your ESP8266 must include this HTTP header:

`X-Matrix-Secret: <your secret>`

### 3) Data format
The plugin expects JSON:

`{ "event": N }`

Where:
- Short press: **0..35**
- Long press: **36..71**

The plugin converts these into:
- **6x6 short** with key **0..35**
- **6x6 long** with key **0..35**
---