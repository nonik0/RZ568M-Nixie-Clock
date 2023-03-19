#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <SPI.h>
//#include <SoftPWM.h>
#include <RTClib.h>
#include "secrets.h"

#define DEBUG

#define PWM_PIN         5 // turn tubes on/off or control with PWM
#define EN_PIN          4 // latch pin of shift registers on drivers

#define HOURS_DELAY    1000
#define SECS_MULTIPLE  10

RTC_DS3231 rtc;

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
  Serial.begin(115200);
  Serial.println("Booting");
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
    
void delayToMultiple(int second, int multiple){
  int delay_ms = roundUpToMultiple(second, multiple);
  delay(delay_ms);
}

void cycleTo(byte tensDigit, byte onesDigit, int cycles) {
#if defined(DEBUG)
  Serial.print(tensDigit);
  Serial.println(onesDigit);
#endif
  float delay_ms = 15;
  //int cycle_time_ms = (cycles * 10) * delay_ms; // 3 cycles -> 1000, 1 cycle -> 333
  
  int count = cycles * 10;
  while (count-- >= 0) {
    display(tensDigit, onesDigit);
    delay(delay_ms);

    tensDigit = (tensDigit+1)%10;
    onesDigit = (onesDigit+1)%10;
  }
}

void swap(int *a, int *b)
{
    int temp = *a;
    *a = *b;
    *b = temp;
}

int digit1Order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
int digit2Order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
void shuffleDigitOrder()
{
    //int digitOrder[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int i = 9; i > 0; i--)
    {
        long j = random(0, 10);
        swap(&digit2Order[i], &digit2Order[j]);

        j = random(0, 10);
        swap(&digit1Order[i], &digit1Order[j]);
    }
}

void shuffleTo(byte tensDigit, byte onesDigit, int cycles) {
  float delay_ms = 100/3.0;

  while (cycles >= 0) {
    shuffleDigitOrder();

// #if defined(DEBUG)
//     Serial.print("2: [");
//     for(int i = 0; i < 10; i++) {
//       Serial.print(digit2Order[i]);
//       Serial.print(",");
//     }
//     Serial.println("]");

//     Serial.print("1: [");
//     for(int i = 0; i < 10; i++) {
//       Serial.print(digit1Order[i]);
//       Serial.print(",");
//     }
//     Serial.println("]");
// #endif

    for (int i = 0; i < 10; i++) {
      display(digit2Order[i], digit1Order[i]);
      delay(delay_ms);
    }

    cycles--;
  }

  display(tensDigit, onesDigit);
}

void setup() 
{
#if defined(DEBUG)
  Serial.begin(9600);
#endif

  // initalize Nixie driver pins
  pinMode(PWM_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(PWM_PIN, HIGH);
  digitalWrite(EN_PIN, LOW);
  //SoftPWMBegin();
  //SoftPWMSet(PWM_PIN, 1);
  SPI.begin();

  if (!rtc.begin()) {
#if defined(DEBUG)
    Serial.println("Couldn't find RTC");
#endif
    while (1);
  }
  // TODO
  //rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  display(0, 0);
  digitalWrite(PWM_PIN, LOW);

  //randomSeed(analogRead(A0));

  otaSetup();

#if defined(DEBUG)
    Serial.println("Setup complete");
#endif
}


unsigned long nextSec = 0;
unsigned long nextHour = 0;

void loop() 
{
  // for(int i = 0; i <= 9; i++)
  // {
  //   SoftPWMSetPercent(PWM_PIN, (100 - brightnessArray[i%5]));
  //   NixieDisplay(i, i);
  //   delay(1000);
  // }
  // if (now.second() is multiple)
  DateTime now = rtc.now();
  int hour = now.hour() % 12;
  cycleTo(hour / 10, hour % 10, 10);
  delay(HOURS_DELAY);
  int minute = now.minute();
  cycleTo(minute / 10, minute % 10, 1);

  // TODO: poll instead of delay
  // delay to next seconds multiple (i.e. repeat at X:10, X:20, etc.)
  delayToMultiple(now.second(), SECS_MULTIPLE);

  ArduinoOTA.handle();
}
