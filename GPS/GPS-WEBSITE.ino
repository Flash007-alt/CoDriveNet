/*********
 * ESP32 GPS Tracker - WiFi Client Mode
 * Connects to existing WiFi network for remote access
 *********/

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <TinyGPS++.h>
#include <ESPmDNS.h>

//----------- Wi-Fi Credentials -----------
// Replace with your WiFi network credentials
const char* ssid = "Pi";
const char* password = "alenalbin991";

// Optional: Set a custom hostname (you can access via http://gps-tracker.local)
const char* hostname = "gps-tracker";
//-----------------------------------------

#define RXD2 16
#define TXD2 17
#define GPS_BAUD 9600

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);
AsyncWebServer server(80);

// Global GPS variables
double latitude = 0.0, longitude = 0.0;
double speed_kmph = 0.0, altitude_m = 0.0;
double hdop = 0.0;
uint32_t satellites = 0;
String date_str = "N/A", time_str = "N/A";
bool location_valid = false;
int current_rate = 1;

// GPS path tracking
struct GPSPoint {
  double lat, lng;
  unsigned long timestamp;
};

const int MAX_PATH_POINTS = 100;
GPSPoint gpsPath[MAX_PATH_POINTS];
int pathIndex = 0;
int pathCount = 0;

void addGPSPoint(double lat, double lng) {
  gpsPath[pathIndex] = {lat, lng, millis()};
  pathIndex = (pathIndex + 1) % MAX_PATH_POINTS;
  if (pathCount < MAX_PATH_POINTS) pathCount++;
}

void setGpsRate(int rate_hz) {
  if (rate_hz < 1 || rate_hz > 10) {
    Serial.println("Invalid GPS rate. Must be between 1-10 Hz");
    return;
  }
  
  uint16_t measRate = 1000 / rate_hz;
  uint8_t payload[] = {
    (uint8_t)(measRate & 0xFF), (uint8_t)(measRate >> 8),
    0x01, 0x00, 0x01, 0x00
  };
  
  uint8_t ubx_command[sizeof(payload) + 8];
  ubx_command[0] = 0xB5; ubx_command[1] = 0x62;
  ubx_command[2] = 0x06; ubx_command[3] = 0x08;
  ubx_command[4] = sizeof(payload); ubx_command[5] = 0x00;
  
  memcpy(ubx_command + 6, payload, sizeof(payload));
  
  uint8_t ck_a = 0, ck_b = 0;
  for (int i = 2; i < sizeof(ubx_command) - 2; i++) {
    ck_a += ubx_command[i]; ck_b += ck_a;
  }
  ubx_command[sizeof(ubx_command) - 2] = ck_a;
  ubx_command[sizeof(ubx_command) - 1] = ck_b;
  
  gpsSerial.write(ubx_command, sizeof(ubx_command));
  Serial.printf("GPS rate set to %d Hz\n", rate_hz);
  delay(100);
}

String getJsonData() {
  String json = "{";
  json += "\"lat\":" + String(latitude, 8) + ",";
  json += "\"lng\":" + String(longitude, 8) + ",";
  json += "\"alt\":" + String(altitude_m, 2) + ",";
  json += "\"spd\":" + String(speed_kmph, 2) + ",";
  json += "\"sat\":" + String(satellites) + ",";
  json += "\"hdop\":" + String(hdop, 2) + ",";
  json += "\"date\":\"" + date_str + "\",";
  json += "\"time\":\"" + time_str + "\",";
  json += "\"valid\":" + String(location_valid ? "true" : "false") + ",";
  json += "\"rate\":" + String(current_rate) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  return json;
}

String getPathData() {
  String json = "[";
  int start = (pathCount < MAX_PATH_POINTS) ? 0 : pathIndex;
  for (int i = 0; i < pathCount; i++) {
    int idx = (start + i) % MAX_PATH_POINTS;
    if (i > 0) json += ",";
    json += "[" + String(gpsPath[idx].lat, 8) + "," + String(gpsPath[idx].lng, 8) + "]";
  }
  json += "]";
  return json;
}

void handleRoot(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE HTML><html><head>";
  html += "<title>ESP32 Live GPS Tracker</title>";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<link rel=\"stylesheet\" href=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.css\" />";
  html += "<script src=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.js\"></script>";
  html += "<style>";
  html += "* { box-sizing: border-box; margin: 0; padding: 0; }";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; height: 100vh; }";
  html += "#map { width: 100%; height: 70vh; }";
  html += ".controls { position: fixed; top: 10px; right: 10px; z-index: 1000; }";
  html += ".control-btn { background: white; border: 2px solid #ccc; padding: 8px 12px; margin: 2px; border-radius: 4px; cursor: pointer; box-shadow: 0 2px 4px rgba(0,0,0,0.2); }";
  html += ".control-btn:hover { background: #f0f0f0; }";
  html += ".info-panel { height: 30vh; padding: 15px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; overflow-y: auto; }";
  html += ".title { text-align: center; font-size: 1.5em; font-weight: bold; margin-bottom: 15px; text-shadow: 0 1px 2px rgba(0,0,0,0.3); }";
  html += ".data-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 10px; }";
  html += ".data-card { background: rgba(255,255,255,0.15); padding: 12px; border-radius: 8px; backdrop-filter: blur(10px); border: 1px solid rgba(255,255,255,0.2); }";
  html += ".data-label { font-size: 0.85em; opacity: 0.9; margin-bottom: 4px; }";
  html += ".data-value { font-size: 1.3em; font-weight: bold; }";
  html += ".status-card { grid-column: 1 / -1; text-align: center; padding: 15px; }";
  html += ".status-good { background: rgba(40, 167, 69, 0.3); }";
  html += ".status-bad { background: rgba(220, 53, 69, 0.3); }";
  html += ".rate-control { margin-top: 10px; }";
  html += ".rate-control select { padding: 8px; border-radius: 4px; border: none; background: white; color: #333; }";
  html += "@media (max-width: 768px) { .data-grid { grid-template-columns: 1fr 1fr; } }";
  html += "</style></head><body>";
  
  html += "<div id=\"map\"></div>";
  html += "<div class=\"controls\">";
  html += "<button class=\"control-btn\" onclick=\"centerOnGPS()\">📍 Center</button>";
  html += "<button class=\"control-btn\" onclick=\"togglePath()\">🛤️ Path</button>";
  html += "<button class=\"control-btn\" onclick=\"clearPath()\">🗑️ Clear</button>";
  html += "</div>";
  
  html += "<div class=\"info-panel\">";
  html += "<div class=\"title\">🛰️ Live GPS Tracker</div>";
  html += "<div class=\"data-grid\">";
  html += "<div class=\"data-card\"><div class=\"data-label\">📍 Latitude</div><div class=\"data-value\" id=\"lat\">0.000000°</div></div>";
  html += "<div class=\"data-card\"><div class=\"data-label\">📍 Longitude</div><div class=\"data-value\" id=\"lng\">0.000000°</div></div>";
  html += "<div class=\"data-card\"><div class=\"data-label\">🛰️ Satellites</div><div class=\"data-value\" id=\"sat\">0</div></div>";
  html += "<div class=\"data-card\"><div class=\"data-label\">📊 HDOP</div><div class=\"data-value\" id=\"hdop\">0.0</div></div>";
  html += "<div class=\"data-card\"><div class=\"data-label\">🏃 Speed</div><div class=\"data-value\" id=\"spd\">0.0 km/h</div></div>";
  html += "<div class=\"data-card\"><div class=\"data-label\">⛰️ Altitude</div><div class=\"data-value\" id=\"alt\">0.0 m</div></div>";
  html += "<div class=\"data-card\"><div class=\"data-label\">📶 WiFi Signal</div><div class=\"data-value\" id=\"rssi\">0 dBm</div></div>";
  html += "<div class=\"data-card\"><div class=\"data-label\">⏱️ Uptime</div><div class=\"data-value\" id=\"uptime\">0s</div></div>";
  html += "<div class=\"data-card status-card\" id=\"statusCard\">";
  html += "<div id=\"status\" class=\"data-value\">🔍 Searching for GPS...</div>";
  html += "<div class=\"rate-control\">Update Rate: ";
  html += "<select id=\"rateSelector\" onchange=\"setRate()\">";
  html += "<option value=\"1\">1 Hz</option><option value=\"2\">2 Hz</option><option value=\"5\">5 Hz</option><option value=\"10\">10 Hz</option>";
  html += "</select></div></div></div></div>";
  
  // JavaScript
  html += "<script>";
  html += "var map = L.map('map').setView([8.48, 76.95], 13);";
  html += "var marker = null; var pathPolyline = null; var showPath = true; var firstFix = true;";
  
  html += "L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {";
  html += "maxZoom: 19, attribution: '&copy; OpenStreetMap contributors'}).addTo(map);";
  
  html += "var customIcon = L.divIcon({";
  html += "className: 'custom-marker', html: '<div style=\"background: #4285f4; width: 20px; height: 20px; border-radius: 50%; border: 3px solid white; box-shadow: 0 2px 6px rgba(0,0,0,0.3);\"></div>',";
  html += "iconSize: [20, 20], iconAnchor: [10, 10]});";
  
  html += "function setRate() {";
  html += "var rate = document.getElementById('rateSelector').value;";
  html += "fetch('/setrate?rate=' + rate).then(r => r.text()).then(console.log); }";
  
  html += "function centerOnGPS() { if(marker) map.setView(marker.getLatLng(), 17); }";
  
  html += "function togglePath() { if(pathPolyline) { showPath = !showPath; pathPolyline.setStyle({opacity: showPath ? 0.7 : 0}); }}";
  
  html += "function clearPath() { fetch('/clearpath'); if(pathPolyline) pathPolyline.remove(); pathPolyline = null; }";
  
  html += "function updatePath() {";
  html += "fetch('/path').then(r => r.json()).then(data => {";
  html += "if(data.length > 1) { if(pathPolyline) pathPolyline.remove();";
  html += "pathPolyline = L.polyline(data, {color: '#ff4444', weight: 3, opacity: showPath ? 0.7 : 0}).addTo(map); }";
  html += "}).catch(console.error); }";
  
  html += "function updateData() {";
  html += "fetch('/data').then(r => r.json()).then(data => {";
  html += "document.getElementById('lat').textContent = data.lat.toFixed(6) + '°';";
  html += "document.getElementById('lng').textContent = data.lng.toFixed(6) + '°';";
  html += "document.getElementById('spd').textContent = data.spd.toFixed(1) + ' km/h';";
  html += "document.getElementById('alt').textContent = data.alt.toFixed(1) + ' m';";
  html += "document.getElementById('sat').textContent = data.sat;";
  html += "document.getElementById('hdop').textContent = data.hdop.toFixed(2);";
  html += "document.getElementById('rssi').textContent = data.rssi + ' dBm';";
  html += "var hours = Math.floor(data.uptime / 3600); var mins = Math.floor((data.uptime % 3600) / 60);";
  html += "document.getElementById('uptime').textContent = hours + 'h ' + mins + 'm';";
  html += "document.getElementById('rateSelector').value = data.rate;";
  
  html += "var statusEl = document.getElementById('status'); var cardEl = document.getElementById('statusCard');";
  html += "if(data.valid && data.lat != 0 && data.lng != 0) {";
  html += "statusEl.innerHTML = '✅ GPS FIX ACQUIRED<br><small>' + data.date + ' ' + data.time + ' UTC</small>';";
  html += "cardEl.className = 'data-card status-card status-good';";
  html += "var pos = [data.lat, data.lng];";
  html += "if(!marker) { marker = L.marker(pos, {icon: customIcon}).addTo(map); }";
  html += "else { marker.setLatLng(pos); }";
  html += "if(firstFix) { map.setView(pos, 16); firstFix = false; }";
  html += "} else {";
  html += "statusEl.innerHTML = '🔍 SEARCHING FOR GPS<br><small>Satellites: ' + data.sat + '</small>';";
  html += "cardEl.className = 'data-card status-card status-bad'; }";
  html += "}).catch(e => { console.error('Update failed:', e);";
  html += "document.getElementById('status').textContent = '❌ CONNECTION ERROR'; }); }";
  
  html += "updateData(); setInterval(updateData, 1000); setInterval(updatePath, 5000);";
  html += "</script></body></html>";
  
  request->send(200, "text/html", html);
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, password);
  
  Serial.println("Connecting to WiFi: " + String(ssid));
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.println("IP Address: " + WiFi.localIP().toString());
    Serial.println("Signal Strength: " + String(WiFi.RSSI()) + " dBm");
    
    // Setup mDNS for easy access
    if (MDNS.begin(hostname)) {
      Serial.println("mDNS started. Access via: http://" + String(hostname) + ".local");
      MDNS.addService("http", "tcp", 80);
    }
  } else {
    Serial.println("\nWiFi Connection Failed!");
    Serial.println("Please check your credentials and network availability.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32 Live GPS Tracker Starting ===");
  
  // Initialize GPS
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2);
  Serial.printf("GPS Serial initialized on pins RX:%d, TX:%d at %d baud\n", RXD2, TXD2, GPS_BAUD);
  
  delay(100);
  while(gpsSerial.available()) gpsSerial.read();
  
  // Connect to WiFi
  connectToWiFi();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot start web server without WiFi connection.");
    return;
  }

  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getJsonData());
  });
  
  server.on("/path", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getPathData());
  });
  
  server.on("/clearpath", HTTP_GET, [](AsyncWebServerRequest *request){
    pathCount = 0; pathIndex = 0;
    request->send(200, "text/plain", "Path cleared");
  });

  server.on("/setrate", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("rate")) {
      int rate = request->getParam("rate")->value().toInt();
      if (rate >= 1 && rate <= 10) {
        setGpsRate(rate);
        current_rate = rate;
        request->send(200, "text/plain", "Rate set to " + String(rate) + " Hz");
      } else {
        request->send(400, "text/plain", "Invalid rate (1-10 Hz)");
      }
    } else {
      request->send(400, "text/plain", "Missing rate parameter");
    }
  });

  server.begin();
  Serial.println("Web server started successfully!");
  Serial.println("=== Setup Complete ===\n");
  
  delay(1000);
  setGpsRate(current_rate);
}

void loop() {
  static unsigned long lastGpsTime = 0;
  static unsigned long gpsCount = 0;
  static unsigned long lastPathUpdate = 0;
  
  // Handle GPS data
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    if (gps.encode(c)) {
      gpsCount++;
      lastGpsTime = millis();
      
      if (gps.location.isValid()) {
        double newLat = gps.location.lat();
        double newLng = gps.location.lng();
        
        // Only update if position changed significantly (reduces noise)
        if (abs(newLat - latitude) > 0.000001 || abs(newLng - longitude) > 0.000001) {
          latitude = newLat;
          longitude = newLng;
          location_valid = true;
          
          // Add to path every 5 seconds when moving
          if (millis() - lastPathUpdate > 5000 && speed_kmph > 1.0) {
            addGPSPoint(latitude, longitude);
            lastPathUpdate = millis();
          }
        }
        
        if (gpsCount % 20 == 0) {
          Serial.printf("GPS Fix #%lu: %.8f, %.8f | Speed: %.1f km/h | Sats: %d\n", 
                       gpsCount, latitude, longitude, speed_kmph, satellites);
        }
      } else {
        location_valid = false;
      }

      // Update other GPS parameters
      if (gps.speed.isValid()) speed_kmph = gps.speed.kmph();
      if (gps.altitude.isValid()) altitude_m = gps.altitude.meters();
      if (gps.hdop.isValid()) hdop = gps.hdop.value() / 100.0;
      if (gps.satellites.isValid()) satellites = gps.satellites.value();

      if (gps.date.isValid() && gps.time.isValid()) {
        date_str = String(gps.date.year()) + "/" + 
                  String(gps.date.month()) + "/" + 
                  String(gps.date.day());
        
        char time_buf[10];
        sprintf(time_buf, "%02d:%02d:%02d", 
                gps.time.hour(), gps.time.minute(), gps.time.second());
        time_str = String(time_buf);
      }
    }
  }
  
  // WiFi connection monitoring
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) { // Check every 30 seconds
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, attempting to reconnect...");
      connectToWiFi();
    }
    lastWifiCheck = millis();
  }
  
  // GPS timeout warning
  if (millis() - lastGpsTime > 10000 && lastGpsTime != 0) {
    static unsigned long lastWarning = 0;
    if (millis() - lastWarning > 30000) {
      Serial.println("⚠️ No GPS data for 10+ seconds - check connections");
      lastWarning = millis();
    }
  }
  
  delay(10);
}