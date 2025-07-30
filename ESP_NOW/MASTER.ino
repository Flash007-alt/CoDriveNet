#include <esp_now.h>
#include <WiFi.h>

// Broadcast MAC address (FF:FF:FF:FF:FF:FF = send to all peers)
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Data structure
typedef struct struct_message {
  int id;
  float battery;
} struct_message;

struct_message dataToSend;

// Send callback for IDF 5.x+
void onSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  Serial.print("Sent to: ");
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           info->des_addr[0], info->des_addr[1], info->des_addr[2],
           info->des_addr[3], info->des_addr[4], info->des_addr[5]);
  Serial.print(macStr);
  Serial.print(" | Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);

  // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // Optional, clean start

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register send callback
  esp_now_register_send_cb(onSent);

  // Register broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

  Serial.println("ESP-NOW Master ready");
}

void loop() {
  dataToSend.id = 1;
  dataToSend.battery = random(300, 420) / 100.0;

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &dataToSend, sizeof(dataToSend));

  if (result == ESP_OK) {
    Serial.println("Sent successfully");
  } else {
    Serial.println("Send error");
  }

  delay(1000);
}
