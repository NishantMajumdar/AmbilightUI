#include <Arduino.h>
#include <FastLED.h>

// -------------------- ADA LIGHT SETTINGS --------------------

const uint16_t Num_Leds = 100;
const uint8_t Brightness = 255;

#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define PIN_DATA 6

const unsigned long SerialSpeed = 500000;
const uint16_t SerialTimeout = 60;

#define SERIAL_FLUSH

// -------------------- RGB + LDR PINS --------------------

#define BLUE_PIN 9
#define GREEN_PIN 10
#define RED_PIN 11
#define LDR_PIN A0

// -------------------- LDR TIMING --------------------

unsigned long lastLDRSend = 0;
const unsigned long LDRInterval = 100; // send every 100ms

// ------------------------------------------------------------

CRGB leds[Num_Leds];

const uint8_t magic[] = {'A','d','a'};
#define MAGICSIZE sizeof(magic)

#define HICHECK (MAGICSIZE)
#define LOCHECK (MAGICSIZE + 1)
#define CHECKSUM (MAGICSIZE + 2)

enum processModes_t {Header, Data} mode = Header;

uint32_t ledIndex;
uint32_t ledsRemaining;

unsigned long lastByteTime;
unsigned long lastAckTime;

unsigned long (*const now)(void) = millis;
const unsigned long Timebase = 1000;

// ------------------------------------------------------------

void headerMode(uint8_t c);
void dataMode(uint8_t c);
void timeouts();

// ------------------------------------------------------------

void setup() {

  FastLED.addLeds<LED_TYPE, PIN_DATA, COLOR_ORDER>(leds, Num_Leds);
  FastLED.setBrightness(Brightness);

  pinMode(BLUE_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);

  Serial.begin(SerialSpeed);
  Serial.print("Ada\n");

  lastByteTime = lastAckTime = millis();
}

// ------------------------------------------------------------

void loop() {

  // -------- Periodic LDR Send (Independent of Adalight) --------
  if (millis() - lastLDRSend > LDRInterval) {
    lastLDRSend = millis();
    int ldrValue = analogRead(LDR_PIN);
    Serial.print("L");
    Serial.println(ldrValue);
  }

  const int c = Serial.read();

  if(c >= 0) {
    lastByteTime = lastAckTime = now();

    switch(mode) {
      case Header:
        headerMode(c);
        break;
      case Data:
        dataMode(c);
        break;
    }
  }
  else {
    timeouts();
  }
}

// ------------------------------------------------------------

void headerMode(uint8_t c) {

  static uint8_t headPos, hi, lo, chk;

  if(headPos < MAGICSIZE) {
    if(c == magic[headPos]) headPos++;
    else headPos = 0;
  }
  else {
    switch(headPos) {
      case HICHECK:
        hi = c;
        headPos++;
        break;

      case LOCHECK:
        lo = c;
        headPos++;
        break;

      case CHECKSUM:
        chk = c;
        if(chk == (hi ^ lo ^ 0x55)) {
          ledIndex = 0;
          ledsRemaining = (256UL * hi + lo + 1UL);
          mode = Data;
        }
        headPos = 0;
        break;
    }
  }
}

// ------------------------------------------------------------

void dataMode(uint8_t c) {

  static uint8_t channelIndex = 0;

  if (ledIndex < Num_Leds) {
    leds[ledIndex].raw[channelIndex] = c;
  }

  channelIndex++;

  if (channelIndex >= 3) {
    channelIndex = 0;
    if (ledIndex < Num_Leds) ledIndex++;
    ledsRemaining--;
  }

  if (ledsRemaining == 0) {

    FastLED.show();
    mode = Header;

    // ---------------- RGB CONTROL PACKET ----------------
    if (Serial.available() >= 4) {
      if (Serial.read() == 'X') {

        uint8_t r = Serial.read();
        uint8_t g = Serial.read();
        uint8_t b = Serial.read();

        analogWrite(RED_PIN, r);
        analogWrite(GREEN_PIN, g);
        analogWrite(BLUE_PIN, b);
      }
    }

#ifdef SERIAL_FLUSH
    while(Serial.available() > 0) Serial.read();
#endif
  }
}

// ------------------------------------------------------------

void timeouts() {

  const unsigned long t = now();

  if((t - lastAckTime) >= Timebase) {

    Serial.print("Ada\n");
    lastAckTime = t;

    if(SerialTimeout != 0 &&
       (t - lastByteTime) >= (uint32_t)SerialTimeout * Timebase) {

      memset(leds, 0, Num_Leds * sizeof(CRGB));
      FastLED.show();
      mode = Header;
      lastByteTime = t;
    }
  }
}