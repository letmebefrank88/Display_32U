#include <Arduino.h>
#include <U8g2lib.h>
#include "Bitmaps.h"
#include <Preferences.h>

// External SSD1322 256x64 SPI OLED 
#define OLED_MOSI  25 // grey
#define OLED_CS    26 // yellow
#define OLED_DC    27 // orange
#define OLED_CLK   14 // green
#define OLED_RST   12 // purple

// UART pin definitions
#define UART_RX 16
#define UART_TX 17
HardwareSerial SerialUART(1);
Preferences prefs;

U8G2_SSD1322_NHD_256X64_F_4W_SW_SPI u8g2(U8G2_R0, /* clock=*/ OLED_CLK, /* data=*/ OLED_MOSI, /* cs=*/ OLED_CS, /* dc=*/ OLED_DC, /* reset=*/ OLED_RST);

// State variables for display
volatile bool playing = false;
char lastTitle[32] = "?";
char lastArtist[32] = "?";

// Debouncing
const unsigned long debounceMs = 20;

// For scrolling
unsigned long scrollTimerTitle = 0;
unsigned long scrollTimerArtist = 0;
const unsigned long scrollInterval = 20;
int titleScroll = 0;
int artistScroll = 0;

// Screensaver state
bool showTuner = false;
unsigned long lastActivityMillis = 0;
const unsigned long screensaverDelay = 15000;

// Brightness indicator
bool showBrightnessIndicator = false;
unsigned long brightnessIndicatorStart = 0;
const unsigned long brightnessIndicatorDelay = 2000; // Show for 2 seconds

// Brightness Control
const uint8_t brightnessLevels[] = {16, 48, 96, 160, 255}; // 5 levels: Very Dim, Dim, Medium, Bright, Very Bright
const char* brightnessNames[] = {"Very Dim", "Dim", "Medium", "Bright", "Very Bright"};
const int numBrightnessLevels = sizeof(brightnessLevels) / sizeof(brightnessLevels[0]);
int brightnessIndex = 2; // Start at Medium brightness

// EQ mode
bool eqMode = false;
unsigned long eqBtnPressStart = 0;
const unsigned long eqLongPressMs = 20;

// --- EQ Band Data ---
enum EQBand { BAND_BASS = 0, BAND_MID = 1, BAND_TREBLE = 2 };
const char* eqBandNames[3] = {"BASS", "MID", "TREBLE"};
int eqBand = 0; // 0: Bass, 1: Mid, 2: Treble
float eqGains[3] = {0, 0, 0}; // dB for each band
const float eqMinGain = -12.0f;
const float eqMaxGain = 12.0f;

// --- UI Drawing Functions (copied/adapted) ---
void drawBrightnessIndicator() {
  // Draw brightness overlay in center of screen
  const int boxW = 160;
  const int boxH = 40;
  const int boxX = (256 - boxW) / 2;
  const int boxY = (64 - boxH) / 2;
  
  // Draw background box
  u8g2.setDrawColor(1);
  u8g2.drawRBox(boxX, boxY, boxW, boxH, 4);
  u8g2.setDrawColor(0);
  u8g2.drawRBox(boxX + 2, boxY + 2, boxW - 4, boxH - 4, 2);
  u8g2.setDrawColor(1);
  
  // Title
  u8g2.setFont(u8g2_font_6x10_tr);
  const char* title = "BRIGHTNESS";
  int titleW = u8g2.getStrWidth(title);
  u8g2.drawStr(boxX + (boxW - titleW) / 2, boxY + 14, title);
  
  // Level name
  u8g2.setFont(u8g2_font_6x10_tr);
  const char* levelName = brightnessNames[brightnessIndex];
  int nameW = u8g2.getStrWidth(levelName);
  u8g2.drawStr(boxX + (boxW - nameW) / 2, boxY + 26, levelName);
  
  // Progress bar
  const int barW = boxW - 20;
  const int barH = 6;
  const int barX = boxX + 10;
  const int barY = boxY + 30;
  
  // Bar background
  u8g2.drawFrame(barX, barY, barW, barH);
  
  // Filled portion
  int fillW = (barW - 2) * (brightnessIndex + 1) / numBrightnessLevels;
  u8g2.drawBox(barX + 1, barY + 1, fillW, barH - 2);
}

void drawButtonBar(bool eqMode, bool playing) {
  const int btnIconY = 54;
  const int btnCount = 5;
  const int btnBarWidth = 256;
  const int btnWidth = btnBarWidth / btnCount;
  const int iconW = 8;
  const int iconH = 8;

  // Main UI button icons
  const uint8_t* mainIcons[5]  = { playing ? iconPauseBtn : iconPlayBtn, iconPrevBtn, iconNextBtn, iconEQBtn, iconDimBtn };
  // EQ screen button icons
  const uint8_t* eqIcons[5]  = { iconEQBtn, iconMinusBtn, iconPlusBtn, iconExitBtn, nullptr };

  const uint8_t** icons = eqMode ? eqIcons : mainIcons;

  for (int i = 0; i < btnCount; ++i) {
    int xCenter = i * btnWidth + btnWidth / 2;
    if (icons[i]) {
      u8g2.drawXBMP(xCenter - iconW/2, btnIconY, iconW, iconH, icons[i]);
    }
  }
}

// --- UART Metadata Reception ---
void handleSerialInput() {
  static String line;
  while (SerialUART.available()) {
    char c = SerialUART.read();
    Serial.print("[UART CHAR] ");
    Serial.println((int)c); // Print ASCII code of every received char
    if (c >= 32 && c < 127) {
      Serial.print("[UART CHAR ASCII] ");
      Serial.println(c);
    }
    if (c == '\n') {
      Serial.println("Received: " + line); // Debug print to USB serial
      // --- DEBUG: Echo received line back to UART ---
      SerialUART.print("[DISPLAY ECHO] ");
      SerialUART.println(line);

      bool event = false;
      if (line.startsWith("T=")) {
        strncpy(lastTitle, line.c_str() + 2, sizeof(lastTitle) - 1);
        lastTitle[sizeof(lastTitle) - 1] = '\0';
        Serial.print("Updated lastTitle: ");
        Serial.println(lastTitle);
        event = true;
      } else if (line.startsWith("A=")) {
        strncpy(lastArtist, line.c_str() + 2, sizeof(lastArtist) - 1);
        lastArtist[sizeof(lastArtist) - 1] = '\0';
        Serial.print("Updated lastArtist: ");
        Serial.println(lastArtist);
        event = true;
      } else if (line.startsWith("P=")) {
        playing = (line.charAt(2) == '1');
        Serial.print("Updated playing: ");
        Serial.println(playing ? "1" : "0");
        event = true;
      } else if (line.startsWith("BTN=")) {
        String btn = line.substring(4);
        event = true;
        if (eqMode) {
          if (btn == "PLAY") {
            eqBand = (eqBand + 1) % 3;
          } else if (btn == "NEXT") {
            eqGains[eqBand] += 1.0f;
            if (eqGains[eqBand] > eqMaxGain) eqGains[eqBand] = eqMaxGain;
            // Save updated gain to Preferences
            prefs.begin("eq", false);
            char key[8];
            snprintf(key, sizeof(key), "band%d", eqBand);
            prefs.putFloat(key, eqGains[eqBand]);
            prefs.end();
          } else if (btn == "PREV") {
            eqGains[eqBand] -= 1.0f;
            if (eqGains[eqBand] < eqMinGain) eqGains[eqBand] = eqMinGain;
            // Save updated gain to Preferences
            prefs.begin("eq", false);
            char key[8];
            snprintf(key, sizeof(key), "band%d", eqBand);
            prefs.putFloat(key, eqGains[eqBand]);
            prefs.end();
          } else if (btn == "EQ") {
            eqMode = false;
            SerialUART.println("EQMODE=0");
          }
        } else {
          if (btn == "DIM") {
            brightnessIndex = (brightnessIndex + 1) % numBrightnessLevels;
            u8g2.setContrast(brightnessLevels[brightnessIndex]);
            // Save brightness setting
            prefs.begin("display", false);
            prefs.putInt("brightness", brightnessIndex);
            prefs.end();
            // Show brightness indicator
            showBrightnessIndicator = true;
            brightnessIndicatorStart = millis();
            Serial.printf("[BRIGHTNESS] Set to level %d: %s (%d)\n", 
                         brightnessIndex, brightnessNames[brightnessIndex], brightnessLevels[brightnessIndex]);
          } else if (btn == "EQ") {
            eqMode = true;
            eqBand = 0;
            SerialUART.println("EQMODE=1");
          }
        }
      } else if (line.startsWith("EQ=")) {
        // Receive EQ value: "EQ=BAND,GAIN" (e.g., "EQ=0,3.5")
        int commaPos = line.indexOf(',');
        if (commaPos > 0) {
          int band = line.substring(3, commaPos).toInt();
          float gain = line.substring(commaPos + 1).toFloat();
          if (band >= 0 && band < 3) { // Only 3 bands now
            eqGains[band] = gain;
            Serial.printf("[EQ] Received band %d: %.1fdB\n", band, gain);
          }
        }
        event = true;
      } else if (line.startsWith("EQBAND=")) {
        // Set current EQ band selection from audio ESP32
        int band = line.substring(7).toInt();
        if (band >= 0 && band < 3) {
          eqBand = band;
          Serial.printf("[EQ] Selected band: %d\n", band);
        }
        event = true;
      } else if (line.startsWith("EQMODE=")) {
        int mode = line.substring(7).toInt();
        eqMode = (mode == 1);
        if (eqMode) eqBand = 0;
        event = true;
      } else if (line.startsWith("B=")) {
        int b = line.substring(2).toInt();
        for (int i = 0; i < numBrightnessLevels; ++i) {
          if (brightnessLevels[i] == b) {
            brightnessIndex = i;
            u8g2.setContrast(brightnessLevels[brightnessIndex]);
            break;
          }
        }
        event = true;
      }
      if (event) {
        lastActivityMillis = millis();
        showTuner = false;
      }
      line = "";
    } else {
      line += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[DISPLAY] setup() started");

  SerialUART.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  Serial.println("[DISPLAY] UART online");

  u8g2.begin();
  u8g2.setContrast(128);
  u8g2.clearBuffer();

  // Load EQ gains from Preferences
  prefs.begin("eq", true); // read-only
  for (int i = 0; i < 3; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "band%d", i);
    eqGains[i] = prefs.getFloat(key, 0.0f);
  }
  prefs.end();

  // Load brightness setting from Preferences
  prefs.begin("display", true); // read-only
  brightnessIndex = prefs.getInt("brightness", 2); // Default to Medium brightness
  prefs.end();
  
  // Apply loaded brightness
  u8g2.setContrast(brightnessLevels[brightnessIndex]);
  Serial.printf("[BRIGHTNESS] Loaded level %d: %s (%d)\n", 
               brightnessIndex, brightnessNames[brightnessIndex], brightnessLevels[brightnessIndex]);

  lastActivityMillis = millis();

  Serial.println("[DISPLAY] setup() complete - DSP control removed");
}

void loop() {
  handleSerialInput();

  // Check if brightness indicator should be hidden
  if (showBrightnessIndicator && (millis() - brightnessIndicatorStart > brightnessIndicatorDelay)) {
    showBrightnessIndicator = false;
  }

  // All button events now come from UART, not local buttons
  // Screensaver and EQ mode logic should be triggered by received events
  if (!eqMode) {
    if (!showTuner && (millis() - lastActivityMillis > screensaverDelay)) {
      showTuner = true;
    }
    if (showTuner) {
      u8g2.clearBuffer();
      u8g2.drawXBMP(
        (256 - tuner_width) / 2,
        (64 - tuner_height) / 2,
        tuner_width, tuner_height, (const uint8_t*)tuner
      );
      u8g2.sendBuffer();
      delay(20);
      return;
    }
    // --- MAIN UI ---
    u8g2.clearBuffer();
    u8g2.setContrast(brightnessLevels[brightnessIndex]);

  // --- Song Title with music note ---
  int titleY = 28; // keep top gap the same
  u8g2.setFont(u8g2_font_9x15_tr); // larger font (~15px) to match 16px icons
    int textStartX = 18;
    int titleWidth = u8g2.getStrWidth(lastTitle);
    int maxTextWidth = 230 - textStartX; // 256px display
    int scrollGap = 32;

    int titleScrollX = 0;
    if (titleWidth > maxTextWidth) {
      if (millis() - scrollTimerTitle > scrollInterval) {
        scrollTimerTitle = millis();
        titleScroll += 4;
        if (titleScroll > titleWidth + scrollGap) titleScroll = 0;
      }
      titleScrollX = -titleScroll;
      u8g2.drawStr(textStartX + titleScrollX, titleY, lastTitle);
      u8g2.drawStr(textStartX + titleScrollX + titleWidth + scrollGap, titleY, lastTitle);
    } else {
      titleScroll = 0;
      titleScrollX = 0;
      u8g2.drawStr(textStartX, titleY, lastTitle);
    }
    u8g2.drawXBMP(0, titleY - 14, 16, 16, iconNote);

  // --- Artist with icon ---
  int artistY = 48; // leave room for bottom button bar
  int artistTextStartX = 18;
    int artistWidth = u8g2.getStrWidth(lastArtist);
    int artistMaxTextWidth = 230 - artistTextStartX;

    int artistScrollX = 0;
    if (artistWidth > artistMaxTextWidth) {
      if (millis() - scrollTimerArtist > scrollInterval) {
        scrollTimerArtist = millis();
        artistScroll += 4;
        if (artistScroll > artistWidth + scrollGap) artistScroll = 0;
      }
      artistScrollX = -artistScroll;
      u8g2.drawStr(artistTextStartX + artistScrollX, artistY, lastArtist);
      u8g2.drawStr(artistTextStartX + artistScrollX + artistWidth + scrollGap, artistY, lastArtist);
    } else {
      artistScroll = 0;
      artistScrollX = 0;
      u8g2.drawStr(artistTextStartX, artistY, lastArtist);
    }
    u8g2.drawXBMP(0, artistY - 14, 16, 16, iconArtist);

    drawButtonBar(false, playing);

    // Draw brightness indicator overlay if active
    if (showBrightnessIndicator) {
      drawBrightnessIndicator();
    }

    u8g2.sendBuffer();
    delay(20);
  } else {
    // --- EQ MODE UI ---
    u8g2.clearBuffer();
    u8g2.setContrast(brightnessLevels[brightnessIndex]);
    u8g2.setFont(u8g2_font_6x13_tr);
    // Centered title
    const char* eqTitle = "EQUALIZER";
    int titleWidth = u8g2.getStrWidth(eqTitle);
    u8g2.drawStr((256 - titleWidth) / 2, 15, eqTitle);

    // 3-band layout with bottom safe area (no overlap with button bar)
    const int BUTTON_BAR_TOP = 54; // y used by button icons
    const int bandWidth = 240;
    const int bandX = 8;
    const int bandY[3] = {27, 39, 51}; // baselines; last is just above button bar
    const int boxOffset = 10; // highlight box extends above baseline
    const int boxH = 12;      // highlight height (kept within safe area)

    for (int i = 0; i < 3; ++i) {
      int y = bandY[i];
      // Highlight selected band
      if (eqBand == i) {
        u8g2.drawRBox(bandX, y - boxOffset, bandWidth, boxH, 3);
        u8g2.setDrawColor(0);
      } else {
        u8g2.setDrawColor(1);
      }
      // Band name
      u8g2.drawStr(bandX + 8, y, eqBandNames[i]);
      // Value
      char valStr[8];
      snprintf(valStr, sizeof(valStr), "%+2.1fdB", eqGains[i]);
      int valWidth = u8g2.getStrWidth(valStr);
      u8g2.drawStr(bandX + bandWidth - valWidth - 8, y, valStr);
      u8g2.setDrawColor(1);
    }
    drawButtonBar(true, playing);
    u8g2.sendBuffer();
    delay(20);
  }
}
