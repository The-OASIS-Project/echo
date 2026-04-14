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

#ifndef SMS_H
#define SMS_H

#include <stdbool.h>
#include <stddef.h>

#include "echo.h"

/**
 * @brief Validate a phone number against the allowed pattern.
 *
 * Pattern: ^[+*#0-9]{1,20}$
 * Allows digits, +, *, # (for DTMF/prefix codes). Max 20 chars.
 *
 * @param number  Null-terminated phone number string.
 * @return true if valid, false if empty, too long, or contains bad chars.
 */
bool sms_validate_number(const char *number);

/**
 * @brief Sanitize an SMS body for safe AT+CMGS transmission.
 *
 * Strips control characters (0x00-0x1F) except newline (0x0A).
 * Rejects the body entirely if it contains Ctrl-Z (0x1A) or ESC (0x1B)
 * since these could terminate the SMS and execute AT commands.
 * Truncates to SMS_BODY_MAX bytes.
 *
 * @param body     Input body (null-terminated).
 * @param out      Output buffer (must be at least SMS_BODY_MAX + 1 bytes).
 * @param out_size Size of the output buffer.
 * @return Length of sanitized body, or -1 if body is rejected (injection attempt).
 */
int sms_sanitize_body(const char *body, char *out, size_t out_size);

/**
 * @brief Sanitize a CLIP (caller ID) string from an incoming call.
 *
 * Strips non-printable characters, validates number format, caps length.
 *
 * @param clip     Raw CLIP value from modem.
 * @param out      Output buffer (must be at least PHONE_NUMBER_MAX + 1 bytes).
 * @param out_size Size of the output buffer.
 * @return true if a valid number was extracted, false otherwise.
 */
bool sms_sanitize_clip(const char *clip, char *out, size_t out_size);

#endif /* SMS_H */
