#include "Arduino.h"
#include <WiFi.h>
#include <WiFiClient.h> 
#include <ESP32_FTPClient.h>
#include "FS.h"
#include "SD_MMC.h"
#include <esp_now.h>

// ================= CONFIGURAÇÕES =================
#define WIFI_SSID "FicaVaral"
#define WIFI_PASS "ibiotec2022"

char ftp_server[] = "186.202.57.130";
char ftp_user[]   = "ftpuser";
char ftp_pass[]   = "ftppass";

ESP32_FTPClient ftp(ftp_server, ftp_user, ftp_pass, 15000, 0);

// ================= VARIÁVEIS DE MÉTRICA (TESE) =================
unsigned long t_start_espnow = 0;
unsigned long t_end_espnow = 0;
unsigned long t_start_handover = 0;
unsigned long t_end_wifi_conn = 0;
int upload_failures = 0;
int total_images_received = 0;

enum SystemMode { MODE_ESPNOW, MODE_UPLOAD };
SystemMode currentMode = MODE_ESPNOW;

int currentChunk = 0;
int totalChunks = 0;
bool imageReady = false;
String imageName = "";
uint8_t imgBuffer[55000]; 
size_t imgSize = 0;
unsigned long lastReceiveTime = 0;
const unsigned long RECEIVE_TIMEOUT = 15000;

// ================= FUNÇÃO DE LOG (EXCEL READY) =================
void logToSD(String data) {
  File logFile = SD_MMC.open("/data_log.csv", FILE_APPEND);
  if (logFile) {
    logFile.println(data);
    logFile.close();
  }
}

// ================= SETUP =================
void setup() {      
  Serial.begin(115200);
  delay(1000);
  
  if (!SD_MMC.begin()) {
    Serial.println("❌ SD Card falhou");
    while(1);
  }
  
  // Criar cabeçalho do CSV se for novo arquivo
  if (!SD_MMC.exists("/data_log.csv")) {
    logToSD("Timestamp;ImgSize;ESPNOW_ms;Handover_ms;Upload_ms;Status");
  }

  startESPNOWMode();
  Serial.println("\n✅ Sistema pronto - Coleta de dados iniciada.");
}

// ================= LOOP =================
void loop() {
  switch(currentMode) {
    case MODE_ESPNOW:
      if (imageReady) {
        t_start_handover = millis();
        switchToUploadMode();
      }
      if (lastReceiveTime > 0 && (millis() - lastReceiveTime > RECEIVE_TIMEOUT)) {
        resetReception();
      }
      break;
      
    case MODE_UPLOAD:
      processUpload();
      switchToESPNOWMode();
      break;
  }
  delay(10);
}

// ================= FUNÇÕES ESP-NOW =================
void startESPNOWMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Slave", "12345678", 1, 0); 

  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_recv_cb(OnDataRecv);
  currentMode = MODE_ESPNOW;
}

void switchToESPNOWMode() { 
  startESPNOWMode();
  resetReception();
}

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  lastReceiveTime = millis();
  uint8_t header = data[0];

  if (header == 0x01) { // Início
    t_start_espnow = millis();
    totalChunks = (data[1] << 8) | data[2];
    currentChunk = 0;
    imgSize = 0;
    imageName = "/img_" + String(millis()) + ".jpg";
  } 
  else if (header == 0x02) { // Dados
    int chunkNum = (data[1] << 8) | data[2];
    if (chunkNum == currentChunk + 1) {
      currentChunk = chunkNum;
      memcpy(imgBuffer + imgSize, data + 3, len - 3);
      imgSize += (len - 3);
      
      if (currentChunk == totalChunks) {
        t_end_espnow = millis();
        imageReady = true;
        total_images_received++;
        Serial.printf("\n✅ Imagem %d recebida! (%d bytes)\n", total_images_received, imgSize);
      }
    }
  }
}

void resetReception() {
  currentChunk = 0;
  totalChunks = 0;
  imageReady = false;
  imgSize = 0;
  lastReceiveTime = 0;
}

// ================= FUNÇÕES UPLOAD E MÉTRICAS =================
void switchToUploadMode() {
  esp_now_unregister_recv_cb();
  WiFi.softAPdisconnect(true);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  Serial.print("📶 Conectando WiFi para Upload...");
  unsigned long startWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWifi < 15000) {
    delay(500); Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    t_end_wifi_conn = millis();
    Serial.println(" OK");
    currentMode = MODE_UPLOAD;
  } else {
    upload_failures++;
    Serial.println(" FALHOU");
    logToSD(String(millis()) + ";" + String(imgSize) + ";0;0;0;WIFI_FAIL");
    switchToESPNOWMode();
  }
}

void processUpload() {
  Serial.println("\n=== PROCESSANDO MÉTRICAS E UPLOAD ===");
  
  // Backup no SD
  File sdFile = SD_MMC.open(imageName.c_str(), FILE_WRITE);
  if (sdFile) {
    sdFile.write(imgBuffer, imgSize);
    sdFile.close();
  }

  unsigned long startFTP = millis();
  bool success = uploadToFTP();
  unsigned long endFTP = millis();

  // Cálculos para a tese
  unsigned long espnow_time = t_end_espnow - t_start_espnow;
  unsigned long handover_time = t_end_wifi_conn - t_start_handover;
  unsigned long upload_time = endFTP - startFTP;

  // Gerar linha para o Excel (CSV)
  String logEntry = String(millis()) + ";" + 
                    String(imgSize) + ";" + 
                    String(espnow_time) + ";" + 
                    String(handover_time) + ";" + 
                    String(upload_time) + ";" + 
                    (success ? "SUCCESS" : "FTP_FAIL");
  
  logToSD(logEntry);

  Serial.println("\n--- RELATÓRIO DE PERFORMANCE ---");
  Serial.printf("⏱️ Tempo ESP-NOW: %lu ms\n", espnow_time);
  Serial.printf("⏱️ Tempo Handover: %lu ms\n", handover_time);
  Serial.printf("⏱️ Tempo Upload: %lu ms\n", upload_time);
  Serial.printf("📊 Velocidade Upload: %.2f KB/s\n", (imgSize / 1024.0) / (upload_time / 1000.0));
  Serial.println("--------------------------------\n");

  imageReady = false;
}

bool uploadToFTP() {
  Serial.println("☁️ Enviando para nuvem...");
  ftp.OpenConnection();
  ftp.ChangeWorkDir("/uploads");
  
  String remoteName = "thesis_" + String(millis()) + ".jpg";
  ftp.InitFile("Type I");
  ftp.NewFile(remoteName.c_str());
  
  const size_t CHUNK = 2048;
  for (size_t i = 0; i < imgSize; i += CHUNK) {
    size_t size = min(CHUNK, imgSize - i);
    ftp.WriteData(imgBuffer + i, size);
  }
  
  ftp.CloseFile();
  ftp.CloseConnection();
  return true;
}