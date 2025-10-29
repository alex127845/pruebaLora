#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>

#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14
#define FILE_PATH "/archivo_recibido.txt"
#define MAX_PACKET_SIZE 250
#define ACK_DELAY 300

// Configura tu WiFi
const char* ssid = "wifi_gtr";
const char* password = "123456789";

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
WebServer server(80);

volatile bool receivedFlag = false;
void IRAM_ATTR setFlag(void) { receivedFlag = true; }

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa CON SERVIDOR WEB ===");

  // Inicializar LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");

  // Conectar WiFi
  WiFi.begin(ssid, password);
  Serial.print("🌐 Conectando a WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi conectado");
    Serial.print("📡 IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("   Accede desde tu navegador a esta IP");
    setupWebServer();
  } else {
    Serial.println("\n⚠️  WiFi no conectado - solo modo LoRa");
  }

  // Inicializar radio
  Serial.println("\nIniciando radio...");
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
  Serial.println("👂 Escuchando paquetes LoRa...\n");
}

void loop() {
  // Manejar servidor web
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }
  
  // Manejar recepción LoRa
  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();
      
      Serial.printf("📡 Paquete: %d bytes | RSSI: %.1f dBm | SNR: %.1f dB\n", 
                    packetLen, radio.getRSSI(), radio.getSNR());
      
      if (packetLen >= 4) {
        processPacket(buffer, packetLen);
      } else {
        Serial.printf("⚠️  Paquete muy corto: %d bytes\n", packetLen);
      }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("❌ Error CRC");
    }

    delay(50);
    receivedFlag = false;
    radio.startReceive();
  }
  
  yield();
}

void setupWebServer() {
  // Página principal
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>Receptor LoRa</title>";
    html += "<style>";
    html += "body { font-family: Arial; max-width: 800px; margin: 50px auto; padding: 20px; }";
    html += "h1 { color: #333; }";
    html += ".file { background: #f0f0f0; padding: 15px; margin: 10px 0; border-radius: 5px; }";
    html += "a { display: inline-block; background: #007bff; color: white; padding: 10px 20px; ";
    html += "text-decoration: none; border-radius: 5px; margin: 5px; }";
    html += "a:hover { background: #0056b3; }";
    html += ".info { color: #666; font-size: 14px; }";
    html += "</style></head><body>";
    html += "<h1>📡 Receptor LoRa - Archivos Recibidos</h1>";
    
    // Listar archivos
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    
    bool hasFiles = false;
    while (file) {
      if (!file.isDirectory()) {
        hasFiles = true;
        html += "<div class='file'>";
        html += "<strong>📄 " + String(file.name()) + "</strong><br>";
        html += "<span class='info'>Tamaño: " + String(file.size()) + " bytes</span><br>";
        html += "<a href='/download?file=" + String(file.name()) + "'>⬇️ Descargar</a>";
        html += "<a href='/view?file=" + String(file.name()) + "'>👁️ Ver</a>";
        html += "<a href='/delete?file=" + String(file.name()) + "' onclick='return confirm(\"¿Eliminar?\")'>🗑️ Eliminar</a>";
        html += "</div>";
      }
      file = root.openNextFile();
    }
    
    if (!hasFiles) {
      html += "<p>No hay archivos recibidos aún.</p>";
    }
    
    html += "<br><a href='/'>🔄 Actualizar</a>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
  });
  
  // Descargar archivo
  server.on("/download", HTTP_GET, []() {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Falta parámetro 'file'");
      return;
    }
    
    String fileName = server.arg("file");
    String filePath = "/" + fileName;
    
    if (!LittleFS.exists(filePath)) {
      server.send(404, "text/plain", "Archivo no encontrado");
      return;
    }
    
    File file = LittleFS.open(filePath, "r");
    if (!file) {
      server.send(500, "text/plain", "Error abriendo archivo");
      return;
    }
    
    // Detectar tipo MIME según extensión
    String contentType = "application/octet-stream";
    if (fileName.endsWith(".txt")) contentType = "text/plain";
    else if (fileName.endsWith(".pdf")) contentType = "application/pdf";
    else if (fileName.endsWith(".jpg") || fileName.endsWith(".jpeg")) contentType = "image/jpeg";
    else if (fileName.endsWith(".png")) contentType = "image/png";
    else if (fileName.endsWith(".json")) contentType = "application/json";
    
    server.streamFile(file, contentType);
    file.close();
    
    Serial.printf("📥 Archivo descargado: %s\n", fileName.c_str());
  });
  
  // Ver contenido (solo texto)
  server.on("/view", HTTP_GET, []() {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Falta parámetro 'file'");
      return;
    }
    
    String fileName = server.arg("file");
    String filePath = "/" + fileName;
    
    File file = LittleFS.open(filePath, "r");
    if (!file) {
      server.send(404, "text/plain", "Archivo no encontrado");
      return;
    }
    
    String content = file.readString();
    file.close();
    
    server.send(200, "text/plain; charset=utf-8", content);
  });
  
  // Eliminar archivo
  server.on("/delete", HTTP_GET, []() {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Falta parámetro 'file'");
      return;
    }
    
    String fileName = server.arg("file");
    String filePath = "/" + fileName;
    
    if (LittleFS.remove(filePath)) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Redirigiendo...");
      Serial.printf("🗑️  Archivo eliminado: %s\n", fileName.c_str());
    } else {
      server.send(500, "text/plain", "Error eliminando archivo");
    }
  });
  
  server.begin();
  Serial.println("🌐 Servidor web iniciado en puerto 80");
}

void processPacket(uint8_t* data, size_t len) {
  uint16_t index, total;
  memcpy(&index, data, 2);
  memcpy(&total, data + 2, 2);

  if (index >= 1000 || total == 0 || total >= 1000) {
    Serial.printf("⚠️  Valores inválidos\n");
    return;
  }

  int dataLen = len - 4;
  Serial.printf("📦 Fragmento [%u/%u] - %d bytes\n", index + 1, total, dataLen);

  const char* mode = (index == 0) ? "w" : "a";
  File file = LittleFS.open(FILE_PATH, mode);
  if (!file) {
    Serial.println("❌ Error abriendo archivo");
    sendAck(index);
    return;
  }
  
  size_t written = file.write(data + 4, dataLen);
  file.close();

  if (written == dataLen) {
    Serial.println("✅ Escrito OK");
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
  
  Serial.printf("📤 ACK[%u]... ", index);
  
  int state = radio.transmit(ackPacket, sizeof(ackPacket));
  Serial.println(state == RADIOLIB_ERR_NONE ? "✅" : "❌");
  
  delay(150);
  Serial.println();
}

void showReceivedFile() {
  Serial.println("\n🎉 ¡ARCHIVO COMPLETO RECIBIDO!\n");
  
  File file = LittleFS.open(FILE_PATH, "r");
  if (!file) return;
  
  Serial.printf("📁 %s\n", FILE_PATH);
  Serial.printf("📊 %d bytes\n", file.size());
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("🌐 Descárgalo en: http://%s/download?file=archivo_recibido.txt\n", 
                  WiFi.localIP().toString().c_str());
  }
  
  Serial.println();
  file.close();
}