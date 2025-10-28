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
#define ACK_TIMEOUT 5000  // 5 segundos
#define MAX_RETRIES 3

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== TRANSMISOR LoRa ===");

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
  
  // Esperar un momento para que el RX esté listo
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

      // Transmitir
      int state = radio.transmit(pkt, 4 + bytesRead);
      
      if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("   ❌ Error en transmit(): %d\n", state);
        retries++;
        delay(500);
        continue;
      }

      Serial.println("   ✅ Transmitido OK");
      Serial.print("   ⏳ Esperando ACK");

      // CRÍTICO: Dar tiempo para que el radio complete la TX
      delay(100);

      // Esperar ACK
      bool ackReceived = false;
      unsigned long startWait = millis();
      
      // Poner en modo recepción
      int rxState = radio.startReceive();
      if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("\n   ❌ Error en startReceive: %d\n", rxState);
        retries++;
        continue;
      }

      while (millis() - startWait < ACK_TIMEOUT) {
        uint8_t ackBuffer[20];
        int recvState = radio.readData(ackBuffer, sizeof(ackBuffer));
        
        if (recvState == RADIOLIB_ERR_NONE) {
          size_t ackLen = radio.getPacketLength();
          
          Serial.printf("\n   📨 Recibido paquete de %d bytes: ", ackLen);
          for(int i = 0; i < min(ackLen, (size_t)10); i++) {
            Serial.printf("%02X ", ackBuffer[i]);
          }
          Serial.println();
          
          // Verificar que sea ACK válido
          if (ackLen == 5 && ackBuffer[0] == 'A' && ackBuffer[1] == 'C' && ackBuffer[2] == 'K') {
            uint16_t ackIndex;
            memcpy(&ackIndex, ackBuffer + 3, 2);
            
            Serial.printf("   🔍 ACK para índice %u (esperamos %u)\n", ackIndex, index);
            
            if (ackIndex == index) {
              Serial.printf("   ✅ ACK correcto! (RSSI: %.1f dBm)\n", radio.getRSSI());
              ackReceived = true;
              break;
            } else {
              Serial.printf("   ⚠️  ACK con índice incorrecto\n");
            }
          } else {
            Serial.println("   ⚠️  Paquete no es ACK válido");
          }
          
          // Continuar escuchando
          radio.startReceive();
        }
        
        // Mostrar progreso
        if ((millis() - startWait) % 1000 == 0) {
          Serial.print(".");
        }
        
        delay(50);
      }

      if (ackReceived) {
        success = true;
        Serial.println();
      } else {
        Serial.println(" ❌ Timeout");
        retries++;
        delay(1000);
      }
    }

    if (!success) {
      Serial.printf("\n❌ FALLO CRÍTICO: Fragmento %u no confirmado después de %d intentos\n", index + 1, MAX_RETRIES);
      Serial.println("   Verifique:");
      Serial.println("   - Que el receptor esté encendido y funcionando");
      Serial.println("   - La distancia entre dispositivos");
      Serial.println("   - Posibles interferencias\n");
      f.close();
      return;
    }

    delay(100); // Pequeña pausa entre fragmentos
  }

  f.close();
  Serial.println("\n🎉 ¡Transmisión completa exitosa!");
  Serial.printf("📊 Total enviado: %u bytes en %u fragmentos\n", totalSize, totalChunks);
}