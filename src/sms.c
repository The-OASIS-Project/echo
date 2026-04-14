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

#include <string.h>

#include "logging.h"

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
