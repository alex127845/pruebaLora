#include <Arduino.h>
#include <RadioLib.h>

#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool packetReceived = false;
void setFlag(void) { packetReceived = true; }

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("Iniciando receptor SX1262...");

  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Error iniciando LoRa (%d)\n", state);
    while (true);
  }

  // Mismos parámetros que el TX
  radio.setSpreadingFactor(9);
  radio.setBandwidth(125.0);
  radio.setCodingRate(7);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);

  // Configurar interrupción DIO1
  radio.setDio1Action(setFlag);
  radio.startReceive();

  Serial.println("SX1262 RX escuchando...");
}

void loop() {
  if (packetReceived) {
    packetReceived = false;

    String str;
    int state = radio.readData(str);
    if (state == RADIOLIB_ERR_NONE) {
      Serial.printf("📥 Mensaje recibido: %s\n", str.c_str());
    } else {
      Serial.printf("⚠️ Error al leer paquete (%d)\n", state);
    }

    // Volver a escuchar
    radio.startReceive();
  }
}
