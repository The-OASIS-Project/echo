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
 * SMS text-mode helpers and phone number validation.
 */

#include "sms.h"

#include <stdio.h>
#include <string.h>

#include "logging.h"

/* Hex digit helpers */
static const char hex_chars[] = "0123456789ABCDEF";

static int hex_val(char c) {
   if (c >= '0' && c <= '9') {
      return c - '0';
   }
   if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
   }
   if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
   }
   return -1;
}

/**
 * @brief Parse 4 hex digits into a 16-bit value.
 */
static int parse_hex16(const char *s, uint16_t *out) {
   int h0 = hex_val(s[0]);
   int h1 = hex_val(s[1]);
   int h2 = hex_val(s[2]);
   int h3 = hex_val(s[3]);
   if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
      return -1;
   }
   *out = (uint16_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
   return 0;
}

bool sms_validate_number(const char *number) {
   if (!number || number[0] == '\0') {
      return false;
   }

   size_t len = strlen(number);
   if (len > PHONE_NUMBER_MAX) {
      return false;
   }

   for (size_t i = 0; i < len; i++) {
      char c = number[i];
      if (c >= '0' && c <= '9') {
         continue;
      }
      if (c == '+' || c == '*' || c == '#') {
         continue;
      }
      return false;
   }

   return true;
}

int sms_sanitize_body(const char *body, char *out, size_t out_size) {
   if (!body || !out || out_size == 0) {
      return -1;
   }

   size_t body_len = strlen(body);

   /* Scan for injection characters — reject the entire body */
   for (size_t i = 0; i < body_len; i++) {
      unsigned char c = (unsigned char)body[i];
      if (c == 0x1A) { /* Ctrl-Z — terminates SMS, executes next AT command */
         OLOG_ERROR("SMS body rejected: contains Ctrl-Z (AT command injection)");
         return -1;
      }
      if (c == 0x1B) { /* ESC — can trigger escape sequences */
         OLOG_ERROR("SMS body rejected: contains ESC character");
         return -1;
      }
   }

   /* Strip remaining control chars (0x00-0x1F) except newline (0x0A) */
   size_t max_len = out_size - 1;
   if (max_len > SMS_BODY_MAX) {
      max_len = SMS_BODY_MAX;
   }

   size_t j = 0;
   for (size_t i = 0; i < body_len && j < max_len; i++) {
      unsigned char c = (unsigned char)body[i];
      if (c < 0x20 && c != 0x0A) {
         continue; /* strip control chars */
      }
      out[j++] = (char)c;
   }
   out[j] = '\0';

   return (int)j;
}

bool sms_sanitize_clip(const char *clip, char *out, size_t out_size) {
   if (!clip || !out || out_size == 0) {
      return false;
   }

   /* Extract printable characters that look like a phone number */
   size_t max_len = out_size - 1;
   if (max_len > PHONE_NUMBER_MAX) {
      max_len = PHONE_NUMBER_MAX;
   }

   size_t j = 0;
   for (size_t i = 0; clip[i] != '\0' && j < max_len; i++) {
      char c = clip[i];
      if ((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#') {
         out[j++] = c;
      }
      /* Skip non-number characters silently */
   }
   out[j] = '\0';

   /* Validate what we extracted */
   if (j == 0) {
      return false;
   }

   return sms_validate_number(out);
}

/* =============================================================================
 * UCS2 Encoding/Decoding
 * ============================================================================= */

int sms_utf8_to_ucs2_hex(const char *utf8, char *hex_out, size_t hex_size) {
   if (!utf8 || !hex_out || hex_size < 5) {
      return -1;
   }

   const unsigned char *s = (const unsigned char *)utf8;
   size_t pos = 0;

   while (*s && pos + 4 < hex_size) {
      uint32_t cp; /* Unicode code point */
      int bytes;

      /* Decode UTF-8 to code point */
      if (*s < 0x80) {
         cp = *s;
         bytes = 1;
      } else if ((*s & 0xE0) == 0xC0) {
         cp = *s & 0x1F;
         bytes = 2;
      } else if ((*s & 0xF0) == 0xE0) {
         cp = *s & 0x0F;
         bytes = 3;
      } else if ((*s & 0xF8) == 0xF0) {
         cp = *s & 0x07;
         bytes = 4;
      } else {
         s++;
         continue; /* skip invalid byte */
      }

      int consumed = 1; /* at least the start byte */
      for (int i = 1; i < bytes; i++) {
         if ((s[i] & 0xC0) != 0x80) {
            cp = '?';
            break;
         }
         cp = (cp << 6) | (s[i] & 0x3F);
         consumed = i + 1;
      }
      s += consumed;

      /* Encode as UCS2 hex (UTF-16BE) */
      if (cp <= 0xFFFF) {
         /* BMP character — 4 hex digits */
         hex_out[pos++] = hex_chars[(cp >> 12) & 0xF];
         hex_out[pos++] = hex_chars[(cp >> 8) & 0xF];
         hex_out[pos++] = hex_chars[(cp >> 4) & 0xF];
         hex_out[pos++] = hex_chars[cp & 0xF];
      } else if (cp <= 0x10FFFF && pos + 8 < hex_size) {
         /* Supplementary character — UTF-16 surrogate pair (8 hex digits) */
         uint32_t adj = cp - 0x10000;
         uint16_t high = 0xD800 + (uint16_t)(adj >> 10);
         uint16_t low = 0xDC00 + (uint16_t)(adj & 0x3FF);

         hex_out[pos++] = hex_chars[(high >> 12) & 0xF];
         hex_out[pos++] = hex_chars[(high >> 8) & 0xF];
         hex_out[pos++] = hex_chars[(high >> 4) & 0xF];
         hex_out[pos++] = hex_chars[high & 0xF];
         hex_out[pos++] = hex_chars[(low >> 12) & 0xF];
         hex_out[pos++] = hex_chars[(low >> 8) & 0xF];
         hex_out[pos++] = hex_chars[(low >> 4) & 0xF];
         hex_out[pos++] = hex_chars[low & 0xF];
      }
   }

   hex_out[pos] = '\0';
   return (int)pos;
}

int sms_ucs2_hex_to_utf8(const char *hex, char *utf8_out, size_t utf8_size) {
   if (!hex || !utf8_out || utf8_size < 2) {
      return -1;
   }

   size_t hex_len = strlen(hex);
   size_t pos = 0;
   size_t i = 0;

   while (i + 4 <= hex_len && pos + 4 < utf8_size) {
      uint16_t w1;
      if (parse_hex16(hex + i, &w1) != 0) {
         break;
      }
      i += 4;

      uint32_t cp;

      /* Check for UTF-16 surrogate pair */
      if (w1 >= 0xD800 && w1 <= 0xDBFF && i + 4 <= hex_len) {
         uint16_t w2;
         if (parse_hex16(hex + i, &w2) == 0 && w2 >= 0xDC00 && w2 <= 0xDFFF) {
            cp = 0x10000 + (((uint32_t)(w1 - 0xD800) << 10) | (w2 - 0xDC00));
            i += 4;
         } else {
            cp = '?'; /* broken surrogate */
         }
      } else {
         cp = w1;
      }

      /* Encode code point as UTF-8 */
      if (cp < 0x80) {
         utf8_out[pos++] = (char)cp;
      } else if (cp < 0x800 && pos + 2 < utf8_size) {
         utf8_out[pos++] = (char)(0xC0 | (cp >> 6));
         utf8_out[pos++] = (char)(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000 && pos + 3 < utf8_size) {
         utf8_out[pos++] = (char)(0xE0 | (cp >> 12));
         utf8_out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
         utf8_out[pos++] = (char)(0x80 | (cp & 0x3F));
      } else if (cp <= 0x10FFFF && pos + 4 < utf8_size) {
         utf8_out[pos++] = (char)(0xF0 | (cp >> 18));
         utf8_out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
         utf8_out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
         utf8_out[pos++] = (char)(0x80 | (cp & 0x3F));
      }
   }

   utf8_out[pos] = '\0';
   return (int)pos;
}
