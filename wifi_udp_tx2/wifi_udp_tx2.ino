#include <WiFi.h>
#include <WiFiUdp.h>

const char* AP_SSID = "DATACAST_TX";
const char* AP_PASS = "12345678";

const uint16_t PORT = 12345;
IPAddress BCAST(192, 168, 4, 255);

// ====== Parámetros de prueba ======
static const uint16_t MAGIC = 0xDCA5;
static const uint16_t TEST_ID = 1;

// Ojo: UDP típico MTU ~1472 bytes payload IP. Usa 200..1200 para ir seguro.
static const uint16_t PAYLOAD_BYTES = 800;   // bytes de payload útil
static const uint16_t PPS = 200;             // paquetes por segundo (200 => 5ms por paquete)

// ==================================
WiFiUDP udp;
uint32_t seq = 0;

#pragma pack(push, 1)
struct Header {
  uint16_t magic;
  uint16_t testId;
  uint32_t seq;
  uint32_t tx_us;
};
#pragma pack(pop)

uint8_t packet[sizeof(Header) + PAYLOAD_BYTES];

void setup() {
  Serial.begin(115200);
  delay(2000);  // Espera más tiempo para estabilizar
  
  // Apagar WiFi primero
  WiFi.mode(WIFI_OFF);
  delay(500);
  
  // Configurar modo AP
  WiFi.mode(WIFI_AP);
  delay(500);
  
  // Crear AP sin configuración avanzada primero
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(1000);  // Dar tiempo a que se establezca
  
  WiFi.setSleep(false);
  
  Serial.println("=== TX INICIADO ===");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("PASS: ");
  Serial.println(AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Canal: ");
  Serial.println(WiFi.channel());
  
  udp.begin(PORT);
  Serial.print("Puerto UDP: ");
  Serial.println(PORT);

  // relleno fijo
  for (uint16_t i = 0; i < PAYLOAD_BYTES; i++) {
    packet[sizeof(Header) + i] = (uint8_t)(i & 0xFF);
  }
  
  Serial.println("TX listo!");
  Serial.println("==================");
}

void loop() {
  Header h;
  h.magic = MAGIC;
  h.testId = TEST_ID;
  h.seq = seq++;
  h.tx_us = (uint32_t)micros();

  memcpy(packet, &h, sizeof(h));

  udp.beginPacket(BCAST, PORT);
  udp.write(packet, sizeof(packet));
  udp.endPacket();

  // Control de ritmo: PPS paquetes/s => periodo en microsegundos
  const uint32_t period_us = 1000000UL / PPS;
  static uint32_t last = micros();
  while ((uint32_t)(micros() - last) < period_us) { /* busy wait */ }
  last += period_us;
}
