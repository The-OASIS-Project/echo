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
 * SMS PDU encoder/decoder. See pdu.h for the public API and 3GPP TS 23.040
 * for the wire format.
 */

/* timegm() needs _DEFAULT_SOURCE on glibc; getrandom() needs <sys/random.h>. */
#define _DEFAULT_SOURCE

#include "pdu.h"

#include <pthread.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define TP_MTI_SUBMIT 0x01
#define TP_MTI_DELIVER 0x00
#define TP_MTI_MASK 0x03
#define TP_UDHI_FLAG 0x40
#define TP_VPF_RELATIVE 0x10 /* VPF=10 in bits 4-3 of first octet */
#define VP_RELATIVE_4_DAYS 0xAA

/* UDH for 8-bit reference concatenation:
 *   UDHL=05, IEI=00, IEL=03, ref, total, seq → 6 octets total (with UDHL byte). */
#define UDH_CONCAT_TOTAL_OCTETS 6

static const char HEX_CHARS[] = "0123456789ABCDEF";

/* ── Hex helpers ─────────────────────────────────────────────────────── */

static inline int hex_nibble(char c) {
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   return -1;
}

static bool hex_string_is_valid(const char *hex, size_t len) {
   if ((len & 1) != 0) {
      return false;
   }
   for (size_t i = 0; i < len; i++) {
      if (hex_nibble(hex[i]) < 0) {
         return false;
      }
   }
   return true;
}

static int hex_read_octet(const char *hex, uint8_t *out) {
   int hi = hex_nibble(hex[0]);
   int lo = hex_nibble(hex[1]);
   if (hi < 0 || lo < 0) {
      return -1;
   }
   *out = (uint8_t)((hi << 4) | lo);
   return 0;
}

static void hex_write_octet(uint8_t v, char *out) {
   out[0] = HEX_CHARS[(v >> 4) & 0x0F];
   out[1] = HEX_CHARS[v & 0x0F];
}

/* ── UTF-8 / UCS2 helpers ────────────────────────────────────────────── */

/* Decode one UTF-8 code point. Returns bytes consumed (1..4), or 0 at end
 * of string. On malformed input, substitutes U+FFFD and advances 1 byte. */
static int utf8_decode(const unsigned char *s, uint32_t *out_cp) {
   if (s[0] == 0) {
      return 0;
   }
   if (s[0] < 0x80) {
      *out_cp = s[0];
      return 1;
   }
   if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
      *out_cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
      return 2;
   }
   if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
      *out_cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
      return 3;
   }
   if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
       (s[3] & 0xC0) == 0x80) {
      *out_cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
      return 4;
   }
   *out_cp = 0xFFFD;
   return 1;
}

/* UCS2 surrogate pair helpers */
static bool is_high_surrogate(uint16_t w) {
   return w >= 0xD800 && w <= 0xDBFF;
}
static bool is_low_surrogate(uint16_t w) {
   return w >= 0xDC00 && w <= 0xDFFF;
}

/* Count UCS2 code units (16-bit words) needed for a UTF-8 body. A BMP
 * character is 1 code unit; a supplementary character is 2 (surrogate pair). */
static int utf8_to_ucs2_units(const char *utf8) {
   if (!utf8) {
      return 0;
   }
   const unsigned char *s = (const unsigned char *)utf8;
   int units = 0;
   while (*s) {
      uint32_t cp;
      int n = utf8_decode(s, &cp);
      if (n == 0) {
         break;
      }
      s += n;
      units += (cp <= 0xFFFF) ? 1 : 2;
   }
   return units;
}

/* ── Sanitization (inbound body) ─────────────────────────────────────── */

/* Strip/replace characters that shouldn't end up in displayed SMS text or in
 * prompts routed to the LLM. Covers three threat classes:
 *   1. C-string termination — U+0000.
 *   2. Display spoofing — bidi overrides, zero-width joiners/non-joiners,
 *      BOM/WJ, variation selectors, Arabic/Mongolian format chars.
 *   3. Prompt injection amplification — the Unicode Tag block (U+E0000..
 *      U+E007F) is widely used to smuggle hidden instructions into LLM
 *      prompts; strip it outright. */
static bool sanitize_ucs2_cp(uint32_t *cp) {
   uint32_t c = *cp;
   if (c == 0x0000) {
      return false;
   }
   /* C0/C1 controls → space (keep \n, \t). */
   if (c < 0x20 && c != 0x0A && c != 0x09) {
      *cp = 0x20;
      return true;
   }
   if (c >= 0x7F && c <= 0x9F) {
      *cp = 0x20;
      return true;
   }
   /* Bidi overrides/isolates. */
   if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)) {
      return false;
   }
   /* Zero-width + formatting characters. */
   if ((c >= 0x200B && c <= 0x200F) || /* ZWSP, ZWNJ, ZWJ, LRM, RLM */
       c == 0x2060 ||                  /* WORD JOINER */
       c == 0xFEFF ||                  /* BOM / ZWNBSP */
       c == 0x061C ||                  /* ALM */
       c == 0x180E) {                  /* MONGOLIAN VOWEL SEPARATOR */
      return false;
   }
   /* Variation selectors (VS1..VS16 in BMP, VS17..VS256 in supplementary). */
   if ((c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF)) {
      return false;
   }
   /* Unicode tag characters — prompt-injection smuggling vector. */
   if (c >= 0xE0000 && c <= 0xE007F) {
      return false;
   }
   return true;
}

/* Append UTF-8 encoding of cp to body_out[pos..]. Returns new pos, or -1 if
 * it wouldn't fit (caller must have reserved room for NUL). */
static int append_utf8(char *body_out, size_t cap, int pos, uint32_t cp) {
   /* Reserve one byte for NUL. */
   if ((size_t)pos >= cap) {
      return -1;
   }
   size_t room = cap - 1 - (size_t)pos;
   if (cp < 0x80) {
      if (room < 1)
         return -1;
      body_out[pos++] = (char)cp;
   } else if (cp < 0x800) {
      if (room < 2)
         return -1;
      body_out[pos++] = (char)(0xC0 | (cp >> 6));
      body_out[pos++] = (char)(0x80 | (cp & 0x3F));
   } else if (cp < 0x10000) {
      if (room < 3)
         return -1;
      body_out[pos++] = (char)(0xE0 | (cp >> 12));
      body_out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      body_out[pos++] = (char)(0x80 | (cp & 0x3F));
   } else {
      if (room < 4)
         return -1;
      body_out[pos++] = (char)(0xF0 | (cp >> 18));
      body_out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      body_out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      body_out[pos++] = (char)(0x80 | (cp & 0x3F));
   }
   return pos;
}

/* ── GSM 7-bit decode (inbound) ──────────────────────────────────────── */
/* We decode GSM7 because most phones default to it for plain ASCII — 99%
 * of real inbound traffic arrives with DCS=0x00, not UCS2. We don't encode
 * GSM7 (UCS2-only for outbound per v1 plan); that's an independent bit of
 * 7-bit packing that's only worth writing once bandwidth metrics justify. */

/* 3GPP TS 23.038 §6.2.1.1 default alphabet. 0x1B is the ESC marker for
 * the extension table; everything else maps to a single Unicode code point. */
static const uint16_t gsm7_default_table[128] = {
   0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC, 0x00F2, 0x00C7, 0x000A, 0x00D8,
   0x00F8, 0x000D, 0x00C5, 0x00E5, 0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
   0x03A3, 0x0398, 0x039E, 0xFFFF, 0x00C6, 0x00E6, 0x00DF, 0x00C9, 0x0020, 0x0021, 0x0022, 0x0023,
   0x00A4, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
   0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B,
   0x003C, 0x003D, 0x003E, 0x003F, 0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
   0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F, 0x0050, 0x0051, 0x0052, 0x0053,
   0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
   0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006A, 0x006B,
   0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
   0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

/* Extension table (3GPP TS 23.038 §6.2.1.1 Table 2). Triggered by ESC
 * (0x1B) in the septet stream. Most values are reserved or duplicate the
 * default alphabet; only the 10 below have distinct extension encodings. */
static uint32_t gsm7_extension_lookup(uint8_t septet) {
   switch (septet) {
      case 0x0A:
         return 0x000C; /* form feed */
      case 0x14:
         return 0x005E; /* ^ */
      case 0x28:
         return 0x007B; /* { */
      case 0x29:
         return 0x007D; /* } */
      case 0x2F:
         return 0x005C; /* \ */
      case 0x3C:
         return 0x005B; /* [ */
      case 0x3D:
         return 0x007E; /* ~ */
      case 0x3E:
         return 0x005D; /* ] */
      case 0x40:
         return 0x007C; /* | */
      case 0x65:
         return 0x20AC; /* € */
      default:
         return 0x003F; /* '?' — unknown extension, best-effort display */
   }
}

/* Unpack n_septets from `data` starting at bit position `start_bit`. Each
 * septet is 7 bits, LSB-first within the byte stream. Output is Unicode
 * code points translated via the default alphabet + extension table. */
static int unpack_gsm7_septets(const uint8_t *data,
                               size_t data_bytes,
                               size_t start_bit,
                               int n_septets,
                               uint32_t *out_cps,
                               int out_cap) {
   int produced = 0;
   bool pending_esc = false;
   for (int i = 0; i < n_septets && produced < out_cap; i++) {
      size_t bit_pos = start_bit + (size_t)i * 7;
      size_t byte_idx = bit_pos / 8;
      size_t bit_off = bit_pos % 8;
      if (byte_idx >= data_bytes) {
         break;
      }
      uint32_t window = data[byte_idx];
      if (byte_idx + 1 < data_bytes) {
         window |= ((uint32_t)data[byte_idx + 1]) << 8;
      }
      uint8_t septet = (uint8_t)((window >> bit_off) & 0x7F);

      if (pending_esc) {
         out_cps[produced++] = gsm7_extension_lookup(septet);
         pending_esc = false;
      } else if (septet == 0x1B) {
         pending_esc = true;
      } else {
         out_cps[produced++] = gsm7_default_table[septet];
      }
   }
   /* A dangling ESC at the end is malformed — drop silently rather than
    * emit an unterminated extension. */
   return produced;
}

/* ── Address encoding ────────────────────────────────────────────────── */

/* Encode a phone number as BCD semi-octets with nibble swap.
 * "+15551234567" → 91 (TOA) 11 (len=11) 51 55 21 43 65 F7  (odd pads with F).
 * Writes to out (TOA + digits), returns bytes written or -1 on bad input. */
static int encode_address_bcd(const char *dest, uint8_t *out, size_t out_cap, int *digit_count) {
   if (!dest || out_cap < 2) {
      return -1;
   }
   uint8_t toa = 0x81; /* unknown type, unknown numbering plan */
   const char *p = dest;
   if (*p == '+') {
      toa = 0x91; /* international */
      p++;
   }

   char digits[20];
   int n = 0;
   for (; *p; p++) {
      if (*p < '0' || *p > '9') {
         return -1;
      }
      if (n >= (int)sizeof(digits)) {
         return -1;
      }
      digits[n++] = *p;
   }
   if (n == 0 || n > 15) {
      return -1;
   }

   size_t need = 1 + ((size_t)n + 1) / 2;
   if (need > out_cap) {
      return -1;
   }

   out[0] = toa;
   int o = 1;
   for (int i = 0; i < n; i += 2) {
      int hi = (i + 1 < n) ? (digits[i + 1] - '0') : 0xF;
      int lo = digits[i] - '0';
      out[o++] = (uint8_t)((hi << 4) | lo);
   }

   *digit_count = n;
   return o;
}

/* Decode a phone number from BCD semi-octets (with nibble swap).
 * `toa` is the type-of-address byte (0x91=international → prepend '+').
 * `semi_octets` is the TP-OA length field (digit count, not byte count).
 * Returns 0 on success, or a PDU_ERR_* on truncation/overflow. */
static pdu_err_t decode_address_bcd(const uint8_t *buf,
                                    size_t buf_len,
                                    uint8_t toa,
                                    int semi_octets,
                                    char *out,
                                    size_t out_cap) {
   if (semi_octets < 0 || semi_octets > 20) {
      return PDU_ERR_BAD_ADDRESS;
   }
   int bytes = (semi_octets + 1) / 2;
   if ((size_t)bytes > buf_len) {
      return PDU_ERR_TRUNCATED;
   }
   /* Need room for optional '+' + digits + NUL. */
   if (out_cap < (size_t)semi_octets + 2) {
      return PDU_ERR_SENDER_OVERFLOW;
   }

   size_t pos = 0;
   if ((toa & 0x70) == 0x10) {
      out[pos++] = '+';
   }
   for (int i = 0; i < semi_octets; i++) {
      int b = buf[i / 2];
      int nib = (i % 2 == 0) ? (b & 0x0F) : ((b >> 4) & 0x0F);
      if (nib > 9) {
         /* 'F' is the valid pad for odd digits; anything else is junk. */
         if (nib == 0xF && i == semi_octets - 1) {
            break;
         }
         return PDU_ERR_BAD_ADDRESS;
      }
      out[pos++] = (char)('0' + nib);
   }
   out[pos] = '\0';
   return PDU_OK;
}

/* ── Public: heuristics ──────────────────────────────────────────────── */

bool pdu_needs_ucs2(const char *utf8_body) {
   (void)utf8_body;
   return true; /* v1 policy */
}

uint8_t pdu_new_ref_id(void) {
   static pthread_mutex_t ref_mutex = PTHREAD_MUTEX_INITIALIZER;
   static uint8_t counter = 0;
   static bool seeded = false;

   pthread_mutex_lock(&ref_mutex);
   if (!seeded) {
      /* Seed from the kernel RNG so a local observer can't predict the next
       * ref_id from an earlier outbound PDU. Fall back to the clock if
       * getrandom is unavailable — still better than 0. */
      uint8_t seed = 0;
      if (getrandom(&seed, 1, GRND_NONBLOCK) != 1) {
         seed = (uint8_t)(time(NULL) & 0xFF);
      }
      counter = seed;
      seeded = true;
   }
   uint8_t v = ++counter;
   if (v == 0) {
      v = ++counter; /* skip 0 */
   }
   pthread_mutex_unlock(&ref_mutex);
   return v;
}

int pdu_segment_count(const char *utf8_body, bool is_ucs2) {
   if (!utf8_body || utf8_body[0] == '\0') {
      return 0;
   }
   if (!is_ucs2) {
      /* GSM7 not implemented for encode path — fall back to UCS2 math so
       * callers that haven't switched don't accidentally pick the wrong
       * segment count. */
      is_ucs2 = true;
   }
   int units = utf8_to_ucs2_units(utf8_body);
   if (units <= 0) {
      return 0;
   }
   /* Single-segment UCS2 allows 70 chars (no UDH); concat uses 67.
    * We always emit concat headers once seg > 1 so the receiver joins them. */
   if (units <= 70) {
      return 1;
   }
   int segs = (units + PDU_UCS2_CHARS_PER_SEG - 1) / PDU_UCS2_CHARS_PER_SEG;
   if (segs > PDU_MAX_SEGMENTS) {
      segs = PDU_MAX_SEGMENTS + 1; /* sentinel: too long */
   }
   return segs;
}

/* ── Encode path ─────────────────────────────────────────────────────── */

/* Convert UTF-8 body to UCS2 code-unit array. Returns units written, or -1
 * on buffer overflow. */
static int utf8_to_ucs2(const char *utf8, uint16_t *units, int max_units) {
   const unsigned char *s = (const unsigned char *)utf8;
   int n = 0;
   while (*s && n < max_units) {
      uint32_t cp;
      int consumed = utf8_decode(s, &cp);
      if (consumed == 0) {
         break;
      }
      s += consumed;
      if (cp <= 0xFFFF) {
         units[n++] = (uint16_t)cp;
      } else {
         if (n + 2 > max_units) {
            return -1;
         }
         uint32_t adj = cp - 0x10000;
         units[n++] = (uint16_t)(0xD800 + (adj >> 10));
         units[n++] = (uint16_t)(0xDC00 + (adj & 0x3FF));
      }
   }
   return n;
}

static int write_tpdu_octet(pdu_segment_t *seg, int tpdu_pos, uint8_t v) {
   /* 2 hex chars per octet, +2 for "00" SMSC prefix already written, +1 NUL. */
   int offset = 2 + tpdu_pos * 2;
   if (offset + 2 >= (int)sizeof(seg->hex)) {
      return -1;
   }
   hex_write_octet(v, seg->hex + offset);
   return tpdu_pos + 1;
}

pdu_err_t pdu_encode_submit(const char *dest,
                            const char *utf8_body,
                            uint8_t ref_id,
                            bool is_ucs2,
                            pdu_segment_t *out_segs,
                            int max_segs,
                            int *n_segs) {
   if (!dest || !utf8_body || !out_segs || !n_segs || max_segs <= 0) {
      return PDU_ERR_NULL_ARG;
   }
   if (!is_ucs2) {
      /* GSM7 encode path deferred. */
      return PDU_ERR_UNSUPPORTED_DCS;
   }

   /* Convert the whole body to UCS2 once. */
   uint16_t units[PDU_UCS2_CHARS_PER_SEG * PDU_MAX_SEGMENTS + 32];
   int total_units = utf8_to_ucs2(utf8_body, units, (int)(sizeof(units) / sizeof(units[0])));
   if (total_units < 0) {
      return PDU_ERR_BODY_TOO_LONG;
   }
   if (total_units == 0) {
      return PDU_ERR_NULL_ARG;
   }

   int per_seg_max;
   if (total_units <= 70) {
      per_seg_max = total_units;
   } else {
      per_seg_max = PDU_UCS2_CHARS_PER_SEG;
   }

   /* Precompute slice boundaries so surrogate pairs don't split across
    * segments. A high surrogate at a slice boundary gets pushed to the next
    * segment; if that would spill past PDU_MAX_SEGMENTS we refuse. */
   int slices[PDU_MAX_SEGMENTS];
   int segs = 0;
   int cursor = 0;
   while (cursor < total_units) {
      if (segs >= PDU_MAX_SEGMENTS || segs >= max_segs) {
         return PDU_ERR_BODY_TOO_LONG;
      }
      int remain = total_units - cursor;
      int slice = (remain > per_seg_max) ? per_seg_max : remain;
      /* Only pull back a surrogate if more units follow. */
      if (cursor + slice < total_units && is_high_surrogate(units[cursor + slice - 1])) {
         slice--;
         if (slice <= 0) {
            return PDU_ERR_INTERNAL; /* per_seg_max == 1 — impossible for UCS2 */
         }
      }
      slices[segs++] = slice;
      cursor += slice;
   }
   if (segs == 0) {
      return PDU_ERR_NULL_ARG;
   }

   /* Encode destination address once (same for all segments). */
   uint8_t addr_buf[16];
   int addr_digits = 0;
   int addr_len = encode_address_bcd(dest, addr_buf, sizeof(addr_buf), &addr_digits);
   if (addr_len < 0) {
      return PDU_ERR_BAD_ADDRESS;
   }

   int slice_start = 0;
   for (int si = 0; si < segs; si++) {
      pdu_segment_t *seg = &out_segs[si];
      memset(seg, 0, sizeof(*seg));

      /* SMSC length prefix "00" tells the modem to use the default SMSC.
       * It is NOT counted in tpdu_octets for AT+CMGS. */
      seg->hex[0] = '0';
      seg->hex[1] = '0';

      int pos = 0;
      int rc;

      /* TP-MTI (SUBMIT) + VPF=10 + UDHI (if concat) */
      uint8_t first = TP_MTI_SUBMIT | TP_VPF_RELATIVE;
      if (segs > 1) {
         first |= TP_UDHI_FLAG;
      }
      if ((rc = write_tpdu_octet(seg, pos, first)) < 0)
         return PDU_ERR_BUFFER_TOO_SMALL;
      pos = rc;

      /* TP-MR — let modem assign */
      if ((rc = write_tpdu_octet(seg, pos, 0x00)) < 0)
         return PDU_ERR_BUFFER_TOO_SMALL;
      pos = rc;

      /* TP-DA: length in semi-octets, then TOA + digits */
      if ((rc = write_tpdu_octet(seg, pos, (uint8_t)addr_digits)) < 0)
         return PDU_ERR_BUFFER_TOO_SMALL;
      pos = rc;
      for (int j = 0; j < addr_len; j++) {
         if ((rc = write_tpdu_octet(seg, pos, addr_buf[j])) < 0)
            return PDU_ERR_BUFFER_TOO_SMALL;
         pos = rc;
      }

      /* TP-PID = 0 */
      if ((rc = write_tpdu_octet(seg, pos, 0x00)) < 0)
         return PDU_ERR_BUFFER_TOO_SMALL;
      pos = rc;

      /* TP-DCS = 0x08 (UCS2) */
      if ((rc = write_tpdu_octet(seg, pos, (uint8_t)PDU_DCS_UCS2)) < 0)
         return PDU_ERR_BUFFER_TOO_SMALL;
      pos = rc;

      /* TP-VP = 0xAA (relative, 4 days) */
      if ((rc = write_tpdu_octet(seg, pos, VP_RELATIVE_4_DAYS)) < 0)
         return PDU_ERR_BUFFER_TOO_SMALL;
      pos = rc;

      int slice = slices[si];
      int body_octets = slice * 2;
      int udh_octets = (segs > 1) ? UDH_CONCAT_TOTAL_OCTETS : 0;
      int udl = body_octets + udh_octets;

      /* TP-UDL */
      if ((rc = write_tpdu_octet(seg, pos, (uint8_t)udl)) < 0)
         return PDU_ERR_BUFFER_TOO_SMALL;
      pos = rc;

      /* TP-UD: optional UDH, then UCS2 body */
      if (segs > 1) {
         /* UDHL=05, IEI=00 (8-bit ref concat), IEL=03, ref, total, seq */
         const uint8_t udh[UDH_CONCAT_TOTAL_OCTETS] = { 0x05,   0x00,          0x03,
                                                        ref_id, (uint8_t)segs, (uint8_t)(si + 1) };
         for (int k = 0; k < UDH_CONCAT_TOTAL_OCTETS; k++) {
            if ((rc = write_tpdu_octet(seg, pos, udh[k])) < 0)
               return PDU_ERR_BUFFER_TOO_SMALL;
            pos = rc;
         }
      }

      for (int u = 0; u < slice; u++) {
         uint16_t w = units[slice_start + u];
         if ((rc = write_tpdu_octet(seg, pos, (uint8_t)(w >> 8))) < 0)
            return PDU_ERR_BUFFER_TOO_SMALL;
         pos = rc;
         if ((rc = write_tpdu_octet(seg, pos, (uint8_t)(w & 0xFF))) < 0)
            return PDU_ERR_BUFFER_TOO_SMALL;
         pos = rc;
      }

      seg->tpdu_octets = pos;
      seg->hex[2 + pos * 2] = '\0';
      slice_start += slice;
   }

   *n_segs = segs;
   return PDU_OK;
}

/* ── Decode path ─────────────────────────────────────────────────────── */

typedef struct {
   uint8_t octets[PDU_TPDU_MAX + 64];
   size_t len;
} pdu_buf_t;

static pdu_err_t hex_to_buf(const char *hex, pdu_buf_t *buf) {
   if (!hex) {
      return PDU_ERR_NULL_ARG;
   }
   size_t len = strlen(hex);
   /* Trim trailing whitespace. */
   while (len > 0 && (hex[len - 1] == '\r' || hex[len - 1] == '\n' || hex[len - 1] == ' ' ||
                      hex[len - 1] == '\t')) {
      len--;
   }
   if (len == 0) {
      return PDU_ERR_TRUNCATED;
   }
   if (!hex_string_is_valid(hex, len)) {
      return PDU_ERR_BAD_HEX;
   }
   size_t bytes = len / 2;
   if (bytes > sizeof(buf->octets)) {
      return PDU_ERR_BAD_LENGTH;
   }
   for (size_t i = 0; i < bytes; i++) {
      uint8_t v;
      if (hex_read_octet(hex + i * 2, &v) < 0) {
         return PDU_ERR_BAD_HEX;
      }
      buf->octets[i] = v;
   }
   buf->len = bytes;
   return PDU_OK;
}

/* Parse a 7-octet SCTS into a time_t. Return 0 on error. */
static time_t parse_scts(const uint8_t *s) {
   /* Each octet is two BCD nibbles with nibble-swap. */
   int yr = (s[0] & 0x0F) * 10 + ((s[0] >> 4) & 0x0F);
   int mo = (s[1] & 0x0F) * 10 + ((s[1] >> 4) & 0x0F);
   int dy = (s[2] & 0x0F) * 10 + ((s[2] >> 4) & 0x0F);
   int hr = (s[3] & 0x0F) * 10 + ((s[3] >> 4) & 0x0F);
   int mn = (s[4] & 0x0F) * 10 + ((s[4] >> 4) & 0x0F);
   int sc = (s[5] & 0x0F) * 10 + ((s[5] >> 4) & 0x0F);
   /* s[6] = timezone in quarters of an hour, sign bit at 0x08 of low nibble. */

   if (mo < 1 || mo > 12 || dy < 1 || dy > 31 || hr > 23 || mn > 59 || sc > 59) {
      return 0;
   }

   struct tm t;
   memset(&t, 0, sizeof(t));
   t.tm_year = 100 + yr; /* year is 00-99, assumed 2000s */
   t.tm_mon = mo - 1;
   t.tm_mday = dy;
   t.tm_hour = hr;
   t.tm_min = mn;
   t.tm_sec = sc;
   return timegm(&t);
}

pdu_err_t pdu_decode(const char *tpdu_hex, pdu_decoded_t *out, char *body_out, size_t body_cap) {
   if (!tpdu_hex || !out || !body_out || body_cap < 2) {
      return PDU_ERR_NULL_ARG;
   }
   memset(out, 0, sizeof(*out));
   body_out[0] = '\0';

   pdu_buf_t buf;
   pdu_err_t err = hex_to_buf(tpdu_hex, &buf);
   if (err != PDU_OK) {
      return err;
   }

   size_t pos = 0;
   size_t remaining = buf.len;

   /* SMSC length prefix. 0 = no SMSC. */
   if (remaining < 1)
      return PDU_ERR_TRUNCATED;
   uint8_t smsc_len = buf.octets[pos++];
   remaining--;
   if (smsc_len > remaining) {
      return PDU_ERR_BAD_LENGTH;
   }
   pos += smsc_len;
   remaining -= smsc_len;

   /* First octet: MTI + flags */
   if (remaining < 1)
      return PDU_ERR_TRUNCATED;
   uint8_t first = buf.octets[pos++];
   remaining--;
   bool udhi = (first & TP_UDHI_FLAG) != 0;
   /* Accept MTI=0 (DELIVER). Other MTIs (STATUS REPORT etc.) not handled. */
   if ((first & TP_MTI_MASK) != TP_MTI_DELIVER) {
      /* Not a fatal decode failure — just nothing useful to extract. */
      return PDU_ERR_UNSUPPORTED_DCS;
   }

   /* TP-OA */
   if (remaining < 1)
      return PDU_ERR_TRUNCATED;
   uint8_t oa_semi = buf.octets[pos++];
   remaining--;
   if (remaining < 1)
      return PDU_ERR_TRUNCATED;
   uint8_t oa_toa = buf.octets[pos++];
   remaining--;

   int oa_bytes = (oa_semi + 1) / 2;
   if (oa_semi > 20 || (size_t)oa_bytes > remaining) {
      return PDU_ERR_BAD_ADDRESS;
   }
   err = decode_address_bcd(buf.octets + pos, remaining, oa_toa, (int)oa_semi, out->sender,
                            sizeof(out->sender));
   if (err != PDU_OK) {
      return err;
   }
   pos += (size_t)oa_bytes;
   remaining -= (size_t)oa_bytes;

   /* TP-PID */
   if (remaining < 1)
      return PDU_ERR_TRUNCATED;
   pos++;
   remaining--;

   /* TP-DCS */
   if (remaining < 1)
      return PDU_ERR_TRUNCATED;
   uint8_t dcs = buf.octets[pos++];
   remaining--;
   bool is_ucs2 = ((dcs & 0x0C) == 0x08);
   out->is_ucs2 = is_ucs2;

   /* TP-SCTS (7 octets) */
   if (remaining < 7)
      return PDU_ERR_TRUNCATED;
   out->scts = parse_scts(buf.octets + pos);
   pos += 7;
   remaining -= 7;

   /* TP-UDL */
   if (remaining < 1)
      return PDU_ERR_TRUNCATED;
   uint8_t udl = buf.octets[pos++];
   remaining--;

   /* For UCS2, UDL is in octets. For GSM7, UDL is in septets. v1 decodes
    * UCS2 bodies only but still extracts UDH for inbound-from-ASCII senders
    * that a receiving device encoded as GSM7 — best effort: skip the body. */
   if (!is_ucs2) {
      /* Walk UDH if present so we still capture concat metadata, then bail
       * on the body with PDU_ERR_UNSUPPORTED_DCS. Callers can still ack
       * and delete the message. */
   }

   size_t ud_len = remaining;
   if (is_ucs2) {
      if (udl > ud_len) {
         return PDU_ERR_BAD_LENGTH;
      }
      ud_len = udl;
   } else {
      /* GSM7 len in septets → bytes = ceil(udl*7/8). */
      size_t gsm7_bytes = ((size_t)udl * 7 + 7) / 8;
      if (gsm7_bytes > ud_len) {
         return PDU_ERR_BAD_LENGTH;
      }
      ud_len = gsm7_bytes;
   }

   size_t body_off = 0;
   if (udhi) {
      if (ud_len < 1) {
         return PDU_ERR_BAD_UDH;
      }
      uint8_t udhl = buf.octets[pos];
      /* udhl is length of UDH *not including itself*, so total UDH bytes
       * = udhl + 1. Must not exceed user data. */
      if ((size_t)udhl + 1 > ud_len) {
         return PDU_ERR_BAD_UDH;
      }
      size_t ie_off = pos + 1;
      size_t ie_end = pos + 1 + udhl;
      bool saw_concat = false;
      while (ie_off + 2 <= ie_end) {
         uint8_t iei = buf.octets[ie_off];
         uint8_t iel = buf.octets[ie_off + 1];
         if (ie_off + 2 + iel > ie_end) {
            return PDU_ERR_BAD_UDH;
         }
         if (iei == 0x00 && iel == 3) {
            /* A second concat IE in one PDU is a spec violation and a
             * slot-confusion vector. Reject. */
            if (saw_concat) {
               return PDU_ERR_BAD_UDH;
            }
            saw_concat = true;
            out->has_udh = true;
            out->udh_ref_id = buf.octets[ie_off + 2];
            out->udh_total = buf.octets[ie_off + 3];
            out->udh_seq = buf.octets[ie_off + 4];
            if (out->udh_total == 0 || out->udh_seq == 0 || out->udh_seq > out->udh_total) {
               return PDU_ERR_BAD_UDH;
            }
         } else if (iei == 0x08 && iel == 4) {
            if (saw_concat) {
               return PDU_ERR_BAD_UDH;
            }
            saw_concat = true;
            /* 16-bit ref concat — use the low 8 bits so reassembly slots
             * don't need a separate key space. Collisions across 256-space
             * wrap are rare and resolve via the 10-min TTL. */
            out->has_udh = true;
            out->udh_ref_id = buf.octets[ie_off + 3];
            out->udh_total = buf.octets[ie_off + 4];
            out->udh_seq = buf.octets[ie_off + 5];
            if (out->udh_total == 0 || out->udh_seq == 0 || out->udh_seq > out->udh_total) {
               return PDU_ERR_BAD_UDH;
            }
         }
         ie_off += 2 + iel;
      }
      body_off = (size_t)udhl + 1;
   }

   if (!is_ucs2) {
      /* GSM 7-bit default alphabet decode (DCS=0x00, the default for plain
       * ASCII traffic from most handsets). UDH, if present, occupies the
       * first ceil((udhl+1)*8 / 7) virtual septets of the user data — with
       * up to 6 fill bits between the UDH's last byte and the first body
       * septet so the body lands on a septet boundary. */
      const uint8_t *ud = buf.octets + pos;
      int udh_septets = udhi ? (int)(((body_off * 8) + 6) / 7) : 0;
      int body_septets = (int)udl - udh_septets;
      if (body_septets < 0) {
         body_septets = 0;
      }
      size_t body_start_bit = (size_t)udh_septets * 7;

      /* Buffer sized for any single-PDU GSM7 payload (UDL ≤ 255 septets). */
      uint32_t cps[256];
      int n_cps = unpack_gsm7_septets(ud, ud_len, body_start_bit, body_septets, cps,
                                      (int)(sizeof(cps) / sizeof(cps[0])));

      int out_pos = 0;
      for (int k = 0; k < n_cps; k++) {
         uint32_t cp = cps[k];
         if (!sanitize_ucs2_cp(&cp)) {
            continue;
         }
         int np = append_utf8(body_out, body_cap, out_pos, cp);
         if (np < 0) {
            break;
         }
         out_pos = np;
      }
      body_out[out_pos] = '\0';
      out->body_len = (size_t)out_pos;
      return PDU_OK;
   }

   /* UCS2 body walk */
   const uint8_t *body = buf.octets + pos + body_off;
   size_t body_octets = ud_len - body_off;
   if ((body_octets & 1) != 0) {
      return PDU_ERR_BAD_LENGTH;
   }

   int out_pos = 0;
   for (size_t i = 0; i + 1 < body_octets; i += 2) {
      uint16_t w1 = (uint16_t)((body[i] << 8) | body[i + 1]);
      uint32_t cp;
      if (is_high_surrogate(w1) && i + 3 < body_octets) {
         uint16_t w2 = (uint16_t)((body[i + 2] << 8) | body[i + 3]);
         if (is_low_surrogate(w2)) {
            cp = 0x10000 + (((uint32_t)(w1 - 0xD800) << 10) | (w2 - 0xDC00));
            i += 2;
         } else {
            cp = 0xFFFD;
         }
      } else if (is_high_surrogate(w1) || is_low_surrogate(w1)) {
         cp = 0xFFFD;
      } else {
         cp = w1;
      }

      if (!sanitize_ucs2_cp(&cp)) {
         continue;
      }
      int np = append_utf8(body_out, body_cap, out_pos, cp);
      if (np < 0) {
         /* Truncation — NUL-terminate what we have and warn. */
         break;
      }
      out_pos = np;
   }
   body_out[out_pos] = '\0';
   out->body_len = (size_t)out_pos;

   return PDU_OK;
}

/* ── Error strings ───────────────────────────────────────────────────── */

const char *pdu_err_str(pdu_err_t err) {
   switch (err) {
      case PDU_OK:
         return "OK";
      case PDU_ERR_NULL_ARG:
         return "NULL_ARG";
      case PDU_ERR_BAD_HEX:
         return "BAD_HEX";
      case PDU_ERR_TRUNCATED:
         return "TRUNCATED";
      case PDU_ERR_BAD_LENGTH:
         return "BAD_LENGTH";
      case PDU_ERR_BAD_UDH:
         return "BAD_UDH";
      case PDU_ERR_SENDER_OVERFLOW:
         return "SENDER_OVERFLOW";
      case PDU_ERR_BODY_TOO_LONG:
         return "BODY_TOO_LONG";
      case PDU_ERR_BUFFER_TOO_SMALL:
         return "BUFFER_TOO_SMALL";
      case PDU_ERR_UNSUPPORTED_DCS:
         return "UNSUPPORTED_DCS";
      case PDU_ERR_BAD_ADDRESS:
         return "BAD_ADDRESS";
      case PDU_ERR_INTERNAL:
         return "INTERNAL";
   }
   return "UNKNOWN";
}
