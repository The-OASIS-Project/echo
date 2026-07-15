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

#include "modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* ── Init sequence ───────────────────────────────────────────────────── */

/**
 * @brief Send a single init command and log the result.
 * @return 0 on OK, -1 on error/timeout.
 */
static int init_cmd(at_context_t *at, const char *cmd, const char *desc) {
   at_response_t resp;
   at_status_t rc = at_command_send(at, cmd, &resp, AT_TIMEOUT_DEFAULT);
   if (rc != AT_OK) {
      OLOG_WARNING("Modem init '%s' (%s): %s", cmd, desc, at_status_str(rc));
      return -1;
   }
   OLOG_INFO("Modem init: %s OK", desc);
   return 0;
}

int modem_init(at_context_t *at, bool pdu_mode) {
   if (!at) {
      return -1;
   }

   OLOG_INFO("Starting modem initialization sequence (mode=%s)", pdu_mode ? "PDU" : "text");

   /* Verify communication */
   if (init_cmd(at, "AT", "verify comm") != 0) {
      OLOG_ERROR("Modem not responding to AT");
      return -1;
   }

   /* Core setup — failures are warnings, not fatal */
   init_cmd(at, "ATE0", "disable echo");
   init_cmd(at, "AT+CMEE=2", "verbose errors");
   init_cmd(at, "AT+CLIP=1", "caller ID");
   if (pdu_mode) {
      init_cmd(at, "AT+CMGF=0", "SMS PDU mode");
   } else {
      /* Text-mode path keeps the DCS hint so UCS2 text-mode encodes still work. */
      init_cmd(at, "AT+CSMP=17,167,0,8", "SMS params UCS2 DCS");
      init_cmd(at, "AT+CMGF=1", "SMS text mode");
   }
   init_cmd(at, "AT+CPMS=\"ME\",\"ME\",\"ME\"", "SMS storage to ME");
   init_cmd(at, "AT+CNMI=2,1,0,0,0", "SMS notification URC");
   init_cmd(at, "AT+CREG=1", "network reg URC");

   /* Audio setup for voice calls (echo cancellation set per-call via modem_call_audio_setup) */
   init_cmd(at, "AT+CSDVC=1", "audio to headset jack");
   init_cmd(at, "AT+CLVL=3", "volume mid-level");

   /* Initial signal read */
   int dbm, csq;
   if (modem_poll_signal(at, &dbm, &csq) == 0) {
      OLOG_INFO("Initial signal: %d dBm (CSQ %d, %d bars)", dbm, csq, modem_dbm_to_bars(dbm));
   }

   /* Initial operator query */
   char oper[64];
   if (modem_query_operator(at, oper, sizeof(oper)) == 0) {
      OLOG_INFO("Operator: %s", oper);
   }

   OLOG_INFO("Modem initialization complete");
   return 0;
}

/* ── Signal polling ──────────────────────────────────────────────────── */

int modem_csq_to_dbm(int csq) {
   if (csq == 99 || csq < 0 || csq > 31) {
      return -999; /* unknown */
   }
   return -113 + (csq * 2);
}

int modem_dbm_to_bars(int dbm) {
   if (dbm <= -113 || dbm == -999) {
      return 0;
   }
   if (dbm >= -51) {
      return 5;
   }
   /* Linear map: -113 dBm = 0, -51 dBm = 5 */
   int bars = (dbm + 113) * 5 / 62;
   if (bars < 0) {
      bars = 0;
   }
   if (bars > 5) {
      bars = 5;
   }
   return bars;
}

int modem_poll_signal(at_context_t *at, int *out_dbm, int *out_csq) {
   if (!at) {
      return -1;
   }

   at_response_t resp;
   at_status_t rc = at_command_send(at, "AT+CSQ", &resp, AT_TIMEOUT_DEFAULT);
   if (rc != AT_OK) {
      OLOG_WARNING("AT+CSQ failed: %s", at_status_str(rc));
      return -1;
   }

   /* Parse "+CSQ: rssi,ber" */
   int rssi = 99, ber = 99;
   const char *p = strstr(resp.data, "+CSQ:");
   if (p) {
      sscanf(p, "+CSQ: %d,%d", &rssi, &ber);
   }

   int dbm = modem_csq_to_dbm(rssi);
   if (out_dbm) {
      *out_dbm = dbm;
   }
   if (out_csq) {
      *out_csq = rssi;
   }

   return 0;
}

/* ── Operator query ──────────────────────────────────────────────────── */

int modem_query_operator(at_context_t *at, char *name, size_t size) {
   if (!at || !name || size == 0) {
      return -1;
   }

   name[0] = '\0';

   at_response_t resp;
   at_status_t rc = at_command_send(at, "AT+COPS?", &resp, AT_TIMEOUT_DEFAULT);
   if (rc != AT_OK) {
      return -1;
   }

   /* Parse '+COPS: mode,format,"operator",act' */
   const char *q1 = strchr(resp.data, '"');
   if (q1) {
      const char *q2 = strchr(q1 + 1, '"');
      if (q2) {
         size_t len = (size_t)(q2 - q1 - 1);
         if (len >= size) {
            len = size - 1;
         }
         memcpy(name, q1 + 1, len);
         name[len] = '\0';
         return 0;
      }
   }

   snprintf(name, size, "Unknown");
   return 0;
}

/* ── Network type ────────────────────────────────────────────────────── */

static const char *network_type_names[] = {
   [0] = "No service", [1] = "GSM",        [2] = "GPRS",  [3] = "EDGE",  [4] = "WCDMA",
   [5] = "HSDPA",      [6] = "HSUPA",      [7] = "HSPA",  [8] = "LTE",   [9] = "TDS-CDMA",
   [10] = "TDS-HSDPA", [11] = "TDS-HSUPA", [12] = "CDMA", [13] = "EVDO", [14] = "HYBRID",
   [15] = "1xRTT",     [16] = "eHRPD",
};
#define NETWORK_TYPE_COUNT (int)(sizeof(network_type_names) / sizeof(network_type_names[0]))

int modem_query_network_type(at_context_t *at, char *type, size_t size) {
   if (!at || !type || size == 0) {
      return -1;
   }

   type[0] = '\0';

   at_response_t resp;
   at_status_t rc = at_command_send(at, "AT+CNSMOD?", &resp, AT_TIMEOUT_DEFAULT);
   if (rc != AT_OK) {
      snprintf(type, size, "Unknown");
      return -1;
   }

   /* Parse "+CNSMOD: mode,sysmode" */
   int mode = 0, sysmode = 0;
   const char *p = strstr(resp.data, "+CNSMOD:");
   if (p) {
      sscanf(p, "+CNSMOD: %d,%d", &mode, &sysmode);
   }

   if (sysmode >= 0 && sysmode < NETWORK_TYPE_COUNT && network_type_names[sysmode]) {
      snprintf(type, size, "%s", network_type_names[sysmode]);
   } else {
      snprintf(type, size, "Unknown (%d)", sysmode);
   }

   return 0;
}

/* ── SIM status ──────────────────────────────────────────────────────── */

sim_status_t modem_query_sim_status(at_context_t *at) {
   if (!at) {
      return SIM_ERROR;
   }

   at_response_t resp;
   at_status_t rc = at_command_send(at, "AT+CPIN?", &resp, AT_TIMEOUT_DEFAULT);
   if (rc == AT_CME_ERROR) {
      if (resp.error_code == 10) {
         return SIM_NOT_INSERTED;
      }
      return SIM_ERROR;
   }
   if (rc != AT_OK) {
      return SIM_ERROR;
   }

   if (strstr(resp.data, "READY")) {
      return SIM_READY;
   }
   if (strstr(resp.data, "SIM PIN")) {
      return SIM_PIN_REQUIRED;
   }
   if (strstr(resp.data, "SIM PUK")) {
      return SIM_PUK_REQUIRED;
   }

   return SIM_UNKNOWN;
}

/* ── Call audio setup ─────────────────────────────────────────────────── */

bool modem_call_audio_setup(at_context_t *at) {
   at_response_t resp;
   if (at_command_send(at, "AT+CECM=1", &resp, AT_TIMEOUT_DEFAULT) == AT_OK) {
      OLOG_INFO("Echo cancellation enabled");
   } else {
      OLOG_WARNING("Echo cancellation failed (AT+CECM=1)");
   }

   /* Wideband 16 kHz USB PCM.  Resets to 8 kHz on modem reset, so set per call.
    * Non-fatal on failure — PCM still works at 8 kHz, but DAWN's bridge expects
    * 16 kHz, so log loudly. */
   if (at_command_send(at, "AT+CPCMFRM=1", &resp, AT_TIMEOUT_DEFAULT) != AT_OK) {
      OLOG_WARNING("USB PCM 16 kHz set failed (AT+CPCMFRM=1)");
   }

   /* Start USB PCM transfer on the modem's USB audio interface (DAWN opens the
    * corresponding /dev node).  This is what makes call audio flow to DAWN; gate
    * pcm_ready on its success. */
   if (at_command_send(at, "AT+CPCMREG=1", &resp, AT_TIMEOUT_DEFAULT) == AT_OK) {
      OLOG_INFO("USB PCM started (16 kHz S16LE)");
      return true;
   }
   OLOG_WARNING("USB PCM start failed (AT+CPCMREG=1) — no call audio to DAWN");
   return false;
}

void modem_call_audio_teardown(at_context_t *at) {
   at_response_t resp;
   /* Stop USB PCM (AT+CPCMREG=0), the teardown half of the SIMCom USB-audio
    * sequence.  Best-effort: at idle the SIM7600 returns ERROR, and if the call
    * already dropped the USB device may have re-enumerated so the AT port is
    * gone — either way this is non-fatal.  Sending it BEFORE a local hangup
    * (while the call + port are still up) is what lets the modem drop USB audio
    * cleanly instead of re-enumerating the whole USB device at an abrupt end. */
   if (at_command_send(at, "AT+CPCMREG=0", &resp, AT_TIMEOUT_DEFAULT) == AT_OK) {
      OLOG_INFO("USB PCM stopped (AT+CPCMREG=0)");
   } else {
      OLOG_INFO("USB PCM stop skipped (AT+CPCMREG=0 non-OK — idle or port gone)");
   }
}

/* ── Heartbeat ───────────────────────────────────────────────────────── */

bool modem_heartbeat(at_context_t *at) {
   at_response_t resp;
   return at_command_send(at, "AT", &resp, AT_TIMEOUT_DEFAULT) == AT_OK;
}

/* ── Telemetry builder ───────────────────────────────────────────────── */

/* Cached slow-changing fields (refreshed every 60s or on CREG change) */
static char cached_operator[64] = "";
static char cached_network_type[16] = "";
static sim_status_t cached_sim = SIM_UNKNOWN;
static time_t last_slow_poll = 0;
#define SLOW_POLL_INTERVAL_S 60

int modem_build_telemetry(at_context_t *at, modem_telemetry_t *telem, call_state_t call_state) {
   if (!at || !telem) {
      return -1;
   }

   memset(telem, 0, sizeof(*telem));

   /* Signal — always poll (fast, changes frequently) */
   modem_poll_signal(at, &telem->signal_dbm, &telem->csq);
   telem->signal_bars = modem_dbm_to_bars(telem->signal_dbm);

   /* Registration — always poll (fast, needed for state tracking) */
   at_response_t resp;
   if (at_command_send(at, "AT+CREG?", &resp, AT_TIMEOUT_DEFAULT) == AT_OK) {
      int n = 0, stat = 0;
      const char *p = strstr(resp.data, "+CREG:");
      if (p) {
         sscanf(p, "+CREG: %d,%d", &n, &stat);
      }
      if (stat >= 0 && stat <= 5) {
         telem->reg = (reg_state_t)stat;
      } else {
         telem->reg = REG_UNKNOWN;
      }
   }

   /* Slow-changing fields: operator, network type, SIM (every 60s) */
   time_t now = time(NULL);
   if (now - last_slow_poll >= SLOW_POLL_INTERVAL_S || cached_operator[0] == '\0') {
      modem_query_operator(at, cached_operator, sizeof(cached_operator));
      modem_query_network_type(at, cached_network_type, sizeof(cached_network_type));
      cached_sim = modem_query_sim_status(at);
      last_slow_poll = now;
   }

   snprintf(telem->operator_name, sizeof(telem->operator_name), "%s", cached_operator);
   snprintf(telem->network_type, sizeof(telem->network_type), "%s", cached_network_type);
   telem->sim = cached_sim;

   /* Call state — passed in from the main daemon's state machine */
   telem->call_state = call_state;

   return 0;
}
