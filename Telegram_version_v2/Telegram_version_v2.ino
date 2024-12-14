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

// 기본 각도값을 변수로 유지
int servoOnAngle = SERVO_ON_ANGLE;
int servoOffAngle = SERVO_OFF_ANGLE;

int currentAngle = 110;
unsigned long lastServoMove = 0;

String s_wifi_ssid;
String s_wifi_password;
String s_telegram_id;
String s_telegram_token;

unsigned long botRequestDelay = 1000;
unsigned long lastTimeBotRan = 0;

String randomNum = String(esp_random());
String devstr = "ESP32-" + randomNum.substring(0,4);

// 각도 입력 대기 상태 변수
bool awaitingOnAngle = false;
bool awaitingOffAngle = false;

bool isDataDifferent(const eprom_data &newData, const eprom_data &oldData){
  if(newData.eprom_good != oldData.eprom_good)return true;
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
    Serial.println("[EEPROM] 유효한 설정 있음.");
    if(strlen(eed.wifi_ssid)>0 && strlen(eed.wifi_password)>0)
      Serial.printf("[EEPROM] WiFi: SSID=%s\n", eed.wifi_ssid);
    else
      Serial.println("[EEPROM] WiFi 정보 없음.");
    if(strlen(eed.telegram_id)>0 && strlen(eed.telegram_token)>0)
      Serial.println("[EEPROM] Telegram 정보 있음.");
    else
      Serial.println("[EEPROM] Telegram 정보 없음.");
    if(strlen(eed.telegram_root_cert)>0)
      Serial.println("[EEPROM] Telegram 루트 인증서 있음.");
    else
      Serial.println("[EEPROM] Telegram 루트 인증서 없음(기본 인증서 시도).");

    // 서보 각도 불러오기
    if(eed.servo_on_angle >= 0 && eed.servo_on_angle <=180) servoOnAngle = eed.servo_on_angle;
    else servoOnAngle = SERVO_ON_ANGLE;
    if(eed.servo_off_angle >=0 && eed.servo_off_angle <=180) servoOffAngle = eed.servo_off_angle;
    else servoOffAngle = SERVO_OFF_ANGLE;

    Serial.printf("[EEPROM] Servo On Angle=%d, Off Angle=%d\n", servoOnAngle, servoOffAngle);
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
        s_wifi_ssid=Serial.readStringUntil('\n');
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
        s_wifi_password=Serial.readStringUntil('\n');
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
        s_telegram_id=Serial.readStringUntil('\n');
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
        s_telegram_token=Serial.readStringUntil('\n');
        s_telegram_token.trim();
        s_telegram_token.toCharArray(newEd.telegram_token,sizeof(newEd.telegram_token));
        got=true;break;
      }
      delay(100);
    }
    if(!got){Serial.println("[SETUP] 시간초과.");return false;}
  }

  newEd.eprom_good=CheckNumber;
  // 현재 서보각도 상태 저장
  newEd.servo_on_angle = servoOnAngle;
  newEd.servo_off_angle = servoOffAngle;

  do_eprom_write(newEd);
  ed=newEd;
  Serial.println("[SETUP] 설정 저장 완료.");

  // 시리얼 입력 후 WiFi 연결 시도
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

  // 서보 각도는 현재 메모리상 값 사용
  newEd.servo_on_angle = servoOnAngle;
  newEd.servo_off_angle = servoOffAngle;

  newEd.eprom_good=CheckNumber;
  if(updated) do_eprom_write(newEd);
  ed=newEd;
  Serial.println("[WiFiManager] 설정 저장 완료.");
}

bool init_wifi(){
  do_eprom_read(ed);

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
        String welcome="스위치 원격제어\n명령어:\n/on - 스위치ON(현재ON각도:"+String(servoOnAngle)+")\n/off - 스위치OFF(현재OFF각도:"+String(servoOffAngle)+")\n/set_on - ON시 각도 설정 모드 진입\n/set_off - OFF시 각도 설정 모드 진입\n/start - 메뉴 다시보기\n";
        bot->sendMessage(telegramChatId,welcome,"");
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
  for(int i=0;i<numNewMessages;i++){
    String incoming_id=String(bot->messages[i].chat_id);
    String text=bot->messages[i].text;
    if(incoming_id!=telegramChatId){
      bot->sendMessage(incoming_id,"권한없음","");
      continue;
    }
    Serial.printf("[Telegram] 수신: %s\n",text.c_str());

    // 각도 입력 대기 상태일 경우
    if(awaitingOnAngle || awaitingOffAngle) {
      // 각도 설정 모드일 때 들어오는 메시지는 숫자만 허용
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
          eprom_data newEd;
          do_eprom_read(newEd);

          if(awaitingOnAngle) {
            servoOnAngle=newAngle;
            bot->sendMessage(telegramChatId,"ON각도 설정: "+String(servoOnAngle)+"도","");
            newEd.servo_on_angle = servoOnAngle;
          } else {
            servoOffAngle=newAngle;
            bot->sendMessage(telegramChatId,"OFF각도 설정: "+String(servoOffAngle)+"도","");
            newEd.servo_off_angle = servoOffAngle;
          }

          // 변경사항 있을 경우 EEPROM 저장
          do_eprom_write(newEd);
        } else {
          bot->sendMessage(telegramChatId,"유효한 각도가 아닙니다. (0~180 범위)","");
        }
      } else {
        bot->sendMessage(telegramChatId,"숫자만 입력해주세요.","");
      }

      // 입력 완료 후 대기 상태 해제
      awaitingOnAngle=false;
      awaitingOffAngle=false;
      continue;
    }

    // 일반 명령 처리
    if(text=="/start"){
      String welcome="스위치 원격제어\n명령어:\n/on - 스위치ON(현재ON각도:"+String(servoOnAngle)+")\n/off - 스위치OFF(현재OFF각도:"+String(servoOffAngle)+")\n/set_on - ON시 각도 설정 모드 진입\n/set_off - OFF시 각도 설정 모드 진입\n/start - 메뉴 다시보기\n";
      bot->sendMessage(telegramChatId,welcome,"");
    } else if(text=="/on"){
      currentAngle=servoOnAngle;
      myServo.write(currentAngle);
      Serial.println(currentAngle);
      delay(500);
      bot->sendMessage(telegramChatId,"스위치 ON","");
    } else if(text=="/off"){
      currentAngle=servoOffAngle;
      myServo.write(currentAngle);
      Serial.println(currentAngle);
      delay(500);
      bot->sendMessage(telegramChatId,"스위치 OFF","");
    } else if(text=="/set_on"){
      awaitingOnAngle=true;
      awaitingOffAngle=false;
      bot->sendMessage(telegramChatId,"ON 상태의 각도를 숫자로만 입력해주세요.","");
    } else if(text=="/set_off"){
      awaitingOffAngle=true;
      awaitingOnAngle=false;
      bot->sendMessage(telegramChatId,"OFF 상태의 각도를 숫자로만 입력해주세요.","");
    } else {
      bot->sendMessage(telegramChatId,"알 수 없는 명령어 입니다.","");
    }
  }
}

void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("[Main] ESP32 스위치 컨트롤러 시작...");
  Serial.println("[Main] AP Name: "+devstr);

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG,0);
  EEPROM.begin(sizeof(eprom_data));

  init_wifi();

  myServo.attach(SERVO_PIN);
  myServo.write(currentAngle);
}

void loop(){
  if(bot && WiFi.status()==WL_CONNECTED){
    if(millis()>lastTimeBotRan+botRequestDelay){
      int numNewMessages=bot->getUpdates(bot->last_message_received+1);
      while(numNewMessages){
        handleNewMessages(numNewMessages);
        numNewMessages=bot->getUpdates(bot->last_message_received+1);
      }
      lastTimeBotRan=millis();
    }
  }

  if(Serial.available()){
    String cmd=Serial.readStringUntil('\n');
    cmd.trim();
    if(cmd.equalsIgnoreCase("reset")){
      resetSettings();
    }
  }
  delay(50);
}
