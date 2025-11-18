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

#define MAX_PACKET_SIZE 250
#define RX_TIMEOUT 30000

// ✅ BUFFER CIRCULAR
#define BUFFER_SIZE 20  // Hasta 20 paquetes en cola

#define METADATA_MAGIC_1 0x4C
#define METADATA_MAGIC_2 0x4D
#define CMD_HANDSHAKE_REQUEST 0xA1
#define CMD_HANDSHAKE_RESPONSE 0xA2

const char* ssid = "LoRa-Gateway-Broadcast";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool receivedFlag = false;

// ✅ Estructura de paquete en buffer
struct PacketBuffer {
  uint8_t data[MAX_PACKET_SIZE];
  size_t length;
  float rssi;
  float snr;
  unsigned long timestamp;
};

// ✅ Cola circular (FIFO)
PacketBuffer packetQueue[BUFFER_SIZE];
volatile int queueHead = 0;  // Escritura (ISR)
volatile int queueTail = 0;  // Lectura (Task)
volatile int queueCount = 0;

SemaphoreHandle_t queueMutex;

String currentFileName = "";
uint32_t expectedFileSize = 0;
bool receivingFile = false;
uint32_t receivedBytes = 0;
uint16_t lastReceivedIndex = 0xFFFF;
uint16_t expectedTotalChunks = 0;

float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;

unsigned long receptionStartTime = 0;
unsigned long receptionEndTime = 0;
float lastReceptionTime = 0;
float lastSpeed = 0;
uint32_t lastFileSize = 0;
unsigned long lastPacketTime = 0;

uint16_t duplicateCount = 0;
uint16_t droppedPackets = 0;  // ✅ Contador de paquetes perdidos
uint16_t processedPackets = 0;

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

// ✅ Agregar paquete al buffer (llamado desde ISR/loop)
bool enqueuePacket(uint8_t* data, size_t len, float rssi, float snr) {
  if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (queueCount < BUFFER_SIZE) {
      memcpy(packetQueue[queueHead].data, data, len);
      packetQueue[queueHead].length = len;
      packetQueue[queueHead].rssi = rssi;
      packetQueue[queueHead].snr = snr;
      packetQueue[queueHead].timestamp = millis();
      
      queueHead = (queueHead + 1) % BUFFER_SIZE;
      queueCount++;
      
      xSemaphoreGive(queueMutex);
      return true;
    } else {
      xSemaphoreGive(queueMutex);
      droppedPackets++;
      Serial.println("⚠️  BUFFER LLENO - Paquete descartado");
      return false;
    }
  }
  return false;
}

// ✅ Extraer paquete del buffer (llamado desde task)
bool dequeuePacket(PacketBuffer* packet) {
  if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (queueCount > 0) {
      memcpy(packet, &packetQueue[queueTail], sizeof(PacketBuffer));
      queueTail = (queueTail + 1) % BUFFER_SIZE;
      queueCount--;
      
      xSemaphoreGive(queueMutex);
      return true;
    }
    xSemaphoreGive(queueMutex);
  }
  return false;
}

// ✅ TASK DE PROCESAMIENTO (FreeRTOS)
void processingTask(void* parameter) {
  PacketBuffer packet;
  
  while (true) {
    if (dequeuePacket(&packet)) {
      processedPackets++;
      
      // Handshake
      if (packet.length == 3 && 
          packet.data[0] == CMD_HANDSHAKE_REQUEST &&
          packet.data[1] == 0xBE &&
          packet.data[2] == 0xEF) {
        handleHandshakeRequest(packet.rssi, packet.snr);
      }
      // Metadatos
      else if (packet.length >= 8 && 
               packet.data[0] == METADATA_MAGIC_1 && 
               packet.data[1] == METADATA_MAGIC_2) {
        if (!receivingFile) {
          processMetadata(packet.data, packet.length);
        }
      }
      // Datos
      else if (packet.length >= 4) {
        processPacket(packet.data, packet.length);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // Esperar 5ms si no hay paquetes
    }
  }
}

String getContentType(String filename) {
  if (filename.endsWith(".pdf")) return "application/pdf";
  else if (filename.endsWith(".txt")) return "text/plain";
  else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  else if (filename.endsWith(".png")) return "image/png";
  else return "application/octet-stream";
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa OPTIMIZADO v9 ===");
  Serial.println("📡 Buffer circular + FreeRTOS");
  Serial.println("🔄 Procesamiento asíncrono\n");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");

  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP: %s\n", IP.toString().c_str());

  setupWebServer();

  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error radio: %d\n", state);
    while (true) delay(1000);
  }
  
  applyLoRaConfig();
  radio.setDio1Action(setFlag);
  radio.startReceive();

  // ✅ Crear mutex y task
  queueMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(
    processingTask,   // Función
    "PacketProcessor", // Nombre
    8192,             // Stack size
    NULL,             // Parámetros
    2,                // Prioridad (alta)
    NULL,             // Handle
    0                 // Core 0
  );

  Serial.println("✅ Sistema listo");
  Serial.printf("🌐 http://%s\n\n", IP.toString().c_str());
}

void applyLoRaConfig() {
  radio.standby();
  delay(100);
  
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  delay(100);
  
  Serial.printf("📻 BW:%.0f SF:%d CR:4/%d\n", currentBW, currentSF, currentCR);
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'><title>LoRa RX v9</title>";
    html += "<style>body{font-family:sans-serif;padding:20px;background:#f0f0f0;}";
    html += ".stat{background:white;padding:15px;margin:10px;border-radius:8px;display:inline-block;}";
    html += ".stat-value{font-size:2em;color:#667eea;font-weight:bold;}";
    html += ".stat-label{color:#666;font-size:0.9em;}</style></head><body>";
    html += "<h1>🛰️ LoRa Gateway v9</h1>";
    
    html += "<div class='stat'><div class='stat-value'>" + String(queueCount) + "</div><div class='stat-label'>En cola</div></div>";
    html += "<div class='stat'><div class='stat-value'>" + String(processedPackets) + "</div><div class='stat-label'>Procesados</div></div>";
    html += "<div class='stat'><div class='stat-value'>" + String(droppedPackets) + "</div><div class='stat-label'>Perdidos</div></div>";
    html += "<div class='stat'><div class='stat-value'>" + String(duplicateCount) + "</div><div class='stat-label'>Duplicados</div></div>";
    
    if (lastReceptionTime > 0) {
      html += "<div class='stat'><div class='stat-value'>" + String(lastSpeed, 2) + "</div><div class='stat-label'>kbps</div></div>";
    }
    
    html += "<h2>📁 Archivos</h2><ul>";
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String displayName = String(file.name());
        if (displayName.startsWith("/")) displayName = displayName.substring(1);
        html += "<li>📄 " + displayName + " (" + String(file.size()) + " bytes)</li>";
      }
      file = root.openNextFile();
    }
    html += "</ul></body></html>";
    
    request->send(200, "text/html", html);
  });

  server.begin();
}

void loop() {
  if (receivingFile && (millis() - lastPacketTime) > RX_TIMEOUT) {
    Serial.println("\n⚠️  TIMEOUT");
    receivingFile = false;
  }
  
  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();
      float rssi = radio.getRSSI();
      float snr = radio.getSNR();
      
      lastPacketTime = millis();
      
      // ✅ AGREGAR A COLA (no procesar aquí)
      if (!enqueuePacket(buffer, packetLen, rssi, snr)) {
        Serial.printf("⚠️  Cola: %d/%d\n", queueCount, BUFFER_SIZE);
      }
    }

    radio.startReceive();
  }
  
  yield();
  delay(5);
}

void handleHandshakeRequest(float rssi, float snr) {
  Serial.printf("\n🤝 Handshake (RSSI:%.1f SNR:%.1f)\n", rssi, snr);
  
  uint8_t handshakeResp[3] = {CMD_HANDSHAKE_RESPONSE, 0xBE, 0xEF};
  radio.standby();
  delay(100);
  radio.transmit(handshakeResp, 3);
  delay(50);
  radio.startReceive();
  
  Serial.println("   ✅ Respuesta enviada\n");
}

void processMetadata(uint8_t* data, size_t len) {
  if (len < 8) return;
  
  receptionStartTime = millis();
  duplicateCount = 0;
  
  memcpy(&expectedFileSize, data + 2, 4);
  if (expectedFileSize == 0 || expectedFileSize > 10485760) return;
  
  uint8_t nameLen = data[6];
  if (nameLen == 0 || nameLen > 100 || len < (7 + nameLen)) return;
  
  char nameBuf[101];
  memcpy(nameBuf, data + 7, nameLen);
  nameBuf[nameLen] = '\0';
  
  currentFileName = String(nameBuf);
  if (!currentFileName.startsWith("/")) currentFileName = "/" + currentFileName;
  
  Serial.printf("\n📋 META: %s (%u bytes)\n", currentFileName.c_str(), expectedFileSize);
  
  if (LittleFS.exists(currentFileName)) LittleFS.remove(currentFileName);
  
  receivingFile = true;
  receivedBytes = 0;
  lastReceivedIndex = 0xFFFF;
  expectedTotalChunks = 0;
}

void processPacket(uint8_t* data, size_t len) {
  if (!receivingFile) return;
  
  uint16_t index, total;
  memcpy(&index, data, 2);
  memcpy(&total, data + 2, 2);

  if (index >= 1000 || total == 0) return;
  
  if (expectedTotalChunks == 0) {
    expectedTotalChunks = total;
  }

  int dataLen = len - 4;

  // Duplicado
  if (index == lastReceivedIndex) {
    duplicateCount++;
    return;
  }
  
  Serial.printf("📦 [%u/%u] %d bytes\n", index + 1, total, dataLen);
  lastReceivedIndex = index;

  const char* mode = (index == 0) ? "w" : "a";
  File file = LittleFS.open(currentFileName, mode);
  if (!file) return;
  
  file.write(data + 4, dataLen);
  file.close();

  receivedBytes += dataLen;

  if ((index + 1) == total) {
    receptionEndTime = millis();
    lastReceptionTime = (receptionEndTime - receptionStartTime) / 1000.0;
    showReceivedFile();
    receivingFile = false;
  }
}

void showReceivedFile() {
  Serial.println("\n🎉 COMPLETO\n");
  
  File recibido = LittleFS.open(currentFileName, "r");
  if (!recibido) return;
  
  lastFileSize = recibido.size();
  lastSpeed = (lastFileSize * 8.0) / (lastReceptionTime * 1000.0);
  
  Serial.printf("📁 %s (%u bytes)\n", currentFileName.c_str(), lastFileSize);
  Serial.printf("⏱️  %.2f s\n", lastReceptionTime);
  Serial.printf("⚡ %.2f kbps\n", lastSpeed);
  Serial.printf("🔄 Duplicados: %u\n", duplicateCount);
  Serial.printf("📉 Perdidos: %u\n", droppedPackets);
  
  recibido.close();
}
