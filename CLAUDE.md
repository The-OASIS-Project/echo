# CLAUDE.md

## Project Overview

ECHO (Enhanced Cellular Handling Operations) is the OASIS modem daemon for the SIM7600G-H 4G modem. It owns the serial port, handles all AT command traffic, publishes telemetry and events via MQTT, and receives commands from DAWN.

Part of The OASIS Project. Template: STAT (system telemetry daemon).

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j8

# Run tests
ctest --test-dir build --output-on-failure

# Run individual test
./build/tests/test_sms
```

## Code Style

- C99, 3-space indentation, 100-char line limit
- Format with clang-format: `clang-format-14 -i src/*.c include/*.h`
- snake_case functions/variables, UPPER_CASE constants
- OLOG_INFO/OLOG_WARNING/OLOG_ERROR macros for logging
- GPL v3 header on all source files

## Architecture

Two threads + mosquitto background thread:
- **Main thread**: telemetry polling, MQTT command dispatch
- **URC reader thread**: blocking serial reads, URC classification, AT response delivery

AT command serialization via mutex + condvar. Sync commands wait for OK/ERROR. Async commands (ATD, ATA) return immediately; results arrive as URC events.

## MQTT Topics

| Topic | Dir | Content |
|-------|-----|---------|
| `echo/telemetry` | out | Signal, network, call state (every 10s) |
| `echo/events` | out | Incoming call, SMS, call ended |
| `echo/response` | out | Command responses with request_id |
| `echo/status` | out | Online/offline (LWT) |
| `echo/cmd` | in | Commands from DAWN |

All messages conform to OCP v1.3.

## Key Files

- `src/oasis-echo.c` — Main entry, getopt, main loop
- `src/at_command.c` — Serial I/O, AT send/receive
- `src/urc_handler.c` — URC reader thread, event classification
- `src/modem.c` — Init sequence, signal polling, health monitoring
- `src/mqtt_comms.c` — MQTT lifecycle, publish, subscribe, command dispatch
- `src/sms.c` — SMS text-mode helpers, phone number validation
- `src/logging.c` — Logging (copied from STAT)

## Testing

Unity framework (vendored in tests/unity/). Four test modules:
- `test_at_command` — AT response parsing
- `test_sms` — Phone number validation, body sanitization
- `test_urc_handler` — URC classification, RING+CLIP merge
- `test_mqtt_messages` — OCP message format

## Design Document

Single source of truth: `~/code/The-OASIS-Project/dawn/docs/PHONE_SMS_DESIGN.md`
