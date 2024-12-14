#include <EEPROM.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "UniversalTelegramBot.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <ESP32Servo.h>

// Telegram 루트 인증서 (실제 PEM 필요)
extern const char TELEGRAM_CERTIFICATE_ROOT[] PROGMEM;

#define SERVO_PIN 4
#define SERVO_ON_ANGLE 80
#define SERVO_OFF_ANGLE 110

struct eprom_data {
  char wifi_ssid[32];
  char wifi_password[64];
  char telegram_id[32];
  char telegram_token[64];
  char telegram_root_cert[1024];
  int eprom_good;
  int servo_on_angle;
  int servo_off_angle;
};

const int CheckNumber = 123;
eprom_data ed;

WiFiClientSecure clientTCP;
UniversalTelegramBot* bot = nullptr;
String telegramChatId;
String telegramBotToken;

WiFiManager wm;
Servo myServo;

int servoOnAngle = SERVO_ON_ANGLE;
int servoOffAngle = SERVO_OFF_ANGLE;
int currentAngle = 110;
int desiredAngle = 110;

bool awaitingOnAngle = false;
bool awaitingOffAngle = false;

unsigned long botRequestDelay = 1000;
unsigned long lastTimeBotRan = 0;

String randomNum = String(esp_random());
String devstr = "ESP32-" + randomNum.substring(0,4);

SemaphoreHandle_t xMutex;

// offset 사용
int offset = 0;

// 메시지 전송 관리 변수
int messageneed = 0;             // 0: 전송 필요없음, 1: 전송 필요
String pendingMessageText = "";  // 전송할 메시지 내용
String pendingMessageChatId = ""; 

bool isDataDifferent(const eprom_data &newData, const eprom_data &oldData){
  if(newData.eprom_good!=oldData.eprom_good)return true;
  if(strcmp(newData.wifi_ssid,oldData.wifi_ssid)!=0)return true;
  if(strcmp(newData.wifi_password,oldData.wifi_password)!=0)return true;
  if(strcmp(newData.telegram_id,oldData.telegram_id)!=0)return true;
  if(strcmp(newData.telegram_token,oldData.telegram_token)!=0)return true;
  if(strcmp(newData.telegram_root_cert,oldData.telegram_root_cert)!=0)return true;
  if(newData.servo_on_angle != oldData.servo_on_angle)return true;
  if(newData.servo_off_angle != oldData.servo_off_angle)return true;
  return false;
}

void do_eprom_write(const eprom_data &newData){
  eprom_data currentData;
  EEPROM.begin(sizeof(eprom_data));
  EEPROM.get(0,currentData);
  EEPROM.end();

  if(isDataDifferent(newData,currentData)){
    Serial.println("[EEPROM] 데이터 변경 감지. 저장...");
    EEPROM.begin(sizeof(eprom_data));
    EEPROM.put(0,newData);
    EEPROM.commit();
    EEPROM.end();
    Serial.println("[EEPROM] 저장 완료.");
  }
}

void do_eprom_read(eprom_data &eed){
  EEPROM.begin(sizeof(eprom_data));
  EEPROM.get(0,eed);
  EEPROM.end();

  if(eed.eprom_good==CheckNumber){
    if(eed.servo_on_angle == 0 && eed.servo_off_angle == 0) {
      eed.servo_on_angle = SERVO_ON_ANGLE;
      eed.servo_off_angle = SERVO_OFF_ANGLE;
      do_eprom_write(eed);
    }
  } else {
    Serial.println("[EEPROM] 유효한 설정 없음. 초기화.");
    memset(&eed,0,sizeof(eed));
    eed.eprom_good=CheckNumber;
    eed.servo_on_angle = SERVO_ON_ANGLE;
    eed.servo_off_angle = SERVO_OFF_ANGLE;
    do_eprom_write(eed);
  }
}

void resetSettings(){
  eprom_data emptyData;
  memset(&emptyData,0,sizeof(emptyData));
  emptyData.eprom_good=0;
  do_eprom_write(emptyData);
  Serial.println("[EEPROM] 설정 초기화 완료.");

  wm.resetSettings();
  Serial.println("[WiFiManager] 기존 WiFi 초기화. 재시작.");
  delay(1000);
  ESP.restart();
}

void enterTelegramRootCert(){
  Serial.println("[CERT] Telegram 루트 인증서 입력(END 종료):");
  String cert="";
  while(true){
    if(Serial.available()){
      String line=Serial.readStringUntil('\n');
      line.trim();
      if(line.equalsIgnoreCase("END"))break;
      cert+=line+"\n";
      Serial.println("[CERT] 인증서 수신...");
    }
    delay(100);
  }

  if(cert.length()>0){
    eprom_data newEd;
    do_eprom_read(newEd);
    strncpy(newEd.telegram_root_cert,cert.c_str(),sizeof(newEd.telegram_root_cert)-1);
    newEd.telegram_root_cert[sizeof(newEd.telegram_root_cert)-1]='\0';
    do_eprom_write(newEd);
    Serial.println("[CERT] 루트 인증서 저장 완료. 재시작.");
    ESP.restart();
  } else {
    Serial.println("[CERT] 인증서 없음. 재시작.");
    ESP.restart();
  }
}

bool enterWiFiAndTelegramCredentials(){
  eprom_data newEd;
  do_eprom_read(newEd);

  Serial.println("[SETUP] WiFi SSID(30초):");
  {
    unsigned long st=millis();bool got=false;
    while(millis()-st<30000){
      if(Serial.available()){
        String s_wifi_ssid=Serial.readStringUntil('\n');
        s_wifi_ssid.trim();
        if(s_wifi_ssid.length()>0){
          s_wifi_ssid.toCharArray(newEd.wifi_ssid,sizeof(newEd.wifi_ssid));
          got=true;break;
        }
      }
      delay(100);
    }
    if(!got){Serial.println("[SETUP] 시간초과.");return false;}
  }

  Serial.println("[SETUP] WiFi 비밀번호(30초):");
  {
    unsigned long st=millis();bool got=false;
    while(millis()-st<30000){
      if(Serial.available()){
        String s_wifi_password=Serial.readStringUntil('\n');
        s_wifi_password.trim();
        s_wifi_password.toCharArray(newEd.wifi_password,sizeof(newEd.wifi_password));
        got=true;break;
      }
      delay(100);
    }
    if(!got){Serial.println("[SETUP] 시간초과.");return false;}
  }

  Serial.println("[SETUP] Telegram Chat ID(30초):");
  {
    unsigned long st=millis();bool got=false;
    while(millis()-st<30000){
      if(Serial.available()){
        String s_telegram_id=Serial.readStringUntil('\n');
        s_telegram_id.trim();
        s_telegram_id.toCharArray(newEd.telegram_id,sizeof(newEd.telegram_id));
        got=true;break;
      }
      delay(100);
    }
    if(!got){Serial.println("[SETUP] 시간초과.");return false;}
  }

  Serial.println("[SETUP] Telegram BOT Token(30초):");
  {
    unsigned long st=millis();bool got=false;
    while(millis()-st<30000){
      if(Serial.available()){
        String s_telegram_token=Serial.readStringUntil('\n');
        s_telegram_token.trim();
        s_telegram_token.toCharArray(newEd.telegram_token,sizeof(newEd.telegram_token));
        got=true;break;
      }
      delay(100);
    }
    if(!got){Serial.println("[SETUP] 시간초과.");return false;}
  }

  newEd.eprom_good=CheckNumber;

  if(newEd.servo_on_angle == 0 && newEd.servo_off_angle == 0) {
    newEd.servo_on_angle = SERVO_ON_ANGLE;
    newEd.servo_off_angle = SERVO_OFF_ANGLE;
  }

  do_eprom_write(newEd);
  ed=newEd;
  Serial.println("[SETUP] 설정 저장 완료.");

  WiFi.begin(ed.wifi_ssid,ed.wifi_password);
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){
    delay(500);Serial.print(".");
  }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.println("[WiFi] 시리얼 정보로 연결 성공!");
    return true;
  } else {
    Serial.println("[WiFi] 시리얼 정보로 연결 실패. WiFiManager 전환.");
    return false;
  }
}

void saveParamCallback(){
  eprom_data newEd;
  do_eprom_read(newEd);

  bool updated=false;

  String ssid=WiFi.SSID();
  String pass=WiFi.psk();
  ssid.toCharArray(newEd.wifi_ssid,sizeof(newEd.wifi_ssid));
  pass.toCharArray(newEd.wifi_password,sizeof(newEd.wifi_password));

  if(wm.server->hasArg("chat_id")){
    String schat_id=wm.server->arg("chat_id");
    schat_id.toCharArray(newEd.telegram_id,sizeof(newEd.telegram_id));
    updated=true;
  }
  if(wm.server->hasArg("BOTtoken")){
    String sBOTtoken=wm.server->arg("BOTtoken");
    sBOTtoken.toCharArray(newEd.telegram_token,sizeof(newEd.telegram_token));
    updated=true;
  }

  newEd.eprom_good=CheckNumber;

  if(newEd.servo_on_angle == 0 && newEd.servo_off_angle == 0) {
    newEd.servo_on_angle = SERVO_ON_ANGLE;
    newEd.servo_off_angle = SERVO_OFF_ANGLE;
  }

  if(updated) do_eprom_write(newEd);
  ed=newEd;
  Serial.println("[WiFiManager] 설정 저장 완료.");
}

bool init_wifi(){
  do_eprom_read(ed);

  servoOnAngle = ed.servo_on_angle;
  servoOffAngle = ed.servo_off_angle;

  bool haveCred=(strlen(ed.wifi_ssid)>0 && strlen(ed.wifi_password)>0 && strlen(ed.telegram_id)>0 && strlen(ed.telegram_token)>0);
  bool wifiConnected=false;

  if(haveCred){
    Serial.println("[Main] EEPROM 설정 사용 시도...");
    WiFi.begin(ed.wifi_ssid,ed.wifi_password);
    unsigned long start=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){delay(500);Serial.print(".");}
    Serial.println();
    if(WiFi.status()==WL_CONNECTED){
      Serial.println("[WiFi] EEPROM 정보로 연결 성공!");
      wifiConnected=true;
    } else {
      Serial.println("[WiFi] EEPROM 정보 실패. WiFiManager 전환.");
    }
  } else {
    Serial.println("[Main] 초기 설정 필요. Serial(y/n)?(10초)");
    unsigned long st=millis();
    bool useSerial=false;
    while(millis()-st<10000){
      if(Serial.available()){
        char c=(char)Serial.read();
        if(c=='y'||c=='Y'){useSerial=true;break;}
        else break;
      }
      delay(100);
    }

    if(useSerial){
      if(!enterWiFiAndTelegramCredentials()){
        Serial.println("[Main] 시리얼 실패. WiFiManager.");
      } else {
        if(WiFi.status()==WL_CONNECTED) wifiConnected=true;
      }
    } else {
      Serial.println("[Main] 시리얼 모드 안함. WiFiManager.");
    }
  }

  if(!wifiConnected){
    char def_chat_id[32];strncpy(def_chat_id,ed.telegram_id,sizeof(def_chat_id)-1);def_chat_id[sizeof(def_chat_id)-1]='\0';
    char def_BOTtoken[64];strncpy(def_BOTtoken,ed.telegram_token,sizeof(def_BOTtoken)-1);def_BOTtoken[sizeof(def_BOTtoken)-1]='\0';

    WiFiManagerParameter t_id("chat_id","Telegram Chat ID",def_chat_id,sizeof(def_chat_id));
    WiFiManagerParameter t_token("BOTtoken","Telegram BOT Token",def_BOTtoken,sizeof(def_BOTtoken));
    wm.addParameter(&t_id);
    wm.addParameter(&t_token);

    wm.setSaveParamsCallback(saveParamCallback);
    wm.setConnectTimeout(30);
    wm.setConfigPortalTimeout(300);

    Serial.println("[WiFiManager] 포털 진입...");
    if(!wm.autoConnect(devstr.c_str())){
      Serial.println("[WiFiManager] 연결 실패. 재부팅...");
      delay(1000);
      ESP.restart();
    } else {
      Serial.println("[WiFiManager] 연결 성공.");
    }
  }

  if(WiFi.status()==WL_CONNECTED){
    telegramBotToken=String(ed.telegram_token);
    telegramChatId=String(ed.telegram_id);
    if(strlen(ed.telegram_root_cert)>0) {
      clientTCP.setCACert(ed.telegram_root_cert);
      Serial.println("[Telegram] EEPROM 루트 인증서 사용.");
    } else {
      clientTCP.setCACert(TELEGRAM_CERTIFICATE_ROOT);
      Serial.println("[Telegram] 기본 루트 인증서 사용.");
    }

    bool botInit=false;
    for(int i=0;i<3&&!botInit;i++){
      bot=new UniversalTelegramBot(telegramBotToken,clientTCP);
      if(bot->getMe()){
        Serial.println("[Telegram] 봇 초기화 성공.");
        botInit=true;
        // 메시지도 messageneed방식으로 보내도 되지만 여기서는 첫 메시지는 한번만!
        pendingMessageText = "스위치 원격제어\n명령어:\n/on - 스위치ON(현재ON각도:"+String(servoOnAngle)+")\n/off - 스위치OFF(현재OFF각도:"+String(servoOffAngle)+")\n/set_on - ON각도 설정\n/set_off - OFF각도 설정\n/start - 메뉴 다시보기\n";
        pendingMessageChatId = telegramChatId;
        messageneed = 1;
      } else {
        Serial.println("[Telegram] 봇 초기화 실패. 재시도...");
        delete bot;bot=nullptr;
        delay(1000);
      }
    }

    if(!botInit) {
      Serial.println("[Telegram] 봇 초기화 실패. 루트 인증서 입력유도.");
      enterTelegramRootCert();
    }
  } else {
    Serial.println("[Main] WiFi 연결 안됨. 재부팅...");
    delay(1000);
    ESP.restart();
  }

  return (WiFi.status()==WL_CONNECTED);
}


void handleNewMessages(int numNewMessages){
  int highestUpdateID = -1;
  for(int i=0;i<numNewMessages;i++){
    int update_id = bot->messages[i].update_id;
    String incoming_id=String(bot->messages[i].chat_id);
    String text=bot->messages[i].text;

    // 일반적으로 offset 사용으로 이미 처리한 메시지는 안 들어옴
    // 굳이 중복 체크 필요 없음.

    xSemaphoreTake(xMutex, portMAX_DELAY);
    if(incoming_id!=telegramChatId){
      // messageneed를 사용
      pendingMessageChatId = incoming_id;
      pendingMessageText = "권한없음";
      messageneed = 1;
    } else {
      Serial.printf("[Telegram] 수신: %s\n",text.c_str());

      if(awaitingOnAngle || awaitingOffAngle) {
        bool isNumber=true;
        for (uint16_t idx=0; idx<text.length(); idx++){
          if(!isdigit((unsigned char)text.charAt(idx))){
            isNumber=false;
            break;
          }
        }

        if(isNumber && text.length()>0){
          int newAngle=text.toInt();
          if(newAngle>=0 && newAngle<=180){
            if(awaitingOnAngle) {
              servoOnAngle=newAngle;
              pendingMessageText="ON각도 설정: "+String(servoOnAngle)+"도";
            } else {
              servoOffAngle=newAngle;
              pendingMessageText="OFF각도 설정: "+String(servoOffAngle)+"도";
            }

            eprom_data newEd;
            do_eprom_read(newEd);
            newEd.servo_on_angle = servoOnAngle;
            newEd.servo_off_angle = servoOffAngle;
            do_eprom_write(newEd);

          } else {
            pendingMessageText="유효한 각도가 아닙니다. (0~180 범위)";
          }
        } else {
          pendingMessageText="숫자만 입력해주세요.";
        }
        awaitingOnAngle=false;
        awaitingOffAngle=false;
        pendingMessageChatId = telegramChatId;
        messageneed = 1;

      } else {
        // 명령어 처리
        if(text=="/start"){
          pendingMessageText="스위치 원격제어\n명령어:\n/on - 스위치ON(현재ON각도:"+String(servoOnAngle)+")\n/off - 스위치OFF(현재OFF각도:"+String(servoOffAngle)+")\n/set_on - ON각도 설정\n/set_off - OFF각도 설정\n/start - 메뉴 다시보기\n";
        } else if(text=="/on"){
          desiredAngle = servoOnAngle;
          pendingMessageText="스위치 ON";
        } else if(text=="/off"){
          desiredAngle = servoOffAngle;
          pendingMessageText="스위치 OFF";
        } else if(text=="/set_on"){
          awaitingOnAngle=true;
          awaitingOffAngle=false;
          pendingMessageText="ON 상태의 각도를 숫자로만 입력해주세요.";
        } else if(text=="/set_off"){
          awaitingOffAngle=true;
          awaitingOnAngle=false;
          pendingMessageText="OFF 상태의 각도를 숫자로만 입력해주세요.";
        } else {
          pendingMessageText="알 수 없는 명령어 입니다.";
        }
        pendingMessageChatId = telegramChatId;
        messageneed = 1;
      }
    }
    xSemaphoreGive(xMutex);

    if(update_id > highestUpdateID) {
      highestUpdateID = update_id;
    }
  }

  if(highestUpdateID != -1) {
    offset = highestUpdateID + 1;
  }
}

void Comm_Task(void *pvParameters) {
  for(;;){
    if(bot && WiFi.status()==WL_CONNECTED){
      unsigned long now=millis();
      if(now > lastTimeBotRan + botRequestDelay){
        int numNewMessages=bot->getUpdates(offset);
        if(numNewMessages > 0){
          handleNewMessages(numNewMessages);
        }

        // messageneed 변수가 1이면 sendMessage 실행
        if(messageneed == 1) {
          // 먼저 messageneed를 0으로
          messageneed = 0;
          // 그 후 메시지 전송
          bot->sendMessage(pendingMessageChatId, pendingMessageText, "");
        }

        lastTimeBotRan=now;
      }
    }

    if(Serial.available()){
      String cmd=Serial.readStringUntil('\n');
      cmd.trim();
      if(cmd.equalsIgnoreCase("reset")){
        resetSettings();
      }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void Servo_Task(void *pvParameters) {
  myServo.attach(SERVO_PIN);
  myServo.write(currentAngle);

  for(;;){
    xSemaphoreTake(xMutex, portMAX_DELAY);
    int angleCopy = desiredAngle;
    xSemaphoreGive(xMutex);

    if(angleCopy != currentAngle){
      currentAngle = angleCopy;
      myServo.write(currentAngle);
      Serial.printf("[Servo_Task] 서보 각도 변경: %d\n", currentAngle);
      delay(500); 
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("[Main] ESP32 스위치 컨트롤러 시작...");
  Serial.println("[Main] AP Name: "+devstr);

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG,0);
  EEPROM.begin(sizeof(eprom_data));

  xMutex = xSemaphoreCreateMutex();

  init_wifi();

  xSemaphoreTake(xMutex, portMAX_DELAY);
  currentAngle = servoOffAngle;
  desiredAngle = servoOffAngle;
  xSemaphoreGive(xMutex);

  xTaskCreatePinnedToCore(
    Comm_Task,
    "CommTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    Servo_Task,
    "ServoTask",
    4096,
    NULL,
    1,
    NULL,
    1
  );
}

void loop(){
  // 사용하지 않음
}
