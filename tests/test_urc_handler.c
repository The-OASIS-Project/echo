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
 * Unit tests for URC classification and parsing.
 */

#include <string.h>

#include "unity.h"
#include "urc_handler.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── URC classification ──────────────────────────────────────────────── */

void test_classify_ring(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("RING", &event));
   TEST_ASSERT_EQUAL_INT(URC_RING, event.type);
}

void test_classify_clip_with_number(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("+CLIP: \"+15551234567\",145,,,,0", &event));
   TEST_ASSERT_EQUAL_INT(URC_CLIP, event.type);
   TEST_ASSERT_EQUAL_STRING("+15551234567", event.number);
}

void test_classify_clip_empty_number(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("+CLIP: \"\",128,,,,0", &event));
   TEST_ASSERT_EQUAL_INT(URC_CLIP, event.type);
   TEST_ASSERT_EQUAL_STRING("", event.number);
}

void test_classify_cmti(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("+CMTI: \"SM\",3", &event));
   TEST_ASSERT_EQUAL_INT(URC_CMTI, event.type);
   TEST_ASSERT_EQUAL_INT(3, event.index);
}

void test_classify_cmti_high_index(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("+CMTI: \"SM\",42", &event));
   TEST_ASSERT_EQUAL_INT(URC_CMTI, event.type);
   TEST_ASSERT_EQUAL_INT(42, event.index);
}

void test_classify_no_carrier(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("NO CARRIER", &event));
   TEST_ASSERT_EQUAL_INT(URC_NO_CARRIER, event.type);
}

void test_classify_busy(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("BUSY", &event));
   TEST_ASSERT_EQUAL_INT(URC_BUSY, event.type);
}

void test_classify_no_answer(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("NO ANSWER", &event));
   TEST_ASSERT_EQUAL_INT(URC_NO_ANSWER, event.type);
}

void test_classify_connect(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("CONNECT", &event));
   TEST_ASSERT_EQUAL_INT(URC_CONNECT, event.type);
}

void test_classify_connect_with_speed(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("CONNECT 115200", &event));
   TEST_ASSERT_EQUAL_INT(URC_CONNECT, event.type);
}

void test_classify_creg_home(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("+CREG: 1", &event));
   TEST_ASSERT_EQUAL_INT(URC_CREG, event.type);
   TEST_ASSERT_EQUAL_INT(1, event.reg_stat);
}

void test_classify_creg_roaming(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("+CREG: 5", &event));
   TEST_ASSERT_EQUAL_INT(URC_CREG, event.type);
   TEST_ASSERT_EQUAL_INT(5, event.reg_stat);
}

void test_classify_voice_call_begin(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("VOICE CALL: BEGIN", &event));
   TEST_ASSERT_EQUAL_INT(URC_VOICE_CALL_BEGIN, event.type);
}

void test_classify_voice_call_end(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("VOICE CALL: END: 000211", &event));
   TEST_ASSERT_EQUAL_INT(URC_VOICE_CALL_END, event.type);
}

void test_classify_sms_prompt(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify("> ", &event));
   TEST_ASSERT_EQUAL_INT(URC_SMS_PROMPT, event.type);
}

void test_classify_sms_prompt_bare(void) {
   urc_event_t event;
   TEST_ASSERT_TRUE(urc_classify(">", &event));
   TEST_ASSERT_EQUAL_INT(URC_SMS_PROMPT, event.type);
}

/* ── Not-URC lines ───────────────────────────────────────────────────── */

void test_classify_ok_is_not_urc(void) {
   urc_event_t event;
   TEST_ASSERT_FALSE(urc_classify("OK", &event));
}

void test_classify_error_is_not_urc(void) {
   urc_event_t event;
   TEST_ASSERT_FALSE(urc_classify("ERROR", &event));
}

void test_classify_data_line_not_urc(void) {
   urc_event_t event;
   TEST_ASSERT_FALSE(urc_classify("+CSQ: 18,99", &event));
}

void test_classify_null(void) {
   urc_event_t event;
   TEST_ASSERT_FALSE(urc_classify(NULL, &event));
}

void test_classify_empty(void) {
   urc_event_t event;
   TEST_ASSERT_FALSE(urc_classify("", &event));
}

/* ── Raw line preserved ──────────────────────────────────────────────── */

void test_raw_line_preserved(void) {
   urc_event_t event;
   urc_classify("RING", &event);
   TEST_ASSERT_EQUAL_STRING("RING", event.raw);
}

/* ── Runner ──────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_classify_ring);
   RUN_TEST(test_classify_clip_with_number);
   RUN_TEST(test_classify_clip_empty_number);
   RUN_TEST(test_classify_cmti);
   RUN_TEST(test_classify_cmti_high_index);
   RUN_TEST(test_classify_no_carrier);
   RUN_TEST(test_classify_busy);
   RUN_TEST(test_classify_no_answer);
   RUN_TEST(test_classify_connect);
   RUN_TEST(test_classify_connect_with_speed);
   RUN_TEST(test_classify_voice_call_begin);
   RUN_TEST(test_classify_voice_call_end);
   RUN_TEST(test_classify_creg_home);
   RUN_TEST(test_classify_creg_roaming);
   RUN_TEST(test_classify_sms_prompt);
   RUN_TEST(test_classify_sms_prompt_bare);
   RUN_TEST(test_classify_ok_is_not_urc);
   RUN_TEST(test_classify_error_is_not_urc);
   RUN_TEST(test_classify_data_line_not_urc);
   RUN_TEST(test_classify_null);
   RUN_TEST(test_classify_empty);
   RUN_TEST(test_raw_line_preserved);

   return UNITY_END();
}
