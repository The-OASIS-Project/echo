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
 * URC (Unsolicited Result Code) reader thread.
 * Single-reader design: this thread owns ALL serial reads.
 */

#ifndef URC_HANDLER_H
#define URC_HANDLER_H

#include <stdbool.h>

#include "at_command.h"
#include "echo.h"

/* URC event types */
typedef enum {
   URC_RING = 0,         /* RING — incoming call */
   URC_CLIP,             /* +CLIP: "number",type — caller ID */
   URC_CMTI,             /* +CMTI: "SM",index — new SMS notification */
   URC_NO_CARRIER,       /* NO CARRIER — call ended by remote */
   URC_BUSY,             /* BUSY — called party busy */
   URC_NO_ANSWER,        /* NO ANSWER — no answer from called party */
   URC_CONNECT,          /* CONNECT — call connected (after ATD) */
   URC_VOICE_CALL_BEGIN, /* VOICE CALL: BEGIN — SIM7600 call connected */
   URC_VOICE_CALL_END,   /* VOICE CALL: END — SIM7600 call ended with duration */
   URC_CREG,             /* +CREG: stat[,lac,ci] — network registration change */
   URC_SMS_PROMPT,       /* > — SMS body prompt (for AT+CMGS) */
   URC_UNKNOWN,          /* unrecognized line */
} urc_type_t;

/* Parsed URC event */
typedef struct {
   urc_type_t type;
   char number[PHONE_NUMBER_MAX + 1]; /* phone number (CLIP, CMTI sender) */
   int index;                         /* SMS index (CMTI) */
   int reg_stat;                      /* registration status (CREG) */
   char raw[AT_RESPONSE_MAX];         /* raw line for debugging */
} urc_event_t;

/**
 * @brief Callback invoked by the URC reader when an event is ready.
 *
 * Called from the URC reader thread — keep processing short or
 * dispatch to the main thread via a queue.
 */
typedef void (*urc_event_callback_t)(const urc_event_t *event, void *userdata);

/* URC reader context */
typedef struct {
   at_context_t *at_ctx;          /* shared AT context (serial fd + pending state) */
   pthread_t thread;              /* reader thread handle */
   volatile bool running;         /* set to false to stop the reader */
   urc_event_callback_t callback; /* event callback */
   void *userdata;                /* opaque userdata for callback */

   /* RING+CLIP merge state */
   bool ring_pending;         /* RING received, waiting for CLIP */
   int64_t ring_timestamp_ms; /* when RING was received */
   urc_event_t ring_event;    /* buffered RING event */
} urc_context_t;

/* RING+CLIP merge timeout (ms) */
#define URC_CLIP_TIMEOUT_MS 300

/**
 * @brief Start the URC reader thread.
 *
 * @param ctx       URC context to initialize.
 * @param at_ctx    AT context with open serial port.
 * @param callback  Function called for each URC event.
 * @param userdata  Opaque pointer passed to callback.
 * @return 0 on success, -1 on error.
 */
int urc_start(urc_context_t *ctx,
              at_context_t *at_ctx,
              urc_event_callback_t callback,
              void *userdata);

/**
 * @brief Stop the URC reader thread and wait for it to exit.
 */
void urc_stop(urc_context_t *ctx);

/**
 * @brief Classify a serial line as a URC and parse its fields.
 *
 * Public for unit testing.
 *
 * @param line  Null-terminated line (CR/LF already stripped).
 * @param event Output: parsed event.
 * @return true if the line is a recognized URC, false otherwise.
 */
bool urc_classify(const char *line, urc_event_t *event);

/**
 * @brief Get the current time in milliseconds (monotonic clock).
 */
int64_t urc_now_ms(void);

#endif /* URC_HANDLER_H */
