#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

// ============================================================================
// RENTECH Battery Tester RT-004 — Firmware v1.13
// Firmware by Arduino.ua&KIRA
//
// VERSION HISTORY — failures deliberately preserved in the version trail:
// v1.07:
// - programmable CHARGE ENABLE + first web/AP
// v1.08:
// - safer sequencing / load sanity; Wi-Fi startup over-engineered
// v1.09:
// - simplified AP attempt
// v1.10:
// - known-good MINI OLEG AP startup; VERIFIED network baseline
// v1.11:
// - bad experiment: DNSServer/captive portal added without isolation;
//   regression reported; REJECTED AS BASE
// v1.12:
// - CLEAN rebuild from v1.10
// - lightweight /status + incremental live log (no full ring every poll)
// - Copy Log + Save Log on the main dashboard
// - configurable cutoff voltage, locked for the duration of each test
// - early-cutoff / partial-start full-capacity estimate using SOC(start)-SOC(cutoff)
// - NO DNSServer, NO captive portal
// - removed accidental duplicate WiFi.softAP() call carried by v1.10
// - classic FULL -> 2.50 V result remains MEASURED only (no fake EST=MEASURED)
// v1.13:
// - device identity normalized to RT-004 everywhere (LCD/web/AP/log/files)
// - Copy Log + Save Log use the full browser-side accumulated session log
// - browser log history is no longer erased when the ESP ring buffer overruns
// - initial "Connecting..." placeholder is removed on first real log chunk
// ============================================================================

// ESP32-C3 Super Mini pin map
constexpr uint8_t PIN_START         = 3;  // button -> GND, INPUT_PULLUP
constexpr uint8_t PIN_SDA           = 4;  // I2C low-voltage side
constexpr uint8_t PIN_SCL           = 5;  // I2C low-voltage side
constexpr uint8_t PIN_CHARGE_ENABLE = 6;  // HIGH -> AO3400/AO3401 switch ON -> HW-373 IN+ gets +5V
constexpr uint8_t PIN_RELAY         = 7;  // LOW = DISCHARGE (active-low relay module)

constexpr uint8_t RELAY_DISCHARGE_LEVEL = LOW;
constexpr uint8_t CHARGE_ENABLE_LEVEL    = HIGH;

constexpr uint8_t LCD_ADDRESS = 0x27;
constexpr uint8_t INA219_ADDRESS_C3 = 0x40;

constexpr char DEVICE_MODEL[] = "RT-004";
constexpr char FW_VERSION[]   = "1.13";

// Wi-Fi AP for browser logs.
// v1.10 intentionally reuses the known-good MINI OLEG startup pattern
// validated on the same ESP32-C3 Super Mini hardware.
constexpr char AP_SSID[]     = "RENTECH-RT004";
constexpr char AP_PASSWORD[] = "rentech004";

// Test limits / UX canon
// MIN_VOLTAGE remains the absolute/full-test lower cutoff from the verified v1.10 path.
// v1.12 allows a higher user-selected early cutoff, but never below MIN_VOLTAGE.
constexpr float MIN_VOLTAGE = 2.50f;
constexpr float DEFAULT_CUTOFF_V = MIN_VOLTAGE;
constexpr float MAX_CONFIGURABLE_CUTOFF_V = 4.10f;
constexpr float FULL_START_TARGET_V = 4.200f;
constexpr float FULL_START_TOLERANCE_V = 0.005f;
constexpr float FULL_START_MIN_V = FULL_START_TARGET_V - FULL_START_TOLERANCE_V;

// The 8 ohm load should draw ~525 mA at 4.2 V and ~312 mA at 2.5 V.
constexpr float MIN_LOAD_CURRENT_MA = 100.0f;
constexpr uint8_t LOAD_VERIFY_SAMPLES = 5;
constexpr uint8_t LOAD_VERIFY_REQUIRED = 3;
constexpr uint32_t LOAD_RELAY_SETTLE_MS = 250;
constexpr uint32_t LOAD_VERIFY_INTERVAL_MS = 200;

// Power-path sequencing.
constexpr uint32_t CHARGE_OFF_SETTLE_MS = 250;
constexpr uint32_t RELAY_TO_CHARGE_SETTLE_MS = 120;

constexpr uint32_t SAMPLE_INTERVAL_MS    = 250;
constexpr uint32_t DISPLAY_INTERVAL_MS   = 500;
constexpr uint32_t READY_INTERVAL_MS     = 500;
constexpr uint32_t DONE_PAGE_INTERVAL_MS = 2500;
constexpr uint32_t LOG_INTERVAL_MS       = 1000;
constexpr uint32_t BUTTON_DEBOUNCE_MS    = 30;
constexpr uint32_t BUTTON_LONG_PRESS_MS  = 1500;
constexpr uint8_t CUTOFF_CONFIRM_SAMPLES = 4;

// Runtime load sanity. With the verified INA orientation, discharge current is positive.
constexpr uint8_t RUN_LOAD_FAIL_CONFIRM_SAMPLES = 4;   // 4 x 250ms ~= 1s
constexpr float REVERSE_CURRENT_FAULT_MA = -20.0f;     // clear wrong-direction current

// Do not extrapolate from a vanishingly small tested SOC slice.
// The voltage/SOC model itself remains UNVALIDATED until compared with real full runs.
constexpr float MIN_TESTED_SOC_FRACTION_FOR_ESTIMATE = 0.05f;

// Web log ring buffer: fixed-size to avoid unbounded String growth.
constexpr size_t WEB_LOG_LINES = 120;
constexpr size_t WEB_LOG_LINE_LEN = 192;

LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
Adafruit_INA219 ina219(INA219_ADDRESS_C3);
WebServer server(80);

enum class TesterState : uint8_t {
  READY,
  DISCHARGING,
  DONE,
  NO_LOAD,
  ERROR_STATE
};

enum class LoadCheckResult : uint8_t {
  PASS,
  NO_LOAD,
  REVERSE_CURRENT,
  READ_ERROR
};

constexpr uint8_t BUTTON_EVENT_NONE  = 0;
constexpr uint8_t BUTTON_EVENT_SHORT = 1;
constexpr uint8_t BUTTON_EVENT_LONG  = 2;

TesterState state = TesterState::READY;

float batteryVoltage_V = 0.0f;
float dischargeCurrent_mA = 0.0f;
float capacity_mAh = 0.0f;
float startVoltage_V = 0.0f;
float startSoc = 0.0f;
float cutoffSoc = 0.0f;
float testedSocFraction = 0.0f;
float estimatedFullCapacity_mAh = 0.0f;

// v1.12: configurable cutoff. configuredCutoffVoltage_V is editable only while
// the test is not locked; activeCutoffVoltage_V is the frozen value for a test.
float configuredCutoffVoltage_V = DEFAULT_CUTOFF_V;
float activeCutoffVoltage_V = DEFAULT_CUTOFF_V;

// Raw INA diagnostics retained for browser/status inspection.
float lastBusVoltage_V = 0.0f;
float lastShuntVoltage_mV = 0.0f;
float lastSignedCurrent_mA = 0.0f;

bool fullStart = false;
bool estimateValid = false;
bool completedToCutoff = false;
bool chargeEnabled = false;
bool relayDischargeEnabled = false;
bool webServerStarted = false;
bool cutoffLocked = false;

uint32_t testStartedMs = 0;
uint32_t testElapsedMs = 0;
uint32_t lastSampleMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t lastReadyMs = 0;
uint32_t lastLogMs = 0;
uint32_t lastDonePageMs = 0;
uint8_t belowCutoffCount = 0;
uint8_t badLoadCount = 0;
uint8_t donePage = 0;
char stopReason[12] = "";

char webLog[WEB_LOG_LINES][WEB_LOG_LINE_LEN];
uint32_t webLogSequence[WEB_LOG_LINES] = {};
size_t webLogHead = 0;
size_t webLogCount = 0;
uint32_t webLogSequenceCounter = 0;

struct SocPoint {
  float voltage;
  float soc;
};

constexpr SocPoint SOC_TABLE[] = {
  {2.50f, 0.00f},
  {3.27f, 0.00f},
  {3.61f, 0.05f},
  {3.69f, 0.10f},
  {3.71f, 0.15f},
  {3.73f, 0.20f},
  {3.75f, 0.25f},
  {3.77f, 0.30f},
  {3.79f, 0.35f},
  {3.80f, 0.40f},
  {3.82f, 0.45f},
  {3.84f, 0.50f},
  {3.85f, 0.55f},
  {3.87f, 0.60f},
  {3.91f, 0.65f},
  {3.95f, 0.70f},
  {3.98f, 0.75f},
  {4.02f, 0.80f},
  {4.08f, 0.85f},
  {4.11f, 0.90f},
  {4.15f, 0.95f},
  {4.20f, 1.00f}
};

constexpr size_t SOC_TABLE_COUNT = sizeof(SOC_TABLE) / sizeof(SOC_TABLE[0]);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void pushWebLog(const char *line) {
  if (line == nullptr) return;

  snprintf(webLog[webLogHead], WEB_LOG_LINE_LEN, "%s", line);
  webLogSequence[webLogHead] = ++webLogSequenceCounter;
  webLogHead = (webLogHead + 1) % WEB_LOG_LINES;
  if (webLogCount < WEB_LOG_LINES) {
    ++webLogCount;
  }
}

void logLine(const char *line) {
  Serial.println(line);
  pushWebLog(line);
}

void logPrintf(const char *fmt, ...) {
  char line[WEB_LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  Serial.println(line);
  pushWebLog(line);
}

// ---------------------------------------------------------------------------
// Web / AP
// ---------------------------------------------------------------------------

const char *stateName() {
  switch (state) {
    case TesterState::READY:        return "READY";
    case TesterState::DISCHARGING: return "RUN";
    case TesterState::DONE:         return "DONE";
    case TesterState::NO_LOAD:      return "NO_LOAD";
    case TesterState::ERROR_STATE:  return "ERROR";
    default:                        return "UNKNOWN";
  }
}

void formatElapsed(uint32_t elapsedMs, char *out, size_t outSize) {
  const uint32_t totalSeconds = elapsedMs / 1000UL;
  const uint32_t hours = totalSeconds / 3600UL;
  const uint32_t minutes = (totalSeconds % 3600UL) / 60UL;
  const uint32_t seconds = totalSeconds % 60UL;
  snprintf(out, outSize, "%02lu:%02lu:%02lu",
           (unsigned long)hours,
           (unsigned long)minutes,
           (unsigned long)seconds);
}

void handleRoot() {
  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RENTECH RT-004</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:18px}
h1{font-size:1.3rem;margin:0 0 4px}.sub{color:#aaa;margin-bottom:14px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(120px,1fr));gap:8px;max-width:640px}
.card{background:#1d1d1d;border:1px solid #333;border-radius:8px;padding:10px}
.k{font-size:.75rem;color:#999}.v{font-size:1.15rem;font-weight:650;margin-top:2px}
.controls{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:14px 0;max-width:640px}
.cutoff{background:#1d1d1d;border:1px solid #333;border-radius:8px;padding:10px;max-width:620px}
input{background:#080808;color:#eee;border:1px solid #555;border-radius:6px;padding:8px;width:88px}
button{background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:6px;padding:8px 12px}
button:disabled,input:disabled{opacity:.45}
.msg{font-size:.85rem;color:#aaa;min-height:1.2em;margin-top:6px}
.warn{color:#d9b45d}
pre{background:#050505;border:1px solid #333;border-radius:8px;padding:10px;overflow:auto;height:42vh;white-space:pre-wrap;max-width:620px}
</style>
</head>
<body>
<h1>RENTECH Battery Tester RT-004</h1>
<div class="sub">Version 1.13 · local AP dashboard</div>

<div class="grid">
<div class="card"><div class="k">STATE</div><div id="state" class="v">-</div></div>
<div class="card"><div class="k">TIME</div><div id="time" class="v">-</div></div>
<div class="card"><div class="k">BATTERY</div><div id="voltage" class="v">-</div></div>
<div class="card"><div class="k">CURRENT</div><div id="current" class="v">-</div></div>
<div class="card"><div class="k">MEASURED</div><div id="capacity" class="v">-</div></div>
<div class="card"><div class="k">EST. FULL</div><div id="estimate" class="v">-</div></div>
<div class="card"><div class="k">TEST CUTOFF</div><div id="cutoff" class="v">-</div></div>
<div class="card"><div class="k">POWER PATH</div><div id="path" class="v">-</div></div>
</div>

<div class="cutoff">
  <div class="k">NEXT TEST CUTOFF</div>
  <div>
    <input id="cutoffInput" type="number" min="2.50" max="4.10" step="0.01" value="2.50">
    <button id="setCutoffBtn" onclick="setCutoff()">Set cutoff</button>
  </div>
  <div id="cutoffMsg" class="msg">Allowed: 2.50–4.10 V. Locked from START until the test leaves the active path.</div>
  <div class="msg warn">EST. FULL uses the voltage/SOC table and is UNVALIDATED until compared with real full-discharge runs.</div>
</div>

<div class="controls">
  <button onclick="copyLog()">Copy Log</button>
  <button onclick="saveLog()">Save Log</button>
  <button onclick="clearLog()">Clear web log</button>
  <span id="copyMsg" class="msg"></span>
</div>

<pre id="log">Connecting...</pre>

<script>
let logSeq=0;
let refreshBusy=false;

function byId(id){return document.getElementById(id)}
function showMsg(id,text){byId(id).textContent=text; setTimeout(()=>{if(byId(id).textContent===text)byId(id).textContent=''},2200)}

async function refreshStatus(){
  const r=await fetch('/status',{cache:'no-store'});
  if(!r.ok) throw new Error('status '+r.status);
  const s=await r.json();

  byId('state').textContent=s.state;
  byId('time').textContent=s.time;
  byId('voltage').textContent=s.voltage.toFixed(3)+' V';
  byId('current').textContent=s.current.toFixed(1)+' mA';
  byId('capacity').textContent=s.capacity.toFixed(1)+' mAh';

  if(s.completed_to_cutoff && !s.estimate_needed){
    byId('estimate').textContent=s.capacity.toFixed(1)+' mAh (measured)';
  }else if(s.estimate_valid){
    byId('estimate').textContent=s.estimate_full.toFixed(1)+' mAh';
  }else{
    byId('estimate').textContent='N/A';
  }

  byId('cutoff').textContent=s.display_cutoff.toFixed(2)+' V';
  byId('path').textContent='CHG '+(s.charge?'ON':'OFF')+' / RELAY '+(s.discharge?'DISCHARGE':'CHARGE');

  const input=byId('cutoffInput');
  if(document.activeElement!==input){
    input.value=s.configured_cutoff.toFixed(2);
  }
  input.disabled=s.cutoff_locked;
  byId('setCutoffBtn').disabled=s.cutoff_locked;
}

async function refreshLog(){
  const r=await fetch('/log?since='+encodeURIComponent(logSeq),{cache:'no-store'});
  if(!r.ok) throw new Error('log '+r.status);

  const reset=r.headers.get('X-Log-Reset')==='1';
  const nextSeq=Number(r.headers.get('X-Log-Seq')||logSeq);
  const text=await r.text();
  const le=byId('log');

  if(logSeq===0 && le.textContent==='Connecting...') le.textContent='';
  if(reset){
    if(le.textContent && !le.textContent.endsWith('\n')) le.textContent+='\n';
    le.textContent+='[WEB] LOG GAP: ESP ring buffer overrun; continuing with oldest available lines\n';
  }
  if(text){
    const nearBottom=(le.scrollHeight-le.scrollTop-le.clientHeight)<60;
    le.textContent+=text;
    if(nearBottom || logSeq===0) le.scrollTop=le.scrollHeight;
  }
  logSeq=nextSeq;
}

async function refresh(){
  if(refreshBusy) return;
  refreshBusy=true;
  try{
    await refreshStatus();
    await refreshLog();
  }catch(e){
    // Keep the last good screen. The next 1.5 s poll retries automatically.
  }finally{
    refreshBusy=false;
  }
}

async function setCutoff(){
  const value=byId('cutoffInput').value;
  try{
    const r=await fetch('/cutoff?value='+encodeURIComponent(value),{method:'POST',cache:'no-store'});
    const j=await r.json();
    if(!r.ok) throw new Error(j.error||('HTTP '+r.status));
    showMsg('cutoffMsg','Cutoff set to '+Number(j.cutoff).toFixed(2)+' V');
    await refreshStatus();
  }catch(e){
    showMsg('cutoffMsg','Set failed: '+e.message);
  }
}

async function copyLog(){
  try{
    const text=byId('log').textContent;
    if(!text){
      showMsg('copyMsg','Log is empty');
      return;
    }

    let copied=false;
    if(navigator.clipboard && navigator.clipboard.writeText){
      try{await navigator.clipboard.writeText(text); copied=true;}catch(e){}
    }

    if(!copied){
      const ta=document.createElement('textarea');
      ta.value=text;
      ta.setAttribute('readonly','');
      ta.style.position='fixed';
      ta.style.left='-9999px';
      document.body.appendChild(ta);
      ta.select();
      copied=document.execCommand('copy');
      document.body.removeChild(ta);
    }

    showMsg('copyMsg',copied?'Copied':'Copy failed');
  }catch(e){
    showMsg('copyMsg','Copy failed');
  }
}

function saveLog(){
  const text=byId('log').textContent;
  if(!text){
    showMsg('copyMsg','Log is empty');
    return;
  }

  const blob=new Blob([text],{type:'text/plain;charset=utf-8'});
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  a.href=url;
  a.download='RT-004_v1.13_log.txt';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}

async function clearLog(){
  try{
    await fetch('/clear',{method:'POST',cache:'no-store'});
    logSeq=0;
    byId('log').textContent='';
    await refreshLog();
  }catch(e){}
}

setInterval(refresh,1500);
refresh();
</script>
</body>
</html>
)HTML";

  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

bool resultUsesEstimate() {
  return !fullStart || (activeCutoffVoltage_V > (MIN_VOLTAGE + 0.005f));
}

void handleStatus() {
  char elapsed[16];
  formatElapsed(testElapsedMs, elapsed, sizeof(elapsed));

  const bool showPreviousTestCutoff =
      (state == TesterState::DISCHARGING) ||
      (state == TesterState::DONE && completedToCutoff);
  const float displayCutoff =
      showPreviousTestCutoff ? activeCutoffVoltage_V : configuredCutoffVoltage_V;

  char json[1024];
  snprintf(json, sizeof(json),
           "{\"model\":\"%s\",\"version\":\"%s\",\"state\":\"%s\","
           "\"voltage\":%.4f,\"bus\":%.4f,\"shunt_mV\":%.3f,"
           "\"current\":%.2f,\"signed_current\":%.2f,\"capacity\":%.3f,"
           "\"estimate_full\":%.3f,\"estimate_valid\":%s,\"estimate_needed\":%s,"
           "\"start_voltage\":%.4f,\"start_soc\":%.5f,\"cutoff_soc\":%.5f,"
           "\"tested_fraction\":%.5f,\"completed_to_cutoff\":%s,"
           "\"configured_cutoff\":%.3f,\"active_cutoff\":%.3f,\"display_cutoff\":%.3f,"
           "\"cutoff_locked\":%s,\"time\":\"%s\",\"charge\":%s,\"discharge\":%s,"
           "\"cutoff_count\":%u,\"cutoff_required\":%u,"
           "\"load_bad_count\":%u,\"load_bad_required\":%u}",
           DEVICE_MODEL,
           FW_VERSION,
           stateName(),
           batteryVoltage_V,
           lastBusVoltage_V,
           lastShuntVoltage_mV,
           dischargeCurrent_mA,
           lastSignedCurrent_mA,
           capacity_mAh,
           estimatedFullCapacity_mAh,
           estimateValid ? "true" : "false",
           resultUsesEstimate() ? "true" : "false",
           startVoltage_V,
           startSoc,
           cutoffSoc,
           testedSocFraction,
           completedToCutoff ? "true" : "false",
           configuredCutoffVoltage_V,
           activeCutoffVoltage_V,
           displayCutoff,
           cutoffLocked ? "true" : "false",
           elapsed,
           chargeEnabled ? "true" : "false",
           relayDischargeEnabled ? "true" : "false",
           (unsigned)belowCutoffCount,
           (unsigned)CUTOFF_CONFIRM_SAMPLES,
           (unsigned)badLoadCount,
           (unsigned)RUN_LOAD_FAIL_CONFIRM_SAMPLES);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

uint32_t parseSinceArg() {
  if (!server.hasArg("since")) {
    return 0;
  }

  const String raw = server.arg("since");
  char *endPtr = nullptr;
  const unsigned long value = strtoul(raw.c_str(), &endPtr, 10);
  if (endPtr == raw.c_str() || (endPtr != nullptr && *endPtr != '\0')) {
    return 0;
  }
  return (uint32_t)value;
}

void streamLog(bool incremental, bool attachment) {
  const uint32_t requestedSince = incremental ? parseSinceArg() : 0;

  const size_t oldest =
      (webLogHead + WEB_LOG_LINES - webLogCount) % WEB_LOG_LINES;
  const uint32_t oldestSeq =
      (webLogCount > 0) ? webLogSequence[oldest] : (webLogSequenceCounter + 1U);

  bool reset = false;
  uint32_t effectiveSince = requestedSince;

  if (incremental && webLogCount > 0 &&
      requestedSince != 0 &&
      requestedSince < (oldestSeq - 1U)) {
    reset = true;
    effectiveSince = 0;
  }

  if (attachment) {
    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"RT-004_v1.13_log.txt\"");
  }
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Log-Seq", String(webLogSequenceCounter));
  server.sendHeader("X-Log-Reset", reset ? "1" : "0");

  // Stream line-by-line: no temporary ~23 KB String on the ESP32.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");

  for (size_t n = 0; n < webLogCount; ++n) {
    const size_t idx = (oldest + n) % WEB_LOG_LINES;
    if (!incremental || effectiveSince == 0 ||
        webLogSequence[idx] > effectiveSince) {
      server.sendContent(webLog[idx]);
      server.sendContent("\n");
    }
  }

  // Final zero-length chunk for chunked HTTP/1.1 response.
  server.sendContent("");
}

void handleLog() {
  streamLog(true, false);
}

void handleFullLog() {
  streamLog(false, false);
}

void handleSaveLog() {
  streamLog(false, true);
}

void handleClearLog() {
  webLogHead = 0;
  webLogCount = 0;
  server.sendHeader("Cache-Control", "no-store");
  server.send(204, "text/plain", "");
}

void handleSetCutoff() {
  if (cutoffLocked || state == TesterState::DISCHARGING) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"cutoff is locked for the active test\"}");
    return;
  }

  if (!server.hasArg("value")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"missing value\"}");
    return;
  }

  String raw = server.arg("value");
  raw.trim();

  char *endPtr = nullptr;
  const float requested = strtof(raw.c_str(), &endPtr);

  if (endPtr == raw.c_str() ||
      (endPtr != nullptr && *endPtr != '\0') ||
      !isfinite(requested)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid number\"}");
    return;
  }

  if (requested < MIN_VOLTAGE ||
      requested > MAX_CONFIGURABLE_CUTOFF_V) {
    char json[160];
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"error\":\"allowed range %.2f..%.2f V\"}",
             MIN_VOLTAGE, MAX_CONFIGURABLE_CUTOFF_V);
    server.send(400, "application/json", json);
    return;
  }

  configuredCutoffVoltage_V = requested;
  logPrintf("[CUTOFF] configured=%.2fV (next test)",
            configuredCutoffVoltage_V);

  char json[96];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"cutoff\":%.3f}",
           configuredCutoffVoltage_V);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void setupWebServer() {
  webServerStarted = false;

  // v1.10 SOURCE OF TRUTH:
  // AP startup logic previously validated in MINI OLEG on the same ESP32-C3
  // Super Mini hardware. v1.12 removes only the accidental duplicate softAP()
  // call from v1.10; sequence/order otherwise remains unchanged.
  WiFi.disconnect(true, true);
  delay(150);
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_15dBm);

  const bool startOk = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (!startOk) {
    logLine("[AP] FAIL: known-good MINI OLEG softAP sequence returned false");
    return;
  }

  const IPAddress actualIp = WiFi.softAPIP();
  const String actualSsid = WiFi.softAPSSID();
  const String actualMac = WiFi.softAPmacAddress();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/log/full", HTTP_GET, handleFullLog);
  server.on("/log/save", HTTP_GET, handleSaveLog);
  server.on("/cutoff", HTTP_POST, handleSetCutoff);
  server.on("/clear", HTTP_POST, handleClearLog);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  webServerStarted = true;

  logPrintf("[AP] READY SSID=%s IP=%s MAC=%s",
            actualSsid.c_str(),
            actualIp.toString().c_str(),
            actualMac.c_str());
  logPrintf("[AP] PASSWORD=%s", AP_PASSWORD);
  logLine("[WEB] dashboard: http://192.168.4.1/");
}

void serviceDelay(uint32_t durationMs) {
  const uint32_t started = millis();
  while ((millis() - started) < durationMs) {
    if (webServerStarted) {
      server.handleClient();
    }
    delay(1);
  }
}

// Forward declaration used by safe power-path error handling.
void showError(const char *message);

// ---------------------------------------------------------------------------
// Power-path controls
// ---------------------------------------------------------------------------

void setRelayDischarge(bool enable) {
  const uint8_t offLevel = (RELAY_DISCHARGE_LEVEL == HIGH) ? LOW : HIGH;
  digitalWrite(PIN_RELAY, enable ? RELAY_DISCHARGE_LEVEL : offLevel);
  relayDischargeEnabled = enable;
}

void setChargeEnabled(bool enable) {
  const uint8_t offLevel = (CHARGE_ENABLE_LEVEL == HIGH) ? LOW : HIGH;
  digitalWrite(PIN_CHARGE_ENABLE, enable ? CHARGE_ENABLE_LEVEL : offLevel);
  chargeEnabled = enable;
}

void returnToChargeMode() {
  // Safety order: remove load first, then re-enable charger.
  setRelayDischarge(false);
  logLine("[RELAY] CHARGE / discharge OFF");
  serviceDelay(RELAY_TO_CHARGE_SETTLE_MS);
  setChargeEnabled(true);
  logLine("[CHARGE] ON / HW-373 IN+ enabled");
}

void isolateChargerForTest() {
  // Critical order: charger OFF first and let HW-373 input collapse.
  setChargeEnabled(false);
  logLine("[CHARGE] OFF / HW-373 IN+ isolated");
  serviceDelay(CHARGE_OFF_SETTLE_MS);
}

void connectDischargeLoad() {
  setRelayDischarge(true);
  logLine("[RELAY] DISCHARGE / relay ON");
}

void enterSafeError(const char *logMessage, const char *lcdMessage) {
  // Deliberate error policy: never leave the load connected.
  // Return to the autonomous TP4056 charge path, then report the fault.
  returnToChargeMode();
  cutoffLocked = false;
  state = TesterState::ERROR_STATE;

  if (logMessage != nullptr) {
    logLine(logMessage);
  }
  showError(lcdMessage);
}

// ---------------------------------------------------------------------------
// LCD / measurements
// ---------------------------------------------------------------------------

void lcdLine(uint8_t row, const char *text) {
  char padded[17];
  snprintf(padded, sizeof(padded), "%-16.16s", text);
  lcd.setCursor(0, row);
  lcd.print(padded);
}

bool readBattery(float &voltageV, float &currentAbs_mA, float *currentSigned_mA = nullptr) {
  const float busV = ina219.getBusVoltage_V();
  const float shuntmV = ina219.getShuntVoltage_mV();
  const float current = ina219.getCurrent_mA();

  if (!isfinite(busV) || !isfinite(shuntmV) || !isfinite(current)) {
    return false;
  }

  // Source-of-truth wiring:
  // BAT+ -> INA219 VIN+ -> INA219 VIN- -> discharge load.
  // Cell terminal voltage is bus voltage plus shunt drop.
  voltageV = busV + (shuntmV / 1000.0f);
  currentAbs_mA = fabsf(current);

  lastBusVoltage_V = busV;
  lastShuntVoltage_mV = shuntmV;
  lastSignedCurrent_mA = current;

  if (currentSigned_mA != nullptr) {
    *currentSigned_mA = current;
  }

  return true;
}

float estimateSocFromVoltage(float voltageV) {
  if (voltageV <= SOC_TABLE[0].voltage) {
    return SOC_TABLE[0].soc;
  }

  if (voltageV >= SOC_TABLE[SOC_TABLE_COUNT - 1].voltage) {
    return SOC_TABLE[SOC_TABLE_COUNT - 1].soc;
  }

  for (size_t i = 1; i < SOC_TABLE_COUNT; ++i) {
    if (voltageV <= SOC_TABLE[i].voltage) {
      const SocPoint &a = SOC_TABLE[i - 1];
      const SocPoint &b = SOC_TABLE[i];
      const float spanV = b.voltage - a.voltage;

      if (spanV <= 0.0f) {
        return a.soc;
      }

      const float t = (voltageV - a.voltage) / spanV;
      return a.soc + t * (b.soc - a.soc);
    }
  }

  return 1.0f;
}

// HD44780-compatible LCDs usually do not contain Ukrainian Cyrillic in ROM.
// Four CGRAM glyphs plus Latin look-alikes render "ПРИВІТ, ВЛАД!".
void showStartupGreeting() {
  static uint8_t chr_P[8] = {
    0b11111, 0b10001, 0b10001, 0b10001,
    0b10001, 0b10001, 0b10001, 0b00000
  };

  static uint8_t chr_Y[8] = {
    0b10001, 0b10011, 0b10101, 0b11001,
    0b10001, 0b10001, 0b10001, 0b00000
  };

  static uint8_t chr_L[8] = {
    0b00111, 0b01001, 0b01001, 0b10001,
    0b10001, 0b10001, 0b10001, 0b00000
  };

  static uint8_t chr_D[8] = {
    0b00110, 0b01001, 0b01001, 0b01001,
    0b01001, 0b11111, 0b10001, 0b00000
  };

  lcd.createChar(0, chr_P);
  lcd.createChar(1, chr_Y);
  lcd.createChar(2, chr_L);
  lcd.createChar(3, chr_D);

  lcd.clear();
  lcd.setCursor(1, 0);
  lcd.write((uint8_t)0); // П
  lcd.print('P');        // Р
  lcd.write((uint8_t)1); // И
  lcd.print('B');        // В
  lcd.print('I');        // І
  lcd.print('T');        // Т
  lcd.print(',');
  lcd.print(' ');
  lcd.print('B');        // В
  lcd.write((uint8_t)2); // Л
  lcd.print('A');        // А
  lcd.write((uint8_t)3); // Д
  lcd.print('!');

  lcd.setCursor(3, 1);
  lcd.print(DEVICE_MODEL);
  serviceDelay(1800);

  lcd.clear();
  lcdLine(0, "RENTECH");
  lcdLine(1, "Battery Tester");
  serviceDelay(1500);

  lcd.clear();
  lcdLine(0, DEVICE_MODEL);
  lcdLine(1, "Version 1.13");
  serviceDelay(1500);

  lcd.clear();
  lcdLine(0, "Firmware by");
  lcdLine(1, "Arduino.ua&KIRA");
  serviceDelay(1800);

  lcd.clear();
}

void showChargeStatus() {
  char line[17];
  snprintf(line, sizeof(line), "RED=CHG   %1.3fV", batteryVoltage_V);
  lcdLine(0, line);
  // TP4056 itself decides charge completion; voltage alone does not.
  lcdLine(1, "BLUE WHEN READY");
}

void showStarting() {
  lcdLine(0, "STARTING TEST...");
  lcdLine(1, "CHECKING LOAD...");
}

void showLowBattery() {
  lcdLine(0, "AT/BELOW CUTOFF");
  lcdLine(1, "SET LOWER/CHARGE");
}

void showNoLoad() {
  lcdLine(0, "ERROR: NO LOAD");
  lcdLine(1, "CHECK RELAY");
}

void showLoadLost() {
  lcdLine(0, "ERROR: LOAD LOST");
  lcdLine(1, "CHECK LOAD");
}

void showRunning() {
  char line[24];
  char elapsed[16];
  formatElapsed(testElapsedMs, elapsed, sizeof(elapsed));

  snprintf(line, sizeof(line), "%1.3fV %5.0fmA",
           batteryVoltage_V, dischargeCurrent_mA);
  lcdLine(0, line);

  snprintf(line, sizeof(line), "%.0fmAh %s", capacity_mAh, elapsed);
  lcdLine(1, line);
}

void showError(const char *message) {
  lcdLine(0, "ERROR / SAFE MODE");
  lcdLine(1, message);
}

uint8_t donePageCount() {
  if (!completedToCutoff) {
    return 2;
  }
  return resultUsesEstimate() ? 4 : 3;
}

bool donePageIsChargePage() {
  return donePage == (donePageCount() - 1);
}

void showDonePage() {
  char line[24];
  char elapsed[16];
  formatElapsed(testElapsedMs, elapsed, sizeof(elapsed));

  if (!completedToCutoff) {
    if (donePage == 0) {
      snprintf(line, sizeof(line), "%s %.0fmAh", stopReason, capacity_mAh);
      lcdLine(0, line);
      snprintf(line, sizeof(line), "%s CHARGE", elapsed);
      lcdLine(1, line);
    } else {
      showChargeStatus();
    }
    return;
  }

  // Classic full start -> full 2.50 V cutoff: measured capacity is the result.
  if (!resultUsesEstimate()) {
    if (donePage == 0) {
      lcdLine(0, "FULL START");
      snprintf(line, sizeof(line), "CAP: %.0fmAh", capacity_mAh);
      lcdLine(1, line);
    } else if (donePage == 1) {
      snprintf(line, sizeof(line), "TIME %s", elapsed);
      lcdLine(0, line);
      lcdLine(1, "RELAY=CHARGE");
    } else {
      showChargeStatus();
    }
    return;
  }

  // Partial start and/or early cutoff: keep measured and estimated values explicit.
  if (donePage == 0) {
    lcdLine(0, "MEASURED CAP");
    snprintf(line, sizeof(line), "MEAS: %.0fmAh", capacity_mAh);
    lcdLine(1, line);
  } else if (donePage == 1) {
    lcdLine(0, "EST. FULL CAP");
    if (estimateValid) {
      snprintf(line, sizeof(line), "EST: %.0fmAh", estimatedFullCapacity_mAh);
      lcdLine(1, line);
    } else {
      lcdLine(1, "EST: N/A");
    }
  } else if (donePage == 2) {
    snprintf(line, sizeof(line), "CUT %.2fV", activeCutoffVoltage_V);
    lcdLine(0, line);
    snprintf(line, sizeof(line), "TIME %s", elapsed);
    lcdLine(1, line);
  } else {
    showChargeStatus();
  }
}

// ---------------------------------------------------------------------------
// Button / test logic
// ---------------------------------------------------------------------------

uint8_t readStartButton() {
  static uint8_t lastRaw = HIGH;
  static uint8_t stableState = HIGH;
  static uint32_t changedMs = 0;
  static uint32_t pressStartedMs = 0;
  static bool longPressReported = false;

  const uint8_t raw = digitalRead(PIN_START);
  const uint32_t now = millis();

  if (raw != lastRaw) {
    lastRaw = raw;
    changedMs = now;
  }

  if ((now - changedMs) >= BUTTON_DEBOUNCE_MS && raw != stableState) {
    stableState = raw;

    if (stableState == LOW) {
      pressStartedMs = now;
      longPressReported = false;
    } else if (!longPressReported) {
      return BUTTON_EVENT_SHORT;
    }
  }

  if (stableState == LOW &&
      !longPressReported &&
      (now - pressStartedMs) >= BUTTON_LONG_PRESS_MS) {
    longPressReported = true;
    return BUTTON_EVENT_LONG;
  }

  return BUTTON_EVENT_NONE;
}

void calculateEstimatedFullCapacity() {
  estimateValid = false;
  estimatedFullCapacity_mAh = 0.0f;
  cutoffSoc = 0.0f;
  testedSocFraction = 0.0f;

  if (!completedToCutoff) {
    return;
  }

  startSoc = fullStart ? 1.0f : estimateSocFromVoltage(startVoltage_V);
  cutoffSoc = estimateSocFromVoltage(activeCutoffVoltage_V);
  testedSocFraction = startSoc - cutoffSoc;

  if (!isfinite(startSoc) || !isfinite(cutoffSoc) ||
      !isfinite(testedSocFraction) || testedSocFraction <= 0.0f) {
    logPrintf("[EST] unavailable: invalid SOC window start=%.3f cutoff=%.3f fraction=%.3f",
              startSoc, cutoffSoc, testedSocFraction);
    return;
  }

  if (testedSocFraction < MIN_TESTED_SOC_FRACTION_FOR_ESTIMATE) {
    logPrintf("[EST] unavailable: tested SOC window %.1f%% < %.1f%% minimum",
              testedSocFraction * 100.0f,
              MIN_TESTED_SOC_FRACTION_FOR_ESTIMATE * 100.0f);
    return;
  }

  if (!(capacity_mAh > 0.0f) || !isfinite(capacity_mAh)) {
    logLine("[EST] unavailable: measured capacity is not positive/finite");
    return;
  }

  // UNVALIDATED MODEL:
  // Qfull = Qmeasured / (SOCstart - SOCcutoff)
  // SOC is estimated from the existing voltage/SOC lookup table.
  estimatedFullCapacity_mAh = capacity_mAh / testedSocFraction;
  estimateValid =
      isfinite(estimatedFullCapacity_mAh) &&
      estimatedFullCapacity_mAh > 0.0f;

  if (estimateValid) {
    logPrintf("[EST] MODEL=UNVALIDATED Vstart=%.3fV SOCstart=%.1f%% cutoff=%.2fV SOCcut=%.1f%% tested=%.1f%% MEAS=%.1fmAh EST_FULL=%.1fmAh",
              startVoltage_V,
              startSoc * 100.0f,
              activeCutoffVoltage_V,
              cutoffSoc * 100.0f,
              testedSocFraction * 100.0f,
              capacity_mAh,
              estimatedFullCapacity_mAh);
  }
}

void stopTest(const char *reason) {
  // v1.09 safety order: load OFF -> relay CHARGE -> charger ON.
  returnToChargeMode();
  cutoffLocked = false;

  testElapsedMs = millis() - testStartedMs;
  state = TesterState::DONE;

  snprintf(stopReason, sizeof(stopReason), "%s", reason);
  completedToCutoff = (strcmp(reason, "CUTOFF") == 0);

  if (completedToCutoff && resultUsesEstimate()) {
    calculateEstimatedFullCapacity();
  } else {
    // Classic FULL-start -> 2.50 V cutoff is a directly measured result.
    // ABORT/non-cutoff stops are also never extrapolated.
    estimateValid = false;
    estimatedFullCapacity_mAh = 0.0f;
    cutoffSoc = completedToCutoff ? estimateSocFromVoltage(activeCutoffVoltage_V) : 0.0f;
    testedSocFraction = completedToCutoff ? 1.0f : 0.0f;
  }

  logPrintf("[STOP] reason=%s V=%.3fV I=%.1fmA C=%.1fmAh T=%lums cutoff=%.2fV",
            reason,
            batteryVoltage_V,
            dischargeCurrent_mA,
            capacity_mAh,
            (unsigned long)testElapsedMs,
            activeCutoffVoltage_V);

  donePage = 0;
  lastDonePageMs = millis();
  showDonePage();
}

LoadCheckResult verifyDischargeLoad() {
  uint8_t goodSamples = 0;
  float sumSignedCurrent = 0.0f;

  serviceDelay(LOAD_RELAY_SETTLE_MS);

  for (uint8_t n = 0; n < LOAD_VERIFY_SAMPLES; ++n) {
    float v = 0.0f;
    float iAbs = 0.0f;
    float iSigned = 0.0f;

    if (!readBattery(v, iAbs, &iSigned)) {
      logLine("[ERROR] INA219 read failed during load verification");
      return LoadCheckResult::READ_ERROR;
    }

    batteryVoltage_V = v;
    dischargeCurrent_mA = (iSigned > 0.0f) ? iSigned : 0.0f;
    sumSignedCurrent += iSigned;

    if (iSigned <= REVERSE_CURRENT_FAULT_MA) {
      logPrintf("[LOADCHK] REVERSE CURRENT V=%.3fV signedI=%.1fmA threshold=%.1fmA",
                v, iSigned, REVERSE_CURRENT_FAULT_MA);
      return LoadCheckResult::REVERSE_CURRENT;
    }

    if (iSigned >= MIN_LOAD_CURRENT_MA) {
      ++goodSamples;
    }

    logPrintf("[LOADCHK] %u/%u V=%.3fV signedI=%.1fmA pass=%u",
              (unsigned)(n + 1),
              (unsigned)LOAD_VERIFY_SAMPLES,
              v,
              iSigned,
              (unsigned)goodSamples);

    if ((n + 1) < LOAD_VERIFY_SAMPLES) {
      serviceDelay(LOAD_VERIFY_INTERVAL_MS);
    }
  }

  const float avgSignedCurrent = sumSignedCurrent / (float)LOAD_VERIFY_SAMPLES;
  logPrintf("[LOADCHK] result: good=%u/%u avgSignedI=%.1fmA threshold=%.1fmA",
            (unsigned)goodSamples,
            (unsigned)LOAD_VERIFY_SAMPLES,
            avgSignedCurrent,
            MIN_LOAD_CURRENT_MA);

  return (goodSamples >= LOAD_VERIFY_REQUIRED)
      ? LoadCheckResult::PASS
      : LoadCheckResult::NO_LOAD;
}

void updateChargeVoltage(bool redraw);

void startTest() {
  capacity_mAh = 0.0f;
  dischargeCurrent_mA = 0.0f;
  estimatedFullCapacity_mAh = 0.0f;
  estimateValid = false;
  completedToCutoff = false;
  cutoffSoc = 0.0f;
  testedSocFraction = 0.0f;
  belowCutoffCount = 0;
  badLoadCount = 0;
  stopReason[0] = '\0';

  // Freeze the configured cutoff before any serviceDelay() can process web requests.
  activeCutoffVoltage_V = configuredCutoffVoltage_V;
  cutoffLocked = true;

  logLine("[TEST] START requested");
  logPrintf("[CUTOFF] locked=%.2fV for this test", activeCutoffVoltage_V);
  showStarting();

  // v1.09: remove charger influence BEFORE capturing Vstart.
  // Relay is still in CHARGE/NC topology here; only HW-373 IN+ is disabled.
  isolateChargerForTest();

  float v = 0.0f;
  float iAbs = 0.0f;
  float iSigned = 0.0f;

  if (!readBattery(v, iAbs, &iSigned)) {
    enterSafeError("[ERROR] INA219 read failed after CHARGE OFF",
                   "INA219 read fail");
    return;
  }

  batteryVoltage_V = v;
  startVoltage_V = v;

  logPrintf("[STARTCHK] CHARGE=OFF V=%.3fV signedI=%.1fmA",
            startVoltage_V, iSigned);

  if (startVoltage_V <= activeCutoffVoltage_V) {
    returnToChargeMode();
    cutoffLocked = false;
    state = TesterState::READY;
    logPrintf("[START BLOCKED] V=%.3fV <= cutoff %.2fV",
              startVoltage_V, activeCutoffVoltage_V);
    showLowBattery();
    serviceDelay(1600);
    showChargeStatus();
    lastReadyMs = millis();
    return;
  }

  fullStart = (startVoltage_V >= FULL_START_MIN_V);
  startSoc = fullStart ? 1.0f : estimateSocFromVoltage(startVoltage_V);

  logPrintf("[START] Vstart=%.3fV class=%s SOCest=%.1f%%",
            startVoltage_V,
            fullStart ? "FULL" : "PARTIAL",
            startSoc * 100.0f);

  // Only now connect the physical discharge load.
  connectDischargeLoad();

  // Timer/mAh do NOT start until the physical positive discharge current is confirmed.
  const LoadCheckResult loadCheck = verifyDischargeLoad();

  if (loadCheck == LoadCheckResult::READ_ERROR) {
    enterSafeError("[ERROR] load verification aborted by INA219 read failure",
                   "INA219 read fail");
    return;
  }

  if (loadCheck == LoadCheckResult::REVERSE_CURRENT) {
    enterSafeError("[ERROR] reverse current during load verification",
                   "CURRENT DIR ERR");
    return;
  }

  if (loadCheck == LoadCheckResult::NO_LOAD) {
    returnToChargeMode();
    cutoffLocked = false;
    state = TesterState::NO_LOAD;
    logLine("[ERROR] NO LOAD: positive discharge current not confirmed");
    showNoLoad();
    return;
  }

  if (batteryVoltage_V <= activeCutoffVoltage_V) {
    returnToChargeMode();
    cutoffLocked = false;
    state = TesterState::READY;
    logPrintf("[START BLOCKED] loaded V=%.3fV <= cutoff %.2fV",
              batteryVoltage_V, activeCutoffVoltage_V);
    showLowBattery();
    serviceDelay(1600);

    float chargeV = 0.0f;
    float chargeI = 0.0f;
    if (readBattery(chargeV, chargeI)) {
      batteryVoltage_V = chargeV;
    }
    showChargeStatus();
    lastReadyMs = millis();
    return;
  }

  testStartedMs = millis();
  lastSampleMs = testStartedMs;
  lastDisplayMs = 0;
  lastLogMs = testStartedMs;
  testElapsedMs = 0;
  state = TesterState::DISCHARGING;

  logPrintf("[TEST] RUNNING V=%.3fV I=%.1fmA cutoff=%.2fV CHARGE=OFF",
            batteryVoltage_V, dischargeCurrent_mA, activeCutoffVoltage_V);

  showRunning();
}

void updateChargeVoltage(bool redraw) {
  float v = 0.0f;
  float i = 0.0f;

  if (!readBattery(v, i)) {
    enterSafeError("[ERROR] INA219 read failed in CHARGE mode",
                   "INA219 read fail");
    return;
  }

  batteryVoltage_V = v;

  if (redraw) {
    showChargeStatus();
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  const uint32_t serialWaitStartedMs = millis();
  while (!Serial && (millis() - serialWaitStartedMs) < 1500) {
    delay(10);
  }

  // Safe hardware defaults BEFORE enabling the pins:
  // - charger OFF
  // - relay in CHARGE / discharge OFF state
  const uint8_t chargeOffLevel = (CHARGE_ENABLE_LEVEL == HIGH) ? LOW : HIGH;
  digitalWrite(PIN_CHARGE_ENABLE, chargeOffLevel);
  pinMode(PIN_CHARGE_ENABLE, OUTPUT);
  setChargeEnabled(false);

  const uint8_t relayOffLevel = (RELAY_DISCHARGE_LEVEL == HIGH) ? LOW : HIGH;
  digitalWrite(PIN_RELAY, relayOffLevel);
  pinMode(PIN_RELAY, OUTPUT);
  setRelayDischarge(false);

  logLine("");
  logLine("=== RENTECH Battery Tester RT-004 v1.13 ===");
  logLine("[BOOT] Serial logging ENABLED @ 115200");
  logLine("[FW] RENTECH Battery Tester RT-004 v1.13");
  logLine("[FW] Firmware by Arduino.ua&KIRA");
  logPrintf("[BOOT] SDA=GPIO%u SCL=GPIO%u START=GPIO%u CHARGE=GPIO%u RELAY=GPIO%u",
            PIN_SDA, PIN_SCL, PIN_START, PIN_CHARGE_ENABLE, PIN_RELAY);
  logPrintf("[BOOT] LCD=0x%02X INA219=0x%02X cutoffDefault=%.2fV cutoffRange=%.2f..%.2fV FULL=%.3fV loadMin=%.0fmA",
            LCD_ADDRESS,
            INA219_ADDRESS_C3,
            DEFAULT_CUTOFF_V,
            MIN_VOLTAGE,
            MAX_CONFIGURABLE_CUTOFF_V,
            FULL_START_TARGET_V,
            MIN_LOAD_CURRENT_MA);
  logLine("[CHARGE] safe boot default: OFF");
  logLine("[RELAY] safe boot default: CHARGE / discharge OFF");

  setupWebServer();

  pinMode(PIN_START, INPUT_PULLUP);
  logLine("[BUTTON] GPIO3 INPUT_PULLUP; short=START, hold 1.5s=STOP");

  Wire.setPins(PIN_SDA, PIN_SCL);
  Wire.begin();
  Wire.setClock(100000);
  logLine("[I2C] bus started @ 100kHz");

  lcd.init();
  lcd.backlight();
  lcd.clear();

  showStartupGreeting();
  logLine("[LCD] LCD1602 initialized at 0x27; RT-004 Version 1.13 splash shown");

  if (!ina219.begin(&Wire)) {
    enterSafeError("[ERROR] INA219 not found at 0x40",
                   "INA219 not found");
    return;
  }
  logLine("[INA219] found at 0x40");

  ina219.setCalibration_32V_1A();
  logLine("[INA219] calibration: 32V / 1A");

  // Normal idle mode after successful hardware init: relay CHARGE, charger ON.
  setRelayDischarge(false);
  setChargeEnabled(true);
  logLine("[CHARGE] ON / HW-373 IN+ enabled");

  updateChargeVoltage(false);
  if (state == TesterState::ERROR_STATE) {
    return;
  }

  state = TesterState::READY;
  showChargeStatus();
  lastReadyMs = millis();

  logPrintf("[READY] CHARGE mode V=%.3fV; configured cutoff=%.2fV",
            batteryVoltage_V, configuredCutoffVoltage_V);
}

void loop() {
  if (webServerStarted) {
    server.handleClient();
  }

  const uint8_t button = readStartButton();

  if (button == BUTTON_EVENT_SHORT &&
      state != TesterState::DISCHARGING &&
      state != TesterState::ERROR_STATE) {
    logLine("[BUTTON] SHORT -> START");
    startTest();
    return;
  } else if (button == BUTTON_EVENT_SHORT &&
             state == TesterState::DISCHARGING) {
    logLine("[BUTTON] SHORT ignored while test is running");
  }

  if (button == BUTTON_EVENT_LONG &&
      state == TesterState::DISCHARGING) {
    logLine("[BUTTON] LONG -> ABORT");
    stopTest("ABORT");
    return;
  } else if (button == BUTTON_EVENT_LONG) {
    logLine("[BUTTON] LONG ignored outside active test");
  }

  const uint32_t now = millis();

  if (state == TesterState::READY) {
    if ((now - lastReadyMs) >= READY_INTERVAL_MS) {
      lastReadyMs = now;
      updateChargeVoltage(true);
    }
    delay(1);
    return;
  }

  if (state == TesterState::NO_LOAD) {
    delay(1);
    return;
  }

  if (state == TesterState::DONE) {
    if ((now - lastReadyMs) >= READY_INTERVAL_MS) {
      lastReadyMs = now;
      updateChargeVoltage(donePageIsChargePage());
      if (state == TesterState::ERROR_STATE) {
        return;
      }
    }

    if ((now - lastDonePageMs) >= DONE_PAGE_INTERVAL_MS) {
      lastDonePageMs = now;
      donePage = (donePage + 1) % donePageCount();
      showDonePage();
    }

    delay(1);
    return;
  }

  if (state != TesterState::DISCHARGING) {
    delay(1);
    return;
  }

  if ((now - lastSampleMs) >= SAMPLE_INTERVAL_MS) {
    const uint32_t dtMs = now - lastSampleMs;
    lastSampleMs = now;

    float v = 0.0f;
    float iAbs = 0.0f;
    float iSigned = 0.0f;

    if (!readBattery(v, iAbs, &iSigned)) {
      enterSafeError("[ERROR] INA219 read failed during discharge",
                     "INA219 read fail");
      return;
    }

    batteryVoltage_V = v;
    testElapsedMs = now - testStartedMs;

    // v1.09: in the verified INA orientation, real discharge current is positive.
    // Never integrate fabs(current): a wrong-direction current must not become fake capacity.
    if (iSigned <= REVERSE_CURRENT_FAULT_MA) {
      logPrintf("[ERROR] REVERSE CURRENT: signedI=%.1fmA threshold=%.1fmA",
                iSigned, REVERSE_CURRENT_FAULT_MA);
      enterSafeError(nullptr, "CURRENT DIR ERR");
      return;
    }

    dischargeCurrent_mA = (iSigned > 0.0f) ? iSigned : 0.0f;
    capacity_mAh += dischargeCurrent_mA * ((float)dtMs / 3600000.0f);

    if (iSigned < MIN_LOAD_CURRENT_MA) {
      if (badLoadCount < 255) {
        ++badLoadCount;
      }
    } else {
      badLoadCount = 0;
    }

    if (badLoadCount >= RUN_LOAD_FAIL_CONFIRM_SAMPLES) {
      returnToChargeMode();
      cutoffLocked = false;
      state = TesterState::NO_LOAD;
      logPrintf("[ERROR] LOAD LOST: signedI=%.1fmA low=%u/%u C=%.1fmAh T=%lums",
                iSigned,
                (unsigned)badLoadCount,
                (unsigned)RUN_LOAD_FAIL_CONFIRM_SAMPLES,
                capacity_mAh,
                (unsigned long)testElapsedMs);
      showLoadLost();
      return;
    }

    if (batteryVoltage_V <= activeCutoffVoltage_V) {
      if (belowCutoffCount < 255) {
        ++belowCutoffCount;
      }
    } else {
      belowCutoffCount = 0;
    }

    if (belowCutoffCount >= CUTOFF_CONFIRM_SAMPLES) {
      stopTest("CUTOFF");
      return;
    }
  }

  if ((now - lastLogMs) >= LOG_INTERVAL_MS) {
    lastLogMs = now;

    char elapsed[16];
    formatElapsed(testElapsedMs, elapsed, sizeof(elapsed));

    logPrintf("[RUN] T=%s V=%.3fV I=%.1fmA rawI=%.1fmA C=%.1fmAh cutoff=%.2fV hit=%u/%u load=%u/%u CHG=%s",
              elapsed,
              batteryVoltage_V,
              dischargeCurrent_mA,
              lastSignedCurrent_mA,
              capacity_mAh,
              activeCutoffVoltage_V,
              belowCutoffCount,
              CUTOFF_CONFIRM_SAMPLES,
              badLoadCount,
              RUN_LOAD_FAIL_CONFIRM_SAMPLES,
              chargeEnabled ? "ON" : "OFF");
  }

  if ((now - lastDisplayMs) >= DISPLAY_INTERVAL_MS) {
    lastDisplayMs = now;
    showRunning();
  }
}
