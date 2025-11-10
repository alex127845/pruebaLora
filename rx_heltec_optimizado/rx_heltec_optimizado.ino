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

#define MAX_PACKET_SIZE 250
#define ACK_DELAY 200
#define RX_TIMEOUT 30000
#define MAX_RETRIES 3

// ✅ MAGIC BYTES para validación de metadatos
#define METADATA_MAGIC_1 0x4C  // 'L'
#define METADATA_MAGIC_2 0x4D  // 'M'

const char* ssid = "LoRa-Gateway";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool receivedFlag = false;
volatile bool transmittingACK = false;

// Variables para archivo actual
String currentFileName = "";
uint32_t expectedFileSize = 0;
bool receivingFile = false;
uint32_t receivedBytes = 0;
uint16_t lastReceivedIndex = 0xFFFF;
uint16_t expectedTotalChunks = 0;  // ✅ Validar total de fragmentos

// Parámetros LoRa configurables
float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;
int currentACK = 5;

// Contador de tiempo
unsigned long receptionStartTime = 0;
unsigned long receptionEndTime = 0;
float lastReceptionTime = 0;
unsigned long lastPacketTime = 0;

void IRAM_ATTR setFlag(void) {
  if (!transmittingACK) {
    receivedFlag = true;
  }
}

String getContentType(String filename) {
  if (filename.endsWith(".pdf")) return "application/pdf";
  else if (filename.endsWith(".txt")) return "text/plain";
  else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".gif")) return "image/gif";
  else if (filename.endsWith(".zip")) return "application/zip";
  else if (filename.endsWith(".doc")) return "application/msword";
  else if (filename.endsWith(".docx")) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  else if (filename.endsWith(".xls")) return "application/vnd.ms-excel";
  else if (filename.endsWith(".xlsx")) return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  else if (filename.endsWith(".mp3")) return "audio/mpeg";
  else if (filename.endsWith(".mp4")) return "video/mp4";
  else return "application/octet-stream";
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa HELTEC OPTIMIZADO v6 ===");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");

  Serial.println("\n📡 Configurando WiFi AP...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP iniciado\n");
  Serial.printf("   SSID: %s\n", ssid);
  Serial.printf("   Password: %s\n", password);
  Serial.printf("   IP: %s\n\n", IP.toString().c_str());

  setupWebServer();

  Serial.println("Iniciando radio SX1262...");
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
  Serial.printf("🌐 Servidor web: http://%s\n\n", IP.toString().c_str());
}

void applyLoRaConfig() {
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  Serial.println("\n📻 Configuración LoRa aplicada:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   ACK cada: %d fragmentos\n", currentACK);
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa Gateway Heltec v6</title>";
    html += "<style>";
    html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
    html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }";
    html += ".container { max-width: 1000px; margin: 0 auto; background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }";
    html += "h1 { color: #333; border-bottom: 3px solid #667eea; padding-bottom: 15px; margin-bottom: 25px; }";
    html += ".section { background: #f8f9fa; padding: 20px; border-radius: 10px; margin: 20px 0; }";
    html += ".section h2 { color: #667eea; margin-bottom: 15px; font-size: 1.3em; }";
    html += ".lora-config { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }";
    html += ".param-group { background: white; padding: 15px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.05); }";
    html += ".param-group label { display: block; font-weight: bold; color: #333; margin-bottom: 8px; font-size: 0.9em; }";
    html += ".param-group select { width: 100%; padding: 10px; border: 2px solid #667eea; border-radius: 5px; font-size: 14px; background: white; cursor: pointer; }";
    html += ".btn-apply { background: #667eea; color: white; padding: 12px 30px; font-size: 16px; border: none; border-radius: 5px; cursor: pointer; margin-top: 15px; width: 100%; font-weight: bold; }";
    html += ".btn-apply:hover { background: #5568d3; }";
    html += ".current-config { background: #e7f3ff; padding: 15px; border-radius: 8px; margin-bottom: 15px; }";
    html += ".config-badge { display: inline-block; background: #667eea; color: white; padding: 5px 12px; border-radius: 15px; margin: 5px; font-size: 0.85em; }";
    html += ".time-display { background: linear-gradient(135deg, #d4edda 0%, #c3e6cb 100%); padding: 20px; border-radius: 8px; margin: 15px 0; text-align: center; }";
    html += ".time-display .time-value { font-size: 2.5em; font-weight: bold; color: #155724; }";
    html += ".time-display .time-label { color: #155724; font-size: 1em; margin-top: 5px; }";
    html += ".file-list { list-style: none; }";
    html += ".file-item { background: white; padding: 15px; margin: 10px 0; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ".file-info { flex-grow: 1; }";
    html += ".file-name { font-weight: bold; color: #333; font-size: 1.1em; }";
    html += ".file-size { color: #666; font-size: 0.9em; margin-top: 5px; }";
    html += ".btn-group { display: flex; gap: 10px; }";
    html += "button { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 14px; transition: all 0.3s; }";
    html += ".btn-download { background: #28a745; color: white; }";
    html += ".btn-download:hover { background: #218838; }";
    html += ".btn-delete { background: #dc3545; color: white; }";
    html += ".btn-delete:hover { background: #c82333; }";
    html += ".btn-refresh { background: #667eea; color: white; margin-top: 20px; width: 100%; padding: 12px; font-weight: bold; }";
    html += ".btn-refresh:hover { background: #5568d3; }";
    html += ".empty { text-align: center; color: #999; padding: 30px; }";
    html += ".receiving { background: #fff3cd; padding: 15px; border-radius: 8px; margin: 15px 0; color: #856404; border-left: 4px solid #ffc107; }";
    html += ".progress-container { background: #f0f0f0; border-radius: 10px; overflow: hidden; margin: 15px 0; }";
    html += ".progress-bar { background: linear-gradient(90deg, #667eea, #764ba2); height: 30px; transition: width 0.3s; display: flex; align-items: center; justify-content: center; color: white; font-weight: bold; }";
    html += ".device-info { background: #e7f3ff; padding: 15px; border-radius: 8px; margin-bottom: 20px; font-size: 0.9em; text-align: center; }";
    html += ".warning { background: #fff3cd; border-left: 4px solid #ffc107; padding: 15px; margin: 15px 0; color: #856404; border-radius: 5px; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>🛰️ LoRa Gateway Heltec v6</h1>";
    
    html += "<div class='device-info'>";
    html += "📟 <strong>Heltec WiFi LoRa 32 V3</strong> | 📡 <strong>SX1262</strong> | 📶 <strong>915 MHz</strong> | 🛡️ <strong>Validación Robusta</strong>";
    html += "</div>";

    // ✅ Advertencia durante recepción
    if (receivingFile) {
      html += "<div class='warning'>";
      html += "⚠️ <strong>RECEPCIÓN EN PROGRESO:</strong> No actualices manualmente ni cambies configuración hasta que termine.";
      html += "</div>";
    }
    
    // Configuración LoRa
    html += "<div class='section'>";
    html += "<h2>⚙️ Configuración LoRa</h2>";
    html += "<div class='current-config'>";
    html += "<strong>Configuración Actual:</strong><br>";
    html += "<span class='config-badge'>BW: " + String((int)currentBW) + " kHz</span>";
    html += "<span class='config-badge'>SF: " + String(currentSF) + "</span>";
    html += "<span class='config-badge'>CR: 4/" + String(currentCR) + "</span>";
    html += "<span class='config-badge'>ACK cada: " + String(currentACK) + " paquetes</span>";
    html += "</div>";
    
    // ✅ Solo permitir cambios si NO hay recepción activa
    if (!receivingFile) {
      html += "<form class='lora-config' action='/config' method='GET'>";
      html += "<div class='param-group'>";
      html += "<label>📶 Bandwidth (kHz)</label>";
      html += "<select name='bw'>";
      html += "<option value='125'" + String(currentBW == 125.0 ? " selected" : "") + ">125</option>";
      html += "<option value='250'" + String(currentBW == 250.0 ? " selected" : "") + ">250</option>";
      html += "<option value='500'" + String(currentBW == 500.0 ? " selected" : "") + ">500</option>";
      html += "</select></div>";
      html += "<div class='param-group'>";
      html += "<label>📡 Spreading Factor</label>";
      html += "<select name='sf'>";
      html += "<option value='7'" + String(currentSF == 7 ? " selected" : "") + ">7</option>";
      html += "<option value='9'" + String(currentSF == 9 ? " selected" : "") + ">9</option>";
      html += "<option value='12'" + String(currentSF == 12 ? " selected" : "") + ">12</option>";
      html += "</select></div>";
      html += "<div class='param-group'>";
      html += "<label>🔧 Coding Rate</label>";
      html += "<select name='cr'>";
      html += "<option value='5'" + String(currentCR == 5 ? " selected" : "") + ">4/5</option>";
      html += "<option value='7'" + String(currentCR == 7 ? " selected" : "") + ">4/7</option>";
      html += "<option value='8'" + String(currentCR == 8 ? " selected" : "") + ">4/8</option>";
      html += "</select></div>";
      html += "<div class='param-group'>";
      html += "<label>✅ ACK cada N paquetes</label>";
      html += "<select name='ack'>";
      html += "<option value='3'" + String(currentACK == 3 ? " selected" : "") + ">3 (Muy Confiable)</option>";
      html += "<option value='5'" + String(currentACK == 5 ? " selected" : "") + ">5 (Recomendado)</option>";
      html += "<option value='7'" + String(currentACK == 7 ? " selected" : "") + ">7 (Balanceado)</option>";
      html += "<option value='10'" + String(currentACK == 10 ? " selected" : "") + ">10 (Rápido)</option>";
      html += "<option value='15'" + String(currentACK == 15 ? " selected" : "") + ">15 (Muy Rápido)</option>";
      html += "</select></div>";
      html += "</form>";
      html += "<button class='btn-apply' onclick='applyConfig()'>✅ Aplicar Configuración</button>";
    } else {
      html += "<p style='text-align:center; color:#856404;'>🔒 Configuración bloqueada durante recepción</p>";
    }
    html += "</div>";
    
    // Progreso de recepción
    if (receivingFile && expectedFileSize > 0) {
      float progress = (receivedBytes * 100.0) / expectedFileSize;
      html += "<div class='receiving'>📡 Recibiendo: <strong>" + currentFileName + "</strong></div>";
      html += "<div class='progress-container'>";
      html += "<div class='progress-bar' style='width: " + String(progress, 1) + "%'>";
      html += String(receivedBytes) + " / " + String(expectedFileSize) + " bytes (" + String(progress, 1) + "%)";
      html += "</div></div>";
    }
    
    // Tiempo de recepción
    if (lastReceptionTime > 0) {
      html += "<div class='time-display'>";
      html += "<div class='time-value'>⏱️ " + String(lastReceptionTime, 2) + " s</div>";
      html += "<div class='time-label'>Última recepción completa</div>";
      html += "</div>";
    }
    
    // Lista de archivos
    html += "<div class='section'>";
    html += "<h2>📁 Archivos Recibidos</h2>";
    
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    bool hasFiles = false;
    
    html += "<ul class='file-list'>";
    while (file) {
      if (!file.isDirectory()) {
        hasFiles = true;
        String fullPath = String(file.name());
        String displayName = fullPath;
        if (displayName.startsWith("/")) displayName = displayName.substring(1);
        
        html += "<li class='file-item'>";
        html += "<div class='file-info'><div class='file-name'>📄 " + displayName + "</div>";
        html += "<div class='file-size'>" + String(file.size()) + " bytes</div></div>";
        html += "<div class='btn-group'>";
        html += "<button class='btn-download' onclick='location.href=\"/download?file=" + displayName + "\"'>📥 Descargar</button>";
        html += "<button class='btn-delete' onclick='if(confirm(\"¿Eliminar?\")) location.href=\"/delete?file=" + displayName + "\"'>🗑️</button>";
        html += "</div></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul>";
    
    if (!hasFiles) {
      html += "<div class='empty'>No hay archivos recibidos aún.<br>Esperando transmisiones LoRa...</div>";
    }
    
    html += "</div>";
    html += "<button class='btn-refresh' onclick='location.reload()'>🔄 Actualizar Manualmente</button>";
    
    html += "<script>";
    html += "function applyConfig() {";
    html += "  const bw = document.querySelector('select[name=bw]').value;";
    html += "  const sf = document.querySelector('select[name=sf]').value;";
    html += "  const cr = document.querySelector('select[name=cr]').value;";
    html += "  const ack = document.querySelector('select[name=ack]').value;";
    html += "  fetch(`/config?bw=${bw}&sf=${sf}&cr=${cr}&ack=${ack}`).then(() => { alert('✅ Configuración aplicada'); location.reload(); });";
    html += "}";
    
    // ✅ Auto-refresh solo durante recepción (cada 2 segundos)
    if (receivingFile) {
      html += "setTimeout(() => location.reload(), 2000);";
    }
    
    html += "</script>";
    html += "</div></body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("bw") && request->hasParam("sf") && request->hasParam("cr") && request->hasParam("ack")) {
      if (receivingFile) {
        request->send(400, "text/plain", "No cambiar durante recepción");
        return;
      }
      
      currentBW = request->getParam("bw")->value().toFloat();
      currentSF = request->getParam("sf")->value().toInt();
      currentCR = request->getParam("cr")->value().toInt();
      currentACK = request->getParam("ack")->value().toInt();
      
      applyLoRaConfig();
      
      radio.standby();
      radio.setDio1Action(setFlag);
      radio.startReceive();
      
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Parámetros faltantes");
    }
  });

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (!filename.startsWith("/")) filename = "/" + filename;
      
      if (LittleFS.exists(filename)) {
        String contentType = getContentType(filename);
        request->send(LittleFS, filename, contentType, true);
      } else {
        request->send(404, "text/plain", "Archivo no encontrado");
      }
    } else {
      request->send(400, "text/plain", "Falta file");
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
  Serial.println("✅ Servidor web iniciado");
}

void loop() {
  // ✅ Timeout de recepción
  if (receivingFile && (millis() - lastPacketTime) > RX_TIMEOUT) {
    Serial.println("\n⚠️  TIMEOUT - No se reciben más paquetes");
    Serial.printf("   Recibido: %u / %u bytes (%.1f%%)\n", 
                  receivedBytes, expectedFileSize, 
                  (receivedBytes * 100.0) / expectedFileSize);
    receivingFile = false;
  }
  
  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();
      lastPacketTime = millis();
      
      Serial.printf("📡 RX: %d bytes | RSSI: %.1f dBm | SNR: %.1f dB\n", 
                    packetLen, radio.getRSSI(), radio.getSNR());
      
      // ✅ VALIDACIÓN ESTRICTA: Verificar si es paquete de metadatos
      if (packetLen >= 8 && buffer[0] == METADATA_MAGIC_1 && buffer[1] == METADATA_MAGIC_2) {
        // ✅ Solo procesar metadatos si NO estamos en medio de una recepción
        if (!receivingFile) {
          processMetadata(buffer, packetLen);
        } else {
          Serial.println("⚠️  Metadatos recibidos durante recepción activa - IGNORANDO");
          Serial.printf("   (Archivo actual: %s, %u/%u bytes recibidos)\n", 
                       currentFileName.c_str(), receivedBytes, expectedFileSize);
        }
      } else if (packetLen >= 4) {
        // Paquete de datos
        processPacket(buffer, packetLen);
      }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("❌ CRC Error");
    }

    delay(20);
    receivedFlag = false;
    radio.startReceive();
  }
  
  yield();
}

// ✅ VALIDACIÓN ROBUSTA DE METADATOS
void processMetadata(uint8_t* data, size_t len) {
  if (len < 8) {  // Mínimo: 2 magic + 4 size + 1 nameLen + 1 char
    Serial.println("⚠️  Metadatos muy cortos");
    return;
  }
  
  // ✅ Verificar magic bytes
  if (data[0] != METADATA_MAGIC_1 || data[1] != METADATA_MAGIC_2) {
    Serial.println("⚠️  Magic bytes incorrectos");
    return;
  }
  
  receptionStartTime = millis();
  lastPacketTime = millis();
  
  // Extraer tamaño (offset +2 por los magic bytes)
  memcpy(&expectedFileSize, data + 2, 4);
  
  // ✅ Validar tamaño razonable (máximo 10MB)
  if (expectedFileSize == 0 || expectedFileSize > 10485760) {
    Serial.printf("⚠️  Tamaño inválido: %u bytes\n", expectedFileSize);
    return;
  }
  
  uint8_t nameLen = data[6];
  
  // ✅ Validar longitud de nombre
  if (nameLen == 0 || nameLen > 100 || len < (7 + nameLen)) {
    Serial.printf("⚠️  Longitud de nombre inválida: %d\n", nameLen);
    return;
  }
  
  // Extraer nombre
  char nameBuf[101];
  memcpy(nameBuf, data + 7, nameLen);
  nameBuf[nameLen] = '\0';
  
  // ✅ Validar caracteres del nombre (solo ASCII imprimible)
  bool validName = true;
  for (int i = 0; i < nameLen; i++) {
    if (nameBuf[i] < 32 || nameBuf[i] > 126) {
      if (nameBuf[i] != '.') {  // Permitir punto
        validName = false;
        break;
      }
    }
  }
  
  if (!validName) {
    Serial.println("⚠️  Nombre contiene caracteres inválidos");
    return;
  }
  
  currentFileName = String(nameBuf);
  
  if (!currentFileName.startsWith("/")) {
    currentFileName = "/" + currentFileName;
  }
  
  Serial.println("\n📋 METADATOS VÁLIDOS:");
  Serial.printf("   📁 %s\n", currentFileName.c_str());
  Serial.printf("   📊 %u bytes\n", expectedFileSize);
  Serial.printf("   📻 BW=%.0f kHz, SF=%d, CR=4/%d, ACK=%d\n", currentBW, currentSF, currentCR, currentACK);
  
  if (LittleFS.exists(currentFileName)) {
    LittleFS.remove(currentFileName);
    Serial.println("   🗑️  Archivo anterior eliminado");
  }
  
  receivingFile = true;
  receivedBytes = 0;
  lastReceivedIndex = 0xFFFF;
  expectedTotalChunks = 0;
  Serial.println("   ✅ Listo\n");
}

void processPacket(uint8_t* data, size_t len) {
  if (!receivingFile || currentFileName == "") {
    Serial.println("⚠️  Datos sin metadatos - ignorando");
    return;
  }
  
  uint16_t index, total;
  memcpy(&index, data, 2);
  memcpy(&total, data + 2, 2);

  // ✅ Validaciones de sanidad
  if (index >= 1000 || total == 0 || total >= 1000) {
    Serial.printf("⚠️  Índices inválidos - idx:%u tot:%u\n", index, total);
    return;
  }
  
  // ✅ Validar total de chunks consistente
  if (expectedTotalChunks == 0) {
    expectedTotalChunks = total;
  } else if (expectedTotalChunks != total) {
    Serial.printf("⚠️  Total inconsistente - esperado:%u recibido:%u\n", expectedTotalChunks, total);
    return;
  }

  int dataLen = len - 4;
  
  Serial.printf("📦 [%u/%u] %d bytes\n", index + 1, total, dataLen);

  // ✅ Detectar duplicados
  if (index == lastReceivedIndex) {
    Serial.println("   ⚠️  Duplicado - reenviando ACK");
    bool isLastFragment = (index + 1 == total);
    bool isMultipleOfACK = ((index + 1) % currentACK == 0);
    if (isMultipleOfACK || isLastFragment) {
      delay(ACK_DELAY);
      sendAck(index);
    }
    return;
  }
  
  lastReceivedIndex = index;

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
    Serial.printf("✅ OK (%u/%u bytes - %.1f%%)\n", 
                  receivedBytes, expectedFileSize,
                  (receivedBytes * 100.0) / expectedFileSize);
  }

  bool isLastFragment = (index + 1 == total);
  bool isMultipleOfACK = ((index + 1) % currentACK == 0);
  
  if (isMultipleOfACK || isLastFragment) {
    delay(ACK_DELAY);
    sendAck(index);
  }

  if (isLastFragment) {
    receptionEndTime = millis();
    lastReceptionTime = (receptionEndTime - receptionStartTime) / 1000.0;
    delay(200);
    showReceivedFile();
    receivingFile = false;
    receivedBytes = 0;
    lastReceivedIndex = 0xFFFF;
    expectedTotalChunks = 0;
  }
}

void sendAck(uint16_t index) {
  transmittingACK = true;
  
  uint8_t ackPacket[5] = {'A', 'C', 'K'};
  memcpy(ackPacket + 3, &index, 2);
  
  Serial.printf("📤 ACK[%u]... ", index);
  
  radio.standby();
  int state = radio.transmit(ackPacket, sizeof(ackPacket));
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("✅");
  } else {
    Serial.printf("❌ Err:%d\n", state);
  }
  
  delay(50);
  radio.startReceive();
  transmittingACK = false;
  Serial.println();
}

void showReceivedFile() {
  Serial.println("\n🎉 ¡ARCHIVO COMPLETO!\n");
  
  File recibido = LittleFS.open(currentFileName, "r");
  if (!recibido) {
    Serial.println("❌ No se pudo abrir");
    return;
  }
  
  Serial.printf("📁 %s\n", currentFileName.c_str());
  Serial.printf("📊 Tamaño: %d bytes\n", recibido.size());
  Serial.printf("📊 Esperado: %u bytes\n", expectedFileSize);
  Serial.printf("⏱️  Tiempo: %.2f s\n", lastReceptionTime);
  Serial.printf("📈 Velocidad: %.2f bytes/s\n", recibido.size() / lastReceptionTime);
  
  if (recibido.size() != expectedFileSize) {
    Serial.printf("⚠️  ADVERTENCIA: Faltan %d bytes\n", 
                  expectedFileSize - recibido.size());
  } else {
    Serial.println("✅ Integridad OK");
  }
  
  Serial.printf("🌐 http://%s\n\n", WiFi.softAPIP().toString().c_str());
  recibido.close();
}