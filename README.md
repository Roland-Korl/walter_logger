# Walter Feels solar/LiFePO4 test node

Firmware for one of the three test nodes in the solar-panel-test-handover
(2026-09-01): a single 30-50Wp panel charging a 12.8V/20Ah LiFePO4 battery
through the onboard LTC4015, with a fixed 10 ohm / 10 W resistor on the 5V
rail standing in for the real device load. Reads the LTC4015 + onboard
sensors + an independent DS18B20 once a minute and serves/publishes the
result.

## Two transport modes (`TELEMETRY_TRANSPORT_WIFI` in `src/config.h`)

- **WiFi (current default)** - connects to a local WiFi network, serves a
  live dashboard and JSON API over HTTP, supports OTA firmware updates,
  and optionally bridges telemetry to an MQTT broker. The node stays
  awake continuously (no deep sleep) so it can answer requests.
- **Cellular** - the original design for the actual winter field
  deployment, which won't have WiFi. Connects over the Walter modem,
  publishes to an MQTT broker, and deep-sleeps between samples to avoid
  the ESP32+modem's own power draw skewing the solar energy balance
  being measured. Not actively exercised recently - re-verify LTE
  connectivity (APN, RAT, signal) before relying on it again.

Everything charger-safety-related (JEITA, chemistry/cell verification,
default-suspended boot sequence) is identical in both modes - see
`ltc4015SafeInit()` in `src/main.cpp`.

## Before you flash

### 1. Solder jumpers on the Walter Feels board

12.8V LiFePO4 = 4 cells, LiFePO4 fixed fast charge:

| Jumper | Setting |
|---|---|
| CELLS0 | Z (both open) |
| CELLS1 | L (OFF closed, ON open) |
| CELLS2 | L (OFF closed, ON open) |
| CHEM0  | Z (both open) |
| CHEM1  | H (ON closed, OFF open) |

The firmware reads `CHEM_CELLS` back at boot and refuses to enable
charging if this doesn't match `BATTERY_EXPECTED_CHEM` /
`BATTERY_EXPECTED_CELL_COUNT` in `src/config.h`. This check only runs once,
at boot - if it fails (e.g. because the panel was disconnected at power-up
so the LTC4015 had no valid measurement yet), charging stays suspended
for the rest of that session even if conditions improve later. Reboot
(power-cycle, or re-push the same firmware via OTA) to re-run the check.

### 2. Wiring

- Solar panel -> MPPT input terminals (**panel Voc must stay under 26V in
  the coldest/brightest conditions you'll test in** - see the separate
  panel-selection discussion; this is independent of this firmware).
- 12.8V/20Ah LiFePO4 -> battery terminals.
- DS18B20 data line -> `DS18B20_PIN` in `src/config.h` (defaults to
  `WFEELS_PIN_GPIO_A`), with a 4.7k pull-up to 3V3. DS18B20 VDD -> 3V3,
  GND -> GND. This is the *independent cross-check* sensor - the primary,
  always-on safety cutoff is the LTC4015's own onboard NTC (already
  populated on the board as R12), which works even if the ESP32 is
  asleep or has crashed.

### 3. Set up `config.h`

`src/config.h` is **gitignored** because it holds real WiFi/OTA/MQTT
credentials. Copy the template and fill in your own values:

```bash
cp src/config.example.h src/config.h
```

At minimum for WiFi mode: `WIFI_SSID`, `WIFI_PASSWORD`, `OTA_PASSWORD`
(change it from the placeholder before mounting the board anywhere you
can't easily reach over USB - anyone who can authenticate at `/update`
can push arbitrary firmware). Everything else has a reasoned default -
see the comments in `src/config.example.h`.

### 4. Build with PlatformIO

```bash
pio run -e usb -t upload --upload-port COMx   # first flash / bench test only
```

`platformio.ini` pulls in all required libraries automatically
(`WalterModem`, `ArduinoJson` v7, `OneWire`, `DallasTemperature`,
`PubSubClient` for the WiFi-mode MQTT bridge) - no separate Library
Manager step needed. There is intentionally **no ElegantOTA dependency**:
OTA is handled directly via the ESP32 `<Update.h>` API in `src/portal.cpp`
(same approach the `pv-logger-c3` sibling project already uses), which
avoids a real upstream bug this project used to work around with a
manually patched local copy of `ElegantOTA.h`.

Once a device is mounted, every further update goes through
`http://<node-ip>/update` (see "What's running" below) - there is no
second, network-push OTA path (no `espota`/`ArduinoOTA`), by design: the
`pv-logger-c3` sibling project's README documents that path failing
against Windows Firewall even with correct auth, and a device with no
physical fallback shouldn't depend on a path known to be flaky.

### 5. ESP32 core version - pinned, not just "latest"

Built and verified against **esp32 Arduino core 3.1.3**, not the newer
3.3.x line. Core 3.2.0+ has a real regression in the new I2C driver
(`esp32-hal-i2c-ng`) that makes every I2C transaction on this board fail
with `ESP_ERR_INVALID_STATE` from the very first transaction - see
[espressif/arduino-esp32#11374](https://github.com/espressif/arduino-esp32/issues/11374).
3.1.3 uses the older, working I2C driver.

`platformio.ini` pins this explicitly via `platform_packages` (the
framework is fetched from the official 3.1.3 GitHub release zip, not
just a `platform = espressif32@X.Y.Z` version number, since that mapping
isn't precise enough to trust blindly for a board with no physical
fallback). **Verify after the first `pio run`** that the resolved core is
actually 3.1.3 before relying on it - re-test this pin before ever
upgrading it.

The partition scheme (`ffat.csv`, vendored in this repo, copied verbatim
from the Arduino IDE's "fatflash" / "16M Flash (2MB APP/12.5MB FATFS)"
board-manager entry - **not** hand-typed offsets, to guarantee the OTA
slot boundaries match what's already burned into a device flashed via
Arduino IDE) has dual OTA app partitions (`app0`/`app1`, `otadata`) - no
further partition table change is needed for OTA to work.

## What's running (WiFi mode)

- **`/`** - dashboard (dark, card-based, inspired by pv.rw-portal.eu):
  charge state, an energy-flow diagram (panel -> battery -> system),
  a grid of stat cards with sparklines (built client-side from
  `/history`), and a decoded fault/status list. Auto-refreshes via
  `fetch()` every 5s, no full page reload.
- **`/telemetry`** - the latest sample as JSON (see below).
- **`/history`** - up to `HISTORY_LEN` (120) recent samples, oldest
  first, for the dashboard's sparklines. RAM-only, not persisted.
- **`/update`** - firmware upload page (`<Update.h>`-based, HTTP Basic
  Auth via `OTA_USERNAME`/`OTA_PASSWORD`). `GET /update` serves the upload
  form, `POST /update` (multipart, the exported `.bin`) streams straight
  to flash and reboots the board with no USB access needed - this is the
  **only** OTA path in this project (see "Build with PlatformIO" above
  for why a second, network-push path was deliberately left out).
- **MQTT bridge** (`MQTT_WIFI_ENABLED`) - if enabled, also publishes the
  same JSON to `MQTT_WIFI_HOST:MQTT_WIFI_PORT` on `MQTT_WIFI_TOPIC_FMT`,
  independent of (and non-blocking for) the HTTP server above. **Not yet
  confirmed working end-to-end** - see the comment block above
  `MQTT_WIFI_ENABLED` in `src/config.example.h`: neither "is the broker
  actually reachable from this node's WiFi network" nor "does the topic/
  payload match what the receiving backend expects" has been verified.
  Check the serial log (`MQTT connect to ...: OK/FAILED`) after flashing.

## Telemetry JSON shape

```json
{
  "node": 1, "boot": 3,
  "chg": {
    "vin": 13.61, "iin": 1.27, "vbat": 13.28, "vbat_filt": 13.28,
    "ibat": 1.06, "vsys": 13.54, "qcount": 32768,
    "icharge_dac": 8.0, "iin_limit_dac": 3.0, "bsr_raw": 2568,
    "die_c": 52.1, "ntc_c": 45.0, "jeita_region": 6,
    "state": 512, "status": 0, "sysstat": 10951,
    "cold_block": false, "cold_block_cycles": 0,
    "flags": {
      "absorb_charge": true, "charger_suspended": false,
      "charger_enabled": true, "ok_to_charge": true,
      "ntc_pause": false, "bat_missing_fault": false, "...": "..."
    }
  },
  "temp": { "ds18b20_c": null, "disagree": false },
  "env": { "temp_c": 47.4, "hum_pct": 32.0, "press_hpa": 964.6 },
  "co2_ppm": 430,
  "cell": { "band": 20, "id": 123456, "rsrp": -97.0, "rsrq": -11.0, "rat": 1 },
  "wifi_rssi": -56
}
```

- `chg.state` / `chg.status` / `chg.sysstat` are the raw LTC4015
  `CHARGER_STATE` / `CHARGE_STATUS` / `SYSTEM_STATUS` registers.
  `chg.flags` decodes every bit of all three into named booleans (see
  `telemetry.cpp`) so you don't have to re-derive the bitmasks - the raw
  values are still included for anything the decode misses.
- `chg.jeita_region` (1-7, 1=coldest/forced-off, 7=hottest/forced-off) is
  computed in firmware from the live NTC reading against the configured
  `JEITA_T1..T6` thresholds - **not** read from an LTC4015 region
  register (that register's exact bit layout wasn't independently
  verified). Since both this and the configured thresholds go through
  the same verified NTC beta-equation, the comparison is self-consistent
  even though the raw hardware register isn't used directly.
- `chg.icharge_dac` / `chg.iin_limit_dac` are the *live* DAC values the
  charger is actually using right now (amps), which can differ from what
  was commanded if JEITA is throttling for temperature - e.g.
  `icharge_dac` sitting at its ceiling during CV/absorb charging is
  normal (the current loop isn't the active constraint then), not a bug.
- `chg.bsr_raw` is the LTC4015's raw battery-series-resistance register,
  refreshed roughly every 30 samples (a fresh measurement is triggered
  periodically, not every cycle). **Not converted to ohms** - that
  formula wasn't independently verified - but the raw trend (rises for
  LiFePO4 in the cold) is useful on its own for the winter test.
- `chg.qcount` is the LTC4015's raw coulomb counter register, also not
  converted to mAh (prescale-factor formula not independently verified).
  The handover doc's primary metric (integral of VIN x IIN = Wh/day) is
  meant to be computed from the logged `vin`/`iin` history downstream
  instead.
- `cold_block_cycles` resets on power loss (RTC memory, not flash). For
  "how many hours were lost to cold" over the whole winter, compute it
  from the logged `charger_state`/`ntc_c` history in your backend instead
  - that's the source of truth, not this counter.

## Known limitations / what to re-verify before trusting this unattended

1. **JEITA temperature thresholds** (the hardware cold-charging cutoff)
   were computed from the standard NTC beta-equation using the board's
   actual thermistor (R12: 10k, beta=3380K) and a datasheet formula/fact
   set cross-checked via multiple independent lookups rather than a
   local copy of the full LTC4015 datasheet PDF. The single most
   safety-critical fact - "JEITA region 1 = charger forced off in
   hardware below JEITA_T1" - was confirmed directly, and observed live
   (charging correctly enabled once NTC read above the threshold).
   **Recommended before unattended winter operation:** bench-verify with
   a lab supply and a variable resistor standing in for the NTC (or the
   real sensor in a fridge/freezer) that charging stops and resumes at
   the expected temperatures.
2. `VCHARGE_JEITA_*` registers are deliberately left unconfigured - see
   `src/ltc4015.h`. Only relevant if you switch to a *programmable* (not
   fixed) chemistry setting later.
3. `set_input_voltage_min()` is a stub (not wired into the boot
   sequence) - the VIN_UVCL_SETTING code-to-volts formula wasn't
   re-verified.
4. The boot-time safety check (chemistry/cells + valid NTC reading) runs
   **once**, not continuously - see the jumper section above. If the
   panel/battery aren't both present and stable at power-up, charging
   stays suspended until the next reboot even if conditions improve.
5. MQTT-over-WiFi bridge (`MQTT_WIFI_ENABLED`) is untested against the
   real target broker/backend - see "What's running" above.

## Architecture notes

- **WiFi mode**: continuous run, `loop()` handles HTTP requests, OTA,
  periodic re-sampling, and the optional MQTT bridge. No deep sleep -
  fine for bring-up/monitoring, but note this draws more power
  continuously than the cellular mode's deep-sleep cycle, which matters
  if you're trying to measure the panel's energy balance precisely.
- **Cellular mode**: the node wakes, does everything in `setup()`, and
  goes back to deep sleep - `loop()` is never reached. This matters for
  the panel-comparison test itself, since a continuously running
  ESP32+modem would add its own power draw on top of the fixed resistor
  load and skew the energy balance you're trying to measure.
