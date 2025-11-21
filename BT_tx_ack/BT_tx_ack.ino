#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

// Pines Heltec WiFi LoRa 32 V3
#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14

#define CHUNK_SIZE 240
#define ACK_TIMEOUT 1200
#define MAX_RETRIES 3
#define METADATA_MAGIC_1 0x4C
#define METADATA_MAGIC_2 0x4D

// UUIDs para BLE
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID_RX "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHARACTERISTIC_UUID_TX "beb5483e-36e1-4688-b7f5-ea07361b26a9"

// Radio LoRa
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// BLE
BLEServer* pServer = NULL;
BLECharacteristic* pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;
String rxBuffer = "";

// Variables de transmisión
volatile bool ackReceived = false;
volatile bool transmitting = false;
volatile bool receivingACK = false;
String currentFile = "";

// Parámetros LoRa
float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;
int currentACK = 5;

// Estadísticas
unsigned long transmissionStartTime = 0;
float lastTransmissionTime = 0;
uint32_t lastFileSize = 0;
float lastSpeed = 0;
uint16_t totalPacketsSent = 0;
uint16_t totalRetries = 0;

// Buffer para recepción de archivos por BLE
String uploadingFileName = "";
uint32_t uploadingFileSize = 0;
File uploadingFile;
bool receivingFile = false;
uint32_t receivedFileBytes = 0;

void IRAM_ATTR setFlag(void) {
  if (receivingACK) {
    ackReceived = true;
  }
}

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
  return 1500;
}

// Callbacks BLE
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("📱 Cliente BLE conectado");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("📱 Cliente BLE desconectado");
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      // Obtener datos directamente
      uint8_t* data = pCharacteristic->getData();
      int len = pCharacteristic->getValue().length();

      // Procesar byte por byte
      for (int i = 0; i < len; i++) {
        char c = (char)data[i];
        
        if (c == '\n') {
          // Comando completo recibido
          rxBuffer.trim();
          if (rxBuffer.length() > 0) {
            handleBLECommand(rxBuffer);
          }
          rxBuffer = "";
        } else {
          rxBuffer += c;
        }
      }
    }
};

// Enviar mensaje por BLE
void bleSend(String message) {
  if (deviceConnected && pTxCharacteristic != NULL) {
    pTxCharacteristic->setValue(message.c_str());
    pTxCharacteristic->notify();
    delay(10);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== TRANSMISOR LoRa BLE v1.0 (Heltec V3) ===");

  // Inicializar LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");
  listFiles();

  // Inicializar BLE
  Serial.println("\n📱 Iniciando BLE...");
  BLEDevice::init("LoRa-TX");
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE
  );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  
  Serial.println("✅ BLE iniciado: LoRa-TX");
  Serial.println("   Visible para emparejamiento");

  // Inicializar Radio LoRa
  Serial.println("\n📡 Iniciando radio...");
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    while (true) delay(1000);
  }
  
  applyLoRaConfig();

  Serial.println("✅ Sistema listo");
  Serial.println("👂 Esperando conexión BLE...\n");
}

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando configuración LoRa...");
  
  radio.standby();
  delay(100);
  
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  delay(100);
  
  Serial.println("📻 Configuración LoRa:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   ACK cada: %d fragmentos\n", currentACK);
  Serial.println("✅ Configuración aplicada\n");
}

void loop() {
  // Manejar desconexión BLE
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("📱 Esperando nueva conexión BLE...");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  // Procesar transmisión LoRa
  if (transmitting) {
    processLoRaTransmission();
  }
  
  yield();
  delay(10);
}

// ==================== COMANDOS BLE ====================

void handleBLECommand(String command) {
  Serial.println("📱 Comando BLE: " + command);
  
  if (command == "GET_CONFIG") {
    sendCurrentConfig();
  }
  else if (command.startsWith("SET_CONFIG:")) {
    String jsonStr = command.substring(11);
    setConfigFromJSON(jsonStr);
  }
  else if (command == "GET_FILES") {
    sendFileList();
  }
  else if (command.startsWith("UPLOAD_FILE:")) {
    String jsonStr = command.substring(12);
    startFileUpload(jsonStr);
  }
  else if (command.startsWith("DELETE_FILE:")) {
    String filename = command.substring(12);
    deleteFile(filename);
  }
  else if (command.startsWith("SEND_LORA:")) {
    String filename = command.substring(10);
    startLoRaTransmission(filename);
  }
  else if (command == "GET_STATUS") {
    sendStatus();
  }
  else {
    bleSend("ERROR:UNKNOWN_COMMAND\n");
  }
}

void sendCurrentConfig() {
  StaticJsonDocument<200> doc;
  doc["bw"] = (int)currentBW;
  doc["sf"] = currentSF;
  doc["cr"] = currentCR;
  doc["ack"] = currentACK;
  
  String json;
  serializeJson(doc, json);
  json += "\n";
  
  bleSend(json);
  Serial.println("✅ Config enviada: " + json);
}

void setConfigFromJSON(String jsonStr) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    bleSend("ERROR:INVALID_JSON\n");
    Serial.println("❌ JSON inválido: " + String(error.c_str()));
    return;
  }
  
  if (transmitting) {
    bleSend("ERROR:TRANSMITTING\n");
    Serial.println("⚠️  No cambiar config durante TX");
    return;
  }
  
  currentBW = doc["bw"];
  currentSF = doc["sf"];
  currentCR = doc["cr"];
  currentACK = doc["ack"];
  
  applyLoRaConfig();
  
  bleSend("OK\n");
  Serial.println("✅ Configuración actualizada");
}

void sendFileList() {
  bleSend("[FILES_START]\n");
  
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  
  while (file) {
    if (!file.isDirectory()) {
      String filename = String(file.name());
      if (filename.startsWith("/")) filename = filename.substring(1);
      
      String msg = filename + "," + String(file.size()) + "\n";
      bleSend(msg);
    }
    file = root.openNextFile();
  }
  
  bleSend("[FILES_END]\n");
  Serial.println("✅ Lista enviada");
}

void startFileUpload(String jsonStr) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    bleSend("ERROR:INVALID_JSON\n");
    return;
  }
  
  uploadingFileName = doc["filename"].as<String>();
  uploadingFileSize = doc["size"];
  
  if (!uploadingFileName.startsWith("/")) {
    uploadingFileName = "/" + uploadingFileName;
  }
  
  if (LittleFS.exists(uploadingFileName)) {
    LittleFS.remove(uploadingFileName);
  }
  
  uploadingFile = LittleFS.open(uploadingFileName, "w");
  
  if (!uploadingFile) {
    bleSend("ERROR:CANT_CREATE_FILE\n");
    Serial.println("❌ No se pudo crear archivo");
    return;
  }
  
  receivingFile = true;
  receivedFileBytes = 0;
  
  bleSend("OK\n");
  Serial.printf("📥 Listo para recibir: %s (%u bytes)\n", 
                uploadingFileName.c_str(), uploadingFileSize);
}

void deleteFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (LittleFS.remove(filename)) {
    bleSend("OK\n");
    Serial.println("🗑️  Eliminado: " + filename);
  } else {
    bleSend("ERROR:CANT_DELETE\n");
  }
}

void startLoRaTransmission(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    bleSend("ERROR:FILE_NOT_FOUND\n");
    Serial.println("❌ Archivo no existe");
    return;
  }
  
  if (transmitting) {
    bleSend("ERROR:ALREADY_TRANSMITTING\n");
    Serial.println("⚠️  Ya transmitiendo");
    return;
  }
  
  currentFile = filename;
  transmitting = true;
  totalPacketsSent = 0;
  totalRetries = 0;
  
  bleSend("OK\n");
  Serial.println("📡 Iniciando transmisión LoRa...");
}

void sendStatus() {
  StaticJsonDocument<300> doc;
  doc["transmitting"] = transmitting;
  doc["currentFile"] = currentFile;
  doc["connected"] = deviceConnected;
  doc["freeSpace"] = LittleFS.totalBytes() - LittleFS.usedBytes();
  doc["totalSpace"] = LittleFS.totalBytes();
  
  String json;
  serializeJson(doc, json);
  json += "\n";
  
  bleSend(json);
}

void processLoRaTransmission() {
  Serial.printf("\n📡 Transmitiendo: %s\n", currentFile.c_str());
  transmissionStartTime = millis();
  
  bool result = sendFile(currentFile.c_str());
  
  lastTransmissionTime = (millis() - transmissionStartTime) / 1000.0;
  lastSpeed = (lastFileSize * 8.0) / (lastTransmissionTime * 1000.0);
  
  if (result) {
    String status = "TX_SUCCESS:" + currentFile + "," + 
                   String(lastTransmissionTime, 2) + "," + 
                   String(lastSpeed, 2) + "\n";
    bleSend(status);
    
    Serial.println("\n✅ Éxito");
    Serial.printf("⏱️  %.2f s\n", lastTransmissionTime);
    Serial.printf("⚡ %.2f kbps\n", lastSpeed);
  } else {
    bleSend("TX_FAILED:" + currentFile + "\n");
    Serial.println("\n❌ Falló");
  }
  
  transmitting = false;
  currentFile = "";
}

bool sendFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint32_t totalSize = f.size();
  lastFileSize = totalSize;
  
  String fileName = String(path);
  if (fileName.startsWith("/")) fileName = fileName.substring(1);
  
  Serial.printf("📁 %s (%u bytes)\n", fileName.c_str(), totalSize);
  
  // METADATOS
  uint8_t nameLen = min((size_t)fileName.length(), (size_t)100);
  uint8_t metaPkt[2 + 4 + 1 + nameLen];
  
  metaPkt[0] = METADATA_MAGIC_1;
  metaPkt[1] = METADATA_MAGIC_2;
  memcpy(metaPkt + 2, &totalSize, 4);
  metaPkt[6] = nameLen;
  memcpy(metaPkt + 7, fileName.c_str(), nameLen);
  
  if (radio.transmit(metaPkt, 7 + nameLen) != RADIOLIB_ERR_NONE) {
    f.close();
    return false;
  }
  
  delay(600);
  
  // DATOS
  uint16_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  Serial.printf("📦 %u fragmentos\n", totalChunks);
  
  for (uint16_t index = 0; index < totalChunks; index++) {
    uint8_t buffer[CHUNK_SIZE];
    size_t bytesRead = f.read(buffer, CHUNK_SIZE);
    if (bytesRead == 0) break;

    bool success = false;
    int retries = 0;

    while (!success && retries < MAX_RETRIES) {
      uint8_t pkt[4 + bytesRead];
      memcpy(pkt, &index, 2);
      memcpy(pkt + 2, &totalChunks, 2);
      memcpy(pkt + 4, buffer, bytesRead);

      if (radio.transmit(pkt, 4 + bytesRead) != RADIOLIB_ERR_NONE) {
        retries++;
        totalRetries++;
        delay(500);
        continue;
      }

      totalPacketsSent++;
      
      bool isLast = (index + 1 == totalChunks);
      bool needACK = ((index + 1) % currentACK == 0);
      
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
          totalRetries++;
          delay(800);
        }
      } else {
        success = true;
      }
    }

    if (!success) {
      f.close();
      return false;
    }

    delay(getInterPacketDelay());
  }

  f.close();
  return true;
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
  Serial.println();
}