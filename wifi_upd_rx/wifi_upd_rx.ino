#include <WiFi.h>
#include <WiFiUdp.h>

const char* SSID = "DATACAST_TX";
const char* PASS = "12345678";
const uint16_t PORT = 12345;

WiFiUDP udp;

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASS);
  WiFi.setSleep(false);

  Serial.print("Conectando");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("STA IP: ");
  Serial.println(WiFi.localIP());

  udp.begin(PORT);
  Serial.println("Escuchando UDP...");
}

void loop() {
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    char buf[256];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len > 0) buf[len] = '\0';

    Serial.print("RX (");
    Serial.print(packetSize);
    Serial.print("B) desde ");
    Serial.print(udp.remoteIP());
    Serial.print(": ");
    Serial.println(buf);
  }
}
