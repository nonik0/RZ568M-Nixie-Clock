#include <ArduinoOTA.h>
#include <ESP32Time.h>
#include <ESPmDNS.h>
#include <RTClib.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <pwmWrite.h>

#include "secrets.h"
#include "time.h"

#define PWM_PIN 5  // turn tubes on/off or control with PWM
#define EN_PIN 4   // latch pin of shift registers on drivers

#define HOURS_MS_DELAY 1000
#define SECS_MULTIPLE 10

#define DAY_MODE_BRIGHTNESS 0
#define NIGHT_MODE_BRIGHTNESS 220
#define PWM_FREQUENCY 200

#define ANIMATION_REFRESH_MS 15

// specific to Nixie sockets used
const uint16_t nixieDigitArray[10]{
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

ESP32Time espRtc;
RTC_DS3231 ds3231Rtc;
Pwm pwm;
hw_timer_t* delayTimer;

// NTP config
const char* NtpServer = "pool.ntp.org";
const long GmtOffsetSecs = -28800;
const int DstOffsetSecs = 3600;

// used as timers in conjunction with countdownTimerISR
volatile long animationDelayMs = 0;
volatile long brightnessDelayMs = 3000;
volatile long timeSyncDelayMs = 5000;

// tracks state of animation
byte tensDigit;
byte onesDigit;
int cyclesLeft;
bool animation = false;
bool hourDisplayed = false;

void IRAM_ATTR delayTimerISR() {
  animationDelayMs--;
  brightnessDelayMs--;
  timeSyncDelayMs--;
}

void otaSetup() {
  Serial.println("OTA setting up...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else  // U_SPIFFS
          type = "filesystem";

        Serial.println("Start updating " + type);
      })
      .onEnd([]() { Serial.println("\nEnd"); })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
          Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
          Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
          Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
          Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
          Serial.println("End Failed");
      });

  ArduinoOTA.begin();

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void display(uint8_t tube2, uint8_t tube1) {
  digitalWrite(EN_PIN, LOW);
  SPI.transfer(nixieDigitArray[tube2] >> 8);
  SPI.transfer(nixieDigitArray[tube2]);
  SPI.transfer(nixieDigitArray[tube1] >> 8);
  SPI.transfer(nixieDigitArray[tube1]);
  digitalWrite(EN_PIN, HIGH);
}

int roundUpToMultiple(int second, int multiple) {
  int new_second = ((second / multiple) + 1) * multiple;
  return (new_second - second);
}

void handleAnimation() {
  if (animationDelayMs < 0) {
    if (animation) {
      display(tensDigit, onesDigit);
      tensDigit = (tensDigit + 1) % 10;
      onesDigit = (onesDigit + 1) % 10;
      cyclesLeft--;

      if (cyclesLeft >= 0) {
        animationDelayMs = ANIMATION_REFRESH_MS;
      } else {
        animation = false;
        animationDelayMs =
            hourDisplayed
                ? HOURS_MS_DELAY
                : roundUpToMultiple(espRtc.getSecond(), SECS_MULTIPLE) * 1000;
      }
    } else {
      if (hourDisplayed) {
        int minute = espRtc.getMinute();
        tensDigit = minute / 10;
        onesDigit = minute % 10;
        cyclesLeft = 1 * 10;
      } else {
        int hour = espRtc.getHour();
        tensDigit = hour / 10;
        onesDigit = hour % 10;
        cyclesLeft = 10 * 10;
      }

      hourDisplayed = !hourDisplayed;
      animation = true;
      animationDelayMs = ANIMATION_REFRESH_MS;
    }
  }
}

void handleBrightness() {
  if (brightnessDelayMs < 0) {
    Serial.println("[handleBrightness]");

    int curMins = espRtc.getHour(true) * 60 + espRtc.getMinute();
    int minsToDay = (6 * 60 - curMins + 1440) % 1440;
    int minsToNight = (21 * 60 - curMins + 1440) % 1440;
    Serial.printf("    curMins: %u\n", curMins);
    Serial.printf("  minsToDay: %u\n", minsToDay);
    Serial.printf("minsToNight: %u\n", minsToNight);

    bool isNight = minsToDay < minsToNight;
    int brightness = isNight ? NIGHT_MODE_BRIGHTNESS : DAY_MODE_BRIGHTNESS;
    int delaySecs = isNight ? minsToNight : minsToDay;
    Serial.printf(" brightness: %u\n", brightness);
    Serial.printf("  delaySecs: %u\n", delaySecs);

    pwm.write(PWM_PIN, brightness, PWM_FREQUENCY);

    brightnessDelayMs = delaySecs * 1000;
  }
}

void handleTimeSync() {
  if (timeSyncDelayMs < 0) {
    Serial.println("[handleTimeSync]");

    char format[] = "hh:mm:ss";
    Serial.printf("DS3231: %s\n", ds3231Rtc.now().toString(format));
    Serial.printf("   ESP: %s\n", espRtc.getTime());

    struct tm timeinfo;
    getLocalTime(&timeinfo); // this adjusts ESP32 RTC

    int yr = timeinfo.tm_year + 1900;
    int mt = timeinfo.tm_mon + 1;
    int dy = timeinfo.tm_mday;
    int hr = timeinfo.tm_hour;
    int mi = timeinfo.tm_min;
    int se = timeinfo.tm_sec;

    Serial.printf("   NTP: %02u:%02u:%02u\n", hr, mi, se);
    Serial.println("Adjusting DS3231 with NTP time");
    ds3231Rtc.adjust(DateTime(yr, mt, dy, hr, mi, se));

    timeSyncDelayMs = 1000 * 60 * 60 * 24;  // 1 day
  }
}

void setup() {
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
    while (1)
      ;
  }

  // NTP config
  configTime(GmtOffsetSecs, DstOffsetSecs, NtpServer);

  otaSetup();

  // setup delay timer interrupt
  delayTimer = timerBegin(0, 80, true);  // 80Mhz / 80 = 1Mhz, 1us count
  timerAttachInterrupt(delayTimer, &delayTimerISR, true);
  timerAlarmWrite(delayTimer, 1000, true);
  timerAlarmEnable(delayTimer);

  Serial.println("Setup complete");
}

void loop() {
  handleAnimation();
  handleBrightness();
  handleTimeSync();
  ArduinoOTA.handle();
}
