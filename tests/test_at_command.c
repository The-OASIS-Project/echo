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
 * Unit tests for AT command response parsing.
 */

#include <string.h>

#include "at_command.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Response terminator parsing ─────────────────────────────────────── */

void test_parse_ok(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_TRUE(at_parse_terminator("OK", &status, &err_code));
   TEST_ASSERT_EQUAL_INT(AT_OK, status);
}

void test_parse_error(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_TRUE(at_parse_terminator("ERROR", &status, &err_code));
   TEST_ASSERT_EQUAL_INT(AT_ERROR, status);
}

void test_parse_cme_error(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_TRUE(at_parse_terminator("+CME ERROR: 10", &status, &err_code));
   TEST_ASSERT_EQUAL_INT(AT_CME_ERROR, status);
   TEST_ASSERT_EQUAL_INT(10, err_code);
}

void test_parse_cms_error(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_TRUE(at_parse_terminator("+CMS ERROR: 500", &status, &err_code));
   TEST_ASSERT_EQUAL_INT(AT_CMS_ERROR, status);
   TEST_ASSERT_EQUAL_INT(500, err_code);
}

void test_parse_no_carrier(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_TRUE(at_parse_terminator("NO CARRIER", &status, &err_code));
   TEST_ASSERT_EQUAL_INT(AT_NO_CARRIER, status);
}

void test_parse_busy(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_TRUE(at_parse_terminator("BUSY", &status, &err_code));
   TEST_ASSERT_EQUAL_INT(AT_BUSY, status);
}

void test_parse_no_answer(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_TRUE(at_parse_terminator("NO ANSWER", &status, &err_code));
   TEST_ASSERT_EQUAL_INT(AT_NO_ANSWER, status);
}

void test_parse_not_terminator(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_FALSE(at_parse_terminator("+CSQ: 18,99", &status, &err_code));
}

void test_parse_not_terminator_ring(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_FALSE(at_parse_terminator("RING", &status, &err_code));
}

void test_parse_null(void) {
   at_status_t status;
   TEST_ASSERT_FALSE(at_parse_terminator(NULL, &status, NULL));
}

void test_parse_empty(void) {
   at_status_t status;
   int err_code;
   TEST_ASSERT_FALSE(at_parse_terminator("", &status, &err_code));
}

/* ── Status string ───────────────────────────────────────────────────── */

void test_status_str_ok(void) {
   TEST_ASSERT_EQUAL_STRING("OK", at_status_str(AT_OK));
}

void test_status_str_timeout(void) {
   TEST_ASSERT_EQUAL_STRING("TIMEOUT", at_status_str(AT_TIMEOUT));
}

void test_status_str_port_error(void) {
   TEST_ASSERT_EQUAL_STRING("PORT ERROR", at_status_str(AT_PORT_ERROR));
}

/* ── Runner ──────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_parse_ok);
   RUN_TEST(test_parse_error);
   RUN_TEST(test_parse_cme_error);
   RUN_TEST(test_parse_cms_error);
   RUN_TEST(test_parse_no_carrier);
   RUN_TEST(test_parse_busy);
   RUN_TEST(test_parse_no_answer);
   RUN_TEST(test_parse_not_terminator);
   RUN_TEST(test_parse_not_terminator_ring);
   RUN_TEST(test_parse_null);
   RUN_TEST(test_parse_empty);
   RUN_TEST(test_status_str_ok);
   RUN_TEST(test_status_str_timeout);
   RUN_TEST(test_status_str_port_error);

   return UNITY_END();
}
