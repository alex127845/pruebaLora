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

#define MAX_PACKET_SIZE 250
#define RX_TIMEOUT 30000

// MAGIC BYTES para validación de metadatos
#define METADATA_MAGIC_1 0x4C  // 'L'
#define METADATA_MAGIC_2 0x4D  // 'M'

// Bluetooth Serial
BluetoothSerial SerialBT;

// Radio LoRa
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Flags
volatile bool receivedFlag = false;
volatile bool transmittingACK = false;

// Variables para archivo actual recibido por LoRa
String currentFileName = "";
uint32_t expectedFileSize = 0;
bool receivingFile = false;
uint32_t receivedBytes = 0;
uint16_t lastReceivedIndex = 0xFFFF;
uint16_t expectedTotalChunks = 0;

// Parámetros LoRa configurables
float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;
int currentACK = 5;

// Estadísticas
unsigned long receptionStartTime = 0;
unsigned long receptionEndTime = 0;
float lastReceptionTime = 0;
float lastSpeed = 0;
uint32_t lastFileSize = 0;
unsigned long lastPacketTime = 0;

void IRAM_ATTR setFlag(void) {
  if (!transmittingACK) {
    receivedFlag = true;
  }
}

int getACKDelay() {
  if (currentBW >= 500.0 && currentSF <= 9) return 150;
  if (currentBW >= 250.0 && currentSF <= 9) return 180;
  return 200;
}

int getProcessingDelay() {
  if (currentBW >= 500.0 && currentSF <= 7) return 10;
  if (currentBW >= 250.0 && currentSF <= 9) return 15;
  return 20;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa BLUETOOTH v1.0 ===");

  // Inicializar LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");
  listFiles();

  // Inicializar Bluetooth Serial
  Serial.println("\n📱 Iniciando Bluetooth...");
  if (!SerialBT.begin("LoRa-RX")) {
    Serial.println("❌ Error iniciando Bluetooth");
    while(1) delay(1000);
  }
  Serial.println("✅ Bluetooth iniciado: LoRa-RX");
  Serial.println("   Visible para emparejamiento");

  // Inicializar Radio LoRa
  Serial.println("\n📡 Iniciando radio SX1262...");
  int state = radio.begin(915.0);
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

  Serial.println("✅ Radio configurado");
  Serial.println("👂 Escuchando paquetes LoRa...");
  Serial.println("📱 Esperando comandos Bluetooth...\n");
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
  
  // Timeout de recepción LoRa
  if (receivingFile && (millis() - lastPacketTime) > RX_TIMEOUT) {
    Serial.println("\n⚠️  TIMEOUT LoRa");
    Serial.printf("   %u/%u bytes (%.1f%%)\n", 
                  receivedBytes, expectedFileSize, 
                  (receivedBytes * 100.0) / expectedFileSize);
    receivingFile = false;
    
    // Notificar a la app
    SerialBT.println("RX_TIMEOUT");
  }
  
  // Procesar paquetes LoRa recibidos
  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();
      lastPacketTime = millis();
      
      Serial.printf("📡 RX: %d bytes | RSSI: %.1f | SNR: %.1f\n", 
                    packetLen, radio.getRSSI(), radio.getSNR());
      
      if (packetLen >= 8 && buffer[0] == METADATA_MAGIC_1 && buffer[1] == METADATA_MAGIC_2) {
        if (!receivingFile) {
          processMetadata(buffer, packetLen);
        } else {
          Serial.println("⚠️  Metadatos durante RX - IGNORANDO");
        }
      } else if (packetLen >= 4) {
        processPacket(buffer, packetLen);
      }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("❌ CRC Error");
    }

    delay(getProcessingDelay());
    receivedFlag = false;
    radio.startReceive();
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
  
  // GET_FILES - Listar archivos recibidos
  else if (command == "GET_FILES") {
    sendFileList();
  }
  
  // DOWNLOAD_FILE:filename - Enviar archivo por Bluetooth
  else if (command.startsWith("DOWNLOAD_FILE:")) {
    String filename = command.substring(14);
    sendFileViaBluetooth(filename);
  }
  
  // DELETE_FILE:filename - Eliminar archivo
  else if (command.startsWith("DELETE_FILE:")) {
    String filename = command.substring(12);
    deleteFile(filename);
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
  
  if (receivingFile) {
    SerialBT.println("ERROR:RECEIVING");
    Serial.println("⚠️  No cambiar config durante RX");
    return;
  }
  
  currentBW = doc["bw"];
  currentSF = doc["sf"];
  currentCR = doc["cr"];
  currentACK = doc["ack"];
  
  applyLoRaConfig();
  
  // Reconfigurar interrupción y volver a RX
  radio.setDio1Action(setFlag);
  int state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error startReceive: %d\n", state);
    SerialBT.println("ERROR:CANT_START_RX");
    return;
  }
  
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

// Enviar archivo por Bluetooth a la app
void sendFileViaBluetooth(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    SerialBT.println("ERROR:FILE_NOT_FOUND");
    Serial.println("❌ Archivo no existe");
    return;
  }
  
  File file = LittleFS.open(filename, "r");
  if (!file) {
    SerialBT.println("ERROR:CANT_OPEN_FILE");
    Serial.println("❌ No se pudo abrir archivo");
    return;
  }
  
  uint32_t fileSize = file.size();
  String displayName = filename;
  if (displayName.startsWith("/")) displayName = displayName.substring(1);
  
  // Enviar header
  SerialBT.printf("[FILE_START:%s:%u]\n", displayName.c_str(), fileSize);
  Serial.printf("📤 Enviando archivo: %s (%u bytes)\n", displayName.c_str(), fileSize);
  
  // Enviar datos en chunks
  uint8_t buffer[512];
  size_t totalSent = 0;
  
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    SerialBT.write(buffer, bytesRead);
    totalSent += bytesRead;
    
    Serial.printf("   Enviado: %u/%u bytes (%.1f%%)\r", 
                  totalSent, fileSize, (totalSent * 100.0) / fileSize);
    
    delay(10); // Pequeña pausa para no saturar
  }
  
  Serial.println();
  file.close();
  
  // Enviar fin
  SerialBT.println("[FILE_END]");
  Serial.println("✅ Archivo enviado completamente");
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

// Enviar estado del sistema
void sendStatus() {
  StaticJsonDocument<300> doc;
  doc["receivingFile"] = receivingFile;
  doc["currentFile"] = currentFileName;
  doc["progress"] = receivingFile ? (receivedBytes * 100.0) / expectedFileSize : 0;
  doc["connected"] = SerialBT.hasClient();
  doc["freeSpace"] = LittleFS.totalBytes() - LittleFS.usedBytes();
  doc["totalSpace"] = LittleFS.totalBytes();
  
  String json;
  serializeJson(doc, json);
  
  SerialBT.println(json);
}

// ==================== RECEPCIÓN LoRa ====================

void processMetadata(uint8_t* data, size_t len) {
  if (len < 8) return;
  if (data[0] != METADATA_MAGIC_1 || data[1] != METADATA_MAGIC_2) return;
  
  receptionStartTime = millis();
  lastPacketTime = millis();
  
  memcpy(&expectedFileSize, data + 2, 4);
  
  if (expectedFileSize == 0 || expectedFileSize > 10485760) {
    Serial.printf("⚠️  Tamaño inválido: %u\n", expectedFileSize);
    return;
  }
  
  uint8_t nameLen = data[6];
  if (nameLen == 0 || nameLen > 100 || len < (7 + nameLen)) return;
  
  char nameBuf[101];
  memcpy(nameBuf, data + 7, nameLen);
  nameBuf[nameLen] = '\0';
  
  bool validName = true;
  for (int i = 0; i < nameLen; i++) {
    if (nameBuf[i] < 32 || nameBuf[i] > 126) {
      if (nameBuf[i] != '.') {
        validName = false;
        break;
      }
    }
  }
  
  if (!validName) return;
  
  currentFileName = String(nameBuf);
  if (!currentFileName.startsWith("/")) currentFileName = "/" + currentFileName;
  
  Serial.println("\n📋 METADATOS RECIBIDOS:");
  Serial.printf("   📁 %s (%u bytes)\n", currentFileName.c_str(), expectedFileSize);
  Serial.printf("   📻 BW=%.0f, SF=%d, CR=4/%d, ACK=%d\n", currentBW, currentSF, currentCR, currentACK);
  
  // Eliminar si existe
  if (LittleFS.exists(currentFileName)) {
    LittleFS.remove(currentFileName);
  }
  
  receivingFile = true;
  receivedBytes = 0;
  lastReceivedIndex = 0xFFFF;
  expectedTotalChunks = 0;
  
  Serial.println("   ✅ Listo para recibir datos\n");
  
  // Notificar a la app
  String displayName = currentFileName;
  if (displayName.startsWith("/")) displayName = displayName.substring(1);
  SerialBT.printf("RX_START:%s:%u\n", displayName.c_str(), expectedFileSize);
}

void processPacket(uint8_t* data, size_t len) {
  if (!receivingFile || currentFileName == "") {
    Serial.println("⚠️  Datos sin metadatos");
    return;
  }
  
  uint16_t index, total;
  memcpy(&index, data, 2);
  memcpy(&total, data + 2, 2);

  if (index >= 1000 || total == 0 || total >= 1000) return;
  
  if (expectedTotalChunks == 0) {
    expectedTotalChunks = total;
  } else if (expectedTotalChunks != total) {
    Serial.printf("⚠️  Total inconsistente\n");
    return;
  }

  int dataLen = len - 4;
  Serial.printf("📦 [%u/%u] %d bytes\n", index + 1, total, dataLen);

  // Manejar duplicados
  if (index == lastReceivedIndex) {
    Serial.println("   ⚠️  Duplicado");
    bool isMultipleOfACK = ((index + 1) % currentACK == 0);
    bool isLastFragment = (index + 1 == total);
    if (isMultipleOfACK || isLastFragment) {
      delay(getACKDelay());
      sendAck(index);
    }
    return;
  }
  
  lastReceivedIndex = index;

  // Escribir datos al archivo
  const char* mode = (index == 0) ? "w" : "a";
  File file = LittleFS.open(currentFileName, mode);
  if (!file) {
    Serial.println("❌ Error abriendo archivo");
    return;
  }
  
  size_t written = file.write(data + 4, dataLen);
  file.close();

  if (written == dataLen) {
    receivedBytes += dataLen;
    float progress = (receivedBytes * 100.0) / expectedFileSize;
    Serial.printf("✅ OK (%u/%u - %.1f%%)\n", receivedBytes, expectedFileSize, progress);
    
    // Notificar progreso a la app
    SerialBT.printf("RX_PROGRESS:%u:%u\n", receivedBytes, expectedFileSize);
  }

  bool isLastFragment = (index + 1 == total);
  bool isMultipleOfACK = ((index + 1) % currentACK == 0);
  
  // Enviar ACK si corresponde
  if (isMultipleOfACK || isLastFragment) {
    delay(getACKDelay());
    sendAck(index);
  }

  // Si es el último fragmento
  if (isLastFragment) {
    receptionEndTime = millis();
    lastReceptionTime = (receptionEndTime - receptionStartTime) / 1000.0;
    delay(200);
    showReceivedFile();
    receivingFile = false;
    receivedBytes = 0;
    lastReceivedIndex = 0xFFFF;
    expectedTotalChunks = 0;
    
    // Notificar a la app
    SerialBT.printf("RX_COMPLETE:%s:%u:%.2f\n", 
                    currentFileName.c_str(), lastFileSize, lastSpeed);
  }
}

void sendAck(uint16_t index) {
  transmittingACK = true;
  
  uint8_t ackPacket[5] = {'A', 'C', 'K'};
  uint16_t fragmentNumber = index + 1;
  memcpy(ackPacket + 3, &fragmentNumber, 2);
  
  Serial.printf("📤 ACK[%u]... ", fragmentNumber);
  
  radio.standby();
  int state = radio.transmit(ackPacket, sizeof(ackPacket));
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("✅");
  } else {
    Serial.printf("❌ %d\n", state);
  }
  
  delay(50);
  radio.startReceive();
  transmittingACK = false;
  Serial.println();
}

void showReceivedFile() {
  Serial.println("\n🎉 ¡RECEPCIÓN COMPLETA!\n");
  
  File recibido = LittleFS.open(currentFileName, "r");
  if (!recibido) {
    Serial.println("❌ No se pudo abrir archivo");
    return;
  }
  
  lastFileSize = recibido.size();
  lastSpeed = (lastFileSize * 8.0) / (lastReceptionTime * 1000.0);
  
  Serial.printf("📁 %s\n", currentFileName.c_str());
  Serial.printf("📊 Tamaño: %u bytes\n", lastFileSize);
  Serial.printf("⏱️  Tiempo: %.2f s\n", lastReceptionTime);
  Serial.println("╔════════════════════════════════╗");
  Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps      ║\n", lastSpeed);
  Serial.println("╚════════════════════════════════╝");
  
  if (lastFileSize != expectedFileSize) {
    Serial.printf("⚠️  Faltan %d bytes\n", expectedFileSize - lastFileSize);
  } else {
    Serial.println("✅ Integridad OK");
  }
  
  Serial.println();
  recibido.close();
  
  listFiles();
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