#include <Arduino.h>
#include "test_config.h"

#if ENABLE_TEST_NFC
  #include <SPI.h>
  #include <MFRC522.h>
#endif

#if ENABLE_TEST_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SH110X.h>
#endif

// ============================================================================
// NFC / RC522 Diagnostic Module
// ============================================================================
#if ENABLE_TEST_NFC
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);

constexpr uint8_t MAX_SEEN_CARDS = 64;
struct SeenCard {
  byte size;
  byte uid[10];
};
SeenCard seen_cards[MAX_SEEN_CARDS];
uint8_t seen_count = 0;
uint32_t total_scans = 0;
uint32_t last_heartbeat_ms = 0;

int findSeenCard(const MFRC522::Uid& uid) {
  for (uint8_t i = 0; i < seen_count; ++i) {
    if (seen_cards[i].size != uid.size) continue;
    bool match = true;
    for (byte b = 0; b < uid.size; ++b) {
      if (seen_cards[i].uid[b] != uid.uidByte[b]) {
        match = false;
        break;
      }
    }
    if (match) return static_cast<int>(i);
  }
  return -1;
}

uint32_t uidToGameHash(const MFRC522::Uid& uid) {
  uint32_t hash = 0;
  for (byte i = 0; i < uid.size && i < 4; ++i) {
    hash = (hash << 8) | uid.uidByte[i];
  }
  return hash;
}

void formatUidHex(const MFRC522::Uid& uid, char* out, size_t out_len) {
  size_t pos = 0;
  for (byte i = 0; i < uid.size && pos + 3 < out_len; ++i) {
    if (i > 0) out[pos++] = ':';
    snprintf(out + pos, out_len - pos, "%02X", uid.uidByte[i]);
    pos += 2;
  }
  out[pos] = '\0';
}

void setupNfcTest() {
  Serial.println(F("\n[NFC] Initializing SPI & MFRC522..."));
  Serial.printf("[NFC] Pins -> SCK:%d MISO:%d MOSI:%d SS(SDA):%d RST:%d\n",
                PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_RFID_SS, PIN_RFID_RST);

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();
  delay(50);

  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[NFC] VersionReg = 0x%02X -> ", ver);
  if (ver == 0x00 || ver == 0xFF) {
    Serial.println(F("FAIL! Communication error (chip not detected)."));
    Serial.println(F("      Checklist:"));
    Serial.println(F("      1) Confirm 3.3V -> 3V3 (NEVER 5V) and GND -> GND."));
    Serial.println(F("      2) Confirm RC522 pins are soldered (loose un-soldered headers fail SPI)."));
    Serial.println(F("      3) Recheck SDA/SS=5, SCK=18, MOSI=23, MISO=19, RST=4."));
    return;
  } else if (ver == 0x91) {
    Serial.println(F("OK! MFRC522 v1.0 detected."));
  } else if (ver == 0x92) {
    Serial.println(F("OK! MFRC522 v2.0 detected."));
  } else if (ver == 0x88) {
    Serial.println(F("OK! Compatible clone (FM17522) detected."));
  } else {
    Serial.println(F("OK! Unknown compatible revision detected (SPI responding)."));
  }

  // Stress-test SPI stability (100 rapid reads catch loose wires/headers immediately)
  uint8_t spi_ok_reads = 0;
  for (uint8_t i = 0; i < 100; ++i) {
    if (rfid.PCD_ReadRegister(MFRC522::VersionReg) == ver) spi_ok_reads++;
  }
  Serial.printf("[NFC] SPI stability test: %u/100 identical reads %s\n",
                spi_ok_reads,
                spi_ok_reads == 100 ? "(ROCK SOLID)" : "(FLAKY WIRING / LOOSE PINS!)");

  // Ensure RF antenna drivers (TX1 & TX2) are enabled, use balanced default gain (33 dB)
  rfid.PCD_AntennaOn();
  rfid.PCD_SetAntennaGain(rfid.RxGain_avg);

  byte tx_ctrl = rfid.PCD_ReadRegister(MFRC522::TxControlReg);
  byte gain = rfid.PCD_GetAntennaGain();
  Serial.printf("[NFC] TxControlReg=0x%02X (antenna %s) | Gain=0x%02X (33dB default)\n",
                tx_ctrl,
                (tx_ctrl & 0x03) == 0x03 ? "ON" : "OFF!",
                gain);
  Serial.println(F("[NFC] READY — Tap any 13.56 MHz card/sticker on the RC522...\n"));
}
#endif

// ============================================================================
// Buttons Diagnostic Module
// ============================================================================
#if ENABLE_TEST_BUTTONS
struct TestButton {
  const char* name;
  int pin;
  bool last_reading;
  bool stable_state;
  uint32_t last_change_ms;
  uint32_t press_count;
};

TestButton test_buttons[5] = {
  {"P1 (Ruedas Verdes)", PIN_BTN_P1,     HIGH, HIGH, 0, 0},
  {"P2 (Estudio Prisma)", PIN_BTN_P2,    HIGH, HIGH, 0, 0},
  {"P3 (Billetera Clara)", PIN_BTN_P3,   HIGH, HIGH, 0, 0},
  {"BACK / Cancelar",     PIN_BTN_BACK,  HIGH, HIGH, 0, 0},
  {"COBRAR / Confirmar",  PIN_BTN_COBRAR,HIGH, HIGH, 0, 0},
};

void setupButtonsTest() {
  Serial.println(F("\n[BTN] Initializing 5 buttons (INPUT_PULLUP -> connect to GND)..."));
  for (uint8_t i = 0; i < 5; ++i) {
    pinMode(test_buttons[i].pin, INPUT_PULLUP);
    delay(5);
    bool state = digitalRead(test_buttons[i].pin);
    test_buttons[i].last_reading = state;
    test_buttons[i].stable_state = state;
    Serial.printf("[BTN] %-21s GPIO %-2d | initial=%s %s\n",
                  test_buttons[i].name,
                  test_buttons[i].pin,
                  state == LOW ? "LOW" : "HIGH",
                  state == LOW ? "(WARN: stuck/shorted to GND?)" : "(OK)");
  }
}

void loopButtonsTest() {
  for (uint8_t i = 0; i < 5; ++i) {
    TestButton& b = test_buttons[i];
    bool reading = digitalRead(b.pin);
    if (reading != b.last_reading) {
      b.last_change_ms = millis();
      b.last_reading = reading;
    }
    if ((millis() - b.last_change_ms) > 30 && reading != b.stable_state) {
      b.stable_state = reading;
      if (b.stable_state == LOW) {
        b.press_count++;
        Serial.printf("[BTN] PRESSED  -> %s (GPIO %d) | count=%lu\n",
                      b.name, b.pin, static_cast<unsigned long>(b.press_count));
      } else {
        Serial.printf("[BTN] RELEASED -> %s (GPIO %d)\n", b.name, b.pin);
      }
    }
  }
}
#endif

// ============================================================================
// Buzzer Diagnostic Module
// ============================================================================
#if ENABLE_TEST_BUZZER
uint32_t last_buzzer_demo_ms = 0;
uint8_t buzzer_step = 0;

void playToneBlocking(uint16_t freq, uint16_t ms) {
  tone(PIN_BUZZER, freq, ms);
  delay(ms + 40);
}

void setupBuzzerTest() {
  Serial.printf("\n[BUZ] Initializing passive buzzer on GPIO %d...\n", PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  Serial.println(F("[BUZ] Playing startup scale (523, 659, 784, 1046 Hz)..."));
  playToneBlocking(523, 120);
  playToneBlocking(659, 120);
  playToneBlocking(784, 120);
  playToneBlocking(1046, 200);
  last_buzzer_demo_ms = millis();
}

void loopBuzzerTest() {
  if (millis() - last_buzzer_demo_ms >= 3500) {
    last_buzzer_demo_ms = millis();
    if (buzzer_step == 0) {
      Serial.println(F("[BUZ] Demo: OK beep (980 Hz)"));
      tone(PIN_BUZZER, 980, 120);
    } else if (buzzer_step == 1) {
      Serial.println(F("[BUZ] Demo: ERROR buzz (220 Hz)"));
      tone(PIN_BUZZER, 220, 260);
    } else {
      Serial.println(F("[BUZ] Demo: WIN chime (1320 Hz)"));
      tone(PIN_BUZZER, 1320, 350);
    }
    buzzer_step = (buzzer_step + 1) % 3;
  }
}
#endif

// ============================================================================
// OLED 1.3" 128x64 (I2C SH1106) Diagnostic Module
// ============================================================================
#if ENABLE_TEST_OLED
Adafruit_SH1106G display(128, 64, &Wire, -1);
bool oled_ready = false;
uint32_t last_oled_refresh_ms = 0;
uint8_t oled_bar_val = 0;

void drawOledScreen(const char* status_line) {
  if (!oled_ready) return;
  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, SH110X_WHITE);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(6, 5);
  display.println(F("HEADHUNTERS TEST"));
  display.drawLine(0, 15, 127, 15, SH110X_WHITE);

  const char* labels[3] = {"RV", "EP", "BC"};
  for (int i = 0; i < 3; ++i) {
    int y = 20 + i * 11;
    uint8_t lvl = (oled_bar_val + i * 4) % 13; // 0..12
    display.setCursor(6, y);
    display.print(labels[i]);
    display.drawRect(24, y, 76, 8, SH110X_WHITE);
    int fill_w = (lvl * 72) / 12;
    if (fill_w > 0) display.fillRect(26, y + 2, fill_w, 4, SH110X_WHITE);
    display.setCursor(104, y);
    display.printf("%2u", lvl);
  }

  display.setCursor(6, 53);
  display.print(status_line);
  display.display();
}

void setupOledTest() {
  Serial.printf("\n[OLED] Scanning I2C bus (SDA=%d, SCL=%d)...\n", PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[OLED] I2C device found at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println(F("[OLED] FAIL! No I2C devices found. Check VCC/GND order and SDA=21/SCL=22."));
    return;
  }

  if (!display.begin(OLED_I2C_ADDR, true)) {
    Serial.printf("[OLED] FAIL! SH1106 init failed at 0x%02X.\n", OLED_I2C_ADDR);
    return;
  }
  oled_ready = true;
  Serial.printf("[OLED] OK! SH1106 128x64 initialized at 0x%02X.\n", OLED_I2C_ADDR);
  drawOledScreen("OLED OK (0x3C)");
  last_oled_refresh_ms = millis();
}

void loopOledTest() {
  if (!oled_ready) return;
  if (millis() - last_oled_refresh_ms >= 800) {
    last_oled_refresh_ms = millis();
    oled_bar_val = (oled_bar_val + 1) % 13;
    drawOledScreen("Bars animating...");
  }
}
#endif

// ============================================================================
// NFC Loop (placed after OLED so it can mirror UIDs to OLED if both are ON)
// ============================================================================
#if ENABLE_TEST_NFC
void loopNfcTest() {
  if (millis() - last_heartbeat_ms >= 5000) {
    last_heartbeat_ms = millis();
    byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    byte tx = rfid.PCD_ReadRegister(MFRC522::TxControlReg);
    Serial.printf("[NFC] Waiting... (ver=0x%02X, ant=%s, scans=%lu, unique=%u)\n",
                  ver,
                  (tx & 0x03) == 0x03 ? "ON" : "OFF!",
                  static_cast<unsigned long>(total_scans),
                  seen_count);
  }

  byte atqa[2] = {0, 0};
  byte atqa_size = sizeof(atqa);
  MFRC522::StatusCode req_status = rfid.PICC_RequestA(atqa, &atqa_size);
  if (req_status == MFRC522::STATUS_TIMEOUT) {
    return; // Normal when no card is near the antenna
  }
  if (req_status != MFRC522::STATUS_OK) {
    byte err_reg = rfid.PCD_ReadRegister(MFRC522::ErrorReg);
    Serial.print(F("[NFC] REQA anomaly -> status="));
    Serial.print(rfid.GetStatusCodeName(req_status));
    Serial.printf(" | ATQA=%02X %02X | ErrorReg=0x%02X\n", atqa[0], atqa[1], err_reg);
    delay(250);
    return;
  }

  MFRC522::StatusCode sel_status = rfid.PICC_Select(&rfid.uid);
  if (sel_status != MFRC522::STATUS_OK) {
    byte err_reg = rfid.PCD_ReadRegister(MFRC522::ErrorReg);
    Serial.print(F("[NFC] Card answered REQA (ATQA="));
    Serial.printf("%02X %02X), but SELECT failed -> status=", atqa[0], atqa[1]);
    Serial.print(rfid.GetStatusCodeName(sel_status));
    Serial.printf(" | ErrorReg=0x%02X\n", err_reg);
    delay(250);
    return;
  }

  total_scans++;
  char uid_str[32];
  formatUidHex(rfid.uid, uid_str, sizeof(uid_str));
  uint32_t game_hash = uidToGameHash(rfid.uid);
  MFRC522::PICC_Type picc_type = rfid.PICC_GetType(rfid.uid.sak);

  int idx = findSeenCard(rfid.uid);
  bool is_new = (idx < 0);
  if (is_new && seen_count < MAX_SEEN_CARDS) {
    idx = seen_count;
    seen_cards[seen_count].size = rfid.uid.size;
    memcpy(seen_cards[seen_count].uid, rfid.uid.uidByte, rfid.uid.size);
    seen_count++;
  }

  Serial.println(F("--------------------------------------------------"));
  Serial.printf("[NFC] Scan #%lu | %s (card #%d)\n",
                static_cast<unsigned long>(total_scans),
                is_new ? "NEW CARD" : "SEEN BEFORE",
                idx + 1);
  Serial.printf("      UID (%u bytes): %s\n", rfid.uid.size, uid_str);
  Serial.printf("      game_data.json nfc_uid: \"%08lX\"\n",
                static_cast<unsigned long>(game_hash));
  Serial.print(F("      Type: "));
  Serial.print(rfid.PICC_GetTypeName(picc_type));
  Serial.printf(" (SAK=0x%02X)\n", rfid.uid.sak);
  if (rfid.uid.size > 4) {
    Serial.println(F("      NOTE: 7-byte UID (NTAG/Ultralight). Consider hashing all 7 bytes in firmware!"));
  }
  Serial.println(F("--------------------------------------------------"));

#if ENABLE_TEST_OLED
  char oled_msg[24];
  snprintf(oled_msg, sizeof(oled_msg), "UID:%08lX", static_cast<unsigned long>(game_hash));
  drawOledScreen(oled_msg);
#endif

#if ENABLE_TEST_BUZZER
  tone(PIN_BUZZER, 1046, 90);
#endif

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(250);
}
#endif

// ============================================================================
// Arduino Entrypoints
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(800); // Give USB-Serial monitor a moment to attach after reset

  Serial.println(F("\n=================================================="));
  Serial.println(F("  Headhunters ESP-32D — Hardware Diagnostic Suite"));
  Serial.println(F("=================================================="));
  Serial.printf("Enabled tests -> NFC:%d  BUTTONS:%d  BUZZER:%d  OLED:%d\n",
                ENABLE_TEST_NFC, ENABLE_TEST_BUTTONS, ENABLE_TEST_BUZZER, ENABLE_TEST_OLED);

#if ENABLE_TEST_NFC
  setupNfcTest();
#endif
#if ENABLE_TEST_BUTTONS
  setupButtonsTest();
#endif
#if ENABLE_TEST_BUZZER
  setupBuzzerTest();
#endif
#if ENABLE_TEST_OLED
  setupOledTest();
#endif
}

void loop() {
#if ENABLE_TEST_NFC
  loopNfcTest();
#endif
#if ENABLE_TEST_BUTTONS
  loopButtonsTest();
#endif
#if ENABLE_TEST_BUZZER
  loopBuzzerTest();
#endif
#if ENABLE_TEST_OLED
  loopOledTest();
#endif
}
