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
 * AT command send/receive for the SIM7600G-H modem.
 * Serial port open/close with flock, sync/async command dispatch,
 * AT+CMGS two-phase SMS protocol, response terminator parsing.
 */

#include "at_command.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "logging.h"
#include "pdu.h"

/* ── Helpers ─────────────────────────────────────────────────────────── */

/**
 * @brief Map an integer baud rate to a termios speed constant.
 */
static speed_t baud_to_speed(int baud) {
   switch (baud) {
      case 9600:
         return B9600;
      case 19200:
         return B19200;
      case 38400:
         return B38400;
      case 57600:
         return B57600;
      case 115200:
         return B115200;
      case 230400:
         return B230400;
      case 460800:
         return B460800;
      default:
         OLOG_WARNING("Unknown baud rate %d, defaulting to 115200", baud);
         return B115200;
   }
}

/* NOTE: the serial-path acceptance rule below (is_raw_tty_node / is_serial_alias
 * / validate_serial_path) is mirrored in DAWN's src/tools/phone_audio_config.c
 * (phone_pcm_port_validate) — the two daemons are independent binaries with no
 * shared lib, so this is a deliberate copy. Keep the two in sync if you change
 * what a valid modem serial/PCM path looks like. */

/**
 * @brief A raw /dev/ttyUSB<n> or /dev/ttyACM<n> node (1-3 digit suffix).
 */
static bool is_raw_tty_node(const char *path) {
   size_t prefix_len;
   if (strncmp(path, "/dev/ttyUSB", 11) == 0 || strncmp(path, "/dev/ttyACM", 11) == 0) {
      prefix_len = 11;
   } else {
      return false;
   }

   /* Suffix must be 1-3 digits only (e.g. "0", "2", "10") */
   const char *suffix = path + prefix_len;
   size_t slen = strlen(suffix);
   if (slen == 0 || slen > 3) {
      return false;
   }
   for (size_t i = 0; i < slen; i++) {
      if (suffix[i] < '0' || suffix[i] > '9') {
         return false;
      }
   }
   return true;
}

/**
 * @brief A udev stable-alias path under /dev/serial/by-id/ or /dev/serial/by-path/.
 *
 * Must name a single entry: non-empty, no '/' in the name (no nested path) and
 * no ".." token.  These aliases are immune to ttyUSB enumeration-order shifts.
 */
static bool is_serial_alias(const char *path) {
   const char *name;
   if (strncmp(path, "/dev/serial/by-id/", 18) == 0) {
      name = path + 18;
   } else if (strncmp(path, "/dev/serial/by-path/", 20) == 0) {
      name = path + 20;
   } else {
      return false;
   }
   if (name[0] == '\0' || strchr(name, '/') != NULL || strstr(name, "..") != NULL) {
      return false;
   }
   return true;
}

/**
 * @brief Validate that a serial port path looks safe (pure syntax check).
 *
 * Accepts a raw /dev/ttyUSB<n> / /dev/ttyACM<n> node, or a udev stable alias
 * under /dev/serial/by-id/ or /dev/serial/by-path/.  Aliases are resolved and
 * re-validated against the raw-node rule in at_open() (realpath), so the actual
 * open() only ever lands on a ttyUSB/ttyACM node — path traversal stays blocked.
 */
bool validate_serial_path(const char *path) {
   if (!path || path[0] == '\0') {
      return false;
   }
   return is_raw_tty_node(path) || is_serial_alias(path);
}

/* ── Serial port ─────────────────────────────────────────────────────── */

int at_open(at_context_t *ctx, const char *port, int baud) {
   if (!ctx || !port) {
      return -1;
   }

   memset(ctx, 0, sizeof(*ctx));
   ctx->fd = -1;

   if (!validate_serial_path(port)) {
      OLOG_ERROR("Invalid serial port path: %s (need /dev/ttyUSB*/ttyACM* or a "
                 "/dev/serial/by-id|by-path alias)",
                 port);
      return -1;
   }

   /* Resolve to the concrete /dev/ttyUSB<n> node: a raw path resolves to itself,
    * a stable alias resolves through its symlink, and either way the target must
    * be a raw ttyUSB/ttyACM node.  We open the RESOLVED path (never the alias),
    * so the by-id indirection is honored while open() lands on a validated node
    * with O_NOFOLLOW meaningful (the resolved node is not itself a symlink). */
   char resolved[PATH_MAX]; /* realpath() writes up to PATH_MAX — don't shrink */
   if (!realpath(port, resolved) || !is_raw_tty_node(resolved)) {
      OLOG_ERROR("Serial port %s does not resolve to a /dev/ttyUSB*/ttyACM* node", port);
      return -1;
   }
   if (strcmp(resolved, port) != 0) {
      OLOG_INFO("Serial port %s -> %s", port, resolved);
   }

   /* is_raw_tty_node() bounds resolved to ~15 chars, but realpath()'s buffer is
    * PATH_MAX; the explicit guard keeps ctx->path[128] safe and silences the
    * -Wformat-truncation heuristic. */
   size_t rlen = strlen(resolved);
   if (rlen >= sizeof(ctx->path)) {
      OLOG_ERROR("Resolved serial path too long (%zu bytes): %s", rlen, resolved);
      return -1;
   }
   memcpy(ctx->path, resolved, rlen + 1);
   ctx->baud = baud;

   /* Open in non-blocking mode first to avoid hanging on modem lines.
    * O_NOFOLLOW: the resolved node must not be a symlink swapped in after
    * realpath(). O_CLOEXEC: don't leak the modem fd across exec. */
   ctx->fd = open(resolved, O_RDWR | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (ctx->fd < 0) {
      OLOG_ERROR("Failed to open %s: %s", resolved, strerror(errno));
      return -1;
   }

   /* Confirm we opened an actual character device, not a regular file. */
   struct stat st;
   if (fstat(ctx->fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
      OLOG_ERROR("%s is not a character device", resolved);
      close(ctx->fd);
      ctx->fd = -1;
      return -1;
   }

   /* Acquire exclusive lock */
   if (flock(ctx->fd, LOCK_EX | LOCK_NB) < 0) {
      OLOG_ERROR("Failed to lock %s (another process owns it): %s", resolved, strerror(errno));
      close(ctx->fd);
      ctx->fd = -1;
      return -1;
   }

   /* Switch back to blocking mode for reads */
   int flags = fcntl(ctx->fd, F_GETFL, 0);
   fcntl(ctx->fd, F_SETFL, flags & ~O_NONBLOCK);

   /* Configure the serial port */
   struct termios tio;
   memset(&tio, 0, sizeof(tio));
   if (tcgetattr(ctx->fd, &tio) < 0) {
      OLOG_ERROR("tcgetattr failed: %s", strerror(errno));
      close(ctx->fd);
      ctx->fd = -1;
      return -1;
   }

   speed_t speed = baud_to_speed(baud);
   cfsetispeed(&tio, speed);
   cfsetospeed(&tio, speed);

   /* Raw mode — no echo, no signals, no canonical processing */
   tio.c_cflag |= (CLOCAL | CREAD);
   tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
   tio.c_cflag |= CS8;
   tio.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                    ICRNL);
   tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
   tio.c_oflag &= ~OPOST;

   /* Read returns after 1 byte or 100ms timeout (VTIME in tenths of a second) */
   tio.c_cc[VMIN] = 0;
   tio.c_cc[VTIME] = 1;

   if (tcsetattr(ctx->fd, TCSANOW, &tio) < 0) {
      OLOG_ERROR("tcsetattr failed: %s", strerror(errno));
      close(ctx->fd);
      ctx->fd = -1;
      return -1;
   }

   tcflush(ctx->fd, TCIOFLUSH);

   /* Init synchronization primitives */
   pthread_mutex_init(&ctx->pending.mutex, NULL);
   pthread_cond_init(&ctx->pending.cond, NULL);
   pthread_mutex_init(&ctx->write_mutex, NULL);
   ctx->pending.type = AT_PENDING_NONE;
   ctx->pending.completed = false;

   OLOG_INFO("Serial port %s opened at %d baud (fd=%d)", port, baud, ctx->fd);
   return 0;
}

void at_close(at_context_t *ctx) {
   if (!ctx) {
      return;
   }
   if (ctx->fd >= 0) {
      flock(ctx->fd, LOCK_UN);
      close(ctx->fd);
      OLOG_INFO("Serial port %s closed", ctx->path);
      ctx->fd = -1;
      /* The sync primitives are initialized only on a fully-successful at_open
       * (below the fd-open, always with fd >= 0), so destroy them here inside the
       * fd guard.  This makes at_close idempotent: a second call — e.g. the
       * shutdown teardown after handle_serial_reconnect already closed on a
       * never-returning modem — is a safe no-op instead of a double-destroy. */
      pthread_mutex_destroy(&ctx->pending.mutex);
      pthread_cond_destroy(&ctx->pending.cond);
      pthread_mutex_destroy(&ctx->write_mutex);
   }
}

/* ── Write ───────────────────────────────────────────────────────────── */

int at_write_raw(at_context_t *ctx, const void *buf, int len) {
   if (!ctx || ctx->fd < 0 || !buf || len <= 0) {
      return -1;
   }

   pthread_mutex_lock(&ctx->write_mutex);
   int written = (int)write(ctx->fd, buf, (size_t)len);
   pthread_mutex_unlock(&ctx->write_mutex);

   if (written < 0) {
      OLOG_ERROR("Serial write failed: %s", strerror(errno));
   }
   return written;
}

/**
 * @brief Write an AT command string followed by CR.
 */
static int at_write_cmd(at_context_t *ctx, const char *cmd) {
   char buf[AT_RESPONSE_MAX];
   int len = snprintf(buf, sizeof(buf), "%s\r", cmd);
   if (len <= 0 || len >= (int)sizeof(buf)) {
      return -1;
   }
   return at_write_raw(ctx, buf, len);
}

/* ── Sync command ────────────────────────────────────────────────────── */

at_status_t at_command_send(at_context_t *ctx,
                            const char *cmd,
                            at_response_t *response,
                            int timeout_ms) {
   if (!ctx || ctx->fd < 0 || !cmd) {
      return AT_PORT_ERROR;
   }

   /* Prepare pending state */
   pthread_mutex_lock(&ctx->pending.mutex);
   ctx->pending.type = AT_PENDING_SYNC;
   ctx->pending.completed = false;
   memset(&ctx->pending.response, 0, sizeof(ctx->pending.response));
   pthread_mutex_unlock(&ctx->pending.mutex);

   /* Write the command */
   if (at_write_cmd(ctx, cmd) < 0) {
      pthread_mutex_lock(&ctx->pending.mutex);
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return AT_PORT_ERROR;
   }

   /* Wait for the URC reader to signal completion */
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += timeout_ms / 1000;
   ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
   if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
   }

   pthread_mutex_lock(&ctx->pending.mutex);
   while (!ctx->pending.completed) {
      int rc = pthread_cond_timedwait(&ctx->pending.cond, &ctx->pending.mutex, &ts);
      if (rc == ETIMEDOUT) {
         ctx->pending.type = AT_PENDING_NONE;
         pthread_mutex_unlock(&ctx->pending.mutex);
         OLOG_WARNING("AT command timed out: %s", cmd);
         if (response) {
            response->status = AT_TIMEOUT;
            response->data[0] = '\0';
            response->data_len = 0;
         }
         return AT_TIMEOUT;
      }
   }

   /* Copy result */
   at_status_t status = ctx->pending.response.status;
   if (response) {
      *response = ctx->pending.response;
   }
   ctx->pending.type = AT_PENDING_NONE;
   pthread_mutex_unlock(&ctx->pending.mutex);

   return status;
}

/* ── Async command ───────────────────────────────────────────────────── */

at_status_t at_command_send_async(at_context_t *ctx, const char *cmd) {
   if (!ctx || ctx->fd < 0 || !cmd) {
      return AT_PORT_ERROR;
   }

   /* Mark as async so the URC reader knows not to wait for OK/ERROR */
   pthread_mutex_lock(&ctx->pending.mutex);
   ctx->pending.type = AT_PENDING_ASYNC;
   ctx->pending.completed = false;
   pthread_mutex_unlock(&ctx->pending.mutex);

   if (at_write_cmd(ctx, cmd) < 0) {
      pthread_mutex_lock(&ctx->pending.mutex);
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return AT_PORT_ERROR;
   }

   /* Release immediately — result comes as URC */
   pthread_mutex_lock(&ctx->pending.mutex);
   ctx->pending.type = AT_PENDING_NONE;
   pthread_mutex_unlock(&ctx->pending.mutex);

   return AT_OK;
}

/* ── SMS two-phase ───────────────────────────────────────────────────── */

at_status_t at_command_send_sms(at_context_t *ctx,
                                const char *number,
                                const char *body,
                                at_response_t *response) {
   if (!ctx || ctx->fd < 0 || !number || !body) {
      return AT_PORT_ERROR;
   }

   /* Phase 1: Send AT+CMGS="number" and wait for '>' prompt */
   char cmgs[64];
   snprintf(cmgs, sizeof(cmgs), "AT+CMGS=\"%s\"", number);

   pthread_mutex_lock(&ctx->pending.mutex);
   ctx->pending.type = AT_PENDING_SMS;
   ctx->pending.completed = false;
   memset(&ctx->pending.response, 0, sizeof(ctx->pending.response));
   pthread_mutex_unlock(&ctx->pending.mutex);

   if (at_write_cmd(ctx, cmgs) < 0) {
      pthread_mutex_lock(&ctx->pending.mutex);
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return AT_PORT_ERROR;
   }

   /* Wait for '>' prompt (URC reader signals when it sees '>' or ERROR) */
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += AT_TIMEOUT_SMS / 1000;
   ts.tv_nsec += (AT_TIMEOUT_SMS % 1000) * 1000000L;
   if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
   }

   pthread_mutex_lock(&ctx->pending.mutex);
   while (!ctx->pending.completed) {
      int rc = pthread_cond_timedwait(&ctx->pending.cond, &ctx->pending.mutex, &ts);
      if (rc == ETIMEDOUT) {
         /* Send ESC to abort the SMS */
         char esc = 0x1B;
         at_write_raw(ctx, &esc, 1);
         ctx->pending.type = AT_PENDING_NONE;
         pthread_mutex_unlock(&ctx->pending.mutex);
         OLOG_ERROR("SMS prompt timeout for AT+CMGS");
         if (response) {
            response->status = AT_TIMEOUT;
         }
         return AT_TIMEOUT;
      }
   }

   /* Check if we got the prompt or an error */
   if (ctx->pending.response.status != AT_OK) {
      at_status_t status = ctx->pending.response.status;
      if (response) {
         *response = ctx->pending.response;
      }
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return status;
   }
   pthread_mutex_unlock(&ctx->pending.mutex);

   /* Phase 2: Send body + Ctrl-Z */
   char sms_buf[SMS_BODY_MAX + 4];
   int body_len = (int)strlen(body);
   if (body_len > SMS_BODY_MAX) {
      body_len = SMS_BODY_MAX;
   }
   memcpy(sms_buf, body, (size_t)body_len);
   sms_buf[body_len] = 0x1A; /* Ctrl-Z terminates SMS */
   body_len++;

   /* Reset pending for the OK/ERROR after Ctrl-Z */
   pthread_mutex_lock(&ctx->pending.mutex);
   ctx->pending.type = AT_PENDING_SYNC;
   ctx->pending.completed = false;
   memset(&ctx->pending.response, 0, sizeof(ctx->pending.response));
   pthread_mutex_unlock(&ctx->pending.mutex);

   if (at_write_raw(ctx, sms_buf, body_len) < 0) {
      pthread_mutex_lock(&ctx->pending.mutex);
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return AT_PORT_ERROR;
   }

   /* Wait for final OK/ERROR */
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += AT_TIMEOUT_SMS / 1000;
   ts.tv_nsec += (AT_TIMEOUT_SMS % 1000) * 1000000L;
   if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
   }

   pthread_mutex_lock(&ctx->pending.mutex);
   while (!ctx->pending.completed) {
      int rc = pthread_cond_timedwait(&ctx->pending.cond, &ctx->pending.mutex, &ts);
      if (rc == ETIMEDOUT) {
         ctx->pending.type = AT_PENDING_NONE;
         pthread_mutex_unlock(&ctx->pending.mutex);
         OLOG_ERROR("SMS send timeout after body");
         if (response) {
            response->status = AT_TIMEOUT;
         }
         return AT_TIMEOUT;
      }
   }

   at_status_t status = ctx->pending.response.status;
   if (response) {
      *response = ctx->pending.response;
   }
   ctx->pending.type = AT_PENDING_NONE;
   pthread_mutex_unlock(&ctx->pending.mutex);

   return status;
}

/* ── PDU send (two-phase with <octets> arg) ──────────────────────────── */

static bool hex_alphabet_valid(const char *hex) {
   if (!hex)
      return false;
   size_t len = 0;
   for (const char *p = hex; *p; p++) {
      char c = *p;
      bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
      if (!ok) {
         return false;
      }
      len++;
   }
   return (len > 0) && ((len & 1) == 0);
}

at_status_t at_command_send_pdu(at_context_t *ctx,
                                int tpdu_octets,
                                const char *pdu_hex,
                                at_response_t *response) {
   if (!ctx || ctx->fd < 0 || !pdu_hex || tpdu_octets <= 0) {
      return AT_PORT_ERROR;
   }
   /* Re-validate before we push bytes to the modem. A bad encode that slips
    * through here would otherwise corrupt modem state, not just error. */
   if (!hex_alphabet_valid(pdu_hex)) {
      OLOG_ERROR("at_command_send_pdu: PDU hex failed alphabet validation");
      if (response) {
         memset(response, 0, sizeof(*response));
         response->status = AT_ERROR;
      }
      return AT_ERROR;
   }

   char cmgs[32];
   snprintf(cmgs, sizeof(cmgs), "AT+CMGS=%d", tpdu_octets);

   pthread_mutex_lock(&ctx->pending.mutex);
   ctx->pending.type = AT_PENDING_SMS;
   ctx->pending.completed = false;
   memset(&ctx->pending.response, 0, sizeof(ctx->pending.response));
   pthread_mutex_unlock(&ctx->pending.mutex);

   if (at_write_cmd(ctx, cmgs) < 0) {
      pthread_mutex_lock(&ctx->pending.mutex);
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return AT_PORT_ERROR;
   }

   /* Wait for '>' prompt. */
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += AT_TIMEOUT_SMS / 1000;
   ts.tv_nsec += (AT_TIMEOUT_SMS % 1000) * 1000000L;
   if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
   }

   pthread_mutex_lock(&ctx->pending.mutex);
   while (!ctx->pending.completed) {
      int rc = pthread_cond_timedwait(&ctx->pending.cond, &ctx->pending.mutex, &ts);
      if (rc == ETIMEDOUT) {
         char esc = 0x1B;
         if (at_write_raw(ctx, &esc, 1) < 0) {
            OLOG_WARNING("PDU abort ESC write failed after prompt timeout");
         }
         ctx->pending.type = AT_PENDING_NONE;
         pthread_mutex_unlock(&ctx->pending.mutex);
         OLOG_ERROR("PDU prompt timeout for AT+CMGS=%d", tpdu_octets);
         if (response) {
            response->status = AT_TIMEOUT;
         }
         return AT_TIMEOUT;
      }
   }

   if (ctx->pending.response.status != AT_OK) {
      at_status_t status = ctx->pending.response.status;
      if (response) {
         *response = ctx->pending.response;
      }
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return status;
   }
   pthread_mutex_unlock(&ctx->pending.mutex);

   /* Phase 2: hex body + Ctrl-Z. Fixed stack buffer — `pdu_hex` has a
    * compile-time ceiling of PDU_MAX_HEX_LEN, so no allocation needed. */
   size_t hex_len = strlen(pdu_hex);
   if (hex_len > PDU_MAX_HEX_LEN) {
      return AT_PORT_ERROR;
   }
   char send_buf[PDU_MAX_HEX_LEN + 2];
   memcpy(send_buf, pdu_hex, hex_len);
   send_buf[hex_len] = 0x1A;

   pthread_mutex_lock(&ctx->pending.mutex);
   ctx->pending.type = AT_PENDING_SYNC;
   ctx->pending.completed = false;
   memset(&ctx->pending.response, 0, sizeof(ctx->pending.response));
   pthread_mutex_unlock(&ctx->pending.mutex);

   if (at_write_raw(ctx, send_buf, (int)(hex_len + 1)) < 0) {
      pthread_mutex_lock(&ctx->pending.mutex);
      ctx->pending.type = AT_PENDING_NONE;
      pthread_mutex_unlock(&ctx->pending.mutex);
      return AT_PORT_ERROR;
   }

   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += AT_TIMEOUT_SMS / 1000;
   ts.tv_nsec += (AT_TIMEOUT_SMS % 1000) * 1000000L;
   if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
   }

   pthread_mutex_lock(&ctx->pending.mutex);
   while (!ctx->pending.completed) {
      int rc = pthread_cond_timedwait(&ctx->pending.cond, &ctx->pending.mutex, &ts);
      if (rc == ETIMEDOUT) {
         ctx->pending.type = AT_PENDING_NONE;
         pthread_mutex_unlock(&ctx->pending.mutex);
         OLOG_ERROR("PDU send timeout after body (octets=%d)", tpdu_octets);
         if (response) {
            response->status = AT_TIMEOUT;
         }
         return AT_TIMEOUT;
      }
   }

   at_status_t status = ctx->pending.response.status;
   if (response) {
      *response = ctx->pending.response;
   }
   ctx->pending.type = AT_PENDING_NONE;
   pthread_mutex_unlock(&ctx->pending.mutex);

   return status;
}

/* ── Response parsing ────────────────────────────────────────────────── */

bool at_parse_terminator(const char *line, at_status_t *status, int *err_code) {
   if (!line || !status) {
      return false;
   }
   if (err_code) {
      *err_code = 0;
   }

   if (strcmp(line, "OK") == 0) {
      *status = AT_OK;
      return true;
   }
   if (strcmp(line, "ERROR") == 0) {
      *status = AT_ERROR;
      return true;
   }
   if (strncmp(line, "+CME ERROR:", 11) == 0) {
      *status = AT_CME_ERROR;
      if (err_code) {
         *err_code = atoi(line + 11);
      }
      return true;
   }
   if (strncmp(line, "+CMS ERROR:", 11) == 0) {
      *status = AT_CMS_ERROR;
      if (err_code) {
         *err_code = atoi(line + 11);
      }
      return true;
   }
   if (strcmp(line, "NO CARRIER") == 0) {
      *status = AT_NO_CARRIER;
      return true;
   }
   if (strcmp(line, "BUSY") == 0) {
      *status = AT_BUSY;
      return true;
   }
   if (strcmp(line, "NO ANSWER") == 0) {
      *status = AT_NO_ANSWER;
      return true;
   }

   return false;
}

const char *at_status_str(at_status_t status) {
   switch (status) {
      case AT_OK:
         return "OK";
      case AT_ERROR:
         return "ERROR";
      case AT_CME_ERROR:
         return "CME ERROR";
      case AT_CMS_ERROR:
         return "CMS ERROR";
      case AT_TIMEOUT:
         return "TIMEOUT";
      case AT_NO_CARRIER:
         return "NO CARRIER";
      case AT_BUSY:
         return "BUSY";
      case AT_NO_ANSWER:
         return "NO ANSWER";
      case AT_PORT_ERROR:
         return "PORT ERROR";
   }
   return "UNKNOWN";
}
