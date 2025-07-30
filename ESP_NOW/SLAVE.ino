#include <ESP8266WiFi.h>
extern "C" {
  #include <espnow.h>
}

/* Definitions */
#define WIFI_CHANNEL 6

/* Global Variables */
std::vector<uint8_t*> masterList;

/* Callback: When data is received */
void onDataReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.printf("Received message from %s\n", macStr);
  Serial.printf("  Message: %.*s\n", len, data);

  // Check if it's a new master
  bool known = false;
  for (auto &storedMac : masterList) {
    if (memcmp(mac, storedMac, 6) == 0) {
      known = true;
      break;
    }
  }

  if (!known) {
    Serial.println("New master detected. Registering...");

    // Allocate memory and store MAC
    uint8_t* newMac = new uint8_t[6];
    memcpy(newMac, mac, 6);
    masterList.push_back(newMac);

    // Add peer (even for broadcast; this step is optional for receiving only)
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, WIFI_CHANNEL, NULL, 0);
    Serial.printf("Master %s registered. Total: %d\n", macStr, masterList.size());
  }
}

void setup() {
  Serial.begin(115200);

  // Set Wi-Fi to station mode and set channel
  WiFi.mode(WIFI_STA);
  wifi_promiscuous_enable(0);
  wifi_set_channel(WIFI_CHANNEL);

  Serial.println("ESP8266 ESP-NOW Broadcast Slave");
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Listening on channel %d...\n", WIFI_CHANNEL);

  // Initialize ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Set receive callback
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataReceive);
}

void loop() {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 10000) {
    lastPrint = millis();
    Serial.printf("Registered masters: %d\n", masterList.size());
    for (size_t i = 0; i < masterList.size(); ++i) {
      uint8_t *mac = masterList[i];
      Serial.printf("  Master %zu: %02X:%02X:%02X:%02X:%02X:%02X\n", i,
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
  }

  delay(100);
}
