/**
 * ════════════════════════════════════════════════════════════════════════
 * 📡 File Transfer System TX - Heltec WiFi LoRa 32 V3 (ESP32-S3)
 * ════════════════════════════════════════════════════════════════════════
 * 
 * Sistema completo de gestión y transferencia de archivos vía BLE + LoRa
 * 
 * MODO: TRANSMISOR (TX)
 * 
 * Características:
 * - LittleFS para almacenamiento persistente
 * - BLE para control desde Android
 * - LoRa para transmisión de archivos a RX
 * - Protocolo con ACK para confiabilidad
 * - Configuración dinámica de parámetros LoRa
 * - Progress tracking en tiempo real
 * - Manejo robusto de errores
 * 
 * @author alex127845
 * @date 2025-01-21
 * @version 3.0 TX
 */

#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

// ════════════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - PINES HELTEC V3
// ════════════════════════════════════════════════════════════════════════

#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14

// ════════════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - BLE
// ════════════════════════════════════════════════════════════════════════

#define DEVICE_NAME "Heltec-TX"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_WRITE_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_READ_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PROGRESS_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ════════════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - TRANSFERENCIAS
// ════════════════════════════════════════════════════════════════════════

#define CHUNK_SIZE_BLE 200         // Chunks para BLE (bytes)
#define CHUNK_SIZE_LORA 240        // Chunks para LoRa (bytes)
#define MAX_FILENAME_LENGTH 64
#define ACK_TIMEOUT_BASE 1200      // Timeout base para ACK (ms)
#define MAX_RETRIES 3              // Máximo de reintentos por fragmento
#define METADATA_MAGIC_1 0x4C      // Magic byte 1
#define METADATA_MAGIC_2 0x4D      // Magic byte 2

// ════════════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - RADIO LORA
// ════════════════════════════════════════════════════════════════════════

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Parámetros LoRa configurables
float currentBW = 125.0;           // Bandwidth en kHz
int currentSF = 9;                 // Spreading Factor
int currentCR = 7;                 // Coding Rate (4/7)
int currentACKInterval = 5;        // ACK cada N fragmentos
int currentPower = 17;             // Potencia en dBm

// Estado de transmisión LoRa
volatile bool ackReceived = false;
volatile bool transmitting = false;
volatile bool receivingACK = false;
String currentLoRaFile = "";
unsigned long loraTransmissionStartTime = 0;
uint16_t totalLoRaPacketsSent = 0;
uint16_t totalLoRaRetries = 0;

// ════════════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - BLE
// ════════════════════════════════════════════════════════════════════════

BLEServer* pServer = NULL;
BLECharacteristic* pCmdCharacteristic = NULL;
BLECharacteristic* pDataCharacteristic = NULL;
BLECharacteristic* pProgressCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// ════════════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - ESTADO DE TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════════════

enum TransferState {
  STATE_IDLE,
  STATE_UPLOADING,
  STATE_DOWNLOADING
};

TransferState currentState = STATE_IDLE;
String currentFilename = "";
File currentFile;
uint32_t expectedFileSize = 0;
uint32_t transferredBytes = 0;
uint16_t expectedChunks = 0;
uint16_t receivedChunks = 0;

// ════════════════════════════════════════════════════════════════════════
// 📝 DECLARACIÓN DE FUNCIONES
// ════════════════════════════════════════════════════════════════════════

// Setup
void setupLittleFS();
void setupBLE();
void setupLoRa();
void applyLoRaConfig();

// BLE - Comandos
void handleCommand(String command);
void sendResponse(String response);
void sendProgress(uint8_t percentage);

// BLE - Gestión de archivos
void listFiles();
void deleteFile(String filename);
void startUpload(String filename, uint32_t fileSize);
void receiveChunk(String base64Data);
void startDownload(String filename);
void sendFileInChunks(String filename);

// LoRa - Configuración
void setLoRaConfig(String jsonStr);
void sendCurrentLoRaConfig();
int getInterPacketDelay();
int getACKTimeout();

// LoRa - Transmisión
void startLoRaTransmission(String filename);
void processLoRaTransmission();
bool sendFileViaLoRa(const char* path);

// Utilidades
String encodeBase64(uint8_t* data, size_t length);
size_t decodeBase64(String input, uint8_t* output, size_t maxLen);
void resetTransferState();

// ════════════════════════════════════════════════════════════════════════
// 🔌 BLE CALLBACKS
// ════════════════════════════════════════════════════════════════════════

class ServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("\n✅ Cliente BLE conectado");
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("\n❌ Cliente BLE desconectado");
    
    // Limpiar estado de transferencia BLE
    if (currentState != STATE_IDLE) {
      Serial.println("⚠️  Transferencia BLE interrumpida, limpiando...");
      if (currentFile) currentFile.close();
      
      if (currentState == STATE_UPLOADING && LittleFS.exists(currentFilename)) {
        LittleFS.remove(currentFilename);
        Serial.println("🗑️  Archivo incompleto eliminado");
      }
      
      resetTransferState();
    }
  }
};

class CmdCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    uint8_t* pData = pCharacteristic->getData();
    size_t len = pCharacteristic->getValue().length();
    
    if (len > 0 && pData != nullptr) {
      String command = "";
      for (size_t i = 0; i < len; i++) {
        command += (char)pData[i];
      }
      command.trim();
      
      if (command.length() > 0) {
        Serial.println("\n📩 Comando BLE recibido: " + command);
        handleCommand(command);
      }
    }
  }
};

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA CALLBACK - Interrupción DIO1
// ════════════════════════════════════════════════════════════════════════

void IRAM_ATTR setFlag(void) {
  if (receivingACK) {
    ackReceived = true;
  }
}

// ════════════════════════════════════════════════════════════════════════
// 🚀 SETUP
// ════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n");
  Serial.println("════════════════════════════════════════════════");
  Serial.println("  📡 File Transfer System v3.0 TX");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("  MODO: TRANSMISOR");
  Serial.println("════════════════════════════════════════════════");
  Serial.println();
  
  setupLittleFS();
  setupBLE();
  setupLoRa();
  
  Serial.println("\n✅ Sistema TX listo");
  Serial.println("👂 Esperando conexión BLE...");
  Serial.println("📡 Radio LoRa configurado para TX\n");
}

// ════════════════════════════════════════════════════════════════════════
// 🔁 LOOP
// ════════════════════════════════════════════════════════════════════════

void loop() {
  // Manejar reconexión BLE
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("🔄 Esperando reconexión BLE...");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  // Procesar transmisión LoRa si está activa
  if (transmitting) {
    processLoRaTransmission();
  }
  
  yield();
  delay(10);
}

// ════════════════════════════════════════════════════════════════════════
// 💾 LITTLEFS - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════════════

void setupLittleFS() {
  Serial.println("💾 Inicializando LittleFS...");
  
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  
  Serial.println("✅ LittleFS montado");
  
  uint32_t totalBytes = LittleFS.totalBytes();
  uint32_t usedBytes = LittleFS.usedBytes();
  
  Serial.printf("   Total: %.2f MB\n", totalBytes / 1048576.0);
  Serial.printf("   Usado: %.2f MB\n", usedBytes / 1048576.0);
  Serial.printf("   Libre: %.2f MB\n", (totalBytes - usedBytes) / 1048576.0);
  
  // Listar archivos
  Serial.println("\n📁 Archivos:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int count = 0;
  
  while (file) {
    if (!file.isDirectory()) {
      Serial.printf("   - %s (%.2f KB)\n", file.name(), file.size() / 1024.0);
      count++;
    }
    file = root.openNextFile();
  }
  
  if (count == 0) Serial.println("   (vacío)");
}

// ════════════════════════════════════════════════════════════════════════
// 📡 BLE - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════════════

void setupBLE() {
  Serial.println("\n📡 Inicializando BLE...");
  
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(517);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Característica CMD (WRITE)
  pCmdCharacteristic = pService->createCharacteristic(
    CMD_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCmdCharacteristic->setCallbacks(new CmdCallbacks());
  
  // Característica DATA (READ/NOTIFY)
  pDataCharacteristic = pService->createCharacteristic(
    DATA_READ_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pDataCharacteristic->addDescriptor(new BLE2902());
  
  // Característica PROGRESS (NOTIFY)
  pProgressCharacteristic = pService->createCharacteristic(
    PROGRESS_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pProgressCharacteristic->addDescriptor(new BLE2902());
  
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  
  Serial.println("✅ BLE iniciado: " + String(DEVICE_NAME));
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════════════

void setupLoRa() {
  Serial.println("\n📡 Inicializando radio LoRa...");
  
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    while (true) delay(1000);
  }
  
  Serial.println("✅ SX1262 inicializado");
  
  applyLoRaConfig();
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA - APLICAR CONFIGURACIÓN
// ════════════════════════════════════════════════════════════════════════

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando configuración LoRa...");
  
  radio.standby();
  delay(100);
  
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(currentPower);
  radio.setCRC(true);
  
  delay(100);
  
  Serial.println("📻 Configuración LoRa TX:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   Power: %d dBm\n", currentPower);
  Serial.printf("   ACK cada: %d fragmentos\n", currentACKInterval);
  Serial.println("✅ Configuración aplicada\n");
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA - CALCULAR DELAYS
// ════════════════════════════════════════════════════════════════════════

int getInterPacketDelay() {
  if (currentBW >= 500.0) {
    if (currentSF <= 7) return 80;
    if (currentSF == 9) return 120;
    return 150;
  } else if (currentBW >= 250.0) {
    if (currentSF <= 7) return 100;
    if (currentSF == 9) return 150;
    return 180;
  } else {
    if (currentSF <= 7) return 120;
    if (currentSF == 9) return 150;
    return 200;
  }
}

int getACKTimeout() {
  if (currentBW >= 500.0 && currentSF <= 9) return 800;
  if (currentBW >= 250.0 && currentSF <= 9) return 1200;
  if (currentSF >= 12) return 2000;
  return 1500;
}

// ════════════════════════════════════════════════════════════════════════
// 🎯 MANEJO DE COMANDOS BLE
// ════════════════════════════════════════════════════════════════════════

void handleCommand(String command) {
  command.trim();
  
  // Comando: LIST
  if (command == "CMD:LIST") {
    Serial.println("📋 Procesando: LIST");
    listFiles();
  }
  
  // Comando: DELETE:filename
  else if (command.startsWith("CMD:DELETE:")) {
    String filename = command.substring(11);
    Serial.println("🗑️  Procesando: DELETE - " + filename);
    deleteFile(filename);
  }
  
  // Comando: UPLOAD_START:filename:size
  else if (command.startsWith("CMD:UPLOAD_START:")) {
    int firstColon = command.indexOf(':', 17);
    if (firstColon > 0) {
      String filename = command.substring(17, firstColon);
      uint32_t fileSize = command.substring(firstColon + 1).toInt();
      startUpload(filename, fileSize);
    } else {
      sendResponse("ERROR:INVALID_UPLOAD_COMMAND");
    }
  }
  
  // Comando: UPLOAD_CHUNK:base64data
  else if (command.startsWith("CMD:UPLOAD_CHUNK:")) {
    String base64Data = command.substring(17);
    receiveChunk(base64Data);
  }
  
  // Comando: DOWNLOAD:filename
  else if (command.startsWith("CMD:DOWNLOAD:")) {
    String filename = command.substring(13);
    Serial.println("📥 Procesando: DOWNLOAD - " + filename);
    startDownload(filename);
  }
  
  // Comando: SET_LORA_CONFIG (NUEVO)
  else if (command.startsWith("CMD:SET_LORA_CONFIG:")) {
    String jsonStr = command.substring(20);
    Serial.println("⚙️  Procesando: SET_LORA_CONFIG");
    setLoRaConfig(jsonStr);
  }
  
  // Comando: GET_LORA_CONFIG (NUEVO)
  else if (command == "CMD:GET_LORA_CONFIG") {
    Serial.println("⚙️  Procesando: GET_LORA_CONFIG");
    sendCurrentLoRaConfig();
  }
  
  // Comando: TX_FILE:filename (NUEVO)
  else if (command.startsWith("CMD:TX_FILE:")) {
    String filename = command.substring(12);
    Serial.println("📡 Procesando: TX_FILE - " + filename);
    startLoRaTransmission(filename);
  }
  
  // Comando: PING
  else if (command == "CMD:PING") {
    Serial.println("🏓 Procesando: PING");
    sendResponse("PONG");
  }
  
  // Comando desconocido
  else {
    Serial.println("⚠️  Comando desconocido: " + command);
    sendResponse("ERROR:UNKNOWN_COMMAND");
  }
}

// ════════════════════════════════════════════════════════════════════════
// 📤 ENVIAR RESPUESTA BLE
// ════════════════════════════════════════════════════════════════════════

void sendResponse(String response) {
  if (!deviceConnected || pDataCharacteristic == NULL) return;
  
  response += "\n";
  pDataCharacteristic->setValue(response.c_str());
  pDataCharacteristic->notify();
  delay(10);
}

// ════════════════════════════════════════════════════════════════════════
// 📊 ENVIAR PROGRESO BLE
// ════════════════════════════════════════════════════════════════════════

void sendProgress(uint8_t percentage) {
  if (!deviceConnected || pProgressCharacteristic == NULL) return;
  
  uint8_t data[1] = { percentage };
  pProgressCharacteristic->setValue(data, 1);
  pProgressCharacteristic->notify();
  delay(5);
}

// ════════════════════════════════════════════════════════════════════════
// 📋 LISTAR ARCHIVOS
// ════════════════════════════════════════════════════════════════════════

void listFiles() {
  sendResponse("FILES_START");
  
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int count = 0;
  
  while (file) {
    if (!file.isDirectory()) {
      String filename = String(file.name());
      if (filename.startsWith("/")) filename = filename.substring(1);
      
      String fileInfo = "FILE:" + filename + ":" + String(file.size());
      sendResponse(fileInfo);
      count++;
    }
    file = root.openNextFile();
  }
  
  sendResponse("FILES_END:" + String(count));
  Serial.printf("✅ Lista enviada: %d archivo(s)\n", count);
}

// ════════════════════════════════════════════════════════════════════════
// 🗑️  ELIMINAR ARCHIVO
// ════════════════════════════════════════════════════════════════════════

void deleteFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  if (transmitting && currentLoRaFile == filename) {
    sendResponse("ERROR:FILE_IN_USE");
    return;
  }
  
  if (currentState != STATE_IDLE && currentFilename == filename) {
    sendResponse("ERROR:FILE_IN_USE");
    return;
  }
  
  delay(100);
  
  if (LittleFS.remove(filename)) {
    Serial.println("✅ Eliminado: " + filename);
    sendResponse("OK:DELETED");
  } else {
    Serial.println("❌ Error eliminando");
    sendResponse("ERROR:CANT_DELETE");
  }
}

// ════════════════════════════════════════════════════════════════════════
// 📤 UPLOAD BLE - INICIAR
// ════════════════════════════════════════════════════════════════════════

void startUpload(String filename, uint32_t fileSize) {
  if (currentState != STATE_IDLE) {
    sendResponse("ERROR:TRANSFER_IN_PROGRESS");
    return;
  }
  
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (fileSize > freeSpace) {
    sendResponse("ERROR:NO_SPACE");
    return;
  }
  
  if (LittleFS.exists(filename)) {
    LittleFS.remove(filename);
  }
  
  currentFile = LittleFS.open(filename, "w");
  if (!currentFile) {
    sendResponse("ERROR:CREATE_FAILED");
    return;
  }
  
  currentState = STATE_UPLOADING;
  currentFilename = filename;
  expectedFileSize = fileSize;
  transferredBytes = 0;
  expectedChunks = (fileSize + CHUNK_SIZE_BLE - 1) / CHUNK_SIZE_BLE;
  receivedChunks = 0;
  
  Serial.printf("✅ Upload BLE iniciado: %s (%u bytes)\n", filename.c_str(), fileSize);
  
  sendResponse("OK:UPLOAD_READY");
  sendProgress(0);
}

// ════════════════════════════════════════════════════════════════════════
// 📦 UPLOAD BLE - RECIBIR CHUNK
// ════════════════════════════════════════════════════════════════════════

void receiveChunk(String base64Data) {
  if (currentState != STATE_UPLOADING) {
    sendResponse("ERROR:NOT_UPLOADING");
    return;
  }
  
  uint8_t buffer[CHUNK_SIZE_BLE + 10];
  size_t decodedLen = decodeBase64(base64Data, buffer, sizeof(buffer));
  
  if (decodedLen == 0) {
    sendResponse("ERROR:DECODE_FAILED");
    return;
  }
  
  size_t written = currentFile.write(buffer, decodedLen);
  if (written != decodedLen) {
    currentFile.close();
    LittleFS.remove(currentFilename);
    resetTransferState();
    sendResponse("ERROR:WRITE_FAILED");
    return;
  }
  
  transferredBytes += written;
  receivedChunks++;
  
  uint8_t progress = (transferredBytes * 100) / expectedFileSize;
  
  if (receivedChunks % 10 == 0 || receivedChunks >= expectedChunks) {
    sendProgress(progress);
  }
  
  sendResponse("ACK:" + String(receivedChunks));
  
  if (receivedChunks >= expectedChunks || transferredBytes >= expectedFileSize) {
    currentFile.flush();
    currentFile.close();
    
    Serial.printf("✅ Upload BLE completo: %s\n", currentFilename.c_str());
    
    sendResponse("OK:UPLOAD_COMPLETE:" + String(transferredBytes));
    sendProgress(100);
    
    resetTransferState();
  }
}

// ════════════════════════════════════════════════════════════════════════
// 📥 DOWNLOAD BLE - INICIAR
// ════════════════════════════════════════════════════════════════════════

void startDownload(String filename) {
  if (currentState != STATE_IDLE) {
    sendResponse("ERROR:TRANSFER_IN_PROGRESS");
    return;
  }
  
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  File file = LittleFS.open(filename, "r");
  if (!file) {
    sendResponse("ERROR:OPEN_FAILED");
    return;
  }
  
  uint32_t fileSize = file.size();
  file.close();
  
  currentState = STATE_DOWNLOADING;
  currentFilename = filename;
  expectedFileSize = fileSize;
  transferredBytes = 0;
  
  String cleanName = filename;
  if (cleanName.startsWith("/")) cleanName = cleanName.substring(1);
  
  sendResponse("DOWNLOAD_START:" + cleanName + ":" + String(fileSize));
  delay(100);
  
  sendFileInChunks(filename);
}

// ════════════════════════════════════════════════════════════════════════
// 📤 DOWNLOAD BLE - ENVIAR EN CHUNKS
// ════════════════════════════════════════════════════════════════════════

void sendFileInChunks(String filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) {
    sendResponse("ERROR:FILE_OPEN_FAILED");
    resetTransferState();
    return;
  }
  
  uint32_t totalSize = file.size();
  uint16_t totalChunks = (totalSize + CHUNK_SIZE_BLE - 1) / CHUNK_SIZE_BLE;
  uint16_t chunkNum = 0;
  
  sendProgress(0);
  
  uint8_t buffer[CHUNK_SIZE_BLE];
  
  while (file.available()) {
    size_t bytesRead = file.read(buffer, CHUNK_SIZE_BLE);
    if (bytesRead == 0) break;
    
    String encoded = encodeBase64(buffer, bytesRead);
    String chunkMsg = "CHUNK:" + String(chunkNum) + ":" + encoded;
    sendResponse(chunkMsg);
    
    transferredBytes += bytesRead;
    chunkNum++;
    
    uint8_t progress = (transferredBytes * 100) / totalSize;
    if (chunkNum % 5 == 0 || chunkNum >= totalChunks) {
      sendProgress(progress);
    }
    
    delay(20);
  }
  
  file.close();
  
  sendResponse("DOWNLOAD_END:" + String(transferredBytes));
  sendProgress(100);
  
  Serial.printf("✅ Download BLE completo: %u bytes\n", transferredBytes);
  
  resetTransferState();
}

// ════════════════════════════════════════════════════════════════════════
// ⚙️  CONFIGURACIÓN LORA - SET
// ════════════════════════════════════════════════════════════════════════

void setLoRaConfig(String jsonStr) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    sendResponse("ERROR:INVALID_JSON");
    return;
  }
  
  if (transmitting) {
    sendResponse("ERROR:TRANSMITTING");
    return;
  }
  
  currentBW = doc["bw"];
  currentSF = doc["sf"];
  currentCR = doc["cr"];
  currentACKInterval = doc["ack"];
  currentPower = doc["power"];
  
  applyLoRaConfig();
  
  sendResponse("OK:LORA_CONFIG_SET");
  Serial.println("✅ Configuración LoRa actualizada");
}

// ════════════════════════════════════════════════════════════════════════
// ⚙️  CONFIGURACIÓN LORA - GET
// ════════════════════════════════════════════════════════════════════════

void sendCurrentLoRaConfig() {
  StaticJsonDocument<200> doc;
  doc["bw"] = (int)currentBW;
  doc["sf"] = currentSF;
  doc["cr"] = currentCR;
  doc["ack"] = currentACKInterval;
  doc["power"] = currentPower;
  
  String json;
  serializeJson(doc, json);
  
  sendResponse("LORA_CONFIG:" + json);
  Serial.println("✅ Config LoRa enviada");
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA TX - INICIAR TRANSMISIÓN
// ════════════════════════════════════════════════════════════════════════

void startLoRaTransmission(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  if (transmitting) {
    sendResponse("ERROR:ALREADY_TRANSMITTING");
    return;
  }
  
  currentLoRaFile = filename;
  transmitting = true;
  totalLoRaPacketsSent = 0;
  totalLoRaRetries = 0;
  
  sendResponse("OK:TX_STARTING");
  Serial.println("📡 Iniciando transmisión LoRa...");
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA TX - PROCESAR TRANSMISIÓN
// ════════════════════════════════════════════════════════════════════════

void processLoRaTransmission() {
  Serial.printf("\n📡 Transmitiendo por LoRa: %s\n", currentLoRaFile.c_str());
  loraTransmissionStartTime = millis();
  
  bool result = sendFileViaLoRa(currentLoRaFile.c_str());
  
  float transmissionTime = (millis() - loraTransmissionStartTime) / 1000.0;
  
  File f = LittleFS.open(currentLoRaFile, "r");
  uint32_t fileSize = f ? f.size() : 0;
  if (f) f.close();
  
  float speed = (fileSize * 8.0) / (transmissionTime * 1000.0);
  
  if (result) {
    String status = "TX_COMPLETE:" + String(fileSize) + ":" + 
                   String(transmissionTime, 2) + ":" + 
                   String(speed, 2);
    sendResponse(status);
    
    Serial.println("\n✅ Transmisión LoRa exitosa");
    Serial.printf("⏱️  Tiempo: %.2f s\n", transmissionTime);
    Serial.printf("⚡ Velocidad: %.2f kbps\n", speed);
    Serial.printf("📦 Paquetes: %u\n", totalLoRaPacketsSent);
    Serial.printf("🔄 Reintentos: %u\n", totalLoRaRetries);
  } else {
    sendResponse("TX_FAILED:TRANSMISSION_ERROR");
    Serial.println("\n❌ Transmisión LoRa fallida");
  }
  
  transmitting = false;
  currentLoRaFile = "";
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA TX - ENVIAR ARCHIVO
// ════════════════════════════════════════════════════════════════════════

bool sendFileViaLoRa(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint32_t totalSize = f.size();
  
  String fileName = String(path);
  if (fileName.startsWith("/")) fileName = fileName.substring(1);
  
  Serial.printf("📁 Archivo: %s (%u bytes)\n", fileName.c_str(), totalSize);
  
  // ═══════════════════════════════════════════════════════════════
  // 1. ENVIAR METADATA
  // ═══════════════════════════════════════════════════════════════
  
  uint8_t nameLen = min((size_t)fileName.length(), (size_t)100);
  uint8_t metaPkt[2 + 4 + 1 + nameLen];
  
  metaPkt[0] = METADATA_MAGIC_1;
  metaPkt[1] = METADATA_MAGIC_2;
  memcpy(metaPkt + 2, &totalSize, 4);
  metaPkt[6] = nameLen;
  memcpy(metaPkt + 7, fileName.c_str(), nameLen);
  
  Serial.println("📤 Enviando metadata...");
  
  if (radio.transmit(metaPkt, 7 + nameLen) != RADIOLIB_ERR_NONE) {
    f.close();
    Serial.println("❌ Error enviando metadata");
    return false;
  }
  
  Serial.println("✅ Metadata enviado");
  delay(600);
  
  // ═══════════════════════════════════════════════════════════════
  // 2. ENVIAR DATOS EN FRAGMENTOS
  // ═══════════════════════════════════════════════════════════════
  
  uint16_t totalChunks = (totalSize + CHUNK_SIZE_LORA - 1) / CHUNK_SIZE_LORA;
  Serial.printf("📦 Total fragmentos: %u\n\n", totalChunks);
  
  for (uint16_t index = 0; index < totalChunks; index++) {
    uint8_t buffer[CHUNK_SIZE_LORA];
    size_t bytesRead = f.read(buffer, CHUNK_SIZE_LORA);
    if (bytesRead == 0) break;

    bool success = false;
    int retries = 0;

    while (!success && retries < MAX_RETRIES) {
      // Crear paquete: índice(2) + total(2) + datos
      uint8_t pkt[4 + bytesRead];
      memcpy(pkt, &index, 2);
      memcpy(pkt + 2, &totalChunks, 2);
      memcpy(pkt + 4, buffer, bytesRead);

      // Transmitir
      if (radio.transmit(pkt, 4 + bytesRead) != RADIOLIB_ERR_NONE) {
        retries++;
        totalLoRaRetries++;
        Serial.printf("⚠️  Reintento %d/3 fragmento %u\n", retries, index + 1);
        delay(500);
        continue;
      }

      totalLoRaPacketsSent++;
      
      bool isLast = (index + 1 == totalChunks);
      bool needACK = ((index + 1) % currentACKInterval == 0);
      
      // ═══════════════════════════════════════════════════════════
      // 3. ESPERAR ACK SI ES NECESARIO
      // ═══════════════════════════════════════════════════════════
      
      if (needACK || isLast) {
        delay(200);
        receivingACK = true;
        radio.setDio1Action(setFlag);
        radio.startReceive();
        
        unsigned long start = millis();
        bool validAck = false;
        
        while (millis() - start < getACKTimeout() && !validAck) {
          if (ackReceived) {
            ackReceived = false;
            uint8_t ackBuf[20];
            
            if (radio.readData(ackBuf, sizeof(ackBuf)) == RADIOLIB_ERR_NONE) {
              if (radio.getPacketLength() == 5 && 
                  ackBuf[0] == 'A' && ackBuf[1] == 'C' && ackBuf[2] == 'K') {
                uint16_t ackNum;
                memcpy(&ackNum, ackBuf + 3, 2);
                if (ackNum == index + 1) {
                  validAck = true;
                  success = true;
                  Serial.printf("✅ ACK %u/%u recibido\n", index + 1, totalChunks);
                }
              }
              if (!validAck) radio.startReceive();
            }
          }
          delayMicroseconds(5000);
        }
        
        receivingACK = false;
        
        if (!success) {
          retries++;
          totalLoRaRetries++;
          Serial.printf("⚠️  Timeout ACK, reintento %d/3\n", retries);
          delay(800);
        }
      } else {
        success = true;
      }
      
      // Notificar progreso por BLE
      if (success && (index + 1) % 10 == 0) {
        uint8_t progress = ((index + 1) * 100) / totalChunks;
        sendProgress(progress);
        
        String status = "TX_STATUS:" + String(index + 1) + "/" + 
                       String(totalChunks) + ":" + String(totalLoRaRetries);
        sendResponse(status);
      }
    }

    if (!success) {
      f.close();
      Serial.printf("❌ Fallo en fragmento %u después de %d reintentos\n", 
                   index + 1, MAX_RETRIES);
      return false;
    }

    delay(getInterPacketDelay());
  }

  f.close();
  sendProgress(100);
  
  Serial.println("\n✅ Todos los fragmentos enviados");
  return true;
}

// ════════════════════════════════════════════════════════════════════════
// 🔐 BASE64 - CODIFICACIÓN
// ════════════════════════════════════════════════════════════════════════

String encodeBase64(uint8_t* data, size_t length) {
  size_t outputLen;
  mbedtls_base64_encode(NULL, 0, &outputLen, data, length);
  
  uint8_t* encoded = (uint8_t*)malloc(outputLen + 1);
  mbedtls_base64_encode(encoded, outputLen + 1, &outputLen, data, length);
  encoded[outputLen] = '\0';
  
  String result = String((char*)encoded);
  free(encoded);
  
  return result;
}

// ════════════════════════════════════════════════════════════════════════
// 🔐 BASE64 - DECODIFICACIÓN
// ════════════════════════════════════════════════════════════════════════

size_t decodeBase64(String input, uint8_t* output, size_t maxLen) {
  size_t outputLen;
  
  int ret = mbedtls_base64_decode(
    output, 
    maxLen, 
    &outputLen, 
    (const unsigned char*)input.c_str(), 
    input.length()
  );
  
  if (ret != 0) return 0;
  
  return outputLen;
}

// ════════════════════════════════════════════════════════════════════════
// 🔄 RESETEAR ESTADO DE TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════════════

void resetTransferState() {
  if (currentFile) currentFile.close();
  
  currentState = STATE_IDLE;
  currentFilename = "";
  expectedFileSize = 0;
  transferredBytes = 0;
  expectedChunks = 0;
  receivedChunks = 0;
}