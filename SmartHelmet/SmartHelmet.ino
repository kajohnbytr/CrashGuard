/*
 * CrashGuard Smart Helmet - ESP32 / ESP8266 prototype firmware
 * Firmware version is target-specific | configuration schema 1
 *
 * IMPORTANT: verify the actual board schematic and every breakout's voltage
 * requirements before power-up. The pin plan below avoids board-specific
 * strapping pins, flash pins, and USB pins.
 *
 * Pin assignments are selected by the PlatformIO target. ESP32-C3 uses
 * native USB CDC; ESP32-WROOM-32 uses UART0 for USB-serial logging.
 */

#include <Arduino.h>
#ifdef CRASHGUARD_ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <bearssl/bearssl_hash.h>
#else
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#endif
#include <DNSServer.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPSPlus.h>
  
// ---------- Identity, limits, and pin map ----------
#ifdef CRASHGUARD_ESP8266
static const char *FW_VERSION = "1.0.0-esp8266";
static const uint8_t PIN_GSM_RX = 3;   // RX / USB bridge RX: ESP receives SIM800L TXD
static const uint8_t PIN_GSM_TX = 1;   // TX / USB bridge TX: ESP sends to SIM800L RXD
static const uint8_t PIN_GPS_RX = 14;  // D5 <- NEO-6M TX (software UART)
static const uint8_t PIN_GPS_TX = 12;  // D6 -> NEO-6M RX (normally unused)
static const uint8_t PIN_MPU_SDA = 4;  // D2
static const uint8_t PIN_MPU_SCL = 5;  // D1
static const uint8_t PIN_BUZZER = 13;  // D7, transistor driver input
static const uint8_t PIN_CANCEL = 16;  // D0, switch to GND
#elif defined(CRASHGUARD_ESP32_WROOM)
static const char *FW_VERSION = "1.1.0-esp32-wroom-32";
static const uint8_t PIN_GSM_RX = 26;  // ESP RX <- SIM800L TXD
static const uint8_t PIN_GSM_TX = 27;  // ESP TX -> SIM800L RXD
static const uint8_t PIN_GPS_RX = 16;  // ESP RX <- NEO-6M TX
static const uint8_t PIN_GPS_TX = 17;  // ESP TX -> NEO-6M RX (normally unused)
static const uint8_t PIN_MPU_SDA = 21;
static const uint8_t PIN_MPU_SCL = 22;
static const uint8_t PIN_BUZZER = 25;  // transistor/MOSFET driver input
static const uint8_t PIN_CANCEL = 33;  // switch to GND, INPUT_PULLUP
#else
static const char *FW_VERSION = "1.0.0";
static const uint8_t PIN_GSM_RX = 0;   // ESP RX <- SIM800L TXD (level-check module)
static const uint8_t PIN_GSM_TX = 1;   // ESP TX -> SIM800L RXD (divider recommended)
static const uint8_t PIN_GPS_RX = 3;   // ESP RX <- NEO-6M TX
static const uint8_t PIN_GPS_TX = 4;   // ESP TX -> NEO-6M RX (normally unused)
static const uint8_t PIN_MPU_SDA = 5;
static const uint8_t PIN_MPU_SCL = 6;
static const uint8_t PIN_BUZZER = 7;   // transistor/MOSFET input, never drive load directly
static const uint8_t PIN_CANCEL = 10;  // switch to GND, INPUT_PULLUP
#endif
static const uint16_t CONFIG_SCHEMA = 1;
static const uint8_t DNS_PORT = 53;
static const uint8_t MAX_CONTACTS = 3;
static const uint8_t LOG_CAPACITY = 40;
static const uint32_t GPS_FRESH_MS = 120000;
static const uint32_t GPS_ACQUIRE_MS = 30000;
static const uint32_t SESSION_TTL_MS = 30UL * 60UL * 1000UL;

#ifdef CRASHGUARD_ESP8266
SoftwareSerial GPS_UART(PIN_GPS_RX, PIN_GPS_TX);
#define GSM_UART Serial
#elif defined(CRASHGUARD_ESP32_WROOM)
HardwareSerial GPS_UART(2);
HardwareSerial GSM_UART(1);
#else
HardwareSerial GPS_UART(0);
HardwareSerial GSM_UART(1);
#endif
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
#ifdef CRASHGUARD_ESP8266
ESP8266WebServer server(80);
#else
WebServer server(80);
#endif
DNSServer dns;
#ifndef CRASHGUARD_ESP8266
Preferences prefs;
#endif

enum SystemState : uint8_t { BOOT, HARDWARE_CHECK, MONITORING, POSSIBLE_CRASH,
  COUNTDOWN, CANCELLED, ALERT_PENDING, GPS_ACQUISITION, SMS_SENDING,
  ALERT_RESULT, FAULT };  
enum SmsState : uint8_t { SMS_IDLE, SMS_AT, SMS_ECHO, SMS_TEXTMODE, SMS_NUMBER,
  SMS_BODY, SMS_WAIT_RESULT, SMS_NEXT, SMS_DONE, SMS_FAILED, DIAG_CPIN,
  DIAG_CREG, DIAG_CSQ, DIAG_COPS };

struct Contact { String name; String phone; bool enabled; };
struct Config {
  uint16_t schema = CONFIG_SCHEMA;
  String deviceName = "CrashGuard Helmet";
  String apSsid = "SmartHelmet-Setup";
  float accelThresholdG = 3.5f;
  float gyroThresholdDps = 350.0f;
  uint16_t persistenceMs = 250;
  uint16_t countdownSec = 20;
  uint16_t sampleHz = 50;
  uint16_t cooldownSec = 120;
  bool requireBoth = false;
  bool testMode = true;
  float accelOffset[3] = {0, 0, 0};
  float gyroOffset[3] = {0, 0, 0};
  bool calibrated = false;
  Contact contacts[MAX_CONTACTS];
} cfg;

struct Telemetry {
  float ax=0, ay=0, az=0, gx=0, gy=0, gz=0, accelG=0, gyroDps=0;
  bool mpuDetected=false, mpuReadOk=false, saturated=false;
  uint32_t lastMpuMs=0, samples=0;
  bool gpsDetected=false, gpsFix=false; double lat=0, lng=0;
  uint32_t gpsChars=0, lastGpsFixMs=0; uint32_t satellites=0;
  bool gsmDetected=false, simReady=false, registered=false, smsReady=false;
  int signal=-1; String operatorName=""; String modemId="";
  String lastSms="Never attempted"; String lastError="None";
} t;

struct LogEntry { uint32_t ms; String text; } logs[LOG_CAPACITY];
uint8_t logHead=0, logCount=0;
SystemState state=BOOT;
SmsState smsState=SMS_IDLE;
uint32_t stateSince=0, lastSampleMs=0, candidateSince=0, cooldownUntil=0;
uint32_t smsDeadline=0, buzzerToggleMs=0, lastGsmProbe=0;
uint32_t lastMpuRetry=0;
bool buzzerOn=false, candidateActive=false, simulatedCandidate=false;
bool calibrationRunning=false; uint16_t calibrationSamples=0;
double calAccSum[3]={0}, calGyroSum[3]={0}, calAccSq=0, calGyroSq=0;
String gsmRx, smsBody, smsTarget; uint8_t smsContactIndex=0;
int8_t forcedSmsContact=-1;
String sessionToken, csrfToken; uint32_t sessionLastSeen=0;
uint8_t loginFailures=0; uint32_t loginBlockedUntil=0, lastTestSms=0;

// ---------- Utilities ----------
String stateName(SystemState s) {
  const char *n[] = {"BOOT","HARDWARE_CHECK","MONITORING","POSSIBLE_CRASH",
    "COUNTDOWN","CANCELLED","ALERT_PENDING","GPS_ACQUISITION","SMS_SENDING",
    "ALERT_RESULT","FAULT"}; return n[(uint8_t)s <= FAULT ? (uint8_t)s : FAULT];
}
String escapeHtml(String v) { v.replace("&","&amp;"); v.replace("<","&lt;");
  v.replace(">","&gt;"); v.replace("\"","&quot;"); v.replace("'","&#39;"); return v; }
String jsonEscape(String v) { v.replace("\\","\\\\"); v.replace("\"","\\\"");
  v.replace("\r","\\r"); v.replace("\n","\\n"); return v; }
String randomHex(size_t bytes) { String out; out.reserve(bytes*2); char b[3];
  for(size_t i=0;i<bytes;i++){ 
#ifdef CRASHGUARD_ESP8266
    uint8_t value=(uint8_t)ESP.random();
#else
    uint8_t value=(uint8_t)esp_random();
#endif
    snprintf(b,sizeof(b),"%02x",value); out+=b; } return out; }
String sha256(const String &input) { uint8_t hash[32];
#ifdef CRASHGUARD_ESP8266
  br_sha256_context c; br_sha256_init(&c); br_sha256_update(&c,input.c_str(),input.length()); br_sha256_out(&c,hash);
#else
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c,0);
  mbedtls_sha256_update(&c,(const unsigned char*)input.c_str(),input.length());
  mbedtls_sha256_finish(&c,hash); mbedtls_sha256_free(&c);
#endif
  String out; char b[3];
  for(uint8_t x:hash){snprintf(b,sizeof(b),"%02x",x);out+=b;} return out; }
void logEvent(const String &msg) { logs[logHead]={millis(),msg}; logHead=(logHead+1)%LOG_CAPACITY;
  if(logCount<LOG_CAPACITY) logCount++;
#ifndef CRASHGUARD_ESP8266
  Serial.println("["+String(millis())+"] "+msg);
#endif
}
void setError(const String &e) { t.lastError=e; logEvent("FAULT: "+e); }
void changeState(SystemState s, const String &reason) { state=s; stateSince=millis();
  logEvent("State -> "+stateName(s)+(reason.length()?": "+reason:"")); }
bool elapsed(uint32_t since, uint32_t duration) { return (uint32_t)(millis()-since)>=duration; }
bool validPhone(const String &p) { if(p.length()<8 || p.length()>16) return false;
  for(size_t i=0;i<p.length();i++) if(!isDigit(p[i]) && !(i==0 && p[i]=='+')) return false; return true; }
float clampFloat(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);} 

// ---------- Persistence ----------
#ifdef CRASHGUARD_ESP8266
struct StoredContact { char name[31]; char phone[17]; uint8_t enabled; };
struct StoredConfig {
  uint32_t magic; uint16_t schema; char deviceName[31]; char apSsid[33];
  float accelThresholdG, gyroThresholdDps; uint16_t persistenceMs, countdownSec, sampleHz, cooldownSec;
  uint8_t requireBoth, testMode, calibrated; float accelOffset[3], gyroOffset[3];
  StoredContact contacts[MAX_CONTACTS]; char salt[33]; char passhash[65]; uint32_t checksum;
};
StoredConfig stored;
uint32_t storedChecksum(const uint8_t *p,size_t bytes){uint32_t h=2166136261UL;for(size_t i=0;i<bytes;i++){h^=p[i];h*=16777619UL;}return h;}
void copyText(char *dst,size_t size,const String &src){strncpy(dst,src.c_str(),size-1);dst[size-1]=0;}
bool readStored(){EEPROM.begin(sizeof(StoredConfig));EEPROM.get(0,stored);return stored.magic==0x43474831UL&&stored.schema==CONFIG_SCHEMA&&stored.checksum==storedChecksum((const uint8_t*)&stored,sizeof(StoredConfig)-sizeof(uint32_t));}
void writeStored(){stored.magic=0x43474831UL;stored.schema=CONFIG_SCHEMA;stored.checksum=storedChecksum((const uint8_t*)&stored,sizeof(StoredConfig)-sizeof(uint32_t));EEPROM.put(0,stored);EEPROM.commit();}
void syncConfigToStored(){copyText(stored.deviceName,sizeof(stored.deviceName),cfg.deviceName);copyText(stored.apSsid,sizeof(stored.apSsid),cfg.apSsid);stored.accelThresholdG=cfg.accelThresholdG;stored.gyroThresholdDps=cfg.gyroThresholdDps;stored.persistenceMs=cfg.persistenceMs;stored.countdownSec=cfg.countdownSec;stored.sampleHz=cfg.sampleHz;stored.cooldownSec=cfg.cooldownSec;stored.requireBoth=cfg.requireBoth;stored.testMode=cfg.testMode;stored.calibrated=cfg.calibrated;memcpy(stored.accelOffset,cfg.accelOffset,sizeof(cfg.accelOffset));memcpy(stored.gyroOffset,cfg.gyroOffset,sizeof(cfg.gyroOffset));for(int i=0;i<MAX_CONTACTS;i++){copyText(stored.contacts[i].name,sizeof(stored.contacts[i].name),cfg.contacts[i].name);copyText(stored.contacts[i].phone,sizeof(stored.contacts[i].phone),cfg.contacts[i].phone);stored.contacts[i].enabled=cfg.contacts[i].enabled;}}
void loadConfig(){if(!readStored()){memset(&stored,0,sizeof(stored));syncConfigToStored();writeStored();return;}cfg.deviceName=stored.deviceName;cfg.apSsid=stored.apSsid;cfg.accelThresholdG=stored.accelThresholdG;cfg.gyroThresholdDps=stored.gyroThresholdDps;cfg.persistenceMs=stored.persistenceMs;cfg.countdownSec=stored.countdownSec;cfg.sampleHz=stored.sampleHz;cfg.cooldownSec=stored.cooldownSec;cfg.requireBoth=stored.requireBoth;cfg.testMode=stored.testMode;cfg.calibrated=stored.calibrated;memcpy(cfg.accelOffset,stored.accelOffset,sizeof(cfg.accelOffset));memcpy(cfg.gyroOffset,stored.gyroOffset,sizeof(cfg.gyroOffset));for(int i=0;i<MAX_CONTACTS;i++){cfg.contacts[i].name=stored.contacts[i].name;cfg.contacts[i].phone=stored.contacts[i].phone;cfg.contacts[i].enabled=stored.contacts[i].enabled;}}
void saveConfig(){syncConfigToStored();writeStored();logEvent("Configuration saved");}
bool hasPassword(){readStored();return stored.passhash[0]!=0;}
bool setPassword(const String &p){if(p.length()<10||p.length()>64)return false;if(!readStored())memset(&stored,0,sizeof(stored));String salt=randomHex(16),hash=sha256(salt+p);copyText(stored.salt,sizeof(stored.salt),salt);copyText(stored.passhash,sizeof(stored.passhash),hash);syncConfigToStored();writeStored();return true;}
bool checkPassword(const String &p){if(!readStored()||!stored.passhash[0])return false;return sha256(String(stored.salt)+p)==String(stored.passhash);}
void resetAdminPassword(){if(readStored()){memset(stored.salt,0,sizeof(stored.salt));memset(stored.passhash,0,sizeof(stored.passhash));writeStored();}}
#else
void loadConfig() {
  prefs.begin("crashguard", true);
  if(prefs.getUShort("schema",0)==CONFIG_SCHEMA) {
    cfg.deviceName=prefs.getString("name",cfg.deviceName); cfg.apSsid=prefs.getString("ssid",cfg.apSsid);
    cfg.accelThresholdG=prefs.getFloat("accel",cfg.accelThresholdG);
    cfg.gyroThresholdDps=prefs.getFloat("gyro",cfg.gyroThresholdDps);
    cfg.persistenceMs=prefs.getUShort("persist",cfg.persistenceMs);
    cfg.countdownSec=prefs.getUShort("countdown",cfg.countdownSec);
    cfg.sampleHz=prefs.getUShort("rate",cfg.sampleHz); cfg.cooldownSec=prefs.getUShort("cooldown",cfg.cooldownSec);
    cfg.requireBoth=prefs.getBool("both",cfg.requireBoth); cfg.testMode=prefs.getBool("test",cfg.testMode);
    cfg.calibrated=prefs.getBool("calibrated",false);
    for(int i=0;i<3;i++){ cfg.accelOffset[i]=prefs.getFloat(("ao"+String(i)).c_str(),0);
      cfg.gyroOffset[i]=prefs.getFloat(("go"+String(i)).c_str(),0); }
    for(int i=0;i<MAX_CONTACTS;i++){ cfg.contacts[i].name=prefs.getString(("cn"+String(i)).c_str(),"");
      cfg.contacts[i].phone=prefs.getString(("cp"+String(i)).c_str(),"");
      cfg.contacts[i].enabled=prefs.getBool(("ce"+String(i)).c_str(),false); }
  }
  prefs.end();
}
void saveConfig() {
  prefs.begin("crashguard",false); prefs.putUShort("schema",CONFIG_SCHEMA);
  prefs.putString("name",cfg.deviceName); prefs.putString("ssid",cfg.apSsid);
  prefs.putFloat("accel",cfg.accelThresholdG); prefs.putFloat("gyro",cfg.gyroThresholdDps);
  prefs.putUShort("persist",cfg.persistenceMs); prefs.putUShort("countdown",cfg.countdownSec);
  prefs.putUShort("rate",cfg.sampleHz); prefs.putUShort("cooldown",cfg.cooldownSec);
  prefs.putBool("both",cfg.requireBoth); prefs.putBool("test",cfg.testMode); prefs.putBool("calibrated",cfg.calibrated);
  for(int i=0;i<3;i++){prefs.putFloat(("ao"+String(i)).c_str(),cfg.accelOffset[i]);prefs.putFloat(("go"+String(i)).c_str(),cfg.gyroOffset[i]);}
  for(int i=0;i<MAX_CONTACTS;i++){prefs.putString(("cn"+String(i)).c_str(),cfg.contacts[i].name);
    prefs.putString(("cp"+String(i)).c_str(),cfg.contacts[i].phone);prefs.putBool(("ce"+String(i)).c_str(),cfg.contacts[i].enabled);}
  prefs.end(); logEvent("Configuration saved");
}
bool hasPassword() { prefs.begin("crashguard",true); bool ok=prefs.isKey("passhash");prefs.end();return ok; }
bool setPassword(const String &p) { if(p.length()<10||p.length()>64) return false; String salt=randomHex(16);
  prefs.begin("crashguard",false);prefs.putString("salt",salt);prefs.putString("passhash",sha256(salt+p));prefs.end();return true; }
bool checkPassword(const String &p){prefs.begin("crashguard",true);String s=prefs.getString("salt","");String h=prefs.getString("passhash","");prefs.end();return h.length()&&sha256(s+p)==h;}
void resetAdminPassword(){prefs.begin("crashguard",false);prefs.remove("salt");prefs.remove("passhash");prefs.end();}
#endif

// ---------- Sensor, calibration, and detection ----------
void sampleMpu() {
  if(!t.mpuDetected) return;
  uint32_t period=1000UL/cfg.sampleHz; if(!elapsed(lastSampleMs,period)) return; lastSampleMs=millis();
  sensors_event_t a,g,temp; if(!mpu.getEvent(&a,&g,&temp)){t.mpuReadOk=false;setError("MPU6050 read failed");return;}
  t.mpuReadOk=true;t.lastMpuMs=millis();t.samples++;
  t.ax=a.acceleration.x-cfg.accelOffset[0];t.ay=a.acceleration.y-cfg.accelOffset[1];t.az=a.acceleration.z-cfg.accelOffset[2];
  t.gx=g.gyro.x-cfg.gyroOffset[0];t.gy=g.gyro.y-cfg.gyroOffset[1];t.gz=g.gyro.z-cfg.gyroOffset[2];
  float rawG=sqrtf(t.ax*t.ax+t.ay*t.ay+t.az*t.az)/SENSORS_GRAVITY_STANDARD;
  float rawDps=sqrtf(t.gx*t.gx+t.gy*t.gy+t.gz*t.gz)*RAD_TO_DEG;
  static float filtG=1, filtDps=0; filtG=.72f*filtG+.28f*rawG;filtDps=.72f*filtDps+.28f*rawDps;
  t.accelG=filtG;t.gyroDps=filtDps;t.saturated=rawG>15.5f||rawDps>1950;
  if(calibrationRunning){ calibrationSamples++; float amag=sqrtf(a.acceleration.x*a.acceleration.x+a.acceleration.y*a.acceleration.y+a.acceleration.z*a.acceleration.z);
    float gmag=sqrtf(g.gyro.x*g.gyro.x+g.gyro.y*g.gyro.y+g.gyro.z*g.gyro.z);
    calAccSum[0]+=a.acceleration.x;calAccSum[1]+=a.acceleration.y;calAccSum[2]+=a.acceleration.z-SENSORS_GRAVITY_STANDARD;
    calGyroSum[0]+=g.gyro.x;calGyroSum[1]+=g.gyro.y;calGyroSum[2]+=g.gyro.z;calAccSq+=amag*amag;calGyroSq+=gmag*gmag;
    if(calibrationSamples>=500){ double am=(calAccSum[2]+500*SENSORS_GRAVITY_STANDARD)/500.0;
      double av=calAccSq/500.0-am*am, gm=(calGyroSum[0]+calGyroSum[1]+calGyroSum[2])/1500.0;
      double gv=calGyroSq/500.0-gm*gm; calibrationRunning=false;
      if(av>0.35||gv>0.02){setError("Calibration rejected: sensor moved");logEvent("Calibration rejected");}
      else {for(int i=0;i<3;i++){cfg.accelOffset[i]=calAccSum[i]/500.0;cfg.gyroOffset[i]=calGyroSum[i]/500.0;}cfg.calibrated=true;saveConfig();logEvent("Calibration accepted (500 samples)");}
    } return;
  }
  if(state!=MONITORING || millis()<cooldownUntil || t.saturated) return;
  bool aHit=filtG>=cfg.accelThresholdG, gHit=filtDps>=cfg.gyroThresholdDps;
  bool hit=cfg.requireBoth?(aHit&&gHit):(aHit||gHit);
  if(hit||simulatedCandidate){ if(!candidateActive){candidateActive=true;candidateSince=millis();changeState(POSSIBLE_CRASH,"threshold candidate");}
    if(elapsed(candidateSince,cfg.persistenceMs)){simulatedCandidate=false;changeState(COUNTDOWN,cfg.testMode?"test mode; SMS disabled":"warning active");}}
}
void updateCrashWorkflow(){
  if(state==FAULT && elapsed(lastMpuRetry,5000)){lastMpuRetry=millis();t.mpuDetected=mpu.begin(0x68,&Wire);if(t.mpuDetected){mpu.setAccelerometerRange(MPU6050_RANGE_16_G);mpu.setGyroRange(MPU6050_RANGE_2000_DEG);mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);t.lastError="None";changeState(MONITORING,"MPU6050 recovered");}}
  if(state==POSSIBLE_CRASH && !elapsed(candidateSince,cfg.persistenceMs)){ if(t.accelG<cfg.accelThresholdG*.75f&&t.gyroDps<cfg.gyroThresholdDps*.75f){candidateActive=false;changeState(MONITORING,"candidate cleared");}}
  if(state==COUNTDOWN){ uint32_t remaining=cfg.countdownSec*1000UL-(millis()-stateSince);
    if(digitalRead(PIN_CANCEL)==LOW){digitalWrite(PIN_BUZZER,LOW);buzzerOn=false;changeState(CANCELLED,"physical button");}
    else { if(elapsed(buzzerToggleMs,250)){buzzerToggleMs=millis();buzzerOn=!buzzerOn;digitalWrite(PIN_BUZZER,buzzerOn);}
      if((int32_t)remaining<=0){digitalWrite(PIN_BUZZER,LOW);buzzerOn=false;
        if(cfg.testMode){t.lastSms="Suppressed by test mode";changeState(ALERT_RESULT,"simulation complete");}
        else changeState(ALERT_PENDING,"countdown expired");}}
  }
  if(state==CANCELLED && elapsed(stateSince,2500)){cooldownUntil=millis()+cfg.cooldownSec*1000UL;candidateActive=false;changeState(MONITORING,"cooldown");}
  if(state==ALERT_PENDING){changeState(GPS_ACQUISITION,"waiting for fresh fix");}
  if(state==GPS_ACQUISITION && ((t.gpsFix&&elapsed(t.lastGpsFixMs,GPS_FRESH_MS)==false)||elapsed(stateSince,GPS_ACQUIRE_MS))){
    smsBody="SMART HELMET EMERGENCY ALERT: Possible crash detected. ";
    if(t.gpsFix&&!elapsed(t.lastGpsFixMs,GPS_FRESH_MS)) smsBody+="Last valid location: https://maps.google.com/?q="+String(t.lat,6)+","+String(t.lng,6)+". ";
    else smsBody+="Location unavailable. "; smsBody+="Please contact the rider and verify their safety.";
    smsContactIndex=0;smsState=SMS_NEXT;changeState(SMS_SENDING,"SMS queue started");
  }
  if(state==ALERT_RESULT && elapsed(stateSince,5000)){cooldownUntil=millis()+cfg.cooldownSec*1000UL;candidateActive=false;changeState(MONITORING,"alert handled");}
}

// ---------- GPS and non-blocking SIM800L handling ----------
void pollGps(){while(GPS_UART.available()){char c=GPS_UART.read();gps.encode(c);t.gpsChars++;}t.gpsDetected=t.gpsChars>20;
  if(gps.location.isUpdated()&&gps.location.isValid()){t.gpsFix=true;t.lat=gps.location.lat();t.lng=gps.location.lng();t.lastGpsFixMs=millis();}
  if(gps.satellites.isValid())t.satellites=gps.satellites.value(); if(t.gpsFix&&elapsed(t.lastGpsFixMs,GPS_FRESH_MS*2))t.gpsFix=false;}
void gsmWrite(const String &s,bool ctrlZ=false){GSM_UART.print(s);if(ctrlZ)GSM_UART.write(26);else GSM_UART.print("\r");gsmRx="";smsDeadline=millis()+5000;}
bool gsmHas(const char *s){return gsmRx.indexOf(s)>=0;}
void pollGsm(){while(GSM_UART.available()){char c=GSM_UART.read();if(gsmRx.length()<700)gsmRx+=c;}
  if(smsState==SMS_IDLE && elapsed(lastGsmProbe,30000)){lastGsmProbe=millis();gsmWrite("AT");smsState=SMS_AT;}
  if(smsState==SMS_IDLE)return;
  bool timeout=(int32_t)(millis()-smsDeadline)>=0;
  if(smsState==SMS_NEXT){if(forcedSmsContact>=0)smsContactIndex=forcedSmsContact;while(smsContactIndex<MAX_CONTACTS&&(!validPhone(cfg.contacts[smsContactIndex].phone)||(forcedSmsContact<0&&!cfg.contacts[smsContactIndex].enabled)))smsContactIndex++;
    if(smsContactIndex>=MAX_CONTACTS){smsState=SMS_DONE;return;}smsTarget=cfg.contacts[smsContactIndex].phone;gsmWrite("AT");smsState=SMS_AT;return;}
  if(smsState==SMS_AT&&(gsmHas("OK")||timeout)){if(gsmHas("OK")){t.gsmDetected=true;gsmWrite("ATE0");smsState=SMS_ECHO;}else{t.gsmDetected=false;smsState=state==SMS_SENDING?SMS_FAILED:SMS_IDLE;}return;}
  if(smsState==SMS_ECHO&&(gsmHas("OK")||timeout)){gsmWrite("AT+CMGF=1");smsState=SMS_TEXTMODE;return;}
  if(smsState==SMS_TEXTMODE&&(gsmHas("OK")||timeout)){t.smsReady=gsmHas("OK");if(state==SMS_SENDING&&t.smsReady){gsmWrite("AT+CMGS=\""+smsTarget+"\"");smsState=SMS_NUMBER;smsDeadline=millis()+8000;}else{gsmWrite("AT+CPIN?");smsState=DIAG_CPIN;}return;}
  if(smsState==SMS_NUMBER&&(gsmHas(">")||timeout)){if(gsmHas(">")){gsmWrite(smsBody,true);smsState=SMS_WAIT_RESULT;smsDeadline=millis()+60000;}else smsState=SMS_FAILED;return;}
  if(smsState==SMS_WAIT_RESULT&&(gsmHas("+CMGS:")||gsmHas("ERROR")||timeout)){if(gsmHas("+CMGS:")){t.lastSms="Modem accepted SMS for "+smsTarget+" (delivery not confirmed)";logEvent(t.lastSms);if(forcedSmsContact>=0){forcedSmsContact=-1;smsState=SMS_DONE;}else{smsContactIndex++;smsState=SMS_NEXT;}}else smsState=SMS_FAILED;return;}
  if(smsState==SMS_FAILED){t.lastSms="SMS attempt failed for "+smsTarget;setError(t.lastSms);if(forcedSmsContact>=0){forcedSmsContact=-1;smsState=SMS_DONE;}else{smsContactIndex++;smsState=SMS_NEXT;}}
  if(smsState==SMS_DONE){smsState=SMS_IDLE;changeState(ALERT_RESULT,"SMS queue finished");}
  if(smsState==DIAG_CPIN&&(gsmHas("OK")||gsmHas("ERROR")||timeout)){t.simReady=gsmHas("+CPIN: READY");gsmWrite("AT+CREG?");smsState=DIAG_CREG;return;}
  if(smsState==DIAG_CREG&&(gsmHas("OK")||gsmHas("ERROR")||timeout)){t.registered=gsmHas(",1")||gsmHas(",5");gsmWrite("AT+CSQ");smsState=DIAG_CSQ;return;}
  if(smsState==DIAG_CSQ&&(gsmHas("OK")||gsmHas("ERROR")||timeout)){int p=gsmRx.indexOf("+CSQ:");if(p>=0)t.signal=gsmRx.substring(p+5).toInt();gsmWrite("AT+COPS?");smsState=DIAG_COPS;return;}
  if(smsState==DIAG_COPS&&(gsmHas("OK")||gsmHas("ERROR")||timeout)){int q1=gsmRx.indexOf('"'),q2=q1>=0?gsmRx.indexOf('"',q1+1):-1;if(q1>=0&&q2>q1)t.operatorName=gsmRx.substring(q1+1,q2);smsState=SMS_IDLE;logEvent("GSM diagnostics refreshed");return;}
}

// ---------- Auth and web API ----------
bool authenticated(){String cookie=server.header("Cookie");int p=cookie.indexOf("CGSESSION=");if(p<0||sessionToken.isEmpty())return false;
  String token=cookie.substring(p+10);int semi=token.indexOf(';');if(semi>=0)token=token.substring(0,semi);
  if(token!=sessionToken||elapsed(sessionLastSeen,SESSION_TTL_MS)){sessionToken="";return false;}sessionLastSeen=millis();return true;}
bool requireAuth(bool csrf=false){if(!authenticated()){server.send(401,"application/json","{\"error\":\"authentication_required\"}");return false;}
  if(csrf&&server.header("X-CSRF-Token")!=csrfToken){server.send(403,"application/json","{\"error\":\"invalid_csrf\"}");return false;}return true;}
String statusJson(){uint32_t remain=state==COUNTDOWN&&millis()-stateSince<cfg.countdownSec*1000UL?cfg.countdownSec-(millis()-stateSince)/1000:0;
  String s="{";s+="\"firmware\":\""+String(FW_VERSION)+"\",\"uptime\":"+String(millis()/1000)+",\"state\":\""+stateName(state)+"\",";
  s+="\"countdown\":"+String(remain)+",\"clients\":"+String(WiFi.softAPgetStationNum())+",\"mpuDetected\":"+(t.mpuDetected?"true":"false")+",";
  s+=String("\"mpuReadOk\":")+(t.mpuReadOk?"true":"false")+",\"ax\":"+String(t.ax,2)+",\"ay\":"+String(t.ay,2)+",\"az\":"+String(t.az,2)+",";
  s+="\"gx\":"+String(t.gx*RAD_TO_DEG,1)+",\"gy\":"+String(t.gy*RAD_TO_DEG,1)+",\"gz\":"+String(t.gz*RAD_TO_DEG,1)+",\"accelG\":"+String(t.accelG,2)+",\"gyroDps\":"+String(t.gyroDps,1)+",";
  s+="\"sampleHz\":"+String(cfg.sampleHz)+",\"saturated\":"+(t.saturated?"true":"false")+",\"calibrated\":"+(cfg.calibrated?"true":"false")+",";
  s+=String("\"calibrationRunning\":")+(calibrationRunning?"true":"false")+",\"calibrationProgress\":"+String(calibrationSamples*100/500)+",";
  s+=String("\"gpsDetected\":")+(t.gpsDetected?"true":"false")+",\"gpsFix\":"+(t.gpsFix?"true":"false")+",\"lat\":"+String(t.lat,6)+",\"lng\":"+String(t.lng,6)+",\"satellites\":"+String(t.satellites)+",";
  s+="\"fixAge\":"+(t.lastGpsFixMs?String((millis()-t.lastGpsFixMs)/1000):"null")+",\"gsmDetected\":"+(t.gsmDetected?"true":"false")+",\"simReady\":"+(t.simReady?"true":"false")+",\"registered\":"+(t.registered?"true":"false")+",\"signal\":"+String(t.signal)+",\"operatorName\":\""+jsonEscape(t.operatorName)+"\",\"smsReady\":"+(t.smsReady?"true":"false")+",";
  s+="\"lastSms\":\""+jsonEscape(t.lastSms)+"\",\"lastError\":\""+jsonEscape(t.lastError)+"\",\"testMode\":"+(cfg.testMode?"true":"false")+"}";return s;}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>CrashGuard</title><style>
:root{--bg:#0f172a;--card:#1b2336;--muted:#94a3b8;--line:#475569;--text:#f8fafc;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 90% 0,#20365a 0,transparent 35%),var(--bg);color:var(--text);font:16px/1.5 system-ui,sans-serif}main{max-width:1100px;margin:auto;padding:18px}.top{display:flex;justify-content:space-between;align-items:center;gap:12px}.brand{font-size:1.3rem;font-weight:800}.muted{color:var(--muted)}nav{display:flex;overflow:auto;gap:8px;margin:18px 0}button,.tab,input,select{min-height:44px;border-radius:8px;border:1px solid var(--line);font:inherit}.tab,button{background:#27344c;color:var(--text);padding:10px 14px;cursor:pointer}.tab.active,button.primary{background:var(--ok);color:#07150d;border-color:transparent;font-weight:700}button.danger{background:var(--bad);color:white}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.card{background:rgba(27,35,54,.94);border:1px solid var(--line);border-radius:12px;padding:16px}.metric{font:700 1.25rem ui-monospace,monospace}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--bad);margin-right:7px}.dot.ok{background:var(--ok)}section{display:none}section.active{display:block}label{display:block;margin:10px 0 4px;font-weight:650}input,select{width:100%;padding:9px 11px;background:#111a2c;color:var(--text)}.actions{display:flex;flex-wrap:wrap;gap:9px;margin-top:14px}.notice{padding:12px;border-left:4px solid var(--warn);background:#2e291c;margin:12px 0}.toast{position:fixed;right:18px;bottom:18px;max-width:330px;padding:12px;background:#111827;border:1px solid var(--line);border-radius:8px;display:none}.logs{max-height:430px;overflow:auto;font:13px/1.6 ui-monospace,monospace}.hide{display:none}@media(max-width:600px){main{padding:12px}.top{align-items:flex-start}.grid{grid-template-columns:1fr}}@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important}}
</style></head><body><main><div class=top><div><div class=brand>CrashGuard</div><div class=muted>Local safety console · HTTP is not encrypted</div></div><button id=logout class=hide>Log out</button></div>
<div id=auth class=card><h1 id=authTitle>Administrator access</h1><p id=authHelp class=muted></p><label for=password>Password</label><input id=password type=password autocomplete=current-password minlength=10 maxlength=64><div class=actions><button class=primary onclick=login()>Continue</button></div></div>
<div id=app class=hide><nav aria-label="Portal sections"><button class="tab active" data-tab=dash>Dashboard</button><button class=tab data-tab=sensor>Sensor</button><button class=tab data-tab=settings>Settings</button><button class=tab data-tab=contacts>Contacts</button><button class=tab data-tab=logs>Logs</button></nav>
<section id=dash class=active><div class=grid><div class=card><span id=stateDot class=dot></span>System state<div id=state class=metric>—</div><div id=countdown class=muted></div></div><div class=card>MPU6050<div id=mpu class=metric>—</div><div id=cal class=muted></div></div><div class=card>GPS<div id=gps class=metric>—</div><div id=gpsMeta class=muted></div></div><div class=card>SIM800L<div id=gsm class=metric>—</div><div id=gsmMeta class=muted></div><div id=sms class=muted></div><button onclick=act('/api/gsm/check')>Refresh modem checks</button></div><div class=card>Access point<div id=wifi class=metric>—</div><div id=uptime class=muted></div></div><div class=card>Last diagnostic<div id=error class=metric>—</div></div></div><div class=notice>Prototype only. A green status means the latest check passed; it does not certify crash detection, SMS delivery, or rider safety.</div></section>
<section id=sensor><div class=grid><div class=card>Acceleration (m/s²)<div id=acc class=metric>—</div><div id=amag class=muted></div></div><div class=card>Angular velocity (°/s)<div id=gyro class=metric>—</div><div id=gmag class=muted></div></div><div class=card>Calibration<div id=calProgress class=metric>—</div><p class=muted>Keep the installed sensor still and level. Bias calibration does not tune crash thresholds.</p><div class=actions><button onclick=act('/api/calibrate')>Start calibration</button><button onclick=act('/api/calibration/reset')>Reset offsets</button></div></div><div class=card>Safe simulation<p class=muted>Exercises detection and countdown. It never sends an SMS while test mode is enabled.</p><button onclick=act('/api/simulate')>Simulate candidate</button></div></div></section>
<section id=settings><div class=card><h2>Crash detection</h2><div class=grid><div><label for=accel>Acceleration threshold (g)</label><input id=accel type=number min=1.5 max=16 step=.1></div><div><label for=gyroT>Angular threshold (°/s)</label><input id=gyroT type=number min=50 max=2000 step=10></div><div><label for=persist>Persistence (ms)</label><input id=persist type=number min=100 max=3000></div><div><label for=count>Countdown (s)</label><input id=count type=number min=5 max=120></div><div><label for=rate>Sample rate (Hz)</label><input id=rate type=number min=20 max=100></div><div><label for=cooldown>Cooldown (s)</label><input id=cooldown type=number min=30 max=3600></div></div><label><input id=both type=checkbox style="width:auto;min-height:auto"> Require both acceleration and rotation thresholds</label><label><input id=test type=checkbox style="width:auto;min-height:auto"> Test mode (SMS disabled)</label><div class=actions><button class=primary onclick=saveSettings()>Save settings</button></div></div></section>
<section id=contacts><div class=card><h2>Emergency contacts</h2><p class=muted>Use E.164 format such as +639171234567. Saving never sends a message.</p><div id=contactRows></div><div class=actions><button class=primary onclick=saveContacts()>Save contacts</button><button onclick=testSms()>Send one test SMS</button></div></div></section>
<section id=logs><div class=card><h2>Recent events</h2><div id=logList class=logs>Loading…</div></div></section></div></main><div id=toast class=toast role=status aria-live=polite></div>
<script>let csrf='',live=null,setup=false;const $=id=>document.getElementById(id),toast=m=>{let x=$('toast');x.textContent=m;x.style.display='block';setTimeout(()=>x.style.display='none',3500)};async function api(url,opt={}){opt.headers={...(opt.headers||{}),'X-CSRF-Token':csrf};let r=await fetch(url,opt),j=await r.json().catch(()=>({error:'invalid response'}));if(!r.ok)throw Error(j.error||'Request failed');return j}async function boot(){let r=await fetch('/api/session'),j=await r.json();setup=j.setupRequired;$('authTitle').textContent=setup?'First-time setup':'Administrator access';$('authHelp').textContent=setup?'Create a unique password (10–64 characters). Record it securely; recovery requires holding the physical cancel button during boot.':'Sign in to change safety settings.';if(j.authenticated)showApp(j.csrf)}async function login(){try{let j=await api(setup?'/api/setup':'/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'password='+encodeURIComponent($('password').value)});showApp(j.csrf)}catch(e){toast(e.message)}}function showApp(c){csrf=c;$('auth').classList.add('hide');$('app').classList.remove('hide');$('logout').classList.remove('hide');loadConfig();refresh();live=setInterval(refresh,1000)}$('logout').onclick=async()=>{await api('/api/logout',{method:'POST'});location.reload()};document.querySelectorAll('.tab').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tab,section').forEach(x=>x.classList.remove('active'));b.classList.add('active');$(b.dataset.tab).classList.add('active');if(b.dataset.tab==='logs')loadLogs()});async function refresh(){try{let s=await api('/api/status');$('state').textContent=s.state;$('stateDot').className='dot '+(s.state==='MONITORING'?'ok':'');$('countdown').textContent=s.countdown?'Alert in '+s.countdown+' s':(s.testMode?'Test mode':'Live alerts');$('mpu').textContent=s.mpuDetected?(s.mpuReadOk?'Detected · reading':'Detected · read fault'):'Not detected';$('cal').textContent=s.calibrated?'Offsets saved':'Not calibrated';$('gps').textContent=s.gpsFix?'Valid fix':(s.gpsDetected?'Detected · no fix':'Not detected');$('gpsMeta').textContent=s.gpsFix?s.lat.toFixed(6)+', '+s.lng.toFixed(6)+' · '+s.satellites+' satellites':'Needs clear sky';$('gsm').textContent=s.gsmDetected?(s.smsReady?'Detected · SMS ready':'Detected · initializing'):'Not detected';$('gsmMeta').textContent='SIM '+(s.simReady?'ready':'not ready')+' · network '+(s.registered?'registered':'not registered')+' · CSQ '+s.signal+(s.operatorName?' · '+s.operatorName:'');$('sms').textContent=s.lastSms;$('wifi').textContent=s.clients+' connected';$('uptime').textContent='Uptime '+s.uptime+' s · firmware '+s.firmware;$('error').textContent=s.lastError;$('acc').textContent=`X ${s.ax} · Y ${s.ay} · Z ${s.az}`;$('amag').textContent=s.accelG+' g'+(s.saturated?' · SATURATED':'');$('gyro').textContent=`X ${s.gx} · Y ${s.gy} · Z ${s.gz}`;$('gmag').textContent=s.gyroDps+' °/s · '+s.sampleHz+' Hz';$('calProgress').textContent=s.calibrationRunning?s.calibrationProgress+'%':'Idle'}catch(e){toast('Connection lost')}}async function act(url){try{let j=await api(url,{method:'POST'});toast(j.message)}catch(e){toast(e.message)}}async function loadConfig(){let c=await api('/api/config');['accel','persist','count','rate','cooldown'].forEach(k=>$(k).value=c[k]);$('gyroT').value=c.gyro;$('both').checked=c.both;$('test').checked=c.test;let h='';c.contacts.forEach((x,i)=>h+=`<div class=grid><div><label for=cn${i}>Name ${i+1}</label><input id=cn${i} value="${esc(x.name)}" maxlength=30></div><div><label for=cp${i}>Phone ${i+1}</label><input id=cp${i} value="${esc(x.phone)}" maxlength=16 inputmode=tel></div><div><label for=ce${i}>Alert enabled</label><select id=ce${i}><option value=1 ${x.enabled?'selected':''}>Yes</option><option value=0 ${!x.enabled?'selected':''}>No</option></select></div></div>`);$('contactRows').innerHTML=h}function esc(s){return s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}async function saveSettings(){let b=new URLSearchParams({accel:$('accel').value,gyro:$('gyroT').value,persist:$('persist').value,count:$('count').value,rate:$('rate').value,cooldown:$('cooldown').value,both:$('both').checked?1:0,test:$('test').checked?1:0});try{let j=await api('/api/config',{method:'POST',body:b});toast(j.message)}catch(e){toast(e.message)}}async function saveContacts(){let b=new URLSearchParams();for(let i=0;i<3;i++){b.set('cn'+i,$('cn'+i).value);b.set('cp'+i,$('cp'+i).value);b.set('ce'+i,$('ce'+i).value)}try{let j=await api('/api/contacts',{method:'POST',body:b});toast(j.message)}catch(e){toast(e.message)}}async function testSms(){let n=prompt('Contact number (1–3) to test:');if(n)try{let j=await api('/api/sms/test',{method:'POST',body:new URLSearchParams({index:n-1})});toast(j.message)}catch(e){toast(e.message)}}async function loadLogs(){let j=await api('/api/logs');$('logList').innerHTML=j.logs.map(x=>`<div>${x.time}s · ${esc(x.text)}</div>`).join('')||'No events' }boot();</script></body></html>)HTML";

void sendPage(){server.send_P(200,"text/html",PAGE);}
void setupRoutes(){
#ifdef CRASHGUARD_ESP8266
  server.collectHeaders("Cookie","X-CSRF-Token");
#else
  const char *headers[]={"Cookie","X-CSRF-Token"};server.collectHeaders(headers,2);
#endif
  server.on("/",HTTP_GET,sendPage); server.on("/generate_204",HTTP_ANY,sendPage);server.on("/hotspot-detect.html",HTTP_ANY,sendPage);server.on("/connecttest.txt",HTTP_ANY,sendPage);server.on("/ncsi.txt",HTTP_ANY,sendPage);
  server.on("/api/session",HTTP_GET,[]{bool a=authenticated();server.send(200,"application/json","{\"setupRequired\":"+String(hasPassword()?"false":"true")+",\"authenticated\":"+String(a?"true":"false")+",\"csrf\":\""+(a?csrfToken:"")+"\"}");});
  server.on("/api/setup",HTTP_POST,[]{if(hasPassword()){server.send(409,"application/json","{\"error\":\"already_configured\"}");return;}if(!setPassword(server.arg("password"))){server.send(422,"application/json","{\"error\":\"password_must_be_10_to_64_characters\"}");return;}sessionToken=randomHex(24);csrfToken=randomHex(16);sessionLastSeen=millis();server.sendHeader("Set-Cookie","CGSESSION="+sessionToken+"; HttpOnly; SameSite=Strict");server.send(200,"application/json","{\"csrf\":\""+csrfToken+"\"}");logEvent("First-time administrator setup completed");});
  server.on("/api/login",HTTP_POST,[]{if((int32_t)(millis()-loginBlockedUntil)<0){server.send(429,"application/json","{\"error\":\"login_temporarily_locked\"}");return;}if(!checkPassword(server.arg("password"))){if(++loginFailures>=5){loginBlockedUntil=millis()+60000;loginFailures=0;}server.send(401,"application/json","{\"error\":\"invalid_credentials\"}");return;}loginFailures=0;sessionToken=randomHex(24);csrfToken=randomHex(16);sessionLastSeen=millis();server.sendHeader("Set-Cookie","CGSESSION="+sessionToken+"; HttpOnly; SameSite=Strict");server.send(200,"application/json","{\"csrf\":\""+csrfToken+"\"}");});
  server.on("/api/logout",HTTP_POST,[]{if(!requireAuth(true))return;sessionToken="";server.sendHeader("Set-Cookie","CGSESSION=; Max-Age=0");server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/status",HTTP_GET,[]{if(!requireAuth())return;server.send(200,"application/json",statusJson());});
  server.on("/api/config",HTTP_GET,[]{if(!requireAuth())return;String s="{\"accel\":"+String(cfg.accelThresholdG)+",\"gyro\":"+String(cfg.gyroThresholdDps)+",\"persist\":"+String(cfg.persistenceMs)+",\"count\":"+String(cfg.countdownSec)+",\"rate\":"+String(cfg.sampleHz)+",\"cooldown\":"+String(cfg.cooldownSec)+",\"both\":"+(cfg.requireBoth?"true":"false")+",\"test\":"+(cfg.testMode?"true":"false")+",\"contacts\":[";for(int i=0;i<MAX_CONTACTS;i++){if(i)s+=',';s+="{\"name\":\""+jsonEscape(cfg.contacts[i].name)+"\",\"phone\":\""+jsonEscape(cfg.contacts[i].phone)+"\",\"enabled\":"+(cfg.contacts[i].enabled?"true":"false")+"}";}s+="]}";server.send(200,"application/json",s);});
  server.on("/api/config",HTTP_POST,[]{if(!requireAuth(true))return;float a=server.arg("accel").toFloat(),g=server.arg("gyro").toFloat();int p=server.arg("persist").toInt(),c=server.arg("count").toInt(),r=server.arg("rate").toInt(),d=server.arg("cooldown").toInt();if(a<1.5||a>16||g<50||g>2000||p<100||p>3000||c<5||c>120||r<20||r>100||d<30||d>3600){server.send(422,"application/json","{\"error\":\"setting_out_of_safe_range\"}");return;}cfg.accelThresholdG=a;cfg.gyroThresholdDps=g;cfg.persistenceMs=p;cfg.countdownSec=c;cfg.sampleHz=r;cfg.cooldownSec=d;cfg.requireBoth=server.arg("both")=="1";cfg.testMode=server.arg("test")=="1";saveConfig();server.send(200,"application/json","{\"message\":\"Settings saved\"}");});
  server.on("/api/contacts",HTTP_POST,[]{if(!requireAuth(true))return;for(int i=0;i<MAX_CONTACTS;i++){String p=server.arg("cp"+String(i));p.trim();if(p.length()&&!validPhone(p)){server.send(422,"application/json","{\"error\":\"invalid_phone_at_contact_"+String(i+1)+"\"}");return;}}for(int i=0;i<MAX_CONTACTS;i++){cfg.contacts[i].name=server.arg("cn"+String(i)).substring(0,30);cfg.contacts[i].phone=server.arg("cp"+String(i));cfg.contacts[i].enabled=server.arg("ce"+String(i))=="1";}saveConfig();server.send(200,"application/json","{\"message\":\"Contacts saved; no message sent\"}");});
  server.on("/api/calibrate",HTTP_POST,[]{if(!requireAuth(true))return;if(!t.mpuDetected){server.send(409,"application/json","{\"error\":\"sensor_not_detected\"}");return;}calibrationSamples=0;memset(calAccSum,0,sizeof(calAccSum));memset(calGyroSum,0,sizeof(calGyroSum));calAccSq=calGyroSq=0;calibrationRunning=true;server.send(202,"application/json","{\"message\":\"Calibration started; keep helmet still\"}");});
  server.on("/api/calibration/reset",HTTP_POST,[]{if(!requireAuth(true))return;memset(cfg.accelOffset,0,sizeof(cfg.accelOffset));memset(cfg.gyroOffset,0,sizeof(cfg.gyroOffset));cfg.calibrated=false;saveConfig();server.send(200,"application/json","{\"message\":\"Calibration offsets reset\"}");});
  server.on("/api/simulate",HTTP_POST,[]{if(!requireAuth(true))return;if(!cfg.testMode){server.send(409,"application/json","{\"error\":\"enable_test_mode_first\"}");return;}if(state!=MONITORING){server.send(409,"application/json","{\"error\":\"system_not_ready\"}");return;}simulatedCandidate=true;server.send(202,"application/json","{\"message\":\"Safe simulation started\"}");});
  server.on("/api/gsm/check",HTTP_POST,[]{if(!requireAuth(true))return;if(smsState!=SMS_IDLE){server.send(409,"application/json","{\"error\":\"modem_busy\"}");return;}gsmWrite("AT");smsState=SMS_AT;server.send(202,"application/json","{\"message\":\"Modem diagnostics queued\"}");});
  server.on("/api/sms/test",HTTP_POST,[]{if(!requireAuth(true))return;if(lastTestSms&&!elapsed(lastTestSms,60000)){server.send(429,"application/json","{\"error\":\"wait_60_seconds_between_tests\"}");return;}int i=server.arg("index").toInt();if(i<0||i>=MAX_CONTACTS||!validPhone(cfg.contacts[i].phone)){server.send(422,"application/json","{\"error\":\"select_a_valid_contact\"}");return;}if(state!=MONITORING){server.send(409,"application/json","{\"error\":\"system_busy\"}");return;}lastTestSms=millis();smsBody="CrashGuard authorized test SMS. No emergency has been detected.";forcedSmsContact=i;smsContactIndex=i;smsState=SMS_NEXT;changeState(SMS_SENDING,"authorized test SMS");server.send(202,"application/json","{\"message\":\"Test SMS queued; check status for modem result\"}");});
  server.on("/api/logs",HTTP_GET,[]{if(!requireAuth())return;String s="{\"logs\":[";for(int i=0;i<logCount;i++){int x=(logHead+LOG_CAPACITY-logCount+i)%LOG_CAPACITY;if(i)s+=',';s+="{\"time\":"+String(logs[x].ms/1000)+",\"text\":\""+jsonEscape(logs[x].text)+"\"}";}s+="]}";server.send(200,"application/json",s);});
  server.onNotFound(sendPage);
}

void setup(){
#ifdef CRASHGUARD_ESP8266
  Serial.begin(9600); // dedicated to SIM800L; do not open USB Serial Monitor
#else
  Serial.begin(115200);
#endif
  pinMode(PIN_BUZZER,OUTPUT);digitalWrite(PIN_BUZZER,LOW);pinMode(PIN_CANCEL,INPUT_PULLUP);loadConfig();logEvent("Boot firmware "+String(FW_VERSION));changeState(HARDWARE_CHECK,"initializing");
  Wire.begin(PIN_MPU_SDA,PIN_MPU_SCL);t.mpuDetected=mpu.begin(0x68,&Wire);if(t.mpuDetected){mpu.setAccelerometerRange(MPU6050_RANGE_16_G);mpu.setGyroRange(MPU6050_RANGE_2000_DEG);mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);logEvent("MPU6050 detected");}else setError("MPU6050 not detected at 0x68");
#ifdef CRASHGUARD_ESP8266
  GPS_UART.begin(9600);
#else
  GPS_UART.begin(9600,SERIAL_8N1,PIN_GPS_RX,PIN_GPS_TX);GSM_UART.begin(9600,SERIAL_8N1,PIN_GSM_RX,PIN_GSM_TX);
#endif
  WiFi.mode(WIFI_AP);WiFi.softAP(cfg.apSsid.c_str());delay(50);dns.start(DNS_PORT,"*",WiFi.softAPIP());setupRoutes();server.begin();logEvent("Setup AP ready at "+WiFi.softAPIP().toString());
  // Physical recovery: hold cancel during boot for 8 seconds to erase only admin password/session.
  if(digitalRead(PIN_CANCEL)==LOW){uint32_t held=millis();while(digitalRead(PIN_CANCEL)==LOW&&millis()-held<8000){delay(10);}if(millis()-held>=8000){resetAdminPassword();logEvent("Administrator password reset by physical hold");}}
  changeState(t.mpuDetected?MONITORING:FAULT,t.mpuDetected?"ready":"sensor required");
}
void loop(){dns.processNextRequest();server.handleClient();pollGps();pollGsm();sampleMpu();updateCrashWorkflow();delay(1);}
