#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

// Pines Heltec WiFi LoRa 32 V3
#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14

#define MAX_PACKET_SIZE 300
#define RX_TIMEOUT 60000         // ✅ Timeout más largo (broadcast puede tener pausas)
#define MAX_CHUNKS 512           // ✅ Máximo chunks soportados (ajustar según RAM)
#define FEC_BLOCK_SIZE 8

// ✅ MAGIC BYTES para broadcast
#define MANIFEST_MAGIC_1 0xAA
#define MANIFEST_MAGIC_2 0xBB
#define DATA_MAGIC_1 0xCC
#define DATA_MAGIC_2 0xDD
#define PARITY_MAGIC_1 0xEE
#define PARITY_MAGIC_2 0xFF
#define FILE_END_MAGIC_1 0x99
#define FILE_END_MAGIC_2 0x88

#define VEXT 36
#define VEXT_ON LOW

const char* ssid = "LoRa-RX-Broadcast";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool receivedFlag = false;

// ✅ Estructura para rastrear sesión de archivo
struct FileSession {
  uint32_t fileID = 0;
  String fileName = "";
  uint32_t totalSize = 0;
  uint16_t totalChunks = 0;
  uint16_t chunkSize = 0;
  bool active = false;
  unsigned long lastPacketTime = 0;
  unsigned long startTime = 0;
  
  // Buffer de chunks recibidos
  bool* chunkReceived = nullptr;      // Array de flags
  uint8_t** chunkData = nullptr;       // Array de punteros a datos
  uint16_t* chunkLengths = nullptr;    // Tamaños de cada chunk
  uint16_t chunksReceivedCount = 0;
  
  // Buffer FEC
  bool* parityReceived = nullptr;
  uint8_t** parityData = nullptr;
  uint16_t* parityLengths = nullptr;
};

FileSession currentSession;

// Parámetros LoRa configurables
float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;

// Estadísticas
float lastReceptionTime = 0;
float lastSpeed = 0;
uint32_t lastFileSize = 0;
uint32_t totalPacketsReceived = 0;
uint32_t totalCrcErrors = 0;
uint32_t totalRecovered = 0;

// ============================================
// ✅ CRC16-CCITT
// ============================================
uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc = crc << 1;
    }
  }
  return crc;
}

void enableVext(bool on) {
  pinMode(VEXT, OUTPUT);
  digitalWrite(VEXT, on ?  VEXT_ON : ! VEXT_ON);
}

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

// ============================================
// ✅ INICIALIZAR SESIÓN
// ============================================
bool initFileSession(uint32_t fileID, String fileName, uint32_t totalSize, uint16_t totalChunks, uint16_t chunkSize) {
  // Liberar sesión anterior si existe
  freeFileSession();
  
  if (totalChunks > MAX_CHUNKS) {
    Serial.printf("❌ Demasiados chunks: %u (max %u)\n", totalChunks, MAX_CHUNKS);
    return false;
  }
  
  currentSession.fileID = fileID;
  currentSession.fileName = fileName;
  currentSession.totalSize = totalSize;
  currentSession.totalChunks = totalChunks;
  currentSession.chunkSize = chunkSize;
  currentSession.active = true;
  currentSession. lastPacketTime = millis();
  currentSession.startTime = millis();
  currentSession.chunksReceivedCount = 0;
  
  // Alocar memoria para buffers
  currentSession.chunkReceived = (bool*)calloc(totalChunks, sizeof(bool));
  currentSession.chunkData = (uint8_t**)calloc(totalChunks, sizeof(uint8_t*));
  currentSession.chunkLengths = (uint16_t*)calloc(totalChunks, sizeof(uint16_t));
  
  uint16_t numParityBlocks = (totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  currentSession.parityReceived = (bool*)calloc(numParityBlocks, sizeof(bool));
  currentSession.parityData = (uint8_t**)calloc(numParityBlocks, sizeof(uint8_t*));
  currentSession.parityLengths = (uint16_t*)calloc(numParityBlocks, sizeof(uint16_t));
  
  if (!currentSession.chunkReceived || !currentSession.chunkData || !currentSession.chunkLengths ||
      !currentSession.parityReceived || !currentSession.parityData || !currentSession.parityLengths) {
    Serial.println("❌ Error de memoria");
    freeFileSession();
    return false;
  }
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.printf("║  🆔 File ID: 0x%08X\n", fileID);
  Serial.printf("║  📁 Archivo: %s\n", fileName. c_str());
  Serial.printf("║  📊 Tamaño: %u bytes\n", totalSize);
  Serial.printf("║  📦 Chunks: %u\n", totalChunks);
  Serial.println("╚════════════════════════════════════════╝");
  
  return true;
}

// ============================================
// ✅ LIBERAR SESIÓN
// ============================================
void freeFileSession() {
  if (currentSession. chunkData) {
    for (uint16_t i = 0; i < currentSession.totalChunks; i++) {
      if (currentSession.chunkData[i]) {
        free(currentSession.chunkData[i]);
      }
    }
    free(currentSession.chunkData);
  }
  
  if (currentSession.parityData) {
    uint16_t numBlocks = (currentSession.totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
    for (uint16_t i = 0; i < numBlocks; i++) {
      if (currentSession.parityData[i]) {
        free(currentSession.parityData[i]);
      }
    }
    free(currentSession.parityData);
  }
  
  if (currentSession.chunkReceived) free(currentSession.chunkReceived);
  if (currentSession. chunkLengths) free(currentSession. chunkLengths);
  if (currentSession.parityReceived) free(currentSession.parityReceived);
  if (currentSession.parityLengths) free(currentSession. parityLengths);
  
  currentSession.active = false;
  currentSession.fileID = 0;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa BROADCAST v7 (UNILATERAL) ===");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");

  Serial.println("\n📡 Configurando WiFi AP.. .");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi. softAPIP();
  Serial.printf("✅ WiFi AP iniciado\n");
  Serial.printf("   SSID: %s\n", ssid);
  Serial.printf("   IP: %s\n\n", IP.toString().c_str());

  setupWebServer();

  Serial.println("Iniciando radio SX1262...");
  enableVext(true);
  delay(200);
  int state = radio.begin(915. 0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    while (true) delay(1000);
  }
  
  applyLoRaConfig();
  radio.setDio1Action(setFlag);
  
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error en startReceive: %d\n", state);
  }

  Serial.println("✅ Radio configurado (MODO RX ONLY - SIN ACK)");
  Serial.println("👂 Escuchando broadcast LoRa...");
  Serial.printf("🌐 Servidor web:  http://%s\n\n", IP.toString().c_str());
}

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando configuración LoRa BROADCAST...");
  
  radio.standby();
  delay(100);
  
  int state = radio.setSpreadingFactor(currentSF);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Error SF: %d\n", state);
  }
  
  state = radio.setBandwidth(currentBW);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Error BW: %d\n", state);
  }
  
  state = radio.setCodingRate(currentCR);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Error CR: %d\n", state);
  }
  
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  delay(100);
  
  Serial.println("📻 Configuración LoRa:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF:  %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.println("✅ Radio listo (RX ONLY)\n");
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa RX Broadcast v7</title>";
    html += "<style>";
    html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
    html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background:  linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }";
    html += ".container { max-width: 1000px; margin: 0 auto; background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }";
    html += "h1 { color: #333; border-bottom: 3px solid #667eea; padding-bottom: 15px; margin-bottom: 25px; }";
    html += ". section { background: #f8f9fa; padding: 20px; border-radius: 10px; margin: 20px 0; }";
    html += ".section h2 { color: #667eea; margin-bottom: 15px; font-size: 1.3em; }";
    html += ".lora-config { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }";
    html += ".param-group { background: white; padding: 15px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.05); }";
    html += ".param-group label { display: block; font-weight: bold; color: #333; margin-bottom:  8px; font-size: 0.9em; }";
    html += ". param-group select { width: 100%; padding: 10px; border: 2px solid #667eea; border-radius: 5px; font-size: 14px; background: white; cursor: pointer; }";
    html += ".btn-apply { background: #667eea; color: white; padding: 12px 30px; font-size: 16px; border: none; border-radius: 5px; cursor: pointer; margin-top: 15px; width: 100%; font-weight: bold; }";
    html += ".btn-apply:hover { background: #5568d3; }";
    html += ".current-config { background: #e7f3ff; padding: 15px; border-radius: 8px; margin-bottom: 15px; }";
    html += ".config-badge { display: inline-block; background: #667eea; color: white; padding: 5px 12px; border-radius: 15px; margin:  5px; font-size: 0.85em; }";
    html += ".broadcast-badge { background: #ff6b6b; }";
    
    html += ". stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; margin:  15px 0; }";
    html += ".stat-box { background: white; padding: 15px; border-radius: 8px; text-align: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ". stat-value { font-size: 1.8em; font-weight: bold; color: #667eea; }";
    html += ".stat-label { color: #666; font-size: 0.85em; margin-top: 5px; }";
    html += ".speed-highlight { background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%); color: white; }";
    
    html += ". file-list { list-style: none; }";
    html += ".file-item { background: white; padding:  15px; margin: 10px 0; border-radius:  8px; display: flex; justify-content: space-between; align-items: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ".file-info { flex-grow: 1; }";
    html += ".file-name { font-weight: bold; color:  #333; font-size: 1.1em; }";
    html += ".file-size { color: #666; font-size: 0.9em; margin-top: 5px; }";
    html += ".btn-group { display: flex; gap: 10px; }";
    html += "button { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 14px; transition: all 0.3s; }";
    html += ". btn-download { background: #28a745; color: white; }";
    html += ".btn-download:hover { background: #218838; }";
    html += ".btn-delete { background: #dc3545; color: white; }";
    html += ".btn-delete:hover { background: #c82333; }";
    html += ".btn-refresh { background: #667eea; color: white; margin-top: 20px; width: 100%; padding: 12px; font-weight: bold; }";
    html += ".btn-refresh:hover { background: #5568d3; }";
    html += ".empty { text-align: center; color: #999; padding: 30px; }";
    html += ".receiving { background: #fff3cd; padding: 15px; border-radius: 8px; margin: 15px 0; color: #856404; border-left: 4px solid #ffc107; }";
    html += ".progress-container { background: #f0f0f0; border-radius: 10px; overflow: hidden; margin: 15px 0; }";
    html += ".progress-bar { background: linear-gradient(90deg, #667eea, #764ba2); height: 30px; transition: width 0.3s; display: flex; align-items: center; justify-content: center; color: white; font-weight: bold; }";
    html += ". device-info { background: #e7f3ff; padding: 15px; border-radius: 8px; margin-bottom: 20px; font-size: 0.9em; text-align: center; }";
    html += ".warning { background: #fff3cd; border-left: 4px solid #ffc107; padding: 15px; margin: 15px 0; color: #856404; border-radius: 5px; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>🛰️ LoRa RX Broadcast v7</h1>";
    
    html += "<div class='device-info'>";
    html += "📟 <strong>Heltec WiFi LoRa 32 V3</strong> | 📡 <strong>SX1262</strong> | 📶 <strong>915 MHz</strong>";
    html += "</div>";

    html += "<div class='warning'>";
    html += "📻 <strong>MODO BROADCAST: </strong> Recepción unilateral sin ACK.  Compatible con transmisiones multipunto.";
    html += "</div>";
    
    // Configuración LoRa
    html += "<div class='section'>";
    html += "<h2>⚙️ Configuración LoRa</h2>";
    html += "<div class='current-config'>";
    html += "<strong>Configuración Actual:</strong><br>";
    html += "<span class='config-badge'>BW: " + String((int)currentBW) + " kHz</span>";
    html += "<span class='config-badge'>SF: " + String(currentSF) + "</span>";
    html += "<span class='config-badge'>CR:  4/" + String(currentCR) + "</span>";
    html += "<span class='config-badge broadcast-badge'>BROADCAST MODE</span>";
    html += "</div>";
    
    if (! currentSession.active) {
      html += "<form class='lora-config'>";
      html += "<div class='param-group'><label>📶 Bandwidth (kHz)</label><select name='bw'>";
      html += "<option value='125'" + String(currentBW == 125.0 ? " selected" : "") + ">125</option>";
      html += "<option value='250'" + String(currentBW == 250.0 ? " selected" : "") + ">250</option>";
      html += "<option value='500'" + String(currentBW == 500.0 ? " selected" : "") + ">500</option>";
      html += "</select></div>";
      html += "<div class='param-group'><label>📡 Spreading Factor</label><select name='sf'>";
      html += "<option value='7'" + String(currentSF == 7 ? " selected" : "") + ">7</option>";
      html += "<option value='9'" + String(currentSF == 9 ? " selected" :  "") + ">9</option>";
      html += "<option value='12'" + String(currentSF == 12 ? " selected" :  "") + ">12</option>";
      html += "</select></div>";
      html += "<div class='param-group'><label>🔧 Coding Rate</label><select name='cr'>";
      html += "<option value='5'" + String(currentCR == 5 ? " selected" : "") + ">4/5</option>";
      html += "<option value='7'" + String(currentCR == 7 ? " selected" :  "") + ">4/7</option>";
      html += "<option value='8'" + String(currentCR == 8 ? " selected" : "") + ">4/8</option>";
      html += "</select></div>";
      html += "</form>";
      html += "<button class='btn-apply' onclick='applyConfig()'>✅ Aplicar</button>";
    } else {
      html += "<p style='text-align: center; color:#856404;'>🔒 Bloqueado durante recepción</p>";
    }
    html += "</div>";
    
    // Progreso
    if (currentSession.active && currentSession.totalChunks > 0) {
      float progress = (currentSession.chunksReceivedCount * 100.0) / currentSession.totalChunks;
      html += "<div class='receiving'>📡 Recibiendo:  <strong>" + currentSession.fileName + "</strong></div>";
      html += "<div class='progress-container'>";
      html += "<div class='progress-bar' style='width: " + String(progress, 1) + "%'>";
      html += String(currentSession.chunksReceivedCount) + " / " + String(currentSession.totalChunks) + " chunks (" + String(progress, 1) + "%)";
      html += "</div></div>";
    }
    
    // Estadísticas
    if (lastReceptionTime > 0) {
      html += "<div class='section'>";
      html += "<h2>📊 Última Recepción</h2>";
      html += "<div class='stats'>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastReceptionTime, 2) + "s</div><div class='stat-label'>Tiempo</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastFileSize) + "</div><div class='stat-label'>Bytes</div></div>";
      html += "<div class='stat-box speed-highlight'><div class='stat-value'>" + String(lastSpeed, 2) + "</div><div class='stat-label'>kbps ⚡</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalPacketsReceived) + "</div><div class='stat-label'>Paquetes</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalCrcErrors) + "</div><div class='stat-label'>CRC Err</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalRecovered) + "</div><div class='stat-label'>FEC Fix</div></div>";
      html += "</div></div>";
    }
    
    // Lista de archivos
    html += "<div class='section'>";
    html += "<h2>📁 Archivos Recibidos</h2>";
    
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    bool hasFiles = false;
    
    html += "<ul class='file-list'>";
    while (file) {
      if (! file.isDirectory()) {
        hasFiles = true;
        String fullPath = String(file.name());
        String displayName = fullPath;
        if (displayName.startsWith("/")) displayName = displayName.substring(1);
        
        html += "<li class='file-item'>";
        html += "<div class='file-info'><div class='file-name'>📄 " + displayName + "</div>";
        html += "<div class='file-size'>" + String(file.size()) + " bytes</div></div>";
        html += "<div class='btn-group'>";
        html += "<button class='btn-download' onclick='location.href=\"/download? file=" + displayName + "\"'>📥</button>";
        html += "<button class='btn-delete' onclick='if(confirm(\"¿Eliminar?\")) location.href=\"/delete? file=" + displayName + "\"'>🗑️</button>";
        html += "</div></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul>";
    
    if (! hasFiles) {
      html += "<div class='empty'>Sin archivos. <br>Esperando broadcast...</div>";
    }
    
    html += "</div>";
    html += "<button class='btn-refresh' onclick='location.reload()'>🔄 Actualizar</button>";
    
    html += "<script>";
    html += "function applyConfig() {";
    html += "  const bw = document.querySelector('select[name=bw]').value;";
    html += "  const sf = document.querySelector('select[name=sf]').value;";
    html += "  const cr = document.querySelector('select[name=cr]').value;";
    html += "  fetch(`/config?bw=${bw}&sf=${sf}&cr=${cr}`).then(() => { alert('✅ OK'); location.reload(); });";
    html += "}";
    
    if (currentSession.active) {
      html += "setTimeout(() => location.reload(), 3000);";
    }
    
    html += "</script>";
    html += "</div></body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("bw") && request->hasParam("sf") && request->hasParam("cr")) {
      if (currentSession.active) {
        request->send(400, "text/plain", "No cambiar durante RX");
        return;
      }
      
      currentBW = request->getParam("bw")->value().toFloat();
      currentSF = request->getParam("sf")->value().toInt();
      currentCR = request->getParam("cr")->value().toInt();
      
      applyLoRaConfig();
      
      radio.setDio1Action(setFlag);
      int state = radio.startReceive();
      if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("❌ Error startReceive: %d\n", state);
      }
      
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Faltan parámetros");
    }
  });

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (!filename.startsWith("/")) filename = "/" + filename;
      
      if (LittleFS.exists(filename)) {
        String contentType = "application/octet-stream";
        if (filename. endsWith(".pdf")) contentType = "application/pdf";
        else if (filename. endsWith(".txt")) contentType = "text/plain";
        else if (filename. endsWith(".jpg") || filename.endsWith(".jpeg")) contentType = "image/jpeg";
        else if (filename. endsWith(".png")) contentType = "image/png";
        
        request->send(LittleFS, filename, contentType, true);
      } else {
        request->send(404, "text/plain", "No encontrado");
      }
    }
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (!filename.startsWith("/")) filename = "/" + filename;
      
      if (LittleFS.remove(filename)) {
        request->redirect("/");
      } else {
        request->send(500, "text/plain", "Error");
      }
    }
  });

  server.begin();
  Serial.println("✅ Web server iniciado");
}

void loop() {
  // ✅ Timeout de sesión
  if (currentSession. active && (millis() - currentSession.lastPacketTime) > RX_TIMEOUT) {
    Serial.println("\n⚠️  TIMEOUT - Intentando recuperar con FEC.. .");
    attemptFECRecovery();
    finalizeFile();
  }
  
  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();
      
      // ✅ Verificar CRC16 (últimos 2 bytes)
      if (packetLen < 4) {
        radio.startReceive();
        return;
      }
      
      uint16_t crcRecv, crcCalc;
      memcpy(&crcRecv, buffer + packetLen - 2, 2);
      crcCalc = crc16_ccitt(buffer, packetLen - 2);
      
      if (crcRecv != crcCalc) {
        Serial.printf("❌ CRC error (calc: 0x%04X, recv: 0x%04X)\n", crcCalc, crcRecv);
        totalCrcErrors++;
        radio.startReceive();
        return;
      }
      
      totalPacketsReceived++;
      
      Serial.printf("📡 RX: %d bytes | RSSI: %. 1f | SNR: %.1f | ", 
                    packetLen, radio.getRSSI(), radio.getSNR());
      
      // ✅ Parsear por magic bytes
      if (buffer[0] == MANIFEST_MAGIC_1 && buffer[1] == MANIFEST_MAGIC_2) {
        Serial.println("MANIFEST");
        processManifest(buffer, packetLen);
      } 
      else if (buffer[0] == DATA_MAGIC_1 && buffer[1] == DATA_MAGIC_2) {
        Serial.println("DATA");
        processDataChunk(buffer, packetLen);
      }
      else if (buffer[0] == PARITY_MAGIC_1 && buffer[1] == PARITY_MAGIC_2) {
        Serial.println("PARITY");
        processParityChunk(buffer, packetLen);
      }
      else if (buffer[0] == FILE_END_MAGIC_1 && buffer[1] == FILE_END_MAGIC_2) {
        Serial.println("FILE_END");
        processFileEnd(buffer, packetLen);
      }
      else {
        Serial.println("UNKNOWN");
      }
      
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("❌ CRC Error (RadioLib)");
      totalCrcErrors++;
    }

    radio.startReceive();
  }
  
  yield();
}

// ============================================
// ✅ PROCESAR MANIFEST
// ============================================
void processManifest(uint8_t* data, size_t len) {
  if (len < 17) return;
  
  uint32_t fileID;
  uint32_t totalSize;
  uint16_t totalChunks;
  uint16_t chunkSize;
  uint8_t nameLen;
  
  size_t idx = 2;  // Saltar magic
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&totalSize, data + idx, 4); idx += 4;
  memcpy(&totalChunks, data + idx, 2); idx += 2;
  memcpy(&chunkSize, data + idx, 2); idx += 2;
  nameLen = data[idx++];
  
  if (nameLen == 0 || nameLen > 100 || len < (idx + nameLen + 2)) return;
  
  char nameBuf[101];
  memcpy(nameBuf, data + idx, nameLen);
  nameBuf[nameLen] = '\0';
  
  String fileName = String(nameBuf);
  if (! fileName.startsWith("/")) fileName = "/" + fileName;
  
  // ✅ Si ya hay sesión activa con mismo ID, ignorar (ya tenemos manifest)
  if (currentSession. active && currentSession.fileID == fileID) {
    Serial.println("   ℹ️  Manifest duplicado (ya iniciado)");
    return;
  }
  
  // ✅ Nueva sesión
  if (!currentSession.active) {
    initFileSession(fileID, fileName, totalSize, totalChunks, chunkSize);
  }
}

// ============================================
// ✅ PROCESAR DATA CHUNK
// ============================================
void processDataChunk(uint8_t* data, size_t len) {
  if (!currentSession.active || len < 12) return;
  
  uint32_t fileID;
  uint16_t chunkIndex;
  uint16_t totalChunks;
  
  size_t idx = 2;  // Saltar magic
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&chunkIndex, data + idx, 2); idx += 2;
  memcpy(&totalChunks, data + idx, 2); idx += 2;
  
  if (fileID != currentSession.fileID) {
    Serial.printf("   ⚠️  File ID mismatch (esperado: 0x%08X, recibido: 0x%08X)\n", 
                  currentSession.fileID, fileID);
    return;
  }
  
  if (chunkIndex >= currentSession.totalChunks) {
    Serial.printf("   ⚠️  Chunk index fuera de rango:  %u\n", chunkIndex);
    return;
  }
  
  currentSession.lastPacketTime = millis();
  
  // ✅ Ya recibido? 
  if (currentSession.chunkReceived[chunkIndex]) {
    Serial.printf("   📦 [%u/%u] DUPLICADO\n", chunkIndex + 1, totalChunks);
    return;
  }
  
  size_t dataLen = len - idx - 2;  // -2 para CRC
  
  // ✅ Guardar chunk en memoria
  currentSession.chunkData[chunkIndex] = (uint8_t*)malloc(dataLen);
  if (!currentSession.chunkData[chunkIndex]) {
    Serial.println("   ❌ Sin memoria");
    return;
  }
  
  memcpy(currentSession.chunkData[chunkIndex], data + idx, dataLen);
  currentSession.chunkLengths[chunkIndex] = dataLen;
  currentSession.chunkReceived[chunkIndex] = true;
  currentSession.chunksReceivedCount++;
  
  float progress = (currentSession.chunksReceivedCount * 100.0) / currentSession.totalChunks;
  Serial.printf("   📦 [%u/%u] %u bytes - %.1f%% completo\n", 
                chunkIndex + 1, totalChunks, dataLen, progress);
}

// ============================================
// ✅ PROCESAR PARITY CHUNK (FEC)
// ============================================
void processParityChunk(uint8_t* data, size_t len) {
  if (!currentSession.active || len < 10) return;
  
  uint32_t fileID;
  uint16_t blockIndex;
  
  size_t idx = 2;
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&blockIndex, data + idx, 2); idx += 2;
  
  if (fileID != currentSession.fileID) return;
  
  uint16_t numBlocks = (currentSession.totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  if (blockIndex >= numBlocks) return;
  
  currentSession.lastPacketTime = millis();
  
  if (currentSession.parityReceived[blockIndex]) {
    Serial.printf("   🛡️  Parity block %u DUPLICADO\n", blockIndex);
    return;
  }
  
  size_t dataLen = len - idx - 2;
  
  currentSession.parityData[blockIndex] = (uint8_t*)malloc(dataLen);
  if (!currentSession.parityData[blockIndex]) {
    Serial.println("   ❌ Sin memoria para parity");
    return;
  }
  
  memcpy(currentSession.parityData[blockIndex], data + idx, dataLen);
  currentSession.parityLengths[blockIndex] = dataLen;
  currentSession.parityReceived[blockIndex] = true;
  
  Serial.printf("   🛡️  Parity block %u guardado (%u bytes)\n", blockIndex, dataLen);
}

// ============================================
// ✅ PROCESAR FILE_END
// ============================================
void processFileEnd(uint8_t* data, size_t len) {
  if (!currentSession. active || len < 10) return;
  
  uint32_t fileID;
  uint16_t totalChunks;
  
  size_t idx = 2;
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&totalChunks, data + idx, 2); idx += 2;
  
  if (fileID != currentSession.fileID) return;
  
  Serial.println("\n🏁 FILE_END recibido - Finalizando.. .");
  
  attemptFECRecovery();
  finalizeFile();
}

// ============================================
// ✅ INTENTAR RECUPERACIÓN FEC
// ============================================
void attemptFECRecovery() {
  if (!currentSession.active) return;
  
  Serial.println("\n🔧 Intentando recuperación FEC...");
  
  uint16_t numBlocks = (currentSession.totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  
  for (uint16_t block = 0; block < numBlocks; block++) {
    uint16_t blockStart = block * FEC_BLOCK_SIZE;
    uint16_t blockEnd = min((uint16_t)(blockStart + FEC_BLOCK_SIZE), currentSession.totalChunks);
    
    // Contar chunks faltantes en este bloque
    int missingCount = 0;
    int missingIndex = -1;
    
    for (uint16_t i = blockStart; i < blockEnd; i++) {
      if (!currentSession.chunkReceived[i]) {
        missingCount++;
        missingIndex = i;
      }
    }
    
    // ✅ Solo recuperar si falta EXACTAMENTE 1 chunk y tenemos parity
    if (missingCount == 1 && currentSession.parityReceived[block]) {
      Serial.printf("   🛡️  Bloque %u: recuperando chunk %u.. .\n", block, missingIndex);
      
      size_t maxLen = currentSession.parityLengths[block];
      uint8_t* recovered = (uint8_t*)malloc(maxLen);
      if (!recovered) {
        Serial.println("   ❌ Sin memoria para recovery");
        continue;
      }
      
      memcpy(recovered, currentSession.parityData[block], maxLen);
      
      // XOR con todos los chunks recibidos del bloque
      for (uint16_t i = blockStart; i < blockEnd; i++) {
        if (currentSession.chunkReceived[i]) {
          size_t len = currentSession.chunkLengths[i];
          for (size_t j = 0; j < len && j < maxLen; j++) {
            recovered[j] ^= currentSession.chunkData[i][j];
          }
        }
      }
      
      // Guardar chunk recuperado
      currentSession.chunkData[missingIndex] = recovered;
      currentSession.chunkLengths[missingIndex] = maxLen;
      currentSession.chunkReceived[missingIndex] = true;
      currentSession.chunksReceivedCount++;
      totalRecovered++;
      
      Serial.printf("   ✅ Chunk %u recuperado con FEC!\n", missingIndex);
    }
  }
}

// ============================================
// ✅ FINALIZAR ARCHIVO
// ============================================
void finalizeFile() {
  if (!currentSession.active) return;
  
  Serial.println("\n📝 Escribiendo archivo...");
  
  // Eliminar si existe
  if (LittleFS.exists(currentSession.fileName)) {
    LittleFS.remove(currentSession.fileName);
  }
  
  File file = LittleFS.open(currentSession.fileName, "w");
  if (!file) {
    Serial.println("❌ Error abriendo archivo");
    freeFileSession();
    return;
  }
  
  uint32_t bytesWritten = 0;
  uint16_t chunksMissing = 0;
  
  for (uint16_t i = 0; i < currentSession.totalChunks; i++) {
    if (currentSession.chunkReceived[i]) {
      file.write(currentSession.chunkData[i], currentSession.chunkLengths[i]);
      bytesWritten += currentSession.chunkLengths[i];
    } else {
      chunksMissing++;
      Serial.printf("⚠️  Chunk %u FALTANTE\n", i);
    }
  }
  
  file.close();
  
  unsigned long endTime = millis();
  lastReceptionTime = (endTime - currentSession.startTime) / 1000.0;
  lastFileSize = bytesWritten;
  lastSpeed = (bytesWritten * 8. 0) / (lastReceptionTime * 1000.0);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🎉 ARCHIVO GUARDADO             ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("📁 %s\n", currentSession.fileName. c_str());
  Serial.printf("📊 %u / %u bytes (%.1f%%)\n", 
                bytesWritten, currentSession.totalSize,
                (bytesWritten * 100.0) / currentSession.totalSize);
  Serial.printf("📦 %u / %u chunks\n", 
                currentSession.chunksReceivedCount, currentSession. totalChunks);
  Serial.printf("⏱️  %. 2f segundos\n", lastReceptionTime);
  
  Serial.println("╔════════════════════════════════════════╗");
  Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps              ║\n", lastSpeed);
  Serial.println("╚════════════════════════════════════════╝");
  
  if (chunksMissing > 0) {
    Serial.printf("⚠️  %u chunks faltantes\n", chunksMissing);
  } else {
    Serial.println("✅ Archivo completo!");
  }
  
  if (totalRecovered > 0) {
    Serial.printf("🛡️  %u chunks recuperados con FEC\n", totalRecovered);
  }
  
  Serial.printf("🌐 http://%s\n\n", WiFi.softAPIP().toString().c_str());
  
  freeFileSession();
}