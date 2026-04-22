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
 * Unit tests for the SMS reassembly store.
 */

#include <string.h>

#include "sms_reassembly.h"
#include "unity.h"

void setUp(void) {
   sms_reassembly_reset();
}
void tearDown(void) {
}

/* ── Happy path ──────────────────────────────────────────────────────── */

void test_complete_in_order(void) {
   char out[256];
   size_t out_len = 0;
   reasm_result_t r;

   r = sms_reassembly_push("+15551234567", 10, 3, 1, "AAA", 3, 1000, out, sizeof(out), &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_INCOMPLETE, r);
   r = sms_reassembly_push("+15551234567", 10, 3, 2, "BBB", 3, 1000, out, sizeof(out), &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_INCOMPLETE, r);
   r = sms_reassembly_push("+15551234567", 10, 3, 3, "CCC", 3, 1000, out, sizeof(out), &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_COMPLETE, r);
   TEST_ASSERT_EQUAL_STRING("AAABBBCCC", out);
   TEST_ASSERT_EQUAL_INT(9, out_len);
}

void test_complete_out_of_order(void) {
   char out[256];
   size_t out_len = 0;
   /* Arrive 3, 1, 2 — should still concatenate in seq order. */
   TEST_ASSERT_EQUAL_INT(REASM_INCOMPLETE, sms_reassembly_push("+15551234567", 20, 3, 3, "CCC", 3,
                                                               1000, out, sizeof(out), &out_len));
   TEST_ASSERT_EQUAL_INT(REASM_INCOMPLETE, sms_reassembly_push("+15551234567", 20, 3, 1, "AAA", 3,
                                                               1000, out, sizeof(out), &out_len));
   TEST_ASSERT_EQUAL_INT(REASM_COMPLETE, sms_reassembly_push("+15551234567", 20, 3, 2, "BBB", 3,
                                                             1000, out, sizeof(out), &out_len));
   TEST_ASSERT_EQUAL_STRING("AAABBBCCC", out);
}

/* ── Duplicate / mismatch / cap rejection ────────────────────────────── */

void test_duplicate_rejected(void) {
   char out[64];
   size_t out_len = 0;
   sms_reassembly_push("+1555", 30, 2, 1, "AA", 2, 1000, out, sizeof(out), &out_len);
   reasm_result_t r = sms_reassembly_push("+1555", 30, 2, 1, "AA", 2, 1000, out, sizeof(out),
                                          &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_REJECTED_DUP, r);

   sms_reassembly_stats_t s;
   sms_reassembly_stats(&s);
   TEST_ASSERT_EQUAL_UINT64(1, s.total_duplicates);
}

void test_total_mismatch_rejected(void) {
   char out[64];
   size_t out_len = 0;
   sms_reassembly_push("+1555", 40, 3, 1, "AA", 2, 1000, out, sizeof(out), &out_len);
   /* Same sender/ref, different total → reject without corrupting slot. */
   reasm_result_t r = sms_reassembly_push("+1555", 40, 5, 2, "BB", 2, 1000, out, sizeof(out),
                                          &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_REJECTED_TOTAL, r);
}

void test_sender_cap(void) {
   char out[64];
   size_t out_len = 0;
   /* Two concurrent messages from same sender, different ref_ids. */
   sms_reassembly_push("+1555", 1, 2, 1, "A", 1, 1000, out, sizeof(out), &out_len);
   sms_reassembly_push("+1555", 2, 2, 1, "B", 1, 1000, out, sizeof(out), &out_len);
   /* Third must be rejected. */
   reasm_result_t r = sms_reassembly_push("+1555", 3, 2, 1, "C", 1, 1000, out, sizeof(out),
                                          &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_REJECTED_CAP, r);

   sms_reassembly_stats_t s;
   sms_reassembly_stats(&s);
   TEST_ASSERT_EQUAL_UINT64(1, s.total_sender_cap_exceeded);
}

/* ── Eviction / timeout ──────────────────────────────────────────────── */

void test_lru_eviction_when_full(void) {
   char out[64];
   size_t out_len = 0;
   /* Fill all 8 slots with 4 different senders × 2 ref_ids each (per-sender
    * cap is 2). Each slot gets exactly 1 fragment so none complete. */
   sms_reassembly_push("+sender1", 1, 2, 1, "A", 1, 1000, out, sizeof(out), &out_len);
   sms_reassembly_push("+sender1", 2, 2, 1, "A", 1, 1001, out, sizeof(out), &out_len);
   sms_reassembly_push("+sender2", 1, 2, 1, "A", 1, 1002, out, sizeof(out), &out_len);
   sms_reassembly_push("+sender2", 2, 2, 1, "A", 1, 1003, out, sizeof(out), &out_len);
   sms_reassembly_push("+sender3", 1, 2, 1, "A", 1, 1004, out, sizeof(out), &out_len);
   sms_reassembly_push("+sender3", 2, 2, 1, "A", 1, 1005, out, sizeof(out), &out_len);
   sms_reassembly_push("+sender4", 1, 2, 1, "A", 1, 1006, out, sizeof(out), &out_len);
   sms_reassembly_push("+sender4", 2, 2, 1, "A", 1, 1007, out, sizeof(out), &out_len);

   sms_reassembly_stats_t s;
   sms_reassembly_stats(&s);
   TEST_ASSERT_EQUAL_UINT32(REASSEMBLY_SLOTS, s.slots_in_use);

   /* A brand new sender arrives — LRU eviction should free the oldest
    * slot (sender1/ref=1 at ts=1000). */
   reasm_result_t r = sms_reassembly_push("+sender9", 7, 2, 1, "A", 1, 1100, out, sizeof(out),
                                          &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_INCOMPLETE, r);

   sms_reassembly_stats(&s);
   TEST_ASSERT_EQUAL_UINT64(1, s.total_dropped_exhaustion);
}

void test_sweep_timeout(void) {
   char out[64];
   size_t out_len = 0;
   sms_reassembly_push("+1555", 1, 2, 1, "A", 1, 1000, out, sizeof(out), &out_len);

   int evicted = sms_reassembly_sweep(1000 + REASSEMBLY_TIMEOUT_SEC + 1);
   TEST_ASSERT_EQUAL_INT(1, evicted);

   sms_reassembly_stats_t s;
   sms_reassembly_stats(&s);
   TEST_ASSERT_EQUAL_UINT64(1, s.total_timed_out);
   TEST_ASSERT_EQUAL_UINT32(0, s.slots_in_use);
}

void test_bad_total_rejected(void) {
   char out[64];
   size_t out_len = 0;
   /* total=0 is invalid per UDH spec. */
   reasm_result_t r = sms_reassembly_push("+1555", 1, 0, 1, "A", 1, 1000, out, sizeof(out),
                                          &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_REJECTED_TOTAL, r);

   /* seq > total */
   r = sms_reassembly_push("+1555", 1, 2, 5, "A", 1, 1000, out, sizeof(out), &out_len);
   TEST_ASSERT_EQUAL_INT(REASM_REJECTED_TOTAL, r);
}

/* ── Unity test runner ───────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_complete_in_order);
   RUN_TEST(test_complete_out_of_order);
   RUN_TEST(test_duplicate_rejected);
   RUN_TEST(test_total_mismatch_rejected);
   RUN_TEST(test_sender_cap);
   RUN_TEST(test_lru_eviction_when_full);
   RUN_TEST(test_sweep_timeout);
   RUN_TEST(test_bad_total_rejected);
   return UNITY_END();
}
