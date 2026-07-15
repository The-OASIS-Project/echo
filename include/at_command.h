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
 * AT command send/receive API for the SIM7600G-H modem.
 */

#ifndef AT_COMMAND_H
#define AT_COMMAND_H

#include <pthread.h>
#include <stdbool.h>

#include "echo.h"

/* AT command result status */
typedef enum {
   AT_OK = 0,
   AT_ERROR,
   AT_CME_ERROR,
   AT_CMS_ERROR,
   AT_TIMEOUT,
   AT_NO_CARRIER,
   AT_BUSY,
   AT_NO_ANSWER,
   AT_PORT_ERROR,
} at_status_t;

/* AT command response — filled by the URC reader when a solicited response completes */
typedef struct {
   at_status_t status;
   char data[AT_RESPONSE_MAX]; /* data lines between command echo and OK/ERROR */
   int data_len;
   int error_code; /* numeric code from +CME ERROR or +CMS ERROR */
} at_response_t;

/* Pending command state — shared between main thread and URC reader */
typedef enum {
   AT_PENDING_NONE = 0,
   AT_PENDING_SYNC,  /* waiting for OK/ERROR */
   AT_PENDING_ASYNC, /* fire-and-forget (ATD, ATA) */
   AT_PENDING_SMS,   /* two-phase: waiting for '>' prompt, then body+Ctrl-Z */
} at_pending_type_t;

typedef struct {
   at_pending_type_t type;
   at_response_t response;
   bool completed;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
} at_pending_t;

/* Serial port context */
typedef struct {
   int fd;         /* serial file descriptor */
   char path[128]; /* device path (e.g. /dev/ttyUSB2) */
   int baud;       /* baud rate */
   at_pending_t pending;
   pthread_mutex_t write_mutex; /* serialize writes to the serial port */
} at_context_t;

/**
 * @brief Validate a serial-port path (pure syntax check; exposed for testing).
 *
 * Accepts a raw /dev/ttyUSB<n> / /dev/ttyACM<n> node (1-3 digit suffix) or a
 * udev stable alias under /dev/serial/by-id/ or /dev/serial/by-path/ (a single
 * path component — no '/', no ".."). Does not touch the filesystem. at_open()
 * additionally realpath()-resolves an alias and re-checks the resolved target is
 * a raw node, so open() only ever lands on a ttyUSB/ttyACM device.
 *
 * @return true if @p path is syntactically an acceptable serial-port path.
 */
bool validate_serial_path(const char *path);

/**
 * @brief Open the serial port with exclusive lock (flock).
 * @return 0 on success, -1 on error.
 */
int at_open(at_context_t *ctx, const char *port, int baud);

/**
 * @brief Close the serial port and release the lock.
 */
void at_close(at_context_t *ctx);

/**
 * @brief Send a synchronous AT command and wait for the response.
 *
 * Acquires the command mutex, writes the command, waits for OK/ERROR
 * from the URC reader thread (via condvar), returns the result.
 *
 * @param ctx       AT context with open serial port.
 * @param cmd       AT command string (e.g. "AT+CSQ").
 * @param response  Output: filled with the response data.
 * @param timeout_ms Maximum wait time in milliseconds.
 * @return AT_OK, AT_ERROR, AT_TIMEOUT, etc.
 */
at_status_t at_command_send(at_context_t *ctx,
                            const char *cmd,
                            at_response_t *response,
                            int timeout_ms);

/**
 * @brief Send an asynchronous AT command (ATD, ATA).
 *
 * Writes the command and returns immediately. The URC reader handles
 * the eventual result (CONNECT, NO CARRIER, BUSY) as events.
 *
 * @return AT_OK if the command was written, AT_PORT_ERROR on write failure.
 */
at_status_t at_command_send_async(at_context_t *ctx, const char *cmd);

/**
 * @brief Send an SMS using the two-phase AT+CMGS protocol.
 *
 * Phase 1: Send AT+CMGS="number", wait for '>' prompt.
 * Phase 2: Send body + Ctrl-Z (0x1A), wait for OK/ERROR.
 *
 * @param ctx       AT context.
 * @param number    Phone number (already validated).
 * @param body      SMS body (already sanitized).
 * @param response  Output response.
 * @return AT_OK on success, AT_ERROR/AT_TIMEOUT on failure.
 */
at_status_t at_command_send_sms(at_context_t *ctx,
                                const char *number,
                                const char *body,
                                at_response_t *response);

/**
 * @brief Send one PDU-mode SMS segment.
 *
 * Two-phase AT+CMGS with the `<octets>` argument, then hex PDU + Ctrl-Z:
 *   Phase 1: AT+CMGS=<tpdu_octets>\r  →  wait for '>' prompt.
 *   Phase 2: <pdu_hex>\x1A             →  wait for +CMGS / OK / ERROR.
 *
 * The hex string is validated for the hex alphabet before transmission —
 * an accidental non-hex byte sent in this mode triggers CMS ERROR 305 at
 * best and unpredictable modem state at worst.
 *
 * @param ctx         AT context.
 * @param tpdu_octets Length argument for AT+CMGS (TPDU only, not SMSC prefix).
 * @param pdu_hex     Full hex payload, including the "00" SMSC-default prefix.
 * @param response    Output response.
 * @return AT_OK or a failure status.
 */
at_status_t at_command_send_pdu(at_context_t *ctx,
                                int tpdu_octets,
                                const char *pdu_hex,
                                at_response_t *response);

/**
 * @brief Write raw bytes to the serial port (thread-safe).
 * @return Number of bytes written, or -1 on error.
 */
int at_write_raw(at_context_t *ctx, const void *buf, int len);

/**
 * @brief Parse a response line to determine if it's a terminator.
 *
 * Recognizes: OK, ERROR, +CME ERROR: N, +CMS ERROR: N, NO CARRIER,
 * BUSY, NO ANSWER, CONNECT.
 *
 * @param line     The line to check (null-terminated, CR/LF stripped).
 * @param status   Output: the status if this is a terminator.
 * @param err_code Output: numeric error code (for CME/CMS errors).
 * @return true if the line is a response terminator.
 */
bool at_parse_terminator(const char *line, at_status_t *status, int *err_code);

/**
 * @brief Return a human-readable string for an AT status code.
 */
const char *at_status_str(at_status_t status);

#endif /* AT_COMMAND_H */
