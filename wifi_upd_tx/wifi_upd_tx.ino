#include <WiFi.h>
#include <WiFiUdp.h>

const char* AP_SSID = "DATACAST_TX";
const char* AP_PASS = "12345678";          // mínimo 8 chars
const uint16_t PORT = 12345;

WiFiUDP udp;
IPAddress bcast(192, 168, 4, 255);         // broadcast típico del SoftAP 192.168.4.0/24
uint32_t counter = 0;

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);

  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(ip);

  // No es obligatorio "begin" para enviar, pero ayuda a fijar un puerto local
  udp.begin(PORT);
}

void loop() {
  char msg[64];
  snprintf(msg, sizeof(msg), "HELLO_BCAST #%lu", (unsigned long)counter++);

  udp.beginPacket(bcast, PORT);
  udp.write((const uint8_t*)msg, strlen(msg));
  udp.endPacket();

  Serial.println(msg);
  delay(200);
}
