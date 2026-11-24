#pragma once

#include <stddef.h>
#include <stdint.h>

#include "game_data.h"

// ============================================================================
// Card registry: which physical NFC tag (UID) means which game card.
//
// The 50 cards are addressed by a "slot" 0..49, in the same order and with the
// same codes as scripts/enroll_cards.py and webapp/:
//   slots  0..23 -> C01..C24  Postulantes            (CANDIDATES[0..23])
//   slots 24..31 -> I01..I08  Ocupaciones (Insight)  (OCCUPATIONS[0..7])
//   slots 32..49 -> M01..M18  Misterio               (MYSTERY_CARDS[0..17])
//
// The mapping lives in ESP32 flash (NVS, namespace "hh_cards") and is edited
// from the board itself in admin mode. Flashing new firmware does NOT erase it.
// Exception: if the firmware is built with a NEW set of UIDs in game_data.h
// (e.g. imported from the phone web app via scripts/import_uids.py), that set
// replaces flash on the first boot. Later admin-mode edits are kept again.
// ============================================================================

constexpr uint8_t NUM_CARDS = NUM_CANDIDATES + NUM_OCCUPATIONS + NUM_MYSTERY_CARDS;

enum CardKind : uint8_t {
  CARD_CANDIDATE = 0,
  CARD_OCCUPATION,
  CARD_MYSTERY,
};

struct CardRef {
  CardKind kind;
  uint8_t index;  // index into CANDIDATES / OCCUPATIONS / MYSTERY_CARDS
};

enum RegistrySource : uint8_t {
  REG_SOURCE_FLASH = 0,
  REG_SOURCE_COMPILED,           // game_data.h (flash empty, nothing to import)
  REG_SOURCE_COMPILED_IMPORTED,  // new UIDs in game_data.h, copied over flash
};

CardRef cardRef(uint8_t slot);

// Loads the mapping from flash (or game_data.h if flash is empty).
RegistrySource registryBegin();

uint32_t registryUid(uint8_t slot);       // 0 if unassigned
int16_t registryFind(uint32_t uid);       // slot, or -1 if unknown
uint8_t registryCount();                  // number of assigned slots

// All writers persist to flash immediately and return false on a flash error.
// registryAssign() also un-assigns any other slot that had the same UID.
bool registryAssign(uint8_t slot, uint32_t uid);
bool registryClear(uint8_t slot);
bool registryClearAll();

// Display helpers (plain ASCII, ready for the OLED font).
void cardCode(uint8_t slot, char* out, size_t n);  // "C05", "I02", "M17"
const char* cardKindLabel(CardKind kind);           // "POSTULANTE", ...
const char* modalityLabel(Modality m);              // "Humano", "Hum+IA", "IA sola"
// Up to 3 lines (each n bytes, n <= 22 recommended) describing the card.
void cardDescribe(uint8_t slot, char* l1, char* l2, char* l3, size_t n);
