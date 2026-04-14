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
 * ECHO — Enhanced Cellular Handling Operations.
 * Standalone modem daemon for the SIM7600G-H 4G modem.
 */

#include <getopt.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "at_command.h"
#include "echo.h"
#include "logging.h"
#include "modem.h"
#include "mqtt_comms.h"
#include "sms.h"
#include "urc_handler.h"

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;
static at_context_t g_at_ctx;
static rate_bucket_t g_call_bucket;
static rate_bucket_t g_sms_bucket;

/* Thread-safe call state using __atomic builtins (GCC) */
static call_state_t g_call_state = CALL_STATE_IDLE;

static call_state_t get_call_state(void) {
   return __atomic_load_n(&g_call_state, __ATOMIC_ACQUIRE);
}

static void set_call_state(call_state_t state) {
   __atomic_store_n(&g_call_state, state, __ATOMIC_RELEASE);
}

/* ── Command queue (thread-safe, lock-free SPSC ring) ────────────────── */

#define CMD_QUEUE_SIZE 16

typedef enum {
   CMD_TYPE_MQTT = 0,       /* MQTT command from echo/cmd */
   CMD_TYPE_CMTI,           /* Deferred SMS read from URC_CMTI */
   CMD_TYPE_CALL_CONNECTED, /* Deferred call audio setup */
} cmd_type_t;

typedef struct {
   cmd_type_t type;
   char action[64];
   char value[256];
   char request_id[64];
   char data_json[1024];
   int sms_index; /* for CMD_TYPE_CMTI */
} cmd_entry_t;

typedef struct {
   cmd_entry_t entries[CMD_QUEUE_SIZE];
   volatile int head; /* written by producer */
   volatile int tail; /* written by consumer */
} cmd_queue_t;

static cmd_queue_t g_cmd_queue;

static void cmd_queue_init(cmd_queue_t *q) {
   memset(q, 0, sizeof(*q));
}

static bool cmd_queue_push(cmd_queue_t *q, const cmd_entry_t *entry) {
   int next = (q->head + 1) % CMD_QUEUE_SIZE;
   if (next == q->tail) {
      return false; /* full */
   }
   q->entries[q->head] = *entry;
   __atomic_store_n(&q->head, next, __ATOMIC_RELEASE);
   return true;
}

static bool cmd_queue_pop(cmd_queue_t *q, cmd_entry_t *entry) {
   if (q->tail == __atomic_load_n(&q->head, __ATOMIC_ACQUIRE)) {
      return false; /* empty */
   }
   *entry = q->entries[q->tail];
   __atomic_store_n(&q->tail, (q->tail + 1) % CMD_QUEUE_SIZE, __ATOMIC_RELEASE);
   return true;
}

/* ── Signal handler ──────────────────────────────────────────────────── */

static void signal_handler(int sig) {
   (void)sig;
   g_running = 0;
}

/* ── Rate limiter ────────────────────────────────────────────────────── */

void rate_bucket_init(rate_bucket_t *bucket, int max_per_hour) {
   memset(bucket, 0, sizeof(*bucket));
   bucket->max_per_hour = max_per_hour;
}

bool rate_bucket_allow(rate_bucket_t *bucket) {
   int64_t now = (int64_t)time(NULL);
   int64_t window_start = now - 3600;

   /* Count events in the last hour */
   int active = 0;
   for (int i = 0; i < bucket->count && i < 64; i++) {
      if (bucket->timestamps[i] >= window_start) {
         active++;
      }
   }

   if (active >= bucket->max_per_hour) {
      return false;
   }

   /* Record this event */
   bucket->timestamps[bucket->head] = now;
   bucket->head = (bucket->head + 1) % 64;
   if (bucket->count < 64) {
      bucket->count++;
   }

   /* Fix: keep count accurate as old entries expire */
   bucket->count = active + 1;

   return true;
}

/* ── Input validation helpers ────────────────────────────────────────── */

/**
 * @brief Validate that a string contains only digits (for SMS index).
 */
static bool validate_sms_index(const char *value, long *out) {
   if (!value || value[0] == '\0') {
      return false;
   }
   char *endptr;
   long idx = strtol(value, &endptr, 10);
   if (*endptr != '\0' || idx < 0 || idx > 999) {
      return false;
   }
   if (out) {
      *out = idx;
   }
   return true;
}

/**
 * @brief Validate a DTMF character: [0-9*#A-D].
 */
static bool validate_dtmf(char c) {
   return (c >= '0' && c <= '9') || c == '*' || c == '#' || (c >= 'A' && c <= 'D');
}

/* ── URC event handler (runs on URC reader thread — keep fast) ───────── */

static void on_urc_event(const urc_event_t *event, void *userdata) {
   (void)userdata;

   char json[1024];

   switch (event->type) {
      case URC_RING: {
         /* Suppress duplicate RING events — only publish on first RING */
         if (get_call_state() == CALL_STATE_RINGING_IN) {
            break;
         }
         set_call_state(CALL_STATE_RINGING_IN);
         char extra[256];
         if (event->number[0] != '\0') {
            snprintf(extra, sizeof(extra), "\"number\":\"%s\"", event->number);
         } else {
            snprintf(extra, sizeof(extra), "\"number\":\"\"");
         }
         if (mqtt_build_event_json("incoming_call", extra, json, sizeof(json)) >= 0) {
            mqtt_publish_event(json);
         }
         OLOG_INFO("Incoming call from: %s", event->number[0] ? event->number : "(blocked)");
         break;
      }

      case URC_CONNECT:
      case URC_VOICE_CALL_BEGIN: {
         set_call_state(CALL_STATE_ACTIVE);
         if (mqtt_build_event_json("call_connected", NULL, json, sizeof(json)) >= 0) {
            mqtt_publish_event(json);
         }
         /* Defer echo cancellation setup to main thread (needs AT command) */
         cmd_entry_t audio_cmd;
         memset(&audio_cmd, 0, sizeof(audio_cmd));
         audio_cmd.type = CMD_TYPE_CALL_CONNECTED;
         cmd_queue_push(&g_cmd_queue, &audio_cmd);
         OLOG_INFO("Call connected");
         break;
      }

      case URC_NO_CARRIER: {
         /* Suppress if already idle (VOICE CALL: END already handled it) */
         call_state_t prev = get_call_state();
         if (prev == CALL_STATE_IDLE) {
            break;
         }
         set_call_state(CALL_STATE_IDLE);
         const char *reason = (prev == CALL_STATE_DIALING) ? "no_carrier" : "remote_hangup";
         char extra[128];
         snprintf(extra, sizeof(extra), "\"reason\":\"%s\"", reason);
         if (mqtt_build_event_json("call_ended", extra, json, sizeof(json)) >= 0) {
            mqtt_publish_event(json);
         }
         OLOG_INFO("Call ended: %s", reason);
         break;
      }

      case URC_BUSY:
         if (get_call_state() == CALL_STATE_IDLE) {
            break;
         }
         set_call_state(CALL_STATE_IDLE);
         if (mqtt_build_event_json("call_ended", "\"reason\":\"busy\"", json, sizeof(json)) >= 0) {
            mqtt_publish_event(json);
         }
         OLOG_INFO("Call ended: busy");
         break;

      case URC_NO_ANSWER:
         if (get_call_state() == CALL_STATE_IDLE) {
            break;
         }
         set_call_state(CALL_STATE_IDLE);
         if (mqtt_build_event_json("call_ended", "\"reason\":\"no_answer\"", json, sizeof(json)) >=
             0) {
            mqtt_publish_event(json);
         }
         OLOG_INFO("Call ended: no answer");
         break;

      case URC_VOICE_CALL_END:
         if (get_call_state() == CALL_STATE_IDLE) {
            break;
         }
         set_call_state(CALL_STATE_IDLE);
         if (mqtt_build_event_json("call_ended", "\"reason\":\"voice_call_end\"", json,
                                   sizeof(json)) >= 0) {
            mqtt_publish_event(json);
         }
         OLOG_INFO("Call ended (VOICE CALL: END)");
         break;

      case URC_CMTI: {
         /* Defer SMS read to main thread — calling at_command_send from
          * the URC reader would deadlock (reader waits for itself). */
         OLOG_INFO("New SMS at index %d, deferring read to main loop", event->index);
         cmd_entry_t cmd;
         memset(&cmd, 0, sizeof(cmd));
         cmd.type = CMD_TYPE_CMTI;
         cmd.sms_index = event->index;
         if (!cmd_queue_push(&g_cmd_queue, &cmd)) {
            OLOG_WARNING("Command queue full, dropping CMTI event for index %d", event->index);
         }
         break;
      }

      case URC_CREG:
         OLOG_INFO("Network registration changed: %d", event->reg_stat);
         break;

      default:
         break;
   }
}

/* ── Deferred SMS read (runs on main thread) ─────────────────────────── */

static void handle_cmti(int sms_index) {
   OLOG_INFO("Reading SMS at index %d...", sms_index);
   char cmd[32];
   snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", sms_index);
   at_response_t resp;
   at_status_t rc = at_command_send(&g_at_ctx, cmd, &resp, AT_TIMEOUT_DEFAULT);
   if (rc != AT_OK) {
      OLOG_WARNING("Failed to read SMS at index %d: %s (code=%d, data=[%s])", sms_index,
                   at_status_str(rc), resp.error_code, resp.data);
      return;
   }

   /* Parse +CMGR response to extract sender and body */
   char sender[PHONE_NUMBER_MAX + 1] = "";
   const char *body = "";

   const char *cmgr = strstr(resp.data, "+CMGR:");
   if (cmgr) {
      const char *q1 = strchr(cmgr, '"');
      if (q1) {
         q1 = strchr(q1 + 1, '"');
         if (q1) {
            q1 = strchr(q1 + 1, '"');
            if (q1) {
               const char *q2 = strchr(q1 + 1, '"');
               if (q2) {
                  size_t len = (size_t)(q2 - q1 - 1);
                  if (len > PHONE_NUMBER_MAX) {
                     len = PHONE_NUMBER_MAX;
                  }
                  memcpy(sender, q1 + 1, len);
                  sender[len] = '\0';
               }
            }
         }
      }
      const char *nl = strchr(cmgr, '\n');
      if (nl) {
         body = nl + 1;
      }
   }

   /* Build event using json-c for proper escaping */
   struct json_object *evt = json_object_new_object();
   json_object_object_add(evt, "device", json_object_new_string("echo"));
   json_object_object_add(evt, "event", json_object_new_string("sms_received"));
   json_object_object_add(evt, "index", json_object_new_int(sms_index));
   json_object_object_add(evt, "sender", json_object_new_string(sender));
   json_object_object_add(evt, "body", json_object_new_string(body));
   json_object_object_add(evt, "timestamp", json_object_new_int64((int64_t)time(NULL)));

   const char *json_str = json_object_to_json_string(evt);
   mqtt_publish_event(json_str);
   json_object_put(evt);

   /* DAWN is responsible for sending delete_sms after committing to phone_db.
    * If ECHO auto-deleted here and DAWN crashed before DB commit, the SMS
    * would be lost. The index is included in the event for DAWN to reference. */
}

/* ── MQTT command handler (queues to main thread) ────────────────────── */

static void on_mqtt_command(const char *action,
                            const char *value,
                            const char *request_id,
                            const char *data_json,
                            void *userdata) {
   (void)userdata;

   cmd_entry_t cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.type = CMD_TYPE_MQTT;
   snprintf(cmd.action, sizeof(cmd.action), "%s", action);
   if (value) {
      snprintf(cmd.value, sizeof(cmd.value), "%s", value);
   }
   snprintf(cmd.request_id, sizeof(cmd.request_id), "%s", request_id);
   if (data_json) {
      snprintf(cmd.data_json, sizeof(cmd.data_json), "%s", data_json);
   }

   if (!cmd_queue_push(&g_cmd_queue, &cmd)) {
      OLOG_WARNING("Command queue full, rejecting action=%s request_id=%s", action, request_id);
      mqtt_publish_response(action, request_id, false, NULL, "QUEUE_FULL",
                            "Command queue full, try again");
   }
}

/* ── Command processor (runs on main thread) ─────────────────────────── */

static void process_mqtt_command(const cmd_entry_t *cmd) {
   const char *action = cmd->action;
   const char *value = cmd->value;
   const char *request_id = cmd->request_id;
   const char *data_json = cmd->data_json;

   /* dial */
   if (strcmp(action, "dial") == 0) {
      if (!sms_validate_number(value)) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_NUMBER",
                               "Phone number validation failed");
         return;
      }
      if (!rate_bucket_allow(&g_call_bucket)) {
         mqtt_publish_response(action, request_id, false, NULL, "RATE_LIMITED",
                               "Call rate limit exceeded");
         return;
      }

      char at_cmd[64];
      snprintf(at_cmd, sizeof(at_cmd), "ATD%s;", value);
      at_status_t rc = at_command_send_async(&g_at_ctx, at_cmd);
      if (rc == AT_OK) {
         set_call_state(CALL_STATE_DIALING);
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, "AT_ERROR",
                               "Failed to send dial command");
      }
      return;
   }

   /* answer */
   if (strcmp(action, "answer") == 0) {
      at_status_t rc = at_command_send_async(&g_at_ctx, "ATA");
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, "AT_ERROR",
                               "Failed to send answer command");
      }
      return;
   }

   /* hangup — AT+CHUP works in all call states on SIM7600 (ATH does not) */
   if (strcmp(action, "hangup") == 0) {
      set_call_state(CALL_STATE_HANGING_UP);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, "AT+CHUP", &resp, AT_TIMEOUT_DEFAULT);
      set_call_state(CALL_STATE_IDLE);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc), "Hangup failed");
      }
      return;
   }

   /* send_sms */
   if (strcmp(action, "send_sms") == 0) {
      if (!sms_validate_number(value)) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_NUMBER",
                               "Phone number validation failed");
         return;
      }
      if (!rate_bucket_allow(&g_sms_bucket)) {
         mqtt_publish_response(action, request_id, false, NULL, "RATE_LIMITED",
                               "SMS rate limit exceeded");
         return;
      }

      /* Extract body from data_json */
      char body[SMS_BODY_MAX + 1] = "";
      if (data_json[0] != '\0') {
         struct json_object *data = json_tokener_parse(data_json);
         if (data) {
            struct json_object *j_content;
            if (json_object_object_get_ex(data, "content", &j_content)) {
               snprintf(body, sizeof(body), "%s", json_object_get_string(j_content));
            }
            json_object_put(data);
         }
      }

      /* Sanitize body */
      char clean[SMS_BODY_MAX + 1];
      int clean_len = sms_sanitize_body(body, clean, sizeof(clean));
      if (clean_len < 0) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_BODY",
                               "SMS body contains dangerous characters");
         return;
      }

      at_response_t resp;
      at_status_t rc = at_command_send_sms(&g_at_ctx, value, clean, &resp);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc),
                               "SMS send failed");
      }
      return;
   }

   /* read_sms — validate index to prevent AT command injection */
   if (strcmp(action, "read_sms") == 0) {
      long idx;
      if (!validate_sms_index(value, &idx)) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_INDEX",
                               "SMS index must be 0-999");
         return;
      }
      char at_cmd[32];
      snprintf(at_cmd, sizeof(at_cmd), "AT+CMGR=%ld", idx);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, at_cmd, &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, resp.data, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc),
                               "Failed to read SMS");
      }
      return;
   }

   /* delete_sms — validate index to prevent AT command injection */
   if (strcmp(action, "delete_sms") == 0) {
      long idx;
      if (!validate_sms_index(value, &idx)) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_INDEX",
                               "SMS index must be 0-999");
         return;
      }
      char at_cmd[32];
      snprintf(at_cmd, sizeof(at_cmd), "AT+CMGD=%ld", idx);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, at_cmd, &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc),
                               "Failed to delete SMS");
      }
      return;
   }

   /* signal — use json-c for clean response JSON */
   if (strcmp(action, "signal") == 0) {
      int dbm, csq;
      if (modem_poll_signal(&g_at_ctx, &dbm, &csq) == 0) {
         struct json_object *val = json_object_new_object();
         json_object_object_add(val, "signal_dbm", json_object_new_int(dbm));
         json_object_object_add(val, "csq", json_object_new_int(csq));
         const char *val_str = json_object_to_json_string(val);
         mqtt_publish_response(action, request_id, true, val_str, NULL, NULL);
         json_object_put(val);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, "SIGNAL_ERROR",
                               "Failed to read signal");
      }
      return;
   }

   /* dtmf — validate character against DTMF set */
   if (strcmp(action, "dtmf") == 0) {
      if (!value || value[0] == '\0') {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_VALUE",
                               "DTMF digit required");
         return;
      }
      if (!validate_dtmf(value[0])) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_DTMF",
                               "DTMF must be 0-9, *, #, A-D");
         return;
      }
      char at_cmd[32];
      snprintf(at_cmd, sizeof(at_cmd), "AT+VTS=%c", value[0]);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, at_cmd, &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc), "DTMF failed");
      }
      return;
   }

   /* query_call */
   if (strcmp(action, "query_call") == 0) {
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, "AT+CLCC", &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, resp.data, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc),
                               "Call query failed");
      }
      return;
   }

   /* Unknown action */
   OLOG_WARNING("Unknown command action: %s", action);
   mqtt_publish_response(action, request_id, false, NULL, "UNKNOWN_ACTION",
                         "Action not recognized");
}

/* ── Usage / Version ─────────────────────────────────────────────────── */

static void print_version(void) {
   printf("ECHO (Enhanced Cellular Handling Operations) v%d.%d.%d\n", ECHO_VERSION_MAJOR,
          ECHO_VERSION_MINOR, ECHO_VERSION_PATCH);
   printf("Part of the OASIS (Operator Assistance and Situational Intelligence System)\n");
   printf("SIM7600G-H modem daemon\n");
}

static void print_usage(const char *prog) {
   printf("Usage: %s [options]\n\n", prog);
   printf("ECHO — SIM7600G-H modem daemon for OASIS\n\n");
   printf("Options:\n");
   printf("  -s, --serial-port PORT   Serial device (default: %s)\n", ECHO_DEFAULT_SERIAL_PORT);
   printf("  -b, --serial-baud BAUD   Baud rate (default: %d)\n", ECHO_DEFAULT_SERIAL_BAUD);
   printf("  -H, --mqtt-host HOST     MQTT broker (default: %s)\n", ECHO_DEFAULT_MQTT_HOST);
   printf("  -P, --mqtt-port PORT     MQTT port (default: %d)\n", ECHO_DEFAULT_MQTT_PORT);
   printf("      --mqtt-username USER MQTT username (or env MQTT_USERNAME)\n");
   printf("      --mqtt-password PASS MQTT password (or env MQTT_PASSWORD, preferred)\n");
   printf("      --mqtt-tls           Enable MQTT TLS\n");
   printf("      --mqtt-ca-cert PATH  CA certificate path (implies --mqtt-tls)\n");
   printf("  -e, --service            Run in service mode (syslog)\n");
   printf("  -h, --help               Show this help\n");
   printf("  -v, --version            Show version\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
   echo_config_t config;

   /* Defaults */
   snprintf(config.serial_port, sizeof(config.serial_port), "%s", ECHO_DEFAULT_SERIAL_PORT);
   config.serial_baud = ECHO_DEFAULT_SERIAL_BAUD;
   snprintf(config.mqtt_host, sizeof(config.mqtt_host), "%s", ECHO_DEFAULT_MQTT_HOST);
   config.mqtt_port = ECHO_DEFAULT_MQTT_PORT;
   config.mqtt_username[0] = '\0';
   config.mqtt_password[0] = '\0';
   config.mqtt_tls = 0;
   config.mqtt_ca_cert[0] = '\0';
   config.telemetry_interval_s = ECHO_DEFAULT_TELEMETRY_S;
   config.rate_limit_calls_per_hour = ECHO_DEFAULT_RATE_CALLS_H;
   config.rate_limit_sms_per_hour = ECHO_DEFAULT_RATE_SMS_H;
   config.service_mode = false;

   /* Environment variable overrides (for systemd EnvironmentFile) */
   const char *env;
   if ((env = getenv("MQTT_HOST"))) {
      snprintf(config.mqtt_host, sizeof(config.mqtt_host), "%s", env);
   }
   if ((env = getenv("MQTT_PORT"))) {
      config.mqtt_port = atoi(env);
   }
   if ((env = getenv("MQTT_USERNAME"))) {
      snprintf(config.mqtt_username, sizeof(config.mqtt_username), "%s", env);
   }
   if ((env = getenv("MQTT_PASSWORD"))) {
      snprintf(config.mqtt_password, sizeof(config.mqtt_password), "%s", env);
   }
   if ((env = getenv("MQTT_TLS"))) {
      config.mqtt_tls = (strcmp(env, "1") == 0 || strcmp(env, "true") == 0);
   }
   if ((env = getenv("MQTT_CA_CERT"))) {
      snprintf(config.mqtt_ca_cert, sizeof(config.mqtt_ca_cert), "%s", env);
   }
   if ((env = getenv("SERIAL_PORT"))) {
      snprintf(config.serial_port, sizeof(config.serial_port), "%s", env);
   }
   if ((env = getenv("SERIAL_BAUD"))) {
      config.serial_baud = atoi(env);
   }
   if ((env = getenv("TELEMETRY_INTERVAL"))) {
      config.telemetry_interval_s = atoi(env);
   }
   if ((env = getenv("RATE_LIMIT_CALLS_PER_HOUR"))) {
      config.rate_limit_calls_per_hour = atoi(env);
   }
   if ((env = getenv("RATE_LIMIT_SMS_PER_HOUR"))) {
      config.rate_limit_sms_per_hour = atoi(env);
   }

   /* Command-line overrides */
   static struct option long_options[] = {
      { "serial-port", required_argument, 0, 's' },
      { "serial-baud", required_argument, 0, 'b' },
      { "mqtt-host", required_argument, 0, 'H' },
      { "mqtt-port", required_argument, 0, 'P' },
      { "mqtt-username", required_argument, 0, 1000 },
      { "mqtt-password", required_argument, 0, 1001 },
      { "mqtt-tls", no_argument, 0, 1002 },
      { "mqtt-ca-cert", required_argument, 0, 1003 },
      { "service", no_argument, 0, 'e' },
      { "help", no_argument, 0, 'h' },
      { "version", no_argument, 0, 'v' },
      { 0, 0, 0, 0 },
   };

   int opt;
   while ((opt = getopt_long(argc, argv, "s:b:H:P:ehv", long_options, NULL)) != -1) {
      switch (opt) {
         case 's':
            snprintf(config.serial_port, sizeof(config.serial_port), "%s", optarg);
            break;
         case 'b':
            config.serial_baud = atoi(optarg);
            break;
         case 'H':
            snprintf(config.mqtt_host, sizeof(config.mqtt_host), "%s", optarg);
            break;
         case 'P':
            config.mqtt_port = atoi(optarg);
            break;
         case 1000:
            snprintf(config.mqtt_username, sizeof(config.mqtt_username), "%s", optarg);
            break;
         case 1001:
            snprintf(config.mqtt_password, sizeof(config.mqtt_password), "%s", optarg);
            break;
         case 1002:
            config.mqtt_tls = 1;
            break;
         case 1003:
            snprintf(config.mqtt_ca_cert, sizeof(config.mqtt_ca_cert), "%s", optarg);
            config.mqtt_tls = 1;
            break;
         case 'e':
            config.service_mode = true;
            break;
         case 'v':
            print_version();
            return 0;
         case 'h':
         default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
      }
   }

   /* Init logging */
   if (config.service_mode) {
      init_syslog("oasis-echo");
   } else {
      init_logging(NULL, LOG_TO_CONSOLE);
   }

   print_version();
   OLOG_INFO("Starting ECHO daemon...");
   OLOG_INFO("Serial: %s @ %d baud", config.serial_port, config.serial_baud);
   OLOG_INFO("MQTT: %s:%d (TLS: %s)", config.mqtt_host, config.mqtt_port,
             config.mqtt_tls ? "yes" : "no");

   /* Signal handlers */
   signal(SIGINT, signal_handler);
   signal(SIGTERM, signal_handler);

   /* Init command queue and rate limiters */
   cmd_queue_init(&g_cmd_queue);
   rate_bucket_init(&g_call_bucket, config.rate_limit_calls_per_hour);
   rate_bucket_init(&g_sms_bucket, config.rate_limit_sms_per_hour);

   /* Open serial port */
   if (at_open(&g_at_ctx, config.serial_port, config.serial_baud) != 0) {
      OLOG_ERROR("Failed to open serial port — exiting");
      close_logging();
      return 1;
   }

   /* Start URC reader thread */
   urc_context_t urc_ctx;
   if (urc_start(&urc_ctx, &g_at_ctx, on_urc_event, NULL) != 0) {
      OLOG_ERROR("Failed to start URC reader — exiting");
      at_close(&g_at_ctx);
      close_logging();
      return 1;
   }

   /* Run modem init sequence */
   if (modem_init(&g_at_ctx) != 0) {
      OLOG_ERROR("Modem init failed — exiting");
      urc_stop(&urc_ctx);
      at_close(&g_at_ctx);
      close_logging();
      return 1;
   }

   /* Connect MQTT */
   if (mqtt_comms_init(&config, on_mqtt_command, NULL) != 0) {
      OLOG_ERROR("MQTT init failed — exiting");
      urc_stop(&urc_ctx);
      at_close(&g_at_ctx);
      close_logging();
      return 1;
   }

   /* Publish online status */
   mqtt_publish_status_online();

   /* ── Main loop ──────────────────────────────────────────────────── */
   OLOG_INFO("ECHO daemon running. Press Ctrl+C to stop.");

   time_t last_telemetry = 0;
   time_t last_heartbeat = 0;
   time_t last_at_success = 0;
   int heartbeat_failures = 0;

   while (g_running) {
      time_t now = time(NULL);

      /* Drain command queue (MQTT commands + deferred events) */
      cmd_entry_t cmd;
      while (cmd_queue_pop(&g_cmd_queue, &cmd)) {
         if (cmd.type == CMD_TYPE_CMTI) {
            handle_cmti(cmd.sms_index);
         } else if (cmd.type == CMD_TYPE_CALL_CONNECTED) {
            modem_call_audio_setup(&g_at_ctx);
         } else {
            process_mqtt_command(&cmd);
         }
         last_at_success = time(NULL);
      }

      /* Telemetry polling */
      if (now - last_telemetry >= config.telemetry_interval_s) {
         modem_telemetry_t telem;
         if (modem_build_telemetry(&g_at_ctx, &telem, get_call_state()) == 0) {
            mqtt_publish_telemetry(&telem);
            last_at_success = now;
         }
         last_telemetry = now;
      }

      /* Heartbeat (every 30s, but skip if recent AT success) */
      if (now - last_heartbeat >= 30) {
         if (now - last_at_success < 30) {
            /* Telemetry or command already proved modem alive */
            heartbeat_failures = 0;
         } else if (modem_heartbeat(&g_at_ctx)) {
            heartbeat_failures = 0;
            last_at_success = now;
         } else {
            heartbeat_failures++;
            OLOG_WARNING("Modem heartbeat failed (%d consecutive)", heartbeat_failures);

            if (heartbeat_failures >= 3) {
               OLOG_ERROR("Modem unresponsive — publishing modem_lost event");
               char json[256];
               if (mqtt_build_event_json("modem_lost", NULL, json, sizeof(json)) >= 0) {
                  mqtt_publish_event(json);
               }
               set_call_state(CALL_STATE_IDLE);
               heartbeat_failures = 0;
            }
         }
         last_heartbeat = now;
      }

      /* Sleep 1 second between iterations */
      sleep(1);
   }

   /* ── Shutdown ───────────────────────────────────────────────────── */
   OLOG_INFO("Shutting down ECHO daemon...");

   mqtt_comms_cleanup();
   urc_stop(&urc_ctx);
   at_close(&g_at_ctx);
   close_logging();

   return 0;
}
