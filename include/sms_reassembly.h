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
 * Inbound multi-segment SMS reassembly with bounded state.
 *
 * Callers push one UDH-carrying fragment at a time. When a message is
 * complete, the helper concatenates the fragments into a caller-provided
 * buffer and frees the slot. State is inline (no heap) so a long-running
 * daemon doesn't fragment; bounded by 8 slots, 2 slots per sender, and a
 * 10-minute TTL so a spammer can't pin resources.
 *
 * Thread-safety: all callers run on the single URC handler thread (CMTI
 * handler is dispatched on the main thread via the command queue, but the
 * reassembly itself happens there and only there). No mutex.
 */

#ifndef SMS_REASSEMBLY_H
#define SMS_REASSEMBLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "pdu.h"

#define REASSEMBLY_SLOTS 8
#define REASSEMBLY_TIMEOUT_SEC 600
#define REASSEMBLY_PER_SENDER 2
/* UCS2 decoded fragment can reach 67 chars × 4 UTF-8 bytes = 268; round up
 * for NUL and replacement characters. */
#define REASSEMBLY_FRAG_BUF_SIZE 320

typedef enum {
   REASM_INCOMPLETE = 0, /* fragment stored, waiting for more */
   REASM_COMPLETE,       /* message whole, body written to out */
   REASM_REJECTED_CAP,   /* per-sender cap tripped */
   REASM_REJECTED_TOTAL, /* mismatched total across fragments */
   REASM_REJECTED_DUP,   /* duplicate seq ignored */
   REASM_ERROR,          /* bad args or fragment copy overflow */
} reasm_result_t;

typedef struct {
   uint32_t slots_in_use;
   uint64_t total_completed;
   uint64_t total_timed_out;
   uint64_t total_dropped_exhaustion;
   uint64_t total_duplicates;
   uint64_t total_sender_cap_exceeded;
} sms_reassembly_stats_t;

/**
 * @brief Push one fragment. Call on every inbound segment with UDH.
 *
 * @param sender     Normalized sender (E.164 or short code).
 * @param ref_id     UDH reference id.
 * @param total      UDH total segments.
 * @param seq        UDH sequence number (1-based).
 * @param fragment   UTF-8 body for this segment.
 * @param frag_len   Bytes in fragment (not including NUL).
 * @param now        Current wall time (injection for testability).
 * @param out_body   Optional output buffer for assembled body (REASM_COMPLETE).
 * @param out_cap    Size of out_body.
 * @param out_len    Optional: bytes written (not including NUL) on COMPLETE.
 * @return reasm_result_t classification.
 */
reasm_result_t sms_reassembly_push(const char *sender,
                                   uint8_t ref_id,
                                   uint8_t total,
                                   uint8_t seq,
                                   const char *fragment,
                                   size_t frag_len,
                                   time_t now,
                                   char *out_body,
                                   size_t out_cap,
                                   size_t *out_len);

/**
 * @brief Evict timed-out slots. Safe to call periodically or on push.
 *
 * @param now  Current wall time.
 * @return Slots evicted.
 */
int sms_reassembly_sweep(time_t now);

/**
 * @brief Snapshot cumulative counters.
 */
void sms_reassembly_stats(sms_reassembly_stats_t *out);

/**
 * @brief Reset all state. Intended for tests.
 */
void sms_reassembly_reset(void);

#endif /* SMS_REASSEMBLY_H */
