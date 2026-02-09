#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <time.h>
#include <Preferences.h>

// ===================== GPIO =====================

const int LED_PIN = 2;

// ===================== USTAWIENIA WIFI (domyślne) =====================

const char* WIFI_SSID_DEFAULT     = "SSID";
const char* WIFI_PASSWORD_DEFAULT = "password";
const bool WIFI_USE_DHCP_DEFAULT = true;

IPAddress WIFI_LOCAL_IP_DEFAULT(192, 168, 1, 50);
IPAddress WIFI_GATEWAY_DEFAULT(192, 168, 1, 1);
IPAddress WIFI_SUBNET_DEFAULT(255, 255, 255, 0);

const long  GMT_OFFSET_SEC_DEFAULT = 1 * 3600;

// ===================== USTAWIENIA APRS-IS =====================

const char* APRS_SERVER = "euro.aprs2.net";
const uint16_t APRS_PORT = 14580;

// ===================== USTAWIENIA NTP (domyślne) =====================

const char* NTP_SERVER_DEFAULT = "pool.ntp.org";
const int   DAYLIGHT_OFFSET_SEC = 0;

// ===================== NVS (Preferences) =====================

Preferences prefs;

// ===================== ZMIENNE KONFIGURACYJNE =====================

String wifiSsid = WIFI_SSID_DEFAULT;
String wifiPassword = WIFI_PASSWORD_DEFAULT;
bool wifiUseDhcp = WIFI_USE_DHCP_DEFAULT;
IPAddress wifiLocalIp = WIFI_LOCAL_IP_DEFAULT;
IPAddress wifiGateway = WIFI_GATEWAY_DEFAULT;
IPAddress wifiSubnet = WIFI_SUBNET_DEFAULT;
long gmtOffsetSec = GMT_OFFSET_SEC_DEFAULT;
String ntpServer = NTP_SERVER_DEFAULT;

// ===================== STRUKTURY =====================

struct AprsObject {
  String name;
  String call;
  String passcode;
  String symbol;
  String latitude;
  String longitude;
  String comment;
  uint32_t interval;
  uint32_t lastSent;
  bool lastOk;
  
  // Harmonogram
  bool scheduleEnabled;
  bool scheduleDays[7];
  uint8_t scheduleStartHour;
  uint8_t scheduleStartMinute;
  uint8_t scheduleStopHour;
  uint8_t scheduleStopMinute;
};

AprsObject objects[5];

WebServer server(80);

String aprsStatusLog = "";

// ===================== LED BLINK =====================

unsigned long lastLedBlink = 0;
bool ledState = false;

void updateLED() {
  if (WiFi.status() == WL_CONNECTED) {
    unsigned long now = millis();
    
    // Mrugaj co 1 sekundę przez 100ms
    if (now - lastLedBlink >= 1000) {
      lastLedBlink = now;
      digitalWrite(LED_PIN, HIGH);
      ledState = true;
    }
    
    // Wyłącz po 100ms
    if (ledState && (now - lastLedBlink >= 100)) {
      digitalWrite(LED_PIN, LOW);
      ledState = false;
    }
  } else {
    // Wyłącz LED gdy brak połączenia
    digitalWrite(LED_PIN, LOW);
    ledState = false;
  }
}

// ===================== GENERATOR HASŁA APRS =====================

uint16_t generateAprsPasscode(String callsign) {
  callsign.trim();
  callsign.toUpperCase();
  
  int dashPos = callsign.indexOf('-');
  if (dashPos > 0) {
    callsign = callsign.substring(0, dashPos);
  }
  
  uint16_t hash = 0x73e2;
  
  for (int i = 0; i < callsign.length(); i++) {
    char c = callsign.charAt(i);
    hash ^= (uint16_t)c << 8;
    
    if (i + 1 < callsign.length()) {
      char c2 = callsign.charAt(i + 1);
      hash ^= (uint16_t)c2;
      i++;
    }
  }
  
  return hash & 0x7fff;
}

// ===================== POMOCNICZE =====================

time_t getNow() {
  time_t now;
  time(&now);
  return now;
}

String getCurrentDateTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "-- -- ---- --:--:--";
  }
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

String formatTimeSince(uint32_t last) {
  if (last == 0) return String("–");
  time_t now = getNow();
  if (now < last) return String("0 s");
  uint32_t diff = now - last;
  uint32_t h = diff / 3600;
  uint32_t m = (diff % 3600) / 60;
  uint32_t s = diff % 60;
  char buf[20];
  if (h > 0) {
    snprintf(buf, sizeof(buf), "%uh %um %us", h, m, s);
  } else if (m > 0) {
    snprintf(buf, sizeof(buf), "%um %us", m, s);
  } else {
    snprintf(buf, sizeof(buf), "%us", s);
  }
  return String(buf);
}

void appendAprsLog(const String &line) {
  aprsStatusLog += line + "\n";
  Serial.println("[APRS] " + line);
  const size_t maxLen = 2000;
  if (aprsStatusLog.length() > maxLen) {
    aprsStatusLog = aprsStatusLog.substring(aprsStatusLog.length() - maxLen);
  }
}

bool isScheduleActive(const AprsObject &obj) {
  if (!obj.scheduleEnabled) return true;
  
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return false;
  }
  
  int dow = timeinfo.tm_wday;
  int dowPL = (dow == 0) ? 6 : (dow - 1);
  
  if (!obj.scheduleDays[dowPL]) {
    return false;
  }
  
  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMinutes = obj.scheduleStartHour * 60 + obj.scheduleStartMinute;
  int stopMinutes = obj.scheduleStopHour * 60 + obj.scheduleStopMinute;
  
  if (startMinutes <= stopMinutes) {
    if (currentMinutes < startMinutes || currentMinutes >= stopMinutes) {
      return false;
    }
  } else {
    if (currentMinutes < startMinutes && currentMinutes >= stopMinutes) {
      return false;
    }
  }
  
  return true;
}

// ===================== NVS: ZAPIS/ODCZYT =====================

void loadConfigFromNVS() {
  prefs.begin("aprs_cfg", true);

  // WiFi settings
  wifiSsid = prefs.getString("wifiSsid", WIFI_SSID_DEFAULT);
  wifiPassword = prefs.getString("wifiPass", WIFI_PASSWORD_DEFAULT);
  wifiUseDhcp = prefs.getBool("wifiDhcp", WIFI_USE_DHCP_DEFAULT);
  
  uint32_t ip = prefs.getUInt("wifiIp", (uint32_t)WIFI_LOCAL_IP_DEFAULT);
  wifiLocalIp = IPAddress(ip);
  
  uint32_t gw = prefs.getUInt("wifiGw", (uint32_t)WIFI_GATEWAY_DEFAULT);
  wifiGateway = IPAddress(gw);
  
  uint32_t sn = prefs.getUInt("wifiSn", (uint32_t)WIFI_SUBNET_DEFAULT);
  wifiSubnet = IPAddress(sn);
  
  gmtOffsetSec = prefs.getLong("gmtOffset", GMT_OFFSET_SEC_DEFAULT);
  ntpServer = prefs.getString("ntpServer", NTP_SERVER_DEFAULT);

  Serial.println("=== Ładowanie konfiguracji z NVS ===");
  Serial.println("WiFi SSID: " + wifiSsid);
  Serial.println("WiFi DHCP: " + String(wifiUseDhcp ? "TAK" : "NIE"));
  Serial.println("WiFi IP: " + wifiLocalIp.toString());
  Serial.println("GMT Offset: " + String(gmtOffsetSec / 3600) + "h");
  Serial.println("NTP Server: " + ntpServer);

  for (int i = 0; i < 5; i++) {
    String idx = String(i);
    
    objects[i].name      = prefs.getString(("name_" + idx).c_str(), "");
    objects[i].call      = prefs.getString(("call_" + idx).c_str(), "");
    objects[i].passcode  = prefs.getString(("pass_" + idx).c_str(), "");
    objects[i].symbol    = prefs.getString(("sym_"  + idx).c_str(), "/-");
    objects[i].latitude  = prefs.getString(("lat_"  + idx).c_str(), "5224.36N");
    objects[i].longitude = prefs.getString(("lon_"  + idx).c_str(), "01655.50E");
    objects[i].comment   = prefs.getString(("com_"  + idx).c_str(), "ESP32 Gateway");
    objects[i].interval  = prefs.getUInt(("int_"   + idx).c_str(), 0);
    objects[i].lastSent  = 0;
    objects[i].lastOk    = false;
    
    objects[i].scheduleEnabled = prefs.getBool(("sch_en_" + idx).c_str(), false);
    uint8_t days = prefs.getUChar(("sch_days_" + idx).c_str(), 0x7F);
    for (int d = 0; d < 7; d++) {
      objects[i].scheduleDays[d] = (days >> d) & 0x01;
    }
    objects[i].scheduleStartHour = prefs.getUChar(("sch_sh_" + idx).c_str(), 0);
    objects[i].scheduleStartMinute = prefs.getUChar(("sch_sm_" + idx).c_str(), 0);
    objects[i].scheduleStopHour = prefs.getUChar(("sch_eh_" + idx).c_str(), 23);
    objects[i].scheduleStopMinute = prefs.getUChar(("sch_em_" + idx).c_str(), 59);
  }

  prefs.end();
  Serial.println("=== Konfiguracja załadowana ===");
}

void saveConfigToNVS() {
  prefs.begin("aprs_cfg", false);

  Serial.println("=== Zapisywanie konfiguracji do NVS ===");

  // WiFi settings
  prefs.putString("wifiSsid", wifiSsid);
  prefs.putString("wifiPass", wifiPassword);
  prefs.putBool("wifiDhcp", wifiUseDhcp);
  prefs.putUInt("wifiIp", (uint32_t)wifiLocalIp);
  prefs.putUInt("wifiGw", (uint32_t)wifiGateway);
  prefs.putUInt("wifiSn", (uint32_t)wifiSubnet);
  prefs.putLong("gmtOffset", gmtOffsetSec);
  prefs.putString("ntpServer", ntpServer);

  for (int i = 0; i < 5; i++) {
    String idx = String(i);
    
    prefs.putString(("name_" + idx).c_str(), objects[i].name);
    prefs.putString(("call_" + idx).c_str(), objects[i].call);
    prefs.putString(("pass_" + idx).c_str(), objects[i].passcode);
    prefs.putString(("sym_"  + idx).c_str(), objects[i].symbol);
    prefs.putString(("lat_"  + idx).c_str(), objects[i].latitude);
    prefs.putString(("lon_"  + idx).c_str(), objects[i].longitude);
    prefs.putString(("com_"  + idx).c_str(), objects[i].comment);
    prefs.putUInt  (("int_"  + idx).c_str(), objects[i].interval);
    
    prefs.putBool(("sch_en_" + idx).c_str(), objects[i].scheduleEnabled);
    uint8_t days = 0;
    for (int d = 0; d < 7; d++) {
      if (objects[i].scheduleDays[d]) {
        days |= (1 << d);
      }
    }
    prefs.putUChar(("sch_days_" + idx).c_str(), days);
    prefs.putUChar(("sch_sh_" + idx).c_str(), objects[i].scheduleStartHour);
    prefs.putUChar(("sch_sm_" + idx).c_str(), objects[i].scheduleStartMinute);
    prefs.putUChar(("sch_eh_" + idx).c_str(), objects[i].scheduleStopHour);
    prefs.putUChar(("sch_em_" + idx).c_str(), objects[i].scheduleStopMinute);
  }

  prefs.end();
  Serial.println("=== Konfiguracja zapisana ===");
}

// ===================== APRS-IS =====================

String validateAprsCoord(String coord, bool isLat) {
  coord.trim();
  coord.toUpperCase();
  
  int expectedLen = isLat ? 8 : 9;
  
  if (coord.length() != expectedLen) {
    return "";
  }
  
  int dotPos = isLat ? 4 : 5;
  if (coord.charAt(dotPos) != '.') {
    return "";
  }
  
  char dir = coord.charAt(expectedLen - 1);
  if (isLat) {
    if (dir != 'N' && dir != 'S') return "";
  } else {
    if (dir != 'E' && dir != 'W') return "";
  }
  
  for (int i = 0; i < expectedLen - 1; i++) {
    if (i == dotPos) continue;
    char c = coord.charAt(i);
    if (c < '0' || c > '9') {
      return "";
    }
  }
  
  return coord;
}

String buildAprsPositionPacket(const AprsObject &obj) {
  String lat = validateAprsCoord(obj.latitude, true);
  String lon = validateAprsCoord(obj.longitude, false);
  
  if (lat.length() == 0 || lon.length() == 0) {
    appendAprsLog("BŁĄD: Nieprawidłowy format współrzędnych dla " + obj.call);
    return "";
  }
  
  String sym = obj.symbol;
  if (sym.length() != 2) {
    sym = "/-";
  }
  
  char table = sym.charAt(0);
  char code = sym.charAt(1);
  
  if (table != '/' && table != '\\') {
    table = '/';
  }
  
  String packet;
  packet.reserve(200);
  packet  = obj.call;
  packet += ">APRS,TCPIP*:";
  packet += "!";
  packet += lat;
  packet += table;
  packet += lon;
  packet += code;
  packet += obj.comment;
  
  return packet;
}

bool aprsSendObject(AprsObject &obj) {
  appendAprsLog("=== Rozpoczynam wysyłanie dla: " + obj.call + " ===");
  
  if (obj.call.length() == 0) {
    obj.lastOk = false;
    return false;
  }

  if (obj.interval == 0) {
    obj.lastOk = false;
    return false;
  }

  if (obj.passcode.length() == 0) {
    obj.lastOk = false;
    return false;
  }

  String packet = buildAprsPositionPacket(obj);
  
  if (packet.length() == 0) {
    obj.lastOk = false;
    return false;
  }

  WiFiClient aprsClient;
  
  if (!aprsClient.connect(APRS_SERVER, APRS_PORT, 10000)) {
    appendAprsLog("BŁĄD: Nie można połączyć się z serwerem");
    obj.lastOk = false;
    return false;
  }

  unsigned long start = millis();
  String banner = "";
  while (millis() - start < 3000) {
    if (aprsClient.available()) {
      char c = aprsClient.read();
      banner += c;
      if (c == '\n') break;
    }
    delay(10);
  }

  String loginLine = "user " + obj.call + " pass " + obj.passcode +
                     " vers ESP32-APRS 1.0\r\n";
  aprsClient.print(loginLine);

  start = millis();
  String response = "";
  while (millis() - start < 3000) {
    if (aprsClient.available()) {
      char c = aprsClient.read();
      response += c;
      if (c == '\n') break;
    }
    delay(10);
  }

  if (response.indexOf("verified") > 0) {
    appendAprsLog("Login zweryfikowany OK");
  }

  appendAprsLog(">>> Wysyłam: " + packet);
  
  aprsClient.print(packet + "\r\n");
  aprsClient.flush();

  delay(500);
  aprsClient.stop();

  obj.lastSent = getNow();
  obj.lastOk   = true;
  
  appendAprsLog("✓ Pakiet wysłany OK");
  
  return true;
}

// ===================== HTML =====================

String htmlPage() {
  String s;
  s += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>ESP32 APRS OBJECT SENDER</title>";
  s += "<style>";
  s += "* {margin:0; padding:0; box-sizing:border-box;}";
  s += "body{font-family:Arial;background-color:#363636;color:#fff;padding:20px;}";
  s += ".main-table{width:900px;margin:0 auto;border-collapse:collapse;}";
  s += ".header{background:#111;padding:15px;text-align:center;border-bottom:2px solid #0f0;}";
  s += ".header h1{color:#0f0;font-size:24px;margin-bottom:5px;}";
  s += ".datetime{color:#ff8c00;font-size:20px;}";
  s += ".content-row{height:600px;}";
  s += ".sidebar{width:250px;background:#111;border-right:2px solid #333;vertical-align:top;padding:0;}";
  s += ".content{background:#000;vertical-align:top;padding:30px 30px 30px 40px;}";
  s += ".menu-item{padding:15px 20px;border-bottom:1px solid #333;cursor:pointer;transition:background 0.3s;}";
  s += ".menu-item:hover{background:#222;}";
  s += ".menu-item.active{background:#0a4d0a;border-left:4px solid #0f0;}";
  s += ".menu-title{font-weight:bold;font-size:14px;color:#fff;}";
  s += ".menu-subtitle{font-size:11px;color:#aaa;margin-top:3px;}";
  s += ".menu-status{font-size:11px;margin-top:5px;}";
  s += ".content-section{display:none;}";
  s += ".content-section.active{display:block;}";
  s += ".form-group{margin-bottom:20px;}";
  s += ".form-label{display:block;color:#aaa;font-size:12px;margin-bottom:5px;font-weight:bold;}";
  s += "input[type=text],input[type=time],input[type=password]{background:#111;color:#fff;border:1px solid #555;padding:8px;width:300px;}";
  s += "input[type=checkbox]{background:#111;color:#fff;border:1px solid #555;width:auto;height:18px;cursor:pointer;}";
  s += ".checkbox-group{display:flex;gap:10px;align-items:center;}";
  s += ".days-group{display:flex;gap:10px;margin-top:5px;}";
  s += ".day-item{display:flex;flex-direction:column;align-items:center;gap:3px;}";
  s += ".day-item label{font-size:11px;}";
  s += ".btn{background:#0a4d0a;color:#fff;border:none;padding:12px 30px;font-size:14px;cursor:pointer;border-radius:3px;}";
  s += ".btn:hover{background:#0f7d0f;}";
  s += ".btn-danger{background:#8b0000;}";
  s += ".btn-danger:hover{background:#b00000;}";
  s += ".status-ok{color:#0f0;}";
  s += ".status-error{color:#f00;}";
  s += ".converter-input{width:300px;margin-bottom:10px;}";
  s += ".converter-result{background:#0a4d0a;padding:8px;margin-top:10px;border-radius:3px;}";
  s += "a{color:#0ff;text-decoration:none;}";
  s += "a:hover{text-decoration:underline;}";
  s += ".scroll-content{height:600px;overflow-y:auto;}";
  
  // Niestandardowe kolory scrollbara
  s += ".scroll-content::-webkit-scrollbar{width:12px;}";
  s += ".scroll-content::-webkit-scrollbar-track{background:#363636;}";
  s += ".scroll-content::-webkit-scrollbar-thumb{background:#0f0;border-radius:6px;}";
  s += ".scroll-content::-webkit-scrollbar-thumb:hover{background:#0f7d0f;}";
  
  s += ".passcode-result{background:#0a4d0a;color:#fff;padding:15px;margin-top:10px;border-radius:3px;font-size:16px;}";
  s += ".passcode-number{color:#0f0;font-size:24px;font-weight:bold;margin:10px 0;}";
  s += ".warning{background:#8b4513;padding:10px;margin-bottom:20px;border-radius:3px;font-size:12px;}";
  s += "</style>";
  s += "<script>";
  
  // Refresh status
  s += "function refreshStatus(){";
  s += "  fetch('/status_data').then(r=>r.json()).then(j=>{";
  s += "    for(let i=0;i<5;i++){";
  s += "      let el=document.getElementById('menu_status_'+i);";
  s += "      if(el && j.objects[i]){";
  s += "        el.innerHTML='Status: '+j.objects[i].status+' | '+j.objects[i].time;";
  s += "      }";
  s += "    }";
  s += "    document.getElementById('datetime').innerHTML=j.datetime;";
  s += "  });";
  s += "}";
  s += "setInterval(refreshStatus,2000);";
  
  // Show section
  s += "function showSection(section){";
  s += "  document.querySelectorAll('.content-section').forEach(el=>el.classList.remove('active'));";
  s += "  document.querySelectorAll('.menu-item').forEach(el=>el.classList.remove('active'));";
  s += "  document.getElementById('section_'+section).classList.add('active');";
  s += "  document.getElementById('menu_'+section).classList.add('active');";
  s += "}";
  
  // Toggle DHCP fields
  s += "function toggleDhcp(){";
  s += "  let dhcp=document.getElementById('dhcp_enabled').checked;";
  s += "  document.getElementById('static_fields').style.display=dhcp?'none':'block';";
  s += "}";
  
  // Converter coords
  s += "function convertCoords(){";
  s += "  let latDec=parseFloat(document.getElementById('lat_dec').value);";
  s += "  let lonDec=parseFloat(document.getElementById('lon_dec').value);";
  s += "  if(isNaN(latDec)||isNaN(lonDec)){alert('Błędne współrzędne');return;}";
  s += "  let latDir=latDec>=0?'N':'S';";
  s += "  let lonDir=lonDec>=0?'E':'W';";
  s += "  latDec=Math.abs(latDec);lonDec=Math.abs(lonDec);";
  s += "  let latDeg=Math.floor(latDec);";
  s += "  let latMin=(latDec-latDeg)*60;";
  s += "  let lonDeg=Math.floor(lonDec);";
  s += "  let lonMin=(lonDec-lonDeg)*60;";
  s += "  let latAPRS=String(latDeg).padStart(2,'0')+String(latMin.toFixed(2)).padStart(5,'0')+latDir;";
  s += "  let lonAPRS=String(lonDeg).padStart(3,'0')+String(lonMin.toFixed(2)).padStart(5,'0')+lonDir;";
  s += "  document.getElementById('result').innerHTML='<strong>Szerokość APRS:</strong> '+latAPRS+'<br><strong>Długość APRS:</strong> '+lonAPRS;";
  s += "  document.getElementById('result').style.display='block';";
  s += "}";
  
  // Generate passcode
  s += "function generatePasscode(){";
  s += "  let call=document.getElementById('passcode_call').value.trim().toUpperCase();";
  s += "  if(call.length==0){alert('Wprowadź znak wywołania');return;}";
  s += "  fetch('/generate_passcode?call='+encodeURIComponent(call))";
  s += "    .then(r=>r.json())";
  s += "    .then(j=>{";
  s += "      document.getElementById('passcode_result').innerHTML=";
  s += "        '<strong>Znak wywołania:</strong> '+j.call+'<div class=\"passcode-number\">'+j.passcode+'</div>';";
  s += "      document.getElementById('passcode_result').style.display='block';";
  s += "    });";
  s += "}";
  
  s += "window.onload=function(){refreshStatus();showSection('obj0');toggleDhcp();};";
  s += "</script>";
  s += "</head><body>";
  
  // Main table
  s += "<table class='main-table'>";
  
  // Header row
  s += "<tr><td colspan='2' class='header'>";
  s += "<h1>ESP32 APRS OBJECT SENDER</h1>";
  s += "<div class='datetime' id='datetime'>" + getCurrentDateTime() + "</div>";
  s += "</td></tr>";
  
  // Content row
  s += "<tr class='content-row'>";
  
  // Sidebar cell
  s += "<td class='sidebar'><div class='scroll-content'>";
  for (int i = 0; i < 5; i++) {
    String menuId = "menu_obj" + String(i);
    s += "<div class='menu-item' id='" + menuId + "' onclick='showSection(\"obj" + String(i) + "\")'>";
    
    // Tytuł - nazwa obiektu lub domyślna
    String menuTitle = objects[i].name.length() > 0 ? objects[i].name : "Obiekt " + String(i + 1);
    s += "<div class='menu-title'>" + menuTitle + "</div>";
    
    // Podtytuł - znak wywołania
    String callSign = objects[i].call.length() > 0 ? objects[i].call : "Nie skonfigurowany";
    s += "<div class='menu-subtitle'>" + callSign + "</div>";
    
    String st;
    if (objects[i].lastSent == 0) {
      st = "–";
    } else {
      st = objects[i].lastOk ? "<span class='status-ok'>✓</span>" : "<span class='status-error'>✗</span>";
    }
    s += "<div class='menu-status' id='menu_status_" + String(i) + "'>Status: " + st + " | " + formatTimeSince(objects[i].lastSent) + "</div>";
    s += "</div>";
  }
  
  // Passcode generator menu item
  s += "<div class='menu-item' id='menu_passcode' onclick='showSection(\"passcode\")'>";
  s += "<div class='menu-title'>Generator hasła</div>";
  s += "<div class='menu-subtitle'>APRS-IS Passcode</div>";
  s += "</div>";
  
  // Converter menu item
  s += "<div class='menu-item' id='menu_converter' onclick='showSection(\"converter\")'>";
  s += "<div class='menu-title'>Konwerter współrzędnych</div>";
  s += "<div class='menu-subtitle'>Google Maps → APRS</div>";
  s += "</div>";
  
  // Settings menu item
  s += "<div class='menu-item' id='menu_settings' onclick='showSection(\"settings\")'>";
  s += "<div class='menu-title'>Ustawienia</div>";
  s += "<div class='menu-subtitle'>WiFi, NTP, System</div>";
  s += "</div>";
  
  s += "</div></td>";
  
  // Content cell
  s += "<td class='content'><div class='scroll-content'>";
  
  // Forms for each object
  for (int i = 0; i < 5; i++) {
    String sectionId = "section_obj" + String(i);
    String activeClass = (i == 0) ? "active" : "";
    s += "<div class='content-section " + activeClass + "' id='" + sectionId + "'>";
    
    String headerTitle = objects[i].name.length() > 0 ? objects[i].name : "Obiekt " + String(i + 1);
    s += "<h2 style='color:#0f0;margin-bottom:20px;'>" + headerTitle + "</h2>";
    
    s += "<form method='POST' action='/save?obj=" + String(i) + "'>";
    
    // Nazwa obiektu
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Nazwa obiektu (opcjonalnie)</label>";
    s += "<input type='text' name='name' value='" + objects[i].name + "' placeholder='np. Dom, Praca, Domek letniskowy'>";
    s += "</div>";
    
    // Znak
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Znak wywołania</label>";
    s += "<input type='text' name='call' value='" + objects[i].call + "' placeholder='SP3VSS-" + String(i+1) + "'>";
    s += "</div>";
    
    // Hasło
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Hasło APRS-IS</label>";
    s += "<input type='text' name='pass' value='" + objects[i].passcode + "' placeholder='12345'>";
    s += "</div>";
    
    // Symbol
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Symbol (2 znaki)</label>";
    s += "<input type='text' name='sym' value='" + objects[i].symbol + "' placeholder='/-'>";
    s += "</div>";
    
    // Szerokość
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Szerokość geograficzna (DDMM.hhN)</label>";
    s += "<input type='text' name='lat' value='" + objects[i].latitude + "' placeholder='5224.36N'>";
    s += "</div>";
    
    // Długość
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Długość geograficzna (DDDMM.hhE)</label>";
    s += "<input type='text' name='lon' value='" + objects[i].longitude + "' placeholder='01655.50E'>";
    s += "</div>";
    
    // Komentarz
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Komentarz</label>";
    s += "<input type='text' name='com' value='" + objects[i].comment + "' placeholder='ESP32 Gateway'>";
    s += "</div>";
    
    // Okres
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Okres wysyłania [sekundy]</label>";
    s += "<input type='text' name='int' value='" + String(objects[i].interval) + "' placeholder='600'>";
    s += "</div>";
    
    // Harmonogram
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Harmonogram wysyłania</label>";
    s += "<div class='checkbox-group'>";
    s += "<input type='checkbox' name='sch_en' value='1'";
    if (objects[i].scheduleEnabled) s += " checked";
    s += "><span>Włącz harmonogram</span>";
    s += "</div>";
    s += "</div>";
    
    // Dni tygodnia
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Dni tygodnia</label>";
    s += "<div class='days-group'>";
    const char* dayNames[] = {"Pon", "Wt", "Śr", "Czw", "Pt", "Sob", "Nie"};
    for (int d = 0; d < 7; d++) {
      s += "<div class='day-item'>";
      s += "<label>" + String(dayNames[d]) + "</label>";
      s += "<input type='checkbox' name='day_" + String(d) + "' value='1'";
      if (objects[i].scheduleDays[d]) s += " checked";
      s += ">";
      s += "</div>";
    }
    s += "</div>";
    s += "</div>";
    
    // Godziny
    char startTime[6], stopTime[6];
    snprintf(startTime, 6, "%02d:%02d", objects[i].scheduleStartHour, objects[i].scheduleStartMinute);
    snprintf(stopTime, 6, "%02d:%02d", objects[i].scheduleStopHour, objects[i].scheduleStopMinute);
    
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Godzina rozpoczęcia</label>";
    s += "<input type='time' name='start' value='" + String(startTime) + "'>";
    s += "</div>";
    
    s += "<div class='form-group'>";
    s += "<label class='form-label'>Godzina zakończenia</label>";
    s += "<input type='time' name='stop' value='" + String(stopTime) + "'>";
    s += "</div>";
    
    // Link do aprs.fi
    if (objects[i].call.length() > 0) {
      s += "<div class='form-group'>";
      s += "<a href='https://aprs.fi/" + objects[i].call + "' target='_blank'>Otwórz w aprs.fi →</a>";
      s += "</div>";
    }
    
    String btnText = objects[i].name.length() > 0 ? objects[i].name : "obiekt " + String(i+1);
    s += "<button type='submit' class='btn'>💾 Zapisz " + btnText + "</button>";
    s += "</form>";
    
    s += "</div>";
  }
  
  // Passcode generator section
  s += "<div class='content-section' id='section_passcode'>";
  s += "<h2 style='color:#0f0;margin-bottom:20px;'>Generator hasła APRS-IS</h2>";
  s += "<p style='color:#aaa;margin-bottom:20px;'>Wygeneruj hasło dostępowe do sieci APRS-IS dla swojego znaku wywołania.</p>";
  s += "<p style='color:#f80;margin-bottom:20px;font-size:12px;'>⚠️ Nie używaj fałszywych znaków wywołania - możesz zostać zbanowany!</p>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Znak wywołania (np. SP3VSS)</label>";
  s += "<input type='text' id='passcode_call' placeholder='SP3VSS' class='converter-input' style='text-transform:uppercase;'>";
  s += "</div>";
  
  s += "<button onclick='generatePasscode()' class='btn'>Generuj hasło</button>";
  
  s += "<div id='passcode_result' class='passcode-result' style='display:none;'></div>";
  
  s += "</div>";
  
  // Converter section
  s += "<div class='content-section' id='section_converter'>";
  s += "<h2 style='color:#0f0;margin-bottom:20px;'>Konwerter współrzędnych</h2>";
  s += "<p style='color:#aaa;margin-bottom:20px;'>Konwertuj współrzędne z formatu Google Maps (DD.dddddd) do formatu APRS (DDMM.hhN)</p>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Szerokość geograficzna (np. 52.406000)</label>";
  s += "<input type='text' id='lat_dec' placeholder='52.406000' class='converter-input'>";
  s += "</div>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Długość geograficzna (np. 16.925000)</label>";
  s += "<input type='text' id='lon_dec' placeholder='16.925000' class='converter-input'>";
  s += "</div>";
  
  s += "<button onclick='convertCoords()' class='btn'>Konwertuj</button>";
  
  s += "<div id='result' class='converter-result' style='display:none;'></div>";
  
  s += "</div>";
  
  // Settings section
  s += "<div class='content-section' id='section_settings'>";
  s += "<h2 style='color:#0f0;margin-bottom:20px;'>Ustawienia systemowe</h2>";
  
  s += "<div class='warning'>⚠️ Po zapisaniu zmian urządzenie zostanie zrestartowane!</div>";
  
  s += "<form method='POST' action='/save_settings'>";
  
  s += "<h3 style='color:#fff;margin-bottom:15px;'>Ustawienia WiFi</h3>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Nazwa sieci WiFi (SSID)</label>";
  s += "<input type='text' name='wifi_ssid' value='" + wifiSsid + "' placeholder='Nazwa_WiFi'>";
  s += "</div>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Hasło WiFi</label>";
  s += "<input type='password' name='wifi_pass' value='" + wifiPassword + "' placeholder='Hasło'>";
  s += "</div>";
  
  s += "<div class='form-group'>";
  s += "<div class='checkbox-group'>";
  s += "<input type='checkbox' id='dhcp_enabled' name='dhcp' value='1' onchange='toggleDhcp()'";
  if (wifiUseDhcp) s += " checked";
  s += "><span>Użyj DHCP (automatyczne IP)</span>";
  s += "</div>";
  s += "</div>";
  
  s += "<div id='static_fields'>";
  
  s += "<h3 style='color:#fff;margin:20px 0 15px 0;'>Ustawienia statycznego IP</h3>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Adres IP (np. 192.168.1.50)</label>";
  s += "<input type='text' name='ip' value='" + wifiLocalIp.toString() + "' placeholder='192.168.1.50'>";
  s += "</div>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Maska podsieci (np. 255.255.255.0)</label>";
  s += "<input type='text' name='subnet' value='" + wifiSubnet.toString() + "' placeholder='255.255.255.0'>";
  s += "</div>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Brama (Gateway) (np. 192.168.1.1)</label>";
  s += "<input type='text' name='gateway' value='" + wifiGateway.toString() + "' placeholder='192.168.1.1'>";
  s += "</div>";
  
  s += "</div>";
  
  s += "<h3 style='color:#fff;margin:20px 0 15px 0;'>Ustawienia czasu (NTP)</h3>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Serwer NTP</label>";
  s += "<input type='text' name='ntp_server' value='" + ntpServer + "' placeholder='pool.ntp.org'>";
  s += "<p style='color:#aaa;font-size:11px;margin-top:5px;'>Przykłady: pool.ntp.org, time.google.com, pl.pool.ntp.org</p>";
  s += "</div>";
  
  s += "<div class='form-group'>";
  s += "<label class='form-label'>Przesunięcie strefy czasowej GMT (godziny)</label>";
  s += "<input type='text' name='gmt_offset' value='" + String(gmtOffsetSec / 3600) + "' placeholder='1' style='width:100px;'>";
  s += "<p style='color:#aaa;font-size:11px;margin-top:5px;'>Dla Polski: 1 (zimą) lub 2 (latem)</p>";
  s += "</div>";
  
  s += "<button type='submit' class='btn btn-danger' style='margin-top:20px;'>💾 Zapisz i zrestartuj</button>";
  s += "</form>";
  
  s += "</div>";
  
  s += "</div></td>";
  s += "</tr>";
  
  s += "</table>";
  
  s += "</body></html>";
  return s;
}

// ===================== HANDLERY WWW =====================

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleSave() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  if (!server.hasArg("obj")) {
    server.send(400, "text/plain", "Missing object index");
    return;
  }

  int objIndex = server.arg("obj").toInt();
  if (objIndex < 0 || objIndex >= 5) {
    server.send(400, "text/plain", "Invalid object index");
    return;
  }

  Serial.println("Zapisywanie obiektu " + String(objIndex+1));

  if (server.hasArg("name")) {
    objects[objIndex].name = server.arg("name");
    objects[objIndex].name.trim();
  }
  if (server.hasArg("call")) {
    objects[objIndex].call = server.arg("call");
    objects[objIndex].call.trim();
    objects[objIndex].call.toUpperCase();
  }
  if (server.hasArg("pass")) {
    objects[objIndex].passcode = server.arg("pass");
    objects[objIndex].passcode.trim();
  }
  if (server.hasArg("sym")) {
    objects[objIndex].symbol = server.arg("sym");
    objects[objIndex].symbol.trim();
  }
  if (server.hasArg("lat")) {
    objects[objIndex].latitude = server.arg("lat");
    objects[objIndex].latitude.trim();
    objects[objIndex].latitude.toUpperCase();
  }
  if (server.hasArg("lon")) {
    objects[objIndex].longitude = server.arg("lon");
    objects[objIndex].longitude.trim();
    objects[objIndex].longitude.toUpperCase();
  }
  if (server.hasArg("com")) {
    objects[objIndex].comment = server.arg("com");
    objects[objIndex].comment.trim();
  }
  if (server.hasArg("int")) {
    objects[objIndex].interval = server.arg("int").toInt();
  }
  
  objects[objIndex].scheduleEnabled = server.hasArg("sch_en");
  
  for (int d = 0; d < 7; d++) {
    objects[objIndex].scheduleDays[d] = server.hasArg("day_" + String(d));
  }
  
  if (server.hasArg("start")) {
    String startTime = server.arg("start");
    int colonPos = startTime.indexOf(':');
    if (colonPos > 0) {
      objects[objIndex].scheduleStartHour = startTime.substring(0, colonPos).toInt();
      objects[objIndex].scheduleStartMinute = startTime.substring(colonPos + 1).toInt();
    }
  }
  
  if (server.hasArg("stop")) {
    String stopTime = server.arg("stop");
    int colonPos = stopTime.indexOf(':');
    if (colonPos > 0) {
      objects[objIndex].scheduleStopHour = stopTime.substring(0, colonPos).toInt();
      objects[objIndex].scheduleStopMinute = stopTime.substring(colonPos + 1).toInt();
    }
  }

  saveConfigToNVS();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveSettings() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  Serial.println("=== Zapisywanie ustawień systemowych ===");

  if (server.hasArg("wifi_ssid")) {
    wifiSsid = server.arg("wifi_ssid");
    wifiSsid.trim();
  }
  
  if (server.hasArg("wifi_pass")) {
    wifiPassword = server.arg("wifi_pass");
  }
  
  wifiUseDhcp = server.hasArg("dhcp");
  
  if (server.hasArg("ip")) {
    wifiLocalIp.fromString(server.arg("ip"));
  }
  
  if (server.hasArg("subnet")) {
    wifiSubnet.fromString(server.arg("subnet"));
  }
  
  if (server.hasArg("gateway")) {
    wifiGateway.fromString(server.arg("gateway"));
  }
  
  if (server.hasArg("ntp_server")) {
    ntpServer = server.arg("ntp_server");
    ntpServer.trim();
  }
  
  if (server.hasArg("gmt_offset")) {
    int hours = server.arg("gmt_offset").toInt();
    gmtOffsetSec = hours * 3600;
  }

  saveConfigToNVS();

  String response = "<html><head><meta charset='UTF-8'></head><body style='background:#000;color:#fff;font-family:Arial;text-align:center;padding:50px;'>";
  response += "<h1 style='color:#0f0;'>Ustawienia zapisane!</h1>";
  response += "<p>Urządzenie zostanie zrestartowane za 5 sekund...</p>";
  response += "<p>Nowy adres może się zmienić, sprawdź monitor Serial.</p>";
  response += "<script>setTimeout(function(){window.location.href='/';},10000);</script>";
  response += "</body></html>";
  
  server.send(200, "text/html", response);
  
  delay(2000);
  ESP.restart();
}

void handleStatusData() {
  String json = "{\"objects\":[";
  
  for (int i = 0; i < 5; i++) {
    if (i > 0) json += ",";
    json += "{";
    
    String st;
    if (objects[i].lastSent == 0) {
      st = "–";
    } else {
      st = objects[i].lastOk ? "<span class='status-ok'>✓</span>" : "<span class='status-error'>✗</span>";
    }
    
    json += "\"status\":\"" + st + "\",";
    json += "\"time\":\"" + formatTimeSince(objects[i].lastSent) + "\"";
    json += "}";
  }
  
  json += "],\"datetime\":\"" + getCurrentDateTime() + "\"}";
  server.send(200, "application/json", json);
}

void handleGeneratePasscode() {
  if (!server.hasArg("call")) {
    server.send(400, "application/json", "{\"error\":\"Missing callsign\"}");
    return;
  }
  
  String callsign = server.arg("call");
  callsign.trim();
  callsign.toUpperCase();
  
  uint16_t passcode = generateAprsPasscode(callsign);
  
  String json = "{\"call\":\"" + callsign + "\",\"passcode\":" + String(passcode) + "}";
  server.send(200, "application/json", json);
}

// ===================== WIFI + NTP =====================

void setupWiFi() {
  delay(100);
  Serial.println("=== ESP32 APRS OBJECT SENDER ===");

  if (!wifiUseDhcp) {
    WiFi.config(wifiLocalIp, wifiGateway, wifiSubnet);
  }

  WiFi.mode(WIFI_MODE_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  Serial.print("Łączenie do ");
  Serial.println(wifiSsid);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Połączono. IP: http://");
    Serial.println(WiFi.localIP());
  }
}

void setupNTP() {
  configTime(gmtOffsetSec, DAYLIGHT_OFFSET_SEC, ntpServer.c_str());
  Serial.println("Konfiguracja NTP zakończona.");
  Serial.println("NTP Server: " + ntpServer);
  Serial.println("GMT Offset: " + String(gmtOffsetSec / 3600) + "h");
}

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(115200);
  delay(2000);

  // Inicjalizacja LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  loadConfigFromNVS();
  setupWiFi();
  setupNTP();

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/save_settings", handleSaveSettings);
  server.on("/status_data", handleStatusData);
  server.on("/generate_passcode", handleGeneratePasscode);
  server.begin();
  
  Serial.println("Serwer HTTP uruchomiony.");
}

void loop() {
  server.handleClient();
  
  // Obsługa LED
  updateLED();

  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    }
  }

  time_t now = getNow();

  for (int i = 0; i < 5; i++) {
    if (objects[i].call.length() == 0) continue;
    if (objects[i].passcode.length() == 0) continue;
    if (objects[i].interval == 0) continue;

    if (!isScheduleActive(objects[i])) continue;

    bool shouldSend = false;
    if (objects[i].lastSent == 0) {
      shouldSend = true;
    } else if (now >= objects[i].lastSent) {
      uint32_t elapsed = now - objects[i].lastSent;
      if (elapsed >= objects[i].interval) {
        shouldSend = true;
      }
    }

    if (shouldSend) {
      Serial.println("*** WYSYŁANIE OBIEKTU " + String(i+1) + " ***");
      aprsSendObject(objects[i]);
      delay(2000);
    }
  }
}
