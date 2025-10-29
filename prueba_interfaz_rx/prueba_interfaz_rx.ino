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
#define FILE_PATH "/archivo_recibido.pdf"
#define MAX_PACKET_SIZE 250
#define ACK_DELAY 300

// Configuración WiFi AP
const char* ssid = "LoRa-Gateway";
const char* password = "12345678";  // Mínimo 8 caracteres

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool receivedFlag = false;

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa + GATEWAY WEB ===");

  // Iniciar LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");

  // Limpiar archivo anterior
  if (LittleFS.exists(FILE_PATH)) {
    LittleFS.remove(FILE_PATH);
    Serial.println("🗑️  Archivo anterior eliminado");
  }

  // Configurar WiFi como Access Point
  Serial.println("\n📡 Configurando WiFi AP...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("✅ WiFi AP iniciado\n");
  Serial.printf("   SSID: %s\n", ssid);
  Serial.printf("   Password: %s\n", password);
  Serial.printf("   IP: %s\n\n", IP.toString().c_str());

  // Configurar servidor web
  setupWebServer();

  // Iniciar radio LoRa
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
  // Página principal con HTML mejorado
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LoRa Gateway</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; background: #f5f5f5; }";
    html += ".container { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
    html += "h1 { color: #333; border-bottom: 3px solid #4CAF50; padding-bottom: 10px; }";
    html += ".info { background: #e3f2fd; padding: 15px; border-radius: 5px; margin: 20px 0; }";
    html += ".status { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }";
    html += ".status.ready { background: #4CAF50; }";
    html += ".status.waiting { background: #ff9800; }";
    html += "button { background: #4CAF50; color: white; padding: 12px 30px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin: 10px 5px; }";
    html += "button:hover { background: #45a049; }";
    html += "button:disabled { background: #ccc; cursor: not-allowed; }";
    html += ".btn-secondary { background: #2196F3; }";
    html += ".btn-secondary:hover { background: #0b7dda; }";
    html += ".btn-danger { background: #f44336; }";
    html += ".btn-danger:hover { background: #da190b; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>🛰️ LoRa Gateway</h1>";
    
    // Estado del archivo
    if (LittleFS.exists(FILE_PATH)) {
      File file = LittleFS.open(FILE_PATH, "r");
      html += "<div class='info'>";
      html += "<span class='status ready'></span><strong>Archivo recibido</strong><br>";
      html += "📁 Nombre: <code>archivo_recibido</code><br>";
      html += "📊 Tamaño: " + String(file.size()) + " bytes<br>";
      html += "⏰ Listo para descargar";
      file.close();
      html += "</div>";
      html += "<button onclick='location.href=\"/download\"'>📥 Descargar Archivo</button>";
      //html += "<button class='btn-secondary' onclick='location.href=\"/view\"'>👁️ Ver Contenido</button>";
      html += "<button class='btn-secondary' onclick='window.open(\"/download\", \"_blank\")'>👁️ Abrir PDF</button>";
      html += "<button class='btn-danger' onclick='if(confirm(\"¿Eliminar archivo?\")) location.href=\"/delete\"'>🗑️ Eliminar</button>";
    } else {
      html += "<div class='info'>";
      html += "<span class='status waiting'></span><strong>Esperando archivo LoRa...</strong><br>";
      html += "El receptor está escuchando transmisiones.";
      html += "</div>";
      html += "<button disabled>📥 Sin archivo para descargar</button>";
    }
    
    html += "<br><br>";
    html += "<button class='btn-secondary' onclick='location.reload()'>🔄 Actualizar</button>";
    html += "</div></body></html>";
    
    request->send(200, "text/html", html);
  });

  // Descargar archivo en pdf
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists(FILE_PATH)) {
      request->send(LittleFS, FILE_PATH, "application/pdf", true);  // ✅ Correcto para PDF
    } else {
      request->send(404, "text/plain", "Archivo no encontrado");
    }
  });

  // Ver contenido del archivo
  server.on("/view", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists(FILE_PATH)) {
      File file = LittleFS.open(FILE_PATH, "r");
      String content = file.readString();
      file.close();
      
      String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
      html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
      html += "<title>Contenido del Archivo</title>";
      html += "<style>body{font-family:monospace;padding:20px;background:#f5f5f5;}";
      html += "pre{background:white;padding:20px;border-radius:5px;overflow-x:auto;}</style></head>";
      html += "<body><h2>📄 Contenido del Archivo</h2>";
      html += "<pre>" + content + "</pre>";
      html += "<br><button onclick='history.back()'>← Volver</button></body></html>";
      
      request->send(200, "text/html", html);
    } else {
      request->send(404, "text/plain", "Archivo no encontrado");
    }
  });

  // Eliminar archivo
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists(FILE_PATH)) {
      LittleFS.remove(FILE_PATH);
      request->redirect("/");
    } else {
      request->send(404, "text/plain", "Archivo no encontrado");
    }
  });

  // API JSON para estado
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"file_exists\": " + String(LittleFS.exists(FILE_PATH) ? "true" : "false");
    if (LittleFS.exists(FILE_PATH)) {
      File file = LittleFS.open(FILE_PATH, "r");
      json += ", \"file_size\": " + String(file.size());
      file.close();
    }
    json += "}";
    request->send(200, "application/json", json);
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
      
      if (packetLen >= 4) {
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

void processPacket(uint8_t* data, size_t len) {
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
  File file = LittleFS.open(FILE_PATH, mode);
  if (!file) {
    Serial.println("❌ Error abriendo archivo");
    sendAck(index);
    return;
  }
  
  size_t written = file.write(data + 4, dataLen);
  file.close();

  if (written != dataLen) {
    Serial.printf("⚠️  Escritura incompleta: %d de %d bytes\n", written, dataLen);
  } else {
    Serial.printf("✅ Datos escritos correctamente\n");
  }

  delay(ACK_DELAY);
  sendAck(index);

  if (index + 1 == total) {
    delay(200);
    showReceivedFile();
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
  
  File recibido = LittleFS.open(FILE_PATH, "r");
  if (!recibido) {
    Serial.println("❌ No se pudo abrir archivo recibido");
    return;
  }
  
  Serial.printf("📁 Guardado en: %s\n", FILE_PATH);
  Serial.printf("📊 Tamaño final: %d bytes\n", recibido.size());
  Serial.printf("🌐 Disponible para descarga en: http://%s\n\n", WiFi.softAPIP().toString().c_str());
  
  Serial.println("📄 Primeros 500 caracteres:");
  Serial.println("================================");
  
  int count = 0;
  while (recibido.available() && count < 500) {
    Serial.write(recibido.read());
    count++;
  }
  
  if (recibido.available()) {
    Serial.printf("\n... (%d bytes más)", recibido.size() - count);
  }
  
  Serial.println("\n================================\n");
  recibido.close();
}