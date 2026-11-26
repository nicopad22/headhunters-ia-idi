#include "card_registry.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "text_util.h"

namespace {

constexpr const char* NVS_NAMESPACE = "hh_cards";
constexpr const char* NVS_KEY_UIDS = "uids";
constexpr const char* NVS_KEY_COUNT = "n";
constexpr const char* NVS_KEY_SOURCE = "src";  // fingerprint of the compiled table flash was based on

constexpr uint8_t SLOT_FIRST_OCCUPATION = NUM_CANDIDATES;
constexpr uint8_t SLOT_FIRST_MYSTERY = NUM_CANDIDATES + NUM_OCCUPATIONS;

uint32_t uids[NUM_CARDS];
uint32_t compiled_fingerprint = 0;

uint32_t compiledUid(uint8_t slot) {
  CardRef r = cardRef(slot);
  switch (r.kind) {
    case CARD_CANDIDATE:  return CANDIDATES[r.index].nfc_uid_hash;
    case CARD_OCCUPATION: return OCCUPATIONS[r.index].nfc_uid_hash;
    case CARD_MYSTERY:    return MYSTERY_CARDS[r.index].nfc_uid_hash;
  }
  return 0;
}

bool save() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  size_t written = prefs.putBytes(NVS_KEY_UIDS, uids, sizeof(uids));
  prefs.putUChar(NVS_KEY_COUNT, NUM_CARDS);
  prefs.putULong(NVS_KEY_SOURCE, compiled_fingerprint);
  prefs.end();
  return written == sizeof(uids);
}

const char* mysteryEffectLabel(MysteryEffect e) {
  switch (e) {
    case MYS_ADD_MONEY:               return "Suma dinero";
    case MYS_BONUS_PER_MODALITY:      return "Bono por modalidad";
    case MYS_PENALTY_PER_MODALITY:    return "Multa por modalidad";
    case MYS_PENALTY_LOW_IA:          return "Auditoria IA +1";
    case MYS_FORCE_RECEPTION_NO_PAY:  return "Recepcion sin cobro";
    case MYS_ENABLE_PAYOUT_RECEPTION: return "Habilita cobro";
    case MYS_BOARD_INFO_ONLY:         return "Jugada en tablero";
    case MYS_NEXT_HIRE_DISCOUNT:      return "Descuento -$100";
    case MYS_UPGRADE_HUMAN_TO_HYBRID: return "Mejora Humano";
    case MYS_RETURN_CANDIDATE_HALF:   return "Devuelve postulante";
    case MYS_TRIVIA_CONFIRM:          return "Trivia Si/No";
    case MYS_SWAP_CANDIDATE:          return "Intercambio";
    case MYS_DIRECT_HIRE:             return "Contrata a distancia";
  }
  return "";
}

}  // namespace

CardRef cardRef(uint8_t slot) {
  if (slot < SLOT_FIRST_OCCUPATION) return {CARD_CANDIDATE, slot};
  if (slot < SLOT_FIRST_MYSTERY) return {CARD_OCCUPATION, static_cast<uint8_t>(slot - SLOT_FIRST_OCCUPATION)};
  return {CARD_MYSTERY, static_cast<uint8_t>(slot - SLOT_FIRST_MYSTERY)};
}

// Precedence:
//  1. game_data.h has UIDs that flash has not seen yet (new import + reflash)
//     -> use them and copy them to flash.
//  2. otherwise, flash has a saved table (admin-mode edits) -> use flash.
//  3. otherwise -> use game_data.h as-is (possibly all empty).
RegistrySource registryBegin() {
  uint32_t compiled[NUM_CARDS];
  bool compiled_any = false;
  uint32_t fp = 2166136261u;  // FNV-1a over the compiled table
  for (uint8_t s = 0; s < NUM_CARDS; ++s) {
    compiled[s] = compiledUid(s);
    compiled_any |= compiled[s] != 0;
    for (uint8_t b = 0; b < 4; ++b) {
      fp ^= (compiled[s] >> (8 * b)) & 0xFF;
      fp *= 16777619u;
    }
  }
  compiled_fingerprint = fp;

  Preferences prefs;
  bool loaded = false;
  uint32_t stored_fp = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {  // read-only; fails if namespace absent
    if (prefs.getUChar(NVS_KEY_COUNT, 0) == NUM_CARDS &&
        prefs.getBytesLength(NVS_KEY_UIDS) == sizeof(uids)) {
      loaded = prefs.getBytes(NVS_KEY_UIDS, uids, sizeof(uids)) == sizeof(uids);
      stored_fp = prefs.getULong(NVS_KEY_SOURCE, 0);
    }
    prefs.end();
  }

  bool compiled_is_new = compiled_any && (!loaded || stored_fp != compiled_fingerprint);
  if (loaded && !compiled_is_new) return REG_SOURCE_FLASH;

  memcpy(uids, compiled, sizeof(uids));
  if (compiled_is_new) {
    save();
    return REG_SOURCE_COMPILED_IMPORTED;
  }
  return REG_SOURCE_COMPILED;
}

uint32_t registryUid(uint8_t slot) {
  return slot < NUM_CARDS ? uids[slot] : 0;
}

int16_t registryFind(uint32_t uid) {
  if (uid == 0) return -1;
  for (uint8_t s = 0; s < NUM_CARDS; ++s) {
    if (uids[s] == uid) return s;
  }
  return -1;
}

uint8_t registryCount() {
  uint8_t n = 0;
  for (uint8_t s = 0; s < NUM_CARDS; ++s) {
    if (uids[s] != 0) n++;
  }
  return n;
}

bool registryAssign(uint8_t slot, uint32_t uid) {
  if (slot >= NUM_CARDS || uid == 0) return false;
  for (uint8_t s = 0; s < NUM_CARDS; ++s) {
    if (s != slot && uids[s] == uid) uids[s] = 0;
  }
  uids[slot] = uid;
  return save();
}

bool registryClear(uint8_t slot) {
  if (slot >= NUM_CARDS) return false;
  uids[slot] = 0;
  return save();
}

bool registryClearAll() {
  memset(uids, 0, sizeof(uids));
  return save();
}

void cardCode(uint8_t slot, char* out, size_t n) {
  CardRef r = cardRef(slot);
  const char prefix = r.kind == CARD_CANDIDATE ? 'C' : r.kind == CARD_OCCUPATION ? 'I' : 'M';
  snprintf(out, n, "%c%02u", prefix, r.index + 1);
}

const char* cardKindLabel(CardKind kind) {
  switch (kind) {
    case CARD_CANDIDATE:  return "POSTULANTE";
    case CARD_OCCUPATION: return "OCUPACION";
    case CARD_MYSTERY:    return "MISTERIO";
  }
  return "";
}

const char* modalityLabel(Modality m) {
  switch (m) {
    case MOD_HUMANO:    return "Humano";
    case MOD_HUMANO_IA: return "Hum+IA";
    case MOD_IA_SOLA:   return "IA sola";
  }
  return "";
}

void cardDescribe(uint8_t slot, char* l1, char* l2, char* l3, size_t n) {
  l1[0] = l2[0] = l3[0] = '\0';
  CardRef r = cardRef(slot);
  switch (r.kind) {
    case CARD_CANDIDATE: {
      const CandidateDef& c = CANDIDATES[r.index];
      asciiFold(c.name, l1, n);
      asciiFold(OCCUPATIONS[c.occupation_id].cargo, l2, n);
      snprintf(l3, n, "%s $%d +%u", modalityLabel(c.modality), c.cost, c.profitability);
      break;
    }
    case CARD_OCCUPATION: {
      const OccupationDef& o = OCCUPATIONS[r.index];
      char raw[40];
      asciiFold(o.cargo, l1, n);
      snprintf(raw, sizeof(raw), "Sala %c %s", o.room_code, o.room);
      asciiFold(raw, l2, n);
      snprintf(l3, n, "Insight $%d", INSIGHT_COST);
      break;
    }
    case CARD_MYSTERY: {
      const MysteryCardDef& m = MYSTERY_CARDS[r.index];
      char raw[64], folded[64];
      snprintf(raw, sizeof(raw), "#%02u %s", m.id, m.title);
      asciiFold(raw, folded, sizeof(folded));
      wrapTwoLines(folded, l1, l2, n - 1);
      snprintf(l3, n, "%s", mysteryEffectLabel(m.effect));
      break;
    }
  }
}
