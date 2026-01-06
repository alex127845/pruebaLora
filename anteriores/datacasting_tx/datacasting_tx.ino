#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14

#define CHUNK_SIZE 240
#define HANDSHAKE_TIMEOUT 5000
#define PACKET_REDUNDANCY 2

#define METADATA_MAGIC_1 0x4C
#define METADATA_MAGIC_2 0x4D
#define CMD_HANDSHAKE_REQUEST 0xA1
#define CMD_HANDSHAKE_RESPONSE 0xA2

const char* ssid = "LoRa-TX-Broadcast";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool handshakeReceived = false;
volatile bool transmitting = false;
volatile bool waitingHandshake = false;
String currentFile = "";
String transmissionStatus = "";

float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;
int currentRedundancy = 2;

unsigned long transmissionStartTime = 0;
unsigned long transmissionEndTime = 0;
float lastTransmissionTime = 0;
uint32_t lastFileSize = 0;
float lastSpeed = 0;
uint16_t totalPacketsSent = 0;

float handshakeRSSI = 0;
float handshakeSNR = 0;

void IRAM_ATTR setFlag(void) {
  if (waitingHandshake) {
    handshakeReceived = true;
  }
}

// ✅ CÁLCULO DE TIME-ON-AIR (ToA)
float calculateToA(int payloadSize) {
  // Símbolo de duración
  float Tsym = (pow(2, currentSF) / (currentBW * 1000.0)) * 1000.0; // en ms
  
  // Preámbulo
  int nPreamble = 8; // RadioLib usa 8 por defecto
  float Tpreamble = (nPreamble + 4.25) * Tsym;
  
  // Payload
  int DE = 0; // Low Data Rate Optimization (solo si SF11/12 con BW=125)
  if (currentSF >= 11 && currentBW == 125.0) DE = 1;
  
  int H = 0;  // Header explícito
  int CRC = 1; // CRC habilitado
  
  float tmp = (8.0 * payloadSize - 4.0 * currentSF + 28.0 + 16.0 * CRC - 20.0 * H) / 
              (4.0 * (currentSF - 2.0 * DE));
  
  int nPayload = 8 + max((int)ceil(tmp) * (currentCR + 4), 0);
  float Tpayload = nPayload * Tsym;
  
  return Tpreamble + Tpayload; // en ms
}

// ✅ DELAY INTELIGENTE (ToA + margen de seguridad)
int getSmartDelay() {
  float toaData = calculateToA(CHUNK_SIZE + 4); // 4 bytes de header
  float processingTime = 30; // Tiempo estimado de procesamiento en RX (ms)
  float margin = toaData * 0.2; // 20% de margen
  
  int smartDelay = (int)(processingTime + margin);
  
  // Límites seguros
  if (smartDelay < 50) smartDelay = 50;
  if (smartDelay > 300) smartDelay = 300;
  
  return smartDelay;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== TRANSMISOR LoRa DATACASTING OPTIMIZADO v9 ===");
  Serial.println("📡 Modo: BROADCAST con cálculo de ToA");
  Serial.println("🔄 Redundancia: 2-3x por paquete\n");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");
  listFiles();

  Serial.println("\n📡 Configurando WiFi AP...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP iniciado\n");
  Serial.printf("   SSID: %s\n", ssid);
  Serial.printf("   IP: %s\n\n", IP.toString().c_str());

  setupWebServer();

  Serial.println("Iniciando radio...");
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    while (true) delay(1000);
  }
  
  applyLoRaConfig();

  Serial.println("✅ Radio configurado");
  Serial.printf("🌐 Interfaz web: http://%s\n\n", IP.toString().c_str());
}

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando nueva configuración LoRa...");
  
  radio.standby();
  delay(100);
  
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  delay(100);
  
  // ✅ Mostrar cálculos
  float toaMeta = calculateToA(100); // Metadatos (~100 bytes)
  float toaData = calculateToA(CHUNK_SIZE + 4);
  int smartDelay = getSmartDelay();
  
  Serial.println("📻 Configuración LoRa:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   Redundancia: %dx\n", currentRedundancy);
  Serial.printf("\n⏱️  Tiempos calculados:\n");
  Serial.printf("   ToA Metadatos: %.1f ms\n", toaMeta);
  Serial.printf("   ToA Datos: %.1f ms\n", toaData);
  Serial.printf("   Delay inteligente: %d ms\n", smartDelay);
  Serial.println("✅ Radio configurado correctamente\n");
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa TX Broadcast v9</title>";
    html += "<style>";
    html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
    html += "body { font-family: 'Segoe UI', sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }";
    html += ".container { max-width: 1000px; margin: 0 auto; background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }";
    html += "h1 { color: #333; border-bottom: 3px solid #667eea; padding-bottom: 15px; margin-bottom: 25px; }";
    html += ".broadcast-badge { background: linear-gradient(135deg, #667eea, #764ba2); color: white; padding: 10px 20px; border-radius: 20px; display: inline-block; margin-bottom: 20px; font-weight: bold; }";
    html += ".section { background: #f8f9fa; padding: 20px; border-radius: 10px; margin: 20px 0; }";
    html += ".section h2 { color: #667eea; margin-bottom: 15px; }";
    html += ".config-badge { display: inline-block; background: #667eea; color: white; padding: 5px 12px; border-radius: 15px; margin: 5px; font-size: 0.85em; }";
    html += ".stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin: 15px 0; }";
    html += ".stat-box { background: white; padding: 15px; border-radius: 8px; text-align: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ".stat-value { font-size: 2em; font-weight: bold; color: #667eea; }";
    html += ".stat-label { color: #666; font-size: 0.9em; margin-top: 5px; }";
    html += ".speed-highlight { background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%); color: white; }";
    html += ".toa-box { background: linear-gradient(135deg, #4ECDC4 0%, #44A08D 100%); color: white; }";
    html += "button { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; margin: 5px; }";
    html += ".btn-send { background: #28a745; color: white; }";
    html += ".btn-delete { background: #dc3545; color: white; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>📡 LoRa TX Broadcast v9</h1>";
    html += "<div class='broadcast-badge'>🚀 OPTIMIZADO - CÁLCULO ToA - BUFFER INTELIGENTE</div>";
    
    // Config actual
    html += "<div class='section'><h2>⚙️ Configuración</h2>";
    html += "<span class='config-badge'>BW: " + String((int)currentBW) + " kHz</span>";
    html += "<span class='config-badge'>SF: " + String(currentSF) + "</span>";
    html += "<span class='config-badge'>CR: 4/" + String(currentCR) + "</span>";
    html += "<span class='config-badge'>Redundancia: " + String(currentRedundancy) + "x</span>";
    html += "</div>";
    
    // Cálculos ToA
    float toaData = 0;
    if (currentBW > 0 && currentSF > 0) {
      float Tsym = (pow(2, currentSF) / (currentBW * 1000.0)) * 1000.0;
      int DE = (currentSF >= 11 && currentBW == 125.0) ? 1 : 0;
      float tmp = (8.0 * (CHUNK_SIZE + 4) - 4.0 * currentSF + 28.0 + 16.0 - 0) / (4.0 * (currentSF - 2.0 * DE));
      int nPayload = 8 + max((int)ceil(tmp) * (currentCR + 4), 0);
      toaData = ((8 + 4.25) * Tsym) + (nPayload * Tsym);
    }
    
    html += "<div class='section'><h2>⏱️ Tiempos Calculados</h2>";
    html += "<div class='stats'>";
    html += "<div class='stat-box toa-box'><div class='stat-value'>" + String(toaData, 1) + "</div><div class='stat-label'>ToA (ms)</div></div>";
    html += "</div></div>";
    
    // Stats
    if (lastTransmissionTime > 0) {
      html += "<div class='section'><h2>📊 Última Transmisión</h2>";
      html += "<div class='stats'>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastTransmissionTime, 2) + "s</div><div class='stat-label'>Tiempo</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastFileSize) + "</div><div class='stat-label'>Bytes</div></div>";
      html += "<div class='stat-box speed-highlight'><div class='stat-value'>" + String(lastSpeed, 2) + "</div><div class='stat-label'>kbps ⚡</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalPacketsSent) + "</div><div class='stat-label'>Paquetes</div></div>";
      if (handshakeRSSI != 0) {
        html += "<div class='stat-box toa-box'><div class='stat-value'>" + String(handshakeRSSI, 1) + "</div><div class='stat-label'>RSSI</div></div>";
        html += "<div class='stat-box toa-box'><div class='stat-value'>" + String(handshakeSNR, 1) + "</div><div class='stat-label'>SNR</div></div>";
      }
      html += "</div></div>";
    }
    
    // Archivos
    html += "<div class='section'><h2>📁 Archivos</h2><ul>";
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String fileName = String(file.name());
        String displayName = fileName.startsWith("/") ? fileName.substring(1) : fileName;
        html += "<li>📄 " + displayName + " (" + String(file.size()) + " bytes) ";
        if (!transmitting) {
          html += "<button class='btn-send' onclick='sendFile(\"" + fileName + "\")'>📡 TX</button>";
          html += "<button class='btn-delete' onclick='deleteFile(\"" + fileName + "\")'>🗑️</button>";
        }
        html += "</li>";
      }
      file = root.openNextFile();
    }
    html += "</ul></div>";
    
    html += "<script>";
    html += "function sendFile(name) { if(confirm('TX?')) fetch('/send?file=' + encodeURIComponent(name)).then(() => location.reload()); }";
    html += "function deleteFile(name) { if(confirm('Del?')) fetch('/delete?file=' + encodeURIComponent(name)).then(() => location.reload()); }";
    html += "</script></div></body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/send", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (!filename.startsWith("/")) filename = "/" + filename;
      if (LittleFS.exists(filename)) {
        currentFile = filename;
        transmitting = true;
        transmissionStatus = "";
        totalPacketsSent = 0;
        handshakeRSSI = 0;
        handshakeSNR = 0;
        request->send(200, "text/plain", "OK");
      } else {
        request->send(404, "text/plain", "Not found");
      }
    }
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (!filename.startsWith("/")) filename = "/" + filename;
      LittleFS.remove(filename) ? request->send(200, "text/plain", "OK") : request->send(500, "text/plain", "Error");
    }
  });

  server.begin();
}

void loop() {
  if (transmitting) {
    Serial.printf("\n📡 TX BROADCAST: %s\n", currentFile.c_str());
    transmissionStartTime = millis();
    
    bool result = sendFile(currentFile.c_str());
    
    transmissionEndTime = millis();
    lastTransmissionTime = (transmissionEndTime - transmissionStartTime) / 1000.0;
    lastSpeed = (lastFileSize * 8.0) / (lastTransmissionTime * 1000.0);
    
    if (result) {
      Serial.println("\n╔════════════════════════════════╗");
      Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps      ║\n", lastSpeed);
      Serial.printf("║  📡 RSSI: %.1f dBm            ║\n", handshakeRSSI);
      Serial.printf("║  📊 SNR: %.1f dB              ║\n", handshakeSNR);
      Serial.println("╚════════════════════════════════╝");
    }
    
    transmitting = false;
    currentFile = "";
  }
  
  yield();
  delay(100);
}

void listFiles() {
  Serial.println("\n📁 Archivos:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int count = 0;
  while (file) {
    if (!file.isDirectory()) {
      Serial.printf("  - %s (%d bytes)\n", file.name(), file.size());
      count++;
    }
    file = root.openNextFile();
  }
  if (count == 0) Serial.println("  (vacío)");
}

bool performHandshake() {
  Serial.println("\n🤝 HANDSHAKE");
  
  uint8_t handshakeReq[3] = {CMD_HANDSHAKE_REQUEST, 0xBE, 0xEF};
  if (radio.transmit(handshakeReq, 3) != RADIOLIB_ERR_NONE) {
    Serial.println("   ❌ Error");
    return false;
  }
  
  Serial.println("   📤 Solicitud enviada");
  delay(300);
  
  waitingHandshake = true;
  handshakeReceived = false;
  radio.setDio1Action(setFlag);
  
  if (radio.startReceive() != RADIOLIB_ERR_NONE) {
    waitingHandshake = false;
    return false;
  }
  
  unsigned long startWait = millis();
  bool success = false;
  
  while (millis() - startWait < HANDSHAKE_TIMEOUT && !success) {
    if (handshakeReceived) {
      handshakeReceived = false;
      
      uint8_t responseBuffer[10];
      if (radio.readData(responseBuffer, 10) == RADIOLIB_ERR_NONE) {
        if (radio.getPacketLength() >= 3 && 
            responseBuffer[0] == CMD_HANDSHAKE_RESPONSE &&
            responseBuffer[1] == 0xBE &&
            responseBuffer[2] == 0xEF) {
          
          handshakeRSSI = radio.getRSSI();
          handshakeSNR = radio.getSNR();
          
          Serial.println("   ✅ OK");
          Serial.printf("   📡 RSSI: %.1f dBm\n", handshakeRSSI);
          Serial.printf("   📊 SNR: %.1f dB\n", handshakeSNR);
          
          if (handshakeRSSI < -120) {
            Serial.println("   ⚠️  SEÑAL MUY DÉBIL");
          } else if (handshakeRSSI < -100) {
            Serial.println("   ⚠️  SEÑAL DÉBIL");
          } else {
            Serial.println("   ✅ SEÑAL BUENA");
          }
          
          success = true;
        }
      }
      if (!success) radio.startReceive();
    }
    delayMicroseconds(5000);
  }
  
  waitingHandshake = false;
  radio.standby();
  
  if (!success) {
    Serial.println("   ⚠️  Sin respuesta");
  }
  
  return true;
}

bool sendFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint32_t totalSize = f.size();
  lastFileSize = totalSize;
  
  String fileName = String(path);
  if (fileName.startsWith("/")) fileName = fileName.substring(1);
  
  Serial.printf("📁 %s (%u bytes)\n", fileName.c_str(), totalSize);
  
  // Handshake
  performHandshake();
  delay(500);
  
  // Metadatos
  Serial.println("\n📤 Metadatos...");
  uint8_t nameLen = min((size_t)fileName.length(), (size_t)100);
  uint8_t metaPkt[7 + nameLen];
  metaPkt[0] = METADATA_MAGIC_1;
  metaPkt[1] = METADATA_MAGIC_2;
  memcpy(metaPkt + 2, &totalSize, 4);
  metaPkt[6] = nameLen;
  memcpy(metaPkt + 7, fileName.c_str(), nameLen);
  
  for (int r = 0; r < currentRedundancy; r++) {
    radio.transmit(metaPkt, 7 + nameLen);
    Serial.printf("✅ Meta %d/%d\n", r+1, currentRedundancy);
    if (r < currentRedundancy - 1) delay(150);
  }
  
  delay(600);
  
  // Datos
  uint16_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  Serial.printf("📦 %u fragmentos (%dx redundancia)\n\n", totalChunks, currentRedundancy);

  int smartDelay = getSmartDelay();

  for (uint16_t index = 0; index < totalChunks; index++) {
    uint8_t buffer[CHUNK_SIZE];
    size_t bytesRead = f.read(buffer, CHUNK_SIZE);
    if (bytesRead == 0) break;

    uint8_t pkt[4 + bytesRead];
    memcpy(pkt, &index, 2);
    memcpy(pkt + 2, &totalChunks, 2);
    memcpy(pkt + 4, buffer, bytesRead);

    Serial.printf("📤 [%u/%u] ", index + 1, totalChunks);
    
    int successCount = 0;
    for (int r = 0; r < currentRedundancy; r++) {
      if (radio.transmit(pkt, 4 + bytesRead) == RADIOLIB_ERR_NONE) {
        successCount++;
      }
      if (r < currentRedundancy - 1) delay(50);
    }
    
    Serial.printf("✅ %d/%d OK\n", successCount, currentRedundancy);
    totalPacketsSent += successCount;

    delay(smartDelay);
  }

  f.close();
  
  Serial.printf("\n🎉 COMPLETO (%u paquetes enviados)\n", totalPacketsSent);
  return true;
}