/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Bounded reassembly store for multi-segment inbound SMS.
 */

#include "sms_reassembly.h"

#include <string.h>

#include "echo.h"
#include "logging.h"

/* ── State ───────────────────────────────────────────────────────────── */

typedef struct {
   bool in_use;
   /* PDU_SENDER_MAX (24) rather than PHONE_NUMBER_MAX+1 (21): pdu_decode()
    * produces up to '+' + 20 digits + NUL = 22 bytes, so a 21-byte field
    * would truncate on max-length E.164 senders. Truncation would then make
    * later fragments' sender key miss the slot, wasting slots and sometimes
    * dropping reassembly entirely. Keep this in sync with pdu.h. */
   char sender[PDU_SENDER_MAX];
   uint8_t ref_id;
   uint8_t total;
   uint16_t received_mask; /* bit (seq-1) set when that fragment has arrived */
   char fragments[PDU_MAX_SEGMENTS][REASSEMBLY_FRAG_BUF_SIZE];
   size_t frag_len[PDU_MAX_SEGMENTS];
   time_t first_seen;
} reasm_slot_t;

static reasm_slot_t g_slots[REASSEMBLY_SLOTS];
static sms_reassembly_stats_t g_stats;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void clear_slot(reasm_slot_t *slot) {
   memset(slot, 0, sizeof(*slot));
}

static reasm_slot_t *find_slot(const char *sender, uint8_t ref_id) {
   for (int i = 0; i < REASSEMBLY_SLOTS; i++) {
      if (g_slots[i].in_use && g_slots[i].ref_id == ref_id &&
          strcmp(g_slots[i].sender, sender) == 0) {
         return &g_slots[i];
      }
   }
   return NULL;
}

static int count_slots_for_sender(const char *sender) {
   int n = 0;
   for (int i = 0; i < REASSEMBLY_SLOTS; i++) {
      if (g_slots[i].in_use && strcmp(g_slots[i].sender, sender) == 0) {
         n++;
      }
   }
   return n;
}

static int popcount_u16(uint16_t x) {
   int n = 0;
   while (x) {
      n += (int)(x & 1u);
      x >>= 1;
   }
   return n;
}

static reasm_slot_t *claim_slot(time_t now) {
   /* Free slot first. */
   for (int i = 0; i < REASSEMBLY_SLOTS; i++) {
      if (!g_slots[i].in_use) {
         clear_slot(&g_slots[i]);
         g_slots[i].in_use = true;
         g_slots[i].first_seen = now;
         return &g_slots[i];
      }
   }
   /* All slots busy — evict the slot with the fewest received fragments,
    * tie-break by oldest first_seen. Naive LRU-by-first_seen would let an
    * attacker pushing fresh fragments under new ref_ids evict honest
    * users' mid-message slots. Preferring low-fragment-count slots keeps
    * the victim-friendly bias. */
   int best = 0;
   int best_frags = popcount_u16(g_slots[0].received_mask);
   for (int i = 1; i < REASSEMBLY_SLOTS; i++) {
      int frags = popcount_u16(g_slots[i].received_mask);
      if (frags < best_frags ||
          (frags == best_frags && g_slots[i].first_seen < g_slots[best].first_seen)) {
         best = i;
         best_frags = frags;
      }
   }
   OLOG_WARNING("SMS reassembly exhausted — evicting slot for sender=%s ref=%u seq_mask=0x%x",
                g_slots[best].sender, g_slots[best].ref_id, g_slots[best].received_mask);
   g_stats.total_dropped_exhaustion++;
   clear_slot(&g_slots[best]);
   g_slots[best].in_use = true;
   g_slots[best].first_seen = now;
   return &g_slots[best];
}

static void update_in_use_count(void) {
   uint32_t n = 0;
   for (int i = 0; i < REASSEMBLY_SLOTS; i++) {
      if (g_slots[i].in_use) {
         n++;
      }
   }
   g_stats.slots_in_use = n;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int sms_reassembly_sweep(time_t now) {
   int evicted = 0;
   for (int i = 0; i < REASSEMBLY_SLOTS; i++) {
      if (g_slots[i].in_use && (now - g_slots[i].first_seen) > REASSEMBLY_TIMEOUT_SEC) {
         OLOG_WARNING("SMS reassembly timeout — sender=%s ref=%u seq_mask=0x%x age=%lds",
                      g_slots[i].sender, g_slots[i].ref_id, g_slots[i].received_mask,
                      (long)(now - g_slots[i].first_seen));
         clear_slot(&g_slots[i]);
         g_stats.total_timed_out++;
         evicted++;
      }
   }
   update_in_use_count();
   return evicted;
}

reasm_result_t sms_reassembly_push(const char *sender,
                                   uint8_t ref_id,
                                   uint8_t total,
                                   uint8_t seq,
                                   const char *fragment,
                                   size_t frag_len,
                                   time_t now,
                                   char *out_body,
                                   size_t out_cap,
                                   size_t *out_len) {
   if (!sender || !fragment) {
      return REASM_ERROR;
   }
   if (total == 0 || total > PDU_MAX_SEGMENTS) {
      return REASM_REJECTED_TOTAL;
   }
   if (seq == 0 || seq > total) {
      return REASM_REJECTED_TOTAL;
   }
   if (frag_len >= REASSEMBLY_FRAG_BUF_SIZE) {
      /* Decoded UTF-8 won't fit — bail before trashing slot state. */
      return REASM_ERROR;
   }

   /* Sweep stale slots on every push — cheap (8 slots) and keeps the table
    * self-cleaning without a separate scheduler. */
   sms_reassembly_sweep(now);

   reasm_slot_t *slot = find_slot(sender, ref_id);
   if (slot) {
      if (slot->total != total) {
         /* Spec violation or spoofed segment — reject the new fragment
          * without corrupting the existing slot. */
         OLOG_WARNING("SMS reassembly total mismatch — sender=%s ref=%u stored=%u incoming=%u",
                      sender, ref_id, slot->total, total);
         return REASM_REJECTED_TOTAL;
      }
      uint16_t bit = (uint16_t)(1u << (seq - 1));
      if (slot->received_mask & bit) {
         g_stats.total_duplicates++;
         return REASM_REJECTED_DUP;
      }
      memcpy(slot->fragments[seq - 1], fragment, frag_len);
      slot->fragments[seq - 1][frag_len] = '\0';
      slot->frag_len[seq - 1] = frag_len;
      slot->received_mask |= bit;
   } else {
      if (count_slots_for_sender(sender) >= REASSEMBLY_PER_SENDER) {
         g_stats.total_sender_cap_exceeded++;
         OLOG_WARNING("SMS reassembly per-sender cap hit for %s (cap=%d)", sender,
                      REASSEMBLY_PER_SENDER);
         return REASM_REJECTED_CAP;
      }
      slot = claim_slot(now);
      snprintf(slot->sender, sizeof(slot->sender), "%s", sender);
      slot->ref_id = ref_id;
      slot->total = total;
      slot->received_mask = (uint16_t)(1u << (seq - 1));
      memcpy(slot->fragments[seq - 1], fragment, frag_len);
      slot->fragments[seq - 1][frag_len] = '\0';
      slot->frag_len[seq - 1] = frag_len;
      update_in_use_count();
   }

   /* Complete when all (1..total) bits set. */
   uint16_t full = (total == 16) ? 0xFFFFu : (uint16_t)((1u << total) - 1u);
   if ((slot->received_mask & full) != full) {
      return REASM_INCOMPLETE;
   }

   /* Concatenate in sequence order. Bail out clean if the output buffer
    * can't hold the result — caller decides how to handle. */
   size_t total_len = 0;
   for (int i = 0; i < total; i++) {
      total_len += slot->frag_len[i];
   }
   if (!out_body || out_cap == 0 || total_len + 1 > out_cap) {
      OLOG_WARNING("SMS reassembly output buffer too small (need=%zu cap=%zu)", total_len + 1,
                   out_cap);
      clear_slot(slot);
      update_in_use_count();
      return REASM_ERROR;
   }

   size_t pos = 0;
   for (int i = 0; i < total; i++) {
      memcpy(out_body + pos, slot->fragments[i], slot->frag_len[i]);
      pos += slot->frag_len[i];
   }
   out_body[pos] = '\0';
   if (out_len) {
      *out_len = pos;
   }

   clear_slot(slot);
   update_in_use_count();
   g_stats.total_completed++;
   return REASM_COMPLETE;
}

void sms_reassembly_stats(sms_reassembly_stats_t *out) {
   if (!out) {
      return;
   }
   *out = g_stats;
}

void sms_reassembly_reset(void) {
   memset(g_slots, 0, sizeof(g_slots));
   memset(&g_stats, 0, sizeof(g_stats));
}
