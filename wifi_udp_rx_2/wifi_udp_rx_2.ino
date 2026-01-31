#include <WiFi.h>
#include <WiFiUdp.h>

const char* SSID = "DATACAST_TX";
const char* PASS = "12345678";
const uint16_t PORT = 12345;

static const uint16_t MAGIC = 0xDCA5;

WiFiUDP udp;

#pragma pack(push, 1)
struct Header {
  uint16_t magic;
  uint16_t testId;
  uint32_t seq;
  uint32_t tx_us;
};
#pragma pack(pop)

uint8_t buf[1600];

// Estadísticas
bool haveOffset = false;
int32_t offset_us = 0; // offset estimado: rx_us_first - tx_us_first

uint32_t lastSeq = 0;
bool haveSeq = false;

uint64_t bytesWindow = 0;
uint32_t recvWindow = 0;
uint32_t lostWindow = 0;

int32_t minLat =  2147483647;
int32_t maxLat = -2147483647;
int64_t sumLat = 0;

uint32_t windowStartMs = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("=== RX INICIANDO ===");
  
  // Limpiar configuración WiFi
  WiFi.mode(WIFI_OFF);
  delay(500);
  
  WiFi.mode(WIFI_STA);
  delay(500);
  
  WiFi.disconnect(true);
  delay(500);
  
  // Intentar conexión
  Serial.print("Conectando a: ");
  Serial.println(SSID);
  Serial.print("Con password: ");
  Serial.println(PASS);
  
  WiFi.begin(SSID, PASS);
  WiFi.setSleep(false);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    if (attempts % 10 == 0) {
      Serial.println();
      Serial.print("Intento "); Serial.print(attempts);
      Serial.print(" - Status: "); Serial.println(WiFi.status());
      
      // Reintentar conexión cada 20 intentos
      if (attempts == 20) {
        Serial.println("Reintentando...");
        WiFi.disconnect();
        delay(1000);
        WiFi.begin(SSID, PASS);
      }
    }
  }
  
  Serial.println();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: No se pudo conectar!");
    Serial.print("Status final: ");
    Serial.println(WiFi.status());
    
    // Escanear redes disponibles para debug
    Serial.println("\nEscaneando redes disponibles:");
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.println(" dBm)");
    }
    
    while(1) { delay(1000); }
  }

  Serial.println("\n¡CONECTADO!");
  Serial.print("IP local: ");
  Serial.println(WiFi.localIP());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  udp.begin(PORT);
  windowStartMs = millis();
  
  Serial.println("\nEscuchando UDP en puerto " + String(PORT) + "...");
  Serial.println("==================");
}
void printAndResetWindow() {
  uint32_t nowMs = millis();
  uint32_t dtMs = nowMs - windowStartMs;
  if (dtMs == 0) dtMs = 1;

  // Throughput (bits/s)
  double bps = (bytesWindow * 8.0) / (dtMs / 1000.0);

  // Pérdida (%)
  uint32_t total = recvWindow + lostWindow;
  double lossPct = (total == 0) ? 0.0 : (100.0 * lostWindow / (double)total);

  // “Latencia relativa” promedio (us)
  double avgLat = (recvWindow == 0) ? 0.0 : (sumLat / (double)recvWindow);

  Serial.println("---- Ventana ----");
  Serial.print("Recv: "); Serial.print(recvWindow);
  Serial.print("  Lost(est): "); Serial.print(lostWindow);
  Serial.print("  Loss%: "); Serial.println(lossPct, 2);

  Serial.print("Throughput: "); Serial.print(bps / 1000.0, 2); Serial.println(" kbps");
  Serial.print("LatRel(us) min/avg/max: ");
  Serial.print(minLat); Serial.print(" / ");
  Serial.print(avgLat, 1); Serial.print(" / ");
  Serial.println(maxLat);

  // Reset ventana
  bytesWindow = 0;
  recvWindow = 0;
  lostWindow = 0;
  minLat =  2147483647;
  maxLat = -2147483647;
  sumLat = 0;
  windowStartMs = nowMs;
}

void loop() {
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    int len = udp.read(buf, sizeof(buf));
    if (len >= (int)sizeof(Header)) {
      Header h;
      memcpy(&h, buf, sizeof(h));
      if (h.magic == MAGIC) {
        // pérdida estimada por gaps
        if (!haveSeq) {
          haveSeq = true;
          lastSeq = h.seq;
        } else {
          uint32_t expected = lastSeq + 1;
          if (h.seq > expected) {
            lostWindow += (h.seq - expected);
          }
          lastSeq = h.seq;
        }

        // offset para latencia relativa (unilateral)
        uint32_t rx_us = (uint32_t)micros();
        if (!haveOffset) {
          offset_us = (int32_t)(rx_us - h.tx_us);
          haveOffset = true;
        }
        int32_t latRel = (int32_t)((int32_t)rx_us - offset_us - (int32_t)h.tx_us);
        // Esto refleja jitter/variación + drift, no latencia absoluta real.
        if (latRel < minLat) minLat = latRel;
        if (latRel > maxLat) maxLat = latRel;
        sumLat += latRel;

        recvWindow++;
        bytesWindow += (uint64_t)len;
      }
    }
  }

  // imprimir cada ~1s
  if (millis() - windowStartMs >= 1000) {
    printAndResetWindow();
  }
}

