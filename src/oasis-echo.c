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
#include "sms_io.h"
#include "sms_reassembly.h"
#include "urc_handler.h"

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_signal_shutdown = 0;
static at_context_t g_at_ctx;
static rate_bucket_t g_call_bucket;
static rate_bucket_t g_sms_bucket;
static rate_bucket_t g_segment_bucket;
static sms_io_ctx_t g_sms_io;

/* Thread-safe call state using __atomic builtins (GCC) */
static call_state_t g_call_state = CALL_STATE_IDLE;

/* Ring timeout: if RINGING_IN persists this long after the last RING URC,
 * poll AT+CLCC to confirm. Modem sends RING every ~5s, so 10s gap means
 * the carrier forwarded to voicemail without a termination URC. */
#define RING_TIMEOUT_SEC 10
static time_t g_last_ring_time = 0;

static call_state_t get_call_state(void) {
   return __atomic_load_n(&g_call_state, __ATOMIC_ACQUIRE);
}

static void set_call_state(call_state_t state) {
   __atomic_store_n(&g_call_state, state, __ATOMIC_RELEASE);
}

static time_t get_last_ring_time(void) {
   return __atomic_load_n(&g_last_ring_time, __ATOMIC_ACQUIRE);
}

static void set_last_ring_time(time_t t) {
   __atomic_store_n(&g_last_ring_time, t, __ATOMIC_RELEASE);
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

/* Two queues so MQTT commands always drain ahead of deferred events. A
 * multi-segment SMS burst pushes up to 10 CMTIs onto the deferred queue;
 * without this split, a newly-arrived hangup/dial would wait behind those
 * CMGR+CMGD operations (several seconds in the worst case). Both queues
 * are single-producer / single-consumer: MQTT thread → g_cmd_queue; URC
 * reader thread → g_deferred_queue; both consumed by main. */
static cmd_queue_t g_cmd_queue;
static cmd_queue_t g_deferred_queue;

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
   g_signal_shutdown = 1;
   g_running = 0;
}

/* ── Rate limiter ────────────────────────────────────────────────────── */

void rate_bucket_init(rate_bucket_t *bucket, int max_per_hour) {
   memset(bucket, 0, sizeof(*bucket));
   bucket->max_per_hour = max_per_hour;
   bucket->tokens = (double)max_per_hour; /* start full */
   bucket->last_refill_sec = (int64_t)time(NULL);
}

static void rate_bucket_refill(rate_bucket_t *bucket) {
   int64_t now = (int64_t)time(NULL);
   int64_t elapsed = now - bucket->last_refill_sec;
   if (elapsed <= 0) {
      return;
   }
   double add = (double)elapsed * (double)bucket->max_per_hour / 3600.0;
   bucket->tokens += add;
   if (bucket->tokens > (double)bucket->max_per_hour) {
      bucket->tokens = (double)bucket->max_per_hour;
   }
   bucket->last_refill_sec = now;
}

bool rate_bucket_take_n(rate_bucket_t *bucket, int n) {
   if (!bucket || n <= 0) {
      return false;
   }
   rate_bucket_refill(bucket);
   if (bucket->tokens < (double)n) {
      return false;
   }
   bucket->tokens -= (double)n;
   return true;
}

bool rate_bucket_allow(rate_bucket_t *bucket) {
   return rate_bucket_take_n(bucket, 1);
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
         set_last_ring_time(time(NULL));
         /* Subsequent RINGs — publish lightweight ring event for MIRAGE/DAWN */
         if (get_call_state() == CALL_STATE_RINGING_IN) {
            if (mqtt_build_event_json("ring", NULL, json, sizeof(json)) >= 0) {
               mqtt_publish_event(json);
            }
            break;
         }
         set_call_state(CALL_STATE_RINGING_IN);
         struct json_object *extra = json_object_new_object();
         json_object_object_add(extra, "number",
                                json_object_new_string(event->number[0] ? event->number : ""));
         if (mqtt_build_event_json("incoming_call", extra, json, sizeof(json)) >= 0) {
            mqtt_publish_event(json);
         }
         json_object_put(extra);
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
         cmd_queue_push(&g_deferred_queue, &audio_cmd);
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
         set_last_ring_time(0);
         const char *reason = (prev == CALL_STATE_DIALING) ? "no_carrier" : "remote_hangup";
         struct json_object *extra = json_object_new_object();
         json_object_object_add(extra, "reason", json_object_new_string(reason));
         if (mqtt_build_event_json("call_ended", extra, json, sizeof(json)) >= 0) {
            mqtt_publish_event(json);
         }
         json_object_put(extra);
         OLOG_INFO("Call ended: %s", reason);
         break;
      }

      case URC_BUSY:
         if (get_call_state() == CALL_STATE_IDLE) {
            break;
         }
         set_call_state(CALL_STATE_IDLE);
         {
            struct json_object *extra = json_object_new_object();
            json_object_object_add(extra, "reason", json_object_new_string("busy"));
            if (mqtt_build_event_json("call_ended", extra, json, sizeof(json)) >= 0) {
               mqtt_publish_event(json);
            }
            json_object_put(extra);
         }
         OLOG_INFO("Call ended: busy");
         break;

      case URC_NO_ANSWER:
         if (get_call_state() == CALL_STATE_IDLE) {
            break;
         }
         set_call_state(CALL_STATE_IDLE);
         {
            struct json_object *extra = json_object_new_object();
            json_object_object_add(extra, "reason", json_object_new_string("no_answer"));
            if (mqtt_build_event_json("call_ended", extra, json, sizeof(json)) >= 0) {
               mqtt_publish_event(json);
            }
            json_object_put(extra);
         }
         OLOG_INFO("Call ended: no answer");
         break;

      case URC_VOICE_CALL_END:
         if (get_call_state() == CALL_STATE_IDLE) {
            break;
         }
         set_call_state(CALL_STATE_IDLE);
         {
            struct json_object *extra = json_object_new_object();
            json_object_object_add(extra, "reason", json_object_new_string("voice_call_end"));
            if (mqtt_build_event_json("call_ended", extra, json, sizeof(json)) >= 0) {
               mqtt_publish_event(json);
            }
            json_object_put(extra);
         }
         OLOG_INFO("Call ended (VOICE CALL: END)");
         break;

      case URC_CMTI: {
         /* Defer SMS read to main thread — calling at_command_send from
          * the URC reader would deadlock (reader waits for itself).
          * Deduplicate: SIM7600 sometimes sends duplicate +CMTI for same index. */
         static int last_cmti_index = -1;
         static time_t last_cmti_time = 0;
         time_t now_t = time(NULL);
         if (event->index == last_cmti_index && (now_t - last_cmti_time) < 3) {
            OLOG_INFO("Suppressing duplicate CMTI for index %d", event->index);
            break;
         }
         last_cmti_index = event->index;
         last_cmti_time = now_t;

         OLOG_INFO("New SMS at index %d, deferring read to main loop", event->index);
         cmd_entry_t cmd;
         memset(&cmd, 0, sizeof(cmd));
         cmd.type = CMD_TYPE_CMTI;
         cmd.sms_index = event->index;
         if (!cmd_queue_push(&g_deferred_queue, &cmd)) {
            OLOG_WARNING("Deferred queue full, dropping CMTI event for index %d", event->index);
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
   sms_io_handle_cmti(&g_sms_io, sms_index);
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
                            "Command queue full, try again", NULL);
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
                               "Phone number validation failed", NULL);
         return;
      }
      if (!rate_bucket_allow(&g_call_bucket)) {
         mqtt_publish_response(action, request_id, false, NULL, "RATE_LIMITED",
                               "Call rate limit exceeded", NULL);
         return;
      }

      /* value is validated <= PHONE_NUMBER_MAX by sms_validate_number above; the
       * bounded precision lets -Wformat-truncation prove no truncation and
       * hard-caps the dial string regardless of the source buffer size. */
      char at_cmd[64];
      snprintf(at_cmd, sizeof(at_cmd), "ATD%.*s;", PHONE_NUMBER_MAX, value);
      at_status_t rc = at_command_send_async(&g_at_ctx, at_cmd);
      if (rc == AT_OK) {
         set_call_state(CALL_STATE_DIALING);
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, "AT_ERROR",
                               "Failed to send dial command", NULL);
      }
      return;
   }

   /* answer */
   if (strcmp(action, "answer") == 0) {
      at_status_t rc = at_command_send_async(&g_at_ctx, "ATA");
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, "AT_ERROR",
                               "Failed to send answer command", NULL);
      }
      return;
   }

   /* hangup — AT+CHUP works in all call states on SIM7600 (ATH does not) */
   if (strcmp(action, "hangup") == 0) {
      call_state_t prev = get_call_state();
      set_call_state(CALL_STATE_HANGING_UP);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, "AT+CHUP", &resp, AT_TIMEOUT_DEFAULT);
      set_call_state(CALL_STATE_IDLE);
      set_last_ring_time(0);
      /* AT+CHUP may return OK or NO CARRIER — both mean the call ended */
      if (rc == AT_OK || rc == AT_NO_CARRIER) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL, NULL);
         /* Publish call_ended directly — don't rely on URC which we'll suppress
          * since state is already IDLE by the time it arrives. */
         if (prev != CALL_STATE_IDLE) {
            char json[1024];
            struct json_object *extra = json_object_new_object();
            json_object_object_add(extra, "reason", json_object_new_string("local_hangup"));
            json_object_object_add(extra, "duration", json_object_new_int(0));
            if (mqtt_build_event_json("call_ended", extra, json, sizeof(json)) >= 0) {
               mqtt_publish_event(json);
            }
            json_object_put(extra);
            OLOG_INFO("Call ended: local hangup");
         }
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc), "Hangup failed",
                               NULL);
      }
      return;
   }

   /* send_sms — delegate encoding + transmission + response to sms_io. */
   if (strcmp(action, "send_sms") == 0) {
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
      sms_io_send_and_respond(&g_sms_io, value, body, action, request_id);
      return;
   }

   /* read_sms — validate index to prevent AT command injection */
   if (strcmp(action, "read_sms") == 0) {
      long idx;
      if (!validate_sms_index(value, &idx)) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_INDEX",
                               "SMS index must be 0-999", NULL);
         return;
      }
      char at_cmd[32];
      snprintf(at_cmd, sizeof(at_cmd), "AT+CMGR=%ld", idx);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, at_cmd, &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, resp.data, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc),
                               "Failed to read SMS", NULL);
      }
      return;
   }

   /* delete_sms — validate index to prevent AT command injection */
   if (strcmp(action, "delete_sms") == 0) {
      long idx;
      if (!validate_sms_index(value, &idx)) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_INDEX",
                               "SMS index must be 0-999", NULL);
         return;
      }
      char at_cmd[32];
      snprintf(at_cmd, sizeof(at_cmd), "AT+CMGD=%ld", idx);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, at_cmd, &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc),
                               "Failed to delete SMS", NULL);
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
         mqtt_publish_response(action, request_id, true, val_str, NULL, NULL, NULL);
         json_object_put(val);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, "SIGNAL_ERROR",
                               "Failed to read signal", NULL);
      }
      return;
   }

   /* dtmf — validate character against DTMF set */
   if (strcmp(action, "dtmf") == 0) {
      if (!value || value[0] == '\0') {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_VALUE",
                               "DTMF digit required", NULL);
         return;
      }
      if (!validate_dtmf(value[0])) {
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_DTMF",
                               "DTMF must be 0-9, *, #, A-D", NULL);
         return;
      }
      char at_cmd[32];
      snprintf(at_cmd, sizeof(at_cmd), "AT+VTS=%c", value[0]);
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, at_cmd, &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc), "DTMF failed",
                               NULL);
      }
      return;
   }

   /* query_call */
   if (strcmp(action, "query_call") == 0) {
      at_response_t resp;
      at_status_t rc = at_command_send(&g_at_ctx, "AT+CLCC", &resp, AT_TIMEOUT_DEFAULT);
      if (rc == AT_OK) {
         mqtt_publish_response(action, request_id, true, resp.data, NULL, NULL, NULL);
      } else {
         mqtt_publish_response(action, request_id, false, NULL, at_status_str(rc),
                               "Call query failed", NULL);
      }
      return;
   }

   /* Unknown action */
   OLOG_WARNING("Unknown command action: %s", action);
   mqtt_publish_response(action, request_id, false, NULL, "UNKNOWN_ACTION", "Action not recognized",
                         NULL);
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
   printf("      --legacy-sms         Use legacy text-mode SMS (AT+CMGF=1), no concat\n");
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
   config.rate_limit_segments_per_hour = ECHO_DEFAULT_RATE_SEGMENTS_H;
   config.inter_segment_delay_ms = ECHO_DEFAULT_SEGMENT_DELAY_MS;
   config.pdu_mode = true;
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
   if ((env = getenv("RATE_LIMIT_SEGMENTS_PER_HOUR"))) {
      config.rate_limit_segments_per_hour = atoi(env);
   }
   if ((env = getenv("INTER_SEGMENT_DELAY_MS"))) {
      config.inter_segment_delay_ms = atoi(env);
   }
   if ((env = getenv("PDU_MODE"))) {
      config.pdu_mode = !(strcmp(env, "0") == 0 || strcmp(env, "false") == 0);
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
      { "legacy-sms", no_argument, 0, 1004 },
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
         case 1004:
            config.pdu_mode = false;
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

   /* Init command queues and rate limiters */
   cmd_queue_init(&g_cmd_queue);
   cmd_queue_init(&g_deferred_queue);
   rate_bucket_init(&g_call_bucket, config.rate_limit_calls_per_hour);
   rate_bucket_init(&g_sms_bucket, config.rate_limit_sms_per_hour);
   rate_bucket_init(&g_segment_bucket, config.rate_limit_segments_per_hour);
   sms_reassembly_reset();

   g_sms_io.at = &g_at_ctx;
   g_sms_io.msg_bucket = &g_sms_bucket;
   g_sms_io.segment_bucket = &g_segment_bucket;
   g_sms_io.inter_segment_delay_ms = config.inter_segment_delay_ms;
   g_sms_io.pdu_mode = config.pdu_mode;

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
   urc_ctx.shutdown_flag = &g_running;

   /* Run modem init sequence */
   if (modem_init(&g_at_ctx, config.pdu_mode) != 0) {
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

      /* Drain MQTT commands first so a newly-arrived hangup/dial never waits
       * behind a queued CMTI burst (a 10-segment inbound SMS generates 10
       * CMTIs, each holding the main thread for a CMGR+CMGD round-trip). */
      cmd_entry_t cmd;
      while (cmd_queue_pop(&g_cmd_queue, &cmd)) {
         process_mqtt_command(&cmd);
         last_at_success = time(NULL);
      }

      /* Then drain one deferred event per tick. Re-checking `g_cmd_queue`
       * between each ensures a command arriving mid-burst still jumps
       * ahead of the remaining CMTIs. */
      while (cmd_queue_pop(&g_deferred_queue, &cmd)) {
         if (cmd.type == CMD_TYPE_CMTI) {
            handle_cmti(cmd.sms_index);
         } else if (cmd.type == CMD_TYPE_CALL_CONNECTED) {
            /* Arm echo cancel + USB PCM (per-call AT commands).  Only announce
             * pcm_ready once CPCMREG=1 succeeded, so DAWN opens ttyUSB4 exactly
             * when audio is flowing — no startup race. */
            if (modem_call_audio_setup(&g_at_ctx)) {
               char json[256];
               if (mqtt_build_event_json("pcm_ready", NULL, json, sizeof(json)) >= 0) {
                  mqtt_publish_event(json);
               }
            }
         }
         last_at_success = time(NULL);

         /* Yield back to MQTT between deferred events. */
         cmd_entry_t mqtt_cmd;
         while (cmd_queue_pop(&g_cmd_queue, &mqtt_cmd)) {
            process_mqtt_command(&mqtt_cmd);
            last_at_success = time(NULL);
         }
      }

      /* Ring timeout watchdog — if ringing for too long without a termination URC,
       * poll the modem to confirm call state before transitioning to idle. */
      time_t last_ring = get_last_ring_time();
      if (get_call_state() == CALL_STATE_RINGING_IN && last_ring > 0 &&
          (now - last_ring) >= RING_TIMEOUT_SEC) {
         /* Ask the modem if any calls are active (AT+CLCC lists current calls) */
         at_response_t clcc_resp;
         at_status_t clcc_rc = at_command_send(&g_at_ctx, "AT+CLCC", &clcc_resp, 3000);
         if (clcc_rc == AT_OK && strstr(clcc_resp.data, "+CLCC:") == NULL) {
            /* Modem confirms no active calls — carrier forwarded to voicemail */
            set_call_state(CALL_STATE_IDLE);
            set_last_ring_time(0);
            char json[1024];
            struct json_object *extra = json_object_new_object();
            json_object_object_add(extra, "reason", json_object_new_string("ring_timeout"));
            if (mqtt_build_event_json("call_ended", extra, json, sizeof(json)) >= 0) {
               mqtt_publish_event(json);
            }
            json_object_put(extra);
            OLOG_INFO("Ring timeout: modem confirms no active calls after %ds", RING_TIMEOUT_SEC);
         } else if (clcc_rc == AT_OK) {
            /* Call still active on modem — extend the timeout */
            set_last_ring_time(now);
            OLOG_INFO("Ring timeout check: modem reports call still active, extending");
         } else {
            /* AT command failed — extend timeout, don't make assumptions */
            set_last_ring_time(now);
            OLOG_WARNING("Ring timeout check: AT+CLCC failed (rc=%d), extending", clcc_rc);
         }
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

      /* Reassembly sweep — clears timed-out slots even during idle periods
       * so the 10-min TTL behavior is deterministic regardless of traffic. */
      sms_reassembly_sweep(now);

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

   if (!g_signal_shutdown) {
      OLOG_ERROR("Exiting due to serial device failure (exit code 1)");
   }

   close_logging();

   return g_signal_shutdown ? 0 : 1;
}
