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
 * SMS PDU encoder/decoder for the SIM7600G-H modem.
 *
 * Implements 3GPP TS 23.040 SMS PDU SUBMIT (outbound) and DELIVER (inbound)
 * handling for UCS2-encoded messages. Multi-segment messages are concatenated
 * via the User Data Header (UDH) with IE 0x00 (8-bit reference) so the
 * receiving device reassembles them into one conversation.
 *
 * v1 is UCS2-only: ASCII messages use 67 chars/segment instead of the 153
 * GSM7 packing would allow. GSM7 is deferred until there's a real metric
 * showing it matters.
 */

#ifndef PDU_H
#define PDU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Limits */
#define PDU_MAX_SEGMENTS 10                     /* hard cap on concat segments */
#define PDU_UCS2_CHARS_PER_SEG 67               /* (140 - 6 UDH) / 2 bytes */
#define PDU_TPDU_MAX 280                        /* max TPDU octets per segment */
#define PDU_MAX_HEX_LEN (PDU_TPDU_MAX * 2 + 32) /* TPDU hex + SMSC prefix slack */
#define PDU_SENDER_MAX 24                       /* matches PHONE_NUMBER_MAX + NUL */

/* Data coding scheme (DCS) */
typedef enum {
   PDU_DCS_GSM7 = 0x00,
   PDU_DCS_UCS2 = 0x08,
} pdu_dcs_t;

/* Encode/decode error codes. SUCCESS (0) is the only success value; anything
 * else is a distinct failure reason so callers and fuzz harness logs can
 * classify without parsing strings. */
typedef enum {
   PDU_OK = 0,
   PDU_ERR_NULL_ARG,
   PDU_ERR_BAD_HEX,          /* non-hex character or odd-length input */
   PDU_ERR_TRUNCATED,        /* TPDU ended mid-field */
   PDU_ERR_BAD_LENGTH,       /* length prefix > remaining buffer */
   PDU_ERR_BAD_UDH,          /* malformed user data header */
   PDU_ERR_SENDER_OVERFLOW,  /* decoded sender > PDU_SENDER_MAX */
   PDU_ERR_BODY_TOO_LONG,    /* message needs more than PDU_MAX_SEGMENTS */
   PDU_ERR_BUFFER_TOO_SMALL, /* caller-supplied output buffer too small */
   PDU_ERR_UNSUPPORTED_DCS,  /* decode of GSM7 packed body not implemented v1 */
   PDU_ERR_BAD_ADDRESS,      /* TP-DA length or digits invalid */
   PDU_ERR_INTERNAL,         /* should-not-happen — bug guard */
} pdu_err_t;

/* One encoded segment, ready to feed into AT+CMGS=<tpdu_octets>. Both the
 * SMSC length prefix "00" (no SMSC override) and the TPDU are in `hex`;
 * `tpdu_octets` counts only the TPDU — carrier counts the same way. */
typedef struct {
   char hex[PDU_MAX_HEX_LEN + 1];
   int tpdu_octets;
} pdu_segment_t;

/* Decoded inbound PDU metadata. Body lands in a caller-provided buffer so
 * growing the body cap (UCS2 is 2048 bytes after reassembly) doesn't bloat
 * this struct for every URC handler. */
typedef struct {
   char sender[PDU_SENDER_MAX];
   bool has_udh;
   uint8_t udh_ref_id;
   uint8_t udh_total;
   uint8_t udh_seq;
   time_t scts; /* service centre timestamp (0 if unparseable) */
   bool is_ucs2;
   size_t body_len; /* bytes written to body_out (not including NUL) */
} pdu_decoded_t;

/**
 * @brief Decide whether a UTF-8 body needs UCS2 encoding.
 *
 * v1: always returns true. GSM7 packing is deferred; overseas/emoji traffic
 * always forced UCS2 anyway.
 *
 * @param utf8_body  UTF-8 string (may be NULL/empty).
 * @return true if UCS2 must be used.
 */
bool pdu_needs_ucs2(const char *utf8_body);

/**
 * @brief Generate a non-zero reference id for a new concatenated message.
 *
 * Monotonically increments an internal 8-bit counter, skipping 0 (receivers
 * interpret ref_id=0 as a special case on some handsets). Thread-safe.
 */
uint8_t pdu_new_ref_id(void);

/**
 * @brief Count segments needed to encode a body at the given DCS.
 *
 * @param utf8_body UTF-8 string.
 * @param is_ucs2   true for UCS2 (67 chars/seg), false for GSM7 (153/seg).
 * @return Segment count [1, PDU_MAX_SEGMENTS], or 0 on empty body.
 */
int pdu_segment_count(const char *utf8_body, bool is_ucs2);

/**
 * @brief Encode a UTF-8 body into one or more SMS SUBMIT PDUs.
 *
 * Produces up to PDU_MAX_SEGMENTS segments. Each segment's `hex` is ready
 * to hand to at_command_send_pdu(); `tpdu_octets` is the value for the
 * AT+CMGS=<n> preamble.
 *
 * @param dest      Destination number in E.164 form (e.g., "+15551234567").
 * @param utf8_body UTF-8 message body.
 * @param ref_id    Concatenation reference id (from pdu_new_ref_id()).
 * @param is_ucs2   Encoding selector — v1 always true.
 * @param out_segs  Output segment array.
 * @param max_segs  Size of out_segs (callers pass PDU_MAX_SEGMENTS).
 * @param n_segs    Output: segments produced.
 * @return PDU_OK or a PDU_ERR_* code.
 */
pdu_err_t pdu_encode_submit(const char *dest,
                            const char *utf8_body,
                            uint8_t ref_id,
                            bool is_ucs2,
                            pdu_segment_t *out_segs,
                            int max_segs,
                            int *n_segs);

/**
 * @brief Decode a SMS-DELIVER PDU from hex (as returned by AT+CMGR).
 *
 * Every length-prefix field is bounds-checked before use. Sender digits and
 * the user data are copied into caller-supplied buffers — the function
 * refuses to overflow them and returns a distinct error so the fuzzer can
 * separate bounds violations from other malformed input.
 *
 * UCS2 bodies are decoded to UTF-8 in body_out with light sanitization:
 * U+0000 is stripped (C-string safety), C0/C1 controls (except \n, \t) are
 * replaced with ' ', and bidi override controls (U+202A–U+202E, U+2066–U+2069)
 * are dropped to prevent display spoofing.
 *
 * @param tpdu_hex TPDU hex (with optional SMSC prefix as delivered by CMGR).
 * @param out      Output metadata.
 * @param body_out UTF-8 output buffer.
 * @param body_cap Size of body_out in bytes (must be >= 2).
 * @return PDU_OK or a PDU_ERR_* code.
 */
pdu_err_t pdu_decode(const char *tpdu_hex, pdu_decoded_t *out, char *body_out, size_t body_cap);

/**
 * @brief Human-readable string for a PDU error code (for logging).
 */
const char *pdu_err_str(pdu_err_t err);

#endif /* PDU_H */
