#include "led_config_web.h"

#include <ArduinoJson.h>
#include <string.h>

namespace {
constexpr char kLedUiStyle[] PROGMEM =
    "<style>.led-meta{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}.led-meta p{margin:.2rem 0}.led-status{display:none;margin:.75rem 0}.led-status.show{display:block}.led-grid{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}.led-grid-3{display:grid;grid-template-columns:repeat(3,1fr);gap:.5rem}.led-stack{display:grid;gap:.5rem}.led-state{margin-top:.9rem;border:1px solid #d5d5d5;border-radius:.85rem;padding:.35rem .8rem;background:#fff}.led-state summary{cursor:pointer;font-weight:700}.led-inline{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap}.led-inline>*{flex:1 1 auto}.led-value{font-weight:700}.led-actions{display:flex;gap:.75rem;flex-wrap:wrap;margin-top:1rem}.led-actions button{flex:1 1 12rem}.led-note{font-size:.9rem;color:#555}.led-sep{border:none;border-top:1px solid #ddd;margin:1rem 0}.led-copy-row{display:flex;gap:.5rem;align-items:end;flex-wrap:wrap}.led-copy-row select,.led-copy-row button{flex:1 1 10rem}@media(max-width:640px){.led-meta,.led-grid,.led-grid-3{grid-template-columns:1fr}}</style>";

constexpr char kLedUiScript[] PROGMEM = R"LEDSCRIPT(
<script>
(function(){
  var app = document.getElementById('led-config-app');
  if (!app) { return; }
  var state = null;
  function esc(v){return String(v).replace(/[&<>"']/g,function(ch){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[ch];});}
  function clamp(v,min,max,fallback){ var n = parseInt(v,10); if (isNaN(n)) { n = fallback; } if (n < min) n = min; if (n > max) n = max; return n; }
  function rgbToHex(rgb){ function h(v){ var s = clamp(v,0,255,0).toString(16).toUpperCase(); return s.length < 2 ? '0' + s : s; } return '#' + h(rgb.r) + h(rgb.g) + h(rgb.b); }
  function hexToRgb(hex){ var clean = String(hex || '').replace('#',''); if (clean.length !== 6) { return {r:255,g:255,b:255}; } return {r:parseInt(clean.slice(0,2),16),g:parseInt(clean.slice(2,4),16),b:parseInt(clean.slice(4,6),16)}; }
  function copyStyle(src){ return JSON.parse(JSON.stringify(src)); }
  function modeLabel(key){ return state && state.modeLabels && state.modeLabels[key] ? state.modeLabels[key] : key; }
  function showMessage(text,isError){
    var box = document.getElementById('led-config-status');
    if (!box) { return; }
    if (!text) { box.className = 'status led-status'; box.textContent = ''; return; }
    box.className = 'status led-status show ' + (isError ? 'err' : 'ok');
    box.textContent = text;
  }
  function speedBounds(mode){
    if (mode === 'flash') { return {min:50,max:10000}; }
    if (mode === 'sine' || mode === 'breathe') { return {min:200,max:20000}; }
    return {min:20,max:10000};
  }
  function updateFrequency(key){
    var speed = document.getElementById('led-speed-' + key);
    var speedRange = document.getElementById('led-speed-range-' + key);
    var freq = document.getElementById('led-freq-' + key);
    var mode = document.getElementById('led-mode-' + key);
    var dutyWrap = document.getElementById('led-duty-wrap-' + key);
    if (!speed || !freq || !mode || !speedRange) { return; }
    var bounds = speedBounds(mode.value);
    speed.min = bounds.min;
    speed.max = bounds.max;
    speedRange.min = bounds.min;
    speedRange.max = bounds.max;
    speed.value = clamp(speed.value, bounds.min, bounds.max, bounds.min);
    speedRange.value = speed.value;
    var hz = 1000 / Math.max(parseInt(speed.value,10) || 1, 1);
    freq.textContent = hz.toFixed(hz >= 1 ? 2 : 3) + ' Hz';
    dutyWrap.style.display = mode.value === 'flash' ? 'block' : 'none';
  }
  function syncColorFromPicker(key){
    var rgb = hexToRgb(document.getElementById('led-color-' + key).value);
    document.getElementById('led-r-' + key).value = rgb.r;
    document.getElementById('led-g-' + key).value = rgb.g;
    document.getElementById('led-b-' + key).value = rgb.b;
  }
  function syncColorFromRgb(key){
    var rgb = {
      r: clamp(document.getElementById('led-r-' + key).value, 0, 255, 255),
      g: clamp(document.getElementById('led-g-' + key).value, 0, 255, 255),
      b: clamp(document.getElementById('led-b-' + key).value, 0, 255, 255)
    };
    document.getElementById('led-color-' + key).value = rgbToHex(rgb);
  }
  function syncRangeValue(id, outId, min, max, fallback){
    var input = document.getElementById(id);
    var out = document.getElementById(outId);
    if (!input || !out) { return; }
    var value = clamp(input.value, min, max, fallback);
    input.value = value;
    out.textContent = value;
  }
  function render(){
    if (!state) { return; }
    var html = '';
    html += '<div class="led-meta"><div><p><strong>' + esc(app.dataset.networkLabel || 'Network') + ':</strong> <code>' + esc(app.dataset.networkValue || '') + '</code></p></div>';
    html += '<div><p><strong>' + esc(app.dataset.locationLabel || 'Portal') + ':</strong> <code>' + esc(app.dataset.locationValue || '') + '</code></p></div></div>';
    html += '<p class="led-note">Changes apply immediately after validation and save. Unknown states use the dedicated fallback style.</p>';
    html += '<div id="led-config-status" class="status led-status"></div>';
    html += '<label for="led-max-brightness">Max Brightness <span id="led-max-brightness-value" class="led-value">' + esc(state.maxBrightness) + '</span></label>';
    html += '<input id="led-max-brightness" type="range" min="1" max="255" value="' + esc(state.maxBrightness) + '">';
    html += '<div id="led-state-list"></div>';
    html += '<div class="led-actions"><button type="button" id="led-apply">Apply LED Settings</button><button type="button" id="led-reset">Reset To Defaults</button></div>';
    app.innerHTML = html;
    var list = document.getElementById('led-state-list');
    var order = state.stateOrder || [];
    var cards = '';
    for (var i = 0; i < order.length; ++i) {
      var key = order[i];
      var style = state.states[key];
      var label = state.stateLabels[key] || key;
      var modeOptions = '';
      for (var m = 0; m < state.modeOrder.length; ++m) {
        var modeKey = state.modeOrder[m];
        modeOptions += '<option value="' + esc(modeKey) + '"' + (style.mode === modeKey ? ' selected' : '') + '>' + esc(modeLabel(modeKey)) + '</option>';
      }
      var copyOptions = '<option value="">Copy settings from...</option>';
      for (var c = 0; c < order.length; ++c) {
        var srcKey = order[c];
        if (srcKey === key) { continue; }
        copyOptions += '<option value="' + esc(srcKey) + '">' + esc(state.stateLabels[srcKey] || srcKey) + '</option>';
      }
      cards += '<details class="led-state"' + (i === 0 ? ' open' : '') + '><summary>' + esc(label) + '</summary>';
      cards += '<div class="led-grid"><div><label for="led-mode-' + esc(key) + '">Mode</label><select id="led-mode-' + esc(key) + '" data-state-key="' + esc(key) + '">' + modeOptions + '</select></div>';
      cards += '<div><label for="led-baseline-' + esc(key) + '">Baseline Brightness <span id="led-baseline-value-' + esc(key) + '" class="led-value">' + esc(style.baselineBrightness) + '</span></label><input id="led-baseline-' + esc(key) + '" data-baseline-key="' + esc(key) + '" type="range" min="0" max="255" value="' + esc(style.baselineBrightness) + '"></div></div>';
      cards += '<div class="led-grid"><div><label for="led-color-' + esc(key) + '">Color</label><input id="led-color-' + esc(key) + '" data-color-key="' + esc(key) + '" type="color" value="' + rgbToHex(style.color) + '"></div>';
      cards += '<div class="led-grid-3"><div><label for="led-r-' + esc(key) + '">R</label><input id="led-r-' + esc(key) + '" data-rgb-key="' + esc(key) + '" data-rgb-channel="r" type="number" min="0" max="255" value="' + esc(style.color.r) + '"></div>';
      cards += '<div><label for="led-g-' + esc(key) + '">G</label><input id="led-g-' + esc(key) + '" data-rgb-key="' + esc(key) + '" data-rgb-channel="g" type="number" min="0" max="255" value="' + esc(style.color.g) + '"></div>';
      cards += '<div><label for="led-b-' + esc(key) + '">B</label><input id="led-b-' + esc(key) + '" data-rgb-key="' + esc(key) + '" data-rgb-channel="b" type="number" min="0" max="255" value="' + esc(style.color.b) + '"></div></div></div>';
      cards += '<div class="led-stack"><label for="led-speed-range-' + esc(key) + '">Period (ms)</label><input id="led-speed-range-' + esc(key) + '" data-speed-range-key="' + esc(key) + '" type="range" min="20" max="20000" value="' + esc(style.speedMs) + '"><div class="led-inline"><input id="led-speed-' + esc(key) + '" data-speed-key="' + esc(key) + '" type="number" min="20" max="20000" value="' + esc(style.speedMs) + '"><small>Frequency: <span id="led-freq-' + esc(key) + '"></span></small></div></div>';
      cards += '<div id="led-duty-wrap-' + esc(key) + '"><label for="led-duty-' + esc(key) + '">Flash Duty % <span id="led-duty-value-' + esc(key) + '" class="led-value">' + esc(style.flashDutyPct) + '</span></label><input id="led-duty-' + esc(key) + '" data-duty-key="' + esc(key) + '" type="range" min="1" max="99" value="' + esc(style.flashDutyPct) + '"></div>';
      cards += '<div class="led-copy-row"><div><label for="led-copy-' + esc(key) + '">Copy Settings</label><select id="led-copy-' + esc(key) + '">' + copyOptions + '</select></div><button type="button" data-copy-target="' + esc(key) + '">Copy</button></div>';
      cards += '</details>';
    }
    list.innerHTML = cards;
    syncRangeValue('led-max-brightness', 'led-max-brightness-value', 1, 255, state.maxBrightness);
    for (var j = 0; j < order.length; ++j) {
      var stateKey = order[j];
      syncRangeValue('led-baseline-' + stateKey, 'led-baseline-value-' + stateKey, 0, 255, state.states[stateKey].baselineBrightness);
      syncRangeValue('led-duty-' + stateKey, 'led-duty-value-' + stateKey, 1, 99, state.states[stateKey].flashDutyPct);
      updateFrequency(stateKey);
    }
  }
  function collectState(key){
    return {
      mode: document.getElementById('led-mode-' + key).value,
      color: {
        r: clamp(document.getElementById('led-r-' + key).value, 0, 255, 255),
        g: clamp(document.getElementById('led-g-' + key).value, 0, 255, 255),
        b: clamp(document.getElementById('led-b-' + key).value, 0, 255, 255)
      },
      baselineBrightness: clamp(document.getElementById('led-baseline-' + key).value, 0, 255, 0),
      speedMs: clamp(document.getElementById('led-speed-' + key).value, 20, 20000, 1000),
      flashDutyPct: clamp(document.getElementById('led-duty-' + key).value, 1, 99, 50)
    };
  }
  function collectConfig(){
    var payload = { maxBrightness: clamp(document.getElementById('led-max-brightness').value, 1, 255, 255), states: {} };
    if (app.dataset.csrf) { payload.csrf = app.dataset.csrf; }
    for (var i = 0; i < state.stateOrder.length; ++i) {
      var key = state.stateOrder[i];
      payload.states[key] = collectState(key);
    }
    return payload;
  }
  function applyResponse(json){
    if (json && json.ok) {
      if (json.config) { state = json.config; render(); }
      showMessage(json.message || 'LED settings saved.', false);
      return true;
    }
    var extra = '';
    if (json && json.state) { extra = ' (' + json.state + ')'; }
    showMessage((json && json.message ? json.message : 'LED settings update failed.') + extra, true);
    return false;
  }
  function requestJson(url, payload){
    return fetch(url, {method:'POST', headers:{'Content-Type':'application/json','Accept':'application/json'}, credentials:'same-origin', body:JSON.stringify(payload)})
      .then(function(res){ return res.json().catch(function(){ return {ok:false,message:'Unexpected server response.'}; }); });
  }
  document.addEventListener('input', function(ev){
    var t = ev.target;
    if (!t || !app.contains(t)) { return; }
    if (t.id === 'led-max-brightness') {
      syncRangeValue('led-max-brightness', 'led-max-brightness-value', 1, 255, state.maxBrightness);
      return;
    }
    if (t.dataset && t.dataset.baselineKey) {
      syncRangeValue(t.id, 'led-baseline-value-' + t.dataset.baselineKey, 0, 255, 0);
      return;
    }
    if (t.dataset && t.dataset.dutyKey) {
      syncRangeValue(t.id, 'led-duty-value-' + t.dataset.dutyKey, 1, 99, 50);
      return;
    }
    if (t.dataset && t.dataset.speedRangeKey) {
      var numberInput = document.getElementById('led-speed-' + t.dataset.speedRangeKey);
      numberInput.value = t.value;
      updateFrequency(t.dataset.speedRangeKey);
      return;
    }
    if (t.dataset && t.dataset.speedKey) {
      var rangeInput = document.getElementById('led-speed-range-' + t.dataset.speedKey);
      rangeInput.value = t.value;
      updateFrequency(t.dataset.speedKey);
      return;
    }
    if (t.dataset && t.dataset.colorKey) {
      syncColorFromPicker(t.dataset.colorKey);
      return;
    }
    if (t.dataset && t.dataset.rgbKey) {
      syncColorFromRgb(t.dataset.rgbKey);
      return;
    }
    if (t.id && t.id.indexOf('led-mode-') === 0) {
      updateFrequency(t.id.substring(9));
      return;
    }
  });
  document.addEventListener('click', function(ev){
    var t = ev.target;
    if (!t || !app.contains(t)) { return; }
    if (t.id === 'led-apply') {
      showMessage('Saving LED settings...', false);
      requestJson(app.dataset.savePath, collectConfig()).then(function(json){
        if (json && json.config) { state = json.config; }
        applyResponse(json);
      }).catch(function(){ showMessage('LED settings update failed.', true); });
      return;
    }
    if (t.id === 'led-reset') {
      if (!window.confirm('Reset LED settings to defaults?')) { return; }
      showMessage('Resetting LED settings...', false);
      var payload = {confirm:true};
      if (app.dataset.csrf) { payload.csrf = app.dataset.csrf; }
      requestJson(app.dataset.resetPath, payload).then(function(json){
        if (json && json.config) { state = json.config; }
        applyResponse(json);
      }).catch(function(){ showMessage('LED settings reset failed.', true); });
      return;
    }
    if (t.dataset && t.dataset.copyTarget) {
      var target = t.dataset.copyTarget;
      var sourceSelect = document.getElementById('led-copy-' + target);
      if (!sourceSelect || !sourceSelect.value) { showMessage('Select a source state to copy from.', true); return; }
      state.states[target] = copyStyle(state.states[sourceSelect.value]);
      render();
    }
  });
  fetch(app.dataset.fetchPath, {headers:{'Accept':'application/json'}, credentials:'same-origin'})
    .then(function(res){ return res.json(); })
    .then(function(json){ state = json; render(); })
    .catch(function(){ showMessage('Unable to load LED settings.', true); });
})();
</script>
)LEDSCRIPT";

String htmlEscapeText(const char* value) {
  String out;
  if (!value) {
    return out;
  }
  out.reserve(strlen(value) + 8);
  for (size_t i = 0; value[i] != '\0'; ++i) {
    switch (value[i]) {
      case '&':
        out += F("&amp;");
        break;
      case '<':
        out += F("&lt;");
        break;
      case '>':
        out += F("&gt;");
        break;
      case '\"':
        out += F("&quot;");
        break;
      case '\'':
        out += F("&#39;");
        break;
      default:
        out += value[i];
        break;
    }
  }
  return out;
}

void appendJsonString(String* out, const char* value) {
  if (!out) {
    return;
  }
  *out += '"';
  if (value) {
    for (size_t i = 0; value[i] != '\0'; ++i) {
      const char ch = value[i];
      if (ch == '\\' || ch == '"') {
        *out += '\\';
        *out += ch;
      } else {
        *out += ch;
      }
    }
  }
  *out += '"';
}

void setParseError(LedConfigJsonParseResult* result, const char* code, String* errorMessage, const __FlashStringHelper* msg,
                   bool hasState = false, LedPrinterState state = LedPrinterState::UNKNOWN) {
  if (result) {
    result->ok = false;
    result->code = code;
    result->hasState = hasState;
    result->state = state;
  }
  if (errorMessage) {
    *errorMessage = msg;
  }
}

bool isAllowedRootKey(const char* key) {
  return key && (!strcmp(key, "maxBrightness") || !strcmp(key, "states") || !strcmp(key, "csrf") ||
                 !strcmp(key, "version") || !strcmp(key, "modeOrder") || !strcmp(key, "modeLabels") ||
                 !strcmp(key, "stateOrder") || !strcmp(key, "stateLabels"));
}

bool isAllowedStyleKey(const char* key) {
  return key && (!strcmp(key, "mode") || !strcmp(key, "color") || !strcmp(key, "baselineBrightness") ||
                 !strcmp(key, "speedMs") || !strcmp(key, "flashDutyPct"));
}

bool isAllowedColorKey(const char* key) {
  return key && (!strcmp(key, "r") || !strcmp(key, "g") || !strcmp(key, "b"));
}

bool readUintField(JsonVariantConst value, uint32_t minValue, uint32_t maxValue, uint32_t* outValue) {
  if (!outValue || value.isNull()) {
    return false;
  }
  if (!(value.is<int>() || value.is<unsigned int>() || value.is<long>() || value.is<unsigned long>())) {
    return false;
  }
  long raw = value.as<long>();
  if (raw < static_cast<long>(minValue) || raw > static_cast<long>(maxValue)) {
    return false;
  }
  *outValue = static_cast<uint32_t>(raw);
  return true;
}

bool parseStateStyle(JsonObjectConst object, LedPrinterState state, LedStateStyle* outStyle, LedConfigJsonParseResult* result,
                     String* errorMessage) {
  if (!outStyle || object.isNull()) {
    setParseError(result, "invalid_style", errorMessage, F("LED state payload is invalid."), true, state);
    return false;
  }

  for (JsonPairConst kv : object) {
    if (!isAllowedStyleKey(kv.key().c_str())) {
      setParseError(result, "unexpected_field", errorMessage, F("Unexpected LED field."), true, state);
      return false;
    }
  }

  const char* modeKey = object["mode"];
  if (!modeKey || !ledAnimationModeFromKey(modeKey, &outStyle->mode)) {
    setParseError(result, "invalid_mode", errorMessage, F("LED mode is invalid."), true, state);
    return false;
  }

  JsonObjectConst color = object["color"].as<JsonObjectConst>();
  if (color.isNull()) {
    setParseError(result, "invalid_color", errorMessage, F("LED color is invalid."), true, state);
    return false;
  }
  for (JsonPairConst kv : color) {
    if (!isAllowedColorKey(kv.key().c_str())) {
      setParseError(result, "unexpected_field", errorMessage, F("Unexpected LED color field."), true, state);
      return false;
    }
  }

  uint32_t component = 0;
  if (!readUintField(color["r"], 0, 255, &component)) {
    setParseError(result, "invalid_color", errorMessage, F("LED color is invalid."), true, state);
    return false;
  }
  outStyle->color.r = static_cast<uint8_t>(component);
  if (!readUintField(color["g"], 0, 255, &component)) {
    setParseError(result, "invalid_color", errorMessage, F("LED color is invalid."), true, state);
    return false;
  }
  outStyle->color.g = static_cast<uint8_t>(component);
  if (!readUintField(color["b"], 0, 255, &component)) {
    setParseError(result, "invalid_color", errorMessage, F("LED color is invalid."), true, state);
    return false;
  }
  outStyle->color.b = static_cast<uint8_t>(component);

  if (!readUintField(object["baselineBrightness"], 0, 255, &component)) {
    setParseError(result, "invalid_baseline", errorMessage, F("LED baseline brightness is invalid."), true, state);
    return false;
  }
  outStyle->baselineBrightness = static_cast<uint8_t>(component);

  if (!readUintField(object["speedMs"], 20, 20000, &component)) {
    setParseError(result, "invalid_speed", errorMessage, F("LED animation period is invalid."), true, state);
    return false;
  }
  outStyle->speedMs = static_cast<uint16_t>(component);

  if (!readUintField(object["flashDutyPct"], 1, 99, &component)) {
    setParseError(result, "invalid_duty", errorMessage, F("LED flash duty is invalid."), true, state);
    return false;
  }
  outStyle->flashDutyPct = static_cast<uint8_t>(component);
  return true;
}

bool ledConfigsEqual(const LedBehaviorConfig& a, const LedBehaviorConfig& b) {
  if (a.maxBrightness != b.maxBrightness) {
    return false;
  }
  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    const LedStateStyle& lhs = a.states[i];
    const LedStateStyle& rhs = b.states[i];
    if (lhs.mode != rhs.mode || lhs.color.r != rhs.color.r || lhs.color.g != rhs.color.g || lhs.color.b != rhs.color.b ||
        lhs.baselineBrightness != rhs.baselineBrightness || lhs.speedMs != rhs.speedMs ||
        lhs.flashDutyPct != rhs.flashDutyPct) {
      return false;
    }
  }
  return true;
}

String buildLedConfigRequestJson(const LedBehaviorConfig& config) {
  String out;
  out.reserve(1500);
  out += F("{\"maxBrightness\":");
  out += String(config.maxBrightness);
  out += F(",\"states\":{");
  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    if (i > 0) {
      out += ',';
    }
    const LedPrinterState state = static_cast<LedPrinterState>(i);
    const LedStateStyle& style = config.states[i];
    appendJsonString(&out, ledPrinterStateKey(state));
    out += F(":{\"mode\":");
    appendJsonString(&out, ledAnimationModeKey(style.mode));
    out += F(",\"color\":{\"r\":");
    out += String(style.color.r);
    out += F(",\"g\":");
    out += String(style.color.g);
    out += F(",\"b\":");
    out += String(style.color.b);
    out += F("},\"baselineBrightness\":");
    out += String(style.baselineBrightness);
    out += F(",\"speedMs\":");
    out += String(style.speedMs);
    out += F(",\"flashDutyPct\":");
    out += String(style.flashDutyPct);
    out += F("}");
  }
  out += F("}}");
  return out;
}
}  // namespace

bool parseLedConfigJsonPayload(const String& payload, LedBehaviorConfig* outConfig, LedConfigJsonParseResult* result,
                               String* errorMessage) {
  if (result) {
    result->ok = false;
    result->code = "invalid_payload";
    result->hasState = false;
    result->state = LedPrinterState::UNKNOWN;
  }
  if (!outConfig) {
    if (errorMessage) {
      *errorMessage = F("LED payload handler unavailable.");
    }
    return false;
  }
  if (payload.length() == 0 || payload.length() > LED_CONFIG_HTTP_MAX_BODY_BYTES) {
    setParseError(result, "payload_too_large", errorMessage, F("LED request body is invalid."));
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    setParseError(result, "invalid_json", errorMessage, F("Unable to parse LED settings JSON."));
    return false;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    setParseError(result, "invalid_json", errorMessage, F("LED settings payload is invalid."));
    return false;
  }
  for (JsonPairConst kv : root) {
    if (!isAllowedRootKey(kv.key().c_str())) {
      setParseError(result, "unexpected_field", errorMessage, F("Unexpected LED payload field."));
      return false;
    }
  }

  if (!root["version"].isNull()) {
    uint32_t version = 0;
    if (!readUintField(root["version"], 1, 255, &version) || version != LED_CONFIG_SCHEMA_VERSION) {
      setParseError(result, "schema_unsupported", errorMessage, F("Unsupported LED schema version."));
      return false;
    }
  }

  clearLedBehaviorConfig(outConfig);

  uint32_t maxBrightness = 0;
  if (!readUintField(root["maxBrightness"], 1, 255, &maxBrightness)) {
    setParseError(result, "invalid_max_brightness", errorMessage, F("LED max brightness is invalid."));
    return false;
  }
  outConfig->maxBrightness = static_cast<uint8_t>(maxBrightness);

  JsonObjectConst states = root["states"].as<JsonObjectConst>();
  if (states.isNull()) {
    setParseError(result, "missing_states", errorMessage, F("LED states payload is missing."));
    return false;
  }

  bool seen[LED_PRINTER_STATE_COUNT] = {false};
  size_t seenCount = 0;
  for (JsonPairConst kv : states) {
    LedPrinterState state;
    if (!ledPrinterStateFromKey(kv.key().c_str(), &state)) {
      setParseError(result, "unknown_state", errorMessage, F("Unknown LED printer state."));
      return false;
    }
    const size_t index = static_cast<size_t>(state);
    if (seen[index]) {
      setParseError(result, "duplicate_state", errorMessage, F("Duplicate LED printer state."), true, state);
      return false;
    }
    if (!parseStateStyle(kv.value().as<JsonObjectConst>(), state, &outConfig->states[index], result, errorMessage)) {
      return false;
    }
    seen[index] = true;
    seenCount++;
  }

  if (seenCount != LED_PRINTER_STATE_COUNT) {
    for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
      if (!seen[i]) {
        setParseError(result, "missing_state", errorMessage, F("LED printer state is missing."), true,
                      static_cast<LedPrinterState>(i));
        return false;
      }
    }
  }

  normalizeLedBehaviorConfig(outConfig);
  const LedConfigValidationResult validation = validateLedBehaviorConfig(*outConfig);
  if (!validation.ok) {
    if (result) {
      result->ok = false;
      result->code = ledConfigValidationCode(validation);
      result->hasState = true;
      result->state = validation.state;
    }
    if (errorMessage) {
      *errorMessage = F("LED settings rejected.");
    }
    return false;
  }

  if (result) {
    result->ok = true;
    result->code = "ok";
    result->hasState = false;
    result->state = LedPrinterState::UNKNOWN;
  }
  return true;
}

String buildLedConfigApiJson(const LedBehaviorConfig& config) {
  String out;
  out.reserve(2200);
  out += F("{\"version\":");
  out += String(LED_CONFIG_SCHEMA_VERSION);
  out += F(",\"maxBrightness\":");
  out += String(config.maxBrightness);
  out += F(",\"modeOrder\":[");
  for (uint8_t i = 0; i < 4; ++i) {
    if (i > 0) {
      out += ',';
    }
    appendJsonString(&out, ledAnimationModeKey(static_cast<LedAnimationMode>(i)));
  }
  out += F("],\"modeLabels\":{");
  for (uint8_t i = 0; i < 4; ++i) {
    if (i > 0) {
      out += ',';
    }
    appendJsonString(&out, ledAnimationModeKey(static_cast<LedAnimationMode>(i)));
    out += ':';
    appendJsonString(&out, ledAnimationModeLabel(static_cast<LedAnimationMode>(i)));
  }
  out += F("},\"stateOrder\":[");
  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    if (i > 0) {
      out += ',';
    }
    appendJsonString(&out, ledPrinterStateKey(static_cast<LedPrinterState>(i)));
  }
  out += F("],\"stateLabels\":{");
  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    if (i > 0) {
      out += ',';
    }
    const LedPrinterState state = static_cast<LedPrinterState>(i);
    appendJsonString(&out, ledPrinterStateKey(state));
    out += ':';
    appendJsonString(&out, ledPrinterStateLabel(state));
  }
  out += F("},\"states\":{");
  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    if (i > 0) {
      out += ',';
    }
    const LedPrinterState state = static_cast<LedPrinterState>(i);
    const LedStateStyle& style = config.states[i];
    appendJsonString(&out, ledPrinterStateKey(state));
    out += F(":{\"mode\":");
    appendJsonString(&out, ledAnimationModeKey(style.mode));
    out += F(",\"color\":{\"r\":");
    out += String(style.color.r);
    out += F(",\"g\":");
    out += String(style.color.g);
    out += F(",\"b\":");
    out += String(style.color.b);
    out += F("},\"baselineBrightness\":");
    out += String(style.baselineBrightness);
    out += F(",\"speedMs\":");
    out += String(style.speedMs);
    out += F(",\"flashDutyPct\":");
    out += String(style.flashDutyPct);
    out += '}';
  }
  out += F("}}");
  return out;
}

String buildLedConfigActionResponse(bool ok, const char* code, const char* message, bool hasState,
                                    LedPrinterState state) {
  String out;
  out.reserve(256);
  out += F("{\"ok\":");
  out += ok ? F("true") : F("false");
  out += F(",\"code\":");
  appendJsonString(&out, code ? code : (ok ? "ok" : "error"));
  if (message && message[0] != '\0') {
    out += F(",\"message\":");
    appendJsonString(&out, message);
  }
  if (hasState) {
    out += F(",\"state\":");
    appendJsonString(&out, ledPrinterStateKey(state));
  }
  out += '}';
  return out;
}

void appendLedConfigEditorSection(String* body, const char* fetchPath, const char* savePath, const char* resetPath,
                                  const char* csrfToken, const char* networkLabel, const char* networkValue,
                                  const char* locationLabel, const char* locationValue) {
  if (!body) {
    return;
  }
  body->reserve(body->length() + 12000);
  *body += FPSTR(kLedUiStyle);
  *body += F("<div class='card'><h2>LED Settings</h2><p>Configure global brightness and per-state LED output. Changes apply without reboot.</p><div id='led-config-app' data-fetch-path='");
  *body += htmlEscapeText(fetchPath ? fetchPath : "/led-config");
  *body += F("' data-save-path='");
  *body += htmlEscapeText(savePath ? savePath : "/led-config");
  *body += F("' data-reset-path='");
  *body += htmlEscapeText(resetPath ? resetPath : "/led-reset");
  *body += F("' data-csrf='");
  *body += htmlEscapeText(csrfToken ? csrfToken : "");
  *body += F("' data-network-label='");
  *body += htmlEscapeText(networkLabel ? networkLabel : "Network");
  *body += F("' data-network-value='");
  *body += htmlEscapeText(networkValue ? networkValue : "");
  *body += F("' data-location-label='");
  *body += htmlEscapeText(locationLabel ? locationLabel : "Portal");
  *body += F("' data-location-value='");
  *body += htmlEscapeText(locationValue ? locationValue : "");
  *body += F("'><p>Loading LED settings...</p></div></div>");
  *body += FPSTR(kLedUiScript);
}

bool runLedConfigJsonSelfTest(Stream& out) {
  LedBehaviorConfig config{};
  LedBehaviorConfig parsed{};
  setDefaultLedBehaviorConfig(&config);
  normalizeLedBehaviorConfig(&config);

  LedConfigJsonParseResult result{};
  String errorMessage;
  if (!parseLedConfigJsonPayload(buildLedConfigRequestJson(config), &parsed, &result, &errorMessage)) {
    out.println("Self-test failed: expected LED JSON payload to parse");
    return false;
  }
  if (!ledConfigsEqual(config, parsed)) {
    out.println("Self-test failed: LED JSON round-trip mismatch");
    return false;
  }

  const String missingStatePayload =
      F("{\"maxBrightness\":100,\"states\":{\"unknown\":{\"mode\":\"SOLID\",\"color\":{\"r\":1,\"g\":2,\"b\":3},"
        "\"baselineBrightness\":1,\"speedMs\":1000,\"flashDutyPct\":50}}}");
  errorMessage.remove(0);
  if (parseLedConfigJsonPayload(missingStatePayload, &parsed, &result, &errorMessage) || strcmp(result.code, "missing_state") != 0) {
    out.println("Self-test failed: expected missing_state rejection");
    return false;
  }

  const String extraStatePayload =
      F("{\"maxBrightness\":100,\"states\":{\"unknown\":{\"mode\":\"SOLID\",\"color\":{\"r\":1,\"g\":2,\"b\":3},"
        "\"baselineBrightness\":1,\"speedMs\":1000,\"flashDutyPct\":50},\"invalid\":{\"mode\":\"SOLID\","
        "\"color\":{\"r\":1,\"g\":2,\"b\":3},\"baselineBrightness\":1,\"speedMs\":1000,\"flashDutyPct\":50}}}");
  errorMessage.remove(0);
  if (parseLedConfigJsonPayload(extraStatePayload, &parsed, &result, &errorMessage) || strcmp(result.code, "unknown_state") != 0) {
    out.println("Self-test failed: expected unknown_state rejection");
    return false;
  }

  clearLedBehaviorConfig(&config);
  clearLedBehaviorConfig(&parsed);
  return true;
}
