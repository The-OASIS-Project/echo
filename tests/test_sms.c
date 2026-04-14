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
 * Unit tests for SMS helpers and phone number validation.
 */

#include <string.h>

#include "sms.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Phone number validation ─────────────────────────────────────────── */

void test_valid_number_e164(void) {
   TEST_ASSERT_TRUE(sms_validate_number("+15551234567"));
}

void test_valid_number_star_prefix(void) {
   TEST_ASSERT_TRUE(sms_validate_number("*67+15551234567"));
}

void test_valid_number_hash_prefix(void) {
   TEST_ASSERT_TRUE(sms_validate_number("#31#"));
}

void test_valid_number_emergency(void) {
   TEST_ASSERT_TRUE(sms_validate_number("911"));
}

void test_valid_number_digits_only(void) {
   TEST_ASSERT_TRUE(sms_validate_number("5551234567"));
}

void test_invalid_number_null(void) {
   TEST_ASSERT_FALSE(sms_validate_number(NULL));
}

void test_invalid_number_empty(void) {
   TEST_ASSERT_FALSE(sms_validate_number(""));
}

void test_invalid_number_too_long(void) {
   TEST_ASSERT_FALSE(sms_validate_number("+123456789012345678901")); /* 22 chars */
}

void test_invalid_number_letters(void) {
   TEST_ASSERT_FALSE(sms_validate_number("+1555abc4567"));
}

void test_invalid_number_semicolon(void) {
   TEST_ASSERT_FALSE(sms_validate_number("+1555;DROP TABLE"));
}

void test_invalid_number_backtick(void) {
   TEST_ASSERT_FALSE(sms_validate_number("`ls`"));
}

void test_invalid_number_space(void) {
   TEST_ASSERT_FALSE(sms_validate_number("555 1234567"));
}

void test_invalid_number_parens(void) {
   TEST_ASSERT_FALSE(sms_validate_number("(555)1234567"));
}

/* ── SMS body sanitization ───────────────────────────────────────────── */

void test_sanitize_body_normal(void) {
   char out[SMS_BODY_MAX + 1];
   int len = sms_sanitize_body("Hello, world!", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(13, len);
   TEST_ASSERT_EQUAL_STRING("Hello, world!", out);
}

void test_sanitize_body_newline_preserved(void) {
   char out[SMS_BODY_MAX + 1];
   int len = sms_sanitize_body("Line1\nLine2", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(11, len);
   TEST_ASSERT_EQUAL_STRING("Line1\nLine2", out);
}

void test_sanitize_body_ctrl_z_rejected(void) {
   char body[] = "Hello\x1AWorld";
   char out[SMS_BODY_MAX + 1];
   int len = sms_sanitize_body(body, out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(-1, len);
}

void test_sanitize_body_esc_rejected(void) {
   char body[] = "Hello\x1BWorld";
   char out[SMS_BODY_MAX + 1];
   int len = sms_sanitize_body(body, out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(-1, len);
}

void test_sanitize_body_control_chars_stripped(void) {
   char body[] = "Hello\x01\x02\x03World";
   char out[SMS_BODY_MAX + 1];
   int len = sms_sanitize_body(body, out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(10, len);
   TEST_ASSERT_EQUAL_STRING("HelloWorld", out);
}

void test_sanitize_body_null_rejected(void) {
   char out[SMS_BODY_MAX + 1];
   TEST_ASSERT_EQUAL_INT(-1, sms_sanitize_body(NULL, out, sizeof(out)));
}

void test_sanitize_body_truncation(void) {
   /* Create a body longer than SMS_BODY_MAX */
   char body[SMS_BODY_MAX + 100];
   memset(body, 'A', sizeof(body) - 1);
   body[sizeof(body) - 1] = '\0';

   char out[SMS_BODY_MAX + 1];
   int len = sms_sanitize_body(body, out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(SMS_BODY_MAX, len);
}

/* ── CLIP sanitization ───────────────────────────────────────────────── */

void test_sanitize_clip_valid(void) {
   char out[PHONE_NUMBER_MAX + 1];
   TEST_ASSERT_TRUE(sms_sanitize_clip("+15551234567", out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("+15551234567", out);
}

void test_sanitize_clip_strips_junk(void) {
   char out[PHONE_NUMBER_MAX + 1];
   TEST_ASSERT_TRUE(sms_sanitize_clip(" +1 (555) 123-4567 ", out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("+15551234567", out);
}

void test_sanitize_clip_empty(void) {
   char out[PHONE_NUMBER_MAX + 1];
   TEST_ASSERT_FALSE(sms_sanitize_clip("", out, sizeof(out)));
}

void test_sanitize_clip_garbage(void) {
   char out[PHONE_NUMBER_MAX + 1];
   TEST_ASSERT_FALSE(sms_sanitize_clip("BLOCKED", out, sizeof(out)));
}

/* ── Runner ──────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* Phone number validation */
   RUN_TEST(test_valid_number_e164);
   RUN_TEST(test_valid_number_star_prefix);
   RUN_TEST(test_valid_number_hash_prefix);
   RUN_TEST(test_valid_number_emergency);
   RUN_TEST(test_valid_number_digits_only);
   RUN_TEST(test_invalid_number_null);
   RUN_TEST(test_invalid_number_empty);
   RUN_TEST(test_invalid_number_too_long);
   RUN_TEST(test_invalid_number_letters);
   RUN_TEST(test_invalid_number_semicolon);
   RUN_TEST(test_invalid_number_backtick);
   RUN_TEST(test_invalid_number_space);
   RUN_TEST(test_invalid_number_parens);

   /* SMS body sanitization */
   RUN_TEST(test_sanitize_body_normal);
   RUN_TEST(test_sanitize_body_newline_preserved);
   RUN_TEST(test_sanitize_body_ctrl_z_rejected);
   RUN_TEST(test_sanitize_body_esc_rejected);
   RUN_TEST(test_sanitize_body_control_chars_stripped);
   RUN_TEST(test_sanitize_body_null_rejected);
   RUN_TEST(test_sanitize_body_truncation);

   /* CLIP sanitization */
   RUN_TEST(test_sanitize_clip_valid);
   RUN_TEST(test_sanitize_clip_strips_junk);
   RUN_TEST(test_sanitize_clip_empty);
   RUN_TEST(test_sanitize_clip_garbage);

   return UNITY_END();
}
