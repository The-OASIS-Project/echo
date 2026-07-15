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
 * URC reader thread — single-reader design for the serial port.
 * Classifies lines as solicited responses (OK/ERROR) or unsolicited
 * result codes (RING, +CLIP, +CMTI, etc.).
 */

#include "urc_handler.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logging.h"
#include "sms.h"

/* ── Time helpers ────────────────────────────────────────────────────── */

int64_t urc_now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

/* ── URC classification ──────────────────────────────────────────────── */

bool urc_classify(const char *line, urc_event_t *event) {
   if (!line || !event) {
      return false;
   }

   memset(event, 0, sizeof(*event));
   snprintf(event->raw, sizeof(event->raw), "%s", line);

   /* RING */
   if (strcmp(line, "RING") == 0) {
      event->type = URC_RING;
      return true;
   }

   /* +CLIP: "number",type[,...] */
   if (strncmp(line, "+CLIP:", 6) == 0) {
      event->type = URC_CLIP;
      /* Extract quoted number */
      const char *quote1 = strchr(line + 6, '"');
      if (quote1) {
         const char *quote2 = strchr(quote1 + 1, '"');
         if (quote2) {
            size_t len = (size_t)(quote2 - quote1 - 1);
            if (len > PHONE_NUMBER_MAX) {
               len = PHONE_NUMBER_MAX;
            }
            memcpy(event->number, quote1 + 1, len);
            event->number[len] = '\0';

            /* Sanitize the extracted number */
            char clean[PHONE_NUMBER_MAX + 1];
            if (sms_sanitize_clip(event->number, clean, sizeof(clean))) {
               snprintf(event->number, sizeof(event->number), "%s", clean);
            } else {
               event->number[0] = '\0'; /* invalid number */
            }
         }
      }
      return true;
   }

   /* +CMTI: "SM",index */
   if (strncmp(line, "+CMTI:", 6) == 0) {
      event->type = URC_CMTI;
      const char *comma = strchr(line + 6, ',');
      if (comma) {
         event->index = atoi(comma + 1);
      }
      return true;
   }

   /* NO CARRIER */
   if (strcmp(line, "NO CARRIER") == 0) {
      event->type = URC_NO_CARRIER;
      return true;
   }

   /* BUSY */
   if (strcmp(line, "BUSY") == 0) {
      event->type = URC_BUSY;
      return true;
   }

   /* NO ANSWER */
   if (strcmp(line, "NO ANSWER") == 0) {
      event->type = URC_NO_ANSWER;
      return true;
   }

   /* CONNECT (after ATD success) */
   if (strncmp(line, "CONNECT", 7) == 0) {
      event->type = URC_CONNECT;
      return true;
   }

   /* VOICE CALL: BEGIN — SIM7600-specific call connected */
   if (strncmp(line, "VOICE CALL: BEGIN", 17) == 0) {
      event->type = URC_VOICE_CALL_BEGIN;
      return true;
   }

   /* VOICE CALL: END: HHMMSS — SIM7600-specific call ended with duration */
   if (strncmp(line, "VOICE CALL: END", 15) == 0) {
      event->type = URC_VOICE_CALL_END;
      return true;
   }

   /* +CREG: stat[,lac,ci] */
   if (strncmp(line, "+CREG:", 6) == 0) {
      event->type = URC_CREG;
      /* Parse registration status (first number after colon) */
      const char *p = line + 6;
      while (*p == ' ') {
         p++;
      }
      event->reg_stat = atoi(p);
      return true;
   }

   /* > (SMS prompt) */
   if (line[0] == '>' && (line[1] == ' ' || line[1] == '\0')) {
      event->type = URC_SMS_PROMPT;
      return true;
   }

   return false;
}

/* ── Line reading ────────────────────────────────────────────────────── */

/**
 * @brief Read one line from the serial port (blocking with short timeout).
 *
 * Reads until CR or LF, strips both. Returns the line length,
 * 0 on timeout (no data), or -1 on error.
 */
static int read_line(int fd, char *buf, int buf_size) {
   int pos = 0;

   while (pos < buf_size - 1) {
      char c;
      ssize_t n = read(fd, &c, 1);

      if (n < 0) {
         if (errno == EINTR) {
            continue;
         }
         return -1; /* real error */
      }
      if (n == 0) {
         /* Timeout (VTIME expired) — return what we have */
         if (pos > 0) {
            break;
         }
         return 0; /* nothing read */
      }

      /* Skip CR and use LF as line terminator */
      if (c == '\r') {
         continue;
      }
      if (c == '\n') {
         if (pos > 0) {
            break; /* complete line */
         }
         continue; /* skip leading LF */
      }

      buf[pos++] = c;
   }

   buf[pos] = '\0';
   return pos;
}

/* ── RING+CLIP merge ─────────────────────────────────────────────────── */

/**
 * @brief Check if a pending RING has timed out waiting for CLIP.
 *
 * If so, dispatch the RING event with an empty number (blocked caller ID).
 */
static void check_ring_timeout(urc_context_t *ctx) {
   if (!ctx->ring_pending) {
      return;
   }

   int64_t elapsed = urc_now_ms() - ctx->ring_timestamp_ms;
   if (elapsed >= URC_CLIP_TIMEOUT_MS) {
      /* Dispatch RING without CLIP (blocked/withheld caller ID) */
      ctx->ring_event.number[0] = '\0';
      if (ctx->callback) {
         ctx->callback(&ctx->ring_event, ctx->userdata);
      }
      ctx->ring_pending = false;
   }
}

/* ── Main reader thread ─────────────────────────────────────��────────── */

static void *urc_reader_thread(void *arg) {
   urc_context_t *ctx = (urc_context_t *)arg;
   at_context_t *at = ctx->at_ctx;
   char line[AT_RESPONSE_MAX];
   int consecutive_empty = 0;
   int64_t empty_run_start_ms = 0;

   OLOG_INFO("URC reader thread started");

   while (ctx->running) {
      /* Check for RING timeout */
      check_ring_timeout(ctx);

      /* Read one line from serial */
      int len = read_line(at->fd, line, sizeof(line));
      if (len < 0) {
         if (ctx->running) {
            OLOG_ERROR("Serial read error: %s", strerror(errno));
         }
         break;
      }
      if (len == 0) {
         if (consecutive_empty == 0) {
            empty_run_start_ms = urc_now_ms();
         }
         consecutive_empty++;
         /* With VTIME=1 (100ms), N legitimate timeouts take ~N*100ms.
          * If we see URC_MAX_CONSECUTIVE_EMPTY empties in far less time
          * than expected, read() is returning EOF instantly — the device
          * is gone.  The time threshold allows ~10ms per read (vs 100ms
          * for a real VTIME timeout) to absorb scheduling jitter. */
         if (consecutive_empty >= URC_MAX_CONSECUTIVE_EMPTY) {
            int64_t elapsed_ms = urc_now_ms() - empty_run_start_ms;
            int64_t expected_ms = (int64_t)consecutive_empty * URC_VTIME_EXPECT_MS;
            if (elapsed_ms < expected_ms) {
               OLOG_ERROR("Serial device disconnected (%d empty reads in %" PRId64
                          "ms, expected ~%" PRId64 "ms)",
                          consecutive_empty, elapsed_ms, expected_ms);
               break;
            }
            consecutive_empty = 0;
         }
         continue;
      }

      consecutive_empty = 0;

      /* Skip empty lines */
      if (line[0] == '\0') {
         continue;
      }

      /* Check if a command is pending and this is a response terminator */
      pthread_mutex_lock(&at->pending.mutex);
      at_pending_type_t pending_type = at->pending.type;

      if (pending_type == AT_PENDING_SYNC) {
         /* Check for response terminator */
         at_status_t status;
         int err_code;
         if (at_parse_terminator(line, &status, &err_code)) {
            at->pending.response.status = status;
            at->pending.response.error_code = err_code;
            at->pending.completed = true;
            pthread_cond_signal(&at->pending.cond);
            pthread_mutex_unlock(&at->pending.mutex);
            continue;
         }

         /* Accumulate data lines for the pending response */
         int remaining = (int)sizeof(at->pending.response.data) - at->pending.response.data_len - 1;
         if (remaining > 0) {
            int to_copy = len;
            if (to_copy > remaining - 1) {
               to_copy = remaining - 1;
            }
            /* Append with newline separator if not first line */
            if (at->pending.response.data_len > 0) {
               at->pending.response.data[at->pending.response.data_len++] = '\n';
               remaining--;
               if (to_copy > remaining) {
                  to_copy = remaining;
               }
            }
            memcpy(at->pending.response.data + at->pending.response.data_len, line,
                   (size_t)to_copy);
            at->pending.response.data_len += to_copy;
            at->pending.response.data[at->pending.response.data_len] = '\0';
         }
         pthread_mutex_unlock(&at->pending.mutex);
         continue;
      }

      if (pending_type == AT_PENDING_SMS) {
         /* Waiting for '>' prompt or ERROR */
         urc_event_t evt;
         if (urc_classify(line, &evt) && evt.type == URC_SMS_PROMPT) {
            at->pending.response.status = AT_OK; /* prompt received */
            at->pending.completed = true;
            pthread_cond_signal(&at->pending.cond);
            pthread_mutex_unlock(&at->pending.mutex);
            continue;
         }
         /* Check for error */
         at_status_t status;
         int err_code;
         if (at_parse_terminator(line, &status, &err_code)) {
            at->pending.response.status = status;
            at->pending.response.error_code = err_code;
            at->pending.completed = true;
            pthread_cond_signal(&at->pending.cond);
            pthread_mutex_unlock(&at->pending.mutex);
            continue;
         }
         pthread_mutex_unlock(&at->pending.mutex);
         continue;
      }

      pthread_mutex_unlock(&at->pending.mutex);

      /* Not a solicited response — try to classify as URC */
      urc_event_t event;
      if (!urc_classify(line, &event)) {
         /* Unknown line — could be command echo or garbage */
         continue;
      }

      /* Handle RING+CLIP merge */
      if (event.type == URC_RING) {
         ctx->ring_pending = true;
         ctx->ring_timestamp_ms = urc_now_ms();
         ctx->ring_event = event;
         continue; /* wait for CLIP */
      }

      if (event.type == URC_CLIP && ctx->ring_pending) {
         /* Merge CLIP into the pending RING event */
         ctx->ring_event.type = URC_RING;
         snprintf(ctx->ring_event.number, sizeof(ctx->ring_event.number), "%s", event.number);
         if (ctx->callback) {
            ctx->callback(&ctx->ring_event, ctx->userdata);
         }
         ctx->ring_pending = false;
         continue;
      }

      /* Dispatch other URCs directly */
      if (ctx->callback) {
         ctx->callback(&event, ctx->userdata);
      }
   }

   /* If we left the loop while still "running", the device disconnected or a read
    * errored (not a deliberate urc_stop()) — flag main to reconnect, don't quit.
    * A deliberate stop clears ctx->running first, so this is skipped for it. */
   if (ctx->running && ctx->disconnect_flag) {
      *ctx->disconnect_flag = 1;
   }
   OLOG_INFO("URC reader thread exiting");
   return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int urc_start(urc_context_t *ctx,
              at_context_t *at_ctx,
              urc_event_callback_t callback,
              void *userdata,
              volatile sig_atomic_t *disconnect_flag) {
   if (!ctx || !at_ctx) {
      return -1;
   }

   memset(ctx, 0, sizeof(*ctx));
   ctx->at_ctx = at_ctx;
   ctx->callback = callback;
   ctx->userdata = userdata;
   ctx->running = true;
   ctx->ring_pending = false;
   /* Wire the disconnect flag BEFORE the thread starts so a drop in the first
    * microseconds of the reader's life is still signalled (the reader reads it
    * on its exit path). */
   ctx->disconnect_flag = disconnect_flag;

   if (pthread_create(&ctx->thread, NULL, urc_reader_thread, ctx) != 0) {
      OLOG_ERROR("Failed to create URC reader thread: %s", strerror(errno));
      return -1;
   }
   ctx->started = true;

   return 0;
}

void urc_stop(urc_context_t *ctx) {
   if (!ctx || !ctx->started) {
      return; /* never started, or already stopped — idempotent no-op */
   }

   ctx->running = false;
   pthread_join(ctx->thread, NULL);
   ctx->started = false;
   OLOG_INFO("URC reader thread stopped");
}
