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
 * SMS send / inbound-dispatch orchestration. Extracted from oasis-echo.c so
 * the main daemon stays under the 1,500-line soft limit and the PDU path
 * can be unit-tested end-to-end.
 */

#ifndef SMS_IO_H
#define SMS_IO_H

#include <stdbool.h>
#include <stddef.h>

#include "at_command.h"
#include "echo.h"

typedef enum {
   SMS_IO_SEND_OK = 0,
   SMS_IO_SEND_INVALID_NUMBER,
   SMS_IO_SEND_INVALID_BODY,
   SMS_IO_SEND_ENCODE_ERROR,
   SMS_IO_SEND_BODY_TOO_LONG,
   SMS_IO_SEND_RATE_LIMITED,
   SMS_IO_SEND_SEGMENT_LIMITED,
   SMS_IO_SEND_AT_ERROR,
   SMS_IO_SEND_PARTIAL_FAIL, /* some segments ok, others failed */
} sms_io_send_err_t;

typedef struct {
   at_context_t *at;
   rate_bucket_t *msg_bucket;
   rate_bucket_t *segment_bucket;
   int inter_segment_delay_ms;
   bool pdu_mode;
} sms_io_ctx_t;

/**
 * @brief Send an outbound SMS and publish the MQTT response in one call.
 *
 * Does everything sms_io_send would do (validate → rate-limit → encode →
 * send with pacing) and then publishes the appropriate success/error
 * response on echo/response, mapping internal error codes to OCP error
 * strings. This keeps sms_io_send_err_t out of oasis-echo.c.
 *
 * Success responses include segments_sent/segments_total in the data blob;
 * PDU_PARTIAL_FAIL error responses include the same so the caller knows
 * how many segments reached the carrier.
 *
 * @param ctx        I/O context (AT port, buckets, config).
 * @param dest       Destination in E.164 form ("+15551234567").
 * @param utf8_body  UTF-8 message body.
 * @param action     MQTT action name (echoed into the response).
 * @param request_id MQTT request_id (echoed into the response).
 */
void sms_io_send_and_respond(const sms_io_ctx_t *ctx,
                             const char *dest,
                             const char *utf8_body,
                             const char *action,
                             const char *request_id);

/**
 * @brief Send an outbound SMS. Lower-level entry point for tests or callers
 *        that want to publish their own response shape.
 *
 * Output parameters mirror sms_io_send_and_respond: segments_sent is 0..total
 * on PARTIAL_FAIL, 0 on other errors, total on success.
 */
sms_io_send_err_t sms_io_send(const sms_io_ctx_t *ctx,
                              const char *dest,
                              const char *utf8_body,
                              int *segments_sent,
                              int *total_segments,
                              char *at_err_detail,
                              size_t at_err_cap);

/**
 * @brief Handle a CMTI URC: read SMS from modem storage, decode, publish.
 *
 * Covers both legacy text-mode and the new PDU path. The function is the
 * single point where inbound messages are parsed and forwarded onto
 * echo/events. Multi-segment PDU messages are run through reassembly and
 * published only when complete.
 *
 * @param ctx       I/O context.
 * @param sms_index Modem storage slot (from +CMTI).
 */
void sms_io_handle_cmti(const sms_io_ctx_t *ctx, int sms_index);

#endif /* SMS_IO_H */
