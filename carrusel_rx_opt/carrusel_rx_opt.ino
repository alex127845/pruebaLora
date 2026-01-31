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
#define RX_TIMEOUT 120000        // ✅ 120s para archivos grandes
#define MAX_CHUNKS 4096          // ✅ Soporta hasta 1MB (4096 * 240 bytes)
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

// ✅ NUEVO: Cooldown para evitar reprocesar mismo archivo
#define FILE_ID_COOLDOWN 30000  // 30s

const char* ssid = "LoRa-RX-Broadcast";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool receivedFlag = false;

// ✅ Estructura optimizada para archivos grandes
struct FileSession {
  uint32_t fileID = 0;
  String fileName = "";
  String tempFileName = "";
  uint32_t totalSize = 0;
  uint16_t totalChunks = 0;
  uint16_t chunkSize = 0;
  bool active = false;
  unsigned long lastPacketTime = 0;
  unsigned long startTime = 0;
  
  bool* chunkReceived = nullptr;
  uint16_t chunksReceivedCount = 0;
  
  struct FECBlock {
    bool received = false;
    uint8_t* data = nullptr;
    uint16_t length = 0;
  };
  FECBlock* parityBlocks = nullptr;
  uint16_t numParityBlocks = 0;
};

FileSession currentSession;

// ✅ NUEVO: Tracking de archivos procesados
uint32_t lastProcessedFileID = 0;
unsigned long lastFileCompletionTime = 0;

// ✅ NUEVO: Archivo temporal abierto
File currentTempFile;

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
uint32_t totalDuplicates = 0;

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
  digitalWrite(VEXT, on ? VEXT_ON : !VEXT_ON);
}

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

// ============================================
// ✅ INICIALIZAR SESIÓN (con archivo abierto)
// ============================================
bool initFileSession(uint32_t fileID, String fileName, uint32_t totalSize, uint16_t totalChunks, uint16_t chunkSize) {
  freeFileSession();
  
  if (totalChunks > MAX_CHUNKS) {
    Serial.printf("❌ Demasiados chunks: %u (max %u)\n", totalChunks, MAX_CHUNKS);
    return false;
  }
  
  currentSession.fileID = fileID;
  currentSession.fileName = fileName;
  currentSession.tempFileName = fileName + ".tmp";
  currentSession.totalSize = totalSize;
  currentSession.totalChunks = totalChunks;
  currentSession.chunkSize = chunkSize;
  currentSession.active = true;
  currentSession.lastPacketTime = millis();
  currentSession.startTime = millis();
  currentSession.chunksReceivedCount = 0;
  
  // ✅ Solo flags (1 byte por chunk)
  currentSession.chunkReceived = (bool*)calloc(totalChunks, sizeof(bool));
  
  // ✅ Buffer FEC
  currentSession.numParityBlocks = (totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  currentSession.parityBlocks = new FileSession::FECBlock[currentSession.numParityBlocks];
  
  if (!currentSession.chunkReceived || !currentSession.parityBlocks) {
    Serial.println("❌ Error de memoria");
    freeFileSession();
    return false;
  }
  
  // ✅ Crear archivo temporal vacío
  if (LittleFS.exists(currentSession.tempFileName)) {
    LittleFS.remove(currentSession.tempFileName);
  }
  
  File tempFile = LittleFS.open(currentSession.tempFileName, "w");
  if (!tempFile) {
    Serial.println("❌ Error creando archivo temporal");
    freeFileSession();
    return false;
  }
  
  // ✅ Pre-llenar con ceros
  uint8_t zeros[256];
  memset(zeros, 0, 256);
  uint32_t written = 0;
  while (written < totalSize) {
    size_t toWrite = min((uint32_t)256, totalSize - written);
    tempFile.write(zeros, toWrite);
    written += toWrite;
    yield();  // ✅ Evitar WDT
  }
  tempFile.close();
  
  // ✅ NUEVO: Abrir archivo en modo r+ para mantenerlo abierto
  currentTempFile = LittleFS.open(currentSession.tempFileName, "r+");
  if (!currentTempFile) {
    Serial.println("❌ Error abriendo temp file para escritura");
    freeFileSession();
    return false;
  }
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.printf("║  🆔 File ID: 0x%08X\n", fileID);
  Serial.printf("║  📁 Archivo: %s\n", fileName.c_str());
  Serial.printf("║  📊 Tamaño: %u bytes (%.2f KB)\n", totalSize, totalSize/1024.0);
  Serial.printf("║  📦 Chunks: %u\n", totalChunks);
  Serial.printf("║  💾 RAM usada: ~%u KB\n", (totalChunks + currentSession.numParityBlocks * chunkSize) / 1024);
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println("✅ Archivo temporal abierto y listo");
  
  return true;
}

// ============================================
// ✅ LIBERAR SESIÓN (con protección)
// ============================================
void freeFileSession() {
  Serial.println("🧹 Liberando sesión...");
  
  // ✅ Cerrar archivo si está abierto
  if (currentTempFile) {
    Serial.println("  └─ Cerrando archivo temporal");
    currentTempFile.flush();
    currentTempFile.close();
    currentTempFile = File();  // ✅ Invalidar handle
  }
  
  // ✅ Liberar parity blocks
  if (currentSession.parityBlocks) {
    Serial.println("  └─ Liberando parity blocks");
    for (uint16_t i = 0; i < currentSession.numParityBlocks; i++) {
      if (currentSession.parityBlocks[i].data) {
        free(currentSession.parityBlocks[i].data);
        currentSession.parityBlocks[i].data = nullptr;  // ✅ Prevenir double free
      }
    }
    delete[] currentSession.parityBlocks;
    currentSession.parityBlocks = nullptr;
  }
  
  // ✅ Liberar chunk flags
  if (currentSession.chunkReceived) {
    Serial.println("  └─ Liberando chunk flags");
    free(currentSession.chunkReceived);
    currentSession.chunkReceived = nullptr;
  }
  
  // ✅ Reset estado
  currentSession.active = false;
  currentSession.fileID = 0;
  currentSession.chunksReceivedCount = 0;
  
  Serial.println("✅ Sesión liberada correctamente");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa BROADCAST v8.1 (FIXED) ===");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  
  Serial.println("✅ LittleFS montado");
  Serial.printf("   Total:  %u bytes (%.2f MB)\n", LittleFS.totalBytes(), LittleFS.totalBytes()/1048576.0);
  Serial.printf("   Usado: %u bytes (%.2f MB)\n", LittleFS.usedBytes(), LittleFS.usedBytes()/1048576.0);
  Serial.printf("   Libre: %u bytes (%.2f MB)\n", 
                LittleFS.totalBytes() - LittleFS.usedBytes(),
                (LittleFS.totalBytes() - LittleFS.usedBytes())/1048576.0);

  Serial.println("\n📡 Configurando WiFi AP...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP: %s\n", ssid);
  Serial.printf("   IP: http://%s\n\n", IP.toString().c_str());

  setupWebServer();

  Serial.println("Iniciando radio SX1262...");
  enableVext(true);
  delay(200);
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262: %d\n", state);
    while (true) delay(1000);
  }
  
  applyLoRaConfig();
  radio.setDio1Action(setFlag);
  
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error startReceive: %d\n", state);
  }

  Serial.println("✅ Radio listo (RX ONLY - SIN ACK)");
  Serial.println("👂 Escuchando broadcast...\n");
}

void applyLoRaConfig() {
  Serial.println("📻 Configurando LoRa...");
  radio.standby();
  delay(100);
  
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  delay(100);
  
  Serial.printf("   BW: %.0f kHz, SF: %d, CR: 4/%d\n", currentBW, currentSF, currentCR);
  Serial.println("✅ Radio configurado\n");
}

// ✅ Web server (sin cambios, solo referencia)
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa RX v8.1</title>";
    html += "<style>";
    html += "body{font-family: Arial;background: linear-gradient(135deg,#667eea,#764ba2);color:#333;padding:20px;margin:0}";
    html += ".container{max-width:1000px;margin:0 auto;background:white;padding:30px;border-radius:15px;box-shadow:0 10px 40px rgba(0,0,0,0.2)}";
    html += "h1{color:#333;border-bottom:3px solid #667eea;padding-bottom:15px;margin-bottom:25px}";
    html += ".section{background:#f8f9fa;padding:20px;border-radius:10px;margin:20px 0}";
    html += ".section h2{color:#667eea;margin-bottom:15px;font-size:1.3em}";
    html += ".lora-config{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:15px}";
    html += ".param-group{background:white;padding:15px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.05)}";
    html += ".param-group label{display:block;font-weight:bold;color:#333;margin-bottom:8px;font-size:0.9em}";
    html += ".param-group select{width:100%;padding:10px;border:2px solid #667eea;border-radius:5px;font-size:14px;background:white;cursor:pointer}";
    html += ".btn-apply{background:#667eea;color:white;padding:12px 30px;font-size:16px;border:none;border-radius:5px;cursor:pointer;margin-top:15px;width:100%;font-weight:bold}";
    html += ".btn-apply:hover{background:#5568d3}";
    html += ".current-config{background:#e7f3ff;padding:15px;border-radius:8px;margin-bottom:15px}";
    html += ".config-badge{display:inline-block;background:#667eea;color:white;padding:5px 12px;border-radius:15px;margin:5px;font-size:0.85em}";
    html += ".broadcast-badge{background:#ff6b6b}";
    html += ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:10px;margin:15px 0}";
    html += ".stat-box{background:white;padding:15px;border-radius:8px;text-align:center;box-shadow:0 2px 5px rgba(0,0,0,0.1)}";
    html += ".stat-value{font-size:1.6em;font-weight:bold;color:#667eea}";
    html += ".stat-label{color:#666;font-size:0.8em;margin-top:5px}";
    html += ".speed-highlight{background:linear-gradient(135deg,#f093fb,#f5576c);color:white}";
    html += ".file-list{list-style:none;padding:0}";
    html += ".file-item{background:white;padding:15px;margin:10px 0;border-radius:8px;display:flex;justify-content:space-between;align-items:center;box-shadow:0 2px 5px rgba(0,0,0,0.1)}";
    html += ".file-info{flex-grow:1}";
    html += ".file-name{font-weight:bold;color:#333;font-size:1.1em}";
    html += ".file-size{color:#666;font-size:0.9em;margin-top:5px}";
    html += ".btn-group{display:flex;gap:10px}";
    html += "button{padding:10px 20px;border:none;border-radius:5px;cursor:pointer;font-size:14px;transition:all 0.3s}";
    html += ".btn-download{background:#28a745;color:white}";
    html += ".btn-download:hover{background:#218838}";
    html += ".btn-delete{background:#dc3545;color:white}";
    html += ".btn-delete:hover{background:#c82333}";
    html += ".btn-refresh{background:#667eea;color:white;margin-top:20px;width:100%;padding:12px;font-weight:bold}";
    html += ".btn-refresh:hover{background:#5568d3}";
    html += ".empty{text-align:center;color:#999;padding:30px}";
    html += ".receiving{background:#fff3cd;padding:15px;border-radius:8px;margin:15px 0;color:#856404;border-left:4px solid #ffc107}";
    html += ".progress-container{background:#f0f0f0;border-radius:10px;overflow:hidden;margin:15px 0}";
    html += ".progress-bar{background:linear-gradient(90deg,#667eea,#764ba2);height:30px;transition:width 0.3s;display:flex;align-items:center;justify-content:center;color:white;font-weight:bold}";
    html += ".device-info{background:#e7f3ff;padding:15px;border-radius:8px;margin-bottom:20px;font-size:0.9em;text-align:center}";
    html += ".warning{background:#fff3cd;border-left:4px solid #ffc107;padding:15px;margin:15px 0;color:#856404;border-radius:5px}";
    html += ".info-box{background:#d1ecf1;border-left:4px solid #0c5460;padding:15px;margin:15px 0;border-radius:5px;color:#0c5460}";
    html += "</style></head><body><div class='container'>";
    html += "<h1>🛰️ LoRa RX Broadcast v8.1 FIXED</h1>";
    
    html += "<div class='device-info'>";
    html += "📟 <strong>Heltec WiFi LoRa 32 V3</strong> | 📡 <strong>SX1262</strong> | 📶 <strong>915 MHz</strong>";
    html += "</div>";

    html += "<div class='warning'>";
    html += "📻 <strong>MODO BROADCAST:</strong> Recepción sin ACK. Compatible con carrusel.";
    html += "</div>";
    
    html += "<div class='info-box'>";
    html += "<strong>ℹ️ MEJORAS v8.1:</strong><br>";
    html += "✅ Archivo abierto durante sesión (sin reinicios)<br>";
    html += "✅ Protección contra heap corruption<br>";
    html += "✅ Ignora vueltas duplicadas del carrusel<br>";
    html += "✅ FEC recovery mejorado";
    html += "</div>";
    
    // Configuración LoRa
    html += "<div class='section'>";
    html += "<h2>⚙️ Configuración LoRa</h2>";
    html += "<div class='current-config'>";
    html += "<strong>Configuración Actual:</strong><br>";
    html += "<span class='config-badge'>BW: " + String((int)currentBW) + " kHz</span>";
    html += "<span class='config-badge'>SF: " + String(currentSF) + "</span>";
    html += "<span class='config-badge'>CR: 4/" + String(currentCR) + "</span>";
    html += "<span class='config-badge broadcast-badge'>BROADCAST MODE</span>";
    html += "</div>";
    
    if (!currentSession.active) {
      html += "<form class='lora-config'>";
      html += "<div class='param-group'><label>📶 Bandwidth</label><select name='bw'>";
      html += "<option value='125'" + String(currentBW == 125.0 ? " selected" : "") + ">125 kHz</option>";
      html += "<option value='250'" + String(currentBW == 250.0 ? " selected" : "") + ">250 kHz</option>";
      html += "<option value='500'" + String(currentBW == 500.0 ? " selected" : "") + ">500 kHz</option>";
      html += "</select></div>";
      html += "<div class='param-group'><label>📡 SF</label><select name='sf'>";
      html += "<option value='7'" + String(currentSF == 7 ? " selected" : "") + ">7</option>";
      html += "<option value='9'" + String(currentSF == 9 ? " selected" : "") + ">9</option>";
      html += "<option value='12'" + String(currentSF == 12 ? " selected" : "") + ">12</option>";
      html += "</select></div>";
      html += "<div class='param-group'><label>🔧 CR</label><select name='cr'>";
      html += "<option value='5'" + String(currentCR == 5 ? " selected" : "") + ">4/5</option>";
      html += "<option value='7'" + String(currentCR == 7 ? " selected" : "") + ">4/7</option>";
      html += "<option value='8'" + String(currentCR == 8 ? " selected" : "") + ">4/8</option>";
      html += "</select></div>";
      html += "</form>";
      html += "<button class='btn-apply' onclick='applyConfig()'>✅ Aplicar</button>";
    } else {
      html += "<p style='text-align:center;color:#856404;'>🔒 Bloqueado durante recepción</p>";
    }
    html += "</div>";
    
    // Progreso
    if (currentSession.active && currentSession.totalChunks > 0) {
      float progress = (currentSession.chunksReceivedCount * 100.0) / currentSession.totalChunks;
      html += "<div class='receiving'>📡 Recibiendo: <strong>" + currentSession.fileName + "</strong></div>";
      html += "<div class='progress-container'>";
      html += "<div class='progress-bar' style='width:" + String(progress, 1) + "%'>";
      html += String(currentSession.chunksReceivedCount) + "/" + String(currentSession.totalChunks) + " (" + String(progress, 1) + "%)";
      html += "</div></div>";
      
      unsigned long elapsed = (millis() - currentSession.startTime) / 1000;
      if (currentSession.chunksReceivedCount > 10) {
        float rate = currentSession.chunksReceivedCount / (float)elapsed;
        uint16_t remaining = currentSession.totalChunks - currentSession.chunksReceivedCount;
        float eta = remaining / rate;
        html += "<p style='text-align:center;margin-top:10px;color:#666;'>⏱️ Tiempo: " + String(elapsed) + "s | ETA: ~" + String((int)eta) + "s</p>";
      }
    }
    
    // Estadísticas
    if (lastReceptionTime > 0) {
      html += "<div class='section'>";
      html += "<h2>📊 Última Recepción</h2>";
      html += "<div class='stats'>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastReceptionTime, 1) + "s</div><div class='stat-label'>Tiempo</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastFileSize/1024.0, 1) + " KB</div><div class='stat-label'>Tamaño</div></div>";
      html += "<div class='stat-box speed-highlight'><div class='stat-value'>" + String(lastSpeed, 2) + "</div><div class='stat-label'>kbps ⚡</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalPacketsReceived) + "</div><div class='stat-label'>Paquetes</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalCrcErrors) + "</div><div class='stat-label'>CRC Err</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalRecovered) + "</div><div class='stat-label'>FEC Fix</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalDuplicates) + "</div><div class='stat-label'>Duplicados</div></div>";
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
      if (!file.isDirectory() && !String(file.name()).endsWith(".tmp")) {
        hasFiles = true;
        String fullPath = String(file.name());
        String displayName = fullPath;
        if (displayName.startsWith("/")) displayName = displayName.substring(1);
        
        float sizeKB = file.size() / 1024.0;
        String sizeStr = sizeKB < 1.0 ? String(file.size()) + " bytes" : String(sizeKB, 2) + " KB";
        
        html += "<li class='file-item'>";
        html += "<div class='file-info'><div class='file-name'>📄 " + displayName + "</div>";
        html += "<div class='file-size'>" + sizeStr + "</div></div>";
        html += "<div class='btn-group'>";
        html += "<button class='btn-download' onclick='location.href=\"/download?file=" + displayName + "\"'>📥</button>";
        html += "<button class='btn-delete' onclick='if(confirm(\"¿Eliminar?\")) location.href=\"/delete?file=" + displayName + "\"'>🗑️</button>";
        html += "</div></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul>";
    
    if (!hasFiles) {
      html += "<div class='empty'>Sin archivos.<br>Esperando broadcast...</div>";
    }
    
    html += "</div>";
    html += "<button class='btn-refresh' onclick='location.reload()'>🔄 Actualizar</button>";
    
    html += "<script>";
    html += "function applyConfig(){";
    html += "const bw=document.querySelector('select[name=bw]').value;";
    html += "const sf=document.querySelector('select[name=sf]').value;";
    html += "const cr=document.querySelector('select[name=cr]').value;";
    html += "fetch(`/config?bw=${bw}&sf=${sf}&cr=${cr}`).then(()=>{alert('✅ OK');location.reload();});";
    html += "}";
    
    if (currentSession.active) {
      html += "setTimeout(()=>location.reload(),3000);";
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
        if (filename.endsWith(".pdf")) contentType = "application/pdf";
        else if (filename.endsWith(".txt")) contentType = "text/plain";
        else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) contentType = "image/jpeg";
        else if (filename.endsWith(".png")) contentType = "image/png";
        
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
  static unsigned long lastDebugPrint = 0;
  
  if (millis() - lastDebugPrint > 10000) {
    Serial.println("👂 Escuchando...");
    lastDebugPrint = millis();
  }
  
  // ✅ Timeout de sesión
  if (currentSession.active && (millis() - currentSession.lastPacketTime) > RX_TIMEOUT) {
    Serial.println("\n⚠️ TIMEOUT - Finalizando con lo recibido");
    attemptFECRecovery();
    finalizeFile();
  }
  
  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();
      
      if (packetLen < 4) {
        radio.startReceive();
        return;
      }
      
      uint16_t crcRecv, crcCalc;
      memcpy(&crcRecv, buffer + packetLen - 2, 2);
      crcCalc = crc16_ccitt(buffer, packetLen - 2);
      
      if (crcRecv != crcCalc) {
        totalCrcErrors++;
        radio.startReceive();
        return;
      }
      
      totalPacketsReceived++;
      
      // Parsear por magic bytes
      if (buffer[0] == MANIFEST_MAGIC_1 && buffer[1] == MANIFEST_MAGIC_2) {
        processManifest(buffer, packetLen);
      } 
      else if (buffer[0] == DATA_MAGIC_1 && buffer[1] == DATA_MAGIC_2) {
        processDataChunk(buffer, packetLen);
      }
      else if (buffer[0] == PARITY_MAGIC_1 && buffer[1] == PARITY_MAGIC_2) {
        processParityChunk(buffer, packetLen);
      }
      else if (buffer[0] == FILE_END_MAGIC_1 && buffer[1] == FILE_END_MAGIC_2) {
        processFileEnd(buffer, packetLen);
      }
      
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      totalCrcErrors++;
    }

    delay(5);
    radio.startReceive();
  }
  
  yield();
  delay(10);
}

// ============================================
// ✅ PROCESAR MANIFEST (con protección carrusel)
// ============================================
void processManifest(uint8_t* data, size_t len) {
  if (len < 17) return;
  
  uint32_t fileID;
  uint32_t totalSize;
  uint16_t totalChunks;
  uint16_t chunkSize;
  uint8_t nameLen;
  
  size_t idx = 2;
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
  if (!fileName.startsWith("/")) fileName = "/" + fileName;
  
  // ✅ NUEVO: Verificar si este FileID ya fue procesado recientemente
  if (fileID == lastProcessedFileID) {
    unsigned long timeSince = millis() - lastFileCompletionTime;
    if (timeSince < FILE_ID_COOLDOWN) {
      Serial.println("🔁 Ignorando MANIFEST duplicado (carrusel extra)");
      return;
    }
  }
  
  // ✅ Si ya hay sesión activa con mismo ID, ignorar
  if (currentSession.active && currentSession.fileID == fileID) {
    return;  // Manifest duplicado de vuelta actual
  }
  
  // ✅ Si hay sesión activa pero es archivo DIFERENTE
  if (currentSession.active && currentSession.fileID != fileID) {
    Serial.println("\n⚠️ Nuevo archivo detectado, finalizando anterior...");
    attemptFECRecovery();
    finalizeFile();
    delay(500);
  }
  
  // ✅ Nueva sesión
  if (!currentSession.active) {
    Serial.println("\n📋 MANIFEST recibido - Nueva sesión");
    if (initFileSession(fileID, fileName, totalSize, totalChunks, chunkSize)) {
      Serial.println("✅ Sesión iniciada");
    } else {
      Serial.println("❌ Error iniciando sesión");
    }
  }
}

// ============================================
// ✅ PROCESAR DATA CHUNK (archivo abierto)
// ============================================
void processDataChunk(uint8_t* data, size_t len) {
  if (!currentSession.active || len < 12) return;
  
  uint32_t fileID;
  uint16_t chunkIndex;
  uint16_t totalChunks;
  
  size_t idx = 2;
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&chunkIndex, data + idx, 2); idx += 2;
  memcpy(&totalChunks, data + idx, 2); idx += 2;
  
  // ✅ Ignorar chunks de FileID incorrecto
  if (fileID != currentSession.fileID) return;
  if (chunkIndex >= currentSession.totalChunks) return;
  
  // ✅ NUEVO: Verificar que archivo esté abierto
  if (!currentTempFile) {
    return;
  }
  
  currentSession.lastPacketTime = millis();
  
  if (currentSession.chunkReceived[chunkIndex]) {
    totalDuplicates++;
    return;
  }
  
  size_t dataLen = len - idx - 2;
  
  // ✅ Escribir usando archivo ya abierto
  uint32_t fileOffset = (uint32_t)chunkIndex * currentSession.chunkSize;
  
  if (!currentTempFile.seek(fileOffset)) {
    return;
  }
  
  size_t written = currentTempFile.write(data + idx, dataLen);
  if (written != dataLen) {
    Serial.printf("❌ Error escritura: %u/%u bytes\n", written, dataLen);
    return;
  }
  
  // ✅ Flush cada 8 chunks
  if (chunkIndex % 8 == 0) {
    currentTempFile.flush();
  }
  
  currentSession.chunkReceived[chunkIndex] = true;
  currentSession.chunksReceivedCount++;
  
  // ✅ yield cada 5 chunks
  if (chunkIndex % 5 == 0) {
    yield();
  }
  
  // Mostrar progreso cada 5%
  static uint16_t lastProgressPercent = 0;
  uint16_t currentPercent = (currentSession.chunksReceivedCount * 100) / currentSession.totalChunks;
  if (currentPercent >= lastProgressPercent + 5 || currentSession.chunksReceivedCount == currentSession.totalChunks) {
    float progress = (currentSession.chunksReceivedCount * 100.0) / currentSession.totalChunks;
    Serial.printf("📦 Progreso: %u/%u (%.1f%%) | RSSI: %.1f dBm\n", 
                  currentSession.chunksReceivedCount, currentSession.totalChunks, progress, radio.getRSSI());
    lastProgressPercent = currentPercent;
  }
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
  if (blockIndex >= currentSession.numParityBlocks) return;
  
  currentSession.lastPacketTime = millis();
  
  if (currentSession.parityBlocks[blockIndex].received) return;
  
  size_t dataLen = len - idx - 2;
  
  currentSession.parityBlocks[blockIndex].data = (uint8_t*)malloc(dataLen);
  if (!currentSession.parityBlocks[blockIndex].data) {
    Serial.println("❌ No RAM para parity");
    return;
  }
  
  memcpy(currentSession.parityBlocks[blockIndex].data, data + idx, dataLen);
  currentSession.parityBlocks[blockIndex].length = dataLen;
  currentSession.parityBlocks[blockIndex].received = true;
  
  yield();
}

// ============================================
// ✅ PROCESAR FILE_END (con espera)
// ============================================
void processFileEnd(uint8_t* data, size_t len) {
  if (!currentSession.active || len < 10) return;
  
  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  
  if (fileID != currentSession.fileID) {
    Serial.printf("⚠️ FILE_END ignorado (FileID diferente)\n");
    return;
  }
  
  Serial.println("\n🏁 FILE_END recibido");
  
  // ✅ Esperar chunks retrasados
  Serial.println("⏳ Esperando 2s por chunks retrasados...");
  unsigned long waitStart = millis();
  while (millis() - waitStart < 2000) {
    yield();
    delay(10);
  }
  
  attemptFECRecovery();
  finalizeFile();
}

// ============================================
// ✅ FEC RECOVERY (con archivo abierto)
// ============================================
void attemptFECRecovery() {
  if (!currentSession.active) return;
  
  Serial.println("\n🔧 Intentando FEC recovery...");
  
  if (!currentTempFile) {
    Serial.println("❌ Archivo no abierto");
    return;
  }
  
  for (uint16_t block = 0; block < currentSession.numParityBlocks; block++) {
    uint16_t blockStart = block * FEC_BLOCK_SIZE;
    uint16_t blockEnd = min((uint16_t)(blockStart + FEC_BLOCK_SIZE), currentSession.totalChunks);
    
    int missingCount = 0;
    int missingIndex = -1;
    
    for (uint16_t i = blockStart; i < blockEnd; i++) {
      if (!currentSession.chunkReceived[i]) {
        missingCount++;
        missingIndex = i;
      }
    }
    
    if (missingCount == 1 && currentSession.parityBlocks[block].received) {
      Serial.printf("🛡️ Recuperando chunk %u...\n", missingIndex);
      
      size_t maxLen = currentSession.parityBlocks[block].length;
      uint8_t* recovered = (uint8_t*)malloc(maxLen);
      if (!recovered) continue;
      
      memcpy(recovered, currentSession.parityBlocks[block].data, maxLen);
      
      uint8_t chunkBuffer[300];
      for (uint16_t i = blockStart; i < blockEnd; i++) {
        if (currentSession.chunkReceived[i]) {
          uint32_t offset = (uint32_t)i * currentSession.chunkSize;
          currentTempFile.seek(offset);
          size_t readLen = currentTempFile.read(chunkBuffer, currentSession.chunkSize);
          
          for (size_t j = 0; j < readLen && j < maxLen; j++) {
            recovered[j] ^= chunkBuffer[j];
          }
        }
        yield();
      }
      
      uint32_t offset = (uint32_t)missingIndex * currentSession.chunkSize;
      currentTempFile.seek(offset);
      size_t written = currentTempFile.write(recovered, maxLen);
      
      if (written == maxLen) {
        currentSession.chunkReceived[missingIndex] = true;
        currentSession.chunksReceivedCount++;
        totalRecovered++;
        Serial.printf("✅ Chunk %u recuperado!\n", missingIndex);
      }
      
      free(recovered);
    }
    
    yield();
  }
  
  currentTempFile.flush();
}

// ============================================
// ✅ FINALIZAR ARCHIVO (con protección)
// ============================================
void finalizeFile() {
  if (!currentSession.active) return;
  
  Serial.println("\n📝 Finalizando archivo...");
  
  // ✅ Cerrar archivo antes de renombrar
  if (currentTempFile) {
    currentTempFile.flush();
    currentTempFile.close();
    Serial.println("✅ Archivo temporal cerrado");
  }
  
  // Eliminar archivo final si existe
  if (LittleFS.exists(currentSession.fileName)) {
    LittleFS.remove(currentSession.fileName);
  }
  
  // Renombrar temp → final
  LittleFS.rename(currentSession.tempFileName, currentSession.fileName);
  
  File file = LittleFS.open(currentSession.fileName, "r");
  if (!file) {
    Serial.println("❌ Error abriendo archivo final");
    freeFileSession();
    return;
  }
  
  lastFileSize = file.size();
  file.close();
  
  unsigned long endTime = millis();
  lastReceptionTime = (endTime - currentSession.startTime) / 1000.0;
  lastSpeed = (lastFileSize * 8.0) / (lastReceptionTime * 1000.0);
  
  uint16_t chunksMissing = currentSession.totalChunks - currentSession.chunksReceivedCount;
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🎉 ARCHIVO GUARDADO             ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("📁 %s\n", currentSession.fileName.c_str());
  Serial.printf("📊 %.2f KB / %.2f KB (%.1f%%)\n", 
                lastFileSize/1024.0, currentSession.totalSize/1024.0,
                (lastFileSize * 100.0) / currentSession.totalSize);
  Serial.printf("📦 %u / %u chunks\n", 
                currentSession.chunksReceivedCount, currentSession.totalChunks);
  Serial.printf("⏱️ %.2f segundos\n", lastReceptionTime);
  
  Serial.println("╔════════════════════════════════════════╗");
  Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps              ║\n", lastSpeed);
  Serial.println("╚════════════════════════════════════════╝");
  
  if (chunksMissing > 0) {
    Serial.printf("⚠️ %u chunks faltantes (%.1f%%)\n", 
                  chunksMissing, (chunksMissing * 100.0) / currentSession.totalChunks);
  } else {
    Serial.println("✅ Archivo 100% completo!");
  }
  
  if (totalRecovered > 0) {
    Serial.printf("🛡️ %u chunks recuperados con FEC\n", totalRecovered);
  }
  
  if (totalDuplicates > 0) {
    Serial.printf("🔁 %u paquetes duplicados ignorados\n", totalDuplicates);
  }
  
  Serial.printf("📊 Total: %u paquetes RX | %u errores CRC\n", 
                totalPacketsReceived, totalCrcErrors);
  
  // ✅ NUEVO: Registrar FileID procesado
  lastProcessedFileID = currentSession.fileID;
  lastFileCompletionTime = millis();
  Serial.printf("🔒 FileID 0x%08X marcado como completado\n\n", lastProcessedFileID);
  
  freeFileSession();
}