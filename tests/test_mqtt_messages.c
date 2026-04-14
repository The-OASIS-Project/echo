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
 * Unit tests for OCP message construction and parsing.
 */

#include <json-c/json.h>
#include <string.h>

#include "mqtt_comms.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Helper to check JSON has field ──────────────────────────────────── */

static struct json_object *parse_and_check(const char *json) {
   struct json_object *root = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL(root);
   return root;
}

static void assert_json_string(struct json_object *root, const char *key, const char *expected) {
   struct json_object *val;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, key, &val));
   TEST_ASSERT_EQUAL_STRING(expected, json_object_get_string(val));
}

static void assert_json_int(struct json_object *root, const char *key, int expected) {
   struct json_object *val;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, key, &val));
   TEST_ASSERT_EQUAL_INT(expected, json_object_get_int(val));
}

static void assert_json_has_key(struct json_object *root, const char *key) {
   struct json_object *val;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, key, &val));
}

/* ── Telemetry JSON ──────────────────────────────────────────────────── */

void test_telemetry_json_fields(void) {
   modem_telemetry_t telem = {
      .signal_dbm = -67,
      .signal_bars = 4,
      .csq = 18,
      .reg = REG_REGISTERED_HOME,
      .operator_name = "US Mobile",
      .network_type = "LTE",
      .call_state = CALL_STATE_IDLE,
      .sim = SIM_READY,
   };

   char buf[1024];
   int len = mqtt_build_telemetry_json(&telem, buf, sizeof(buf));
   TEST_ASSERT_GREATER_THAN(0, len);

   struct json_object *root = parse_and_check(buf);
   assert_json_string(root, "device", "echo");
   assert_json_int(root, "signal_dbm", -67);
   assert_json_int(root, "signal_bars", 4);
   assert_json_int(root, "csq", 18);
   assert_json_string(root, "registration", "registered_home");
   assert_json_string(root, "operator", "US Mobile");
   assert_json_string(root, "network_type", "LTE");
   assert_json_string(root, "call_state", "idle");
   assert_json_string(root, "sim_status", "ready");
   assert_json_has_key(root, "timestamp");
   json_object_put(root);
}

void test_telemetry_json_null(void) {
   char buf[1024];
   TEST_ASSERT_EQUAL_INT(-1, mqtt_build_telemetry_json(NULL, buf, sizeof(buf)));
}

void test_telemetry_json_small_buffer(void) {
   modem_telemetry_t telem = { 0 };
   char buf[10];
   TEST_ASSERT_EQUAL_INT(-1, mqtt_build_telemetry_json(&telem, buf, sizeof(buf)));
}

/* ── Event JSON ──────────────────────────────────────────────────────── */

void test_event_json_simple(void) {
   char buf[512];
   int len = mqtt_build_event_json("modem_lost", NULL, buf, sizeof(buf));
   TEST_ASSERT_GREATER_THAN(0, len);

   struct json_object *root = parse_and_check(buf);
   assert_json_string(root, "device", "echo");
   assert_json_string(root, "event", "modem_lost");
   assert_json_has_key(root, "timestamp");
   json_object_put(root);
}

void test_event_json_with_extra(void) {
   char buf[512];
   int len = mqtt_build_event_json("incoming_call", "\"number\":\"+15551234567\"", buf,
                                   sizeof(buf));
   TEST_ASSERT_GREATER_THAN(0, len);

   struct json_object *root = parse_and_check(buf);
   assert_json_string(root, "device", "echo");
   assert_json_string(root, "event", "incoming_call");
   assert_json_string(root, "number", "+15551234567");
   json_object_put(root);
}

void test_event_json_null_type(void) {
   char buf[512];
   TEST_ASSERT_EQUAL_INT(-1, mqtt_build_event_json(NULL, NULL, buf, sizeof(buf)));
}

/* ── Response JSON ───────────────────────────────────────────────────── */

void test_response_json_success(void) {
   char buf[512];
   int len = mqtt_build_response_json("dial", "worker_0_42", true, NULL, NULL, NULL, buf,
                                      sizeof(buf));
   TEST_ASSERT_GREATER_THAN(0, len);

   struct json_object *root = parse_and_check(buf);
   assert_json_string(root, "device", "echo");
   assert_json_string(root, "action", "dial");
   assert_json_string(root, "request_id", "worker_0_42");
   assert_json_string(root, "status", "success");
   json_object_put(root);
}

void test_response_json_success_with_value(void) {
   char buf[512];
   int len = mqtt_build_response_json("signal", "worker_0_48", true, "{\\\"signal_dbm\\\":-67}",
                                      NULL, NULL, buf, sizeof(buf));
   TEST_ASSERT_GREATER_THAN(0, len);

   struct json_object *root = parse_and_check(buf);
   assert_json_string(root, "status", "success");
   assert_json_has_key(root, "value");
   json_object_put(root);
}

void test_response_json_error(void) {
   char buf[512];
   int len = mqtt_build_response_json("dial", "worker_0_42", false, NULL, "NO_CARRIER",
                                      "Call failed: no carrier", buf, sizeof(buf));
   TEST_ASSERT_GREATER_THAN(0, len);

   struct json_object *root = parse_and_check(buf);
   assert_json_string(root, "status", "error");
   struct json_object *err;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "error", &err));
   assert_json_string(err, "code", "NO_CARRIER");
   assert_json_string(err, "message", "Call failed: no carrier");
   json_object_put(root);
}

/* ── Command parsing ─────────────────────────────────────────────────── */

void test_parse_command_dial(void) {
   const char *json =
       "{\"device\":\"echo\",\"action\":\"dial\","
       "\"value\":\"+15551234567\",\"request_id\":\"worker_0_42\",\"timestamp\":1744300000}";

   char action[64], value[256], req_id[64], data[1024];
   int rc = mqtt_parse_command(json, action, sizeof(action), value, sizeof(value), req_id,
                               sizeof(req_id), data, sizeof(data));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("dial", action);
   TEST_ASSERT_EQUAL_STRING("+15551234567", value);
   TEST_ASSERT_EQUAL_STRING("worker_0_42", req_id);
}

void test_parse_command_send_sms_with_data(void) {
   const char *json = "{\"device\":\"echo\",\"action\":\"send_sms\","
                      "\"value\":\"+15551234567\",\"request_id\":\"worker_0_45\","
                      "\"data\":{\"type\":\"text/plain\",\"encoding\":\"utf8\","
                      "\"content\":\"On my way\"},\"timestamp\":1744300000}";

   char action[64], value[256], req_id[64], data[1024];
   int rc = mqtt_parse_command(json, action, sizeof(action), value, sizeof(value), req_id,
                               sizeof(req_id), data, sizeof(data));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("send_sms", action);

   /* data_json should be the stringified data object */
   struct json_object *d = json_tokener_parse(data);
   TEST_ASSERT_NOT_NULL(d);
   assert_json_string(d, "content", "On my way");
   json_object_put(d);
}

void test_parse_command_missing_action(void) {
   const char *json = "{\"device\":\"echo\",\"request_id\":\"w42\"}";
   char action[64], value[256], req_id[64], data[1024];
   int rc = mqtt_parse_command(json, action, sizeof(action), value, sizeof(value), req_id,
                               sizeof(req_id), data, sizeof(data));
   TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_parse_command_missing_request_id(void) {
   const char *json = "{\"device\":\"echo\",\"action\":\"dial\",\"value\":\"+1555\"}";
   char action[64], value[256], req_id[64], data[1024];
   int rc = mqtt_parse_command(json, action, sizeof(action), value, sizeof(value), req_id,
                               sizeof(req_id), data, sizeof(data));
   TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_parse_command_invalid_json(void) {
   char action[64], value[256], req_id[64], data[1024];
   int rc = mqtt_parse_command("not json", action, sizeof(action), value, sizeof(value), req_id,
                               sizeof(req_id), data, sizeof(data));
   TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_parse_command_null(void) {
   char action[64], req_id[64];
   TEST_ASSERT_EQUAL_INT(-1, mqtt_parse_command(NULL, action, sizeof(action), NULL, 0, req_id,
                                                sizeof(req_id), NULL, 0));
}

/* ── Runner ──────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* Telemetry */
   RUN_TEST(test_telemetry_json_fields);
   RUN_TEST(test_telemetry_json_null);
   RUN_TEST(test_telemetry_json_small_buffer);

   /* Events */
   RUN_TEST(test_event_json_simple);
   RUN_TEST(test_event_json_with_extra);
   RUN_TEST(test_event_json_null_type);

   /* Responses */
   RUN_TEST(test_response_json_success);
   RUN_TEST(test_response_json_success_with_value);
   RUN_TEST(test_response_json_error);

   /* Command parsing */
   RUN_TEST(test_parse_command_dial);
   RUN_TEST(test_parse_command_send_sms_with_data);
   RUN_TEST(test_parse_command_missing_action);
   RUN_TEST(test_parse_command_missing_request_id);
   RUN_TEST(test_parse_command_invalid_json);
   RUN_TEST(test_parse_command_null);

   return UNITY_END();
}
