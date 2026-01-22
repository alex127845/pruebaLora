#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>

// ✅ AGREGAR ESTAS LÍNEAS PARA ASYNCTCP (ESP32 Core 3. x)
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

// ✅ Control RXEN (RX mode)
void setRXMode(bool enable) {
  pinMode(LORA_RXEN, OUTPUT);
  digitalWrite(LORA_RXEN, enable ? HIGH : LOW);
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
  
  if (currentSession.tempFileName. length() > 0 && LittleFS.exists(currentSession.tempFileName)) {
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
    Serial.println("❌ Error:  no hay memoria para flags");
    return false;
  }
  memset(currentSession.chunkReceived, 0, currentSession.totalChunks);
  
  currentSession.numParityBlocks = (currentSession.totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  currentSession.parityBlocks = new FileSession:: FECBlock[currentSession.numParityBlocks];
  if (currentSession. parityBlocks == nullptr) {
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
                fileName. c_str(), totalSize, currentSession.totalChunks);
  
  return true;
}

// ============================================
// ✅ GUARDAR CHUNK
// ============================================
bool saveChunk(uint16_t chunkIndex, const uint8_t* data, uint16_t length) {
  if (! currentSession.active || chunkIndex >= currentSession.totalChunks) {
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
  currentSession. chunksReceivedCount++;
  currentSession.lastPacketTime = millis();
  
  return true;
}

// ============================================
// ✅ FEC:  RECUPERAR CHUNKS PERDIDOS
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
          Serial.printf("✅ FEC:  recuperado chunk %u\n", missingIdx);
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
  
  Serial.printf("✅ ARCHIVO RECIBIDO: %s (%u bytes en %. 2fs = %. 2f B/s)\n", 
                currentSession.fileName.c_str(), currentSession.totalSize, duration, speed);
  Serial.printf("📊 Paquetes:  %u | Duplicados: %u | Recuperados FEC: %u\n",
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
  if (len < 16) return;
  
  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  
  uint32_t totalSize;
  memcpy(&totalSize, data + 6, 4);
  
  uint16_t chunkSize;
  memcpy(&chunkSize, data + 10, 2);
  
  uint8_t nameLen = data[12];
  if (nameLen == 0 || nameLen > 64 || len < 13 + nameLen + 2) return;
  
  char fileName[65];
  memcpy(fileName, data + 13, nameLen);
  fileName[nameLen] = '\0';
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + 13 + nameLen, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data + 2, 11 + nameLen);
  
  if (receivedCRC != calculatedCRC) {
    Serial.println("❌ Manifest:  CRC inválido");
    totalCrcErrors++;
    return;
  }
  
  if (! currentSession.active || currentSession.fileID != fileID) {
    startSession(fileID, String(fileName), totalSize, chunkSize);
  } else {
    currentSession.lastPacketTime = millis();
  }
}

// ============================================
// ✅ PROCESAR DATA CHUNK
// ============================================
void processDataChunk(const uint8_t* data, size_t len) {
  if (!currentSession.active || len < 8) return;
  
  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  
  if (fileID != currentSession.fileID) return;
  
  uint16_t chunkIndex;
  memcpy(&chunkIndex, data + 6, 2);
  
  uint16_t dataLen = len - 10;
  const uint8_t* chunkData = data + 8;
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data + 2, len - 4);
  
  if (receivedCRC != calculatedCRC) {
    totalCrcErrors++;
    return;
  }
  
  saveChunk(chunkIndex, chunkData, dataLen);
}

// ============================================
// ✅ PROCESAR PARITY BLOCK
// ============================================
void processParityBlock(const uint8_t* data, size_t len) {
  if (!currentSession.active || len < 8) return;
  
  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  
  if (fileID != currentSession.fileID) return;
  
  uint16_t blockIndex;
  memcpy(&blockIndex, data + 6, 2);
  
  if (blockIndex >= currentSession.numParityBlocks) return;
  
  uint16_t dataLen = len - 10;
  const uint8_t* parityData = data + 8;
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data + 2, len - 4);
  
  if (receivedCRC != calculatedCRC) {
    totalCrcErrors++;
    return;
  }
  
  if (currentSession.parityBlocks[blockIndex].received) return;
  
  currentSession. parityBlocks[blockIndex]. data = new uint8_t[dataLen];
  if (currentSession.parityBlocks[blockIndex].data == nullptr) return;
  
  memcpy(currentSession.parityBlocks[blockIndex].data, parityData, dataLen);
  currentSession.parityBlocks[blockIndex].length = dataLen;
  currentSession.parityBlocks[blockIndex].received = true;
  
  currentSession.lastPacketTime = millis();
}

// ============================================
// ✅ PROCESAR FILE END
// ============================================
void processFileEnd(const uint8_t* data, size_t len) {
  if (!currentSession.active || len < 8) return;
  
  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  
  if (fileID != currentSession.fileID) return;
  
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + 6, 2);
  
  uint16_t calculatedCRC = crc16_ccitt(data + 2, 4);
  
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
  }
}

// ============================================
// ✅ WEB SERVER - HTML
// ============================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>LoRa RX - XIAO ESP32-S3</title>
  <style>
    body { font-family: Arial; margin: 20px; background:  #f0f0f0; }
    .container { max-width: 800px; margin:  auto; background: white; padding: 20px; border-radius: 8px; }
    h1 { color: #333; }
    .status { padding: 10px; margin: 10px 0; border-radius: 4px; }
    .active { background: #4CAF50; color: white; }
    .inactive { background: #ccc; color: #666; }
    button { padding: 10px 20px; margin: 5px; background: #2196F3; color: white; border: none; border-radius:  4px; cursor: pointer; }
    button:hover { background: #0b7dda; }
    table { width: 100%; border-collapse: collapse; margin: 10px 0; }
    th, td { padding: 8px; text-align: left; border-bottom: 1px solid #ddd; }
    th { background: #4CAF50; color: white; }
    . file-link { color: #2196F3; text-decoration: none; }
    .file-link:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <div class="container">
    <h1>📡 LoRa Receiver - XIAO ESP32-S3</h1>
    
    <div class="status" id="status">Estado: Esperando... </div>
    
    <h2>⚙️ Configuración LoRa</h2>
    <form action="/config" method="POST">
      <label>Bandwidth (kHz): <input type="number" name="bw" value="125" step="0.1"></label><br><br>
      <label>Spreading Factor: <input type="number" name="sf" value="9" min="7" max="12"></label><br><br>
      <label>Coding Rate (4/x): <input type="number" name="cr" value="7" min="5" max="8"></label><br><br>
      <button type="submit">Aplicar</button>
    </form>
    
    <h2>📊 Estadísticas</h2>
    <table>
      <tr><th>Métrica</th><th>Valor</th></tr>
      <tr><td>Paquetes recibidos</td><td id="packets">0</td></tr>
      <tr><td>Errores CRC</td><td id="crc">0</td></tr>
      <tr><td>Recuperados FEC</td><td id="fec">0</td></tr>
      <tr><td>Duplicados</td><td id="dup">0</td></tr>
      <tr><td>Última velocidad</td><td id="speed">-</td></tr>
      <tr><td>Último archivo</td><td id="file">-</td></tr>
    </table>
    
    <h2>📁 Archivos Recibidos</h2>
    <div id="files"></div>
    <button onclick="location.reload()">🔄 Actualizar</button>
  </div>
  
  <script>
    setInterval(() => {
      fetch('/stats').then(r => r.json()).then(d => {
        document.getElementById('status').className = d.active ? 'status active' : 'status inactive';
        document.getElementById('status').textContent = d.active ?  '✅ Recibiendo:  ' + d.filename : '⏸️ Inactivo';
        document.getElementById('packets').textContent = d.packets;
        document.getElementById('crc').textContent = d.crc;
        document.getElementById('fec').textContent = d.fec;
        document.getElementById('dup').textContent = d.dup;
        document. getElementById('speed').textContent = d.speed;
        document.getElementById('file').textContent = d.lastFile;
      });
    }, 1000);
  </script>
</body>
</html>
)rawliteral";

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  
  server.on("/stats", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"active\":" + String(currentSession.active ? "true" : "false") + ",";
    json += "\"filename\": \"" + currentSession.fileName + "\",";
    json += "\"packets\":" + String(totalPacketsReceived) + ",";
    json += "\"crc\":" + String(totalCrcErrors) + ",";
    json += "\"fec\":" + String(totalRecovered) + ",";
    json += "\"dup\":" + String(totalDuplicates) + ",";
    json += "\"speed\": \"" + String(lastSpeed, 2) + " B/s\",";
    json += "\"lastFile\": \"" + String(lastFileSize) + " bytes en " + String(lastReceptionTime, 2) + "s\"";
    json += "}";
    request->send(200, "application/json", json);
  });
  
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("bw", true)) {
      currentBW = request->getParam("bw", true)->value().toFloat();
    }
    if (request->hasParam("sf", true)) {
      currentSF = request->getParam("sf", true)->value().toInt();
    }
    if (request->hasParam("cr", true)) {
      currentCR = request->getParam("cr", true)->value().toInt();
    }
    
    radio.standby();
    delay(100);
    radio.setBandwidth(currentBW);
    radio.setSpreadingFactor(currentSF);
    radio.setCodingRate(currentCR);
    delay(100);
    radio.startReceive();
    
    request->send(200, "text/plain", "Configuración aplicada");
  });
  
  server.serveStatic("/", LittleFS, "/");
  
  // ✅ Agregar delay antes de iniciar el servidor
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
  
  Serial.println("🚀 LoRa RX - XIAO ESP32-S3 - DEBUG MODE");
  
  // ✅ PASO 1: Configurar pin RXEN PRIMERO
  Serial.println("📌 Configurando pin RXEN.. .");
  pinMode(LORA_RXEN, OUTPUT);
  digitalWrite(LORA_RXEN, HIGH);
  delay(100);
  Serial.println("✅ RXEN configurado");
  
  // ✅ PASO 2: Configurar pines de control
  Serial.println("📌 Configurando pines de control...");
  pinMode(LORA_RST, OUTPUT);
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);  // CS en HIGH (inactivo)
  delay(100);
  
  // Reset manual del módulo
  Serial.println("📌 Reset manual del módulo...");
  digitalWrite(LORA_RST, LOW);
  delay(100);
  digitalWrite(LORA_RST, HIGH);
  delay(200);
  Serial.println("✅ Reset completado");
  
  // ✅ PASO 3: Configurar SPI estándar
  Serial.println("📌 Inicializando SPI personalizado...");
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
  Serial.println("\n📡 Configurando WiFi AP.. .");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP:  %s | IP: %s\n", ssid, IP.toString().c_str());
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
  
  // Configurar DIO2 como RF switch (control automático de antena)
  state = radio.setDio2AsRfSwitch(true);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Advertencia: Error configurando DIO2 como RF switch: %d\n", state);
  }
  
  // Aplicar configuración LoRa después de la inicialización
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  // Configurar interrupción e iniciar recepción
  radio.setDio1Action(setFlag);
  
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error en startReceive: %d\n", state);
  }
  
  Serial.println("✅ Radio configurado");
  Serial.println("👂 Escuchando paquetes LoRa...");
  Serial.printf("📻 Bandwidth:        %.1f kHz\n", currentBW);
  Serial.printf("📻 Spreading Factor: %d\n", currentSF);
  Serial.printf("📻 Coding Rate:      4/%d\n", currentCR);
  Serial.printf("📻 Frecuencia:       915 MHz\n");
  
  // ✅ Iniciar servidor web
  delay(500);
  setupWebServer();
  
  Serial.println("✅ Servidor web activo");
  Serial.printf("🌐 Interfaz web: http://%s\n", IP.toString().c_str());
  Serial.println("\n📡 ESPERANDO DATOS LoRa...\n");
}


// ============================================
// ✅ LOOP
// ============================================
void loop() {
  if (receivedFlag) {
    receivedFlag = false;
    
    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);
    
    if (state == RADIOLIB_ERR_NONE) {
      size_t len = radio.getPacketLength();
      processPacket(buffer, len);
    }
    
    radio.startReceive();
  }
  
  // ✅ Timeout de sesión
  if (currentSession. active && (millis() - currentSession.lastPacketTime > RX_TIMEOUT)) {
    Serial.println("⏱️ Timeout:  finalizando sesión.. .");
    finalizeFile();
  }
  
  delay(1);
}