#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <BluetoothSerial.h>
#include <ArduinoJson.h>

// Pines Heltec WiFi LoRa 32 V3
#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14

// Configuración optimizada
#define CHUNK_SIZE 240
#define ACK_TIMEOUT 1200
#define MAX_RETRIES 3
#define METADATA_MAGIC_1 0x4C
#define METADATA_MAGIC_2 0x4D

// Bluetooth Serial
BluetoothSerial SerialBT;

// Radio LoRa
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Variables de transmisión
volatile bool ackReceived = false;
volatile bool transmitting = false;
volatile bool receivingACK = false;
String currentFile = "";

// Parámetros LoRa configurables
float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;
int currentACK = 5;

// Estadísticas
unsigned long transmissionStartTime = 0;
unsigned long transmissionEndTime = 0;
float lastTransmissionTime = 0;
uint32_t lastFileSize = 0;
float lastSpeed = 0;
uint16_t totalPacketsSent = 0;
uint16_t totalRetries = 0;

// Buffer para recepción de archivos por BT
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

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== TRANSMISOR LoRa BLUETOOTH v1.0 ===");

  // Inicializar LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");
  listFiles();

  // Inicializar Bluetooth Serial
  Serial.println("\n📱 Iniciando Bluetooth...");
  if (!SerialBT.begin("LoRa-TX")) {
    Serial.println("❌ Error iniciando Bluetooth");
    while(1) delay(1000);
  }
  Serial.println("✅ Bluetooth iniciado: LoRa-TX");
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
  Serial.println("👂 Esperando comandos Bluetooth...\n");
}

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando configuración LoRa...");
  
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
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   ACK cada: %d fragmentos\n", currentACK);
  Serial.println("✅ Configuración aplicada\n");
}

void loop() {
  // Manejar comandos Bluetooth
  handleBluetoothCommands();
  
  // Procesar transmisión LoRa si está activa
  if (transmitting) {
    processLoRaTransmission();
  }
  
  yield();
  delay(10);
}

// ==================== COMANDOS BLUETOOTH ====================

void handleBluetoothCommands() {
  if (!SerialBT.available()) return;
  
  String command = SerialBT.readStringUntil('\n');
  command.trim();
  
  Serial.println("📱 Comando BT: " + command);
  
  // GET_CONFIG - Obtener configuración actual
  if (command == "GET_CONFIG") {
    sendCurrentConfig();
  }
  
  // SET_CONFIG:{json} - Configurar parámetros LoRa
  else if (command.startsWith("SET_CONFIG:")) {
    String jsonStr = command.substring(11);
    setConfigFromJSON(jsonStr);
  }
  
  // GET_FILES - Listar archivos
  else if (command == "GET_FILES") {
    sendFileList();
  }
  
  // UPLOAD_FILE:{json} - Iniciar subida de archivo
  else if (command.startsWith("UPLOAD_FILE:")) {
    String jsonStr = command.substring(12);
    startFileUpload(jsonStr);
  }
  
  // DELETE_FILE:filename - Eliminar archivo
  else if (command.startsWith("DELETE_FILE:")) {
    String filename = command.substring(12);
    deleteFile(filename);
  }
  
  // SEND_LORA:filename - Transmitir por LoRa
  else if (command.startsWith("SEND_LORA:")) {
    String filename = command.substring(10);
    startLoRaTransmission(filename);
  }
  
  // GET_STATUS - Estado del sistema
  else if (command == "GET_STATUS") {
    sendStatus();
  }
  
  else {
    SerialBT.println("ERROR:UNKNOWN_COMMAND");
  }
}

// Enviar configuración actual
void sendCurrentConfig() {
  StaticJsonDocument<200> doc;
  doc["bw"] = (int)currentBW;
  doc["sf"] = currentSF;
  doc["cr"] = currentCR;
  doc["ack"] = currentACK;
  
  String json;
  serializeJson(doc, json);
  
  SerialBT.println(json);
  Serial.println("✅ Config enviada: " + json);
}

// Configurar desde JSON
void setConfigFromJSON(String jsonStr) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    SerialBT.println("ERROR:INVALID_JSON");
    Serial.println("❌ JSON inválido");
    return;
  }
  
  if (transmitting) {
    SerialBT.println("ERROR:TRANSMITTING");
    Serial.println("⚠️  No cambiar config durante TX");
    return;
  }
  
  currentBW = doc["bw"];
  currentSF = doc["sf"];
  currentCR = doc["cr"];
  currentACK = doc["ack"];
  
  applyLoRaConfig();
  
  SerialBT.println("OK");
  Serial.println("✅ Configuración actualizada");
}

// Enviar lista de archivos
void sendFileList() {
  SerialBT.println("[FILES_START]");
  
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  
  while (file) {
    if (!file.isDirectory()) {
      String filename = String(file.name());
      if (filename.startsWith("/")) filename = filename.substring(1);
      
      SerialBT.printf("%s,%d\n", filename.c_str(), file.size());
    }
    file = root.openNextFile();
  }
  
  SerialBT.println("[FILES_END]");
  Serial.println("✅ Lista de archivos enviada");
}

// Iniciar subida de archivo
void startFileUpload(String jsonStr) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    SerialBT.println("ERROR:INVALID_JSON");
    return;
  }
  
  uploadingFileName = doc["filename"].as<String>();
  uploadingFileSize = doc["size"];
  
  if (!uploadingFileName.startsWith("/")) {
    uploadingFileName = "/" + uploadingFileName;
  }
  
  // Eliminar si existe
  if (LittleFS.exists(uploadingFileName)) {
    LittleFS.remove(uploadingFileName);
  }
  
  // Abrir archivo para escritura
  uploadingFile = LittleFS.open(uploadingFileName, "w");
  
  if (!uploadingFile) {
    SerialBT.println("ERROR:CANT_CREATE_FILE");
    Serial.println("❌ No se pudo crear archivo");
    return;
  }
  
  receivingFile = true;
  receivedFileBytes = 0;
  
  SerialBT.println("OK");
  Serial.printf("📥 Listo para recibir: %s (%u bytes)\n", 
                uploadingFileName.c_str(), uploadingFileSize);
}

// Recibir chunks de archivo
void receiveFileChunk(uint8_t* data, size_t len) {
  if (!receivingFile || !uploadingFile) return;
  
  // Formato: [C][chunk_num 2bytes][total 2bytes][datos...]
  if (len < 5 || data[0] != 'C') return;
  
  uint16_t chunkNum = (data[1] << 8) | data[2];
  uint16_t totalChunks = (data[3] << 8) | data[4];
  
  size_t dataLen = len - 5;
  uploadingFile.write(data + 5, dataLen);
  receivedFileBytes += dataLen;
  
  Serial.printf("📦 Chunk %u/%u (%u bytes)\n", chunkNum + 1, totalChunks, dataLen);
  
  // Si es el último chunk
  if (chunkNum + 1 == totalChunks) {
    uploadingFile.close();
    receivingFile = false;
    
    SerialBT.println("UPLOAD_COMPLETE");
    Serial.printf("✅ Archivo recibido: %s (%u bytes)\n", 
                  uploadingFileName.c_str(), receivedFileBytes);
    
    listFiles();
  }
}

// Eliminar archivo
void deleteFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (LittleFS.remove(filename)) {
    SerialBT.println("OK");
    Serial.println("🗑️  Archivo eliminado: " + filename);
  } else {
    SerialBT.println("ERROR:CANT_DELETE");
    Serial.println("❌ Error eliminando archivo");
  }
}

// Iniciar transmisión LoRa
void startLoRaTransmission(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    SerialBT.println("ERROR:FILE_NOT_FOUND");
    Serial.println("❌ Archivo no existe");
    return;
  }
  
  if (transmitting) {
    SerialBT.println("ERROR:ALREADY_TRANSMITTING");
    Serial.println("⚠️  Ya transmitiendo");
    return;
  }
  
  currentFile = filename;
  transmitting = true;
  totalPacketsSent = 0;
  totalRetries = 0;
  
  SerialBT.println("OK");
  Serial.println("📡 Iniciando transmisión LoRa...");
}

// Enviar estado del sistema
void sendStatus() {
  StaticJsonDocument<300> doc;
  doc["transmitting"] = transmitting;
  doc["currentFile"] = currentFile;
  doc["connected"] = SerialBT.hasClient();
  doc["freeSpace"] = LittleFS.totalBytes() - LittleFS.usedBytes();
  doc["totalSpace"] = LittleFS.totalBytes();
  
  String json;
  serializeJson(doc, json);
  
  SerialBT.println(json);
}

// ==================== TRANSMISIÓN LoRa ====================

void processLoRaTransmission() {
  Serial.printf("\n📡 Transmitiendo: %s\n", currentFile.c_str());
  transmissionStartTime = millis();
  
  bool result = sendFile(currentFile.c_str());
  
  transmissionEndTime = millis();
  lastTransmissionTime = (transmissionEndTime - transmissionStartTime) / 1000.0;
  lastSpeed = (lastFileSize * 8.0) / (lastTransmissionTime * 1000.0);
  
  if (result) {
    String status = "TX_SUCCESS:" + currentFile + "," + 
                   String(lastTransmissionTime, 2) + "," + 
                   String(lastSpeed, 2);
    SerialBT.println(status);
    
    Serial.println("\n✅ Transmisión exitosa");
    Serial.printf("⏱️  Tiempo: %.2f s\n", lastTransmissionTime);
    Serial.printf("⚡ Velocidad: %.2f kbps\n", lastSpeed);
  } else {
    SerialBT.println("TX_FAILED:" + currentFile);
    Serial.println("\n❌ Transmisión fallida");
  }
  
  transmitting = false;
  currentFile = "";
}

bool sendFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("❌ Archivo no existe: %s\n", path);
    return false;
  }

  uint32_t totalSize = f.size();
  lastFileSize = totalSize;
  
  String fileName = String(path);
  if (fileName.startsWith("/")) fileName = fileName.substring(1);
  
  Serial.printf("📁 %s\n", fileName.c_str());
  Serial.printf("📊 %u bytes\n", totalSize);
  Serial.printf("📻 BW=%.0f, SF=%d, CR=4/%d, ACK=%d\n", 
                currentBW, currentSF, currentCR, currentACK);
  
  // METADATOS
  uint8_t nameLen = min((size_t)fileName.length(), (size_t)100);
  uint8_t metaPkt[2 + 4 + 1 + nameLen];
  
  metaPkt[0] = METADATA_MAGIC_1;
  metaPkt[1] = METADATA_MAGIC_2;
  memcpy(metaPkt + 2, &totalSize, 4);
  metaPkt[6] = nameLen;
  memcpy(metaPkt + 7, fileName.c_str(), nameLen);
  
  Serial.println("\n📤 Enviando metadatos...");
  int metaState = radio.transmit(metaPkt, 7 + nameLen);
  
  if (metaState != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error metadatos: %d\n", metaState);
    f.close();
    return false;
  }
  
  Serial.println("✅ Metadatos OK");
  delay(600);
  
  // DATOS
  uint16_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  Serial.printf("📦 %u fragmentos\n\n", totalChunks);

  int dynamicACKTimeout = getACKTimeout();
  int dynamicDelay = getInterPacketDelay();

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

      Serial.printf("📤 [%u/%u] %d bytes", index + 1, totalChunks, bytesRead);
      if (retries > 0) Serial.printf(" (retry %d)", retries);
      Serial.println();

      ackReceived = false;
      receivingACK = false;
      
      int state = radio.transmit(pkt, 4 + bytesRead);
      
      if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("   ❌ TX error: %d\n", state);
        retries++;
        totalRetries++;
        delay(500);
        continue;
      }

      Serial.println("   ✅ TX OK");
      totalPacketsSent++;
      
      bool isLastFragment = (index + 1 == totalChunks);
      bool isMultipleOfACK = ((index + 1) % currentACK == 0);
      
      if (isMultipleOfACK || isLastFragment) {
        Serial.println("   ⏳ Esperando ACK...");
        
        delay(200);
        
        receivingACK = true;
        radio.setDio1Action(setFlag);
        
        int rxState = radio.startReceive();
        if (rxState != RADIOLIB_ERR_NONE) {
          Serial.printf("   ❌ RX error: %d\n", rxState);
          receivingACK = false;
          retries++;
          totalRetries++;
          delay(500);
          continue;
        }

        unsigned long startWait = millis();
        bool validAck = false;
        
        while (millis() - startWait < dynamicACKTimeout && !validAck) {
          if (ackReceived) {
            ackReceived = false;
            
            uint8_t ackBuffer[20];
            int recvState = radio.readData(ackBuffer, sizeof(ackBuffer));
            
            if (recvState == RADIOLIB_ERR_NONE) {
              size_t ackLen = radio.getPacketLength();
              
              Serial.printf("   📨 ACK recibido (%d bytes)\n", ackLen);
              
              if (ackLen == 5 && ackBuffer[0] == 'A' && 
                  ackBuffer[1] == 'C' && ackBuffer[2] == 'K') {
                uint16_t ackFragmentNumber;
                memcpy(&ackFragmentNumber, ackBuffer + 3, 2);
                
                if (ackFragmentNumber == index + 1) {
                  Serial.printf("   ✅ ACK válido [%u]\n\n", ackFragmentNumber);
                  validAck = true;
                  success = true;
                } else {
                  Serial.printf("   ⚠️  ACK incorrecto\n");
                }
              }
              
              if (!validAck) radio.startReceive();
            }
          }
          delayMicroseconds(5000);
        }

        receivingACK = false;
        
        if (!success) {
          Serial.println("   ❌ Timeout ACK");
          retries++;
          totalRetries++;
          delay(800);
        }
      } else {
        Serial.printf("   ⏭️  Sin ACK\n\n");
        success = true;
      }
    }

    if (!success) {
      Serial.printf("\n❌ Fragmento %u falló\n", index + 1);
      f.close();
      return false;
    }

    delay(dynamicDelay);
  }

  f.close();
  Serial.println("\n🎉 Transmisión completa!");
  Serial.printf("📊 %u bytes en %u fragmentos\n", totalSize, totalChunks);
  Serial.printf("📈 Reintentos: %u\n", totalRetries);
  return true;
}

// ==================== UTILIDADES ====================

void listFiles() {
  Serial.println("\n📁 Archivos en LittleFS:");
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