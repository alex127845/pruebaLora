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
#define ACK_DELAY 300  // Delay antes de enviar ACK (ms)

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool receivedFlag = false;

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEPTOR LoRa MEJORADO ===");

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

    // Pequeña pausa antes de reiniciar recepción
    delay(50);
    
    // Reiniciar recepción
    receivedFlag = false;  // Limpiar flag por si acaso
    int restartState = radio.startReceive();
    if (restartState != RADIOLIB_ERR_NONE) {
      Serial.printf("⚠️  Error en startReceive: %d\n", restartState);
    }
  }
  
  yield();
}

void processPacket(uint8_t* data, size_t len) {
  // Extraer índice y total
  uint16_t index, total;
  memcpy(&index, data, 2);
  memcpy(&total, data + 2, 2);

  // Validar valores razonables
  if (index >= 1000 || total == 0 || total >= 1000) {
    Serial.printf("⚠️  Valores inválidos - index:%u total:%u\n", index, total);
    return;
  }

  int dataLen = len - 4;
  
  Serial.printf("📦 Fragmento [%u/%u] - %d bytes de datos\n", index + 1, total, dataLen);

  // Abrir archivo
  const char* mode = (index == 0) ? "w" : "a";
  File file = LittleFS.open(FILE_PATH, mode);
  if (!file) {
    Serial.println("❌ Error abriendo archivo");
    sendAck(index);
    return;
  }
  
  // Escribir datos
  size_t written = file.write(data + 4, dataLen);
  file.close();

  if (written != dataLen) {
    Serial.printf("⚠️  Escritura incompleta: %d de %d bytes\n", written, dataLen);
  } else {
    Serial.printf("✅ Datos escritos correctamente\n");
  }

  // IMPORTANTE: Esperar antes de enviar ACK para dar tiempo al TX
  delay(ACK_DELAY);
  
  // Enviar ACK
  sendAck(index);

  // Verificar si completamos el archivo
  if (index + 1 == total) {
    delay(200);
    showReceivedFile();
  }
}

void sendAck(uint16_t index) {
  // Crear ACK: "ACK" + index (2 bytes)
  uint8_t ackPacket[5] = {'A', 'C', 'K'};
  memcpy(ackPacket + 3, &index, 2);
  
  Serial.printf("📤 Enviando ACK[%u]... ", index);
  
  // Transmitir ACK
  int state = radio.transmit(ackPacket, sizeof(ackPacket));
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("✅ OK");
  } else {
    Serial.printf("❌ Error: %d\n", state);
  }
  
  // Esperar a que se complete la transmisión física
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
  Serial.printf("📊 Tamaño final: %d bytes\n\n", recibido.size());
  
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