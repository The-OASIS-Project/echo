# CLAUDE.md

## Project Overview

ECHO (Enhanced Cellular Handling Operations) is the OASIS modem daemon for the SIM7600G-H 4G modem. It owns the serial port, handles all AT command traffic, publishes telemetry and events via MQTT, and receives commands from DAWN.

Part of The OASIS Project. Template: STAT (system telemetry daemon).

## Building

```bash
# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j8

# Run tests
ctest --test-dir build --output-on-failure

# Run individual test
./build/tests/test_sms
```

### Dependencies
- `libmosquitto` — MQTT client library
- `json-c` — JSON construction and parsing
- `pthread` — threading (system)
- Unity (vendored in `tests/unity/`) — unit test framework

## Code Formatting

**MANDATORY**: All code MUST be formatted before committing.

```bash
# Format all code (run from repository root)
./format_code.sh

# Check formatting without modifying files
./format_code.sh --check

# Format only changed files (fast)
./format_code.sh --changed
```

Requires `clang-format-14`. Install: `sudo apt-get install clang-format-14`

### Git Hooks
Install the pre-commit hook to automatically check formatting:
```bash
./install-git-hooks.sh
```

## Architecture

### Threading Model

Three threads:
- **Main thread**: Command queue drain, telemetry polling (10s), heartbeat (30s)
- **URC reader thread**: Blocking serial reads, line parsing, URC classification, AT response delivery via condvar
- **Mosquitto thread**: Network I/O, message callbacks (queues commands to main thread)

### Key Design Decisions

- **Single serial reader**: URC reader owns ALL reads. Main thread only writes AT commands.
- **Command queue**: MQTT commands queued to lock-free SPSC ring buffer, drained by main thread. Prevents blocking mosquitto's event loop.
- **Deferred CMTI**: SMS reads queued from URC thread to main thread to avoid condvar deadlock.
- **Atomic call state**: `__atomic` builtins for `g_call_state` (3 threads access it).

### AT Command Types

| Type | Function | Behavior |
|------|----------|----------|
| Sync | `at_command_send()` | Block on condvar until OK/ERROR/timeout |
| Async | `at_command_send_async()` | Write and return; result comes as URC |
| SMS | `at_command_send_sms()` | Two-phase: wait for `>` prompt, then body+Ctrl-Z |

### MQTT Topics

| Topic | Dir | Content |
|-------|-----|---------|
| `echo/telemetry` | out | Signal, network, call state (every 10s) |
| `echo/events` | out | Incoming call, SMS, call ended |
| `echo/response` | out | Command responses with request_id |
| `echo/status` | out | Online/offline (LWT) |
| `echo/cmd` | in | Commands from DAWN |

All messages conform to OCP v1.3.

## Coding Standards

Follow `CODING_STYLE_GUIDE.md` strictly:

**Naming**: `snake_case` functions/variables, `UPPER_CASE` constants, `_t` suffix on types.

**Error Handling**: Return 0 on success. Always check return values. Log with `OLOG_ERROR()`.

**Memory**: Prefer static allocation. Minimize malloc. Free and NULL.

**File Headers**: GPL license block required on all `.c` and `.h` files (see CODING_STYLE_GUIDE.md).

**Functions**: Soft target < 50 lines. Inputs first, outputs last.

**Threading**: Never call `at_command_send()` from the URC reader thread. Use the command queue.

## Important Files

**Source modules:**
- `src/oasis-echo.c` — Main entry, command queue, URC event dispatch, MQTT command processor
- `src/at_command.c` — Serial I/O with flock, sync/async/SMS AT commands, terminator parsing
- `src/urc_handler.c` — URC reader thread, classification, RING+CLIP merge
- `src/modem.c` — Init sequence, signal polling, telemetry builder, echo cancellation
- `src/mqtt_comms.c` — MQTT lifecycle, json-c JSON builders, command parser
- `src/sms.c` — Phone number validation, SMS body sanitization, CLIP sanitization
- `src/logging.c` — Logging (copied from STAT)

**Headers:**
- `include/echo.h` — Global types, config struct, call/reg/SIM enums, rate bucket
- `include/at_command.h` — AT context, response, pending state types
- `include/urc_handler.h` — URC event types, callback, context
- `include/modem.h` — Modem init, polling, telemetry builder
- `include/mqtt_comms.h` — MQTT topics, publish/subscribe/parse API
- `include/sms.h` — Validation and sanitization API

**Configuration:**
- `config/echo.conf` — MQTT credentials, serial port, rate limits (systemd EnvironmentFile)
- `config/oasis-echo.service` — systemd service unit
- `config/sim7600-rndis.service` — RNDIS data path boot service
- `scripts/sim7600-rndis-up.sh` — RNDIS activation script

**Tooling:**
- `.clang-format` — clang-format-14 config (matches DAWN)
- `format_code.sh` — Format all code (adapted from DAWN)
- `pre-commit.hook` — Git pre-commit formatting check
- `install-git-hooks.sh` — Hook installer
- `.github/workflows/ci.yml` — CI: format-check + build + tests

## Testing

Unity framework (vendored in `tests/unity/`, MIT license). Four test modules:

| Test | Assertions | What it covers |
|------|-----------|---------------|
| `test_at_command` | 14 | Response terminator parsing, status strings |
| `test_sms` | 24 | Phone number validation, body sanitization, CLIP sanitization |
| `test_urc_handler` | 22 | URC classification, RING+CLIP merge, VOICE CALL URCs |
| `test_mqtt_messages` | 16 | Telemetry/event/response JSON, command parsing |

Tests link against specific source files (not the full daemon binary), so they run without hardware or an MQTT broker.

```bash
# Build and run all tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug && make -C build -j8
ctest --test-dir build --output-on-failure
```

## SIM7600 Hardware Notes

Discoveries from live hardware testing:

- `AT+CSCS="GSM"` required at init — modem defaults to UCS2 charset which breaks ASCII phone numbers
- `AT+CPMS="ME","ME","ME"` required — default SMS read storage is "SR" (status reports)
- `AT+CHUP` for hangup instead of `ATH` — works reliably in all call states
- `AT+CECM=1` only works during active calls — sent per-call, not at init
- `VOICE CALL: BEGIN` / `VOICE CALL: END` are SIM7600-specific URCs (not standard `CONNECT`)
- Modem sends `VOICE CALL: END` + `NO CARRIER` back-to-back — duplicate suppressed in event handler

## Development Lifecycle

1. **Implement**: Build and format check after each chunk: `make -C build -j8` + `./format_code.sh --check`
2. **Test**: Run `ctest --test-dir build --output-on-failure`
3. **Review**: Run review agents on the diff (architecture-reviewer, embedded-efficiency-reviewer, security-auditor)
4. **Manual test**: Verify on live hardware if touching AT commands, URC handling, or MQTT
5. **Format**: `./format_code.sh`
6. **Commit**: Provide `git add` + commit message to developer (never run git commands directly)

## Design Document

Single source of truth: `~/code/The-OASIS-Project/dawn/docs/PHONE_SMS_DESIGN.md`

## License

GPLv3 or later. All source files include GPL header block.
