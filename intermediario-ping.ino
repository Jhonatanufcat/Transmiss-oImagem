#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "FS.h"
#include "SD_MMC.h"

#define CHANNEL 1
#define ONBOARD_LED 4

#define CMD_START_IMAGE 0x01
#define CMD_IMAGE_DATA  0x02
#define CMD_PING        0x10
#define CMD_PONG        0x11

uint8_t destinationMac[] = {0xC8, 0xF0, 0x9E, 0x9D, 0x4F, 0xBD};

uint8_t masterMac[6];
bool masterConhecido = false;
bool masterOnline = false;
bool pingEnviado = false;

bool newImageReceived = false;
File imageFile;
bool isFileOpen = false;

int imageCounter = 0;
char currentFileName[32];

unsigned long transmissionStartTime = 0;
unsigned long transmissionEndTime = 0;
int receivedPackets = 0;
uint32_t expectedPackets = 0;
int lostPackets = 0;
size_t totalBytesReceived = 0;
uint32_t lastPacketIndex = 0;
int rssiSum = 0;
int rssiCount = 0;

void sendData(const uint8_t *data, size_t len);
void configDeviceAP();
void InitESPNow();
void addPeer(const uint8_t *mac);
void printReport();
void processReceivedData(const uint8_t *data, int data_len, const uint8_t *senderMac);
void sendPingToMaster();
void findNextImageCounter();

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len) {
  const uint8_t *senderMac = recv_info->src_addr;

  if (recv_info->rx_ctrl) {
    rssiSum += recv_info->rx_ctrl->rssi;
    rssiCount++;
  }

  processReceivedData(data, data_len, senderMac);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32 INTERMEDIARIO - LONG RANGE + PING UNICO");

  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, LOW);

  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Falha ao montar SD");
  } else {
    Serial.println("SD OK");
    findNextImageCounter();
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  configDeviceAP();
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);

  InitESPNow();

  esp_wifi_config_espnow_rate(WIFI_IF_AP, WIFI_PHY_RATE_1M_L);
  esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);

  addPeer(destinationMac);

  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("Aguardando MASTER...");
}

void loop() {
  if (newImageReceived) {
    newImageReceived = false;
    digitalWrite(ONBOARD_LED, HIGH);
    delay(200);
    digitalWrite(ONBOARD_LED, LOW);
  }
}

void findNextImageCounter() {
  imageCounter = 0;

  int nextNumber = 1;

  while (true) {
    char testFileName[32];
    snprintf(testFileName, sizeof(testFileName), "/image_%d.jpg", nextNumber);

    if (!SD_MMC.exists(testFileName)) {
      imageCounter = nextNumber - 1;
      break;
    }

    nextNumber++;
  }

  Serial.print("Próxima imagem será: /image_");
  Serial.print(imageCounter + 1);
  Serial.println(".jpg");
}

void processReceivedData(const uint8_t *data, int data_len, const uint8_t *senderMac) {
  if (data_len <= 0) return;

  uint8_t header = data[0];

  if (!masterConhecido && memcmp(senderMac, destinationMac, 6) != 0) {
    memcpy(masterMac, senderMac, 6);
    masterConhecido = true;
    addPeer(masterMac);

    Serial.print("✅ MASTER detectado: ");
    for (int i = 0; i < 6; i++) {
      Serial.printf("%02X", masterMac[i]);
      if (i < 5) Serial.print(":");
    }
    Serial.println();

    if (!pingEnviado) {
      sendPingToMaster();
      pingEnviado = true;
    }
  }

  if (header == CMD_PONG) {
    masterOnline = true;
    Serial.println("✅ PONG recebido do MASTER. Conexão OK.");
    return;
  }

  if (header == CMD_PING) {
    Serial.println("📥 PING recebido");
    uint8_t pong[] = {CMD_PONG};
    addPeer(senderMac);
    esp_now_send(senderMac, pong, sizeof(pong));
    Serial.println("📤 PONG respondido");
    return;
  }

  const uint8_t *payload = &data[1];

  switch (header) {
    case CMD_START_IMAGE: {
      if (data_len >= 5) {
        expectedPackets =
          ((uint32_t)data[1] << 24) |
          ((uint32_t)data[2] << 16) |
          ((uint32_t)data[3] << 8)  |
          ((uint32_t)data[4]);
      } else {
        expectedPackets = payload[0] << 8 | payload[1];
      }

      receivedPackets = 0;
      lostPackets = 0;
      totalBytesReceived = 0;
      lastPacketIndex = 0;
      rssiSum = 0;
      rssiCount = 0;
      transmissionStartTime = millis();

      if (isFileOpen) {
        imageFile.close();
        isFileOpen = false;
      }

      imageCounter++;
      snprintf(currentFileName, sizeof(currentFileName), "/image_%d.jpg", imageCounter);

      while (SD_MMC.exists(currentFileName)) {
        imageCounter++;
        snprintf(currentFileName, sizeof(currentFileName), "/image_%d.jpg", imageCounter);
      }

      Serial.printf("\nRECEBENDO: %s (%lu pacotes)\n", currentFileName, expectedPackets);

      imageFile = SD_MMC.open(currentFileName, FILE_WRITE);
      if (imageFile) {
        isFileOpen = true;
      } else {
        Serial.println("❌ Erro ao criar arquivo no SD");
      }

      sendData(data, data_len);
      break;
    }

    case CMD_IMAGE_DATA: {
      uint32_t packetIdx = payload[0] << 8 | payload[1];

      receivedPackets++;

      int payloadSize = data_len - 3;
      totalBytesReceived += payloadSize;

      if (packetIdx > lastPacketIndex + 1) {
        lostPackets += (packetIdx - lastPacketIndex - 1);
      }

      lastPacketIndex = packetIdx;

      if (isFileOpen) {
        imageFile.write(&data[3], payloadSize);
      }

      sendData(data, data_len);

      if (packetIdx == expectedPackets) {
        if (isFileOpen) {
          imageFile.close();
          isFileOpen = false;
        }

        transmissionEndTime = millis();
        printReport();
        newImageReceived = true;
      }

      break;
    }
  }
}

void sendPingToMaster() {
  uint8_t ping[] = {CMD_PING};

  esp_err_t result = esp_now_send(masterMac, ping, sizeof(ping));

  if (result == ESP_OK) {
    Serial.println("📤 PING enviado ao MASTER uma única vez");
  } else {
    Serial.println("❌ Falha ao enviar PING ao MASTER");
  }
}

void printReport() {
  unsigned long totalTime = transmissionEndTime - transmissionStartTime;
  float throughput = (totalTime > 0) ? (float)totalBytesReceived / (totalTime / 1000.0) : 0;

  int extraPackets = receivedPackets - expectedPackets;
  float per = (expectedPackets > 0 && receivedPackets > 0)
                ? ((float)extraPackets / receivedPackets) * 100
                : 0;

  Serial.println("\n--- MÉTRICAS PARA TESE ---");
  Serial.printf("ID Imagem: %s | Tamanho: %zu Bytes\n", currentFileName, totalBytesReceived);
  Serial.printf("Tempo de Recepção: %lu ms\n", totalTime);
  Serial.printf("Vazão Útil: %.2f KB/s\n", throughput / 1024.0);
  Serial.printf("RSSI Médio: %d dBm\n", (rssiCount > 0) ? rssiSum / rssiCount : 0);
  Serial.printf("Pacotes Esperados/Recebidos: %lu / %d\n", expectedPackets, receivedPackets);
  Serial.printf("Taxa de Reenvio/PER estimada: %.2f %%\n", per);
  Serial.println("---------------------------\n");
}

void InitESPNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("Erro ESP-NOW");
    ESP.restart();
  }
}

void configDeviceAP() {
  WiFi.softAP("Slave", "12345678", CHANNEL, 0);
}

void addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = CHANNEL;
  peerInfo.encrypt = false;

  esp_now_add_peer(&peerInfo);
}

void sendData(const uint8_t *data, size_t len) {
  esp_now_send(destinationMac, data, len);
}