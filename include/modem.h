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
 * Modem initialization, signal polling, and health monitoring.
 */

#ifndef MODEM_H
#define MODEM_H

#include <stdbool.h>

#include "at_command.h"
#include "echo.h"

/**
 * @brief Run the modem initialization sequence.
 *
 * Sends: AT, ATE0, AT+CMEE=2, AT+CLIP=1, AT+CMGF=0 (PDU) or 1 (text),
 * AT+CNMI=2,1,0,0,0, AT+CREG=1, AT+CSDVC=1, AT+CLVL=3, AT+CSQ, AT+COPS?
 *
 * AT+CSMP (SMS parameters) is only sent in text mode — PDU mode carries the
 * DCS per-frame so the init-time default is irrelevant.
 *
 * @param at       AT context with open serial port.
 * @param pdu_mode true → AT+CMGF=0 (PDU), false → AT+CMGF=1 (text).
 * @return 0 on success (basic AT works), -1 on failure.
 */
int modem_init(at_context_t *at, bool pdu_mode);

/**
 * @brief Poll signal strength (AT+CSQ).
 *
 * @param at       AT context.
 * @param out_dbm  Output: signal in dBm.
 * @param out_csq  Output: raw CSQ value (0-31, 99=unknown).
 * @return 0 on success, -1 on error.
 */
int modem_poll_signal(at_context_t *at, int *out_dbm, int *out_csq);

/**
 * @brief Query network operator (AT+COPS?).
 *
 * @param at   AT context.
 * @param name Output: operator name (null-terminated).
 * @param size Size of name buffer.
 * @return 0 on success, -1 on error.
 */
int modem_query_operator(at_context_t *at, char *name, size_t size);

/**
 * @brief Query network type (AT+CNSMOD?).
 *
 * @param at   AT context.
 * @param type Output: network type string (e.g. "LTE").
 * @param size Size of type buffer.
 * @return 0 on success, -1 on error.
 */
int modem_query_network_type(at_context_t *at, char *type, size_t size);

/**
 * @brief Query SIM status (AT+CPIN?).
 *
 * @param at AT context.
 * @return SIM status enum value.
 */
sim_status_t modem_query_sim_status(at_context_t *at);

/**
 * @brief Send a heartbeat AT command to verify modem is responsive.
 *
 * @param at AT context.
 * @return true if modem responds OK, false on timeout/error.
 */
bool modem_heartbeat(at_context_t *at);

/**
 * @brief Convert CSQ value to dBm.
 *
 * CSQ 0 = -113 dBm, CSQ 31 = -51 dBm, 99 = unknown.
 */
int modem_csq_to_dbm(int csq);

/**
 * @brief Convert dBm to signal bars (0-5).
 *
 * -113 dBm = 0, -51 dBm = 5.
 */
int modem_dbm_to_bars(int dbm);

/**
 * @brief Build a telemetry snapshot from the modem's current state.
 *
 * @param at        AT context.
 * @param telem     Output: telemetry snapshot.
 * @param call_state Current call state (passed in from state machine).
 * @return 0 on success.
 */
int modem_build_telemetry(at_context_t *at, modem_telemetry_t *telem, call_state_t call_state);

/**
 * @brief Per-call audio setup: echo cancellation + USB PCM.
 *
 * AT+CECM=1, AT+CPCMFRM=1 (16 kHz wideband) and AT+CPCMREG=1 (start USB PCM on
 * the modem's USB audio interface) only work during an active voice call on the
 * SIM7600 — they return ERROR at idle.  Called when VOICE CALL: BEGIN or CONNECT
 * is detected.  AT+CPCMFRM resets to 8 kHz on modem reset, so it is set per call.
 *
 * @return true if USB PCM was armed (AT+CPCMREG=1 returned OK) so the caller can
 *         publish a pcm_ready event; false if PCM did not start.
 */
bool modem_call_audio_setup(at_context_t *at);

/**
 * @brief Per-call audio teardown: stop USB PCM (AT+CPCMREG=0).
 *
 * The teardown half of the SIMCom USB-audio sequence.  Best-effort / non-fatal:
 * returns ERROR at idle and may fail if the call already dropped and the USB
 * device re-enumerated.  Send it BEFORE a local hangup (while the call + USB
 * port are still up) so the modem drops USB audio cleanly instead of
 * re-enumerating the whole USB device at an abrupt call end.
 */
void modem_call_audio_teardown(at_context_t *at);

#endif /* MODEM_H */
