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
 * ECHO — Global types, configuration, and version definitions.
 */

#ifndef ECHO_H
#define ECHO_H

#include <stdbool.h>
#include <stdint.h>

/* Version */
#define ECHO_VERSION_MAJOR 1
#define ECHO_VERSION_MINOR 0
#define ECHO_VERSION_PATCH 0

/* Defaults */
#define ECHO_DEFAULT_SERIAL_PORT "/dev/ttyUSB2"
#define ECHO_DEFAULT_SERIAL_BAUD 115200
#define ECHO_DEFAULT_MQTT_HOST "localhost"
#define ECHO_DEFAULT_MQTT_PORT 8883
#define ECHO_DEFAULT_TELEMETRY_S 10
#define ECHO_DEFAULT_RATE_CALLS_H 5
#define ECHO_DEFAULT_RATE_SMS_H 20
/* Per-segment rate bucket for PDU mode. A single concat SMS can burn up to
 * PDU_MAX_SEGMENTS airtime units, so we budget this separately from the
 * per-message rate so a chatty user can't exhaust the network quota. */
#define ECHO_DEFAULT_RATE_SEGMENTS_H 200
/* Inter-segment pacing. T-Mobile + SIM7600 can wedge on back-to-back
 * concat sends without a breather. 150ms is gentle on both. */
#define ECHO_DEFAULT_SEGMENT_DELAY_MS 150

/* AT command limits */
#define AT_RESPONSE_MAX 4096    /* large enough for UCS2 hex SMS bodies */
#define AT_TIMEOUT_DEFAULT 2000 /* ms */
#define AT_TIMEOUT_SMS 60000    /* ms — AT+CMGS waits for network */
#define AT_TIMEOUT_DIAL 5000    /* ms — ATD returns quickly, result comes as URC */
/* Inbound SMS storage ops (CMGR read, CMGD delete) hit local modem memory
 * and normally return <100ms. A short timeout here matters because a
 * multi-segment SMS fires one CMTI per segment; at the 2s default, a 10-
 * segment message could block the main-thread command-queue drain for up
 * to 40s. 500ms caps that at ~10s and still leaves margin over real-world
 * modem latency. On timeout we fall back to logging + CMGD-fire-forget so
 * the inbox doesn't fill. */
#define AT_TIMEOUT_SMS_STORAGE 500

/* SMS limits */
#define SMS_BODY_MAX 800
#define SMS_BODY_HEX_MAX 3200 /* UCS2 hex: up to 4x body length (8x for all emoji) */
#define PHONE_NUMBER_MAX 20
#define PHONE_NUMBER_HEX_MAX 80 /* UCS2 hex: 4 hex chars per digit */

/* Call states (shared between modem.c and mqtt_comms.c) */
typedef enum {
   CALL_STATE_IDLE = 0,
   CALL_STATE_DIALING,
   CALL_STATE_RINGING_IN,
   CALL_STATE_ACTIVE,
   CALL_STATE_HANGING_UP,
} call_state_t;

/* Network registration states */
typedef enum {
   REG_NOT_REGISTERED = 0,
   REG_REGISTERED_HOME,
   REG_SEARCHING,
   REG_DENIED,
   REG_UNKNOWN,
   REG_REGISTERED_ROAMING,
} reg_state_t;

/* SIM status */
typedef enum {
   SIM_UNKNOWN = 0,
   SIM_READY,
   SIM_PIN_REQUIRED,
   SIM_PUK_REQUIRED,
   SIM_NOT_INSERTED,
   SIM_ERROR,
} sim_status_t;

/* Modem telemetry snapshot — published every TELEMETRY_INTERVAL seconds */
typedef struct {
   int signal_dbm;
   int signal_bars; /* 0-5 */
   int csq;         /* raw CSQ value 0-31, 99=unknown */
   reg_state_t reg;
   char operator_name[64];
   char network_type[16]; /* "LTE", "WCDMA", "GSM", etc. */
   call_state_t call_state;
   sim_status_t sim;
} modem_telemetry_t;

/* Runtime configuration — populated from CLI args + env */
typedef struct {
   char serial_port[128];
   int serial_baud;
   char mqtt_host[128];
   int mqtt_port;
   char mqtt_username[128];
   char mqtt_password[128];
   int mqtt_tls;
   char mqtt_ca_cert[256];
   int telemetry_interval_s;
   int rate_limit_calls_per_hour;
   int rate_limit_sms_per_hour;
   int rate_limit_segments_per_hour;
   int inter_segment_delay_ms;
   bool pdu_mode; /* true = PDU (AT+CMGF=0), false = legacy text mode */
   bool service_mode;
} echo_config_t;

/* Leaky-bucket rate limiter. Fills at `max_per_hour / 3600` tokens/sec up to
 * a `max_per_hour` ceiling; a `take_n()` spends N tokens atomically.
 *
 * Replaces an earlier ring-buffer design that silently capped active count
 * at 64 — the old limiter never rejected anything when `max_per_hour > 64`.
 * The counter form is correct at any configured limit and O(1) per call. */
typedef struct {
   double tokens;           /* current token balance (fractional) */
   int64_t last_refill_sec; /* wall clock seconds of last refill */
   int max_per_hour;        /* bucket ceiling + refill rate input */
} rate_bucket_t;

/**
 * @brief Initialize a rate limiter bucket, pre-filled to capacity.
 */
void rate_bucket_init(rate_bucket_t *bucket, int max_per_hour);

/**
 * @brief Try to consume one token; record it if available.
 * @return true if allowed, false if rate limited.
 */
bool rate_bucket_allow(rate_bucket_t *bucket);

/**
 * @brief Try to consume `n` tokens atomically (no partial debit on failure).
 * @return true if all N allowed, false if insufficient balance.
 */
bool rate_bucket_take_n(rate_bucket_t *bucket, int n);

#endif /* ECHO_H */
