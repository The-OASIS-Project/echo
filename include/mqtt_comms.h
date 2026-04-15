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
 * MQTT communications — connect, publish, subscribe, command dispatch.
 * All messages conform to OCP v1.4.
 */

#ifndef MQTT_COMMS_H
#define MQTT_COMMS_H

#include <json-c/json.h>
#include <stdbool.h>

#include "at_command.h"
#include "echo.h"

/* MQTT topics */
#define MQTT_TOPIC_TELEMETRY "echo/telemetry"
#define MQTT_TOPIC_EVENTS "echo/events"
#define MQTT_TOPIC_RESPONSE "echo/response"
#define MQTT_TOPIC_STATUS "echo/status"
#define MQTT_TOPIC_CMD "echo/cmd"

/**
 * @brief Command handler callback — called when a command arrives on echo/cmd.
 *
 * The handler is responsible for executing the command and calling
 * mqtt_publish_response() with the result.
 */
typedef void (*mqtt_cmd_handler_t)(const char *action,
                                   const char *value,
                                   const char *request_id,
                                   const char *data_json,
                                   void *userdata);

/**
 * @brief Initialize the MQTT connection.
 *
 * Connects to the broker, sets LWT on echo/status, subscribes to echo/cmd.
 *
 * @param config   ECHO configuration.
 * @param handler  Command handler callback.
 * @param userdata Opaque pointer passed to handler.
 * @return 0 on success, -1 on error.
 */
int mqtt_comms_init(const echo_config_t *config, mqtt_cmd_handler_t handler, void *userdata);

/**
 * @brief Publish telemetry data (echo/telemetry, QoS 0, retained).
 */
int mqtt_publish_telemetry(const modem_telemetry_t *telem);

/**
 * @brief Publish an event (echo/events, QoS 1, not retained).
 *
 * @param event_json  Complete JSON string for the event.
 */
int mqtt_publish_event(const char *event_json);

/**
 * @brief Publish a command response (echo/response, QoS 1, not retained).
 *
 * @param action     The action that was requested.
 * @param request_id The request_id from the command.
 * @param success    true = success, false = error.
 * @param value      Optional value string (for success responses).
 * @param err_code   Error code string (for error responses, e.g. "NO_CARRIER").
 * @param err_msg    Error message string (for error responses).
 */
int mqtt_publish_response(const char *action,
                          const char *request_id,
                          bool success,
                          const char *value,
                          const char *err_code,
                          const char *err_msg);

/**
 * @brief Publish online status (echo/status, QoS 1, retained).
 */
int mqtt_publish_status_online(void);

/**
 * @brief Clean up MQTT resources.
 */
void mqtt_comms_cleanup(void);

/**
 * @brief Build a telemetry JSON string from a telemetry snapshot.
 *
 * Public for unit testing.
 *
 * @param telem  Telemetry snapshot.
 * @param buf    Output buffer.
 * @param size   Buffer size.
 * @return Length of JSON string, or -1 on error.
 */
int mqtt_build_telemetry_json(const modem_telemetry_t *telem, char *buf, size_t size);

/**
 * @brief Build an event JSON string (OCP v1.4).
 *
 * @param event_type  Event name (e.g., "incoming_call", "call_ended").
 * @param extra       Additional fields to merge (NULL for none, caller retains ownership).
 * @param buf         Output buffer.
 * @param size        Buffer size.
 * @return Length of JSON string, or -1 on error.
 *
 * Public for unit testing.
 */
int mqtt_build_event_json(const char *event_type,
                          struct json_object *extra,
                          char *buf,
                          size_t size);

/**
 * @brief Build a response JSON string.
 *
 * Public for unit testing.
 */
int mqtt_build_response_json(const char *action,
                             const char *request_id,
                             bool success,
                             const char *value,
                             const char *err_code,
                             const char *err_msg,
                             char *buf,
                             size_t size);

/**
 * @brief Parse a command JSON from echo/cmd.
 *
 * Public for unit testing.
 *
 * @param json       Input JSON string.
 * @param action     Output: action string.
 * @param action_sz  Size of action buffer.
 * @param value      Output: value string (may be empty).
 * @param value_sz   Size of value buffer.
 * @param req_id     Output: request_id string.
 * @param req_id_sz  Size of request_id buffer.
 * @param data_json  Output: "data" object as JSON string (may be empty).
 * @param data_sz    Size of data buffer.
 * @return 0 on success, -1 on parse error.
 */
int mqtt_parse_command(const char *json,
                       char *action,
                       size_t action_sz,
                       char *value,
                       size_t value_sz,
                       char *req_id,
                       size_t req_id_sz,
                       char *data_json,
                       size_t data_sz);

#endif /* MQTT_COMMS_H */
