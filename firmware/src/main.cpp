#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>

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
//
// Rules implemented (Instructivo A5 + Guias por empresa, data/Updated game data/):
//   - Everyone starts with $500 and profitability level 2.
//   - Hiring: pay the card price, profitability goes up +1/+2/+3 (OIT data).
//     Replacing a role costs the full price of the new card; the old card goes
//     back to its room with no refund.
//   - Recepcion: $100 per profitability level, only if the player entered at
//     least one room since the last payout (a hire counts; otherwise the console
//     asks for confirmation).
//   - No money: you can't hire. If you must pay and can't, you return one card
//     to its room with no refund.
//   - End: when someone completes their 5 roles a last round is played; then
//     highest profitability wins, ties broken by money.
//
// Turn flow: start screen -> [$] -> random starting player -> each turn the
// console names the player, who rolls, moves and does at most ONE action
// (hire a candidate, draw a Misterio card, or collect at Recepcion), then ends
// the turn with [<-]. Insight cards are free extras and don't use the action.
// After someone completes 5/5, every other player gets one last turn and the
// console shows the final ranking by itself.
// ============================================================================

constexpr uint8_t OLED_I2C_ADDR = 0x3C;

// OLED orientation (the screen is mounted backwards in the box):
//   0 = normal, 1 = flipped vertically, 2 = rotated 180 degrees (default).
// Change it without reflashing by typing "oled 0", "oled 1" or "oled 2" in the
// Serial Monitor (saved in flash), or at build time with -DOLED_ORIENTATION_DEFAULT=n.
#ifndef OLED_ORIENTATION_DEFAULT
#define OLED_ORIENTATION_DEFAULT 2
#endif
enum OledOrientation : uint8_t {
  OLED_NORMAL = 0,
  OLED_FLIP_VERTICAL = 1,
  OLED_ROTATE_180 = 2,
};
constexpr const char* OLED_NVS_NAMESPACE = "hh_cfg";
constexpr const char* OLED_NVS_KEY = "oled";
constexpr uint32_t FEEDBACK_DURATION_MS = 9000;  // game messages; [<-] skips them
constexpr uint32_t PEEK_DURATION_MS = 5000;      // P1-P3 scoreboard peek

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
  STATE_START = 0,            // title screen, [$] starts a game
  STATE_ROLL_STARTER,         // animation picking the starting player
  STATE_TURN,                 // active_player's turn
  STATE_PEEK_SCORES,          // P1-P3 during a turn: scoreboard for a few seconds
  STATE_SHOW_FEEDBACK,
  STATE_WAIT_TRIVIA_CONFIRM,
  STATE_WAIT_PAYOUT_CONFIRM,  // COBRAR without a hire since last payout: "did you enter a room?"
  STATE_WAIT_RETURN_SCAN,
  STATE_WAIT_UPGRADE_SCAN,    // Capacitacion interna: scan the Humano + IA that replaces your Humano
  STATE_WAIT_AUDIT_CHOICE,    // Auditoria: [$] pay the fine / [<-] return the IA +1
  STATE_WAIT_SWAP_MINE,       // Fuga de talento: scan your own card
  STATE_WAIT_SWAP_THEIRS,     // Fuga de talento: scan the other player's card (same role)
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
  int16_t next_hire_discount;
  bool eligible_for_payout;  // hired (so entered a room) or Pitch exitoso since last payout
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
uint8_t oled_orientation = OLED_ORIENTATION_DEFAULT;

const char* oledOrientationLabel(uint8_t mode) {
  switch (mode) {
    case OLED_NORMAL:        return "normal";
    case OLED_FLIP_VERTICAL: return "volteada vertical";
    case OLED_ROTATE_180:    return "girada 180";
  }
  return "?";
}

// Vertical flip is done in the controller (COM scan direction); 180 degrees is
// done in software so the SH1106's 132-column RAM offset doesn't shift the image.
void applyOledOrientation(uint8_t mode) {
  if (mode > OLED_ROTATE_180) mode = OLED_ORIENTATION_DEFAULT;
  oled_orientation = mode;
  if (!oled_ok) return;
  display.oled_command(0xA1);  // segment remap: library default (left-right as normal)
  display.oled_command(mode == OLED_FLIP_VERTICAL ? 0xC0 : 0xC8);  // COM scan: up / down
  display.setRotation(mode == OLED_ROTATE_180 ? 2 : 0);
}

uint8_t loadOledOrientation() {
  Preferences prefs;
  uint8_t mode = OLED_ORIENTATION_DEFAULT;
  if (prefs.begin(OLED_NVS_NAMESPACE, true)) {
    mode = prefs.getUChar(OLED_NVS_KEY, OLED_ORIENTATION_DEFAULT);
    prefs.end();
  }
  return mode <= OLED_ROTATE_180 ? mode : OLED_ORIENTATION_DEFAULT;
}

void saveOledOrientation(uint8_t mode) {
  Preferences prefs;
  if (prefs.begin(OLED_NVS_NAMESPACE, false)) {
    prefs.putUChar(OLED_NVS_KEY, mode);
    prefs.end();
  }
}

PlayerState players[NUM_PLAYERS];
bool candidate_taken[NUM_CANDIDATES];
ConsoleState console_state = STATE_START;
ConsoleState state_after_feedback = STATE_START;
int8_t active_player = -1;       // whose turn it is (-1 before the game starts)
int8_t starting_player = -1;
bool action_done = false;        // this turn's one action (hire / Misterio / cobro) was used
bool bonus_hire = false;         // Headhunting directo: one extra hire allowed this turn
uint16_t turn_number = 0;
ConsoleState peek_return_state = STATE_TURN;
uint32_t peek_started_ms = 0;
// Starting-player draw animation
constexpr uint32_t ROLL_DURATION_MS = 2400;
constexpr uint32_t ROLL_STEP_MS = 120;
uint32_t roll_started_ms = 0;
uint32_t roll_last_step_ms = 0;
uint8_t roll_highlight = 0;
bool pending_turn_start = false;  // start active_player's turn when the feedback closes
int8_t winner_player = -1;
int8_t last_round_by = -1;       // player who completed 5/5 first (-1: not in last round)
uint8_t final_rank[NUM_PLAYERS]; // player indices, best first (valid in STATE_GAME_OVER)
int16_t pending_trivia_reward = 0;
bool return_forced = false;      // WAIT_RETURN_SCAN because the player couldn't pay (no refund)
bool return_low_ia_only = false; // Auditoria: only an "IA sola" +1 may be returned
int16_t pending_audit_fine = 0;
int8_t swap_my_cand = -1;        // Fuga de talento: candidate the active player gives away

// Feedback screen buffer (ASCII-folded for the 5x7 OLED font)
char fb_title[22] = "";
char fb_line1[22] = "";
char fb_line2[22] = "";
char fb_line3[22] = "";
uint32_t fb_started_ms = 0;

// Company names in game_data.h are plain ASCII, so they can go to the OLED as-is.
#define COMPANY_SHORT(p) (COMPANIES[p].short_name)
#define COMPANY_ASCII(p) (COMPANIES[p].name)

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

ConsoleState resume_state = STATE_START; // where to go after admin / failed code
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

// Maps the 6 temporary cards to real game items (P1 = NotCow):
// [0] Candidate #3  (Yoni Aiv — Diseno de productos, Humano, $200, +3)  [Valid P1]
// [1] Candidate #4  (Ray Eims  — Diseno de productos, H+IA,   $300, +2)  [Replacement P1]
// [2] Candidate #7  (Sergio Larrayn — Fotografia,          H+IA,   $300, +3)  [2nd role P1]
// [3] Candidate #0  (Marie Curri     — Ingenieria mecanica) [NOT in NotCow guide]
// [4] Mystery #1    (Ronda de inversion — +$300)
// [5] Mystery #15   (Dato OIT #1     — Trivia +$100 via COBRAR/BACK)
const CardRef DUMMY_CARD_MAP[DUMMY_CARD_COUNT] = {
  {CARD_CANDIDATE, 3}, {CARD_CANDIDATE, 4}, {CARD_CANDIDATE, 7},
  {CARD_CANDIDATE, 0}, {CARD_MYSTERY, 0}, {CARD_MYSTERY, 14},
};

const char* DUMMY_ENROLL_LABELS[DUMMY_CARD_COUNT] = {
  "1/6: Yoni H $200",
  "2/6: Ray (Reempl)",
  "3/6: Sergio (H+IA)",
  "4/6: Marie (No P1)",
  "5/6: Misterio +$300",
  "6/6: Trivia OIT",
};

const char* DUMMY_ENROLL_SERIAL[DUMMY_CARD_COUNT] = {
  "Card #1 -> Candidate: Yoni Aiv (Diseno de productos, Humano $200, +3) [valid for P1 NotCow]",
  "Card #2 -> Candidate: Ray Eims (Diseno de productos, H+IA $300, +2) [replacement test]",
  "Card #3 -> Candidate: Sergio Larrayn (Fotografia, H+IA $300, +3) [2nd role for P1]",
  "Card #4 -> Candidate: Marie Curri (Ingenieria mecanica) [NOT in NotCow guide -> block test]",
  "Card #5 -> Mystery Card: Ronda de inversion (+$300)",
  "Card #6 -> Mystery Card: Dato OIT #1 (Trivia +$100 via COBRAR=Si / BACK=No)",
};

const char* DUMMY_STEP_PROMPT_OLED[12] = {
  "1: [$] Empezar",
  "2: Escanea #4 Marie",
  "3: Escanea #1 Yoni",
  "4: Escanea #3 Sergio",
  "5: [<-] x3 (NotCow)",
  "6: Presiona COBRAR",
  "7: [<-] x3 (NotCow)",
  "8: Escanea #2 Ray",
  "9: [<-] x3 (NotCow)",
  "10: #6 + COBRAR(Si)",
  "11: [<-]x3 + #5",
  "TEST OK! Modo libre",
};

void printDummyStepPrompt() {
  if (dummy_step >= 11) {
    Serial.println(F("\n=================================================="));
    Serial.println(F("  ALL 11 GUIDED FLOW STEPS PASSED!"));
    Serial.println(F("  Console is now in FREE-PLAY mode with your 6 cards."));
    Serial.println(F("  Keep playing turns, or hire 5 roles to start the last round."));
    Serial.println(F("==================================================\n"));
    return;
  }
  Serial.println(F("\n>>> NEXT GUIDED TEST STEP:"));
  switch (dummy_step) {
    case 0: Serial.println(F("  [STEP 1/11] Press COBRAR on the start screen. Expect: draw animation, NotCow starts (forced in test).")); break;
    case 1: Serial.println(F("  [STEP 2/11] Tap Card #4 (Marie Curri - NOT in NotCow guide). Expect: BLOCKED (action not used).")); break;
    case 2: Serial.println(F("  [STEP 3/11] Tap Card #1 (Yoni Aiv, Humano $200, +3). Expect: -$200 ($300), Level 2 -> 5. Action used.")); break;
    case 3: Serial.println(F("  [STEP 4/11] Tap Card #3 (Sergio Larrayn). Expect: BLOCKED - only one action per turn.")); break;
    case 4: Serial.println(F("  [STEP 5/11] Press ATRAS 3 times (end NotCow, Fintubal, Ziptedi turns). Expect: NotCow's turn again.")); break;
    case 5: Serial.println(F("  [STEP 6/11] Press COBRAR at level 5. Expect: +$500 ($800), no question (hired since last cobro).")); break;
    case 6: Serial.println(F("  [STEP 7/11] Press ATRAS 3 times. Expect: NotCow's turn again.")); break;
    case 7: Serial.println(F("  [STEP 8/11] Tap Card #2 (Ray Eims, SAME role as Yoni). Expect: REPLACED! -$300 ($500), Level 5-3+2 = 4.")); break;
    case 8: Serial.println(F("  [STEP 9/11] Press ATRAS 3 times. Expect: NotCow's turn again.")); break;
    case 9: Serial.println(F("  [STEP 10/11] Tap Card #6 (Dato OIT), then press COBRAR (=Si). Expect: +$100 ($600).")); break;
    case 10: Serial.println(F("  [STEP 11/11] Press ATRAS 3 times, then tap Card #5 (Ronda de inversion). Expect: +$300 ($900).")); break;
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

// Scan cooldown: a card with weak coupling (e.g. through the acrylic) can drop
// out of the RF field and re-enter it, which the RC522 reports as a brand-new
// tap. Any read less than SCAN_COOLDOWN_MS after the previous read is treated
// as the same tap and ignored. Ignored reads also restart the window, so a card
// resting/flickering on the reader never fires twice; it must be lifted away
// for at least SCAN_COOLDOWN_MS before it counts again.
constexpr uint32_t SCAN_COOLDOWN_MS = 1000;
uint32_t last_read_uid = 0;
uint32_t last_read_ms = 0;
bool has_last_read = false;

enum ScanCooldown : uint8_t {
  COOLDOWN_ANY_CARD = 0,  // gameplay: any read within 1 s is ignored
  COOLDOWN_SAME_CARD,     // admin: only the same UID within 1 s is ignored
};

// Returns true (and the UID) when a genuinely new tap happened. Halts the card
// so it is not re-read while it stays in the field.
bool readCard(uint32_t& uid_out, ScanCooldown mode = COOLDOWN_ANY_CARD) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return false;
  uint32_t uid = uidToHash(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  uint32_t now = millis();
  bool within_window = has_last_read && (now - last_read_ms) < SCAN_COOLDOWN_MS;
  bool repeated = within_window && (mode == COOLDOWN_ANY_CARD || uid == last_read_uid);
  last_read_uid = uid;
  last_read_ms = now;
  has_last_read = true;

  if (repeated || uid == 0) {
    Serial.printf("(lectura repetida ignorada: 0x%08lX)\n", static_cast<unsigned long>(uid));
    return false;
  }
  uid_out = uid;
  Serial.printf("UID escaneado: 0x%08lX\n", static_cast<unsigned long>(uid));
  return true;
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

  if (console_state == STATE_START) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(31, 3);
    display.print(F("HEADHUNTERS"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(13, 17); display.print(F("en la era de la IA"));
    // "NotCow  Fintubal" / "Ziptedi" (all three don't fit on one 21-char line)
    display.setCursor(0, 30);
    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      if (p == 2) display.setCursor(0, 40);
      else if (p) display.print(F("  "));
      display.print(COMPANY_ASCII(p));
    }
    display.printf("  $%d Nv%u", STARTING_MONEY, STARTING_PROFITABILITY);
    display.drawLine(0, 52, 127, 52, SH110X_WHITE);
    display.setCursor(0, 55);
#ifdef DUMMY_FLOW_TEST
    display.print(DUMMY_STEP_PROMPT_OLED[dummy_step]);
#else
    display.print(F("[$] Empezar partida"));
#endif
  } else if (console_state == STATE_ROLL_STARTER) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("QUIEN EMPIEZA?"));
    display.setTextColor(SH110X_WHITE);
    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      int y = 18 + p * 14;
      if (p == roll_highlight) {
        display.fillRect(0, y - 2, 128, 12, SH110X_WHITE);
        display.setTextColor(SH110X_BLACK);
      }
      display.setCursor(30, y);
      display.print(COMPANY_ASCII(p));
      display.setTextColor(SH110X_WHITE);
    }
  } else if (console_state == STATE_TURN && active_player >= 0) {
    const PlayerState& ps = players[active_player];
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("TURNO: "));
    display.print(COMPANY_ASCII(active_player));
    display.setTextColor(SH110X_WHITE);
    if (!action_done) {
      display.setCursor(0, 16); display.print(F("1 Lanza dados y mueve"));
      display.setCursor(0, 26); display.print(F("2 Escanea o [$]Cobra"));
    } else if (bonus_hire) {
      display.setCursor(0, 16); display.print(F("Headhunting: escanea"));
      display.setCursor(0, 26); display.print(F("1 cargo de tu guia"));
    } else {
      display.setCursor(0, 16); display.print(F("Accion lista."));
      display.setCursor(0, 26); display.print(F("Termina tu turno."));
    }
    display.setCursor(0, 36);
    display.printf("$%d Nv%u Cargos %u/%u", ps.money, ps.profitability,
                   countHiredRoles(active_player), ROLES_PER_COMPANY);
    display.setCursor(0, 45);
    if (last_round_by >= 0) display.print(F("** ULTIMA RONDA **"));
    else if (ps.next_hire_discount > 0) display.printf("Descuento: -$%d", ps.next_hire_discount);
    display.drawLine(0, 53, 127, 53, SH110X_WHITE);
    display.setCursor(0, 56);
#ifdef DUMMY_FLOW_TEST
    display.print(DUMMY_STEP_PROMPT_OLED[dummy_step]);
#else
    display.print(F("[<-] Terminar turno"));
#endif
  } else if (console_state == STATE_PEEK_SCORES) {
    display.fillRect(0, 0, 128, 12, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 2);
    display.print(last_round_by >= 0 ? F("PUNTAJES  ULT.RONDA") : F("PUNTAJES  Nv Cg  $"));
    display.setTextColor(SH110X_WHITE);

    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      int y = 16 + p * 12;
      display.setCursor(0, y);
      display.print(COMPANY_SHORT(p));
      display.drawRect(15, y, 40, 8, SH110X_WHITE);
      uint8_t clamped = min<uint8_t>(players[p].profitability, MAX_PROFITABILITY);
      int fill_w = (clamped * 36) / MAX_PROFITABILITY;
      if (fill_w > 0) display.fillRect(17, y + 2, fill_w, 4, SH110X_WHITE);
      display.setCursor(58, y);
      display.printf("%2u", players[p].profitability);
      display.setCursor(73, y);
      display.printf("%u/%u", countHiredRoles(p), ROLES_PER_COMPANY);
      display.setCursor(98, y);
      display.printf("$%d", players[p].money);
    }

    display.drawLine(0, 52, 127, 52, SH110X_WHITE);
    display.setCursor(0, 55);
    if (active_player >= 0) {
      display.print(F("Turno de "));
      display.print(COMPANY_ASCII(active_player));
    }
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
#else
    display.setCursor(80, 56); display.print(F("[<-] ok"));
#endif
  } else if (console_state == STATE_WAIT_TRIVIA_CONFIRM) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.printf("DATO OIT (+$%d)", TRIVIA_REWARD);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 20); display.print(F("Acerto la respuesta?"));
    display.setCursor(0, 36); display.print(F("[$] COBRAR = SI"));
    display.setCursor(0, 48); display.print(F("[<-] ATRAS  = NO"));
  } else if (console_state == STATE_WAIT_PAYOUT_CONFIRM && active_player >= 0) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("COBRO RECEPCION"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 17); display.print(F("Entraste a una sala"));
    display.setCursor(0, 27); display.print(F("desde tu ult. cobro?"));
    display.setCursor(0, 41);
    display.printf("[$] SI  +$%d", players[active_player].profitability * RECEPTION_PAYOUT_PER_LEVEL);
    display.setCursor(0, 53); display.print(F("[<-] NO"));
  } else if (console_state == STATE_WAIT_RETURN_SCAN) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(return_forced ? F("NO TE ALCANZA") : F("REESTRUCTURACION"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 20); display.print(F("Escanea la tarjeta"));
    display.setCursor(0, 32);
    display.print(return_forced ? F("que devuelves (sin $)") : F("a devolver (+50% $)"));
    if (!return_forced) { display.setCursor(0, 52); display.print(F("[<-] Cancelar")); }
  } else if (console_state == STATE_WAIT_AUDIT_CHOICE) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("AUDITORIA"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 17); display.print(F("Tienes una IA sola +1"));
    display.setCursor(0, 31); display.printf("[$] Pagar $%d", pending_audit_fine);
    display.setCursor(0, 43); display.print(F("[<-] Devolver la IA"));
    display.setCursor(0, 53); display.print(F("     (sin reembolso)"));
  } else if (console_state == STATE_WAIT_SWAP_MINE || console_state == STATE_WAIT_SWAP_THEIRS) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("FUGA DE TALENTO"));
    display.setTextColor(SH110X_WHITE);
    if (console_state == STATE_WAIT_SWAP_MINE) {
      display.setCursor(0, 18); display.print(F("1) Escanea TU tarjeta"));
      display.setCursor(0, 30); display.print(F("a intercambiar."));
    } else {
      char who[22];
      asciiFold(CANDIDATES[swap_my_cand].name, who, sizeof(who));
      display.setCursor(0, 17); display.print(who);
      display.setCursor(0, 28); display.print(F("2) Escanea la del"));
      display.setCursor(0, 38); display.print(F("rival, mismo cargo."));
    }
    display.setCursor(0, 53); display.print(F("[<-] Cancelar"));
  } else if (console_state == STATE_WAIT_UPGRADE_SCAN) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("CAPACITACION"));
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 17); display.print(F("Escanea tu Humano"));
    display.setCursor(0, 27); display.print(F("(o su Humano+IA)."));
    display.setCursor(0, 37); display.print(F("Pagas $100."));
    display.setCursor(0, 53); display.print(F("[<-] No usar"));
  } else if (console_state == STATE_GAME_OVER && winner_player >= 0) {
    display.fillRect(0, 0, 128, 13, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(3, 3);
    display.print(F("GANA "));
    display.print(COMPANY_ASCII(winner_player));
    display.print('!');
    display.setTextColor(SH110X_WHITE);
    for (uint8_t r = 0; r < NUM_PLAYERS; ++r) {
      uint8_t p = final_rank[r];
      display.setCursor(0, 17 + r * 11);
      display.printf("%u.%-8s Nv%2u $%d", r + 1, COMPANY_ASCII(p), players[p].profitability,
                     players[p].money);
    }
    display.setCursor(0, 54); display.print(F("[<-] Nueva partida"));
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
    players[p].next_hire_discount = 0;
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
  last_round_by = -1;
  pending_trivia_reward = 0;
  return_forced = false;
  return_low_ia_only = false;
  pending_audit_fine = 0;
  swap_my_cand = -1;
  starting_player = -1;
  pending_turn_start = false;
  action_done = false;
  bonus_hire = false;
  turn_number = 0;
  console_state = STATE_START;
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
    Serial.printf("  %-8s | Nivel: %2u | Plata: $%d | Cargos: %u/%u | Visito sala: %s\n",
                  COMPANY_ASCII(p),
                  players[p].profitability,
                  players[p].money,
                  countHiredRoles(p), ROLES_PER_COMPANY,
                  players[p].eligible_for_payout ? "SI" : "?");
  }
  if (last_round_by >= 0) {
    Serial.printf("  ** ULTIMA RONDA (la inicio %s): cada otro jugador juega un turno mas. **\n",
                  COMPANY_ASCII(last_round_by));
  }
}

// Called after a hire: the first player to complete all roles starts the last round.
bool maybeStartLastRound(uint8_t player_idx) {
  if (last_round_by >= 0 || countHiredRoles(player_idx) < ROLES_PER_COMPANY) return false;
  last_round_by = static_cast<int8_t>(player_idx);
  Serial.printf("%s completo sus %u cargos -> ULTIMA RONDA para todos.\n",
                COMPANY_ASCII(player_idx), ROLES_PER_COMPANY);
  return true;
}

// Highest profitability wins; ties broken by money.
void endGame() {
  for (uint8_t i = 0; i < NUM_PLAYERS; ++i) final_rank[i] = i;
  for (uint8_t i = 1; i < NUM_PLAYERS; ++i) {
    for (uint8_t j = i; j > 0; --j) {
      const PlayerState& a = players[final_rank[j]];
      const PlayerState& b = players[final_rank[j - 1]];
      bool better = a.profitability > b.profitability ||
                    (a.profitability == b.profitability && a.money > b.money);
      if (!better) break;
      uint8_t t = final_rank[j]; final_rank[j] = final_rank[j - 1]; final_rank[j - 1] = t;
    }
  }
  winner_player = static_cast<int8_t>(final_rank[0]);
  active_player = -1;
  console_state = STATE_GAME_OVER;
  Serial.println(F("=== FIN DE LA PARTIDA ==="));
  for (uint8_t r = 0; r < NUM_PLAYERS; ++r) {
    uint8_t p = final_rank[r];
    Serial.printf("  %u. %s | Nivel %u | $%d\n", r + 1, COMPANY_ASCII(p),
                  players[p].profitability, players[p].money);
  }
  const PlayerState& w = players[final_rank[0]];
  const PlayerState& s2 = players[final_rank[1]];
  if (w.profitability == s2.profitability && w.money == s2.money) {
    Serial.println(F("  (Empate exacto en nivel y dinero.)"));
  }
  beepWin();
  renderScreen();
}

// ---------------------------------------------------------------------------
// Turns
// ---------------------------------------------------------------------------
void startTurn(uint8_t player_idx) {
  active_player = static_cast<int8_t>(player_idx);
  action_done = false;
  bonus_hire = false;
  turn_number++;
  console_state = STATE_TURN;
  Serial.printf("\n=== Turno %u: %s — lanza los dados y mueve ===\n", turn_number,
                COMPANY_ASCII(player_idx));
  beepOk();
  renderScreen();
#ifdef DUMMY_FLOW_TEST
  if (player_idx == 0 && (dummy_step == 0 || dummy_step == 4 || dummy_step == 6 || dummy_step == 8)) {
    advanceDummyStep(dummy_step);
  }
#endif
}

// [<-] on the turn screen. After someone completed 5/5, the game ends when the
// turn would come back to them (everyone else played one last turn).
void endTurn() {
  if (active_player < 0) return;
  uint8_t next = static_cast<uint8_t>((active_player + 1) % NUM_PLAYERS);
  Serial.printf("Fin del turno de %s.\n", COMPANY_ASCII(active_player));
  if (last_round_by >= 0 && next == static_cast<uint8_t>(last_round_by)) {
    endGame();
    return;
  }
  active_player = static_cast<int8_t>(next);
  pending_turn_start = true;
  char l2[22];
  snprintf(l2, sizeof(l2), "%s!", COMPANY_ASCII(next));
  beepTick();
  showFeedback("SIGUIENTE TURNO", "Ahora juega:", l2,
               last_round_by >= 0 ? "Ultima ronda!" : "Lanza los dados.", STATE_TURN);
  // startTurn() runs when the feedback screen closes (see loop()).
}

void beginStarterDraw() {
  console_state = STATE_ROLL_STARTER;
  roll_started_ms = roll_last_step_ms = millis();
#ifdef DUMMY_FLOW_TEST
  starting_player = 0;  // deterministic for the guided test
#else
  starting_player = static_cast<int8_t>(esp_random() % NUM_PLAYERS);
#endif
  roll_highlight = 0;
  Serial.println(F("Sorteando quien empieza..."));
  renderScreen();
}

void rollStarterTick() {
  uint32_t now = millis();
  if (now - roll_started_ms >= ROLL_DURATION_MS) {
    roll_highlight = static_cast<uint8_t>(starting_player);
    renderScreen();
    beepWin();
    char l2[22], l3[22];
    snprintf(l2, sizeof(l2), "%s!", COMPANY_ASCII(starting_player));
    snprintf(l3, sizeof(l3), "Orden: %s>%s>%s", COMPANY_SHORT(starting_player),
             COMPANY_SHORT((starting_player + 1) % NUM_PLAYERS),
             COMPANY_SHORT((starting_player + 2) % NUM_PLAYERS));
    Serial.printf("Empieza %s. %s\n", COMPANY_ASCII(starting_player), l3);
    active_player = starting_player;
    turn_number = 0;
    pending_turn_start = true;
    showFeedback("EMPIEZA", l2, l3, "Lanza los dados.", STATE_TURN);
    return;
  }
  // Slow down towards the end, like a spinning wheel.
  uint32_t step = ROLL_STEP_MS + (now - roll_started_ms) / 8;
  if (now - roll_last_step_ms >= step) {
    roll_last_step_ms = now;
    roll_highlight = (roll_highlight + 1) % NUM_PLAYERS;
    beepTick();
    renderScreen();
  }
}

// The turn's single action is gone (or a Misterio card is required to finish first).
bool actionBlocked(const char* what) {
  if (!action_done) return false;
  Serial.printf("BLOQUEADO: ya hiciste tu accion este turno (%s).\n", what);
  beepErr();
  showFeedback("YA JUGASTE", "Solo 1 accion por", "turno. Termina con", "[<-]", STATE_TURN);
  return true;
}

// Charge `amount`. If the player can't pay, they must hand back one hired card
// with no refund instead (or lose what they have if they hired nobody).
// Returns true if the money was paid; false if a return scan is now pending.
bool chargeOrForceReturn(uint8_t player_idx, int16_t amount) {
  PlayerState& ps = players[player_idx];
  if (amount <= 0 || ps.money >= amount) {
    ps.money -= max<int16_t>(0, amount);
    return true;
  }
  if (countHiredRoles(player_idx) == 0) {
    ps.money = 0;
    return true;
  }
  Serial.printf("%s no puede pagar $%d (tiene $%d): devuelve una tarjeta sin reembolso.\n",
                COMPANY_ASCII(player_idx), amount, ps.money);
  return_forced = true;
  console_state = STATE_WAIT_RETURN_SCAN;
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
    showFeedback("BLOQUEADO!", "Cargo fuera de tu", "guia de startup.", COMPANY_ASCII(player_idx),
                 STATE_TURN);
#ifdef DUMMY_FLOW_TEST
    if (player_idx == 0 && cand_idx == 0) advanceDummyStep(1);
#endif
    return;
  }

  if (candidate_taken[cand_idx]) {
    Serial.println(F("BLOQUEADO: Postulante ya contratado."));
    beepErr();
    showFeedback("NO DISPONIBLE", cand.name, "Ya fue contratado.", "", STATE_TURN);
    return;
  }

  int8_t prev_idx = ps.hired_candidate_by_slot[slot];
  if (prev_idx == static_cast<int8_t>(cand_idx)) {
    beepErr();
    showFeedback("YA ES TUYO", cand.name, "Ya trabaja en tu", "startup.", STATE_TURN);
    return;
  }

  // Replacing a role costs the full price of the new card (old one goes back, no refund).
  bool is_replacement = (prev_idx >= 0);

  int16_t final_cost = cand.cost;
  if (ps.next_hire_discount > 0) {
    final_cost = max<int16_t>(0, final_cost - ps.next_hire_discount);
  }

  if (ps.money < final_cost) {
    Serial.println(F("BLOQUEADO: Dinero insuficiente."));
    beepErr();
    char buf[32];
    snprintf(buf, sizeof(buf), "Costo:$%d Hay:$%d", final_cost, ps.money);
    showFeedback("SIN DINERO!", cand.name, buf, "", STATE_TURN);
    return;
  }

  // Only consume the discount once the hire actually goes through.
  if (ps.next_hire_discount > 0) ps.next_hire_discount = 0;
  bool via_headhunting = bonus_hire;
  bonus_hire = false;
  action_done = true;

  if (is_replacement) {
    candidate_taken[prev_idx] = false;
    ps.profitability -= CANDIDATES[prev_idx].profitability;
  }

  ps.money -= final_cost;
  ps.profitability += cand.profitability;
  ps.hired_candidate_by_slot[slot] = static_cast<int8_t>(cand_idx);
  if (!via_headhunting) ps.eligible_for_payout = true;  // hiring means you entered a room
  candidate_taken[cand_idx] = true;
  bool last_round_started = maybeStartLastRound(player_idx);

  char name_ascii[32];
  asciiFold(cand.name, name_ascii, sizeof(name_ascii));
  Serial.printf("%s %s %s (%s, $%d) -> Nivel: %u | Cargos: %u/%u | Saldo: $%d\n",
                COMPANY_ASCII(player_idx),
                is_replacement ? "reemplazo por" : "contrato a",
                name_ascii, modalityLabel(cand.modality), final_cost,
                ps.profitability, countHiredRoles(player_idx), ROLES_PER_COMPANY, ps.money);
  printLeaderboard();

  char l2[32], l3[32];
  snprintf(l2, sizeof(l2), "%s -$%d", modalityLabel(cand.modality), final_cost);
  if (last_round_started) {
    beepWin();
    snprintf(l3, sizeof(l3), "+%u Nv:%u ULT.RONDA!", cand.profitability, ps.profitability);
    showFeedback("5/5 CARGOS!", cand.name, l2, l3, STATE_TURN);
  } else {
    beepOk();
    snprintf(l3, sizeof(l3), "+%u rentab. (Nv:%u)", cand.profitability, ps.profitability);
    showFeedback(is_replacement ? "REEMPLAZADO!" : "CONTRATADO!", cand.name, l2, l3,
                 STATE_TURN);
  }

#ifdef DUMMY_FLOW_TEST
  if (player_idx == 0 && cand_idx == 3) advanceDummyStep(2);
  if (player_idx == 0 && cand_idx == 4 && is_replacement) advanceDummyStep(7);
#endif
}

void handleCandidateReturn(uint8_t player_idx, uint8_t cand_idx) {
  PlayerState& ps = players[player_idx];
  const CandidateDef& cand = CANDIDATES[cand_idx];
  int8_t slot = findRequiredSlot(player_idx, cand.occupation_id);
  if (slot < 0 || ps.hired_candidate_by_slot[slot] != static_cast<int8_t>(cand_idx)) {
    Serial.println(F("Ese postulante no pertenece a tu equipo."));
    beepErr();
    showFeedback("ERROR", "No es tu postulante.", "Escanea uno tuyo o",
                 return_forced ? "" : "presiona Cancelar.", STATE_WAIT_RETURN_SCAN);
    return;
  }
  if (return_low_ia_only && !(cand.modality == MOD_IA_SOLA && cand.profitability == 1)) {
    beepErr();
    showFeedback("ERROR", "Debes devolver tu", "IA sola de +1.", "", STATE_WAIT_RETURN_SCAN);
    return;
  }

  ps.hired_candidate_by_slot[slot] = -1;
  candidate_taken[cand_idx] = false;
  ps.profitability = max<int>(0, ps.profitability - cand.profitability);
  int16_t refund = return_forced ? 0 : cand.cost / 2;
  ps.money += refund;
  return_forced = false;
  return_low_ia_only = false;

  Serial.printf("Postulante devuelto (%s). Reembolso: +$%d | Nivel: %u\n", cand.name, refund, ps.profitability);
  beepOk();
  printLeaderboard();

  char l2[32], l3[32];
  if (refund > 0) snprintf(l2, sizeof(l2), "Reembolso: +$%d", refund);
  else            snprintf(l2, sizeof(l2), "Sin reembolso");
  snprintf(l3, sizeof(l3), "Nv:%u Saldo:$%d", ps.profitability, ps.money);
  showFeedback("DEVUELTO", cand.name, l2, l3, STATE_TURN);
}

// Occupation card (Insight): pay to see the role's OIT score and best modality.
void handleInsightScan(uint8_t player_idx, uint8_t occ_idx) {
  PlayerState& ps = players[player_idx];
  const OccupationDef& occ = OCCUPATIONS[occ_idx];
  if (ps.money < INSIGHT_COST) {
    Serial.println(F("Dinero insuficiente para el insight."));
    beepErr();
    char l1[22];
    snprintf(l1, sizeof(l1), "Insight cuesta $%d.", INSIGHT_COST);
    showFeedback("SIN DINERO!", l1, "", "", STATE_TURN);
    return;
  }
  ps.money -= INSIGHT_COST;

  char cargo[32];
  asciiFold(occ.cargo, cargo, sizeof(cargo));
  Serial.printf("INSIGHT (-$%d): %s | OIT %.2f (%s) | Rinde mas: %s | Saldo: $%d\n",
                INSIGHT_COST, cargo, occ.oit_score, occ.oit_level,
                modalityLabel(occ.ideal_modality), ps.money);
  beepOk();
  printLeaderboard();

  char title[22], l2[32], l3[32];
  snprintf(title, sizeof(title), "INSIGHT -$%d", INSIGHT_COST);
  snprintf(l2, sizeof(l2), "OIT %.2f %s", occ.oit_score, occ.oit_level);
  if (strlen(l2) > 21) snprintf(l2, sizeof(l2), "OIT %.2f", occ.oit_score);
  snprintf(l3, sizeof(l3), "Rinde mas: %s", modalityLabel(occ.ideal_modality));
  showFeedback(title, occ.cargo, l2, l3, STATE_TURN);
}

// Owner (player index) of a hired candidate, or -1.
int8_t candidateOwner(uint8_t cand_idx) {
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
      if (players[p].hired_candidate_by_slot[s] == static_cast<int8_t>(cand_idx)) return p;
    }
  }
  return -1;
}

// Fuga de talento is possible if another player hired someone for a role you also hired.
bool swapPossible(uint8_t player_idx) {
  for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
    int8_t mine = players[player_idx].hired_candidate_by_slot[s];
    if (mine < 0) continue;
    uint8_t occ = CANDIDATES[mine].occupation_id;
    for (uint8_t q = 0; q < NUM_PLAYERS; ++q) {
      if (q == player_idx) continue;
      int8_t qs = findRequiredSlot(q, occ);
      if (qs >= 0 && players[q].hired_candidate_by_slot[qs] >= 0) return true;
    }
  }
  return false;
}

void handleSwapScan(uint8_t player_idx, uint8_t cand_idx) {
  const CandidateDef& cand = CANDIDATES[cand_idx];
  if (swap_my_cand < 0) {
    if (candidateOwner(cand_idx) != static_cast<int8_t>(player_idx)) {
      beepErr();
      showFeedback("ERROR", "No es tu postulante.", "Escanea uno tuyo.", "", STATE_WAIT_SWAP_MINE);
      return;
    }
    swap_my_cand = static_cast<int8_t>(cand_idx);
    beepTick();
    console_state = STATE_WAIT_SWAP_THEIRS;
    renderScreen();
    return;
  }
  const CandidateDef& mine = CANDIDATES[swap_my_cand];
  int8_t owner = candidateOwner(cand_idx);
  if (owner < 0 || owner == static_cast<int8_t>(player_idx) ||
      cand.occupation_id != mine.occupation_id) {
    beepErr();
    showFeedback("ERROR", "Debe ser de otro", "jugador y del mismo", "cargo que el tuyo.",
                 STATE_WAIT_SWAP_THEIRS);
    return;
  }
  PlayerState& me = players[player_idx];
  PlayerState& them = players[owner];
  int8_t my_slot = findRequiredSlot(player_idx, mine.occupation_id);
  int8_t their_slot = findRequiredSlot(owner, cand.occupation_id);
  me.hired_candidate_by_slot[my_slot] = static_cast<int8_t>(cand_idx);
  them.hired_candidate_by_slot[their_slot] = swap_my_cand;
  me.profitability = me.profitability - mine.profitability + cand.profitability;
  them.profitability = them.profitability - cand.profitability + mine.profitability;

  char a[22], b[22];
  asciiFold(mine.name, a, sizeof(a));
  asciiFold(cand.name, b, sizeof(b));
  Serial.printf("Fuga de talento: %s (%s) <-> %s (%s)\n", a, COMPANY_ASCII(player_idx), b,
                COMPANY_ASCII(owner));
  swap_my_cand = -1;
  beepOk();
  printLeaderboard();
  char l3[32];
  snprintf(l3, sizeof(l3), "Nv: tu %u / %s %u", me.profitability, COMPANY_SHORT(owner),
           them.profitability);
  showFeedback("INTERCAMBIO!", a, b, l3, STATE_TURN);
}

void payReception(uint8_t player_idx);

// Index of the Humano + IA card for an occupation, or -1.
int8_t hybridFor(uint8_t occupation_id) {
  for (uint8_t c = 0; c < NUM_CANDIDATES; ++c) {
    if (CANDIDATES[c].occupation_id == occupation_id && CANDIDATES[c].modality == MOD_HUMANO_IA) return c;
  }
  return -1;
}

// Capacitacion interna: a hired Humano whose Humano + IA is still free.
bool upgradePossible(uint8_t player_idx) {
  for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
    int8_t ci = players[player_idx].hired_candidate_by_slot[s];
    if (ci < 0 || CANDIDATES[ci].modality != MOD_HUMANO) continue;
    int8_t h = hybridFor(CANDIDATES[ci].occupation_id);
    if (h >= 0 && !candidate_taken[h]) return true;
  }
  return false;
}

// Accepts either card of the pair: the Humano you hired, or the free Humano + IA
// of that same cargo (players naturally scan their own Humano first).
void handleUpgradeScan(uint8_t player_idx, uint8_t scanned_idx) {
  PlayerState& ps = players[player_idx];
  const CandidateDef& scanned = CANDIDATES[scanned_idx];
  int8_t slot = findRequiredSlot(player_idx, scanned.occupation_id);
  int8_t prev = slot >= 0 ? ps.hired_candidate_by_slot[slot] : -1;
  int8_t hyb = hybridFor(scanned.occupation_id);
  const char* why = nullptr;
  if (scanned.modality == MOD_IA_SOLA) {
    why = "Es una IA sola.";
  } else if (prev < 0 || CANDIDATES[prev].modality != MOD_HUMANO) {
    why = "No tienes a su Humano";
  } else if (hyb < 0 || candidate_taken[hyb]) {
    why = "Su Hum+IA esta usado";
  }
  if (why) {
    beepErr();
    char a[22];
    asciiFold(scanned.name, a, sizeof(a));
    Serial.printf("Capacitacion: %s no sirve (%s).\n", a, why);
    showFeedback("NO SIRVE", scanned.name, why, "Escanea otro o [<-]", STATE_WAIT_UPGRADE_SCAN);
    return;
  }
  uint8_t cand_idx = static_cast<uint8_t>(hyb);
  const CandidateDef& cand = CANDIDATES[cand_idx];
  constexpr int16_t UPGRADE_COST = 100;
  ps.money -= UPGRADE_COST;
  candidate_taken[prev] = false;
  candidate_taken[cand_idx] = true;
  ps.profitability = ps.profitability - CANDIDATES[prev].profitability + cand.profitability;
  ps.hired_candidate_by_slot[slot] = static_cast<int8_t>(cand_idx);
  char a[22], b[22];
  asciiFold(CANDIDATES[prev].name, a, sizeof(a));
  asciiFold(cand.name, b, sizeof(b));
  Serial.printf("Capacitacion: %s -> %s (-$%d) | Nivel %u | Saldo $%d\n", a, b, UPGRADE_COST,
                ps.profitability, ps.money);
  beepOk();
  printLeaderboard();
  char l3[32];
  snprintf(l3, sizeof(l3), "-$%d  Nivel: %u", UPGRADE_COST, ps.profitability);
  showFeedback("CAPACITADO!", a, b, l3, STATE_TURN);
}

void handleMysteryScan(uint8_t player_idx, uint8_t card_idx) {
  PlayerState& ps = players[player_idx];
  const MysteryCardDef& card = MYSTERY_CARDS[card_idx];
  char l2[32] = "", l3[32] = "";

  Serial.printf("MISTERIO (#%u): %s\n", card.id, card.title);
  action_done = true;  // drawing a Misterio card is this turn's action

  switch (card.effect) {
    case MYS_ADD_MONEY:
      ps.money += card.value;
      snprintf(l2, sizeof(l2), "+$%d al banco", card.value);
      snprintf(l3, sizeof(l3), "Saldo: $%d", ps.money);
      beepOk();
      showFeedback("MISTERIO", card.title, l2, l3, STATE_TURN);
#ifdef DUMMY_FLOW_TEST
      if (player_idx == 0 && card_idx == 0) advanceDummyStep(10);
#endif
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
      showFeedback("MISTERIO", card.title, l2, l3, STATE_TURN);
      break;
    }
    case MYS_PENALTY_PER_MODALITY: {
      int count = 0;
      for (uint8_t s = 0; s < ROLES_PER_COMPANY; ++s) {
        int8_t ci = ps.hired_candidate_by_slot[s];
        if (ci >= 0 && CANDIDATES[ci].modality == card.target_modality) count++;
      }
      int16_t delta = count * card.value;
      beepErr();
      if (!chargeOrForceReturn(player_idx, delta)) {
        snprintf(l2, sizeof(l2), "Multa $%d: no", delta);
        showFeedback("MISTERIO", card.title, l2, "alcanza. Devuelve 1", STATE_WAIT_RETURN_SCAN);
        return;
      }
      snprintf(l2, sizeof(l2), "Multa: -$%d", delta);
      snprintf(l3, sizeof(l3), "Saldo: $%d", ps.money);
      showFeedback("MISTERIO", card.title, l2, l3, STATE_TURN);
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
        // Pay the fine or hand back the IA +1 (no refund). Can't pay -> must return it.
        beepErr();
        return_low_ia_only = true;
        if (ps.money < card.value) {
          return_forced = true;
          snprintf(l2, sizeof(l2), "Multa $%d: no", card.value);
          showFeedback("AUDITORIA", l2, "alcanza. Devuelve", "tu IA sola +1.", STATE_WAIT_RETURN_SCAN);
          return;
        }
        pending_audit_fine = card.value;
        console_state = STATE_WAIT_AUDIT_CHOICE;
        Serial.println(F("Auditoria: COBRAR = pagar la multa / ATRAS = devolver la IA sola +1."));
        renderScreen();
        return;
      } else {
        snprintf(l2, sizeof(l2), "Sin IA +1: $0 multa");
        beepOk();
      }
      snprintf(l3, sizeof(l3), "Saldo: $%d", ps.money);
      showFeedback("AUDITORIA", card.title, l2, l3, STATE_TURN);
      break;
    }
    case MYS_FORCE_RECEPTION_NO_PAY:
      ps.eligible_for_payout = false;
      beepErr();
      showFeedback("CAIDA SERVIDOR", "Vuelve a Recepcion", "SIN cobrar trimestre", "",
                   STATE_TURN);
      break;
    case MYS_ENABLE_PAYOUT_RECEPTION:
      // "Ve directo a la Recepcion y cobra tu trimestre": pay it right away.
      Serial.println(F("Pitch exitoso: ve a la Recepcion, cobras tu trimestre ahora."));
      payReception(player_idx);
      return;
    case MYS_SWAP_CANDIDATE:
      if (!swapPossible(player_idx)) {
        beepOk();
        showFeedback("FUGA DE TALENTO", "Nadie tiene un cargo", "igual al tuyo:", "no pasa nada.",
                     STATE_TURN);
        break;
      }
      swap_my_cand = -1;
      console_state = STATE_WAIT_SWAP_MINE;
      Serial.println(F("Fuga de talento: escanea tu tarjeta y luego la del rival (mismo cargo)."));
      beepOk();
      renderScreen();
      return;
    case MYS_NEXT_HIRE_DISCOUNT:
      ps.next_hire_discount += card.value;
      beepOk();
      showFeedback("DESCUENTO", "Proxima contratacion", "cuesta -$100!", "",
                   STATE_TURN);
      break;
    case MYS_UPGRADE_HUMAN_TO_HYBRID:
      if (ps.money < 100) {
        beepOk();
        showFeedback("CAPACITACION", "Cuesta $100 y no", "te alcanza:", "no pasa nada.", STATE_TURN);
        break;
      }
      if (!upgradePossible(player_idx)) {
        beepOk();
        showFeedback("CAPACITACION", COMPANY_ASCII(player_idx), "no tiene un Humano", "con Hum+IA libre.",
                     STATE_TURN);
        break;
      }
      console_state = STATE_WAIT_UPGRADE_SCAN;
      Serial.println(F("Capacitacion: escanea tu Humano (o su Humano + IA). Pagas $100."));
      beepOk();
      renderScreen();
      return;
    case MYS_DIRECT_HIRE:
      bonus_hire = true;
      beepOk();
      showFeedback("HEADHUNTING!", "Contrata ahora en", "cualquier sala de tu", "guia: escanea.",
                   STATE_TURN);
      break;
    case MYS_RETURN_CANDIDATE_HALF:
      if (countHiredRoles(player_idx) == 0) {
        beepOk();
        showFeedback("REESTRUCTURACION", "No tienes a nadie", "contratado: no pasa", "nada.",
                     STATE_TURN);
        break;
      }
      return_forced = false;
      console_state = STATE_WAIT_RETURN_SCAN;
      Serial.println(F("Escanea el postulante que deseas devolver (o presiona Cancelar)."));
      beepOk();
      renderScreen();
      return;
    case MYS_TRIVIA_CONFIRM:
      pending_trivia_reward = card.value;
      Serial.printf("%s (respuesta al reverso de la carta)\n", card.title);
      console_state = STATE_WAIT_TRIVIA_CONFIRM;
      Serial.println(F("Dato OIT: el jugador de la derecha pregunta. COBRAR si acerto, ATRAS si fallo."));
      beepOk();
      renderScreen();
      return;
    case MYS_BOARD_INFO_ONLY:
    default:
      beepOk();
      showFeedback("MISTERIO", card.title, "Aplica en el tablero", "", STATE_TURN);
      break;
  }
  printLeaderboard();
}

// Recepcion: $100 per profitability level, only if the player entered a room
// since their last payout.
void payReception(uint8_t player_idx) {
  PlayerState& ps = players[player_idx];
  int16_t payout = ps.profitability * RECEPTION_PAYOUT_PER_LEVEL;
  ps.money += payout;
  ps.eligible_for_payout = false;
  action_done = true;

  Serial.printf("%s cobro +$%d (%u x $%d) -> Saldo: $%d\n",
                COMPANY_ASCII(player_idx), payout, ps.profitability,
                RECEPTION_PAYOUT_PER_LEVEL, ps.money);
  beepOk();
  printLeaderboard();

  char l1[32], l2[32];
  snprintf(l1, sizeof(l1), "+$%d (%u x $%d)", payout, ps.profitability, RECEPTION_PAYOUT_PER_LEVEL);
  snprintf(l2, sizeof(l2), "Saldo total: $%d", ps.money);
  showFeedback("COBRO RECEPCION", l1, l2, "", STATE_TURN);

#ifdef DUMMY_FLOW_TEST
  if (player_idx == 0 && dummy_step == 5) advanceDummyStep(5);
#endif
}

void handleCobrar(uint8_t player_idx) {
  if (players[player_idx].eligible_for_payout) {
    payReception(player_idx);
    return;
  }
  // The console can't see the board: a hire proves a room visit, otherwise ask.
  Serial.println(F("Cobro: entraste a una sala desde tu ultimo cobro? COBRAR=Si / ATRAS=No"));
  beepTick();
  console_state = STATE_WAIT_PAYOUT_CONFIRM;
  renderScreen();
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
      char all_line[22];
      snprintf(all_line, sizeof(all_line), "%u/%u asignadas", NUM_CARDS, NUM_CARDS);
      adminMessage("TODO LISTO", "No faltan tarjetas.", all_line, ADM_MENU, 2000);
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
  // Same-card cooldown only, so you can swap quickly to the next card while enrolling.
  if (adm_screen == ADM_WALK || adm_screen == ADM_TEST) scanned = readCard(uid, COOLDOWN_SAME_CARD);

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
    } else if (strcmp(buf, "oled") == 0) {
      Serial.printf("Orientacion OLED: %u (%s). Usa: oled 0|1|2\n", oled_orientation,
                    oledOrientationLabel(oled_orientation));
    } else if (strncmp(buf, "oled ", 5) == 0 && buf[5] >= '0' && buf[5] <= '2' && buf[6] == '\0') {
      uint8_t mode = static_cast<uint8_t>(buf[5] - '0');
      applyOledOrientation(mode);
      saveOledOrientation(mode);
      Serial.printf("Orientacion OLED -> %u (%s), guardada en flash.\n", mode,
                    oledOrientationLabel(mode));
      renderScreen();
    } else if (strcmp(buf, "help") == 0) {
      Serial.println(F("Comandos: dump (lista UIDs en flash), admin (abre modo admin),"));
      Serial.println(F("          oled 0|1|2 (pantalla: 0 normal, 1 volteada vertical, 2 girada 180)"));
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
  if (oled_ok) {
    applyOledOrientation(loadOledOrientation());
    Serial.printf("Orientacion OLED: %u (%s) — cambiar con 'oled 0|1|2'\n", oled_orientation,
                  oledOrientationLabel(oled_orientation));
  }

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();
  rfid.PCD_AntennaOn();
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522 VersionReg: 0x%02X\n", ver);

  RegistrySource src = registryBegin();
  uint8_t assigned = registryCount();
  Serial.printf("Tarjetas NFC: %u/%u asignadas (fuente: %s)\n", assigned, NUM_CARDS,
                src == REG_SOURCE_FLASH ? "memoria flash"
                : src == REG_SOURCE_COMPILED_IMPORTED ? "game_data.h nuevo (copiado a flash)"
                : "game_data.h");

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
    showFeedback("FALTAN TARJETAS", l1, "Asignalas en el", "modo admin.", STATE_START);
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

  // Feedback screen: closes after a timeout, or early with [<-] (cancel). The tap
  // that closes it does nothing else (so a double-tap can't end the turn). Other
  // buttons are ignored while a message is up, so nobody skips it by accident.
  if (console_state == STATE_SHOW_FEEDBACK) {
    bool skip = btnTapped(BTN_BACK);
    bool other_btn = btnPressed(BTN_P1) || btnPressed(BTN_P2) || btnPressed(BTN_P3) ||
                     btnPressed(BTN_COBRAR);
    if (skip || millis() - fb_started_ms >= FEEDBACK_DURATION_MS) {
      console_state = state_after_feedback;
      if (pending_turn_start && console_state == STATE_TURN) {
        pending_turn_start = false;
        startTurn(static_cast<uint8_t>(active_player));
      } else {
        renderScreen();
      }
      if (skip) return;
    } else if (other_btn) {
      return;
    }
  }

  if (console_state == STATE_PEEK_SCORES) {
    bool any_btn = btnPressed(BTN_P1) || btnPressed(BTN_P2) || btnPressed(BTN_P3) ||
                   btnPressed(BTN_COBRAR) || btnTapped(BTN_BACK);
    if (any_btn || millis() - peek_started_ms >= PEEK_DURATION_MS) {
      console_state = peek_return_state;
      renderScreen();
    }
    return;
  }

  if (console_state == STATE_ROLL_STARTER) {
    rollStarterTick();
    return;
  }

  if (console_state == STATE_GAME_OVER) {
    if (btnTapped(BTN_BACK)) {
      resetGame();
      beepOk();
      renderScreen();
    }
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

  if (console_state == STATE_START) {
    uint32_t uid_hash;
    if (btnPressed(BTN_COBRAR)) {
      beepOk();
      beginStarterDraw();
    } else if (readCard(uid_hash)) {
      beepErr();
      showFeedback("AUN NO", "Primero empieza la", "partida con [$].", "", STATE_START);
    }
    return;
  }

  // From here on a game is running and active_player is the turn's player.
  if (active_player < 0) return;
  const uint8_t ap = static_cast<uint8_t>(active_player);

  // A forced return (couldn't pay) or the audit choice must be resolved first,
  // even while its feedback screen is still showing.
  ConsoleState effective_state =
      console_state == STATE_SHOW_FEEDBACK ? state_after_feedback : console_state;
  bool locked = return_forced || effective_state == STATE_WAIT_AUDIT_CHOICE;

  // P1-P3: peek at the scoreboard during a turn.
  if (console_state == STATE_TURN) {
    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      if (btnPressed(p)) {
        peek_return_state = STATE_TURN;
        peek_started_ms = millis();
        console_state = STATE_PEEK_SCORES;
        printLeaderboard();
        beepTick();
        renderScreen();
        return;
      }
    }
  }

  // BACK / CANCEL / END TURN (acts on release)
  if (btnTapped(BTN_BACK)) {
    if (console_state == STATE_WAIT_TRIVIA_CONFIRM) {
      pending_trivia_reward = 0;
      Serial.println(F("Trivia fallida: $0."));
      beepErr();
      showFeedback("DATO OIT", "Respuesta incorrecta", "Cobras $0.", "", STATE_TURN);
    } else if (console_state == STATE_WAIT_PAYOUT_CONFIRM) {
      Serial.println(F("Sin cobro: primero entra a una sala."));
      beepErr();
      showFeedback("SIN COBRO", "Entra a una sala", "antes de volver a", "Recepcion.", STATE_TURN);
    } else if (console_state == STATE_WAIT_AUDIT_CHOICE) {
      return_forced = true;  // return_low_ia_only is already set
      pending_audit_fine = 0;
      Serial.println(F("Auditoria: escanea tu IA sola +1 para devolverla."));
      beepTick();
      console_state = STATE_WAIT_RETURN_SCAN;
      renderScreen();
    } else if (console_state == STATE_WAIT_SWAP_MINE || console_state == STATE_WAIT_SWAP_THEIRS) {
      swap_my_cand = -1;
      console_state = STATE_TURN;
      Serial.println(F("Intercambio cancelado."));
      beepOk();
      renderScreen();
    } else if (console_state == STATE_WAIT_UPGRADE_SCAN) {
      console_state = STATE_TURN;
      Serial.println(F("Capacitacion no usada."));
      beepOk();
      renderScreen();
    } else if (locked) {
      beepErr();  // must hand back a card first
    } else if (console_state == STATE_WAIT_RETURN_SCAN) {
      console_state = STATE_TURN;
      Serial.println(F("Devolucion cancelada."));
      beepOk();
      renderScreen();
    } else if (console_state == STATE_TURN) {
      endTurn();
    }
  }

  // COBRAR / CONFIRM button
  if (btnPressed(BTN_COBRAR)) {
    if (console_state == STATE_WAIT_AUDIT_CHOICE) {
      PlayerState& ps = players[ap];
      ps.money -= pending_audit_fine;
      Serial.printf("Auditoria: multa -$%d -> Saldo: $%d\n", pending_audit_fine, ps.money);
      char l1[22], l2[22];
      snprintf(l1, sizeof(l1), "Multa IA +1: -$%d", pending_audit_fine);
      snprintf(l2, sizeof(l2), "Saldo: $%d", ps.money);
      pending_audit_fine = 0;
      return_low_ia_only = false;
      beepOk();
      printLeaderboard();
      showFeedback("AUDITORIA", l1, l2, "", STATE_TURN);
    } else if (console_state == STATE_WAIT_TRIVIA_CONFIRM) {
      players[ap].money += pending_trivia_reward;
      int16_t reward = pending_trivia_reward;
      pending_trivia_reward = 0;
      Serial.printf("Trivia correcta! +$%d -> Saldo: $%d\n", reward, players[ap].money);
      beepOk();
      printLeaderboard();
      char l1[22], l2[22];
      snprintf(l1, sizeof(l1), "+$%d Dato OIT", reward);
      snprintf(l2, sizeof(l2), "Saldo: $%d", players[ap].money);
      showFeedback("CORRECTO!", l1, l2, "", STATE_TURN);
#ifdef DUMMY_FLOW_TEST
      if (dummy_step == 9) advanceDummyStep(9);
#endif
    } else if (console_state == STATE_WAIT_PAYOUT_CONFIRM) {
      payReception(ap);
    } else if (console_state == STATE_TURN) {
      if (!actionBlocked("cobro")) handleCobrar(ap);
    } else {
      beepErr();  // finish the current prompt first (scan a card or [<-])
    }
  }

  // NFC / RFID scan
  uint32_t uid_hash;
  if (readCard(uid_hash)) {
    CardRef card;
    bool known = resolveCard(uid_hash, card);

    // A card tapped while "Ahora juega: X" is showing belongs to X's new turn.
    if (pending_turn_start && console_state == STATE_SHOW_FEEDBACK) {
      pending_turn_start = false;
      startTurn(ap);
      effective_state = STATE_TURN;
    }

    if (effective_state == STATE_WAIT_SWAP_MINE || effective_state == STATE_WAIT_SWAP_THEIRS) {
      if (known && card.kind == CARD_CANDIDATE) handleSwapScan(ap, card.index);
      else beepErr();
    } else if (effective_state == STATE_WAIT_UPGRADE_SCAN) {
      if (known && card.kind == CARD_CANDIDATE) handleUpgradeScan(ap, card.index);
      else beepErr();
    } else if (effective_state == STATE_WAIT_RETURN_SCAN) {
      if (known && card.kind == CARD_CANDIDATE) {
        handleCandidateReturn(ap, card.index);
      } else {
        Serial.println(F("Escanea la tarjeta del postulante a devolver."));
        beepErr();
      }
    } else if (effective_state != STATE_TURN) {
      beepErr();  // answer the question on screen first (COBRAR = Si / ATRAS = No)
    } else if (!known) {
      Serial.println(F("Tag no registrado. Asignalo en el modo admin."));
      beepErr();
      showFeedback("TAG DESCONOCIDO", "Tarjeta sin asignar.", "Registrala en el", "modo admin.",
                   STATE_TURN);
    } else if (card.kind == CARD_CANDIDATE) {
      if (bonus_hire || !actionBlocked("contratar")) {
        handleCandidateHireOrReplace(ap, card.index);
      }
#ifdef DUMMY_FLOW_TEST
      if (action_done && card.index == 7 && dummy_step == 3) advanceDummyStep(3);
#endif
    } else if (card.kind == CARD_OCCUPATION) {
      handleInsightScan(ap, card.index);  // free extra, not the turn's action
    } else {
      if (!actionBlocked("misterio")) handleMysteryScan(ap, card.index);
    }
  }
}
