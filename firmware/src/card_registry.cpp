#include "card_registry.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "text_util.h"

namespace {

constexpr const char* NVS_NAMESPACE = "hh_cards";
constexpr const char* NVS_KEY_UIDS = "uids";
constexpr const char* NVS_KEY_COUNT = "n";

constexpr uint8_t SLOT_FIRST_OCCUPATION = NUM_CANDIDATES;
constexpr uint8_t SLOT_FIRST_MYSTERY = NUM_CANDIDATES + NUM_OCCUPATIONS;

uint32_t uids[NUM_CARDS];

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
  }
  return "";
}

}  // namespace

CardRef cardRef(uint8_t slot) {
  if (slot < SLOT_FIRST_OCCUPATION) return {CARD_CANDIDATE, slot};
  if (slot < SLOT_FIRST_MYSTERY) return {CARD_OCCUPATION, static_cast<uint8_t>(slot - SLOT_FIRST_OCCUPATION)};
  return {CARD_MYSTERY, static_cast<uint8_t>(slot - SLOT_FIRST_MYSTERY)};
}

RegistrySource registryBegin() {
  Preferences prefs;
  bool loaded = false;
  if (prefs.begin(NVS_NAMESPACE, true)) {  // read-only; fails if namespace absent
    if (prefs.getUChar(NVS_KEY_COUNT, 0) == NUM_CARDS &&
        prefs.getBytesLength(NVS_KEY_UIDS) == sizeof(uids)) {
      loaded = prefs.getBytes(NVS_KEY_UIDS, uids, sizeof(uids)) == sizeof(uids);
    }
    prefs.end();
  }
  if (loaded) return REG_SOURCE_FLASH;

  for (uint8_t s = 0; s < NUM_CARDS; ++s) uids[s] = compiledUid(s);
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
    case CARD_OCCUPATION: return "INSIGHT";
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
      asciiFold(OCCUPATIONS[c.occupation_id].room, l2, n);
      snprintf(l3, n, "%s $%d +%u", modalityLabel(c.modality), c.cost, c.profitability);
      break;
    }
    case CARD_OCCUPATION: {
      const OccupationDef& o = OCCUPATIONS[r.index];
      asciiFold(o.room, l1, n);
      asciiFold(o.role, l2, n);
      snprintf(l3, n, "Tarjeta Insight $%d", INSIGHT_COST);
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
