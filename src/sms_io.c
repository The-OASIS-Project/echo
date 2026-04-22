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
 * SMS send/dispatch implementation. See sms_io.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "sms_io.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "mqtt_comms.h"
#include "pdu.h"
#include "sms.h"
#include "sms_reassembly.h"

/* ── Shared reassembly body buffer. Capped so a single URC can't OOM, and
 *    sized large enough for max-concat UCS2 (10 * 67 chars * worst-case 4
 *    UTF-8 bytes = 2680; 4 KiB covers it with headroom). ─────────────── */
#define SMS_IO_REASM_BODY_MAX 4096

/* Emergency eviction watchdog: if reassembly fails with ERROR (not incomplete)
 * we still want the modem slot cleared so inbox doesn't fill. Short timeout
 * — CMGD on local storage is a sub-100ms op in practice. "Fire-and-forget"
 * means the caller doesn't wait for completion before moving on; failures
 * are still logged so a pinned inbox surfaces before it overflows. */
static void cmgd_fire_and_forget(at_context_t *at, int sms_index) {
   char cmd[32];
   snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", sms_index);
   at_response_t resp;
   at_status_t rc = at_command_send(at, cmd, &resp, AT_TIMEOUT_SMS_STORAGE);
   if (rc != AT_OK) {
      OLOG_WARNING("CMGD idx %d failed: %s (inbox may fill)", sms_index, at_status_str(rc));
   }
}

/* ── Outbound ────────────────────────────────────────────────────────── */

/* Legacy UCS2-hex text-mode send. Kept behind --legacy-sms until PDU mode
 * is soaked on real hardware; deleting this function + the pdu_mode flag
 * + the AT+CSMP init line in modem.c is all the removal work takes. */
static sms_io_send_err_t send_text_mode(const sms_io_ctx_t *ctx,
                                        const char *dest,
                                        const char *clean,
                                        int *segments_sent,
                                        int *total_segments,
                                        char *at_err_detail,
                                        size_t at_err_cap) {
   char hex_number[PHONE_NUMBER_HEX_MAX + 1];
   char hex_body[SMS_BODY_HEX_MAX + 1];
   if (sms_utf8_to_ucs2_hex(dest, hex_number, sizeof(hex_number)) < 0 ||
       sms_utf8_to_ucs2_hex(clean, hex_body, sizeof(hex_body)) < 0) {
      return SMS_IO_SEND_ENCODE_ERROR;
   }
   at_response_t resp;
   at_status_t rc = at_command_send_sms(ctx->at, hex_number, hex_body, &resp);
   if (rc == AT_OK) {
      if (segments_sent) {
         *segments_sent = 1;
      }
      if (total_segments) {
         *total_segments = 1;
      }
      return SMS_IO_SEND_OK;
   }
   if (at_err_detail && at_err_cap) {
      snprintf(at_err_detail, at_err_cap, "%s", at_status_str(rc));
   }
   return SMS_IO_SEND_AT_ERROR;
}

static sms_io_send_err_t send_pdu_mode(const sms_io_ctx_t *ctx,
                                       const char *dest,
                                       const char *clean,
                                       int *segments_sent,
                                       int *total_segments,
                                       char *at_err_detail,
                                       size_t at_err_cap) {
   bool ucs2 = pdu_needs_ucs2(clean);
   int seg_count = pdu_segment_count(clean, ucs2);
   if (seg_count <= 0 || seg_count > PDU_MAX_SEGMENTS) {
      return SMS_IO_SEND_BODY_TOO_LONG;
   }

   /* Segment budget separate from message budget — 200/hr default covers a
    * healthy conversation pace without letting one user burn the whole
    * carrier plan. Atomic take: partial debit is worse than either
    * reject-and-retry or allow-the-whole-message. */
   if (ctx->segment_bucket && !rate_bucket_take_n(ctx->segment_bucket, seg_count)) {
      return SMS_IO_SEND_SEGMENT_LIMITED;
   }

   pdu_segment_t segs[PDU_MAX_SEGMENTS];
   int n_segs = 0;
   uint8_t ref_id = pdu_new_ref_id();
   pdu_err_t err = pdu_encode_submit(dest, clean, ref_id, ucs2, segs, PDU_MAX_SEGMENTS, &n_segs);
   if (err != PDU_OK) {
      OLOG_ERROR("PDU encode failed: %s", pdu_err_str(err));
      if (at_err_detail && at_err_cap) {
         snprintf(at_err_detail, at_err_cap, "%s", pdu_err_str(err));
      }
      return (err == PDU_ERR_BODY_TOO_LONG) ? SMS_IO_SEND_BODY_TOO_LONG : SMS_IO_SEND_ENCODE_ERROR;
   }
   if (total_segments) {
      *total_segments = n_segs;
   }

   for (int i = 0; i < n_segs; i++) {
      at_response_t resp;
      at_status_t rc = at_command_send_pdu(ctx->at, segs[i].tpdu_octets, segs[i].hex, &resp);
      if (rc != AT_OK) {
         OLOG_WARNING("PDU segment %d/%d failed: %s", i + 1, n_segs, at_status_str(rc));
         if (at_err_detail && at_err_cap) {
            snprintf(at_err_detail, at_err_cap, "%s", at_status_str(rc));
         }
         if (segments_sent) {
            *segments_sent = i;
         }
         return (i == 0) ? SMS_IO_SEND_AT_ERROR : SMS_IO_SEND_PARTIAL_FAIL;
      }
      if (i + 1 < n_segs && ctx->inter_segment_delay_ms > 0) {
         struct timespec delay;
         delay.tv_sec = ctx->inter_segment_delay_ms / 1000;
         delay.tv_nsec = (long)(ctx->inter_segment_delay_ms % 1000) * 1000000L;
         nanosleep(&delay, NULL);
      }
   }

   if (segments_sent) {
      *segments_sent = n_segs;
   }
   return SMS_IO_SEND_OK;
}

sms_io_send_err_t sms_io_send(const sms_io_ctx_t *ctx,
                              const char *dest,
                              const char *utf8_body,
                              int *segments_sent,
                              int *total_segments,
                              char *at_err_detail,
                              size_t at_err_cap) {
   if (segments_sent) {
      *segments_sent = 0;
   }
   if (total_segments) {
      *total_segments = 0;
   }
   if (at_err_detail && at_err_cap) {
      at_err_detail[0] = '\0';
   }
   if (!ctx || !ctx->at || !ctx->msg_bucket || !dest || !utf8_body) {
      return SMS_IO_SEND_INVALID_NUMBER;
   }
   if (!sms_validate_number(dest)) {
      return SMS_IO_SEND_INVALID_NUMBER;
   }

   /* Validate body BEFORE debiting any bucket so a rejected request doesn't
    * burn rate tokens. Empty bodies explicitly flagged as INVALID_BODY —
    * without this, pdu_segment_count(clean)==0 would later be classified as
    * BODY_TOO_LONG which is wrong and misleading for the caller. */
   char clean[SMS_BODY_MAX + 1];
   int clean_len = sms_sanitize_body(utf8_body, clean, sizeof(clean));
   if (clean_len < 0) {
      return SMS_IO_SEND_INVALID_BODY;
   }
   if (clean_len == 0) {
      return SMS_IO_SEND_INVALID_BODY;
   }

   /* For PDU mode, check the segment budget first so a too-long or segment-
    * rate-limited send doesn't consume a message-bucket token. send_pdu_mode
    * still does its own segment-bucket debit; the pre-check here is purely
    * for bucket accounting ordering. */
   if (ctx->pdu_mode) {
      bool ucs2 = pdu_needs_ucs2(clean);
      int seg_count = pdu_segment_count(clean, ucs2);
      if (seg_count <= 0 || seg_count > PDU_MAX_SEGMENTS) {
         return SMS_IO_SEND_BODY_TOO_LONG;
      }
   }

   if (!rate_bucket_allow(ctx->msg_bucket)) {
      return SMS_IO_SEND_RATE_LIMITED;
   }

   if (ctx->pdu_mode) {
      return send_pdu_mode(ctx, dest, clean, segments_sent, total_segments, at_err_detail,
                           at_err_cap);
   }
   return send_text_mode(ctx, dest, clean, segments_sent, total_segments, at_err_detail,
                         at_err_cap);
}

static int build_segment_data_json(char *buf, size_t cap, int sent, int tot) {
   if (!buf || cap == 0) {
      return -1;
   }
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return -1;
   }
   json_object_object_add(obj, "segments_sent", json_object_new_int(sent));
   json_object_object_add(obj, "segments_total", json_object_new_int(tot));
   const char *s = json_object_to_json_string(obj);
   int len = (int)strlen(s);
   if ((size_t)len >= cap) {
      json_object_put(obj);
      return -1;
   }
   memcpy(buf, s, (size_t)len + 1);
   json_object_put(obj);
   return len;
}

void sms_io_send_and_respond(const sms_io_ctx_t *ctx,
                             const char *dest,
                             const char *utf8_body,
                             const char *action,
                             const char *request_id) {
   int sent = 0;
   int total = 0;
   char at_detail[64] = "";
   sms_io_send_err_t rc = sms_io_send(ctx, dest, utf8_body, &sent, &total, at_detail,
                                      sizeof(at_detail));

   /* Pre-initialize so a failed build (e.g., OOM from json-c) can't publish
    * uninitialized stack bytes as the response data blob. */
   char data_buf[128] = "";
   const char *data_ptr = NULL;
   if (build_segment_data_json(data_buf, sizeof(data_buf), sent, total) > 0) {
      data_ptr = data_buf;
   }

   switch (rc) {
      case SMS_IO_SEND_OK:
         mqtt_publish_response(action, request_id, true, NULL, NULL, NULL, data_ptr);
         break;
      case SMS_IO_SEND_INVALID_NUMBER:
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_NUMBER",
                               "Phone number validation failed", NULL);
         break;
      case SMS_IO_SEND_INVALID_BODY:
         mqtt_publish_response(action, request_id, false, NULL, "INVALID_BODY",
                               "SMS body contains dangerous characters", NULL);
         break;
      case SMS_IO_SEND_ENCODE_ERROR:
         mqtt_publish_response(action, request_id, false, NULL, "ENCODE_ERROR",
                               at_detail[0] ? at_detail : "Failed to encode SMS", NULL);
         break;
      case SMS_IO_SEND_BODY_TOO_LONG:
         mqtt_publish_response(action, request_id, false, NULL, "BODY_TOO_LONG",
                               "SMS body exceeds maximum concatenated length", NULL);
         break;
      case SMS_IO_SEND_RATE_LIMITED:
         mqtt_publish_response(action, request_id, false, NULL, "RATE_LIMITED",
                               "SMS rate limit exceeded", NULL);
         break;
      case SMS_IO_SEND_SEGMENT_LIMITED:
         mqtt_publish_response(action, request_id, false, NULL, "SEGMENT_RATE_LIMITED",
                               "SMS segment rate limit exceeded", NULL);
         break;
      case SMS_IO_SEND_AT_ERROR:
         mqtt_publish_response(action, request_id, false, NULL,
                               at_detail[0] ? at_detail : "AT_ERROR", "SMS send failed", NULL);
         break;
      case SMS_IO_SEND_PARTIAL_FAIL:
         mqtt_publish_response(action, request_id, false, NULL, "PDU_PARTIAL_FAIL",
                               at_detail[0] ? at_detail : "Partial SMS send", data_ptr);
         break;
   }
}

/* ── Inbound ─────────────────────────────────────────────────────────── */

/* Publish one reassembled message. Routes through the canonical OCP event
 * builder so the device/msg_type/event/timestamp envelope stays consistent
 * with every other event emission in the daemon. */
static void publish_inbound(int sms_index, const char *sender, const char *body) {
   struct json_object *extra = json_object_new_object();
   if (!extra) {
      OLOG_ERROR("publish_inbound: json_object_new_object failed");
      return;
   }
   json_object_object_add(extra, "index", json_object_new_int(sms_index));
   json_object_object_add(extra, "sender", json_object_new_string(sender ? sender : ""));
   json_object_object_add(extra, "body", json_object_new_string(body ? body : ""));

   char buf[SMS_IO_REASM_BODY_MAX + 512];
   if (mqtt_build_event_json("sms_received", extra, buf, sizeof(buf)) >= 0) {
      mqtt_publish_event(buf);
   }
   json_object_put(extra);
}

/* Extract the hex payload line from AT+CMGR output in PDU mode. In PDU mode
 * CMGR returns:
 *   +CMGR: <stat>,[<alpha>],<length>\r\n<pdu_hex>\r\n\r\nOK
 * We already strip the trailing OK/terminator in the response accumulator,
 * so the body lives on the line after the +CMGR header. */
static const char *extract_pdu_hex(const char *cmgr_data) {
   const char *header = strstr(cmgr_data, "+CMGR:");
   if (!header) {
      return NULL;
   }
   const char *nl = strchr(header, '\n');
   if (!nl) {
      return NULL;
   }
   return nl + 1;
}

/* Decode text-mode CMGR output — the pre-existing UCS2-text-mode behavior. */
static void handle_cmti_text_mode(at_context_t *at, int sms_index) {
   char cmd[32];
   snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", sms_index);
   at_response_t resp;
   at_status_t rc = at_command_send(at, cmd, &resp, AT_TIMEOUT_SMS_STORAGE);
   if (rc != AT_OK) {
      OLOG_WARNING("CMGR text-mode failed at idx %d: %s (deleting slot)", sms_index,
                   at_status_str(rc));
      cmgd_fire_and_forget(at, sms_index);
      return;
   }

   char sender_hex[PHONE_NUMBER_HEX_MAX + 1] = "";
   const char *body_hex = "";
   const char *cmgr = strstr(resp.data, "+CMGR:");
   if (cmgr) {
      const char *q1 = strchr(cmgr, '"');
      if (q1 && (q1 = strchr(q1 + 1, '"')) && (q1 = strchr(q1 + 1, '"'))) {
         const char *q2 = strchr(q1 + 1, '"');
         if (q2) {
            size_t len = (size_t)(q2 - q1 - 1);
            if (len > PHONE_NUMBER_HEX_MAX) {
               len = PHONE_NUMBER_HEX_MAX;
            }
            memcpy(sender_hex, q1 + 1, len);
            sender_hex[len] = '\0';
         }
      }
      const char *nl = strchr(cmgr, '\n');
      if (nl) {
         body_hex = nl + 1;
         /* Trim trailing CR/LF/space on the body line. */
         size_t blen = strlen(body_hex);
         char *end = resp.data + (body_hex - resp.data) + blen;
         while (end > body_hex && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *(--end) = '\0';
         }
      }
   }

   char sender[PHONE_NUMBER_MAX + 1] = "";
   char body[SMS_BODY_MAX + 1] = "";
   sms_ucs2_hex_to_utf8(sender_hex, sender, sizeof(sender));
   sms_ucs2_hex_to_utf8(body_hex, body, sizeof(body));

   publish_inbound(sms_index, sender, body);
}

static void handle_cmti_reassemble(at_context_t *at,
                                   int sms_index,
                                   const pdu_decoded_t *dec,
                                   const char *frag) {
   char full[SMS_IO_REASM_BODY_MAX];
   size_t full_len = 0;
   reasm_result_t r = sms_reassembly_push(dec->sender, dec->udh_ref_id, dec->udh_total,
                                          dec->udh_seq, frag, dec->body_len, time(NULL), full,
                                          sizeof(full), &full_len);
   switch (r) {
      case REASM_COMPLETE:
         publish_inbound(sms_index, dec->sender, full);
         cmgd_fire_and_forget(at, sms_index);
         break;
      case REASM_INCOMPLETE:
         cmgd_fire_and_forget(at, sms_index);
         break;
      case REASM_REJECTED_DUP:
         cmgd_fire_and_forget(at, sms_index);
         break;
      default:
         OLOG_WARNING("Reassembly rejected idx %d: code=%d", sms_index, (int)r);
         cmgd_fire_and_forget(at, sms_index);
         break;
   }
}

static void handle_cmti_pdu_mode(at_context_t *at, int sms_index) {
   char cmd[32];
   snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", sms_index);
   at_response_t resp;
   at_status_t rc = at_command_send(at, cmd, &resp, AT_TIMEOUT_SMS_STORAGE);
   if (rc != AT_OK) {
      OLOG_WARNING("CMGR PDU failed at idx %d: %s (deleting slot)", sms_index, at_status_str(rc));
      cmgd_fire_and_forget(at, sms_index);
      return;
   }

   const char *pdu_hex = extract_pdu_hex(resp.data);
   if (!pdu_hex || pdu_hex[0] == '\0') {
      OLOG_WARNING("CMGR PDU idx %d: no hex payload in response", sms_index);
      cmgd_fire_and_forget(at, sms_index);
      return;
   }

   pdu_decoded_t dec;
   char frag[SMS_IO_REASM_BODY_MAX];
   pdu_err_t err = pdu_decode(pdu_hex, &dec, frag, sizeof(frag));
   if (err != PDU_OK) {
      OLOG_WARNING("PDU decode idx %d failed: %s", sms_index, pdu_err_str(err));
      /* Delete anyway so the inbox doesn't fill with bad messages. */
      cmgd_fire_and_forget(at, sms_index);
      return;
   }

   if (!dec.has_udh) {
      publish_inbound(sms_index, dec.sender, frag);
      cmgd_fire_and_forget(at, sms_index);
      return;
   }

   /* Multi-segment reassembly runs in its own frame so the 4 KiB `full`
    * buffer doesn't sit on the stack for the single-segment fast path. */
   handle_cmti_reassemble(at, sms_index, &dec, frag);
}

void sms_io_handle_cmti(const sms_io_ctx_t *ctx, int sms_index) {
   if (!ctx || !ctx->at) {
      return;
   }
   OLOG_INFO("Reading SMS at index %d (%s)", sms_index, ctx->pdu_mode ? "PDU" : "text");
   if (ctx->pdu_mode) {
      handle_cmti_pdu_mode(ctx->at, sms_index);
   } else {
      handle_cmti_text_mode(ctx->at, sms_index);
   }
}
