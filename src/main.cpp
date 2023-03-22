#include <ArduinoOTA.h>
#include <ESP32Time.h>
#include <ESPmDNS.h>
#include <pwmWrite.h>
#include <RTClib.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "secrets.h"
#include "time.h"

#define PWM_PIN                 5 // turn tubes on/off or control with PWM
#define EN_PIN                  4 // latch pin of shift registers on drivers

#define HOURS_MS_DELAY       1000
#define SECS_MULTIPLE          10

#define DAY_MODE_BRIGHTNESS     0
#define NIGHT_MODE_BRIGHTNESS 220
#define PWM_FREQUENCY         200

const char* NtpServer = "pool.ntp.org";
const long  GmtOffsetSecs = -28800;
const int   DstOffsetSecs = 3600;

ESP32Time espRtc;
RTC_DS3231 ds3231Rtc;
Pwm pwm;

uint16_t nixieDigitArray[10]
{
  0b0000000000100000,  // 0
  0b0000010000000000,  // 1
  0b0000000001000000,  // 2
  0b1000000000000000,  // 3
  0b0000000010000000,  // 4
  0b0001000000000000,  // 5
  0b0000000000010000,  // 6
  0b0100000000000000,  // 7
  0b0000001000000000,  // 8
  0b0000100000000000   // 9
};

void otaSetup() {
  Serial.println("OTA setting up...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  //ArduinoOTA.setHostname("TinyS3");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void display(uint8_t tube2, uint8_t tube1)
{
  digitalWrite(EN_PIN, LOW);
  SPI.transfer(nixieDigitArray[tube2] >> 8);
  SPI.transfer(nixieDigitArray[tube2]);
  SPI.transfer(nixieDigitArray[tube1] >> 8);
  SPI.transfer(nixieDigitArray[tube1]);
  digitalWrite(EN_PIN, HIGH);
}

int roundUpToMultiple(int second, int multiple) {
  int new_second = ((second / multiple) + 1) * multiple;
  return (new_second - second)*1000;
}
    
void cycleTo(byte tensDigit, byte onesDigit, int cycles) {
  float delay_ms = 15; 
  int count = cycles * 10;
  while (count-- >= 0) {
    display(tensDigit, onesDigit);
    delay(delay_ms);

    tensDigit = (tensDigit+1)%10;
    onesDigit = (onesDigit+1)%10;
  }
}

bool hourDisplayed = false;
unsigned long displayDelayStart = 0;
unsigned long displayDelayMs = 0;
void handleDisplay() {
  if ((millis() - displayDelayStart) > displayDelayMs) {
    if (hourDisplayed) {     
      int minute = espRtc.getMinute();
      cycleTo(minute / 10, minute % 10, 1);
      displayDelayMs = roundUpToMultiple(espRtc.getSecond(), SECS_MULTIPLE);
    }
    else {
      int hour = espRtc.getHour();
      cycleTo(hour / 10, hour % 10, 10);
      displayDelayMs = HOURS_MS_DELAY;
    }

    hourDisplayed = !hourDisplayed;
    displayDelayStart = millis();
  }
}

unsigned long brightnessDelayStart = 0;
unsigned long brightnessDelayMs = 3000;
void handleBrightness() {
  if ((millis() - brightnessDelayStart) > brightnessDelayMs) {
    Serial.println("[handleBrightness]");

    int curMins = espRtc.getHour(true) * 60 + espRtc.getMinute();
    int minsToDay = (6*60 - curMins + 1440) % 1440;
    int minsToNight = (23*60 - curMins + 1440) % 1440;
    Serial.printf("    curMins: %u\n", curMins);
    Serial.printf("  minsToDay: %u\n", minsToDay);
    Serial.printf("minsToNight: %u\n", minsToNight);

    bool isNight = minsToDay < minsToNight;
    int brightness = isNight ? NIGHT_MODE_BRIGHTNESS : DAY_MODE_BRIGHTNESS;
    int delaySecs = isNight ? minsToNight : minsToDay;
    Serial.printf(" brightness: %u\n", brightness);
    Serial.printf("  delaySecs: %u\n", delaySecs);

    pwm.write(PWM_PIN, brightness, 200);

    brightnessDelayStart = millis();
    brightnessDelayMs = delaySecs * 1000; 
  }
}

bool adjustedOnce = true;
unsigned long timeSyncDelayStart = 0;
unsigned long timeSyncDelayMs = 5000; // easier to see in serial output
void handleTimeSync() {
  if ((millis() - timeSyncDelayStart) > timeSyncDelayMs) {
    Serial.println("[handleTimeSync]");

    struct tm timeinfo;
    getLocalTime(&timeinfo);

    int yr = timeinfo.tm_year + 1900;
    int mt = timeinfo.tm_mon + 1;
    int dy = timeinfo.tm_mday;
    int hr = timeinfo.tm_hour;
    int mi = timeinfo.tm_min;
    int se = timeinfo.tm_sec;

    char format[] = "hh:mm:ss";
    Serial.printf("DS3231: %s\n", ds3231Rtc.now().toString(format));
    Serial.printf("   ESP: %s\n", espRtc.getTime());
    Serial.printf("   NTP: %02u:%02u:%02u\n", hr, mi, se);

    // debug to test RTC drift
    if (!adjustedOnce) {
      Serial.println("Adjusting RTCs with NTP time");
      ds3231Rtc.adjust(DateTime(yr, mt, dy, hr, mi, se));
      espRtc.setTimeStruct(timeinfo);

      adjustedOnce = true;
    }
    
    timeSyncDelayStart = millis();
    //timeSyncDelayMs = 1000*60*60*24*7; // 1 week
    timeSyncDelayMs = 1000*60*10;
  }
}

// unsigned int cycleCount = 0;
// unsigned long debugDelayStart = 0;
// unsigned long debugDelayMs = 2000;
// void handleDebug() {
//   if ((millis() - debugDelayStart) > debugDelayMs) {
//     Serial.println("[handleDebug]");

//     unsigned long totalDelayMs = millis() - debugDelayStart;
//     float cyclesPerSec = cycleCount / ( totalDelayMs / 1000.0 );

//     Serial.printf("  cycleCount: %u\n", cycleCount);
//     Serial.printf("totalDelayMs: %u\n", totalDelayMs);
//     Serial.printf("cyclesPerSec: %f\n", cyclesPerSec);

//     cycleCount = 0;
//     debugDelayStart = millis();
//   }
// }

void setup() 
{
  Serial.begin(115200);
  Serial.println("Starting setup...");

  // initalize Nixie driver pins
  pinMode(PWM_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(PWM_PIN, HIGH);
  digitalWrite(EN_PIN, LOW);
  SPI.begin();

  // RTC init
  if (!ds3231Rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  // NTP config
  configTime(GmtOffsetSecs, DstOffsetSecs, NtpServer);

  // tubes init
  // display(0, 0);
  // pwm.write(PWM_PIN, 0);

  otaSetup();

  Serial.println("Setup complete");
}

void loop() 
{
  handleBrightness();
  handleDisplay();
  handleTimeSync();
  ArduinoOTA.handle();
}
