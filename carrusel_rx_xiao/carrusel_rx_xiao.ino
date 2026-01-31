#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>

// ✅ AGREGAR ESTAS LÍNEAS PARA ASYNCTCP (ESP32 Core 3.x)
#ifndef CONFIG_ASYNC_TCP_RUNNING_CORE
#define CONFIG_ASYNC_TCP_RUNNING_CORE 1
#endif

#ifndef CONFIG_ASYNC_TCP_USE_WDT
#define CONFIG_ASYNC_TCP_USE_WDT 0
#endif

// Pines XIAO ESP32-S3
#define LORA_CS   41
#define LORA_RST  42
#define LORA_BUSY 40
#define LORA_DIO1 39
#define LORA_MISO 8
#define LORA_MOSI 9
#define LORA_SCK  7
#define LORA_RXEN 38

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

const char* ssid = "LoRa-RX-XIAO";
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

// ✅ Control RXEN (RX mode) - MANTENER SIEMPRE ACTIVO
void setRXMode(bool enable) {
  pinMode(LORA_RXEN, OUTPUT);
  digitalWrite(LORA_RXEN, enable ? HIGH : LOW);
  Serial.printf("🔧 RX Mode: %s (RXEN=%d)\n", enable ? "ON" : "OFF", enable ? HIGH : LOW);
}

// ============================================
// ✅ LIMPIEZA DE SESIÓN
// ============================================
void cleanupSession() {
  if (currentSession.chunkReceived != nullptr) {
    delete[] currentSession.chunkReceived;
    currentSession.chunkReceived = nullptr;
  }
  
  if (currentSession.parityBlocks != nullptr) {
    for (uint16_t i = 0; i < currentSession.numParityBlocks; i++) {
      if (currentSession.parityBlocks[i].data != nullptr) {
        delete[] currentSession.parityBlocks[i].data;
      }
    }
    delete[] currentSession.parityBlocks;
    currentSession.parityBlocks = nullptr;
  }
  
  if (currentSession.tempFileName.length() > 0 && LittleFS.exists(currentSession.tempFileName)) {
    LittleFS.remove(currentSession.tempFileName);
  }
  
  currentSession.active = false;
  currentSession.fileID = 0;
  currentSession.fileName = "";
  currentSession.tempFileName = "";
  currentSession.totalChunks = 0;
  currentSession.chunksReceivedCount = 0;
}

// ============================================
// ✅ INICIAR SESIÓN
// ============================================
bool startSession(uint32_t fileID, const String& fileName, uint32_t totalSize, uint16_t chunkSize) {
  cleanupSession();
  
  currentSession.fileID = fileID;
  currentSession.fileName = fileName;
  currentSession.totalSize = totalSize;
  currentSession.chunkSize = chunkSize;
  currentSession.totalChunks = (totalSize + chunkSize - 1) / chunkSize;
  currentSession.chunksReceivedCount = 0;
  currentSession.startTime = millis();
  
  if (currentSession.totalChunks > MAX_CHUNKS) {
    Serial.println("❌ Archivo demasiado grande");
    return false;
  }
  
  currentSession.chunkReceived = new bool[currentSession.totalChunks];
  if (currentSession.chunkReceived == nullptr) {
    Serial.println("❌ Error: no hay memoria para flags");
    return false;
  }
  memset(currentSession.chunkReceived, 0, currentSession.totalChunks);
  
  currentSession.numParityBlocks = (currentSession.totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  currentSession.parityBlocks = new FileSession::FECBlock[currentSession.numParityBlocks];
  if (currentSession.parityBlocks == nullptr) {
    delete[] currentSession.chunkReceived;
    currentSession.chunkReceived = nullptr;
    Serial.println("❌ Error: no hay memoria para bloques FEC");
    return false;
  }
  memset(currentSession.parityBlocks, 0, sizeof(FileSession::FECBlock) * currentSession.numParityBlocks);
  
  currentSession.tempFileName = "/temp_" + String(fileID) + ".dat";
  File f = LittleFS.open(currentSession.tempFileName, "w");
  if (!f) {
    Serial.println("❌ Error: no se puede crear archivo temporal");
    delete[] currentSession.chunkReceived;
    delete[] currentSession.parityBlocks;
    return false;
  }
  
  for (uint32_t i = 0; i < totalSize; i++) {
    f.write(0x00);
  }
  f.close();
  
  currentSession.active = true;
  currentSession.lastPacketTime = millis();
  
  Serial.printf("✅ Sesión iniciada: %s (%u bytes, %u chunks)\n", 
                fileName.c_str(), totalSize, currentSession.totalChunks);
  
  return true;
}

// ============================================
// ✅ GUARDAR CHUNK
// ============================================
bool saveChunk(uint16_t chunkIndex, const uint8_t* data, uint16_t length) {
  if (!currentSession.active || chunkIndex >= currentSession.totalChunks) {
    return false;
  }
  
  if (currentSession.chunkReceived[chunkIndex]) {
    totalDuplicates++;
    return true;
  }
  
  File f = LittleFS.open(currentSession.tempFileName, "r+");
  if (!f) {
    Serial.println("❌ Error abriendo archivo temporal");
    return false;
  }
  
  uint32_t offset = (uint32_t)chunkIndex * currentSession.chunkSize;
  f.seek(offset);
  
  size_t written = f.write(data, length);
  f.close();
  
  if (written != length) {
    Serial.println("❌ Error escribiendo chunk");
    return false;
  }
  
  currentSession.chunkReceived[chunkIndex] = true;
  currentSession.chunksReceivedCount++;
  currentSession.lastPacketTime = millis();
  
  return true;
}

// ============================================
// ✅ FEC: RECUPERAR CHUNKS PERDIDOS
// ============================================
void attemptFECRecovery() {
  for (uint16_t blockIdx = 0; blockIdx < currentSession.numParityBlocks; blockIdx++) {
    if (!currentSession.parityBlocks[blockIdx].received) continue;
    
    uint16_t startChunk = blockIdx * FEC_BLOCK_SIZE;
    uint16_t endChunk = min((uint16_t)(startChunk + FEC_BLOCK_SIZE), currentSession.totalChunks);
    
    int missingIdx = -1;
    int missingCount = 0;
    
    for (uint16_t i = startChunk; i < endChunk; i++) {
      if (!currentSession.chunkReceived[i]) {
        missingIdx = i;
        missingCount++;
      }
    }
    
    if (missingCount == 1) {
      uint8_t* recoveredData = new uint8_t[currentSession.chunkSize];
      if (recoveredData == nullptr) continue;
      
      memcpy(recoveredData, currentSession.parityBlocks[blockIdx].data, 
             currentSession.parityBlocks[blockIdx].length);
      
      File f = LittleFS.open(currentSession.tempFileName, "r");
      if (f) {
        for (uint16_t i = startChunk; i < endChunk; i++) {
          if (i != missingIdx && currentSession.chunkReceived[i]) {
            uint8_t buffer[256];
            f.seek((uint32_t)i * currentSession.chunkSize);
            size_t toRead = min((size_t)currentSession.chunkSize, sizeof(buffer));
            f.read(buffer, toRead);
            
            for (size_t j = 0; j < toRead; j++) {
              recoveredData[j] ^= buffer[j];
            }
          }
        }
        f.close();
        
        if (saveChunk(missingIdx, recoveredData, currentSession.chunkSize)) {
          totalRecovered++;
          Serial.printf("✅ FEC: recuperado chunk %u\n", missingIdx);
        }
      }
      
      delete[] recoveredData;
    }
  }
}

// ============================================
// ✅ FINALIZAR RECEPCIÓN
// ============================================
void finalizeFile() {
  attemptFECRecovery();
  
  if (currentSession.chunksReceivedCount < currentSession.totalChunks) {
    Serial.printf("❌ Recepción incompleta: %u/%u chunks\n", 
                  currentSession.chunksReceivedCount, currentSession.totalChunks);
    cleanupSession();
    return;
  }
  
  String finalPath = "/" + currentSession.fileName;
  if (LittleFS.exists(finalPath)) {
    LittleFS.remove(finalPath);
  }
  
  LittleFS.rename(currentSession.tempFileName, finalPath);
  
  float duration = (millis() - currentSession.startTime) / 1000.0;
  float speed = currentSession.totalSize / duration;
  
  lastReceptionTime = duration;
  lastSpeed = speed;
  lastFileSize = currentSession.totalSize;
  
  Serial.printf("✅ ARCHIVO RECIBIDO: %s (%u bytes en %.2fs = %.2f B/s)\n", 
                currentSession.fileName.c_str(), currentSession.totalSize, duration, speed);
  Serial.printf("📊 Paquetes: %u | Duplicados: %u | Recuperados FEC: %u\n",
                totalPacketsReceived, totalDuplicates, totalRecovered);
  
  cleanupSession();
}

// ============================================
// ✅ ISR
// ============================================
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  receivedFlag = true;
}

// ============================================
// ✅ PROCESAR MANIFEST
// ============================================
void processManifest(const uint8_t* data, size_t len) {
  if (len < 17) return;  // Mínimo: 2 magic + 4 fileID + 4 size + 2 chunks + 2 chunkSize + 1 nameLen + 2 CRC
  
  uint32_t fileID;
  uint32_t totalSize;
  uint16_t totalChunks;  // ✅ AGREGAR
  uint16_t chunkSize;
  
  size_t idx = 2;  // Saltar magic bytes
  memcpy(&fileID, data + idx, 4); idx += 4;           // [2-5]
  memcpy(&totalSize, data + idx, 4); idx += 4;        // [6-9]
  memcpy(&totalChunks, data + idx, 2); idx += 2;      // ✅ [10-11] AGREGAR
  memcpy(&chunkSize, data + idx, 2); idx += 2;        // [12-13]
  uint8_t nameLen = data[idx++];                      // [14]
  
  if (nameLen == 0 || nameLen > 64 || len < (idx + nameLen + 2)) return;
  
  char fileName[65];
  memcpy(fileName, data + idx, nameLen);
  fileName[nameLen] = '\0';
  idx += nameLen;
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + idx, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data + 2, idx - 2);
  
  if (receivedCRC != calculatedCRC) {
    Serial.println("❌ Manifest: CRC inválido");
    totalCrcErrors++;
    return;
  }
  
  if (!currentSession.active || currentSession.fileID != fileID) {
    Serial.printf("📡 MANIFEST RECIBIDO: %s (ID=0x%08X, %u bytes, %u chunks)\n", 
                  fileName, fileID, totalSize, totalChunks);
    startSession(fileID, String(fileName), totalSize, chunkSize);
  } else {
    currentSession.lastPacketTime = millis();
  }
}

// ============================================
// ✅ PROCESAR DATA CHUNK
// ============================================
void processDataChunk(const uint8_t* data, size_t len) {
  if (!currentSession.active || len < 12) return;  // Mínimo: 2 magic + 4 fileID + 2 index + 2 totalChunks + 2 CRC
  
  uint32_t fileID;
  uint16_t chunkIndex;
  uint16_t totalChunks;  // ✅ AGREGAR
  
  size_t idx = 2;  // Saltar magic bytes
  memcpy(&fileID, data + idx, 4); idx += 4;          // [2-5]
  memcpy(&chunkIndex, data + idx, 2); idx += 2;      // [6-7]
  memcpy(&totalChunks, data + idx, 2); idx += 2;     // ✅ [8-9] AGREGAR
  
  if (fileID != currentSession.fileID) return;
  if (chunkIndex >= currentSession.totalChunks) return;
  
  uint16_t dataLen = len - idx - 2;  // ✅ CORREGIR: payload size (descontando CRC)
  const uint8_t* chunkData = data + idx;  // ✅ CORREGIR: payload empieza en idx=[10]
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data, len - 2);  // ✅ CORREGIR: calcular desde inicio hasta antes del CRC
  
  if (receivedCRC != calculatedCRC) {
    totalCrcErrors++;
    return;
  }
  
  if (saveChunk(chunkIndex, chunkData, dataLen)) {
    // Progreso cada 10 chunks
    if (chunkIndex % 10 == 0) {
      float progress = (currentSession.chunksReceivedCount * 100.0) / currentSession.totalChunks;
      Serial.printf("📦 Recibiendo: %.1f%% (%u/%u chunks)\n", 
                    progress, currentSession.chunksReceivedCount, currentSession.totalChunks);
    }
  }
}

// ============================================
// ✅ PROCESAR PARITY BLOCK
// ============================================
void processParityBlock(const uint8_t* data, size_t len) {
  if (!currentSession.active || len < 10) return;  // Mínimo: 2 magic + 4 fileID + 2 blockIdx + 2 CRC
  
  uint32_t fileID;
  uint16_t blockIndex;
  
  size_t idx = 2;  // Saltar magic bytes
  memcpy(&fileID, data + idx, 4); idx += 4;       // [2-5]
  memcpy(&blockIndex, data + idx, 2); idx += 2;   // [6-7]
  
  if (fileID != currentSession.fileID) return;
  if (blockIndex >= currentSession.numParityBlocks) return;
  
  uint16_t dataLen = len - idx - 2;  // ✅ CORREGIR
  const uint8_t* parityData = data + idx;  // ✅ Payload empieza en [8]
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data, len - 2);  // ✅ CORREGIR
  
  if (receivedCRC != calculatedCRC) {
    totalCrcErrors++;
    return;
  }
  
  if (currentSession.parityBlocks[blockIndex].received) return;
  
  currentSession.parityBlocks[blockIndex].data = new uint8_t[dataLen];
  if (currentSession.parityBlocks[blockIndex].data == nullptr) return;
  
  memcpy(currentSession.parityBlocks[blockIndex].data, parityData, dataLen);
  currentSession.parityBlocks[blockIndex].length = dataLen;
  currentSession.parityBlocks[blockIndex].received = true;
  
  currentSession.lastPacketTime = millis();
  Serial.printf("🛡️ Parity block %u recibido\n", blockIndex);
}

// ============================================
// ✅ PROCESAR FILE END
// ============================================
void processFileEnd(const uint8_t* data, size_t len) {
  if (!currentSession.active || len < 10) return;  // 2 magic + 4 fileID + 2 totalChunks + 2 CRC
  
  uint32_t fileID;
  uint16_t totalChunks;
  
  size_t idx = 2;
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&totalChunks, data + idx, 2); idx += 2;  // ✅ AGREGAR (aunque no se use)
  
  if (fileID != currentSession.fileID) return;
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + idx, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data, idx);  // ✅ CORREGIR
  
  if (receivedCRC != calculatedCRC) {
    totalCrcErrors++;
    return;
  }
  
  Serial.println("📡 Señal de fin de archivo recibida");
  finalizeFile();
}
// ============================================
// ✅ PROCESAR PAQUETE RECIBIDO
// ============================================
void processPacket(const uint8_t* data, size_t len) {
  if (len < 2) return;
  
  totalPacketsReceived++;
  
  uint8_t magic1 = data[0];
  uint8_t magic2 = data[1];
  
  if (magic1 == MANIFEST_MAGIC_1 && magic2 == MANIFEST_MAGIC_2) {
    processManifest(data, len);
  } else if (magic1 == DATA_MAGIC_1 && magic2 == DATA_MAGIC_2) {
    processDataChunk(data, len);
  } else if (magic1 == PARITY_MAGIC_1 && magic2 == PARITY_MAGIC_2) {
    processParityBlock(data, len);
  } else if (magic1 == FILE_END_MAGIC_1 && magic2 == FILE_END_MAGIC_2) {
    processFileEnd(data, len);
  } else {
    Serial.printf("⚠️ Paquete desconocido: 0x%02X 0x%02X (len=%zu)\n", magic1, magic2, len);
  }
}

// ============================================
// ✅ WEB SERVER - HTML (igual que antes)
// ============================================
void setupWebServer() {
  // ============================================
  // ✅ PÁGINA PRINCIPAL CON LISTA DE ARCHIVOS
  // ============================================
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa RX - XIAO</title>";
    html += "<style>";
    html += "body{font-family: Arial;background: linear-gradient(135deg,#667eea,#764ba2);color:#333;padding:20px;margin:0}";
    html += ".container{max-width:900px;margin:0 auto;background:white;padding:30px;border-radius:15px;box-shadow:0 10px 40px rgba(0,0,0,0.2)}";
    html += "h1{color:#333;border-bottom:3px solid #667eea;padding-bottom:15px;margin-bottom:25px}";
    html += ".section{background:#f8f9fa;padding:20px;border-radius:10px;margin:20px 0}";
    html += ".section h2{color:#667eea;margin-bottom:15px;font-size:1.3em}";
    html += ".status{padding:15px;border-radius:8px;margin:15px 0;font-weight:bold}";
    html += ".active{background:#d4edda;color:#155724;border-left:4px solid #28a745}";
    html += ".inactive{background:#e2e3e5;color:#383d41;border-left:4px solid #6c757d}";
    html += "table{width:100%;border-collapse:collapse;margin:10px 0}";
    html += "th,td{padding:10px;text-align:left;border-bottom:1px solid #ddd}";
    html += "th{background:#4CAF50;color:white}";
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
    html += ".btn-config{background:#667eea;color:white;margin-top:15px;width:100%;padding:12px;font-weight:bold}";
    html += ".btn-config:hover{background:#5568d3}";
    html += ".btn-refresh{background:#17a2b8;color:white;margin-top:20px;width:100%;padding:12px;font-weight:bold}";
    html += ".btn-refresh:hover{background:#138496}";
    html += ".empty{text-align:center;color:#999;padding:30px}";
    html += ".config-form{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:15px;margin-bottom:15px}";
    html += ".config-group{background:white;padding:10px;border-radius:5px}";
    html += ".config-group label{display:block;font-weight:bold;margin-bottom:5px;font-size:0.9em}";
    html += ".config-group input{width:100%;padding:8px;border:2px solid #667eea;border-radius:5px}";
    html += ".device-info{background:#e7f3ff;padding:15px;border-radius:8px;margin-bottom:20px;text-align:center}";
    html += "</style></head><body><div class='container'>";
    html += "<h1>📡 LoRa RX - XIAO ESP32-S3</h1>";
    
    html += "<div class='device-info'>";
    html += "📟 <strong>Seeed XIAO ESP32-S3</strong> | 📡 <strong>E22-900M30S (SX1262)</strong> | 📶 <strong>915 MHz</strong>";
    html += "</div>";
    
    // Estado de recepción
    if (currentSession.active) {
      float progress = (currentSession.chunksReceivedCount * 100.0) / currentSession.totalChunks;
      html += "<div class='status active'>📡 Recibiendo: <strong>" + currentSession.fileName + "</strong> (" + String(progress, 1) + "%)</div>";
    } else {
      html += "<div class='status inactive'>⏸️ Inactivo - Esperando transmisión...</div>";
    }
    
    // Configuración LoRa
    html += "<div class='section'>";
    html += "<h2>⚙️ Configuración LoRa</h2>";
    html += "<form class='config-form'>";
    html += "<div class='config-group'><label>BW (kHz)</label><input type='number' name='bw' value='" + String((int)currentBW) + "' step='1'></div>";
    html += "<div class='config-group'><label>SF</label><input type='number' name='sf' value='" + String(currentSF) + "' min='7' max='12'></div>";
    html += "<div class='config-group'><label>CR (4/x)</label><input type='number' name='cr' value='" + String(currentCR) + "' min='5' max='8'></div>";
    html += "</form>";
    html += "<button class='btn-config' onclick='applyConfig()'>✅ Aplicar Configuración</button>";
    html += "</div>";
    
    // Estadísticas
    if (lastReceptionTime > 0) {
      html += "<div class='section'>";
      html += "<h2>📊 Última Recepción</h2>";
      html += "<table>";
      html += "<tr><th>Métrica</th><th>Valor</th></tr>";
      html += "<tr><td>Tiempo</td><td>" + String(lastReceptionTime, 2) + " s</td></tr>";
      html += "<tr><td>Tamaño</td><td>" + String(lastFileSize/1024.0, 1) + " KB</td></tr>";
      html += "<tr><td>Velocidad</td><td>" + String(lastSpeed, 2) + " B/s</td></tr>";
      html += "<tr><td>Paquetes RX</td><td>" + String(totalPacketsReceived) + "</td></tr>";
      html += "<tr><td>CRC Errors</td><td>" + String(totalCrcErrors) + "</td></tr>";
      html += "<tr><td>FEC Recuperados</td><td>" + String(totalRecovered) + "</td></tr>";
      html += "<tr><td>Duplicados</td><td>" + String(totalDuplicates) + "</td></tr>";
      html += "</table></div>";
    }
    
    // Lista de archivos
    html += "<div class='section'>";
    html += "<h2>📁 Archivos Recibidos</h2>";
    
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    bool hasFiles = false;
    
    html += "<ul class='file-list'>";
    while (file) {
      if (!file.isDirectory() && !String(file.name()).startsWith("/temp_")) {
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
        html += "<button class='btn-download' onclick='location.href=\"/download?file=" + displayName + "\"'>📥 Descargar</button>";
        html += "<button class='btn-delete' onclick='if(confirm(\"¿Eliminar " + displayName + "?\")) location.href=\"/delete?file=" + displayName + "\"'>🗑️ Borrar</button>";
        html += "</div></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul>";
    
    if (!hasFiles) {
      html += "<div class='empty'>📭 Sin archivos. <br>Esperando transmisión LoRa...</div>";
    }
    
    html += "</div>";
    html += "<button class='btn-refresh' onclick='location.reload()'>🔄 Actualizar Página</button>";
    
    html += "<script>";
    html += "function applyConfig(){";
    html += "const bw=document.querySelector('input[name=bw]').value;";
    html += "const sf=document.querySelector('input[name=sf]').value;";
    html += "const cr=document.querySelector('input[name=cr]').value;";
    html += "fetch(`/config?bw=${bw}&sf=${sf}&cr=${cr}`).then(()=>{alert('✅ Configuración aplicada');location.reload();});";
    html += "}";
    
    // Auto-refresh si está recibiendo
    if (currentSession.active) {
      html += "setTimeout(()=>location.reload(),3000);";
    }
    
    html += "</script>";
    html += "</div></body></html>";
    
    request->send(200, "text/html", html);
  });
  
  // ============================================
  // ✅ ENDPOINT: ESTADÍSTICAS JSON
  // ============================================
  server.on("/stats", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"active\":" + String(currentSession.active ? "true" : "false") + ",";
    json += "\"filename\":\"" + currentSession.fileName + "\",";
    json += "\"packets\":" + String(totalPacketsReceived) + ",";
    json += "\"crc\":" + String(totalCrcErrors) + ",";
    json += "\"fec\":" + String(totalRecovered) + ",";
    json += "\"dup\":" + String(totalDuplicates) + ",";
    json += "\"speed\":\"" + String(lastSpeed, 2) + " B/s\",";
    json += "\"lastFile\":\"" + String(lastFileSize) + " bytes en " + String(lastReceptionTime, 2) + "s\"";
    json += "}";
    request->send(200, "application/json", json);
  });
  
  // ============================================
  // ✅ ENDPOINT: CONFIGURAR LORA
  // ============================================
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("bw") && request->hasParam("sf") && request->hasParam("cr")) {
      if (currentSession.active) {
        request->send(400, "text/plain", "No se puede cambiar durante recepción");
        return;
      }
      
      currentBW = request->getParam("bw")->value().toFloat();
      currentSF = request->getParam("sf")->value().toInt();
      currentCR = request->getParam("cr")->value().toInt();
      
      radio.standby();
      delay(100);
      radio.setBandwidth(currentBW);
      radio.setSpreadingFactor(currentSF);
      radio.setCodingRate(currentCR);
      delay(100);
      
      // ✅ Reactivar RXEN y recepción
      setRXMode(true);
      delay(50);
      radio.startReceive();
      
      Serial.printf("⚙️ Nueva config: BW=%.0f kHz, SF=%d, CR=4/%d\n", currentBW, currentSF, currentCR);
      
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Faltan parámetros");
    }
  });
  
  // ============================================
  // ✅ ENDPOINT: DESCARGAR ARCHIVO
  // ============================================
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
        else if (filename.endsWith(".html")) contentType = "text/html";
        else if (filename.endsWith(".json")) contentType = "application/json";
        
        Serial.printf("📥 Descargando: %s\n", filename.c_str());
        request->send(LittleFS, filename, contentType, true);  // true = download attachment
      } else {
        request->send(404, "text/plain", "Archivo no encontrado");
      }
    } else {
      request->send(400, "text/plain", "Falta parámetro 'file'");
    }
  });
  
  // ============================================
  // ✅ ENDPOINT: ELIMINAR ARCHIVO
  // ============================================
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (!filename.startsWith("/")) filename = "/" + filename;
      
      // Evitar borrar archivos temporales activos
      if (currentSession.active && filename == currentSession.tempFileName) {
        request->send(400, "text/plain", "No se puede eliminar durante recepción");
        return;
      }
      
      if (LittleFS.remove(filename)) {
        Serial.printf("🗑️ Eliminado: %s\n", filename.c_str());
        request->redirect("/");
      } else {
        request->send(500, "text/plain", "Error al eliminar");
      }
    } else {
      request->send(400, "text/plain", "Falta parámetro 'file'");
    }
  });
  
  delay(500);
  server.begin();
  
  Serial.println("✅ Servidor web iniciado");
}

// ============================================
// ✅ SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("🚀 LoRa RX - XIAO ESP32-S3 - BROADCAST MODE");
  
  // ✅ PASO 1: Configurar pin RXEN PRIMERO Y MANTENERLO ACTIVO
  Serial.println("📌 Configurando pin RXEN...");
  pinMode(LORA_RXEN, OUTPUT);
  digitalWrite(LORA_RXEN, HIGH);  // ✅ ACTIVAR INMEDIATAMENTE
  delay(100);
  Serial.println("✅ RXEN configurado y ACTIVO");
  
  // ✅ PASO 2: Configurar pines de control
  Serial.println("📌 Configurando pines de control...");
  pinMode(LORA_RST, OUTPUT);
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);
  delay(100);
  
  // Reset manual del módulo
  Serial.println("📌 Reset manual del módulo...");
  digitalWrite(LORA_RST, LOW);
  delay(100);
  digitalWrite(LORA_RST, HIGH);
  delay(200);
  Serial.println("✅ Reset completado");
  
  // ✅ PASO 3: Configurar SPI estándar
  Serial.println("📌 Inicializando SPI...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  Serial.println("✅ SPI configurado");
  Serial.printf("   SCK: %d, MISO: %d, MOSI: %d, CS: %d\n", 
                LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  
  // ✅ PASO 4: Inicializar LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while (1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");
  
  // ✅ PASO 5: Configurar WiFi AP
  Serial.println("\n📡 Configurando WiFi AP...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP: %s | IP: %s\n", ssid, IP.toString().c_str());
  delay(1000);
  
  // ✅ PASO 6: Inicializar radio
  Serial.println("\n📻 Iniciando radio SX1262...");
  Serial.printf("   Pines: CS=%d, DIO1=%d, RST=%d, BUSY=%d\n", 
                LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
  
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    while (true) delay(1000);
  }
  
  // Configurar DIO2 como RF switch (puede fallar en E22, no es crítico)
  state = radio.setDio2AsRfSwitch(true);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️ Advertencia: Error configurando DIO2: %d (normal en E22)\n", state);
  }
  
  // ✅ CONFIGURACIÓN CRÍTICA: debe coincidir EXACTAMENTE con el TX
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);  // ✅ CRÍTICO: debe ser 0x12
  radio.setOutputPower(17);
  
  // Configurar interrupción
  radio.setDio1Action(setFlag);
  
  // ✅ CRÍTICO: Asegurar que RXEN está activo ANTES de startReceive
  setRXMode(true);
  delay(100);
  
  // Iniciar recepción
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error en startReceive: %d\n", state);
    while (true) delay(1000);
  }
  
  Serial.println("✅ Radio configurado y ESCUCHANDO");
  Serial.printf("📻 Bandwidth:        %.1f kHz\n", currentBW);
  Serial.printf("📻 Spreading Factor: %d\n", currentSF);
  Serial.printf("📻 Coding Rate:      4/%d\n", currentCR);
  Serial.printf("📻 Frecuencia:       915 MHz\n");
  Serial.printf("📻 SyncWord:         0x12\n");
  Serial.printf("📻 RXEN:             Pin %d = HIGH\n", LORA_RXEN);
  
  // ✅ Iniciar servidor web
  delay(500);
  setupWebServer();
  
  Serial.println("✅ Servidor web activo");
  Serial.printf("🌐 Interfaz web: http://%s\n", IP.toString().c_str());
  Serial.println("\n📡 ESPERANDO DATOS LoRa...\n");
  Serial.println("👂 Receptor ACTIVO - Esperando paquetes broadcast del Heltec...\n");
}

// ============================================
// ✅ LOOP
// ============================================
void loop() {
  // ✅ AGREGAR: Heartbeat cada 10 segundos
  static unsigned long lastDebugPrint = 0;
  if (millis() - lastDebugPrint > 10000) {
    Serial.println("👂 Escuchando... (radio activo)");
    float rssi = radio.getRSSI();
    float snr = radio.getSNR();
    Serial.printf("   RSSI: %.1f dBm | SNR: %.1f dB\n", rssi, snr);
    lastDebugPrint = millis();
  }
  
  if (receivedFlag) {
    receivedFlag = false;
    
    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);
    
    if (state == RADIOLIB_ERR_NONE) {
      size_t len = radio.getPacketLength();
      float rssi = radio.getRSSI();
      float snr = radio.getSNR();
      
      Serial.printf("📡 Paquete recibido: %zu bytes (RSSI=%.1f dBm, SNR=%.1f dB)\n", len, rssi, snr);
      
      processPacket(buffer, len);
    } else {
      Serial.printf("⚠️ Error leyendo paquete: %d\n", state);
    }
    
    // ✅ CRÍTICO: Volver a activar recepción
    radio.startReceive();
  }
  
  // Timeout de sesión
  if (currentSession.active && (millis() - currentSession.lastPacketTime > RX_TIMEOUT)) {
    Serial.println("⏱️ Timeout: finalizando sesión...");
    finalizeFile();
  }
  
  delay(1);
}