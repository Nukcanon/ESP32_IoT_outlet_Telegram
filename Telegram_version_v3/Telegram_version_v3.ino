#include <EEPROM.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "UniversalTelegramBot.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <ESP32Servo.h>
#include <esp_mac.h>

// Telegram 루트 인증서 (실제 PEM 필요)
extern const char TELEGRAM_CERTIFICATE_ROOT[] PROGMEM;

#define SERVO_PIN 4
#define SERVO_ON_ANGLE 120
#define SERVO_OFF_ANGLE 80

// AP 이름 최대 크기
#define AP_NAME_SIZE 32

struct eprom_data {
  char wifi_ssid[32];
  char wifi_password[64];
  char telegram_id[32];
  char telegram_token[64];
  char telegram_root_cert[1024];
  int eprom_good;
  int servo_on_angle;
  int servo_off_angle;
  char ap_name[AP_NAME_SIZE];
};

const int CheckNumber = 123;
eprom_data ed;

WiFiClientSecure clientTCP;
UniversalTelegramBot *bot = nullptr;
String telegramChatId;
String telegramBotToken;

WiFiManager wm;
Servo myServo;

int servoOnAngle = SERVO_ON_ANGLE;
int servoOffAngle = SERVO_OFF_ANGLE;
int currentAngle = 100;
int desiredAngle = 100;

bool awaitingOnAngle = false;
bool awaitingOffAngle = false;

unsigned long botRequestDelay = 1000;
unsigned long lastTimeBotRan = 0;

// 메시지 처리 offset
int offset = 0;

// 메시지 전송 관리 변수
int messageneed = 0;
String pendingMessageText = "";
String pendingMessageChatId = "";

// ***** 추가: AP 이름 관리 *****
String devstr;  // WiFiManager용 AP 이름 (실행 시점에 ed.ap_name에서 가져옴)

// ----------------------------------------------------------------------------
// MAC 기반 AP SSID 생성 (실패 시 랜덤값)
// ----------------------------------------------------------------------------
void generateAPSSID(char *outName, size_t size) {
  uint32_t rand_num = esp_random();
  int randomNumber = (rand_num % 9000) + 1000;  // 1000 ~ 9999
  String tmp = "ESP32-" + String(randomNumber);

  // outName에 저장
  strncpy(outName, tmp.c_str(), size - 1);
  outName[size - 1] = '\0';

  Serial.print("랜덤 4자리 AP SSID 생성: ");
  Serial.println(outName);
}

void setAPSSIDwithMAC(char *outName, size_t size) {
  // MAC 주소를 담을 배열 (6바이트)
  uint8_t mac[6] = { 0 };
  // ESP32의 기본 MAC 주소를 읽어옴
  esp_err_t ret = esp_efuse_mac_get_default(mac);
  //  Serial.print("esp_efuse_mac_get_default() 반환값: ");
  //  Serial.println(ret);

  if (ret == ESP_OK) {
    // MAC 주소 마지막 2바이트(예: mac[4], mac[5])만 사용 -> 4자리 16진수
    char macLast4[5];
    sprintf(macLast4, "%02X%02X", mac[4], mac[5]);
    // 예: mac[4] = 0xAB, mac[5] = 0xCD => "ABCD"

    String newSSID = "ESP32-" + String(macLast4);

    // outName에 복사
    strncpy(outName, newSSID.c_str(), size - 1);
    outName[size - 1] = '\0';

    Serial.print("MAC 기반 AP SSID 생성: ");
    Serial.println(outName);
  } else {
    //    Serial.println("MAC 주소 읽기 실패 -> 랜덤 4자리 생성");
    generateAPSSID(outName, size);
  }
}
// ----------------------------------------------------------------------------


bool isDataDifferent(const eprom_data &newData, const eprom_data &oldData) {
  if (newData.eprom_good != oldData.eprom_good) return true;
  if (strcmp(newData.wifi_ssid, oldData.wifi_ssid) != 0) return true;
  if (strcmp(newData.wifi_password, oldData.wifi_password) != 0) return true;
  if (strcmp(newData.telegram_id, oldData.telegram_id) != 0) return true;
  if (strcmp(newData.telegram_token, oldData.telegram_token) != 0) return true;
  if (strcmp(newData.telegram_root_cert, oldData.telegram_root_cert) != 0) return true;
  if (newData.servo_on_angle != oldData.servo_on_angle) return true;
  if (newData.servo_off_angle != oldData.servo_off_angle) return true;
  if (strcmp(newData.ap_name, oldData.ap_name) != 0) return true;

  return false;
}

void do_eprom_write(const eprom_data &newData) {
  eprom_data currentData;
  EEPROM.begin(sizeof(eprom_data));
  EEPROM.get(0, currentData);
  EEPROM.end();

  if (isDataDifferent(newData, currentData)) {
    Serial.println("[EEPROM] 데이터 변경 감지. 저장...");
    EEPROM.begin(sizeof(eprom_data));
    EEPROM.put(0, newData);
    EEPROM.commit();
    EEPROM.end();
    Serial.println("[EEPROM] 저장 완료.");
  }
}

void do_eprom_read(eprom_data &eed) {
  EEPROM.begin(sizeof(eprom_data));
  EEPROM.get(0, eed);
  EEPROM.end();

  if (eed.eprom_good == CheckNumber) {
    // 기존 로직: 서보 각도가 0이면 기본값으로
    if (eed.servo_on_angle == 0 && eed.servo_off_angle == 0) {
      eed.servo_on_angle = SERVO_ON_ANGLE;
      eed.servo_off_angle = SERVO_OFF_ANGLE;
      do_eprom_write(eed);
    }

    // **추가된 로직**: ap_name이 "ESP32-"로 시작하지 않으면 다시 생성
    if (strncmp(eed.ap_name, "ESP32-", 6) != 0) {
      Serial.println("[EEPROM] AP 이름이 'ESP32-'로 시작하지 않음. 새로 생성.");
      setAPSSIDwithMAC(eed.ap_name, sizeof(eed.ap_name));
      do_eprom_write(eed);
    }

  } else {
    Serial.println("[EEPROM] 유효한 설정 없음. 초기화.");
    memset(&eed, 0, sizeof(eed));
    eed.eprom_good = CheckNumber;
    eed.servo_on_angle = SERVO_ON_ANGLE;
    eed.servo_off_angle = SERVO_OFF_ANGLE;

    // CheckNumber가 유효하지 않은 경우, 새로 생성
    setAPSSIDwithMAC(eed.ap_name, sizeof(eed.ap_name));
    do_eprom_write(eed);
  }
}

void resetSettings() {
  eprom_data emptyData;
  memset(&emptyData, 0, sizeof(emptyData));
  emptyData.eprom_good = 0;
  do_eprom_write(emptyData);
  Serial.println("[EEPROM] 설정 초기화 완료 재시작");

  wm.resetSettings();
  //  Serial.println("[WiFiManager] 기존 WiFi 초기화. 재시작.");
  delay(500);
  ESP.restart();
}

void enterTelegramRootCert() {
  Serial.println("[CERT] Telegram 루트 인증서 입력(END 종료):");
  String cert = "";
  while (true) {
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.equalsIgnoreCase("END")) break;
      cert += line + "\n";
      Serial.println("[CERT] 인증서 수신...");
    }
    delay(100);
  }

  if (cert.length() > 0) {
    eprom_data newEd;
    do_eprom_read(newEd);
    strncpy(newEd.telegram_root_cert, cert.c_str(), sizeof(newEd.telegram_root_cert) - 1);
    newEd.telegram_root_cert[sizeof(newEd.telegram_root_cert) - 1] = '\0';
    do_eprom_write(newEd);
    Serial.println("[CERT] 루트 인증서 저장 완료. 재시작.");
    ESP.restart();
  } else {
    Serial.println("[CERT] 인증서 없음. 재시작.");
    ESP.restart();
  }
}

bool enterWiFiAndTelegramCredentials() {
  eprom_data newEd;
  do_eprom_read(newEd);

  Serial.println("[SETUP] WiFi SSID(30초):");
  {
    unsigned long st = millis();
    bool got = false;
    while (millis() - st < 30000) {
      if (Serial.available()) {
        String s_wifi_ssid = Serial.readStringUntil('\n');
        s_wifi_ssid.trim();
        if (s_wifi_ssid.length() > 0) {
          s_wifi_ssid.toCharArray(newEd.wifi_ssid, sizeof(newEd.wifi_ssid));
          got = true;
          break;
        }
      }
      delay(100);
    }
    if (!got) {
      Serial.println("[SETUP] 시간초과.");
      return false;
    }
  }

  Serial.println("[SETUP] WiFi 비밀번호(30초):");
  {
    unsigned long st = millis();
    bool got = false;
    while (millis() - st < 30000) {
      if (Serial.available()) {
        String s_wifi_password = Serial.readStringUntil('\n');
        s_wifi_password.trim();
        s_wifi_password.toCharArray(newEd.wifi_password, sizeof(newEd.wifi_password));
        got = true;
        break;
      }
      delay(100);
    }
    if (!got) {
      Serial.println("[SETUP] 시간초과.");
      return false;
    }
  }

  Serial.println("[SETUP] Telegram Chat ID(30초):");
  {
    unsigned long st = millis();
    bool got = false;
    while (millis() - st < 30000) {
      if (Serial.available()) {
        String s_telegram_id = Serial.readStringUntil('\n');
        s_telegram_id.trim();
        s_telegram_id.toCharArray(newEd.telegram_id, sizeof(newEd.telegram_id));
        got = true;
        break;
      }
      delay(100);
    }
    if (!got) {
      Serial.println("[SETUP] 시간초과.");
      return false;
    }
  }

  Serial.println("[SETUP] Telegram BOT Token(30초):");
  {
    unsigned long st = millis();
    bool got = false;
    while (millis() - st < 30000) {
      if (Serial.available()) {
        String s_telegram_token = Serial.readStringUntil('\n');
        s_telegram_token.trim();
        s_telegram_token.toCharArray(newEd.telegram_token, sizeof(newEd.telegram_token));
        got = true;
        break;
      }
      delay(100);
    }
    if (!got) {
      Serial.println("[SETUP] 시간초과.");
      return false;
    }
  }

  newEd.eprom_good = CheckNumber;

  if (newEd.servo_on_angle == 0 && newEd.servo_off_angle == 0) {
    newEd.servo_on_angle = SERVO_ON_ANGLE;
    newEd.servo_off_angle = SERVO_OFF_ANGLE;
  }

  if (strlen(newEd.ap_name) == 0) {
    setAPSSIDwithMAC(newEd.ap_name, sizeof(newEd.ap_name));
  }

  do_eprom_write(newEd);
  ed = newEd;
  Serial.println("[SETUP] 설정 저장 완료.");

  // WiFi 연결 시도
  WiFi.begin(ed.wifi_ssid, ed.wifi_password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] 시리얼 정보로 연결 성공");
    return true;
  } else {
    Serial.println("[WiFi] 시리얼 정보로 연결 실패. WiFiManager 전환");
    return false;
  }
}

bool enterTelegramCredentials() {
  eprom_data newEd;
  do_eprom_read(newEd);

  Serial.println("[봇 설정] Telegram Chat ID를 입력하세요 (30초 대기):");
  {
    unsigned long st = millis();
    bool got = false;
    while (millis() - st < 30000) {
      if (Serial.available()) {
        String s_telegram_id = Serial.readStringUntil('\n');
        s_telegram_id.trim();
        if (s_telegram_id.length() > 0) {
          s_telegram_id.toCharArray(newEd.telegram_id, sizeof(newEd.telegram_id));
          got = true;
          break;
        }
      }
      delay(100);
    }
    if (!got) {
      Serial.println("[봇 설정] 시간 초과로 변경 취소");
      return false;
    }
  }

  Serial.println("[봇 설정] Telegram BOT Token을 입력하세요 (30초 대기):");
  {
    unsigned long st = millis();
    bool got = false;
    while (millis() - st < 30000) {
      if (Serial.available()) {
        String s_telegram_token = Serial.readStringUntil('\n');
        s_telegram_token.trim();
        if (s_telegram_token.length() > 0) {
          s_telegram_token.toCharArray(newEd.telegram_token, sizeof(newEd.telegram_token));
          got = true;
          break;
        }
      }
      delay(100);
    }
    if (!got) {
      Serial.println("[봇 설정] 시간 초과로 변경 취소");
      return false;
    }
  }

  do_eprom_write(newEd);  // EEPROM에 반영
  ed = newEd;             // 전역 ed에도 반영
  Serial.println("[봇 설정] 봇 정보 저장 완료");
  return true;
}

void saveParamCallback() {
  eprom_data newEd;
  do_eprom_read(newEd);

  bool updated = false;

  String ssid = WiFi.SSID();
  String pass = WiFi.psk();
  ssid.toCharArray(newEd.wifi_ssid, sizeof(newEd.wifi_ssid));
  pass.toCharArray(newEd.wifi_password, sizeof(newEd.wifi_password));

  if (wm.server->hasArg("chat_id")) {
    String schat_id = wm.server->arg("chat_id");
    schat_id.toCharArray(newEd.telegram_id, sizeof(newEd.telegram_id));
    updated = true;
  }
  if (wm.server->hasArg("BOTtoken")) {
    String sBOTtoken = wm.server->arg("BOTtoken");
    sBOTtoken.toCharArray(newEd.telegram_token, sizeof(newEd.telegram_token));
    updated = true;
  }

  newEd.eprom_good = CheckNumber;

  if (newEd.servo_on_angle == 0 && newEd.servo_off_angle == 0) {
    newEd.servo_on_angle = SERVO_ON_ANGLE;
    newEd.servo_off_angle = SERVO_OFF_ANGLE;
  }

  // ***** ap_name은 이미 저장되어 있을 것으로 간주 *****

  if (updated) do_eprom_write(newEd);
  ed = newEd;
  Serial.println("[WiFiManager] 설정 저장 완료");
}

bool init_wifi() {
  // EEPROM에서 읽기
  do_eprom_read(ed);

  servoOnAngle = ed.servo_on_angle;
  servoOffAngle = ed.servo_off_angle;

  // ***** AP 이름 세팅 로직 *****
  if (strlen(ed.ap_name) == 0) {
    // eeprom에 ap_name이 없다면 생성
    setAPSSIDwithMAC(ed.ap_name, sizeof(ed.ap_name));
    do_eprom_write(ed);
  }
  // 전역 String devstr에 저장
  devstr = String(ed.ap_name);
  Serial.print("[Main] AP Name: ");
  Serial.println(devstr);

  bool haveCred = (strlen(ed.wifi_ssid) > 0 && strlen(ed.wifi_password) > 0
                   && strlen(ed.telegram_id) > 0 && strlen(ed.telegram_token) > 0);
  bool wifiConnected = false;

  if (haveCred) {
    Serial.println("[Main] EEPROM 설정 사용 시도...");
    WiFi.begin(ed.wifi_ssid, ed.wifi_password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] EEPROM 정보로 연결 성공");
      wifiConnected = true;
    } else {
      Serial.println("[Main] 초기 설정 필요. Serial(y/n)?(10초)");
      unsigned long st = millis();
      bool useSerial = false;
      while (millis() - st < 10000) {
        if (Serial.available()) {
          char c = (char)Serial.read();
          if (c == 'y' || c == 'Y') {
            useSerial = true;
            break;
          } else break;
        }
        delay(100);
      }

      if (useSerial) {
        if (!enterWiFiAndTelegramCredentials()) {
          Serial.println("[Main] 시리얼 통신 정보로 설정 실패. WiFiManager 진입");
        } else {
          if (WiFi.status() == WL_CONNECTED) wifiConnected = true;
        }
      } else {
        Serial.println("[Main] 시리얼 통신 정보로 설정 안함. WiFiManager 진입");
      }
    }
  } else {
    Serial.println("[Main] 초기 설정 필요. 시리얼 통신으로 설정 하시겠습니까?(y/n)?(10초)");
    unsigned long st = millis();
    bool useSerial = false;
    while (millis() - st < 10000) {
      if (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'y' || c == 'Y') {
          useSerial = true;
          break;
        } else break;
      }
      delay(100);
    }

    if (useSerial) {
      if (!enterWiFiAndTelegramCredentials()) {
        Serial.println("[Main] 시리얼 통신 정보로 설정 실패. WiFiManager 진입");
      } else {
        if (WiFi.status() == WL_CONNECTED) wifiConnected = true;
      }
    } else {
      Serial.println("[Main] 시리얼 통신 정보로 설정 안함. WiFiManager 진입");
    }
  }

  if (!wifiConnected) {
    char def_chat_id[32];
    strncpy(def_chat_id, ed.telegram_id, sizeof(def_chat_id) - 1);
    def_chat_id[sizeof(def_chat_id) - 1] = '\0';
    char def_BOTtoken[64];
    strncpy(def_BOTtoken, ed.telegram_token, sizeof(def_BOTtoken) - 1);
    def_BOTtoken[sizeof(def_BOTtoken) - 1] = '\0';

    WiFiManagerParameter t_id("chat_id", "Telegram Chat ID", def_chat_id, sizeof(def_chat_id));
    WiFiManagerParameter t_token("BOTtoken", "Telegram BOT Token", def_BOTtoken, sizeof(def_BOTtoken));
    wm.addParameter(&t_id);
    wm.addParameter(&t_token);

    wm.setSaveParamsCallback(saveParamCallback);
    wm.setConnectTimeout(30);
    wm.setConfigPortalTimeout(300);

    Serial.println("[WiFiManager] WiFiManager 포털 진입...");
    if (!wm.autoConnect(devstr.c_str())) {
      Serial.println("[WiFiManager] WiFiManager로 연결 실패. 재부팅...");
      delay(1000);
      ESP.restart();
    } else {
      Serial.println("[WiFiManager] WiFiManager로 연결 성공");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    telegramBotToken = String(ed.telegram_token);
    telegramChatId = String(ed.telegram_id);
    if (strlen(ed.telegram_root_cert) > 0) {
      clientTCP.setCACert(ed.telegram_root_cert);
      Serial.println("[Telegram] EEPROM에 저장된 루트 인증서 사용");
    } else {
      clientTCP.setCACert(TELEGRAM_CERTIFICATE_ROOT);
      Serial.println("[Telegram] 기본 루트 인증서 사용");
    }

    bool botInit = false;
    for (int i = 0; i < 3 && !botInit; i++) {
      bot = new UniversalTelegramBot(telegramBotToken, clientTCP);
      if (bot->getMe()) {
        Serial.println("[Telegram] 봇 초기화 성공");
        botInit = true;
        pendingMessageText = "스위치 원격제어\n명령어:\n/on - 스위치ON(현재ON각도:" + String(servoOnAngle) + ")\n/off - 스위치OFF(현재OFF각도:" + String(servoOffAngle) + ")\n/set_on - ON각도 설정\n/set_off - OFF각도 설정\n/start - 메뉴 다시보기\n";
        pendingMessageChatId = telegramChatId;
        messageneed = 1;
      } else {
        Serial.println("[Telegram] 봇 초기화 실패. 재시도...");
        delete bot;
        bot = nullptr;
        delay(1000);
      }
    }
    if (!botInit) {
      Serial.println("[Telegram] 봇 초기화 실패. 정보 다시 입력 필요");
      Serial.println("[Main] 봇 재설정 필요. 시리얼 통신으로 설정 하시겠습니까?(y/n)?(10초)");
      bool doReEnter = false;
      unsigned long promptStart = millis();
      while (millis() - promptStart < 10000) {  // 10초간 대기
        if (Serial.available()) {
          char c = (char)Serial.read();
          if (c == 'y' || c == 'Y') {
            doReEnter = true;
            break;
          } else {
            // 'y'가 아니라면 그 즉시 빠져나옴
            break;
          }
        }
        delay(50);
      }

      if (doReEnter) {
        // 텔레그램 정보만 재입력
        enterTelegramCredentials();

        // EEPROM에 새로 저장된 봇 정보를 다시 읽어와 bot 재초기화
        if (bot) {
          delete bot;
          bot = nullptr;
        }
        telegramBotToken = String(ed.telegram_token);
        if (strlen(ed.telegram_root_cert) > 0) {
          clientTCP.setCACert(ed.telegram_root_cert);
        }
        bot = new UniversalTelegramBot(telegramBotToken, clientTCP);

        // 재시도
        if (bot->getMe()) {
          Serial.println("[Comm_Task] 새 봇 정보로 연결 성공");
        } else {
          Serial.println("[Comm_Task] 새 봇 정보 연결 실패");
          ESP.restart();
        }
      }
    }
  } else {
    Serial.println("[Main] WiFi 연결 실패. 정보 다시 입력 필요");
    Serial.println("[Main] 재설정 필요. 시리얼 통신으로 설정 하시겠습니까?(y/n)?(10초)");
    unsigned long st = millis();
    bool useSerial = false;
    while (millis() - st < 10000) {
      if (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'y' || c == 'Y') {
          useSerial = true;
          break;
        } else break;
      }
      delay(100);
    }

    if (useSerial) {
      if (!enterWiFiAndTelegramCredentials()) {
        Serial.println("[Main] 시리얼 통신 정보로 설정 실패. WiFiManager 진입");
      } else {
        if (WiFi.status() == WL_CONNECTED) wifiConnected = true;
      }
    } else {
      Serial.println("[Main] 시리얼 통신 정보로 설정 안함. WiFiManager 진입");
    }
  }
  return (WiFi.status() == WL_CONNECTED);
}

SemaphoreHandle_t xMutex;

void handleNewMessages(int numNewMessages) {
  int highestUpdateID = -1;
  for (int i = 0; i < numNewMessages; i++) {
    int update_id = bot->messages[i].update_id;
    String incoming_id = String(bot->messages[i].chat_id);
    String text = bot->messages[i].text;

    xSemaphoreTake(xMutex, portMAX_DELAY);
    if (incoming_id != telegramChatId) {
      pendingMessageChatId = incoming_id;
      pendingMessageText = "권한없음. bot 관련 정보가 잘못 입력 되었습니다.";
      Serial.println("[Main] 봇 재설정 필요. 시리얼 통신으로 설정 하시겠습니까?(y/n)?(10초)");
      messageneed = 1;
      bool doReEnter = false;
      unsigned long promptStart = millis();
      while (millis() - promptStart < 10000) {  // 10초간 대기
        if (Serial.available()) {
          char c = (char)Serial.read();
          if (c == 'y' || c == 'Y') {
            doReEnter = true;
            break;
          } else {
            // 'y'가 아니라면 그 즉시 빠져나옴
            break;
          }
        }
        delay(50);
      }

      if (doReEnter) {
        // 텔레그램 정보만 재입력
        enterTelegramCredentials();

        // EEPROM에 새로 저장된 봇 정보를 다시 읽어와 bot 재초기화
        if (bot) {
          delete bot;
          bot = nullptr;
        }
        telegramBotToken = String(ed.telegram_token);
        if (strlen(ed.telegram_root_cert) > 0) {
          clientTCP.setCACert(ed.telegram_root_cert);
        }
        bot = new UniversalTelegramBot(telegramBotToken, clientTCP);

        // 재시도
        if (bot->getMe()) {
          Serial.println("[Comm_Task] 새 봇 정보로 연결 성공");
        } else {
          Serial.println("[Comm_Task] 새 봇 정보 연결 실패");
          ESP.restart();
        }
      }

    } else {
      Serial.printf("[Telegram] 수신: %s\n", text.c_str());

      if (awaitingOnAngle || awaitingOffAngle) {
        bool isNumber = true;
        for (uint16_t idx = 0; idx < text.length(); idx++) {
          if (!isdigit((unsigned char)text.charAt(idx))) {
            isNumber = false;
            break;
          }
        }

        if (isNumber && text.length() > 0) {
          int newAngle = text.toInt();
          if (newAngle >= 0 && newAngle <= 180) {
            if (awaitingOnAngle) {
              servoOnAngle = newAngle;
              pendingMessageText = "ON각도 설정: " + String(servoOnAngle) + "도";
            } else {
              servoOffAngle = newAngle;
              pendingMessageText = "OFF각도 설정: " + String(servoOffAngle) + "도";
            }

            eprom_data newEd;
            do_eprom_read(newEd);
            newEd.servo_on_angle = servoOnAngle;
            newEd.servo_off_angle = servoOffAngle;
            do_eprom_write(newEd);

          } else {
            pendingMessageText = "유효한 각도가 아닙니다. (0~180 범위)";
          }
        } else {
          pendingMessageText = "숫자만 입력해주세요.";
        }
        awaitingOnAngle = false;
        awaitingOffAngle = false;
        pendingMessageChatId = telegramChatId;
        messageneed = 1;

      } else {
        if (text == "/start") {
          pendingMessageText = "스위치 원격제어\n명령어:\n/on - 스위치ON(현재ON각도:" + String(servoOnAngle) + ")\n/off - 스위치OFF(현재OFF각도:" + String(servoOffAngle) + ")\n/set_on - ON각도 설정\n/set_off - OFF각도 설정\n/start - 메뉴 다시보기\n";
        } else if (text == "/on") {
          desiredAngle = servoOnAngle;
          pendingMessageText = "스위치 ON";
        } else if (text == "/off") {
          desiredAngle = servoOffAngle;
          pendingMessageText = "스위치 OFF";
        } else if (text == "/set_on") {
          awaitingOnAngle = true;
          awaitingOffAngle = false;
          pendingMessageText = "ON 상태의 각도를 숫자로만 입력해주세요.";
        } else if (text == "/set_off") {
          awaitingOffAngle = true;
          awaitingOnAngle = false;
          pendingMessageText = "OFF 상태의 각도를 숫자로만 입력해주세요.";
        } else {
          pendingMessageText = "알 수 없는 명령어 입니다.";
        }
        pendingMessageChatId = telegramChatId;
        messageneed = 1;
      }
    }
    xSemaphoreGive(xMutex);

    if (update_id > highestUpdateID) {
      highestUpdateID = update_id;
    }
  }

  if (highestUpdateID != -1) {
    offset = highestUpdateID + 1;
  }
}

// Comm_Task 부분만 발췌, 수정 예시

void Comm_Task(void *pvParameters) {
  // 통신 타임아웃을 원한다면 주석 해제 후 사용
  // clientTCP.setTimeout(2000);

  // --- 추가: 끊긴 상태 메시지 및 재연결 주기를 관리하기 위한 static 변수 ---
  static unsigned long lastDisconnectMsg = 0;     // 마지막에 끊김 메시지를 보낸 시각
  static unsigned long lastReconnectAttempt = 0;  // 마지막에 재연결 시도한 시각

  // 원하는 간격 (단위: ms)
  const unsigned long DISCONNECT_MSG_INTERVAL = 3000;  // 3초
  const unsigned long RECONNECT_INTERVAL = 5000;       // 5초

  while (1) {
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd.equalsIgnoreCase("reset")) {
        Serial.println("[Main] reset 명령 수신 -> EEPROM 및 WiFi 설정 초기화 후 재부팅");
        resetSettings();
      }
    }

    if (bot && WiFi.status() == WL_CONNECTED) {
      unsigned long now = millis();

      // botRequestDelay 간격마다 getUpdates() 시도
      if (now - lastTimeBotRan >= botRequestDelay) {
        int numNewMessages = bot->getUpdates(offset);
        vTaskDelay(10 / portTICK_PERIOD_MS);

        if (numNewMessages < 0) {
          Serial.println("[Comm_Task] WiFi가 끊겼거나 불안정합니다. (getUpdates() 실패)");
        } else if (numNewMessages > 0) {
          handleNewMessages(numNewMessages);
        }

        if (messageneed == 1) {
          messageneed = 0;
          bool sendOK = bot->sendMessage(pendingMessageChatId, pendingMessageText, "");
          vTaskDelay(10 / portTICK_PERIOD_MS);

          if (!sendOK) {
            Serial.println("[Comm_Task] WiFi가 끊겼거나 불안정합니다. (sendMessage() 실패)");
          }
        }

        lastTimeBotRan = now;
      }

    } else {
      // --- WiFi 연결 안 됨 (또는 bot == nullptr) ---

      // (1) 3초에 한 번씩만 끊김 메시지
      unsigned long now = millis();
      if (now - lastDisconnectMsg >= DISCONNECT_MSG_INTERVAL) {
        Serial.println("[Comm_Task] WiFi가 끊겼거나 불안정합니다.");
        lastDisconnectMsg = now;
      }

      if (now - lastReconnectAttempt >= RECONNECT_INTERVAL) {
        lastReconnectAttempt = now;

        // 먼저 연결 끊고 다시 연결
        WiFi.disconnect();
        WiFi.begin(ed.wifi_ssid, ed.wifi_password);

        Serial.println("[Comm_Task] WiFi 재연결 시도...");

        // (옵션) 여기서 약간 대기하면서 연결 여부 확인 (최대 5초 예시)
        unsigned long startWait = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startWait < 5000) {
          delay(100);
        }

        // 이제 WiFi 연결 결과 체크
        if (WiFi.status() == WL_CONNECTED) {
          // bot이 아직 없으면 새로 생성
          if (!bot) {
            bot = new UniversalTelegramBot(telegramBotToken, clientTCP);
          }

          // getMe()로 봇 연결 테스트
          if (bot->getMe()) {
            Serial.println("[Comm_Task] WiFi와 봇 재연결 성공");
          } else {
            // 봇 정보만 다시 입력할지 물어봄
            Serial.println("[Comm_Task] WiFi는 연결되었으나, Telegram 봇 연결 실패. 봇 정보만 다시 입력하시겠습니까? (y/n)");

            bool doReEnter = false;
            unsigned long promptStart = millis();
            while (millis() - promptStart < 10000) {  // 10초간 대기
              if (Serial.available()) {
                char c = (char)Serial.read();
                if (c == 'y' || c == 'Y') {
                  doReEnter = true;
                  break;
                } else {
                  // 'y'가 아니라면 그 즉시 빠져나옴
                  break;
                }
              }
              delay(50);
            }

            if (doReEnter) {
              // 텔레그램 정보만 재입력
              enterTelegramCredentials();

              // EEPROM에 새로 저장된 봇 정보를 다시 읽어와 bot 재초기화
              if (bot) {
                delete bot;
                bot = nullptr;
              }
              telegramBotToken = String(ed.telegram_token);
              if (strlen(ed.telegram_root_cert) > 0) {
                clientTCP.setCACert(ed.telegram_root_cert);
              }
              bot = new UniversalTelegramBot(telegramBotToken, clientTCP);

              // 재시도
              if (bot->getMe()) {
                Serial.println("[Comm_Task] 새 봇 정보로 연결 성공");
              } else {
                Serial.println("[Comm_Task] 새 봇 정보 연결 실패.");
              }
            }
          }

        } else {
          Serial.println("[Comm_Task] WiFi 재연결 실패");
        }
      }

      // 상태 변화는 약간의 시간이 필요하므로 잠시 대기
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    // 메인 반복 주기 제한 + Idle Task 시간 확보
    vTaskDelay(50 / portTICK_PERIOD_MS);
    delay(1);
  }

  // 이 태스크가 끝날 일은 없지만, 관례적으로
  vTaskDelete(NULL);
}

void Servo_Task(void *pvParameters) {
  while (1) {
    xSemaphoreTake(xMutex, portMAX_DELAY);
    int angleCopy = desiredAngle;
    xSemaphoreGive(xMutex);

    if (angleCopy != currentAngle) {
      currentAngle = angleCopy;
      myServo.write(angleCopy);
      myServo.attach(SERVO_PIN);
      myServo.write(angleCopy);
      Serial.printf("[Servo_Task] 서보 각도 변경: %d\n", angleCopy);
      delay(300);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
    myServo.detach();
  }
  vTaskDelete(NULL);
  delay(1);
}

void setup() {
  Serial.begin(115200);
  Serial.println("[Main] ESP32 스위치 컨트롤러 시작...");
  Serial.println("\n사용 가능한 명령어:");
  Serial.println(" RESET - 기본 설정으로 재설정");

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  EEPROM.begin(sizeof(eprom_data));

  xMutex = xSemaphoreCreateMutex();

  // WiFi + Telegram 봇 초기화
  init_wifi();
  myServo.attach(SERVO_PIN);

  xSemaphoreTake(xMutex, portMAX_DELAY);
  currentAngle = 100;
  desiredAngle = 100;
  xSemaphoreGive(xMutex);

  // 통신 처리(텔레그램) 태스크
  xTaskCreatePinnedToCore(
    Comm_Task,
    "CommTask",
    8192,
    NULL,
    1,
    NULL,
    0);

  // 서보 제어 태스크
  xTaskCreatePinnedToCore(
    Servo_Task,
    "ServoTask",
    4096,
    NULL,
    1,
    NULL,
    1);
}

void loop() {
  // 사용하지 않음
}
