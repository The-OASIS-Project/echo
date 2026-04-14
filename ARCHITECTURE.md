# ECHO Architecture

## Overview

ECHO is a standalone modem daemon following the OASIS pattern of dedicated daemons per hardware domain. It owns the SIM7600G-H serial port and exposes modem functionality via MQTT. DAWN (the AI brain) sends commands and receives events — it never touches the serial port directly.

```
DAWN (AI assistant)                     ECHO (modem daemon)
  phone_tool.c  ── MQTT ──────────────>  oasis-echo
    |                echo/cmd               |
  phone_service.c <── MQTT ─────────────  AT commands
    |                echo/events            |
    |                echo/telemetry         |
  phone_db.c                            SIM7600G-H
    |                                   /dev/ttyUSB2
  audio bridge
    |
  USB sound card <── crossover cable ── modem 3.5mm jack
```

MIRAGE (the HUD) subscribes directly to `echo/telemetry` for LTE signal bars — no DAWN relay needed.

## Threading Model

Three threads total:

```
Main Thread                         URC Reader Thread          Mosquitto Thread
    |                                     |                        |
    |-- drain command queue               |-- blocking read()      |-- network I/O
    |   (MQTT cmds, CMTI reads,           |   on serial fd         |   (connect, publish,
    |    call audio setup)                 |-- parse lines          |    subscribe, keepalive)
    |-- telemetry polling (10s)           |-- classify:            |-- on_message callback
    |   (AT+CSQ, AT+CREG?)               |   URC → publish event  |   queues to cmd_queue
    |-- heartbeat (30s)                   |   AT response → signal |
    |-- sleep(1)                          |   condvar to main      |
```

### Key Design Decisions

**Single serial reader**: The URC reader thread owns ALL reads from the serial fd. The main thread only writes AT commands. This avoids the classic two-reader race on modem serial ports.

**Command queue**: MQTT commands arrive on mosquitto's callback thread but are never processed there. They are queued to a lock-free ring buffer and drained by the main thread. This prevents blocking mosquitto's event loop during long AT operations (SMS send can take 60+ seconds).

**Deferred CMTI reads**: When an incoming SMS notification (`+CMTI`) arrives, the URC reader queues it rather than calling `AT+CMGR` directly. If the URC reader called `at_command_send()`, it would deadlock — the reader would wait on a condvar for itself to deliver the response.

**Deferred call audio setup**: `AT+CECM=1` (echo cancellation) only works during an active voice call. When `VOICE CALL: BEGIN` is detected, the setup is queued to the main thread which sends the AT command.

**Atomic call state**: `g_call_state` is accessed from three threads (URC reader sets it on call events, main thread reads it for telemetry, mosquitto thread reads it via queued commands). Uses `__atomic` builtins rather than a mutex — low contention, single enum value.

## AT Command Serialization

Two command types, one mutex:

**Synchronous** (`at_command_send`): Acquires mutex, writes command, waits on condvar for OK/ERROR. The URC reader signals the condvar when it sees a response terminator. Timeout: 2 seconds default, 60 seconds for SMS.

**Asynchronous** (`at_command_send_async`): Writes command and returns immediately. Used for `ATD` (dial) and `ATA` (answer) where the result arrives later as a URC event (`VOICE CALL: BEGIN`, `NO CARRIER`, `BUSY`).

**SMS two-phase** (`at_command_send_sms`): Phase 1 sends `AT+CMGS="number"` and waits for `>` prompt. Phase 2 sends body + Ctrl-Z and waits for OK/ERROR. ESC sent on timeout to abort.

## Module Map

```
oasis-echo.c (864 lines)
├── main() — config, getopt, env vars, lifecycle
├── command queue — lock-free SPSC ring buffer
├── rate limiter — sliding window, 64-entry ring
├── on_urc_event() — URC → MQTT events (runs on URC thread, no AT commands)
├── on_mqtt_command() — queues to cmd_queue (runs on mosquitto thread)
├── process_mqtt_command() — dispatches commands (runs on main thread)
├── handle_cmti() — deferred SMS read (runs on main thread)
└── main loop — drain queue, telemetry, heartbeat

at_command.c (512 lines)
├── at_open/close — serial port with flock, termios config
├── at_command_send — synchronous AT with condvar wait
├── at_command_send_async — fire-and-forget for ATD/ATA
├── at_command_send_sms — two-phase AT+CMGS protocol
└── at_parse_terminator — OK/ERROR/+CME/+CMS/NO CARRIER/BUSY

urc_handler.c (393 lines)
├── urc_reader_thread — single-reader serial loop
├── urc_classify — RING, +CLIP, +CMTI, NO CARRIER, CONNECT,
│                   VOICE CALL: BEGIN/END, +CREG, SMS prompt
├── RING+CLIP merge — 300ms timer, dispatch with or without number
└── read_line — byte-at-a-time with VTIME timeout

modem.c (325 lines)
├── modem_init — AT, ATE0, CMEE, CSMP, CLIP, CMGF, CPMS, CNMI,
│                CREG, CSDVC, CLVL, CSQ, COPS
├── modem_poll_signal — AT+CSQ → dBm + bars
├── modem_build_telemetry — signal + reg (always), operator +
│                           network type + SIM (cached, 60s refresh)
├── modem_call_audio_setup — AT+CECM=1 (per-call echo cancellation)
└── modem_heartbeat — AT liveness check

mqtt_comms.c (428 lines)
├── mqtt_comms_init — connect, TLS, auth, LWT, subscribe
├── mqtt_publish_telemetry/event/response/status
├── mqtt_build_*_json — json-c builders (public for testing)
├── mqtt_parse_command — json-c parser (public for testing)
└── on_message → dispatch to cmd_handler callback

sms.c (120 lines)
├── sms_validate_number — [+*#0-9]{1,20}
├── sms_sanitize_body — strip control chars, reject Ctrl-Z/ESC
└── sms_sanitize_clip — extract and validate caller ID

logging.c (172 lines)
└── Copied from STAT — console/file/syslog, ANSI colors, OLOG_* macros
```

## Modem Init Sequence

Commands sent at startup, in order:

| Command | Purpose | Notes |
|---------|---------|-------|
| `AT` | Verify communication | Fatal if fails |
| `ATE0` | Disable command echo | |
| `AT+CMEE=2` | Verbose error messages | |
| `AT+CSMP=17,167,0,8` | SMS params with UCS2 DCS | DCS=8 tells network body is UCS2; enables emoji |
| `AT+CLIP=1` | Enable caller ID | |
| `AT+CMGF=1` | SMS text mode | |
| `AT+CPMS="ME","ME","ME"` | SMS storage to modem memory | Default is "SR" (status reports) which causes read failures |
| `AT+CNMI=2,1,0,0,0` | SMS notification via URC | |
| `AT+CREG=1` | Network registration URCs | |
| `AT+CSDVC=1` | Audio to headset jack | |
| `AT+CLVL=3` | Volume mid-level | |
| `AT+CSQ` | Initial signal read | |
| `AT+COPS?` | Initial operator query | |

SIM7600-specific discoveries during hardware testing:
- Modem kept in default UCS2 charset — `AT+CSMP=17,167,0,8` sets DCS for UCS2 network encoding
- Phone numbers and SMS bodies UCS2 hex-encoded for AT+CMGS, decoded from AT+CMGR. CLIP and ATD use plain ASCII.
- `AT+CPMS="ME","ME","ME"` required — default SMS read storage is "SR"
- `AT+CHUP` used for hangup instead of `ATH` — works in all call states
- `AT+CECM=1` only works during active calls — sent per-call, not at init
- `VOICE CALL: BEGIN` / `VOICE CALL: END` are SIM7600 URCs (not standard `CONNECT`)

## Duplicate Event Suppression

The SIM7600 sends multiple overlapping URCs for call state changes:

- **RING repeats** every ~5s while ringing → only the first publishes `incoming_call`
- **VOICE CALL: END + NO CARRIER** arrive back-to-back → whichever fires first sets idle, the other is suppressed
- **BUSY/NO ANSWER** also suppressed if already idle

## Security Model

### Input Validation (Defense in Depth)

| Input | Validation | Where |
|-------|-----------|-------|
| Phone numbers (dial, send_sms) | `[+*#0-9]{1,20}` | `sms_validate_number()` |
| SMS body | Reject Ctrl-Z/ESC, strip 0x00-0x1F except newline | `sms_sanitize_body()` |
| SMS index (read, delete) | `strtol`, range 0-999 | `validate_sms_index()` |
| DTMF digit | `[0-9*#A-D]` | `validate_dtmf()` |
| Caller ID (+CLIP) | Strip non-number chars, validate | `sms_sanitize_clip()` |
| Serial port path | `/dev/ttyUSB[0-9]{1,3}` or `/dev/ttyACM[0-9]{1,3}` | `validate_serial_path()` |

### Rate Limiting

Sliding 1-hour window with a 64-entry ring buffer. Defaults: 5 calls/hour, 20 SMS/hour. Configurable via echo.conf. DAWN has its own UX-level limits — ECHO's limits are the hard floor.

### JSON Construction

All JSON output uses json-c (`json_object_new_string`, etc.) which handles escaping automatically. No manual `snprintf` JSON construction — eliminates JSON injection from modem responses or SMS content.

## RNDIS Data Path

The modem provides tertiary internet via RNDIS USB ethernet:

| Interface | Metric | Priority |
|-----------|--------|----------|
| `enP8p1s0` (wired) | 100 | Primary |
| `wlP1p1s0` (WiFi) | 1000 | Secondary |
| `usb0` (RNDIS) | 20100 | Tertiary |

Managed by NetworkManager (not ModemManager). The `sim7600-rndis.service` runs at boot to activate the data connection. ECHO starts after it completes (`After=sim7600-rndis.service`).

## Dependencies

- `libmosquitto` — MQTT client
- `json-c` — JSON construction and parsing
- `pthread` — threading
- Unity (vendored) — unit test framework
