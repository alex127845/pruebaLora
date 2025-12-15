/**
 * ════════════════════════════════════════════════════════════════════════
 * 📡 File Transfer System RX - Heltec WiFi LoRa 32 V3 (ESP32-S3)
 * ════════════════════════════════════════════════════════════════════════
 * 
 * Sistema completo de gestión y recepción de archivos vía BLE + LoRa
 * 
 * MODO: RECEPTOR (RX)
 * 
 * Características:
 * - LittleFS para almacenamiento persistente
 * - BLE para control desde Android
 * - LoRa para recepción de archivos desde TX
 * - Protocolo con ACK para confiabilidad
 * - Configuración dinámica de parámetros LoRa
 * - Progress tracking en tiempo real
 * - Detección automática de transmisiones
 * - Manejo robusto de errores
 * 
 * @author alex127845
 * @date 2025-01-21
 * @version 3.0 RX
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

#define DEVICE_NAME "Heltec-RX"
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
#define RX_TIMEOUT 30000           // Timeout de recepción (30s)
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

// Estado de recepción LoRa
volatile bool packetReceived = false;
bool receivingFile = false;
String receivingFileName = "";
uint32_t receivingFileSize = 0;
uint32_t receivedBytes = 0;
uint16_t expectedFragments = 0;
uint16_t receivedFragments = 0;
uint16_t lastFragmentIndex = 0;
File receivingFileHandle;
unsigned long lastPacketTime = 0;
unsigned long receptionStartTime = 0;

// Estadísticas
int16_t avgRSSI = 0;
float avgSNR = 0;
int rssiCount = 0;
uint16_t duplicatePackets = 0;

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
  STATE_DOWNLOADING
};

TransferState currentState = STATE_IDLE;
String currentFilename = "";
File currentFile;
uint32_t expectedFileSize = 0;
uint32_t transferredBytes = 0;

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
void startDownload(String filename);
void sendFileInChunks(String filename);

// LoRa - Configuración
void setLoRaConfig(String jsonStr);
void sendCurrentLoRaConfig();

// LoRa - Recepción
void processLoRaPacket();
void handleMetadata(uint8_t* data, size_t len);
void handleDataFragment(uint8_t* data, size_t len);
void sendACK(uint16_t fragmentIndex);
void completeReception();
void cancelReception(String reason);

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
  packetReceived = true;
}

// ════════════════════════════════════════════════════════════════════════
// 🚀 SETUP
// ════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n");
  Serial.println("════════════════════════════════════════════════");
  Serial.println("  📡 File Transfer System v3.0 RX");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("  MODO: RECEPTOR");
  Serial.println("════════════════════════════════════════════════");
  Serial.println();
  
  setupLittleFS();
  setupBLE();
  setupLoRa();
  
  Serial.println("\n✅ Sistema RX listo");
  Serial.println("👂 Esperando conexión BLE...");
  Serial.println("📡 Radio LoRa en modo escucha continua\n");
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
  
  // Procesar paquetes LoRa recibidos
  if (packetReceived) {
    packetReceived = false;
    processLoRaPacket();
  }
  
  // Timeout de recepción
  if (receivingFile) {
    if (millis() - lastPacketTime > RX_TIMEOUT) {
      Serial.println("\n⏱️  Timeout de recepción");
      cancelReception("TIMEOUT");
    }
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
  
  // Configurar interrupción
  radio.setDio1Action(setFlag);
  
  // Iniciar recepción continua
  state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("📻 Radio en modo RX continuo");
  } else {
    Serial.printf("❌ Error iniciando RX: %d\n", state);
  }
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
  
  Serial.println("📻 Configuración LoRa RX:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   Power: %d dBm\n", currentPower);
  Serial.printf("   ACK cada: %d fragmentos\n", currentACKInterval);
  Serial.println("✅ Configuración aplicada\n");
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
  
  // Comando: DOWNLOAD:filename
  else if (command.startsWith("CMD:DOWNLOAD:")) {
    String filename = command.substring(13);
    Serial.println("📥 Procesando: DOWNLOAD - " + filename);
    startDownload(filename);
  }
  
  // Comando: SET_LORA_CONFIG
  else if (command.startsWith("CMD:SET_LORA_CONFIG:")) {
    String jsonStr = command.substring(20);
    Serial.println("⚙️  Procesando: SET_LORA_CONFIG");
    setLoRaConfig(jsonStr);
  }
  
  // Comando: GET_LORA_CONFIG
  else if (command == "CMD:GET_LORA_CONFIG") {
    Serial.println("⚙️  Procesando: GET_LORA_CONFIG");
    sendCurrentLoRaConfig();
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
  
  if (receivingFile && receivingFileName == filename) {
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
  
  if (receivingFile) {
    sendResponse("ERROR:RECEIVING");
    return;
  }
  
  currentBW = doc["bw"];
  currentSF = doc["sf"];
  currentCR = doc["cr"];
  currentACKInterval = doc["ack"];
  currentPower = doc["power"];
  
  applyLoRaConfig();
  
  // Reiniciar recepción
  radio.setDio1Action(setFlag);
  radio.startReceive();
  
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
// 📡 LORA RX - PROCESAR PAQUETE
// ════════════════════════════════════════════════════════════════════════

void processLoRaPacket() {
  uint8_t buffer[256];
  
  int state = radio.readData(buffer, sizeof(buffer));
  
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Error leyendo paquete: %d\n", state);
    radio.startReceive();
    return;
  }
  
  size_t len = radio.getPacketLength();
  
  // Obtener RSSI y SNR
  int16_t rssi = radio.getRSSI();
  float snr = radio.getSNR();
  
  // Actualizar estadísticas
  avgRSSI = (avgRSSI * rssiCount + rssi) / (rssiCount + 1);
  avgSNR = (avgSNR * rssiCount + snr) / (rssiCount + 1);
  rssiCount++;
  
  lastPacketTime = millis();
  
  // Detectar tipo de paquete
  
  // METADATA (magic bytes 0x4C, 0x4D)
  if (len >= 7 && buffer[0] == METADATA_MAGIC_1 && buffer[1] == METADATA_MAGIC_2) {
    handleMetadata(buffer, len);
  }
  // FRAGMENTO DE DATOS (índice + total + datos)
  else if (len > 4) {
    handleDataFragment(buffer, len);
  }
  
  // Reiniciar recepción
  radio.startReceive();
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA RX - MANEJAR METADATA
// ════════════════════════════════════════════════════════════════════════

void handleMetadata(uint8_t* data, size_t len) {
  Serial.println("\n📦 METADATA recibido");
  
  // Si ya estaba recibiendo, cancelar recepción anterior
  if (receivingFile) {
    Serial.println("⚠️  Cancelando recepción anterior");
    cancelReception("NEW_TRANSFER");
  }
  
  // Extraer información
  uint32_t fileSize;
  memcpy(&fileSize, data + 2, 4);
  
  uint8_t nameLen = data[6];
  if (nameLen > 100) nameLen = 100;
  
  char fileName[101];
  memcpy(fileName, data + 7, nameLen);
  fileName[nameLen] = '\0';
  
  Serial.printf("📄 Archivo: %s\n", fileName);
  Serial.printf("📊 Tamaño: %u bytes\n", fileSize);
  Serial.printf("📶 RSSI: %d dBm, SNR: %.2f dB\n", radio.getRSSI(), radio.getSNR());
  
  // Verificar espacio disponible
  uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (fileSize > freeSpace) {
    Serial.printf("❌ Espacio insuficiente (libre: %u, necesario: %u)\n", 
                 freeSpace, fileSize);
    sendResponse("RX_FAILED:NO_SPACE");
    return;
  }
  
  // Preparar recepción
  receivingFileName = "/" + String(fileName);
  receivingFileSize = fileSize;
  receivedBytes = 0;
  receivedFragments = 0;
  lastFragmentIndex = 0;
  duplicatePackets = 0;
  rssiCount = 0;
  avgRSSI = 0;
  avgSNR = 0;
  
  expectedFragments = (fileSize + CHUNK_SIZE_LORA - 1) / CHUNK_SIZE_LORA;
  
  // Eliminar archivo si existe
  if (LittleFS.exists(receivingFileName)) {
    LittleFS.remove(receivingFileName);
  }
  
  // Crear archivo
  receivingFileHandle = LittleFS.open(receivingFileName, "w");
  
  if (!receivingFileHandle) {
    Serial.println("❌ Error creando archivo");
    sendResponse("RX_FAILED:CANT_CREATE");
    return;
  }
  
  receivingFile = true;
  receptionStartTime = millis();
  lastPacketTime = millis();
  
  Serial.printf("✅ Listo para recibir %u fragmentos\n", expectedFragments);
  
  sendResponse("RX_START:" + String(fileName) + ":" + String(fileSize));
  sendProgress(0);
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA RX - MANEJAR FRAGMENTO DE DATOS
// ════════════════════════════════════════════════════════════════════════

void handleDataFragment(uint8_t* data, size_t len) {
  if (!receivingFile) return;
  
  // Extraer índice y total
  uint16_t fragmentIndex;
  uint16_t totalFragments;
  
  memcpy(&fragmentIndex, data, 2);
  memcpy(&totalFragments, data + 2, 2);
  
  // Validar
  if (totalFragments != expectedFragments) {
    Serial.printf("⚠️  Total fragmentos no coincide: esperado %u, recibido %u\n", 
                 expectedFragments, totalFragments);
    return;
  }
  
  // Detectar duplicado
  if (fragmentIndex < lastFragmentIndex) {
    duplicatePackets++;
    Serial.printf("⚠️  Fragmento duplicado: %u (ignorando)\n", fragmentIndex);
    return;
  }
  
  // Escribir datos
  size_t dataLen = len - 4;
  size_t written = receivingFileHandle.write(data + 4, dataLen);
  
  if (written != dataLen) {
    Serial.println("❌ Error escribiendo fragmento");
    cancelReception("WRITE_ERROR");
    return;
  }
  
  receivedBytes += written;
  receivedFragments++;
  lastFragmentIndex = fragmentIndex;
  
  // Calcular progreso
  uint8_t progress = (receivedBytes * 100) / receivingFileSize;
  
  // Log cada 10 fragmentos
  if (receivedFragments % 10 == 0 || receivedFragments >= expectedFragments) {
    Serial.printf("📦 Fragmento %u/%u (%.1f%%) - %u bytes\n", 
                 receivedFragments, expectedFragments, progress, receivedBytes);
    
    sendProgress(progress);
    
    String status = "RX_STATUS:" + String(receivedFragments) + "/" + 
                   String(expectedFragments) + ":" + String(receivedBytes);
    sendResponse(status);
  }
  
  // Enviar ACK si es necesario
  bool isLast = (fragmentIndex + 1 == expectedFragments);
  bool needACK = ((fragmentIndex + 1) % currentACKInterval == 0);
  
  if (needACK || isLast) {
    sendACK(fragmentIndex + 1);
  }
  
  // ¿Recepción completa?
  if (receivedFragments >= expectedFragments) {
    completeReception();
  }
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA RX - ENVIAR ACK
// ════════════════════════════════════════════════════════════════════════

void sendACK(uint16_t fragmentIndex) {
  uint8_t ackPacket[5];
  ackPacket[0] = 'A';
  ackPacket[1] = 'C';
  ackPacket[2] = 'K';
  memcpy(ackPacket + 3, &fragmentIndex, 2);
  
  radio.standby();
  int state = radio.transmit(ackPacket, 5);
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("✅ ACK %u enviado\n", fragmentIndex);
  } else {
    Serial.printf("⚠️  Error enviando ACK: %d\n", state);
  }
  
  radio.startReceive();
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA RX - COMPLETAR RECEPCIÓN
// ════════════════════════════════════════════════════════════════════════

void completeReception() {
  receivingFileHandle.flush();
  receivingFileHandle.close();
  
  float receptionTime = (millis() - receptionStartTime) / 1000.0;
  float speed = (receivingFileSize * 8.0) / (receptionTime * 1000.0);
  
  // Verificar tamaño
  File checkFile = LittleFS.open(receivingFileName, "r");
  size_t actualSize = checkFile ? checkFile.size() : 0;
  if (checkFile) checkFile.close();
  
  Serial.println("\n✅ Recepción LoRa completada");
  Serial.printf("📄 Archivo: %s\n", receivingFileName.c_str());
  Serial.printf("📊 Tamaño esperado: %u bytes\n", receivingFileSize);
  Serial.printf("📊 Tamaño real: %u bytes\n", actualSize);
  Serial.printf("⏱️  Tiempo: %.2f s\n", receptionTime);
  Serial.printf("⚡ Velocidad: %.2f kbps\n", speed);
  Serial.printf("📦 Fragmentos: %u/%u\n", receivedFragments, expectedFragments);
  Serial.printf("🔄 Duplicados: %u\n", duplicatePackets);
  Serial.printf("📶 RSSI promedio: %d dBm\n", avgRSSI);
  Serial.printf("📶 SNR promedio: %.2f dB\n", avgSNR);
  
  String cleanName = receivingFileName;
  if (cleanName.startsWith("/")) cleanName = cleanName.substring(1);
  
  if (actualSize == receivingFileSize) {
    String status = "RX_COMPLETE:" + cleanName + ":" + 
                   String(actualSize) + ":" + String(receptionTime, 2);
    sendResponse(status);
    sendProgress(100);
  } else {
    Serial.println("⚠️  Advertencia: Tamaño no coincide");
    sendResponse("RX_COMPLETE:SIZE_MISMATCH:" + cleanName);
  }
  
  // Resetear estado
  receivingFile = false;
  receivingFileName = "";
  receivingFileSize = 0;
  receivedBytes = 0;
  receivedFragments = 0;
  lastFragmentIndex = 0;
  
  Serial.println("👂 Esperando nueva transmisión...\n");
}

// ════════════════════════════════════════════════════════════════════════
// 📡 LORA RX - CANCELAR RECEPCIÓN
// ════════════════════════════════════════════════════════════════════════

void cancelReception(String reason) {
  Serial.println("❌ Cancelando recepción: " + reason);
  
  if (receivingFileHandle) {
    receivingFileHandle.close();
  }
  
  if (LittleFS.exists(receivingFileName)) {
    LittleFS.remove(receivingFileName);
    Serial.println("🗑️  Archivo incompleto eliminado");
  }
  
  sendResponse("RX_FAILED:" + reason);
  sendProgress(0);
  
  receivingFile = false;
  receivingFileName = "";
  receivingFileSize = 0;
  receivedBytes = 0;
  receivedFragments = 0;
  lastFragmentIndex = 0;
  
  Serial.println("👂 Esperando nueva transmisión...\n");
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
}