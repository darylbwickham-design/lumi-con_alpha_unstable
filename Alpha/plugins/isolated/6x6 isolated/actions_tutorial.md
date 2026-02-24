---
## Matrix Relay (6x6 Ver) — Actions / Variations

This plugin does not add manual actions.

Instead, configure actions on **Alert Variations**:

### Alerts created by this plugin
- **6x6 short**
- **6x6 long**

Each alert has a variation condition:
- **Which Key (0–35)**

### How to set it up in Lumia
1) Go to **Alerts → Matrix Relay (6x6 Ver) → 6x6 short**
2) Enable **Only Variations** (base alert is disabled by the plugin)
3) Click **Add Variation**
   - Condition: *Which Key (0–35)*
   - Value: pick *Key 0* (or whatever key you want)
4) Add the Lumia actions you want for that key
5) Repeat for Key 1..Key 35
6) Do the same for **6x6 long**

### Notes
- Short press triggers **6x6 short**
- Long press triggers **6x6 long**
- The variation matching uses `dynamic.value` (string) internally, so Key values are matched as text ("0".."35").
---