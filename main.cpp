#include <ArduinoOTA.h>
#include <ESP32Time.h>
#include <ESPmDNS.h>
#include <RTClib.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <aREST.h>
#include <pwmWrite.h>

#include "secrets.h"
#include "time.h"

#define PWM_PIN 5  // turn tubes on/off or control with PWM
#define EN_PIN 4   // latch pin of shift registers on drivers

#define HOURS_MS_DELAY 1000
#define SECS_MULTIPLE 10

#define MIN_BRIGHTNESS 0 // 255 raw
#define DAY_MODE_BRIGHTNESS_DEFAULT 100 // 0 raw
#define NIGHT_MODE_BRIGHTNESS_DEFAULT 14 // 220 raw
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

aREST rest = aREST();
hw_timer_t* delayTimer;
ESP32Time espRtc;
RTC_DS3231 ds3231Rtc;
Pwm pwm;
WiFiServer server(80);

// NTP config
const char* NtpServer = "pool.ntp.org";
const long GmtOffsetSecs = -28800;
const int DstOffsetSecs = 3600;

// used as timers in conjunction with delayTimerISR
volatile long animationDelayMs = 0;
volatile long brightnessDelayMs = 0;
volatile long timeSyncDelayMs = 0;
volatile long wifiStatusDelayMs = 0;

void IRAM_ATTR delayTimerISR() {
  animationDelayMs--;
  brightnessDelayMs--;
  timeSyncDelayMs--;
  wifiStatusDelayMs--;
}

// tracks state of animation
byte tensDigit;
byte onesDigit;
int cyclesLeft;
bool animation = false;
bool hourDisplayed = false;

// track state of tube PWM
bool displayOn = true;
bool isNightMode;
int brightness = 0;
int brightnessNight = NIGHT_MODE_BRIGHTNESS_DEFAULT;
int brightnessDay = DAY_MODE_BRIGHTNESS_DEFAULT;

// rest api info
String lastLog1 = "";
String lastLog2 = "";
String lastLog3 = "";
int wifiDisconnects = 0;

//                  //
// helper functions //
//                  //

void display(uint8_t tube2, uint8_t tube1) {
  digitalWrite(EN_PIN, LOW);
  SPI.transfer(nixieDigitArray[tube2] >> 8);
  SPI.transfer(nixieDigitArray[tube2]);
  SPI.transfer(nixieDigitArray[tube1] >> 8);
  SPI.transfer(nixieDigitArray[tube1]);
  digitalWrite(EN_PIN, HIGH);
}

void log(String message) {
  Serial.println(message);
  lastLog3 = lastLog2;
  lastLog2 = lastLog1;
  lastLog1 = "[" + ds3231Rtc.now().timestamp(DateTime::TIMESTAMP_FULL) + "] " + message;
}

int roundUpToMultiple(int second, int multiple) {
  int new_second = ((second / multiple) + 1) * multiple;
  return (new_second - second);
}

int restart(String notUsed) {
  ESP.restart();
  return 0;
}

int runTimeSync(String notUsed) {
  log("Adjusting DS3231 with NTP time");
  timeSyncDelayMs = 0;
  return 0;
}

int setDisplay(String stateStr) {
  if (stateStr == "on") {
    log("Turning display on");
    brightnessDelayMs = 0;
    displayOn = true;
    return 0;
  }
  else if (stateStr == "off") {
    log("Turning display off");
    brightnessDelayMs = 0;
    displayOn = false;
    return 0;
  }
  else {
    log("Invalid display state");
    return 1;
  }
}
  
int setBrightness(String brightnessStr) {
  int brightness = -1;
  try {
    brightness = brightnessStr.toInt();
  }
  catch (const std::exception& e) {
    log("Invalid brightness value: " + brightnessStr);
    return 1;
  }

  brightnessStr = String(brightness);
  if (brightness < 0|| brightness > 100) {
    log("Brightness out of range:" + brightnessStr);
    return 1;
  }

  if (isNightMode) {
    brightnessNight = brightness;
  }
  else {
    brightnessDay = brightness;
  }

  log("Setting brightness to " + brightnessStr);
  brightnessDelayMs = 0;
  displayOn = brightness > 0;
  return 0;
}

//                 //
// setup functions //
//                 //

void otaSetup() {
  Serial.println("OTA setting up...");

  ArduinoOTA.setHostname("RZ568M-Nixie-Clock");

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

  Serial.println("OTA setup complete.");
}

void restSetup() {
  // aREST config
  rest.function((char*)"restart", restart);
  rest.function((char*)"runTimeSync", runTimeSync);
  rest.function((char*)"setDisplay", setDisplay);
  rest.function((char*)"setBrightness", setBrightness);
  rest.variable("brightness", &brightness);
  rest.variable("brightnessDay", &brightnessDay);
  rest.variable("brightnessNight", &brightnessNight);
  rest.variable("displayOn", &displayOn);
  rest.variable("isNightMode", &isNightMode);
  rest.variable("lastLog1", &lastLog1);
  rest.variable("lastLog2", &lastLog2);
  rest.variable("lastLog3", &lastLog3);
  rest.variable("wifiDisconnects", &wifiDisconnects);
  rest.set_id("041823");
  rest.set_name((char*)"RZ568M-Nixie-Clock");

  // start rest server
  server.begin();
}

void wifiSetup() {
  Serial.println("Wifi setting up...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  Serial.print("Wifi ready, IP address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  delay(5000);
  Serial.begin(115200);
  Serial.println("Starting setup...");

  // initalize Nixie driver pins
  SPI.begin();
  pinMode(PWM_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(PWM_PIN, HIGH);
  digitalWrite(EN_PIN, LOW);
  digitalWrite(PWM_PIN, LOW);

  display(9,9);

  // RTC init
  if (!ds3231Rtc.begin()) {
    Serial.println("Couldn't find RTC");
    //while (1) ;
  }
  display(8,8);


  wifiSetup();
  otaSetup();
  restSetup();
  display(7,7);
  
  // NTP config
  configTime(GmtOffsetSecs, DstOffsetSecs, NtpServer);
  display(6,6);

  // setup delay timer interrupt
  delayTimer = timerBegin(0, 80, true);  // 80Mhz / 80 = 1Mhz, 1us count
  timerAttachInterrupt(delayTimer, &delayTimerISR, true);
  timerAlarmWrite(delayTimer, 1000, true);
  timerAlarmEnable(delayTimer);
  display(5,5);

  log("Setup complete");
}

//                //
// loop functions //
//                //

void checkWifiStatus() {
  if (wifiStatusDelayMs < 0) {
    try {
      if (WiFi.status() != WL_CONNECTED) {
        log("Reconnecting to WiFi...");
        WiFi.disconnect();
        WiFi.reconnect();
        wifiDisconnects++;
        log("Reconnected to WiFi");
      }
    }
    catch (const std::exception& e) {
      log("Wifi error:" + String(e.what()));
      wifiStatusDelayMs = 10 * 60 * 1000; // 10   minutes
    }

    wifiStatusDelayMs = 60 * 1000; // 1 minute
  }
}

void handleAnimation() {
  if (animationDelayMs < 0) {
    DateTime now = ds3231Rtc.now();

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
                //: roundUpToMultiple(espRtc.getSecond(), SECS_MULTIPLE) * 1000;
                : roundUpToMultiple(now.second(), SECS_MULTIPLE) * 1000;
      }
    } else {
      if (hourDisplayed) {
        //int minute = espRtc.getMinute();
        int minute = now.minute();
        tensDigit = minute / 10;
        onesDigit = minute % 10;
        cyclesLeft = 1 * 10;
      } else {
        //int hour = espRtc.getHour();
        int hour = now.hour() % 12;
        if (hour == 0) hour = 12;
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
  // avoid interrupting animation
  if (/*!animation && */brightnessDelayMs < 0) {
    Serial.println("[handleBrightness]");
    
    int delaySecs;
    if (displayOn) {
      DateTime now = ds3231Rtc.now();
      //int curMins = espRtc.getHour(true) * 60 + espRtc.getMinute();
      int curMins = now.hour() * 60 + now.minute();
      int minsToDay = (6 * 60 - curMins + 1440) % 1440; // 6 AM
      int minsToNight = (21 * 60 - curMins + 1440) % 1440; // 9 PM

      isNightMode = minsToDay < minsToNight;
      brightness = isNightMode ? brightnessNight : brightnessDay;
      delaySecs = isNightMode ? minsToDay : minsToNight;

      Serial.printf("isNightMode: %u\n", isNightMode);
    }
    else {
      brightness = MIN_BRIGHTNESS;
      delaySecs = INT_MAX;
    }
    
    Serial.printf(" brightness: %u\n", brightness);
    Serial.printf("  delaySecs: %u\n", delaySecs);
    Serial.printf("    display: %u\n", displayOn);

    int brightnessRaw = (255.0 - 2.55 * brightness);
    pwm.write(PWM_PIN, brightnessRaw, PWM_FREQUENCY);

    brightnessDelayMs = delaySecs * 1000;
  }
}

void handleRestRequest() {
  WiFiClient client = server.available();
  if (client) {
    unsigned long timeout = millis() + 3000;
    while (!client.available() && millis() <= timeout) ;

    if (!client.available()) {
      log("Client connection timed out");
      return;
    }

    Serial.println("[handleRestRequest] handling request...");
    rest.handle(client);
  }
}

void handleTimeSync() {
  // avoid interrupting animation
  if (!animation && timeSyncDelayMs < 0) {
    Serial.println("[handleTimeSync]");
    display(4,4);

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
    ds3231Rtc.adjust(DateTime(yr, mt, dy, hr, mi, se));
    Serial.printf("Adjusted DS3231 with NTP time");

    timeSyncDelayMs = 1000 * 60 * 60 * 24;  // 1 day
    display(3,3);
  }
}

void loop() {
  handleTimeSync();
  handleBrightness();
  handleAnimation();
  handleRestRequest();
  ArduinoOTA.handle();

  checkWifiStatus();
}