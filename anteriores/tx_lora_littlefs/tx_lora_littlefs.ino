#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>

// Pines SX1262 (Heltec V3)
#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14
#define FILE_PATH "/archivo.txt"
#define CHUNK_SIZE 200
#define ACK_TIMEOUT 3000
#define MAX_RETRIES 3

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool ackReceived = false;
void IRAM_ATTR setFlag(void) { 
  ackReceived = true; 
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== TRANSMISOR LoRa MEJORADO ===");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }

  Serial.println("✅ LittleFS montado");
  Serial.println("\nArchivos disponibles:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf(" - %s (%d bytes)\n", file.name(), file.size());
    file = root.openNextFile();
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

  Serial.println("✅ Radio configurado");
  
  Serial.println("\n⏱️  Esperando 3 segundos antes de transmitir...");
  delay(3000);
  
  Serial.println("🚀 Iniciando transmisión de archivo...\n");
  sendFile(FILE_PATH);
}

void loop() {
  // Transmisión única en setup
}

void sendFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.println("❌ Error: archivo no existe");
    return;
  }

  uint32_t totalSize = f.size();
  uint16_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  
  Serial.printf("📁 Archivo: %s\n", path);
  Serial.printf("📊 Tamaño: %u bytes\n", totalSize);
  Serial.printf("📦 Fragmentos: %u\n\n", totalChunks);

  for (uint16_t index = 0; index < totalChunks; index++) {
    uint8_t buffer[CHUNK_SIZE];
    size_t bytesRead = f.read(buffer, CHUNK_SIZE);
    
    if (bytesRead == 0) {
      Serial.println("⚠️  No hay más datos para leer");
      break;
    }

    bool success = false;
    int retries = 0;

    while (!success && retries < MAX_RETRIES) {
      // Crear paquete: [index(2)][total(2)][data(n)]
      uint8_t pkt[4 + bytesRead];
      memcpy(pkt, &index, 2);
      memcpy(pkt + 2, &totalChunks, 2);
      memcpy(pkt + 4, buffer, bytesRead);

      Serial.printf("📤 [%u/%u] Enviando %d bytes", index + 1, totalChunks, bytesRead);
      if (retries > 0) Serial.printf(" (reintento %d/%d)", retries, MAX_RETRIES);
      Serial.println();

      // Limpiar flag de interrupción
      ackReceived = false;
      
      // Transmitir
      int state = radio.transmit(pkt, 4 + bytesRead);
      
      if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("   ❌ Error en transmit(): %d\n", state);
        retries++;
        delay(500);
        continue;
      }

      Serial.println("   ✅ Transmitido OK, esperando ACK...");

      // IMPORTANTE: Dar tiempo para que el radio complete físicamente la TX
      delay(200);

      // Configurar interrupción para recibir ACK
      radio.setDio1Action(setFlag);
      
      // Cambiar a modo recepción
      int rxState = radio.startReceive();
      if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("   ❌ Error en startReceive: %d\n", rxState);
        retries++;
        delay(500);
        continue;
      }

      // Esperar ACK con interrupción
      unsigned long startWait = millis();
      bool validAck = false;
      
      while (millis() - startWait < ACK_TIMEOUT && !validAck) {
        if (ackReceived) {
          ackReceived = false;
          
          uint8_t ackBuffer[20];
          int recvState = radio.readData(ackBuffer, sizeof(ackBuffer));
          
          if (recvState == RADIOLIB_ERR_NONE) {
            size_t ackLen = radio.getPacketLength();
            
            Serial.printf("   📨 ACK recibido: %d bytes | RSSI: %.1f dBm | SNR: %.1f dB\n", 
                         ackLen, radio.getRSSI(), radio.getSNR());
            
            // Verificar formato ACK: "ACK" + index(2 bytes)
            if (ackLen == 5 && ackBuffer[0] == 'A' && ackBuffer[1] == 'C' && ackBuffer[2] == 'K') {
              uint16_t ackIndex;
              memcpy(&ackIndex, ackBuffer + 3, 2);
              
              if (ackIndex == index) {
                Serial.printf("   ✅ ACK válido para fragmento %u\n\n", index + 1);
                validAck = true;
                success = true;
              } else {
                Serial.printf("   ⚠️  ACK con índice incorrecto (recibido:%u esperado:%u)\n", ackIndex, index);
              }
            } else {
              Serial.println("   ⚠️  Paquete no es ACK válido");
            }
            
            // Continuar escuchando si no es el ACK correcto
            if (!validAck) {
              radio.startReceive();
            }
          } else if (recvState == RADIOLIB_ERR_CRC_MISMATCH) {
            Serial.println("   ⚠️  ACK corrupto (CRC error)");
            radio.startReceive();
          }
        }
        
        delay(10);
      }

      if (!success) {
        Serial.println("   ❌ Timeout esperando ACK");
        retries++;
        delay(1000);
      }
    }

    if (!success) {
      Serial.printf("\n❌ FALLO CRÍTICO: Fragmento %u no confirmado después de %d intentos\n", index + 1, MAX_RETRIES);
      Serial.println("   Abortando transmisión...\n");
      f.close();
      return;
    }

    delay(100); // Pausa entre fragmentos
  }

  f.close();
  Serial.println("\n🎉 ¡Transmisión completa exitosa!");
  Serial.printf("📊 Total enviado: %u bytes en %u fragmentos\n", totalSize, totalChunks);
}