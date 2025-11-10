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
#define CHUNK_SIZE 200
#define ACK_TIMEOUT 4000      // ✅ Aumentado de 3000 a 4000ms
#define MAX_RETRIES 5         // ✅ Aumentado de 3 a 5
#define INTER_PACKET_DELAY 150 // ✅ Delay entre paquetes

// ✅ Agregar al inicio del archivo (después de los #define)
#define METADATA_MAGIC_1 0x4C  // 'L'
#define METADATA_MAGIC_2 0x4D  // 'M'

const char* ssid = "LoRa-TX";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool ackReceived = false;
volatile bool transmitting = false;
volatile bool receivingACK = false;  // ✅ Evitar colisiones
String currentFile = "";
String transmissionStatus = "";

// Parámetros LoRa configurables
float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;
int currentACK = 5;

// Contador de tiempo y estadísticas
unsigned long transmissionStartTime = 0;
unsigned long transmissionEndTime = 0;
float lastTransmissionTime = 0;
uint16_t totalPacketsSent = 0;      // ✅ Estadísticas
uint16_t totalRetries = 0;           // ✅ Estadísticas

void IRAM_ATTR setFlag(void) {
  if (receivingACK) {  // ✅ Solo procesar si estamos esperando ACK
    ackReceived = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== TRANSMISOR LoRa OPTIMIZADO v5 ===");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");
  listFiles();

  Serial.println("\n📡 Configurando WiFi AP...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP iniciado\n");
  Serial.printf("   SSID: %s\n", ssid);
  Serial.printf("   Password: %s\n", password);
  Serial.printf("   IP: %s\n\n", IP.toString().c_str());

  setupWebServer();

  Serial.println("Iniciando radio...");
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    while (true) delay(1000);
  }
  
  applyLoRaConfig();

  Serial.println("✅ Radio configurado");
  Serial.printf("🌐 Interfaz web: http://%s\n\n", IP.toString().c_str());
}

void applyLoRaConfig() {
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  
  Serial.println("\n📻 Configuración LoRa:");
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
    html += "<title>LoRa TX v5</title>";
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
    
    // ✅ Estadísticas
    html += ".stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin: 15px 0; }";
    html += ".stat-box { background: white; padding: 15px; border-radius: 8px; text-align: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ".stat-value { font-size: 2em; font-weight: bold; color: #667eea; }";
    html += ".stat-label { color: #666; font-size: 0.9em; margin-top: 5px; }";
    
    html += ".time-display { background: linear-gradient(135deg, #d4edda 0%, #c3e6cb 100%); padding: 20px; border-radius: 8px; margin: 15px 0; text-align: center; }";
    html += ".time-display .time-value { font-size: 2.5em; font-weight: bold; color: #155724; }";
    html += ".file-list { list-style: none; }";
    html += ".file-item { background: white; padding: 15px; margin: 10px 0; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ".file-info { flex-grow: 1; }";
    html += ".file-name { font-weight: bold; color: #333; font-size: 1.1em; }";
    html += ".file-size { color: #666; font-size: 0.9em; margin-top: 5px; }";
    html += ".btn-group { display: flex; gap: 10px; }";
    html += "button { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 14px; transition: all 0.3s; }";
    html += ".btn-send { background: #28a745; color: white; }";
    html += ".btn-send:hover { background: #218838; }";
    html += ".btn-delete { background: #dc3545; color: white; }";
    html += ".btn-delete:hover { background: #c82333; }";
    html += ".btn-upload { background: #667eea; color: white; padding: 12px 30px; font-size: 16px; }";
    html += "button:disabled { background: #ccc; cursor: not-allowed; }";
    html += ".upload-form { display: flex; gap: 15px; align-items: center; flex-wrap: wrap; }";
    html += "input[type='file'] { flex-grow: 1; padding: 10px; border: 2px dashed #667eea; border-radius: 5px; background: white; }";
    html += ".status { padding: 15px; border-radius: 8px; margin: 15px 0; font-weight: bold; }";
    html += ".status.transmitting { background: #fff3cd; color: #856404; border-left: 4px solid #ffc107; }";
    html += ".status.success { background: #d4edda; color: #155724; border-left: 4px solid #28a745; }";
    html += ".status.error { background: #f8d7da; color: #721c24; border-left: 4px solid #dc3545; }";
    html += ".empty { text-align: center; color: #999; padding: 30px; }";
    html += ".storage-info { background: #e7f3ff; padding: 15px; border-radius: 8px; margin-bottom: 20px; }";
    html += ".progress-bar { width: 100%; height: 8px; background: #e0e0e0; border-radius: 4px; overflow: hidden; margin-top: 10px; }";
    html += ".progress-fill { height: 100%; background: #667eea; transition: width 0.3s; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>📡 LoRa Transmisor v5 (Optimizado)</h1>";
    
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
    html += "<form class='lora-config'>";
    html += "<div class='param-group'><label>📶 Bandwidth (kHz)</label><select name='bw'>";
    html += "<option value='125'" + String(currentBW == 125.0 ? " selected" : "") + ">125</option>";
    html += "<option value='250'" + String(currentBW == 250.0 ? " selected" : "") + ">250</option>";
    html += "<option value='500'" + String(currentBW == 500.0 ? " selected" : "") + ">500</option>";
    html += "</select></div>";
    html += "<div class='param-group'><label>📡 Spreading Factor</label><select name='sf'>";
    html += "<option value='7'" + String(currentSF == 7 ? " selected" : "") + ">7</option>";
    html += "<option value='9'" + String(currentSF == 9 ? " selected" : "") + ">9</option>";
    html += "<option value='12'" + String(currentSF == 12 ? " selected" : "") + ">12</option>";
    html += "</select></div>";
    html += "<div class='param-group'><label>🔧 Coding Rate</label><select name='cr'>";
    html += "<option value='5'" + String(currentCR == 5 ? " selected" : "") + ">4/5</option>";
    html += "<option value='7'" + String(currentCR == 7 ? " selected" : "") + ">4/7</option>";
    html += "<option value='8'" + String(currentCR == 8 ? " selected" : "") + ">4/8</option>";
    html += "</select></div>";
    html += "<div class='param-group'><label>✅ ACK cada N paquetes</label><select name='ack'>";
    html += "<option value='3'" + String(currentACK == 3 ? " selected" : "") + ">3 (Muy Confiable)</option>";
    html += "<option value='5'" + String(currentACK == 5 ? " selected" : "") + ">5 (Recomendado)</option>";
    html += "<option value='7'" + String(currentACK == 7 ? " selected" : "") + ">7 (Balanceado)</option>";
    html += "<option value='10'" + String(currentACK == 10 ? " selected" : "") + ">10 (Rápido)</option>";
    html += "</select></div>";
    html += "</form>";
    html += "<button class='btn-apply' onclick='applyConfig()'>✅ Aplicar Configuración</button>";
    html += "</div>";
    
    // ✅ Estadísticas de última transmisión
    if (lastTransmissionTime > 0) {
      html += "<div class='section'>";
      html += "<h2>📊 Última Transmisión</h2>";
      html += "<div class='stats'>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastTransmissionTime, 2) + "s</div><div class='stat-label'>Tiempo</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalPacketsSent) + "</div><div class='stat-label'>Paquetes</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalRetries) + "</div><div class='stat-label'>Reintentos</div></div>";
      html += "</div></div>";
    }
    
    if (transmitting) {
      html += "<div class='status transmitting'>⚡ Transmitiendo: " + currentFile + "...</div>";
    } else if (transmissionStatus != "") {
      html += "<div class='status " + String(transmissionStatus.startsWith("✅") ? "success" : "error") + "'>" + transmissionStatus + "</div>";
    }

    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    int usedPercent = (usedBytes * 100) / totalBytes;
    
    html += "<div class='storage-info'>";
    html += "<strong>💾 Almacenamiento:</strong> " + String(usedBytes) + " / " + String(totalBytes) + " bytes (" + String(usedPercent) + "%)";
    html += "<div class='progress-bar'><div class='progress-fill' style='width:" + String(usedPercent) + "%'></div></div>";
    html += "</div>";
    
    html += "<div class='section'>";
    html += "<h2>📤 Subir Archivo</h2>";
    html += "<form class='upload-form' method='POST' action='/upload' enctype='multipart/form-data'>";
    html += "<input type='file' name='file' required>";
    html += "<button type='submit' class='btn-upload'>Subir</button>";
    html += "</form></div>";
    
    html += "<div class='section'>";
    html += "<h2>📁 Archivos Disponibles</h2>";
    
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    bool hasFiles = false;
    
    html += "<ul class='file-list'>";
    while (file) {
      if (!file.isDirectory()) {
        hasFiles = true;
        String fileName = String(file.name());
        String displayName = fileName;
        if (displayName.startsWith("/")) displayName = displayName.substring(1);
        
        html += "<li class='file-item'>";
        html += "<div class='file-info'>";
        html += "<div class='file-name'>📄 " + displayName + "</div>";
        html += "<div class='file-size'>" + String(file.size()) + " bytes</div>";
        html += "</div><div class='btn-group'>";
        if (!transmitting) {
          html += "<button class='btn-send' onclick='sendFile(\"" + fileName + "\")'>📡 Transmitir</button>";
          html += "<button class='btn-delete' onclick='deleteFile(\"" + fileName + "\")'>🗑️</button>";
        } else {
          html += "<button disabled>📡 Transmitir</button>";
          html += "<button disabled>🗑️</button>";
        }
        html += "</div></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul>";
    
    if (!hasFiles) {
      html += "<div class='empty'>No hay archivos. Sube uno para comenzar.</div>";
    }
    
    html += "</div>";
    
    html += "<script>";
    html += "function applyConfig() {";
    html += "  const bw = document.querySelector('select[name=bw]').value;";
    html += "  const sf = document.querySelector('select[name=sf]').value;";
    html += "  const cr = document.querySelector('select[name=cr]').value;";
    html += "  const ack = document.querySelector('select[name=ack]').value;";
    html += "  fetch(`/config?bw=${bw}&sf=${sf}&cr=${cr}&ack=${ack}`).then(() => { alert('✅ Configuración aplicada'); location.reload(); });";
    html += "}";
    html += "function sendFile(name) {";
    html += "  if(confirm('¿Transmitir ' + name + '?')) {";
    html += "    fetch('/send?file=' + encodeURIComponent(name)).then(() => location.reload());";
    html += "  }";
    html += "}";
    html += "function deleteFile(name) {";
    html += "  if(confirm('¿Eliminar ' + name + '?')) {";
    html += "    fetch('/delete?file=' + encodeURIComponent(name)).then(() => location.reload());";
    html += "  }";
    html += "}";
    html += "if(document.querySelector('.transmitting')) setTimeout(() => location.reload(), 3000);";
    html += "</script>";
    
    html += "</div></body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("bw") && request->hasParam("sf") && request->hasParam("cr") && request->hasParam("ack")) {
      if (transmitting) {
        request->send(400, "text/plain", "No cambiar durante transmisión");
        return;
      }
      
      currentBW = request->getParam("bw")->value().toFloat();
      currentSF = request->getParam("sf")->value().toInt();
      currentCR = request->getParam("cr")->value().toInt();
      currentACK = request->getParam("ack")->value().toInt();
      
      applyLoRaConfig();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Parámetros faltantes");
    }
  });

  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request){
    request->redirect("/");
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    static File uploadFile;
    if (!index) {
      Serial.printf("📥 Subiendo: %s\n", filename.c_str());
      if (!filename.startsWith("/")) filename = "/" + filename;
      uploadFile = LittleFS.open(filename, "w");
    }
    if (uploadFile) uploadFile.write(data, len);
    if (final) {
      uploadFile.close();
      Serial.printf("✅ Subido: %s (%d bytes)\n", filename.c_str(), index + len);
    }
  });

  server.on("/send", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (!filename.startsWith("/")) filename = "/" + filename;
      
      if (LittleFS.exists(filename)) {
        request->send(200, "text/plain", "Transmitiendo...");
        currentFile = filename;
        transmitting = true;
        transmissionStatus = "";
        totalPacketsSent = 0;
        totalRetries = 0;
      } else {
        request->send(404, "text/plain", "No encontrado");
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
        request->send(200, "text/plain", "Eliminado");
      } else {
        request->send(500, "text/plain", "Error");
      }
    }
  });

  server.begin();
  Serial.println("✅ Servidor web iniciado");
}

void loop() {
  if (transmitting) {
    Serial.printf("\n📡 Iniciando transmisión: %s\n", currentFile.c_str());
    transmissionStartTime = millis();
    
    bool result = sendFile(currentFile.c_str());
    
    transmissionEndTime = millis();
    lastTransmissionTime = (transmissionEndTime - transmissionStartTime) / 1000.0;
    
    if (result) {
      transmissionStatus = "✅ Éxito: " + currentFile;
      transmissionStatus += " (" + String(lastTransmissionTime, 2) + "s, ";
      transmissionStatus += String(totalPacketsSent) + " paquetes, ";
      transmissionStatus += String(totalRetries) + " reintentos)";
      Serial.println("\n" + transmissionStatus);
    } else {
      transmissionStatus = "❌ Error: " + currentFile;
      Serial.println("\n" + transmissionStatus);
    }
    
    transmitting = false;
    currentFile = "";
  }
  
  yield();
  delay(100);
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

bool sendFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("❌ Archivo no existe: %s\n", path);
    return false;
  }

  uint32_t totalSize = f.size();
  String fileName = String(path);
  if (fileName.startsWith("/")) fileName = fileName.substring(1);
  
  Serial.printf("📁 %s\n", fileName.c_str());
  Serial.printf("📊 %u bytes\n", totalSize);
  Serial.printf("📻 BW=%.0f, SF=%d, CR=4/%d, ACK=%d\n", currentBW, currentSF, currentCR, currentACK);
  
  // ✅ METADATOS CON MAGIC BYTES
  //uint8_t nameLen = min(fileName.length(), 100);
  uint8_t nameLen = min((size_t)fileName.length(), (size_t)100);
  uint8_t metaPkt[2 + 4 + 1 + nameLen];  // +2 para magic bytes
  
  metaPkt[0] = METADATA_MAGIC_1;  // ✅ Magic byte 1
  metaPkt[1] = METADATA_MAGIC_2;  // ✅ Magic byte 2
  memcpy(metaPkt + 2, &totalSize, 4);
  metaPkt[6] = nameLen;
  memcpy(metaPkt + 7, fileName.c_str(), nameLen);
  
  Serial.println("\n📤 Metadatos con firma...");
  int metaState = radio.transmit(metaPkt, 7 + nameLen);  // 7 = 2+4+1
  
  if (metaState != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error metadatos: %d\n", metaState);
    f.close();
    return false;
  }
  
  Serial.println("✅ Metadatos OK");
  delay(800);
  
  // ✅ DATOS
  uint16_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  Serial.printf("📦 %u fragmentos\n\n", totalChunks);

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
      receivingACK = false;  // ✅ Reset flag
      
      // ✅ Transmitir paquete
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
        
        // ✅ Pequeño delay antes de cambiar a RX
        delay(250);
        
        receivingACK = true;  // ✅ Activar flag ANTES de setDio1Action
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
        
        while (millis() - startWait < ACK_TIMEOUT && !validAck) {
          if (ackReceived) {
            ackReceived = false;
            
            uint8_t ackBuffer[20];
            int recvState = radio.readData(ackBuffer, sizeof(ackBuffer));
            
            if (recvState == RADIOLIB_ERR_NONE) {
              size_t ackLen = radio.getPacketLength();
              
              Serial.printf("   📨 ACK: %d bytes | RSSI: %.1f | SNR: %.1f\n", 
                           ackLen, radio.getRSSI(), radio.getSNR());
              
              if (ackLen == 5 && ackBuffer[0] == 'A' && ackBuffer[1] == 'C' && ackBuffer[2] == 'K') {
                uint16_t ackFragmentNumber;
                memcpy(&ackFragmentNumber, ackBuffer + 3, 2);
                
                // ✅ CAMBIO CRÍTICO: Comparar con número de fragmento
                uint16_t expectedFragmentNumber = index + 1;
                
                if (ackFragmentNumber == expectedFragmentNumber) {
                  Serial.printf("   ✅ ACK válido [%u]\n\n", expectedFragmentNumber);
                  validAck = true;
                  success = true;
                } else {
                  Serial.printf("   ⚠️  ACK incorrecto (rx:%u exp:%u)\n", 
                              ackFragmentNumber, expectedFragmentNumber);
                }



              } else {
                Serial.println("   ⚠️  No es ACK");
              }
              
              if (!validAck) radio.startReceive();
            } else if (recvState == RADIOLIB_ERR_CRC_MISMATCH) {
              Serial.println("   ⚠️  CRC error");
              radio.startReceive();
            }
          }
          delay(10);
        }

        receivingACK = false;  // ✅ Desactivar flag
        
        if (!success) {
          Serial.println("   ❌ Timeout ACK");
          retries++;
          totalRetries++;
          delay(1000);
        }
      } else {
        Serial.printf("   ⏭️  Sin ACK (próximo: %d)\n\n", currentACK);
        success = true;
      }
    }

    if (!success) {
      Serial.printf("\n❌ CRÍTICO: Frag %u falló (%d intentos)\n", index + 1, MAX_RETRIES);
      f.close();
      return false;
    }

    // ✅ Delay entre paquetes
    delay(INTER_PACKET_DELAY);
  }

  f.close();
  Serial.println("\n🎉 ¡TRANSMISIÓN COMPLETA!");
  Serial.printf("📊 %u bytes en %u fragmentos\n", totalSize, totalChunks);
  Serial.printf("📈 Reintentos totales: %u\n", totalRetries);
  return true;
}