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
 * MQTT communications — publish telemetry/events/responses,
 * subscribe to commands, OCP v1.4 JSON formatting.
 */

#include "mqtt_comms.h"

#include <inttypes.h>
#include <json-c/json.h>
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"

/* ── Static state ────────────────────────────────────────────────────── */

static struct mosquitto *mosq = NULL;
static bool mqtt_initialized = false;
static mqtt_cmd_handler_t cmd_handler = NULL;
static void *cmd_userdata = NULL;

/* ── Registration state names ────────────────────────────────────────── */

static const char *reg_state_names[] = {
   [REG_NOT_REGISTERED] = "not_registered",
   [REG_REGISTERED_HOME] = "registered_home",
   [REG_SEARCHING] = "searching",
   [REG_DENIED] = "denied",
   [REG_UNKNOWN] = "unknown",
   [REG_REGISTERED_ROAMING] = "registered_roaming",
};

static const char *call_state_names[] = {
   [CALL_STATE_IDLE] = "idle",
   [CALL_STATE_DIALING] = "dialing",
   [CALL_STATE_RINGING_IN] = "ringing_in",
   [CALL_STATE_ACTIVE] = "active",
   [CALL_STATE_HANGING_UP] = "hanging_up",
};

static const char *sim_status_names[] = {
   [SIM_UNKNOWN] = "unknown",  [SIM_READY] = "ready",          [SIM_PIN_REQUIRED] = "pin",
   [SIM_PUK_REQUIRED] = "puk", [SIM_NOT_INSERTED] = "missing", [SIM_ERROR] = "error",
};

/* ── Timestamp ───────────────────────────────────────────────────────── */

static int64_t get_timestamp(void) {
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── JSON builders (public for testing) ──────────────────────────────── */

int mqtt_build_telemetry_json(const modem_telemetry_t *telem, char *buf, size_t size) {
   if (!telem || !buf || size == 0) {
      return -1;
   }

   const char *reg_str = (telem->reg >= 0 && telem->reg <= REG_REGISTERED_ROAMING)
                             ? reg_state_names[telem->reg]
                             : "unknown";
   const char *call_str = (telem->call_state >= 0 && telem->call_state <= CALL_STATE_HANGING_UP)
                              ? call_state_names[telem->call_state]
                              : "idle";
   const char *sim_str = (telem->sim >= 0 && telem->sim <= SIM_ERROR) ? sim_status_names[telem->sim]
                                                                      : "unknown";

   struct json_object *obj = json_object_new_object();
   json_object_object_add(obj, "device", json_object_new_string("echo"));
   json_object_object_add(obj, "msg_type", json_object_new_string("telemetry"));
   json_object_object_add(obj, "signal_dbm", json_object_new_int(telem->signal_dbm));
   json_object_object_add(obj, "signal_bars", json_object_new_int(telem->signal_bars));
   json_object_object_add(obj, "csq", json_object_new_int(telem->csq));
   json_object_object_add(obj, "registration", json_object_new_string(reg_str));
   json_object_object_add(obj, "operator", json_object_new_string(telem->operator_name));
   json_object_object_add(obj, "network_type", json_object_new_string(telem->network_type));
   json_object_object_add(obj, "call_state", json_object_new_string(call_str));
   json_object_object_add(obj, "sim_status", json_object_new_string(sim_str));
   json_object_object_add(obj, "timestamp", json_object_new_int64(get_timestamp()));

   const char *json_str = json_object_to_json_string(obj);
   int len = (int)strlen(json_str);
   if (len <= 0 || (size_t)len >= size) {
      json_object_put(obj);
      return -1;
   }
   memcpy(buf, json_str, (size_t)len + 1);
   json_object_put(obj);

   return len;
}

int mqtt_build_event_json(const char *event_type,
                          struct json_object *extra,
                          char *buf,
                          size_t size) {
   if (!event_type || !buf || size == 0) {
      return -1;
   }

   struct json_object *obj = json_object_new_object();
   json_object_object_add(obj, "device", json_object_new_string("echo"));
   json_object_object_add(obj, "msg_type", json_object_new_string("event"));
   json_object_object_add(obj, "event", json_object_new_string(event_type));

   /* Merge extra fields (caller retains ownership) */
   if (extra) {
      json_object_object_foreach(extra, key, val) {
         json_object_object_add(obj, key, json_object_get(val));
      }
   }

   json_object_object_add(obj, "timestamp", json_object_new_int64(get_timestamp()));

   const char *json_str = json_object_to_json_string(obj);
   int len = (int)strlen(json_str);
   if (len <= 0 || (size_t)len >= size) {
      json_object_put(obj);
      return -1;
   }
   memcpy(buf, json_str, (size_t)len + 1);
   json_object_put(obj);

   return len;
}

int mqtt_build_response_json(const char *action,
                             const char *request_id,
                             bool success,
                             const char *value,
                             const char *err_code,
                             const char *err_msg,
                             const char *data_json,
                             char *buf,
                             size_t size) {
   if (!action || !request_id || !buf || size == 0) {
      return -1;
   }

   struct json_object *obj = json_object_new_object();
   json_object_object_add(obj, "device", json_object_new_string("echo"));
   json_object_object_add(obj, "action", json_object_new_string(action));
   json_object_object_add(obj, "request_id", json_object_new_string(request_id));

   if (success) {
      json_object_object_add(obj, "status", json_object_new_string("success"));
      if (value && value[0] != '\0') {
         json_object_object_add(obj, "value", json_object_new_string(value));
      }
   } else {
      json_object_object_add(obj, "status", json_object_new_string("error"));
      struct json_object *err = json_object_new_object();
      json_object_object_add(err, "code", json_object_new_string(err_code ? err_code : "UNKNOWN"));
      json_object_object_add(err, "message",
                             json_object_new_string(err_msg ? err_msg : "Unknown error"));
      json_object_object_add(obj, "error", err);
   }

   /* Optional caller-provided data object. Parse it here so the published
    * payload is a real nested object instead of a quoted string blob. Bad
    * input is dropped with a warning — we don't want a caller bug to eat
    * the whole response. */
   if (data_json && data_json[0] != '\0') {
      struct json_object *data = json_tokener_parse(data_json);
      if (data) {
         json_object_object_add(obj, "data", data);
      } else {
         OLOG_WARNING("mqtt_build_response_json: malformed data_json dropped");
      }
   }

   json_object_object_add(obj, "timestamp", json_object_new_int64(get_timestamp()));

   const char *json_str = json_object_to_json_string(obj);
   int len = (int)strlen(json_str);
   if (len <= 0 || (size_t)len >= size) {
      json_object_put(obj);
      return -1;
   }
   memcpy(buf, json_str, (size_t)len + 1);
   json_object_put(obj);

   return len;
}

/* ── Command parser (public for testing) ────────────────���────────────── */

int mqtt_parse_command(const char *json,
                       char *action,
                       size_t action_sz,
                       char *value,
                       size_t value_sz,
                       char *req_id,
                       size_t req_id_sz,
                       char *data_json,
                       size_t data_sz) {
   if (!json || !action || !req_id) {
      return -1;
   }

   action[0] = '\0';
   if (value) {
      value[0] = '\0';
   }
   req_id[0] = '\0';
   if (data_json) {
      data_json[0] = '\0';
   }

   struct json_object *root = json_tokener_parse(json);
   if (!root) {
      OLOG_ERROR("Failed to parse command JSON");
      return -1;
   }

   struct json_object *j_action, *j_value, *j_reqid, *j_data;

   if (json_object_object_get_ex(root, "action", &j_action)) {
      snprintf(action, action_sz, "%s", json_object_get_string(j_action));
   }
   if (json_object_object_get_ex(root, "value", &j_value) && value) {
      snprintf(value, value_sz, "%s", json_object_get_string(j_value));
   }
   if (json_object_object_get_ex(root, "request_id", &j_reqid)) {
      snprintf(req_id, req_id_sz, "%s", json_object_get_string(j_reqid));
   }
   if (json_object_object_get_ex(root, "data", &j_data) && data_json) {
      snprintf(data_json, data_sz, "%s", json_object_to_json_string(j_data));
   }

   json_object_put(root);

   if (action[0] == '\0' || req_id[0] == '\0') {
      OLOG_ERROR("Command missing required fields (action, request_id)");
      return -1;
   }

   return 0;
}

/* ── MQTT callbacks ──────────────────────────────────────────────────── */

static void on_connect(struct mosquitto *m, void *obj, int rc) {
   (void)obj;
   if (rc != 0) {
      OLOG_ERROR("MQTT connection failed: %s", mosquitto_strerror(rc));
      return;
   }
   OLOG_INFO("MQTT: Connected to broker");

   /* Subscribe to command topic */
   int sub_rc = mosquitto_subscribe(m, NULL, MQTT_TOPIC_CMD, 1);
   if (sub_rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("MQTT: Failed to subscribe to %s: %s", MQTT_TOPIC_CMD, mosquitto_strerror(sub_rc));
   } else {
      OLOG_INFO("MQTT: Subscribed to %s", MQTT_TOPIC_CMD);
   }
}

static void on_disconnect(struct mosquitto *m, void *obj, int rc) {
   (void)m;
   (void)obj;
   if (mqtt_initialized) {
      OLOG_ERROR("MQTT: Disconnected: %s", mosquitto_strerror(rc));
   }
}

static void on_message(struct mosquitto *m, void *obj, const struct mosquitto_message *msg) {
   (void)m;
   (void)obj;

   if (!msg || !msg->payload || msg->payloadlen == 0) {
      return;
   }
   if (strcmp(msg->topic, MQTT_TOPIC_CMD) != 0) {
      return;
   }

   /* Parse the command */
   char action[64], value[256], req_id[64], data[1024];
   if (mqtt_parse_command((const char *)msg->payload, action, sizeof(action), value, sizeof(value),
                          req_id, sizeof(req_id), data, sizeof(data)) != 0) {
      OLOG_WARNING("MQTT: Invalid command received");
      return;
   }

   OLOG_INFO("MQTT: Command received: action=%s request_id=%s", action, req_id);

   /* Dispatch to handler */
   if (cmd_handler) {
      cmd_handler(action, value, req_id, data, cmd_userdata);
   }
}

/* ── Public API ──────────────────────────────────────────────────────── */

int mqtt_comms_init(const echo_config_t *config, mqtt_cmd_handler_t handler, void *userdata) {
   if (!config) {
      return -1;
   }

   cmd_handler = handler;
   cmd_userdata = userdata;

   mosquitto_lib_init();

   mosq = mosquitto_new("oasis-echo", true, NULL);
   if (!mosq) {
      OLOG_ERROR("MQTT: Failed to create client");
      return -1;
   }

   mosquitto_connect_callback_set(mosq, on_connect);
   mosquitto_disconnect_callback_set(mosq, on_disconnect);
   mosquitto_message_callback_set(mosq, on_message);
   mosquitto_reconnect_delay_set(mosq, 2, 30, true);

   /* Authentication */
   if (config->mqtt_username[0] != '\0') {
      const char *pw = config->mqtt_password[0] != '\0' ? config->mqtt_password : NULL;
      int rc = mosquitto_username_pw_set(mosq, config->mqtt_username, pw);
      if (rc != MOSQ_ERR_SUCCESS) {
         OLOG_ERROR("MQTT: Failed to set credentials: %s", mosquitto_strerror(rc));
      }
   }

   /* TLS */
   if (config->mqtt_tls) {
      const char *ca = config->mqtt_ca_cert[0] != '\0' ? config->mqtt_ca_cert : NULL;
      int rc = mosquitto_tls_set(mosq, ca, NULL, NULL, NULL, NULL);
      if (rc != MOSQ_ERR_SUCCESS) {
         OLOG_ERROR("MQTT: TLS setup failed: %s", mosquitto_strerror(rc));
         mosquitto_destroy(mosq);
         mosq = NULL;
         return -1;
      }
      OLOG_INFO("MQTT: TLS enabled (CA: %s)", ca ? ca : "system");
   }

   /* Last Will and Testament — offline status (timestamp:0 since broker sends it) */
   struct json_object *lwt_obj = json_object_new_object();
   if (lwt_obj) {
      json_object_object_add(lwt_obj, "device", json_object_new_string("echo"));
      json_object_object_add(lwt_obj, "msg_type", json_object_new_string("status"));
      json_object_object_add(lwt_obj, "status", json_object_new_string("offline"));
      json_object_object_add(lwt_obj, "timestamp", json_object_new_int64(0));
      const char *lwt_str = json_object_to_json_string(lwt_obj);
      mosquitto_will_set(mosq, MQTT_TOPIC_STATUS, (int)strlen(lwt_str), lwt_str, 1, true);
      json_object_put(lwt_obj);
   }

   /* Connect */
   int rc = mosquitto_connect(mosq, config->mqtt_host, config->mqtt_port, 60);
   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("MQTT: Connect failed: %s", mosquitto_strerror(rc));
      mosquitto_destroy(mosq);
      mosq = NULL;
      return -1;
   }

   /* Start background loop thread */
   rc = mosquitto_loop_start(mosq);
   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("MQTT: Loop start failed: %s", mosquitto_strerror(rc));
      mosquitto_disconnect(mosq);
      mosquitto_destroy(mosq);
      mosq = NULL;
      return -1;
   }

   mqtt_initialized = true;
   OLOG_INFO("MQTT: Initialized (host=%s port=%d)", config->mqtt_host, config->mqtt_port);
   return 0;
}

int mqtt_publish_telemetry(const modem_telemetry_t *telem) {
   if (!mosq || !mqtt_initialized || !telem) {
      return -1;
   }

   char buf[1024];
   if (mqtt_build_telemetry_json(telem, buf, sizeof(buf)) < 0) {
      return -1;
   }

   return mosquitto_publish(mosq, NULL, MQTT_TOPIC_TELEMETRY, (int)strlen(buf), buf, 0, true);
}

int mqtt_publish_event(const char *event_json) {
   if (!mosq || !mqtt_initialized || !event_json) {
      return -1;
   }

   return mosquitto_publish(mosq, NULL, MQTT_TOPIC_EVENTS, (int)strlen(event_json), event_json, 1,
                            false);
}

int mqtt_publish_response(const char *action,
                          const char *request_id,
                          bool success,
                          const char *value,
                          const char *err_code,
                          const char *err_msg,
                          const char *data_json) {
   if (!mosq || !mqtt_initialized) {
      return -1;
   }

   char buf[1024];
   if (mqtt_build_response_json(action, request_id, success, value, err_code, err_msg, data_json,
                                buf, sizeof(buf)) < 0) {
      return -1;
   }

   return mosquitto_publish(mosq, NULL, MQTT_TOPIC_RESPONSE, (int)strlen(buf), buf, 1, false);
}

int mqtt_publish_status_online(void) {
   if (!mosq || !mqtt_initialized) {
      return -1;
   }

   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return -1;
   }
   json_object_object_add(obj, "device", json_object_new_string("echo"));
   json_object_object_add(obj, "msg_type", json_object_new_string("status"));
   json_object_object_add(obj, "status", json_object_new_string("online"));
   json_object_object_add(obj, "timestamp", json_object_new_int64(get_timestamp()));

   const char *json_str = json_object_to_json_string(obj);
   int rc = mosquitto_publish(mosq, NULL, MQTT_TOPIC_STATUS, (int)strlen(json_str), json_str, 1,
                              true);
   json_object_put(obj);
   return rc;
}

void mqtt_comms_cleanup(void) {
   if (mosq) {
      if (mqtt_initialized) {
         /* Publish offline before disconnecting */
         struct json_object *obj = json_object_new_object();
         if (obj) {
            json_object_object_add(obj, "device", json_object_new_string("echo"));
            json_object_object_add(obj, "msg_type", json_object_new_string("status"));
            json_object_object_add(obj, "status", json_object_new_string("offline"));
            json_object_object_add(obj, "timestamp", json_object_new_int64(get_timestamp()));
            const char *json_str = json_object_to_json_string(obj);
            mosquitto_publish(mosq, NULL, MQTT_TOPIC_STATUS, (int)strlen(json_str), json_str, 1,
                              true);
            json_object_put(obj);
         }

         mosquitto_disconnect(mosq);
         mosquitto_loop_stop(mosq, false);
      }
      mosquitto_destroy(mosq);
      mosq = NULL;
   }
   mosquitto_lib_cleanup();
   mqtt_initialized = false;
   OLOG_INFO("MQTT: Cleaned up");
}
