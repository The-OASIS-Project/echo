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
 * PDU encode/decode unit tests + malformed-input rejection.
 */

#include <string.h>

#include "pdu.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Heuristics ──────────────────────────────────────────────────────── */

void test_needs_ucs2_always_true_v1(void) {
   TEST_ASSERT_TRUE(pdu_needs_ucs2("ASCII"));
   TEST_ASSERT_TRUE(pdu_needs_ucs2(""));
}

void test_ref_id_is_nonzero(void) {
   /* Run a few times so a naive counter eventually rolls through 0. */
   for (int i = 0; i < 300; i++) {
      uint8_t r = pdu_new_ref_id();
      TEST_ASSERT_NOT_EQUAL(0, r);
   }
}

void test_segment_count_empty(void) {
   TEST_ASSERT_EQUAL_INT(0, pdu_segment_count("", true));
}

void test_segment_count_single(void) {
   TEST_ASSERT_EQUAL_INT(1, pdu_segment_count("Hello", true));
}

void test_segment_count_boundary(void) {
   /* 70 UCS2 chars fits in a single-segment PDU (no UDH). */
   char body[71];
   memset(body, 'A', 70);
   body[70] = '\0';
   TEST_ASSERT_EQUAL_INT(1, pdu_segment_count(body, true));

   /* 71 chars forces concat → 2 segments of 67 chars each. */
   char body2[72];
   memset(body2, 'A', 71);
   body2[71] = '\0';
   TEST_ASSERT_EQUAL_INT(2, pdu_segment_count(body2, true));
}

void test_segment_count_three(void) {
   /* 135 chars → 3 segments (67 + 67 + 1). */
   char body[136];
   memset(body, 'A', 135);
   body[135] = '\0';
   TEST_ASSERT_EQUAL_INT(3, pdu_segment_count(body, true));
}

/* ── Encode ──────────────────────────────────────────────────────────── */

void test_encode_single_segment(void) {
   pdu_segment_t segs[PDU_MAX_SEGMENTS];
   int n = 0;
   pdu_err_t err = pdu_encode_submit("+15551234567", "Hi", 42, true, segs, PDU_MAX_SEGMENTS, &n);
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   TEST_ASSERT_EQUAL_INT(1, n);
   /* UDHI must NOT be set on single-segment. First TPDU octet is at
    * offset 2 (after "00" SMSC prefix). Bits: MTI=01, VPF=10 → 0x11. */
   TEST_ASSERT_EQUAL_STRING_LEN("0011", segs[0].hex, 4);
   TEST_ASSERT_GREATER_THAN(0, segs[0].tpdu_octets);
}

void test_encode_sets_udhi_for_multi_segment(void) {
   char body[200];
   memset(body, 'A', 100); /* 100 UCS2 units → 2 segments */
   body[100] = '\0';

   pdu_segment_t segs[PDU_MAX_SEGMENTS];
   int n = 0;
   pdu_err_t err = pdu_encode_submit("+15551234567", body, 0x5A, true, segs, PDU_MAX_SEGMENTS, &n);
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   TEST_ASSERT_EQUAL_INT(2, n);
   /* UDHI bit = 0x40 → first octet = 0x51 (MTI=01, VPF=10, UDHI=1) */
   TEST_ASSERT_EQUAL_STRING_LEN("0051", segs[0].hex, 4);
   TEST_ASSERT_EQUAL_STRING_LEN("0051", segs[1].hex, 4);

   /* UDH concat is 05 00 03 <ref> <total> <seq>. Preamble occupies 14
    * octets (MTI, MR, DA[1+1+6], PID, DCS, VP, UDL), so UDH starts at
    * hex offset 2 + 14*2 = 30. */
   TEST_ASSERT_EQUAL_STRING_LEN("0500035A", segs[0].hex + 30, 8);
   TEST_ASSERT_EQUAL_STRING_LEN("02", segs[0].hex + 38, 2); /* total=2 */
   TEST_ASSERT_EQUAL_STRING_LEN("01", segs[0].hex + 40, 2); /* seq=1 */
   TEST_ASSERT_EQUAL_STRING_LEN("02", segs[1].hex + 38, 2);
   TEST_ASSERT_EQUAL_STRING_LEN("02", segs[1].hex + 40, 2); /* seq=2 */
}

void test_encode_bad_address_rejected(void) {
   pdu_segment_t segs[PDU_MAX_SEGMENTS];
   int n = 0;
   pdu_err_t err = pdu_encode_submit("not-a-number", "Hi", 1, true, segs, PDU_MAX_SEGMENTS, &n);
   TEST_ASSERT_EQUAL_INT(PDU_ERR_BAD_ADDRESS, err);
}

void test_encode_too_long_body_rejected(void) {
   /* 11 segments worth of content = 11 * 67 = 737 chars, > PDU_MAX_SEGMENTS. */
   char body[760];
   memset(body, 'A', 750);
   body[750] = '\0';
   pdu_segment_t segs[PDU_MAX_SEGMENTS];
   int n = 0;
   pdu_err_t err = pdu_encode_submit("+15551234567", body, 1, true, segs, PDU_MAX_SEGMENTS, &n);
   TEST_ASSERT_EQUAL_INT(PDU_ERR_BODY_TOO_LONG, err);
}

/* ── Decode: malformed-input rejection ───────────────────────────────── */

void test_decode_odd_hex_rejected(void) {
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode("0123456", &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_ERR_BAD_HEX, err);
}

void test_decode_non_hex_rejected(void) {
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode("00ZZ1234", &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_ERR_BAD_HEX, err);
}

void test_decode_empty_rejected(void) {
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode("", &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_ERR_TRUNCATED, err);
}

void test_decode_oversized_smsc_len_rejected(void) {
   /* smsc_len = 0xFF but only 2 bytes follow */
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode("FF0000", &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_ERR_BAD_LENGTH, err);
}

/* ── Decode: round-trip ──────────────────────────────────────────────── */

/* Captured DELIVER PDU. Hand-built for the test: no SMSC (len=0), MTI=0,
 * sender = +15551234567, DCS = UCS2 (0x08), SCTS = fixed, body = "Hi".
 *   00            SMSC length = 0 (no override)
 *   04            first octet: MTI=00 (DELIVER), no UDHI
 *   0B            TP-OA length = 11 semi-octets
 *   91            TOA = international
 *   51 55 21 43 65 F7   +15551234567 in nibble-swapped BCD
 *   00            TP-PID
 *   08            TP-DCS = UCS2
 *   26 04 21 50 00 00 00   TP-SCTS = 2026-04-12T05:00:00 UTC (placeholder)
 *   04            TP-UDL = 4 octets
 *   00 48 00 69   UCS2 "Hi"
 */
void test_decode_simple_ucs2(void) {
   const char *hex = "00040B915155214365F70008260421500000000400480069";
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode(hex, &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   TEST_ASSERT_EQUAL_STRING("+15551234567", out.sender);
   TEST_ASSERT_FALSE(out.has_udh);
   TEST_ASSERT_TRUE(out.is_ucs2);
   TEST_ASSERT_EQUAL_STRING("Hi", body);
   TEST_ASSERT_EQUAL_INT(2, out.body_len);
}

/* UDH concat DELIVER PDU. Same shape, UDHI set, UDHL=05, IEI=00, IEL=03,
 * ref=7A, total=2, seq=1, then UCS2 body "Hi".
 *   00 44 0B 91 51 55 21 43 65 F7 00 08 26 04 21 50 00 00 00
 *   0A                     TP-UDL = 10 = UDH(6) + body(4)
 *   05 00 03 7A 02 01      UDH concat IE
 *   00 48 00 69            UCS2 "Hi"
 */
void test_decode_udh_concat(void) {
   /* Fields: 00 44 0B 91 5155214365F7 00 08 26042150000000 0A 050003 7A 02 01 00480069 */
   const char *hex = "00440B915155214365F7000826042150000000"
                     "0A"
                     "0500037A020100480069";
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode(hex, &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   TEST_ASSERT_EQUAL_STRING("+15551234567", out.sender);
   TEST_ASSERT_TRUE(out.has_udh);
   TEST_ASSERT_EQUAL_UINT8(0x7A, out.udh_ref_id);
   TEST_ASSERT_EQUAL_UINT8(2, out.udh_total);
   TEST_ASSERT_EQUAL_UINT8(1, out.udh_seq);
   TEST_ASSERT_EQUAL_STRING("Hi", body);
}

void test_decode_bad_udh_seq_rejected(void) {
   /* UDHL=05, concat IE total=2, seq=5 → invalid. */
   const char *hex = "00440B915155214365F7000826042150000000"
                     "0A"
                     "0500037A020500480069";
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode(hex, &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_ERR_BAD_UDH, err);
}

void test_decode_sanitizes_zero_width_and_tag(void) {
   /* UCS2 body "A\u200B B\uE0041C" where U+200B is ZWSP (dropped) and
    * U+E0041 is a tag char (dropped). Expect "A BC".
    * Surrogate encoding of U+E0041 = DB40 DC41.
    *   0x0041 'A' 0x200B ZWSP 0x0020 ' ' 0x0042 'B' 0xDB40 0xDC41 0x0043 'C'
    * = 7 UCS2 code units = 14 octets. */
   const char *hex = "00040B915155214365F7000826042150000000"
                     "0E"
                     "0041200B00200042DB40DC410043";
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode(hex, &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   TEST_ASSERT_EQUAL_STRING("A BC", body);
}

void test_decode_rejects_duplicate_concat_ie(void) {
   /* UDH carries two IEI=0x00 concat IEs. Should reject as BAD_UDH.
    * UDHL=0B (11 bytes): 05 00 03 AA 02 01 05 00 03 BB 02 01
    * Body: 00 48 = "H" (2 octets)
    * TP-UDL = 11 + 1 + 2 = 14 = 0x0E */
   const char *hex = "00440B915155214365F7000826042150000000"
                     "0E"
                     "0B"
                     "050003AA0201"
                     "050003BB0201"
                     "0048";
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode(hex, &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_ERR_BAD_UDH, err);
}

void test_decode_sanitizes_nul_and_bidi(void) {
   /* UCS2 body "A\u0000B\u202EC" → sanitized to "ABC" (NUL stripped,
    * bidi U+202E dropped). 5 UCS2 units = 10 octets → UDL = 0x0A. */
   const char *hex = "00040B915155214365F7000826042150000000"
                     "0A"
                     "004100000042202E0043";
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode(hex, &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   TEST_ASSERT_EQUAL_STRING("ABC", body);
}

void test_decode_gsm7_simple(void) {
   /* DELIVER PDU, DCS=0x00 (GSM7), body = "hello" (5 septets packed into
    * 5 bytes: E8 32 9B FD 06). SCTS is a filler. Sender = +15551234567.
    * Real-world test — this is exactly what an iPhone sending "hello"
    * lands on the modem. */
   const char *hex = "00040B915155214365F700002604215000000005E8329BFD06";
   pdu_decoded_t out;
   char body[64];
   pdu_err_t err = pdu_decode(hex, &out, body, sizeof(body));
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   TEST_ASSERT_EQUAL_STRING("+15551234567", out.sender);
   TEST_ASSERT_FALSE(out.is_ucs2);
   TEST_ASSERT_EQUAL_STRING("hello", body);
}

void test_decode_buffer_too_small_graceful(void) {
   /* Same body "Hi" but only 1-byte output — decoder must not overflow
    * and should truncate cleanly. */
   const char *hex = "00040B915155214365F70008260421500000000400480069";
   pdu_decoded_t out;
   char tiny[2]; /* room for NUL only */
   pdu_err_t err = pdu_decode(hex, &out, tiny, sizeof(tiny));
   TEST_ASSERT_EQUAL_INT(PDU_OK, err);
   /* 'H' is 1 byte UTF-8, won't fit with NUL room guard → truncates. */
   TEST_ASSERT_LESS_OR_EQUAL_INT(1, out.body_len);
}

/* ── Unity test runner ───────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_needs_ucs2_always_true_v1);
   RUN_TEST(test_ref_id_is_nonzero);
   RUN_TEST(test_segment_count_empty);
   RUN_TEST(test_segment_count_single);
   RUN_TEST(test_segment_count_boundary);
   RUN_TEST(test_segment_count_three);

   RUN_TEST(test_encode_single_segment);
   RUN_TEST(test_encode_sets_udhi_for_multi_segment);
   RUN_TEST(test_encode_bad_address_rejected);
   RUN_TEST(test_encode_too_long_body_rejected);

   RUN_TEST(test_decode_odd_hex_rejected);
   RUN_TEST(test_decode_non_hex_rejected);
   RUN_TEST(test_decode_empty_rejected);
   RUN_TEST(test_decode_oversized_smsc_len_rejected);

   RUN_TEST(test_decode_simple_ucs2);
   RUN_TEST(test_decode_udh_concat);
   RUN_TEST(test_decode_bad_udh_seq_rejected);
   RUN_TEST(test_decode_sanitizes_nul_and_bidi);
   RUN_TEST(test_decode_sanitizes_zero_width_and_tag);
   RUN_TEST(test_decode_rejects_duplicate_concat_ie);
   RUN_TEST(test_decode_gsm7_simple);
   RUN_TEST(test_decode_buffer_too_small_graceful);

   return UNITY_END();
}
