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
#define ACK_DELAY 300
#define ACK_EVERY 5  // ✅ NUEVO: Enviar ACK cada 5 fragmentos

const char* ssid = "LoRa-Gateway";
const char* password = "12345678";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool receivedFlag = false;

// ✅ Variables para archivo actual
String currentFileName = "";
uint32_t expectedFileSize = 0;
bool receivingFile = false;

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

// ✅ Función para detectar tipo MIME
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
  
  Serial.println("\n=== RECEPTOR LoRa + GATEWAY WEB v2 (ACK cada 5 fragmentos) ===");

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

  Serial.println("Iniciando radio...");
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    while (true) delay(1000);
  }
  
  radio.setSpreadingFactor(9);
  radio.setBandwidth(125.0);
  radio.setCodingRate(7);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);
  radio.setDio1Action(setFlag);
  
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error en startReceive: %d\n", state);
  }

  Serial.println("✅ Radio configurado");
  Serial.println("👂 Escuchando paquetes LoRa...");
  Serial.printf("🌐 Servidor web disponible en: http://%s\n\n", IP.toString().c_str());
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa Gateway v2</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; max-width: 900px; margin: 50px auto; padding: 20px; background: #f5f5f5; }";
    html += ".container { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
    html += "h1 { color: #333; border-bottom: 3px solid #4CAF50; padding-bottom: 10px; }";
    html += ".section { background: #f8f9fa; padding: 20px; border-radius: 8px; margin: 20px 0; }";
    html += ".file-list { list-style: none; padding: 0; }";
    html += ".file-item { background: white; padding: 15px; margin: 10px 0; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; }";
    html += ".file-name { font-weight: bold; color: #333; }";
    html += ".file-size { color: #666; font-size: 0.9em; }";
    html += "button { background: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; margin: 0 5px; }";
    html += "button:hover { background: #45a049; }";
    html += ".btn-delete { background: #f44336; }";
    html += ".btn-delete:hover { background: #da190b; }";
    html += ".empty { text-align: center; color: #999; padding: 30px; }";
    html += ".receiving { background: #fff3cd; padding: 15px; border-radius: 5px; margin: 15px 0; color: #856404; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>🛰️ LoRa Gateway v2</h1>";
    
    if (receivingFile) {
      html += "<div class='receiving'>📡 Recibiendo: <strong>" + currentFileName + "</strong> (" + String(expectedFileSize) + " bytes)</div>";
    }
    
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
        
        // Remover "/" solo para mostrar
        if (displayName.startsWith("/")) {
          displayName = displayName.substring(1);
        }
        
        html += "<li class='file-item'>";
        html += "<div>";
        html += "<div class='file-name'>📄 " + displayName + "</div>";
        html += "<div class='file-size'>" + String(file.size()) + " bytes</div>";
        html += "</div>";
        html += "<div>";
        // ✅ Enviar displayName (sin /) - se agregará en el servidor
        html += "<button onclick='location.href=\"/download?file=" + displayName + "\"'>📥 Descargar</button>";
        html += "<button class='btn-delete' onclick='if(confirm(\"¿Eliminar?\")) location.href=\"/delete?file=" + displayName + "\"'>🗑️</button>";
        html += "</div>";
        html += "</li>";
      }
      file = root.openNextFile();
    }
    html += "</ul>";
    
    if (!hasFiles) {
      html += "<div class='empty'>No hay archivos recibidos aún.<br>Esperando transmisiones LoRa...</div>";
    }
    
    html += "</div>";
    html += "<button onclick='location.reload()'>🔄 Actualizar</button>";
    html += "<script>setInterval(() => location.reload(), 5000);</script>";
    html += "</div></body></html>";
    
    request->send(200, "text/html", html);
  });

  // ✅ DOWNLOAD CORREGIDO
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      
      // ✅ Asegurar que tenga "/" al inicio
      if (!filename.startsWith("/")) {
        filename = "/" + filename;
      }
      
      Serial.printf("📥 Intentando descargar: %s\n", filename.c_str());
      
      if (LittleFS.exists(filename)) {
        String contentType = getContentType(filename);
        Serial.printf("✅ Archivo encontrado, tipo: %s\n", contentType.c_str());
        request->send(LittleFS, filename, contentType, true);
      } else {
        Serial.printf("❌ Archivo NO encontrado: %s\n", filename.c_str());
        
        // Debug: listar archivos disponibles
        Serial.println("📁 Archivos en LittleFS:");
        File root = LittleFS.open("/");
        File f = root.openNextFile();
        while (f) {
          Serial.printf("  - %s\n", f.name());
          f = root.openNextFile();
        }
        
        request->send(404, "text/plain", "Archivo no encontrado");
      }
    } else {
      request->send(400, "text/plain", "Falta parámetro file");
    }
  });

  // ✅ DELETE CORREGIDO
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      
      // ✅ Asegurar que tenga "/" al inicio
      if (!filename.startsWith("/")) {
        filename = "/" + filename;
      }
      
      Serial.printf("🗑️  Intentando eliminar: %s\n", filename.c_str());
      
      if (LittleFS.remove(filename)) {
        Serial.printf("✅ Eliminado: %s\n", filename.c_str());
        request->redirect("/");
      } else {
        Serial.printf("❌ No se pudo eliminar: %s\n", filename.c_str());
        request->send(500, "text/plain", "Error al eliminar");
      }
    } else {
      request->send(400, "text/plain", "Falta parámetro file");
    }
  });

  server.begin();
  Serial.println("✅ Servidor web iniciado");
}

void loop() {
  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();
      
      Serial.printf("📡 Paquete recibido: %d bytes | RSSI: %.1f dBm | SNR: %.1f dB\n", 
                    packetLen, radio.getRSSI(), radio.getSNR());
      
      // ✅ Detectar tipo de paquete
      if (packetLen > 0 && buffer[0] == 'M') {
        // Paquete de METADATOS
        processMetadata(buffer, packetLen);
      } else if (packetLen >= 4) {
        // Paquete de DATOS
        processPacket(buffer, packetLen);
      } else {
        Serial.printf("⚠️  Paquete muy corto: %d bytes\n", packetLen);
      }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("❌ Error CRC - paquete corrupto");
    } else {
      Serial.printf("❌ Error lectura: %d\n", state);
    }

    delay(50);
    receivedFlag = false;
    int restartState = radio.startReceive();
    if (restartState != RADIOLIB_ERR_NONE) {
      Serial.printf("⚠️  Error en startReceive: %d\n", restartState);
    }
  }
  
  yield();
}

// ✅ Procesar metadatos del archivo
void processMetadata(uint8_t* data, size_t len) {
  if (len < 6) {
    Serial.println("⚠️  Metadatos inválidos");
    return;
  }
  
  // Extraer tamaño del archivo
  memcpy(&expectedFileSize, data + 1, 4);
  
  // Extraer nombre del archivo
  uint8_t nameLen = data[5];
  if (nameLen > 0 && nameLen < 100 && len >= (6 + nameLen)) {
    char nameBuf[101];
    memcpy(nameBuf, data + 6, nameLen);
    nameBuf[nameLen] = '\0';
    currentFileName = String(nameBuf);
    
    // Agregar "/" al inicio
    if (!currentFileName.startsWith("/")) {
      currentFileName = "/" + currentFileName;
    }
    
    Serial.println("\n📋 METADATOS RECIBIDOS:");
    Serial.printf("   📁 Archivo: %s\n", currentFileName.c_str());
    Serial.printf("   📊 Tamaño: %u bytes\n", expectedFileSize);
    
    // Limpiar archivo anterior si existe
    if (LittleFS.exists(currentFileName)) {
      LittleFS.remove(currentFileName);
      Serial.println("   🗑️  Archivo anterior eliminado");
    }
    
    receivingFile = true;
    Serial.println("   ✅ Listo para recibir datos\n");
  } else {
    Serial.println("⚠️  Nombre de archivo inválido en metadatos");
  }
}

void processPacket(uint8_t* data, size_t len) {
  if (!receivingFile || currentFileName == "") {
    Serial.println("⚠️  Datos recibidos sin metadatos previos");
    return;
  }
  
  uint16_t index, total;
  memcpy(&index, data, 2);
  memcpy(&total, data + 2, 2);

  if (index >= 1000 || total == 0 || total >= 1000) {
    Serial.printf("⚠️  Valores inválidos - index:%u total:%u\n", index, total);
    return;
  }

  int dataLen = len - 4;
  
  Serial.printf("📦 Fragmento [%u/%u] - %d bytes de datos\n", index + 1, total, dataLen);

  const char* mode = (index == 0) ? "w" : "a";
  File file = LittleFS.open(currentFileName, mode);
  if (!file) {
    Serial.println("❌ Error abriendo archivo");
    return;
  }
  
  size_t written = file.write(data + 4, dataLen);
  file.close();

  if (written != dataLen) {
    Serial.printf("⚠️  Escritura incompleta: %d de %d bytes\n", written, dataLen);
  } else {
    Serial.printf("✅ Datos escritos correctamente\n");
  }

  // ✅ CAMBIO PRINCIPAL: Enviar ACK solo cada 5 fragmentos o en el último
  bool isLastFragment = (index + 1 == total);
  bool isMultipleOf5 = ((index + 1) % ACK_EVERY == 0);
  
  if (isMultipleOf5 || isLastFragment) {
    delay(ACK_DELAY);
    sendAck(index);
  } else {
    Serial.println("⏭️  Sin ACK (esperando múltiplo de 5)\n");
  }

  if (isLastFragment) {
    delay(200);
    showReceivedFile();
    receivingFile = false;
  }
}

void sendAck(uint16_t index) {
  uint8_t ackPacket[5] = {'A', 'C', 'K'};
  memcpy(ackPacket + 3, &index, 2);
  
  Serial.printf("📤 Enviando ACK[%u]... ", index);
  
  int state = radio.transmit(ackPacket, sizeof(ackPacket));
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("✅ OK");
  } else {
    Serial.printf("❌ Error: %d\n", state);
  }
  
  delay(150);
  Serial.println();
}

void showReceivedFile() {
  Serial.println("\n🎉 ¡ARCHIVO COMPLETO RECIBIDO!\n");
  
  File recibido = LittleFS.open(currentFileName, "r");
  if (!recibido) {
    Serial.println("❌ No se pudo abrir archivo recibido");
    return;
  }
  
  Serial.printf("📁 Guardado en: %s\n", currentFileName.c_str());
  Serial.printf("📊 Tamaño final: %d bytes\n", recibido.size());
  Serial.printf("🌐 Disponible en: http://%s\n\n", WiFi.softAPIP().toString().c_str());
  
  recibido.close();
}