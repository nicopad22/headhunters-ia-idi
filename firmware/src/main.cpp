#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "card_registry.h"
#include "game_data.h"
#include "pin_config.h"
#include "text_util.h"

// ============================================================================
// Headhunters en la era de la IA — ESP-32D Production Firmware
// Hardware: ESP-32D + RC522 (SPI) + OLED 1.3" 128x64 (I2C SH1106) + 5 Buttons + Buzzer
//
// ADMIN MODE (assign NFC cards from the board itself):
//   Hold ATRAS/CANCELAR for 10 s (double beep), release, then press P1, P2, P3.
//   The 50-card mapping is stored in ESP32 flash and survives reboots/reflashes.
//   (Also reachable by typing "admin" in the Serial Monitor.)
//
// Compile with -DDUMMY_FLOW_TEST=1 (via ./run_firmware.sh --dummy-test) to run
// the interactive 6-card, 11-step guided walkthrough of the entire game flow.
// ============================================================================

constexpr uint8_t OLED_I2C_ADDR = 0x3C;
constexpr uint32_t FEEDBACK_DURATION_MS = 3800;

// Button indices
constexpr uint8_t BTN_P1 = 0;
constexpr uint8_t BTN_P2 = 1;
constexpr uint8_t BTN_P3 = 2;
constexpr uint8_t BTN_BACK = 3;
constexpr uint8_t BTN_COBRAR = 4;
constexpr uint8_t NUM_BUTTONS = 5;
constexpr uint32_t DEBOUNCE_MS = 35;

// Admin entry: hold BACK for ADMIN_HOLD_MS, then type ADMIN_CODE.
constexpr uint32_t ADMIN_HOLD_MS = 10000;
constexpr uint32_t ADMIN_CODE_STEP_TIMEOUT_MS = 6000;
constexpr uint8_t ADMIN_CODE[] = {BTN_P1, BTN_P2, BTN_P3};
constexpr uint8_t ADMIN_CODE_LEN = sizeof(ADMIN_CODE);

enum ConsoleState : uint8_t {
  STATE_IDLE_LEADERBOARD = 0,
  STATE_PLAYER_SELECTED,
  STATE_SHOW_FEEDBACK,
  STATE_WAIT_TRIVIA_CONFIRM,
  STATE_WAIT_RETURN_SCAN,
  STATE_GAME_OVER,
  STATE_ADMIN_CODE,  // BACK was held 10 s, waiting for the P1-P2-P3 code
  STATE_ADMIN,       // card-assignment admin mode
#ifdef DUMMY_FLOW_TEST
  STATE_DUMMY_ENROLL,
#endif
};

struct PlayerState {
  int16_t money;
  uint8_t profitability;
  int8_t hired_candidate_by_slot[ROLES_PER_COMPANY]; // -1 if empty
  int8_t last_hired_occupation_id;                   // -1 if none yet (cooldown)
  int16_t next_hire_discount;
  bool upgrade_human_ready;
  bool eligible_for_payout;
};

struct Button {
  int pin;
  bool last_reading;
  uint32_t last_change_ms;
  bool down;             // debounced state
  uint32_t down_since_ms;
  bool pressed;          // edge: went down this loop
  bool released;         // edge: went up this loop
  bool suppress_release; // set when a long hold was consumed (no "tap" on release)
};

MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
Adafruit_SH1106G display(128, 64, &Wire, -1);
bool oled_ok = false;

PlayerState players[NUM_PLAYERS];
bool candidate_taken[NUM_CANDIDATES];
ConsoleState console_state = STATE_IDLE_LEADERBOARD;
ConsoleState state_after_feedback = STATE_IDLE_LEADERBOARD;
int8_t active_player = -1;
int8_t winner_player = -1;
int16_t pending_trivia_reward = 0;

// Feedback screen buffer (ASCII-folded for the 5x7 OLED font)
char fb_title[22] = "";
char fb_line1[22] = "";
char fb_line2[22] = "";
char fb_line3[22] = "";
uint32_t fb_started_ms = 0;

const char* COMPANY_SHORT[NUM_PLAYERS] = {"RV", "EP", "BC"};
const char* COMPANY_ASCII[NUM_PLAYERS] = {"Ruedas Verdes", "Estudio Prisma", "Billetera Clara"};

Button buttons[NUM_BUTTONS] = {
  {PIN_BTN_P1, HIGH, 0, false, 0, false, false, false},
  {PIN_BTN_P2, HIGH, 0, false, 0, false, false, false},
  {PIN_BTN_P3, HIGH, 0, false, 0, false, false, false},
  {PIN_BTN_BACK, HIGH, 0, false, 0, false, false, false},
  {PIN_BTN_COBRAR, HIGH, 0, false, 0, false, false, false},
};

// ============================================================================
// Admin mode state
// ============================================================================
enum AdminScreen : uint8_t {
  ADM_MENU = 0,
  ADM_WALK,      // prompting for one card at a time
  ADM_CONFLICT,  // scanned tag already belongs to another card
  ADM_CONFIRM,   // yes/no confirmation
  ADM_TEST,      // scan any card to see what it is
  ADM_MESSAGE,   // transient message, then adm_after_msg
};

enum AdminConfirm : uint8_t {
  CONFIRM_CLEAR_ONE = 0,
  CONFIRM_WIPE_ALL,
};

ConsoleState resume_state = STATE_IDLE_LEADERBOARD; // where to go after admin / failed code
uint8_t code_progress = 0;
uint32_t code_last_ms = 0;

AdminScreen adm_screen = ADM_MENU;
AdminScreen adm_after_msg = ADM_MENU;
AdminConfirm adm_confirm = CONFIRM_CLEAR_ONE;
bool adm_missing_only = true;
uint8_t adm_slot = 0;
int16_t adm_conflict_slot = -1;
uint32_t adm_pending_uid = 0;
int16_t adm_test_slot = -1;
uint32_t adm_test_uid = 0;
char adm_msg_title[22] = "";
char adm_msg_l1[22] = "";
char adm_msg_l2[22] = "";
uint32_t adm_msg_until = 0;

// ============================================================================
// Guided Dummy Flow Test State (active only when -DDUMMY_FLOW_TEST=1)
// ============================================================================
#ifdef DUMMY_FLOW_TEST
constexpr uint8_t DUMMY_CARD_COUNT = 6;
uint32_t dummy_uids[DUMMY_CARD_COUNT] = {0, 0, 0, 0, 0, 0};
uint8_t dummy_enrolled = 0;
uint8_t dummy_step = 0; // 0..10 guided steps, 11 = completed (free play!)

// Maps the 6 temporary cards to real game items:
// [0] Candidate #0  (Rosa Quispe     — Ing. Mecanico, Humano, $200, +3)  [Valid P1]
// [1] Candidate #1  (Tomas Fuentes   — Ing. Mecanico, H+IA,   $300, +2)  [Replacement P1]
// [2] Candidate #7  (Joaquin Herrera — Fotografo,     H+IA,   $300, +3)  [2nd role P1]
// [3] Candidate #12 (Lucia Navarro   — Dis. Grafico,  Humano, $200, +2)  [NOT in P1 guide]
// [4] Occupation #0 (Taller Mecanico — Insight $50, OIT 0.32 -> Humano)
// [5] Mystery #15   (Dato OIT #1     — Trivia +$100 via COBRAR/BACK)
const CardRef DUMMY_CARD_MAP[DUMMY_CARD_COUNT] = {
  {CARD_CANDIDATE, 0}, {CARD_CANDIDATE, 1}, {CARD_CANDIDATE, 7},
  {CARD_CANDIDATE, 12}, {CARD_OCCUPATION, 0}, {CARD_MYSTERY, 14},
};

const char* DUMMY_ENROLL_LABELS[DUMMY_CARD_COUNT] = {
  "1/6: Rosa (H $200)",
  "2/6: Tomas (Reempl)",
  "3/6: Joaquin (H+IA)",
  "4/6: Lucia (No P1)",
  "5/6: Insight Mec.",
  "6/6: Trivia OIT",
};

const char* DUMMY_ENROLL_SERIAL[DUMMY_CARD_COUNT] = {
  "Card #1 -> Candidate: Rosa Quispe (Ing. Mecanico, Humano $200, +3) [valid for P1]",
  "Card #2 -> Candidate: Tomas Fuentes (Ing. Mecanico, H+IA $300, +2) [replacement test]",
  "Card #3 -> Candidate: Joaquin Herrera (Fotografo, H+IA $300, +3) [2nd role for P1]",
  "Card #4 -> Candidate: Lucia Navarro (Dis. Grafico) [NOT in P1 guide -> block test]",
  "Card #5 -> Occupation Insight: Taller Mecanico ($50, OIT 0.32 -> Humano)",
  "Card #6 -> Mystery Card: Dato OIT #1 (Trivia +$100 via COBRAR=Si / BACK=No)",
};

const char* DUMMY_STEP_PROMPT_OLED[12] = {
  "1: P1 + Tarjeta #4",
  "2: Presiona COBRAR",
  "3: Escanea #5 Insgt",
  "4: Presiona COBRAR",
  "5: Escanea #1 Rosa",
  "6: Escanea #2 Tomas",
  "7: Escanea #3 Joaq",
  "8: Presiona COBRAR",
  "9: Escanea #2 Tomas",
  "10: #6 + COBRAR(Si)",
  "11: Presiona ATRAS",
  "TEST OK! Modo libre",
};

void printDummyStepPrompt() {
  if (dummy_step >= 11) {
    Serial.println(F("\n=================================================="));
    Serial.println(F("  ALL 11 GUIDED FLOW STEPS PASSED!"));
    Serial.println(F("  Console is now in FREE-PLAY mode with your 6 cards."));
    Serial.println(F("  Try P2/P3 to test exclusivity or keep playing to level 12!"));
    Serial.println(F("==================================================\n"));
    return;
  }
  Serial.println(F("\n>>> NEXT GUIDED TEST STEP:"));
  switch (dummy_step) {
    case 0: Serial.println(F("  [STEP 1/11] Press button P1 (Ruedas Verdes), then tap Card #4 (Lucia - NOT in P1 guide). Expect: BLOCKED.")); break;
    case 1: Serial.println(F("  [STEP 2/11] Press button COBRAR before buying anything. Expect: BLOCKED (not eligible yet).")); break;
    case 2: Serial.println(F("  [STEP 3/11] Tap Card #5 (Insight: Taller Mecanico). Expect: -$50 ($450 left), shows OIT 0.32 -> Humano, enables Cobrar.")); break;
    case 3: Serial.println(F("  [STEP 4/11] Press button COBRAR at level 0. Expect: +$100 ($100 base + $100*0 -> $550), disables Cobrar.")); break;
    case 4: Serial.println(F("  [STEP 5/11] Tap Card #1 (Rosa Quispe, Humano $200, +3). Expect: -$200 ($350), Level 0 -> 3.")); break;
    case 5: Serial.println(F("  [STEP 6/11] Immediately tap Card #2 (Tomas Fuentes, SAME role). Expect: BLOCKED by replacement cooldown!")); break;
    case 6: Serial.println(F("  [STEP 7/11] Tap Card #3 (Joaquin Herrera, Fotografo H+IA $300, +3). Expect: -$300 ($50), Level 3 -> 6.")); break;
    case 7: Serial.println(F("  [STEP 8/11] Press button COBRAR at level 6. Expect: +$700 ($100 + $100*6 -> $750).")); break;
    case 8: Serial.println(F("  [STEP 9/11] Tap Card #2 (Tomas Fuentes) again now that another role was hired. Expect: REPLACED! -$300 ($450), Level 6-3+2 = 5.")); break;
    case 9: Serial.println(F("  [STEP 10/11] Tap Card #6 (Dato OIT Trivia), then press COBRAR (=Si). Expect: +$100 ($550).")); break;
    case 10: Serial.println(F("  [STEP 11/11] Press button ATRAS/CANCELAR to return to the 3-startup Leaderboard on OLED.")); break;
  }
}

void advanceDummyStep(uint8_t completed_step) {
  if (dummy_step == completed_step) {
    Serial.printf("  [PASS] Step %u/11 verified!\n", completed_step + 1);
    dummy_step++;
    printDummyStepPrompt();
  }
}
#endif

// ============================================================================
// Audio & Input Helpers
// ============================================================================
void beepOk()   { tone(PIN_BUZZER, 980, 110); }
void beepErr()  { tone(PIN_BUZZER, 220, 260); }
void beepTick() { tone(PIN_BUZZER, 1400, 40); }
void beepWin() {
  tone(PIN_BUZZER, 784, 140); delay(160);
  tone(PIN_BUZZER, 988, 140); delay(160);
  tone(PIN_BUZZER, 1318, 350);
}
void beepAdminArmed() {
  tone(PIN_BUZZER, 1200, 70); delay(110);
  tone(PIN_BUZZER, 1200, 70);
}
void beepAdminEnter() {
  tone(PIN_BUZZER, 660, 90);  delay(110);
  tone(PIN_BUZZER, 880, 90);  delay(110);
  tone(PIN_BUZZER, 1320, 160);
}

// Call exactly once per loop(); computes debounced state and edges.
void updateButtons() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_BUTTONS; ++i) {
    Button& b = buttons[i];
    b.pressed = false;
    b.released = false;
    bool reading = digitalRead(b.pin);
    if (reading != b.last_reading) {
      b.last_reading = reading;
      b.last_change_ms = now;
    }
    if (now - b.last_change_ms > DEBOUNCE_MS) {
      bool down = (reading == LOW);
      if (down != b.down) {
        b.down = down;
        if (down) {
          b.pressed = true;
          b.down_since_ms = now;
          b.suppress_release = false;
        } else {
          b.released = true;
        }
      }
    }
  }
}

bool btnPressed(uint8_t idx) { return buttons[idx].pressed; }

// BACK acts on release so that a 10 s hold (admin entry) never triggers "back".
bool btnTapped(uint8_t idx) { return buttons[idx].released && !buttons[idx].suppress_release; }

uint8_t countHiredRoles(uint8_t player_idx) {
  uint8_t cnt = 0;
  for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
    if (players[player_idx].hired_candidate_by_slot[s] >= 0) cnt++;
  }
  return cnt;
}

uint32_t uidToHash(const MFRC522::Uid& uid) {
  uint32_t hash = 0;
  for (byte i = 0; i < uid.size && i < 4; ++i) {
    hash = (hash << 8) | uid.uidByte[i];
  }
  return hash;
}

// Returns true (and the UID) when a new card was tapped. Halts the card so it is
// not re-read until it is removed and presented again.
bool readCard(uint32_t& uid_out) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return false;
  uid_out = uidToHash(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  Serial.printf("UID escaneado: 0x%08lX\n", static_cast<unsigned long>(uid_out));
  return uid_out != 0;
}

// ============================================================================
// OLED rendering helpers
// ============================================================================
void drawHeader(const char* left, const char* right = nullptr) {
  display.fillRect(0, 0, 128, 12, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(2, 2);
  display.print(left);
  if (right) {
    int16_t x = 128 - 2 - static_cast<int16_t>(strlen(right)) * 6;
    display.setCursor(x, 2);
    display.print(right);
  }
  display.setTextColor(SH110X_WHITE);
}

void drawLine(int16_t y, const char* text) {
  display.setCursor(0, y);
  display.print(text);
}

void renderAdminCode() {
  drawHeader("MODO TECNICO");
  drawLine(20, "Ingresa el codigo");
  char prog[22] = "";
  size_t o = 0;
  for (uint8_t i = 0; i < ADMIN_CODE_LEN && o + 5 < sizeof(prog); ++i) {
    prog[o++] = '[';
    prog[o++] = i < code_progress ? '*' : ' ';
    prog[o++] = ']';
    prog[o++] = ' ';
  }
  prog[o] = '\0';
  display.setCursor(30, 34);
  display.print(prog);
  drawLine(54, "[<-] Cancelar");
}

void renderAdmin() {
  char count[8];
  snprintf(count, sizeof(count), "%u/%u", registryCount(), NUM_CARDS);

  switch (adm_screen) {
    case ADM_MENU:
      drawHeader("MODO ADMIN", count);
      drawLine(15, "P1 Asignar faltantes");
      drawLine(25, "P2 Revisar todas");
      drawLine(35, "P3 Probar tarjeta");
      drawLine(45, "$  Borrar todas");
      drawLine(55, "<- Salir y jugar");
      break;

    case ADM_WALK: {
      char code[6], head[22], l1[22], l2[22], l3[22], uid_line[22];
      cardCode(adm_slot, code, sizeof(code));
      snprintf(head, sizeof(head), "%s %s", code, cardKindLabel(cardRef(adm_slot).kind));
      drawHeader(head, count);
      cardDescribe(adm_slot, l1, l2, l3, sizeof(l1));
      drawLine(15, l1);
      drawLine(25, l2);
      drawLine(35, l3);
      uint32_t uid = registryUid(adm_slot);
      if (uid) snprintf(uid_line, sizeof(uid_line), "UID %08lX", static_cast<unsigned long>(uid));
      else     snprintf(uid_line, sizeof(uid_line), ">> Acerca tarjeta <<");
      drawLine(45, uid_line);
      display.drawLine(0, 53, 127, 53, SH110X_WHITE);
      drawLine(56, "1< 2> 3:Borra <-Menu");
      break;
    }

    case ADM_CONFLICT: {
      char code_own[6], code_this[6], l1[22], l2[22], l3[22], buf[22];
      cardCode(adm_conflict_slot, code_own, sizeof(code_own));
      cardCode(adm_slot, code_this, sizeof(code_this));
      cardDescribe(adm_conflict_slot, l1, l2, l3, sizeof(l1));
      drawHeader("YA ASIGNADA");
      drawLine(15, "Esa tarjeta ya es:");
      snprintf(buf, sizeof(buf), "%s %s", code_own, l1);
      drawLine(25, buf);
      snprintf(buf, sizeof(buf), "Moverla a %s?", code_this);
      drawLine(37, buf);
      drawLine(47, "[$] Si, mover");
      drawLine(56, "[<-] No");
      break;
    }

    case ADM_CONFIRM: {
      char buf[22];
      if (adm_confirm == CONFIRM_CLEAR_ONE) {
        char code[6], l1[22], l2[22], l3[22];
        cardCode(adm_slot, code, sizeof(code));
        cardDescribe(adm_slot, l1, l2, l3, sizeof(l1));
        drawHeader("BORRAR UID");
        snprintf(buf, sizeof(buf), "%s %s", code, l1);
        drawLine(16, buf);
        drawLine(28, "Quitar su tarjeta?");
        drawLine(47, "[$] Si, borrar");
      } else {
        drawHeader("BORRAR TODAS");
        snprintf(buf, sizeof(buf), "Se borran %u UIDs", registryCount());
        drawLine(16, buf);
        drawLine(28, "de la memoria flash.");
        drawLine(47, "[$] Si, borrar todo");
      }
      drawLine(56, "[<-] No");
      break;
    }

    case ADM_TEST: {
      drawHeader("PROBAR TARJETA", count);
      if (adm_test_uid == 0) {
        drawLine(26, "Acerca una tarjeta");
        drawLine(36, "para ver que es.");
      } else if (adm_test_slot < 0) {
        char buf[22];
        drawLine(18, "NO REGISTRADA");
        snprintf(buf, sizeof(buf), "UID %08lX", static_cast<unsigned long>(adm_test_uid));
        drawLine(30, buf);
      } else {
        char code[6], head[22], l1[22], l2[22], l3[22];
        cardCode(adm_test_slot, code, sizeof(code));
        snprintf(head, sizeof(head), "%s %s", code, cardKindLabel(cardRef(adm_test_slot).kind));
        cardDescribe(adm_test_slot, l1, l2, l3, sizeof(l1));
        drawLine(15, head);
        drawLine(25, l1);
        drawLine(35, l2);
        drawLine(45, l3);
      }
      drawLine(56, "[<-] Menu");
      break;
    }

    case ADM_MESSAGE:
      drawHeader(adm_msg_title);
      drawLine(22, adm_msg_l1);
      drawLine(36, adm_msg_l2);
      break;
  }
}

void renderScreen() {
  if (!oled_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  if (console_state == STATE_ADMIN_CODE) {
    renderAdminCode();
    display.display();
    return;
  }
  if (console_state == STATE_ADMIN) {
    renderAdmin();
    display.display();
    return;
  }

#ifdef DUMMY_FLOW_TEST
  if (console_state == STATE_DUMMY_ENROLL) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(4, 3);
    display.print(F("DUMMY TEST: ENROLL"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 18);
    display.print(F("Acerca tarjeta para:"));
    display.setCursor(0, 32);
    display.print(DUMMY_ENROLL_LABELS[dummy_enrolled]);
    display.drawRect(0, 48, 128, 12, SH110X_WHITE);
    int w = (dummy_enrolled * 124) / DUMMY_CARD_COUNT;
    if (w > 0) display.fillRect(2, 50, w, 8, SH110X_WHITE);
    display.display();
    return;
  }
#endif

  if (console_state == STATE_IDLE_LEADERBOARD) {
    display.fillRect(0, 0, 128, 12, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 2);
    display.print(F("HEADHUNTERS  META:12"));
    display.setTextColor(SH110X_WHITE);

    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      int y = 16 + p * 12;
      display.setCursor(0, y);
      display.print(COMPANY_SHORT[p]);
      display.drawRect(16, y, 52, 8, SH110X_WHITE);
      uint8_t clamped = min<uint8_t>(players[p].profitability, WIN_PROFITABILITY_TARGET);
      int fill_w = (clamped * 48) / WIN_PROFITABILITY_TARGET;
      if (fill_w > 0) display.fillRect(18, y + 2, fill_w, 4, SH110X_WHITE);
      display.setCursor(71, y);
      display.printf("%2u", players[p].profitability);
      display.setCursor(89, y);
      display.printf("$%-4d", players[p].money);
    }

    display.drawLine(0, 52, 127, 52, SH110X_WHITE);
    display.setCursor(0, 55);
#ifdef DUMMY_FLOW_TEST
    display.print(DUMMY_STEP_PROMPT_OLED[dummy_step]);
#else
    display.print(F("Turno: P1  P2  P3"));
#endif
  } else if (console_state == STATE_PLAYER_SELECTED && active_player >= 0) {
    const PlayerState& ps = players[active_player];
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(COMPANY_ASCII[active_player]);
    display.setTextColor(SH110X_WHITE);

    display.setCursor(0, 17);
    display.printf("Plata: $%-4d Nv:%2u/12", ps.money, ps.profitability);
    display.setCursor(0, 29);
    display.printf("Cargos:%u/5 Cobro:%s", countHiredRoles(active_player),
                   ps.eligible_for_payout ? "SI" : "NO");
    display.setCursor(0, 41);
    display.print(F("Acerca tarjeta NFC"));
    display.drawLine(0, 52, 127, 52, SH110X_WHITE);
    display.setCursor(0, 55);
#ifdef DUMMY_FLOW_TEST
    display.print(DUMMY_STEP_PROMPT_OLED[dummy_step]);
#else
    display.print(F("[<-]Volver [$]Cobrar"));
#endif
  } else if (console_state == STATE_SHOW_FEEDBACK) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(fb_title);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 18); display.print(fb_line1);
    display.setCursor(0, 31); display.print(fb_line2);
    display.setCursor(0, 44); display.print(fb_line3);
#ifdef DUMMY_FLOW_TEST
    display.setCursor(0, 56); display.print(DUMMY_STEP_PROMPT_OLED[dummy_step]);
#endif
  } else if (console_state == STATE_WAIT_TRIVIA_CONFIRM) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("DATO OIT (+$100)"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 20); display.print(F("Acerto la respuesta?"));
    display.setCursor(0, 36); display.print(F("[$] COBRAR = SI"));
    display.setCursor(0, 48); display.print(F("[<-] ATRAS  = NO"));
  } else if (console_state == STATE_WAIT_RETURN_SCAN) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("REESTRUCTURACION"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 20); display.print(F("Escanea tu postulante"));
    display.setCursor(0, 32); display.print(F("a devolver (+50% $)"));
    display.setCursor(0, 52); display.print(F("[<-] Cancelar"));
  } else if (console_state == STATE_GAME_OVER && winner_player >= 0) {
    display.drawRect(0, 0, 128, 64, SH110X_WHITE);
    display.setCursor(20, 8);  display.print(F("*** VICTORIA! ***"));
    display.setCursor(10, 24); display.print(COMPANY_ASCII[winner_player]);
    display.setCursor(10, 37); display.printf("Nivel: %u/12  $%d",
                                              players[winner_player].profitability,
                                              players[winner_player].money);
    display.setCursor(10, 51); display.print(F("[<-] Nueva partida"));
  }

  display.display();
}

void showFeedback(const char* title, const char* l1, const char* l2, const char* l3,
                  ConsoleState next_state) {
  asciiFold(title, fb_title, sizeof(fb_title));
  asciiFold(l1, fb_line1, sizeof(fb_line1));
  asciiFold(l2, fb_line2, sizeof(fb_line2));
  asciiFold(l3, fb_line3, sizeof(fb_line3));
  state_after_feedback = next_state;
  fb_started_ms = millis();
  console_state = STATE_SHOW_FEEDBACK;
  renderScreen();
}

// ============================================================================
// Game State & Lookup Helpers
// ============================================================================
void resetGame() {
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    players[p].money = STARTING_MONEY;
    players[p].profitability = STARTING_PROFITABILITY;
    players[p].last_hired_occupation_id = -1;
    players[p].next_hire_discount = 0;
    players[p].upgrade_human_ready = false;
    players[p].eligible_for_payout = false;
    for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
      players[p].hired_candidate_by_slot[s] = -1;
    }
  }
  for (uint8_t c = 0; c < NUM_CANDIDATES; ++c) {
    candidate_taken[c] = false;
  }
  active_player = -1;
  winner_player = -1;
  pending_trivia_reward = 0;
  console_state = STATE_IDLE_LEADERBOARD;
}

// Resolves a scanned UID to a game card (dummy-test bindings first, then flash registry).
bool resolveCard(uint32_t uid, CardRef& out) {
  if (uid == 0) return false;
#ifdef DUMMY_FLOW_TEST
  for (uint8_t i = 0; i < DUMMY_CARD_COUNT; ++i) {
    if (dummy_uids[i] == uid) {
      out = DUMMY_CARD_MAP[i];
      return true;
    }
  }
#endif
  int16_t slot = registryFind(uid);
  if (slot < 0) return false;
  out = cardRef(static_cast<uint8_t>(slot));
  return true;
}

int8_t findRequiredSlot(uint8_t player_idx, uint8_t occupation_id) {
  for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
    if (COMPANIES[player_idx].required_occupations[s] == occupation_id) {
      return static_cast<int8_t>(s);
    }
  }
  return -1;
}

void printLeaderboard() {
  Serial.println(F("=== LEADERBOARD ==="));
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    Serial.printf("  %s | Nivel: %2u/%u | Plata: $%d | Cargos: %u/5 | Cobro: %s\n",
                  COMPANY_ASCII[p],
                  players[p].profitability,
                  WIN_PROFITABILITY_TARGET,
                  players[p].money,
                  countHiredRoles(p),
                  players[p].eligible_for_payout ? "SI" : "NO");
  }
}

bool checkVictory(uint8_t player_idx) {
  if (players[player_idx].profitability >= WIN_PROFITABILITY_TARGET) {
    winner_player = static_cast<int8_t>(player_idx);
    console_state = STATE_GAME_OVER;
    Serial.printf("VICTORIA! Gano %s (Nivel %u)\n",
                  COMPANY_ASCII[player_idx], players[player_idx].profitability);
    beepWin();
    renderScreen();
    return true;
  }
  return false;
}

// ============================================================================
// Core Game Actions
// ============================================================================
void handleCandidateHireOrReplace(uint8_t player_idx, uint8_t cand_idx) {
  const CandidateDef& cand = CANDIDATES[cand_idx];
  PlayerState& ps = players[player_idx];

  int8_t slot = findRequiredSlot(player_idx, cand.occupation_id);
  if (slot < 0) {
    Serial.println(F("BLOQUEADO: Ese cargo no pertenece a la guia de tu startup."));
    beepErr();
    showFeedback("BLOQUEADO!", "Cargo fuera de tu", "guia de startup.", COMPANY_ASCII[player_idx],
                 STATE_PLAYER_SELECTED);
#ifdef DUMMY_FLOW_TEST
    if (player_idx == 0 && cand_idx == 12) advanceDummyStep(0);
#endif
    return;
  }

  if (candidate_taken[cand_idx]) {
    Serial.println(F("BLOQUEADO: Postulante ya contratado."));
    beepErr();
    showFeedback("NO DISPONIBLE", cand.name, "Ya fue contratado.", "", STATE_PLAYER_SELECTED);
    return;
  }

  int8_t prev_idx = ps.hired_candidate_by_slot[slot];
  bool is_replacement = (prev_idx >= 0);
  bool is_upgrade = is_replacement && ps.upgrade_human_ready &&
                    CANDIDATES[prev_idx].modality == MOD_HUMANO &&
                    cand.modality == MOD_HUMANO_IA;

  if (is_replacement && !is_upgrade &&
      ps.last_hired_occupation_id == static_cast<int8_t>(cand.occupation_id)) {
    Serial.println(F("BLOQUEADO: Debes contratar otro cargo antes de reemplazar este."));
    beepErr();
    showFeedback("COOLDOWN!", "Contrata otro cargo", "antes de reemplazar", "esta ocupacion.",
                 STATE_PLAYER_SELECTED);
#ifdef DUMMY_FLOW_TEST
    if (player_idx == 0 && cand_idx == 1) advanceDummyStep(5);
#endif
    return;
  }

  int16_t final_cost = cand.cost;
  if (is_upgrade) {
    final_cost = 100;
  } else if (ps.next_hire_discount > 0) {
    final_cost = max<int16_t>(0, final_cost - ps.next_hire_discount);
  }

  if (ps.money < final_cost) {
    Serial.println(F("BLOQUEADO: Dinero insuficiente."));
    beepErr();
    char buf[32];
    snprintf(buf, sizeof(buf), "Costo:$%d Hay:$%d", final_cost, ps.money);
    showFeedback("SIN DINERO!", cand.name, buf, "", STATE_PLAYER_SELECTED);
    return;
  }

  // Only consume the upgrade/discount once the hire actually goes through.
  if (is_upgrade) ps.upgrade_human_ready = false;
  else if (ps.next_hire_discount > 0) ps.next_hire_discount = 0;

  if (is_replacement) {
    candidate_taken[prev_idx] = false;
    ps.profitability -= CANDIDATES[prev_idx].profitability;
  }

  ps.money -= final_cost;
  ps.profitability += cand.profitability;
  ps.hired_candidate_by_slot[slot] = static_cast<int8_t>(cand_idx);
  ps.last_hired_occupation_id = static_cast<int8_t>(cand.occupation_id);
  ps.eligible_for_payout = true;
  candidate_taken[cand_idx] = true;

  char name_ascii[32];
  asciiFold(cand.name, name_ascii, sizeof(name_ascii));
  Serial.printf("%s %s %s (%s, $%d) -> Nivel: %u/12 | Saldo: $%d\n",
                COMPANY_ASCII[player_idx],
                is_replacement ? "reemplazo por" : "contrato a",
                name_ascii, modalityLabel(cand.modality), final_cost,
                ps.profitability, ps.money);
  beepOk();
  printLeaderboard();

  if (checkVictory(player_idx)) return;

  char l2[32], l3[32];
  snprintf(l2, sizeof(l2), "%s -$%d", modalityLabel(cand.modality), final_cost);
  snprintf(l3, sizeof(l3), "+%u rent (Nv:%u/12)", cand.profitability, ps.profitability);
  showFeedback(is_replacement ? "REEMPLAZADO!" : "CONTRATADO!", cand.name, l2, l3,
               STATE_PLAYER_SELECTED);

#ifdef DUMMY_FLOW_TEST
  if (player_idx == 0 && cand_idx == 0) advanceDummyStep(4);
  if (player_idx == 0 && cand_idx == 7) advanceDummyStep(6);
  if (player_idx == 0 && cand_idx == 1 && is_replacement) advanceDummyStep(8);
#endif
}

void handleCandidateReturn(uint8_t player_idx, uint8_t cand_idx) {
  PlayerState& ps = players[player_idx];
  const CandidateDef& cand = CANDIDATES[cand_idx];
  int8_t slot = findRequiredSlot(player_idx, cand.occupation_id);
  if (slot < 0 || ps.hired_candidate_by_slot[slot] != static_cast<int8_t>(cand_idx)) {
    Serial.println(F("Ese postulante no pertenece a tu equipo."));
    beepErr();
    showFeedback("ERROR", "No es tu postulante.", "Escanea uno tuyo o", "presiona Cancelar.",
                 STATE_WAIT_RETURN_SCAN);
    return;
  }

  ps.hired_candidate_by_slot[slot] = -1;
  candidate_taken[cand_idx] = false;
  ps.profitability = max<int>(0, ps.profitability - cand.profitability);
  int16_t refund = cand.cost / 2;
  ps.money += refund;

  Serial.printf("Postulante devuelto (%s). Reembolso: +$%d | Nivel: %u\n", cand.name, refund, ps.profitability);
  beepOk();
  printLeaderboard();

  char l2[32], l3[32];
  snprintf(l2, sizeof(l2), "Reembolso: +$%d", refund);
  snprintf(l3, sizeof(l3), "Nv:%u Saldo:$%d", ps.profitability, ps.money);
  showFeedback("DEVUELTO", cand.name, l2, l3, STATE_PLAYER_SELECTED);
}

void handleInsightScan(uint8_t player_idx, uint8_t occ_idx) {
  PlayerState& ps = players[player_idx];
  if (ps.money < INSIGHT_COST) {
    Serial.println(F("Dinero insuficiente para comprar insight."));
    beepErr();
    showFeedback("SIN DINERO!", "Insight cuesta $50.", "", "", STATE_PLAYER_SELECTED);
    return;
  }
  ps.money -= INSIGHT_COST;
  ps.eligible_for_payout = true;

  const OccupationDef& occ = OCCUPATIONS[occ_idx];
  Serial.printf("INSIGHT (-$%d): %s | OIT %.2f (%s) | Ideal: %s | Saldo: $%d\n",
                INSIGHT_COST, occ.role, occ.oit_score, occ.oit_level,
                modalityLabel(occ.ideal_modality), ps.money);
  beepOk();
  printLeaderboard();

  char l2[32], l3[32];
  snprintf(l2, sizeof(l2), "OIT %.2f (%s)", occ.oit_score, occ.oit_level);
  snprintf(l3, sizeof(l3), "Ideal: %s", modalityLabel(occ.ideal_modality));
  showFeedback("INSIGHT -$50", occ.room, l2, l3, STATE_PLAYER_SELECTED);

#ifdef DUMMY_FLOW_TEST
  if (player_idx == 0 && occ_idx == 0) advanceDummyStep(2);
#endif
}

void handleMysteryScan(uint8_t player_idx, uint8_t card_idx) {
  PlayerState& ps = players[player_idx];
  const MysteryCardDef& card = MYSTERY_CARDS[card_idx];
  char l2[32] = "", l3[32] = "";

  Serial.printf("MISTERIO (#%u): %s\n", card.id, card.title);

  switch (card.effect) {
    case MYS_ADD_MONEY:
      ps.money += card.value;
      snprintf(l2, sizeof(l2), "+$%d al banco", card.value);
      snprintf(l3, sizeof(l3), "Saldo: $%d", ps.money);
      beepOk();
      showFeedback("MISTERIO", card.title, l2, l3, STATE_PLAYER_SELECTED);
      break;
    case MYS_BONUS_PER_MODALITY: {
      int count = 0;
      for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
        int8_t ci = ps.hired_candidate_by_slot[s];
        if (ci >= 0 && CANDIDATES[ci].modality == card.target_modality) count++;
      }
      int16_t delta = count * card.value;
      ps.money += delta;
      snprintf(l2, sizeof(l2), "%d x $%d = +$%d", count, card.value, delta);
      snprintf(l3, sizeof(l3), "Saldo: $%d", ps.money);
      beepOk();
      showFeedback("MISTERIO", card.title, l2, l3, STATE_PLAYER_SELECTED);
      break;
    }
    case MYS_PENALTY_PER_MODALITY: {
      int count = 0;
      for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
        int8_t ci = ps.hired_candidate_by_slot[s];
        if (ci >= 0 && CANDIDATES[ci].modality == card.target_modality) count++;
      }
      int16_t delta = count * card.value;
      ps.money = max<int16_t>(0, ps.money - delta);
      snprintf(l2, sizeof(l2), "Multa: -$%d", delta);
      snprintf(l3, sizeof(l3), "Saldo: $%d", ps.money);
      beepErr();
      showFeedback("MISTERIO", card.title, l2, l3, STATE_PLAYER_SELECTED);
      break;
    }
    case MYS_PENALTY_LOW_IA: {
      int count = 0;
      for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
        int8_t ci = ps.hired_candidate_by_slot[s];
        if (ci >= 0 && CANDIDATES[ci].modality == MOD_IA_SOLA && CANDIDATES[ci].profitability == 1) {
          count++;
        }
      }
      if (count > 0) {
        ps.money = max<int16_t>(0, ps.money - card.value);
        snprintf(l2, sizeof(l2), "Multa IA +1: -$%d", card.value);
        beepErr();
      } else {
        snprintf(l2, sizeof(l2), "Sin IA +1: $0 multa");
        beepOk();
      }
      snprintf(l3, sizeof(l3), "Saldo: $%d", ps.money);
      showFeedback("AUDITORIA", card.title, l2, l3, STATE_PLAYER_SELECTED);
      break;
    }
    case MYS_FORCE_RECEPTION_NO_PAY:
      ps.eligible_for_payout = false;
      beepErr();
      showFeedback("CAIDA SERVIDOR", "Vuelve a Recepcion", "SIN cobrar trimestre", "",
                   STATE_PLAYER_SELECTED);
      break;
    case MYS_ENABLE_PAYOUT_RECEPTION:
      ps.eligible_for_payout = true;
      beepOk();
      showFeedback("PITCH EXITOSO!", "Ve a Recepcion y", "presiona COBRAR.", "",
                   STATE_PLAYER_SELECTED);
      break;
    case MYS_NEXT_HIRE_DISCOUNT:
      ps.next_hire_discount += card.value;
      beepOk();
      showFeedback("DESCUENTO", "Proxima contratacion", "cuesta -$100!", "",
                   STATE_PLAYER_SELECTED);
      break;
    case MYS_UPGRADE_HUMAN_TO_HYBRID:
      ps.upgrade_human_ready = true;
      beepOk();
      showFeedback("CAPACITACION", "Mejora 1 Humano a", "Hum+IA por solo $100", "",
                   STATE_PLAYER_SELECTED);
      break;
    case MYS_RETURN_CANDIDATE_HALF:
      console_state = STATE_WAIT_RETURN_SCAN;
      Serial.println(F("Escanea el postulante que deseas devolver (o presiona Cancelar)."));
      beepOk();
      renderScreen();
      return;
    case MYS_TRIVIA_CONFIRM:
      pending_trivia_reward = card.value;
      console_state = STATE_WAIT_TRIVIA_CONFIRM;
      Serial.println(F("Dato OIT: presiona COBRAR si acerto (+$100), o ATRAS/CANCELAR si fallo."));
      beepOk();
      renderScreen();
      return;
    case MYS_BOARD_INFO_ONLY:
    default:
      beepOk();
      showFeedback("MISTERIO", card.title, "Aplica en el tablero", "", STATE_PLAYER_SELECTED);
      break;
  }
  printLeaderboard();
}

void handleCobrar(uint8_t player_idx) {
  PlayerState& ps = players[player_idx];
  if (!ps.eligible_for_payout) {
    Serial.println(F("BLOQUEADO: Debes comprar algo o sacar Pitch exitoso desde tu ultimo cobro."));
    beepErr();
    showFeedback("COBRO BLOQUEADO", "Compra un cargo,", "insight o Pitch", "antes de cobrar.",
                 STATE_PLAYER_SELECTED);
#ifdef DUMMY_FLOW_TEST
    if (player_idx == 0 && dummy_step == 1) advanceDummyStep(1);
#endif
    return;
  }
  int16_t payout = RECEPTION_BASE_PAYOUT + (ps.profitability * RECEPTION_PAYOUT_PER_LEVEL);
  ps.money += payout;
  ps.eligible_for_payout = false;

  Serial.printf("%s cobro +$%d ($%d base + %u*$%d) -> Saldo: $%d\n",
                COMPANY_ASCII[player_idx], payout, RECEPTION_BASE_PAYOUT,
                ps.profitability, RECEPTION_PAYOUT_PER_LEVEL, ps.money);
  beepOk();
  printLeaderboard();

  char l1[32], l2[32];
  snprintf(l1, sizeof(l1), "+$%d (Nivel %u)", payout, ps.profitability);
  snprintf(l2, sizeof(l2), "Saldo total: $%d", ps.money);
  showFeedback("COBRO RECEPCION", l1, l2, "", STATE_PLAYER_SELECTED);

#ifdef DUMMY_FLOW_TEST
  if (player_idx == 0 && dummy_step == 3) advanceDummyStep(3);
  if (player_idx == 0 && dummy_step == 7) advanceDummyStep(7);
#endif
}

// ============================================================================
// Admin Mode — assign NFC tags to game cards from the board itself
// ============================================================================
void printSlotSerial(const char* prefix, uint8_t slot) {
  char code[6], l1[22], l2[22], l3[22];
  cardCode(slot, code, sizeof(code));
  cardDescribe(slot, l1, l2, l3, sizeof(l1));
  uint32_t uid = registryUid(slot);
  Serial.printf("%s %s %-10s | %s %s | %s | UID: %s", prefix, code,
                cardKindLabel(cardRef(slot).kind), l1, l2, l3, uid ? "" : "(vacia)");
  if (uid) Serial.printf("%08lX", static_cast<unsigned long>(uid));
  Serial.println();
}

void dumpRegistrySerial() {
  Serial.printf("BEGIN DUMP %u/%u\n", registryCount(), NUM_CARDS);
  for (uint8_t s = 0; s < NUM_CARDS; ++s) {
    char code[6];
    cardCode(s, code, sizeof(code));
    Serial.printf("CARD %s %08lX\n", code, static_cast<unsigned long>(registryUid(s)));
  }
  Serial.println(F("END DUMP"));
}

void adminMessage(const char* title, const char* l1, const char* l2, AdminScreen next,
                  uint32_t duration_ms) {
  asciiFold(title, adm_msg_title, sizeof(adm_msg_title));
  asciiFold(l1, adm_msg_l1, sizeof(adm_msg_l1));
  asciiFold(l2, adm_msg_l2, sizeof(adm_msg_l2));
  adm_after_msg = next;
  adm_msg_until = millis() + duration_ms;
  adm_screen = ADM_MESSAGE;
  renderScreen();
}

// Next slot in direction dir (+1/-1), optionally only unassigned ones. -1 if none.
int16_t adminFindSlot(int16_t from, int8_t dir, bool missing_only) {
  for (int16_t step = 1; step <= NUM_CARDS; ++step) {
    int16_t s = ((from + dir * step) % NUM_CARDS + NUM_CARDS) % NUM_CARDS;
    if (!missing_only || registryUid(static_cast<uint8_t>(s)) == 0) return s;
  }
  return -1;
}

void adminShowWalk() {
  adm_screen = ADM_WALK;
  printSlotSerial("[ADMIN] Escanea ->", adm_slot);
  renderScreen();
}

void adminShowMenu() {
  adm_screen = ADM_MENU;
  Serial.printf("[ADMIN] Menu (%u/%u asignadas): P1=faltantes P2=revisar todas "
                "P3=probar $=borrar todas <-=salir\n", registryCount(), NUM_CARDS);
  renderScreen();
}

void adminGoTo(AdminScreen s) {
  if (s == ADM_WALK) adminShowWalk();
  else if (s == ADM_MENU) adminShowMenu();
  else { adm_screen = s; renderScreen(); }
}

void adminStartWalk(bool missing_only) {
  adm_missing_only = missing_only;
  if (!missing_only) {
    adm_slot = 0;
  } else {
    int16_t s = registryUid(0) == 0 ? 0 : adminFindSlot(0, +1, true);
    if (s < 0) {
      beepOk();
      adminMessage("TODO LISTO", "No faltan tarjetas.", "50/50 asignadas", ADM_MENU, 2000);
      return;
    }
    adm_slot = static_cast<uint8_t>(s);
  }
  beepOk();
  adminShowWalk();
}

void adminAfterAssign(bool already_was) {
  char code[6], l1[22], l2[22], l3[22], done_line[22], saved_line[22];
  cardCode(adm_slot, code, sizeof(code));
  cardDescribe(adm_slot, l1, l2, l3, sizeof(l1));
  snprintf(saved_line, sizeof(saved_line), "%s %s", code, already_was ? "(sin cambios)" : "guardada");
  snprintf(done_line, sizeof(done_line), "%u/%u asignadas", registryCount(), NUM_CARDS);
  Serial.printf("[ADMIN] %s = %08lX\n", code, static_cast<unsigned long>(registryUid(adm_slot)));

  int16_t next;
  bool finished;
  if (adm_missing_only) {
    next = adminFindSlot(adm_slot, +1, true);
    finished = next < 0;
  } else {
    next = adm_slot + 1;
    finished = next >= NUM_CARDS;
  }

  if (finished) {
    beepAdminEnter();
    adminMessage(adm_missing_only ? "COMPLETO!" : "FIN RECORRIDO", saved_line, done_line,
                 ADM_MENU, 2500);
  } else {
    adm_slot = static_cast<uint8_t>(next);
    adminMessage(already_was ? "YA ERA ESTA" : "GUARDADA!", saved_line, l1, ADM_WALK, 900);
  }
}

void adminHandleWalkScan(uint32_t uid) {
  int16_t owner = registryFind(uid);
  if (owner == adm_slot) {
    beepOk();
    adminAfterAssign(true);
  } else if (owner >= 0) {
    adm_conflict_slot = owner;
    adm_pending_uid = uid;
    adm_screen = ADM_CONFLICT;
    printSlotSerial("[ADMIN] Tarjeta ya asignada a", static_cast<uint8_t>(owner));
    beepErr();
    renderScreen();
  } else if (registryAssign(adm_slot, uid)) {
    beepOk();
    adminAfterAssign(false);
  } else {
    beepErr();
    adminMessage("ERROR FLASH", "No se pudo guardar.", "Intenta de nuevo.", ADM_WALK, 2000);
  }
}

void enterAdmin(ConsoleState from_state) {
  resume_state = (from_state == STATE_SHOW_FEEDBACK) ? state_after_feedback : from_state;
  console_state = STATE_ADMIN;
  Serial.println(F("\n===== MODO ADMIN: asignacion de tarjetas NFC ====="));
  beepAdminEnter();
  adminShowMenu();
}

void exitAdmin() {
  char l1[22];
  snprintf(l1, sizeof(l1), "Tarjetas: %u/%u", registryCount(), NUM_CARDS);
  Serial.printf("[ADMIN] Saliendo. %s guardadas en flash.\n", l1);
  dumpRegistrySerial();
  beepOk();
  showFeedback("ADMIN CERRADO", l1, "Guardadas en flash.", "A jugar!", resume_state);
}

void adminLoop() {
  if (adm_screen == ADM_MESSAGE) {
    bool skip = btnPressed(BTN_P1) || btnPressed(BTN_P2) || btnPressed(BTN_P3) || btnPressed(BTN_COBRAR);
    if (skip || static_cast<int32_t>(millis() - adm_msg_until) >= 0) adminGoTo(adm_after_msg);
    return;  // don't read cards while a message is up; they'll be read right after
  }

  uint32_t uid = 0;
  bool scanned = false;
  if (adm_screen == ADM_WALK || adm_screen == ADM_TEST) scanned = readCard(uid);

  switch (adm_screen) {
    case ADM_MENU:
      if (btnPressed(BTN_P1)) adminStartWalk(true);
      else if (btnPressed(BTN_P2)) adminStartWalk(false);
      else if (btnPressed(BTN_P3)) {
        adm_test_uid = 0;
        adm_test_slot = -1;
        beepOk();
        adminGoTo(ADM_TEST);
      } else if (btnPressed(BTN_COBRAR)) {
        if (registryCount() == 0) {
          beepErr();
          adminMessage("NADA QUE BORRAR", "No hay tarjetas", "asignadas.", ADM_MENU, 1500);
        } else {
          adm_confirm = CONFIRM_WIPE_ALL;
          beepErr();
          adminGoTo(ADM_CONFIRM);
        }
      } else if (btnTapped(BTN_BACK)) {
        exitAdmin();
      }
      break;

    case ADM_WALK:
      if (scanned) {
        adminHandleWalkScan(uid);
      } else if (btnPressed(BTN_P1) || btnPressed(BTN_P2)) {
        int8_t dir = btnPressed(BTN_P1) ? -1 : +1;
        int16_t s = adminFindSlot(adm_slot, dir, adm_missing_only);
        if (s < 0 || s == adm_slot) {
          beepErr();
        } else {
          adm_slot = static_cast<uint8_t>(s);
          beepTick();
          adminShowWalk();
        }
      } else if (btnPressed(BTN_P3)) {
        if (registryUid(adm_slot) == 0) {
          beepErr();
        } else {
          adm_confirm = CONFIRM_CLEAR_ONE;
          beepErr();
          adminGoTo(ADM_CONFIRM);
        }
      } else if (btnTapped(BTN_BACK)) {
        beepOk();
        adminShowMenu();
      }
      break;

    case ADM_CONFLICT:
      if (btnPressed(BTN_COBRAR)) {
        if (registryAssign(adm_slot, adm_pending_uid)) {
          beepOk();
          adminAfterAssign(false);
        } else {
          beepErr();
          adminMessage("ERROR FLASH", "No se pudo guardar.", "", ADM_WALK, 2000);
        }
      } else if (btnTapped(BTN_BACK)) {
        beepOk();
        adminShowWalk();
      }
      break;

    case ADM_CONFIRM:
      if (btnPressed(BTN_COBRAR)) {
        if (adm_confirm == CONFIRM_CLEAR_ONE) {
          char code[6];
          cardCode(adm_slot, code, sizeof(code));
          bool ok = registryClear(adm_slot);
          ok ? beepOk() : beepErr();
          adminMessage(ok ? "BORRADA" : "ERROR FLASH", code, ok ? "Sin tarjeta asignada" : "",
                       ADM_WALK, 1200);
        } else {
          bool ok = registryClearAll();
          ok ? beepOk() : beepErr();
          adminMessage(ok ? "TODO BORRADO" : "ERROR FLASH", ok ? "0 tarjetas asignadas" : "",
                       "", ADM_MENU, 1800);
          Serial.println(ok ? F("[ADMIN] Todas las tarjetas fueron borradas.") : F("[ADMIN] Error de flash."));
        }
      } else if (btnTapped(BTN_BACK)) {
        beepOk();
        adminGoTo(adm_confirm == CONFIRM_CLEAR_ONE ? ADM_WALK : ADM_MENU);
      }
      break;

    case ADM_TEST:
      if (scanned) {
        adm_test_uid = uid;
        adm_test_slot = registryFind(uid);
        if (adm_test_slot >= 0) {
          beepOk();
          printSlotSerial("[ADMIN] Prueba:", static_cast<uint8_t>(adm_test_slot));
        } else {
          beepErr();
          Serial.println(F("[ADMIN] Prueba: tarjeta NO registrada."));
        }
        renderScreen();
      } else if (btnTapped(BTN_BACK)) {
        beepOk();
        adminShowMenu();
      }
      break;

    case ADM_MESSAGE:
      break;
  }
}

// Detects the 10 s BACK hold that arms the admin code. Returns true if it fired.
bool checkAdminHold() {
  Button& b = buttons[BTN_BACK];
  if (!b.down || b.suppress_release || millis() - b.down_since_ms < ADMIN_HOLD_MS) return false;
  b.suppress_release = true;  // releasing BACK now must not act as "back"
  resume_state = (console_state == STATE_SHOW_FEEDBACK) ? state_after_feedback : console_state;
  code_progress = 0;
  code_last_ms = millis();
  console_state = STATE_ADMIN_CODE;
  beepAdminArmed();
  renderScreen();
  return true;
}

void adminCodeLoop() {
  bool fail = (millis() - code_last_ms) > ADMIN_CODE_STEP_TIMEOUT_MS;
  for (uint8_t i = 0; i < NUM_BUTTONS && !fail; ++i) {
    if (!btnPressed(i)) continue;
    if (i == ADMIN_CODE[code_progress]) {
      code_progress++;
      code_last_ms = millis();
      if (code_progress >= ADMIN_CODE_LEN) {
        enterAdmin(resume_state);
        return;
      }
      beepTick();
      renderScreen();
    } else {
      fail = true;
    }
  }
  if (fail) {
    buttons[BTN_BACK].suppress_release = true;  // a BACK press here only cancels
    beepErr();
    console_state = resume_state;
    renderScreen();
  }
}

// Serial commands: "admin", "dump", "help".
void pollSerial() {
  static char buf[24];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c != '\n' && c != '\r') {
      if (len + 1 < sizeof(buf)) buf[len++] = c;
      continue;
    }
    if (len == 0) continue;
    buf[len] = '\0';
    len = 0;
    if (strcmp(buf, "dump") == 0) {
      dumpRegistrySerial();
    } else if (strcmp(buf, "admin") == 0) {
      if (console_state != STATE_ADMIN) enterAdmin(console_state);
    } else if (strcmp(buf, "help") == 0) {
      Serial.println(F("Comandos: dump (lista UIDs en flash), admin (abre modo admin)"));
    } else {
      Serial.printf("Comando desconocido: %s (escribe 'help')\n", buf);
    }
  }
}

// ============================================================================
// Setup & Main Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(600);

  for (uint8_t i = 0; i < NUM_BUTTONS; ++i) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
  }
  pinMode(PIN_BUZZER, OUTPUT);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  oled_ok = display.begin(OLED_I2C_ADDR, true);
  Serial.printf("OLED SH1106 (0x%02X): %s\n", OLED_I2C_ADDR, oled_ok ? "OK" : "NOT FOUND");

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();
  rfid.PCD_AntennaOn();
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522 VersionReg: 0x%02X\n", ver);

  RegistrySource src = registryBegin();
  uint8_t assigned = registryCount();
  Serial.printf("Tarjetas NFC: %u/%u asignadas (fuente: %s)\n", assigned, NUM_CARDS,
                src == REG_SOURCE_FLASH ? "memoria flash" : "game_data.h");

  resetGame();

#ifdef DUMMY_FLOW_TEST
  console_state = STATE_DUMMY_ENROLL;
  Serial.println(F("\n=================================================="));
  Serial.println(F("  HEADHUNTERS — GUIDED END-TO-END DUMMY FLOW TEST"));
  Serial.println(F("=================================================="));
  Serial.println(F("Grab ANY 6 NFC cards on your desk to bind them temporarily:"));
  Serial.printf("  Tap %s\n", DUMMY_ENROLL_SERIAL[0]);
  beepOk();
  renderScreen();
#else
  Serial.println(F("\nHeadhunters Consola ESP-32D lista."));
  Serial.println(F("Modo admin: mantener ATRAS 10 s, soltar, luego P1-P2-P3 (o escribe 'admin')."));
  printLeaderboard();
  beepOk();
  if (assigned < NUM_CARDS) {
    char l1[22];
    snprintf(l1, sizeof(l1), "Tarjetas: %u/%u", assigned, NUM_CARDS);
    showFeedback("FALTAN TARJETAS", l1, "Asignalas en el", "modo admin.", STATE_IDLE_LEADERBOARD);
  } else {
    renderScreen();
  }
#endif
}

void loop() {
  updateButtons();
  pollSerial();

  if (console_state == STATE_ADMIN) {
    adminLoop();
    return;
  }
  if (console_state == STATE_ADMIN_CODE) {
    adminCodeLoop();
    return;
  }
  if (checkAdminHold()) return;

  // Auto-dismiss feedback screen after timeout
  if (console_state == STATE_SHOW_FEEDBACK && (millis() - fb_started_ms >= FEEDBACK_DURATION_MS)) {
    console_state = state_after_feedback;
    renderScreen();
  }

  // Handle Game Over reset
  if (console_state == STATE_GAME_OVER) {
    if (btnTapped(BTN_BACK)) {
      resetGame();
      beepOk();
      printLeaderboard();
      renderScreen();
    }
    return;
  }

#ifdef DUMMY_FLOW_TEST
  if (console_state == STATE_DUMMY_ENROLL) {
    uint32_t uid_hash;
    if (readCard(uid_hash)) {
      bool dup = false;
      for (uint8_t i = 0; i < dummy_enrolled; ++i) {
        if (dummy_uids[i] == uid_hash) dup = true;
      }
      if (dup) {
        Serial.println(F("  [WARN] Already used that card! Tap a DIFFERENT card."));
        beepErr();
      } else {
        dummy_uids[dummy_enrolled] = uid_hash;
        Serial.printf("  [OK] Bound UID 0x%08lX -> %s\n",
                      static_cast<unsigned long>(uid_hash), DUMMY_ENROLL_LABELS[dummy_enrolled]);
        dummy_enrolled++;
        beepOk();
        if (dummy_enrolled >= DUMMY_CARD_COUNT) {
          console_state = STATE_IDLE_LEADERBOARD;
          printLeaderboard();
          printDummyStepPrompt();
        } else {
          Serial.printf("  Tap %s\n", DUMMY_ENROLL_SERIAL[dummy_enrolled]);
        }
        renderScreen();
      }
      delay(300);
    }
    return;
  }
#endif

  // Player selector buttons (P1, P2, P3)
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (btnPressed(p)) {
      active_player = static_cast<int8_t>(p);
      console_state = STATE_PLAYER_SELECTED;
      Serial.printf("Jugador activo: %s\n", COMPANY_ASCII[p]);
      beepOk();
      renderScreen();
    }
  }

  // BACK / CANCEL button (acts on release)
  if (btnTapped(BTN_BACK)) {
    if (console_state == STATE_WAIT_TRIVIA_CONFIRM) {
      pending_trivia_reward = 0;
      Serial.println(F("Trivia fallida: $0."));
      beepErr();
      showFeedback("DATO OIT", "Respuesta incorrecta", "Cobras $0.", "", STATE_PLAYER_SELECTED);
    } else if (console_state == STATE_WAIT_RETURN_SCAN) {
      console_state = STATE_PLAYER_SELECTED;
      Serial.println(F("Devolucion cancelada."));
      beepOk();
      renderScreen();
    } else {
      active_player = -1;
      console_state = STATE_IDLE_LEADERBOARD;
      printLeaderboard();
      beepOk();
#ifdef DUMMY_FLOW_TEST
      if (dummy_step == 10) advanceDummyStep(10);
#endif
      renderScreen();
    }
  }

  // COBRAR / CONFIRM button
  if (btnPressed(BTN_COBRAR)) {
    if (console_state == STATE_WAIT_TRIVIA_CONFIRM && active_player >= 0) {
      players[active_player].money += pending_trivia_reward;
      int16_t reward = pending_trivia_reward;
      pending_trivia_reward = 0;
      Serial.printf("Trivia correcta! +$%d -> Saldo: $%d\n", reward, players[active_player].money);
      beepOk();
      printLeaderboard();
      char l2[22];
      snprintf(l2, sizeof(l2), "Saldo: $%d", players[active_player].money);
      showFeedback("CORRECTO!", "+$100 Dato OIT", l2, "", STATE_PLAYER_SELECTED);
#ifdef DUMMY_FLOW_TEST
      if (dummy_step == 9) advanceDummyStep(9);
#endif
    } else if (console_state == STATE_WAIT_RETURN_SCAN) {
      beepErr();  // must scan a candidate or cancel
    } else if (active_player >= 0) {
      handleCobrar(static_cast<uint8_t>(active_player));
    } else {
      Serial.println(F("Selecciona un jugador primero (P1, P2 o P3)."));
      beepErr();
      showFeedback("ELIGE JUGADOR", "Presiona P1, P2 o P3", "antes de cobrar.", "",
                   STATE_IDLE_LEADERBOARD);
    }
  }

  // NFC / RFID Scan
  uint32_t uid_hash;
  if (readCard(uid_hash)) {
    CardRef card;
    bool known = resolveCard(uid_hash, card);

    if (active_player < 0) {
      Serial.println(F("Selecciona un jugador primero (P1, P2 o P3)."));
      beepErr();
      showFeedback("ELIGE JUGADOR", "Presiona P1, P2 o P3", "antes de escanear.", "",
                   STATE_IDLE_LEADERBOARD);
    } else if (console_state == STATE_WAIT_RETURN_SCAN) {
      if (known && card.kind == CARD_CANDIDATE) {
        handleCandidateReturn(static_cast<uint8_t>(active_player), card.index);
      } else {
        Serial.println(F("Escanea la tarjeta del postulante a devolver."));
        beepErr();
      }
    } else if (console_state == STATE_WAIT_TRIVIA_CONFIRM) {
      beepErr();  // answer the trivia first (COBRAR = Si / ATRAS = No)
    } else if (!known) {
      Serial.println(F("Tag no registrado. Asignalo en el modo admin."));
      beepErr();
      showFeedback("TAG DESCONOCIDO", "Tarjeta sin asignar.", "Registrala en el", "modo admin.",
                   STATE_PLAYER_SELECTED);
    } else if (card.kind == CARD_CANDIDATE) {
      handleCandidateHireOrReplace(static_cast<uint8_t>(active_player), card.index);
    } else if (card.kind == CARD_OCCUPATION) {
      handleInsightScan(static_cast<uint8_t>(active_player), card.index);
    } else {
      handleMysteryScan(static_cast<uint8_t>(active_player), card.index);
    }
    delay(250);
  }
}
