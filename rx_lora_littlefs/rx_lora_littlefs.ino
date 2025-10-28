#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>

#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14
#define FILE_PATH "/archivo_recibido.txt"
#define MAX_PACKET_SIZE 250

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool receivedFlag = false;

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa ===");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  
  Serial.println("✅ LittleFS montado");

  // Limpiar archivo anterior si existe
  if (LittleFS.exists(FILE_PATH)) {
    LittleFS.remove(FILE_PATH);
    Serial.println("🗑️  Archivo anterior eliminado");
  }

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

  // Configurar interrupción
  radio.setDio1Action(setFlag);
  
  // Iniciar recepción
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error en startReceive: %d\n", state);
  }

  Serial.println("✅ Radio configurado");
  Serial.println("👂 Escuchando paquetes...\n");
}

void loop() {
  if (receivedFlag) {
    receivedFlag = false;

    // Leer el paquete SIN especificar tamaño - RadioLib maneja esto
    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      // Obtener tamaño REAL del paquete recibido
      size_t packetLen = radio.getPacketLength();
      
      Serial.printf("📡 Paquete recibido: %d bytes | RSSI: %.1f dBm | SNR: %.1f dB\n", 
                    packetLen, radio.getRSSI(), radio.getSNR());
      
      if (packetLen >= 4) {
        processPacket(buffer, packetLen);
      } else {
        Serial.printf("⚠️  Paquete muy corto: %d bytes (esperado >= 4)\n", packetLen);
      }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("❌ Error CRC - paquete corrupto");
    } else {
      Serial.printf("❌ Error lectura: %d\n", state);
    }

    // Pequeña pausa antes de reiniciar recepción
    delay(10);
    
    // Reiniciar recepción
    int restartState = radio.startReceive();
    if (restartState != RADIOLIB_ERR_NONE) {
      Serial.printf("⚠️  Error en startReceive: %d\n", restartState);
    }
  }
  
  yield(); // Dar tiempo al sistema
}

void processPacket(uint8_t* data, size_t len) {
  // FILTRAR ACKs que son ecos de nuestra propia transmisión
  if (len == 5 && data[0] == 'A' && data[1] == 'C' && data[2] == 'K') {
    Serial.println("🔄 ACK detectado (eco propio) - ignorando");
    return;
  }
  
  // Extraer índice y total (little-endian)
  uint16_t index, total;
  memcpy(&index, data, 2);
  memcpy(&total, data + 2, 2);

  // Validar valores razonables
  if (index >= 1000 || total == 0 || total >= 1000) {
    Serial.printf("⚠️  Valores sospechosos - index:%u total:%u - posible corrupción\n", index, total);
    Serial.print("   Bytes raw: ");
    for(int i = 0; i < min(len, (size_t)10); i++) {
      Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    return;
  }

  int dataLen = len - 4;
  
  Serial.printf("📦 Fragmento [%u/%u] - %d bytes de datos\n", index + 1, total, dataLen);

  // Abrir archivo
  const char* mode = (index == 0) ? "w" : "a";
  File file = LittleFS.open(FILE_PATH, mode);
  if (!file) {
    Serial.println("❌ Error abriendo archivo para escritura");
    sendAck(index); // Enviar ACK de todas formas
    return;
  }
  
  // Escribir datos
  size_t written = file.write(data + 4, dataLen);
  file.close();

  if (written != dataLen) {
    Serial.printf("⚠️  Escritura incompleta: %d de %d bytes\n", written, dataLen);
  }

  // Enviar ACK
  sendAck(index);

  // Verificar si completamos el archivo
  if (index + 1 == total) {
    delay(100); // Dar tiempo a que se complete la escritura
    showReceivedFile();
  }
}

void sendAck(uint16_t index) {
  // ACK: "ACK" + index (2 bytes)
  uint8_t ackPacket[5] = {'A', 'C', 'K'};
  memcpy(ackPacket + 3, &index, 2);
  
  // CRÍTICO: Limpiar flag ANTES de transmitir
  receivedFlag = false;
  
  Serial.printf("📤 Enviando ACK[%u]: ", index);
  for(int i = 0; i < 5; i++) {
    Serial.printf("%02X ", ackPacket[i]);
  }
  Serial.println();
  
  int state = radio.transmit(ackPacket, sizeof(ackPacket));
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("✅ ACK transmitido OK\n\n");
  } else {
    Serial.printf("❌ Error enviando ACK: %d\n\n", state);
  }
  
  // Esperar a que la transmisión se complete físicamente
  delay(150);
  
  // Limpiar flag nuevamente por si acaso
  receivedFlag = false;
}

void showReceivedFile() {
  Serial.println("\n🎉 ¡ARCHIVO COMPLETO RECIBIDO!\n");
  
  File recibido = LittleFS.open(FILE_PATH, "r");
  if (!recibido) {
    Serial.println("❌ No se pudo abrir archivo recibido");
    return;
  }
  
  Serial.printf("📁 Guardado en: %s\n", FILE_PATH);
  Serial.printf("📊 Tamaño final: %d bytes\n\n", recibido.size());
  
  Serial.println("📄 Primeros 500 caracteres del archivo:");
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