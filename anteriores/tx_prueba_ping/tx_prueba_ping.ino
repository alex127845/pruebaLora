#include <Arduino.h>
#include <RadioLib.h>

// Pines Heltec WiFi LoRa 32 V3 (SX1262)
#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("Iniciando transmisor SX1262...");

  int state = radio.begin(915.0);  // 915 MHz para América
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Error iniciando LoRa (%d)\n", state);
    while (true);
  }

  // Parámetros opcionales (más alcance)
  radio.setSpreadingFactor(9);
  radio.setBandwidth(125.0);
  radio.setCodingRate(7);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);

  Serial.println("SX1262 TX listo. Enviando mensaje cada 3 segundos...");
}

void loop() {
  int state = radio.transmit("PING SX1262!");
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("📤 Enviado: PING SX1262!");
  } else {
    Serial.printf("⚠️ Error al transmitir (%d)\n", state);
  }

  delay(3000);
}

