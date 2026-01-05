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

// ✅ Configuración BROADCAST unilateral
#define CHUNK_SIZE 240
#define MAX_RETRIES 3            // Solo para errores de radio persistentes
#define MANIFEST_MAGIC_1 0xAA
#define MANIFEST_MAGIC_2 0xBB
#define DATA_MAGIC_1 0xCC
#define DATA_MAGIC_2 0xDD
#define PARITY_MAGIC_1 0xEE
#define PARITY_MAGIC_2 0xFF
#define FILE_END_MAGIC_1 0x99
#define FILE_END_MAGIC_2 0x88
#define FEC_BLOCK_SIZE 8         // 8 chunks + 1 parity
#define MANIFEST_REPEAT 5        // Repetir manifest al inicio
#define MANIFEST_INTERVAL 50     // Repetir manifest cada N chunks
#define VEXT 36
#define VEXT_ON LOW

const char* ssid = "LoRa-TX-Broadcast";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool transmitting = false;
String currentFile = "";
String transmissionStatus = "";

// Parámetros LoRa configurables
float currentBW = 125.0;
int currentSF = 9;
int currentCR = 7;
int currentREPEAT = 2;           // ✅ Repeticiones (carrusel), antes era currentACK

// Estadísticas
unsigned long transmissionStartTime = 0;
unsigned long transmissionEndTime = 0;
float lastTransmissionTime = 0;
uint32_t lastFileSize = 0;
float lastSpeed = 0;
uint32_t totalPacketsSent = 0;
uint32_t totalRetries = 0;
uint32_t currentFileID = 0;      // ✅ ID único por archivo

// ============================================
// ✅ CRC16-CCITT (polinomio 0x1021)
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

void enableVext(bool on) {
  pinMode(VEXT, OUTPUT);
  digitalWrite(VEXT, on ?  VEXT_ON : ! VEXT_ON);
}

// ✅ Delays dinámicos (sin timeout ACK)
int getInterPacketDelay() {
  if (currentBW >= 500.0) {
    if (currentSF <= 7) return 60;
    if (currentSF == 9) return 100;
    return 130;
  } else if (currentBW >= 250.0) {
    if (currentSF <= 7) return 80;
    if (currentSF == 9) return 120;
    return 150;
  } else {  // BW = 125
    if (currentSF <= 7) return 100;
    if (currentSF == 9) return 130;
    return 180;
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== TRANSMISOR LoRa BROADCAST v7 (UNILATERAL) ===");

  if (! LittleFS.begin(true)) {
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
  Serial.printf("   IP: %s\n\n", IP.toString().c_str());

  setupWebServer();

  Serial.println("Iniciando radio...");
  enableVext(true);
  delay(200);
  int state = radio.begin(915. 0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código:  %d\n", state);
    while (true) delay(1000);
  }
  
  applyLoRaConfig();

  Serial.println("✅ Radio configurado (MODO TX ONLY - SIN RECEPCIÓN)");
  Serial.printf("🌐 Interfaz web: http://%s\n\n", IP.toString().c_str());
}

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando configuración LoRa BROADCAST...");
  
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
  Serial.printf("   REPETICIONES: %d vueltas\n", currentREPEAT);
  Serial.printf("   Delay inter-pkt: %dms\n", getInterPacketDelay());
  Serial.println("✅ Radio listo (TX ONLY)\n");
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa TX Broadcast v7</title>";
    html += "<style>";
    html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
    html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }";
    html += ".container { max-width: 1000px; margin: 0 auto; background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }";
    html += "h1 { color: #333; border-bottom: 3px solid #667eea; padding-bottom: 15px; margin-bottom: 25px; }";
    html += ". section { background: #f8f9fa; padding: 20px; border-radius: 10px; margin:  20px 0; }";
    html += ".section h2 { color: #667eea; margin-bottom: 15px; font-size: 1.3em; }";
    html += ".lora-config { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }";
    html += ".param-group { background: white; padding: 15px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.05); }";
    html += ". param-group label { display: block; font-weight: bold; color: #333; margin-bottom:  8px; font-size: 0.9em; }";
    html += ". param-group select { width: 100%; padding:  10px; border: 2px solid #667eea; border-radius: 5px; font-size: 14px; background: white; cursor: pointer; }";
    html += ".btn-apply { background: #667eea; color: white; padding: 12px 30px; font-size: 16px; border: none; border-radius: 5px; cursor: pointer; margin-top: 15px; width: 100%; font-weight: bold; }";
    html += ".btn-apply:hover { background: #5568d3; }";
    html += ".current-config { background: #e7f3ff; padding: 15px; border-radius: 8px; margin-bottom: 15px; }";
    html += ".config-badge { display: inline-block; background: #667eea; color: white; padding: 5px 12px; border-radius: 15px; margin:  5px; font-size: 0.85em; }";
    html += ".broadcast-badge { background: #ff6b6b; }";
    
    html += ". stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin:  15px 0; }";
    html += ".stat-box { background: white; padding: 15px; border-radius: 8px; text-align: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ". stat-value { font-size: 2em; font-weight: bold; color: #667eea; }";
    html += ".stat-label { color: #666; font-size: 0.9em; margin-top: 5px; }";
    html += ".speed-highlight { background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%); color: white; }";
    
    html += ".file-list { list-style: none; }";
    html += ".file-item { background: white; padding:  15px; margin: 10px 0; border-radius:  8px; display: flex; justify-content: space-between; align-items: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }";
    html += ".file-info { flex-grow: 1; }";
    html += ".file-name { font-weight: bold; color: #333; font-size: 1.1em; }";
    html += ". file-size { color: #666; font-size: 0.9em; margin-top: 5px; }";
    html += ".btn-group { display: flex; gap: 10px; }";
    html += "button { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 14px; transition: all 0.3s; }";
    html += ". btn-send { background: #28a745; color: white; }";
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
    html += ". progress-fill { height: 100%; background: #667eea; transition: width 0.3s; }";
    html += ". warning-box { background: #fff3cd; border-left: 4px solid #ffc107; padding: 15px; margin: 15px 0; border-radius: 5px; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>📡 LoRa TX Broadcast v7 (Unilateral)</h1>";
    
    html += "<div class='warning-box'>";
    html += "<strong>⚠️ MODO BROADCAST: </strong> Sin ACK.  Los receptores reciben sin confirmación.  ";
    html += "Configura 'Repeticiones' para redundancia. ";
    html += "</div>";
    
    // Configuración LoRa
    html += "<div class='section'>";
    html += "<h2>⚙️ Configuración LoRa</h2>";
    html += "<div class='current-config'>";
    html += "<strong>Configuración Actual:</strong><br>";
    html += "<span class='config-badge'>BW: " + String((int)currentBW) + " kHz</span>";
    html += "<span class='config-badge'>SF: " + String(currentSF) + "</span>";
    html += "<span class='config-badge'>CR:  4/" + String(currentCR) + "</span>";
    html += "<span class='config-badge broadcast-badge'>Repeticiones: " + String(currentREPEAT) + " vueltas</span>";
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
    html += "<option value='12'" + String(currentSF == 12 ? " selected" :  "") + ">12</option>";
    html += "</select></div>";
    html += "<div class='param-group'><label>🔧 Coding Rate</label><select name='cr'>";
    html += "<option value='5'" + String(currentCR == 5 ? " selected" : "") + ">4/5</option>";
    html += "<option value='7'" + String(currentCR == 7 ? " selected" : "") + ">4/7</option>";
    html += "<option value='8'" + String(currentCR == 8 ? " selected" : "") + ">4/8</option>";
    html += "</select></div>";
    html += "<div class='param-group'><label>🔁 Repeticiones (Carrusel)</label><select name='repeat'>";
    html += "<option value='1'" + String(currentREPEAT == 1 ? " selected" : "") + ">1 vuelta</option>";
    html += "<option value='2'" + String(currentREPEAT == 2 ? " selected" :  "") + ">2 vueltas</option>";
    html += "<option value='3'" + String(currentREPEAT == 3 ? " selected" :  "") + ">3 vueltas</option>";
    html += "<option value='5'" + String(currentREPEAT == 5 ? " selected" : "") + ">5 vueltas</option>";
    html += "</select></div>";
    html += "</form>";
    html += "<button class='btn-apply' onclick='applyConfig()'>✅ Aplicar Configuración</button>";
    html += "</div>";
    
    // Estadísticas
    if (lastTransmissionTime > 0) {
      html += "<div class='section'>";
      html += "<h2>📊 Última Transmisión</h2>";
      html += "<div class='stats'>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastTransmissionTime, 2) + "s</div><div class='stat-label'>Tiempo</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(lastFileSize) + "</div><div class='stat-label'>Bytes</div></div>";
      html += "<div class='stat-box speed-highlight'><div class='stat-value'>" + String(lastSpeed, 2) + "</div><div class='stat-label'>kbps ⚡</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalPacketsSent) + "</div><div class='stat-label'>Paquetes</div></div>";
      html += "<div class='stat-box'><div class='stat-value'>" + String(totalRetries) + "</div><div class='stat-label'>Fallos Radio</div></div>";
      html += "</div></div>";
    }
    
    if (transmitting) {
      html += "<div class='status transmitting'>⚡ Transmitiendo:  " + currentFile + "...</div>";
    } else if (transmissionStatus != "") {
      html += "<div class='status " + String(transmissionStatus. startsWith("✅") ? "success" : "error") + "'>" + transmissionStatus + "</div>";
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
      if (! file.isDirectory()) {
        hasFiles = true;
        String fileName = String(file.name());
        String displayName = fileName;
        if (displayName.startsWith("/")) displayName = displayName.substring(1);
        
        html += "<li class='file-item'>";
        html += "<div class='file-info'>";
        html += "<div class='file-name'>📄 " + displayName + "</div>";
        html += "<div class='file-size'>" + String(file.size()) + " bytes</div>";
        html += "</div><div class='btn-group'>";
        if (! transmitting) {
          html += "<button class='btn-send' onclick='sendFile(\"" + fileName + "\")'>📡 Broadcast</button>";
          html += "<button class='btn-delete' onclick='deleteFile(\"" + fileName + "\")'>🗑️</button>";
        } else {
          html += "<button disabled>📡 Broadcast</button>";
          html += "<button disabled>🗑️</button>";
        }
        html += "</div></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul>";
    
    if (! hasFiles) {
      html += "<div class='empty'>No hay archivos.  Sube uno para comenzar.</div>";
    }
    
    html += "</div>";
    
    html += "<script>";
    html += "function applyConfig() {";
    html += "  const bw = document.querySelector('select[name=bw]').value;";
    html += "  const sf = document.querySelector('select[name=sf]').value;";
    html += "  const cr = document.querySelector('select[name=cr]').value;";
    html += "  const repeat = document. querySelector('select[name=repeat]').value;";
    html += "  fetch(`/config?bw=${bw}&sf=${sf}&cr=${cr}&repeat=${repeat}`).then(() => { alert('✅ Configuración aplicada'); location.reload(); });";
    html += "}";
    html += "function sendFile(name) {";
    html += "  if(confirm('¿Transmitir ' + name + ' en modo BROADCAST?')) {";
    html += "    fetch('/send? file=' + encodeURIComponent(name)).then(() => location.reload());";
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
    if (request->hasParam("bw") && request->hasParam("sf") && request->hasParam("cr") && request->hasParam("repeat")) {
      if (transmitting) {
        request->send(400, "text/plain", "No cambiar durante transmisión");
        return;
      }
      
      currentBW = request->getParam("bw")->value().toFloat();
      currentSF = request->getParam("sf")->value().toInt();
      currentCR = request->getParam("cr")->value().toInt();
      currentREPEAT = request->getParam("repeat")->value().toInt();
      
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
      Serial.printf("📥 Subiendo:  %s\n", filename.c_str());
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
        request->send(200, "text/plain", "Transmitiendo.. .");
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
    Serial.printf("\n📡 Iniciando transmisión BROADCAST:  %s\n", currentFile. c_str());
    transmissionStartTime = millis();
    
    bool result = sendFile(currentFile. c_str());
    
    transmissionEndTime = millis();
    lastTransmissionTime = (transmissionEndTime - transmissionStartTime) / 1000.0;
    
    lastSpeed = (lastFileSize * 8. 0 * currentREPEAT) / (lastTransmissionTime * 1000.0);  // kbps
    
    if (result) {
      transmissionStatus = "✅ BROADCAST COMPLETO:  " + currentFile;
      transmissionStatus += " (" + String(lastTransmissionTime, 2) + "s";
      transmissionStatus += ", " + String(lastSpeed, 2) + " kbps";
      transmissionStatus += ", " + String(totalPacketsSent) + " paquetes";
      transmissionStatus += ", " + String(currentREPEAT) + " vueltas)";
      Serial.println("\n" + transmissionStatus);
      
      Serial.println("╔════════════════════════════════╗");
      Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps      ║\n", lastSpeed);
      Serial.println("╚════════════════════════════════╝");
    } else {
      transmissionStatus = "❌ Error crítico de radio:  " + currentFile;
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

// ============================================
// ✅ TRANSMITIR MANIFEST (con CRC16)
// ============================================
bool sendManifest(uint32_t fileID, uint32_t totalSize, uint16_t totalChunks, const String& fileName) {
  uint8_t nameLen = min((size_t)fileName.length(), (size_t)100);
  uint8_t manifestPkt[2 + 4 + 4 + 2 + 2 + 1 + nameLen + 2];  // +2 para CRC
  size_t idx = 0;
  
  manifestPkt[idx++] = MANIFEST_MAGIC_1;
  manifestPkt[idx++] = MANIFEST_MAGIC_2;
  memcpy(manifestPkt + idx, &fileID, 4); idx += 4;
  memcpy(manifestPkt + idx, &totalSize, 4); idx += 4;
  memcpy(manifestPkt + idx, &totalChunks, 2); idx += 2;
  uint16_t chunkSize = CHUNK_SIZE;
  memcpy(manifestPkt + idx, &chunkSize, 2); idx += 2;
  manifestPkt[idx++] = nameLen;
  memcpy(manifestPkt + idx, fileName. c_str(), nameLen); idx += nameLen;
  
  uint16_t crc = crc16_ccitt(manifestPkt, idx);
  memcpy(manifestPkt + idx, &crc, 2); idx += 2;
  
  int state = radio.transmit(manifestPkt, idx);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error manifest: %d\n", state);
    return false;
  }
  
  return true;
}

// ============================================
// ✅ TRANSMITIR DATA CHUNK (con CRC16)
// ============================================
bool sendDataChunk(uint32_t fileID, uint16_t chunkIndex, uint16_t totalChunks, uint8_t* data, size_t len) {
  uint8_t dataPkt[2 + 4 + 2 + 2 + CHUNK_SIZE + 2];  // Header + data + CRC
  size_t idx = 0;
  
  dataPkt[idx++] = DATA_MAGIC_1;
  dataPkt[idx++] = DATA_MAGIC_2;
  memcpy(dataPkt + idx, &fileID, 4); idx += 4;
  memcpy(dataPkt + idx, &chunkIndex, 2); idx += 2;
  memcpy(dataPkt + idx, &totalChunks, 2); idx += 2;
  memcpy(dataPkt + idx, data, len); idx += len;
  
  uint16_t crc = crc16_ccitt(dataPkt, idx);
  memcpy(dataPkt + idx, &crc, 2); idx += 2;
  
  int retries = 0;
  while (retries < MAX_RETRIES) {
    int state = radio.transmit(dataPkt, idx);
    if (state == RADIOLIB_ERR_NONE) {
      return true;
    }
    
    Serial.printf("⚠️  TX error %d, retry %d\n", state, retries + 1);
    retries++;
    totalRetries++;
    delay(100);
  }
  
  Serial.printf("❌ CRÍTICO:  Fallo persistente en chunk %u\n", chunkIndex);
  return false;
}

// ============================================
// ✅ TRANSMITIR PARITY (FEC - XOR simple)
// ============================================
bool sendParityChunk(uint32_t fileID, uint16_t blockIndex, uint8_t* parityData, size_t len) {
  uint8_t parityPkt[2 + 4 + 2 + CHUNK_SIZE + 2];
  size_t idx = 0;
  
  parityPkt[idx++] = PARITY_MAGIC_1;
  parityPkt[idx++] = PARITY_MAGIC_2;
  memcpy(parityPkt + idx, &fileID, 4); idx += 4;
  memcpy(parityPkt + idx, &blockIndex, 2); idx += 2;
  memcpy(parityPkt + idx, parityData, len); idx += len;
  
  uint16_t crc = crc16_ccitt(parityPkt, idx);
  memcpy(parityPkt + idx, &crc, 2); idx += 2;
  
  int state = radio.transmit(parityPkt, idx);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Parity TX error: %d\n", state);
    return false;
  }
  
  return true;
}

// ============================================
// ✅ TRANSMITIR FILE_END (fin de vuelta)
// ============================================
bool sendFileEnd(uint32_t fileID, uint16_t totalChunks) {
  uint8_t endPkt[2 + 4 + 2 + 2];
  size_t idx = 0;
  
  endPkt[idx++] = FILE_END_MAGIC_1;
  endPkt[idx++] = FILE_END_MAGIC_2;
  memcpy(endPkt + idx, &fileID, 4); idx += 4;
  memcpy(endPkt + idx, &totalChunks, 2); idx += 2;
  
  uint16_t crc = crc16_ccitt(endPkt, idx);
  memcpy(endPkt + idx, &crc, 2); idx += 2;
  
  int state = radio.transmit(endPkt, idx);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  FILE_END error: %d\n", state);
    return false;
  }
  
  return true;
}

// ============================================
// ✅ ENVIAR ARCHIVO CON CARRUSEL + FEC
// ============================================
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
  
  uint16_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  
  // ✅ File ID único (basado en tiempo + tamaño)
  currentFileID = (uint32_t)millis() ^ totalSize;
  
  Serial.printf("╔════════════════════════════════════════╗\n");
  Serial.printf("║  📁 Archivo: %s\n", fileName.c_str());
  Serial.printf("║  📊 Tamaño: %u bytes\n", totalSize);
  Serial.printf("║  📦 Chunks: %u\n", totalChunks);
  Serial.printf("║  🔁 Repeticiones: %u vueltas\n", currentREPEAT);
  Serial.printf("║  🆔 File ID: 0x%08X\n", currentFileID);
  Serial.printf("╚════════════════════════════════════════╝\n\n");
  
  int dynamicDelay = getInterPacketDelay();
  
  // ============================================
  // ✅ CARRUSEL:  Repetir N vueltas
  // ============================================
  for (int round = 1; round <= currentREPEAT; round++) {
    Serial.printf("\n╔════════════════════════════════════════╗\n");
    Serial.printf("║       🔁 VUELTA %d de %d                \n", round, currentREPEAT);
    Serial.printf("╚════════════════════════════════════════╝\n\n");
    
    // ✅ Enviar MANIFEST repetido al inicio
    Serial.println("📤 Enviando MANIFEST (5 repeticiones)...");
    for (int m = 0; m < MANIFEST_REPEAT; m++) {
      if (!sendManifest(currentFileID, totalSize, totalChunks, fileName)) {
        f.close();
        return false;
      }
      delay(dynamicDelay + 50);
    }
    Serial.println("✅ MANIFEST OK\n");
    
    delay(300);
    
    // ✅ Leer y transmitir chunks (con interleaving opcional)
    f.seek(0);  // Volver al inicio del archivo
    
    uint8_t fecBlock[FEC_BLOCK_SIZE][CHUNK_SIZE];  // Buffer FEC
    size_t fecLengths[FEC_BLOCK_SIZE];
    int fecIndex = 0;
    
    for (uint16_t index = 0; index < totalChunks; index++) {
      uint8_t buffer[CHUNK_SIZE];
      size_t bytesRead = f.read(buffer, CHUNK_SIZE);
      
      if (bytesRead == 0) break;
      
      // ✅ Guardar en buffer FEC
      memcpy(fecBlock[fecIndex], buffer, bytesRead);
      fecLengths[fecIndex] = bytesRead;
      fecIndex++;
      
      // ✅ Transmitir chunk
      Serial.printf("📤 [%u/%u] %d bytes (Vuelta %d)", 
                    index + 1, totalChunks, bytesRead, round);
      
      if (!sendDataChunk(currentFileID, index, totalChunks, buffer, bytesRead)) {
        f.close();
        return false;
      }
      
      Serial.println(" ✅");
      totalPacketsSent++;
      
      delay(dynamicDelay);
      
      // ✅ Cada FEC_BLOCK_SIZE chunks, enviar paridad XOR
      if (fecIndex == FEC_BLOCK_SIZE || index + 1 == totalChunks) {
        uint8_t parityData[CHUNK_SIZE];
        memset(parityData, 0, CHUNK_SIZE);
        
        size_t maxLen = 0;
        for (int i = 0; i < fecIndex; i++) {
          if (fecLengths[i] > maxLen) maxLen = fecLengths[i];
        }
        
        for (int i = 0; i < fecIndex; i++) {
          for (size_t j = 0; j < fecLengths[i]; j++) {
            parityData[j] ^= fecBlock[i][j];
          }
        }
        
        uint16_t blockIndex = index / FEC_BLOCK_SIZE;
        Serial.printf("🛡️  Parity block %u (%d chunks)\n", blockIndex, fecIndex);
        
        if (!sendParityChunk(currentFileID, blockIndex, parityData, maxLen)) {
          Serial.println("⚠️  Parity falló (continuando)");
        }
        
        totalPacketsSent++;
        fecIndex = 0;
        delay(dynamicDelay);
      }
      
      // ✅ Re-enviar manifest periódicamente
      if ((index + 1) % MANIFEST_INTERVAL == 0) {
        Serial.println("\n📤 Re-enviando MANIFEST...");
        sendManifest(currentFileID, totalSize, totalChunks, fileName);
        delay(dynamicDelay + 30);
        Serial.println();
      }
    }
    
    // ✅ FILE_END al terminar cada vuelta
    Serial.printf("\n🏁 Enviando FILE_END (vuelta %d)...\n", round);
    sendFileEnd(currentFileID, totalChunks);
    delay(500);
  }

  f.close();
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║     🎉 TRANSMISIÓN COMPLETA           ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("📊 Total paquetes:  %u\n", totalPacketsSent);
  Serial.printf("📈 Fallos de radio: %u\n", totalRetries);
  
  return true;
}