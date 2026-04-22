# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project Overview

ECHO (Enhanced Cellular Handling Operations) is the OASIS modem daemon for the SIM7600G-H 4G modem. It owns the serial port, handles all AT command traffic, publishes telemetry and events via MQTT, and receives commands from DAWN. Template: STAT (system telemetry daemon).

See @ARCHITECTURE.md for subsystem details and @README.md for deployment context.

## Critical Rules — Always Follow

- **NEVER delete files.** Tell the developer which files to delete.
- **NEVER run `git add`, `git commit`, or `git push`.** Suggest the command and message; let the developer run it.
- **Feedback before implementation.** Provide analysis, trade-offs, and a recommendation *first*. Wait for explicit confirmation ("go ahead", "do it", "yes") before coding.
- **Format before committing.** Every change must pass `./format_code.sh --check`. Pre-commit hook enforces this.
- **GPL header on every new `.c`/`.h`.** Template in @CODING_STYLE_GUIDE.md.
- **Design doc commit policy**: commit design docs only when they describe shipped or in-flight code. Docs for planned-but-unstarted work stay untracked.

## Build & Test

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j8

# Run tests (Unity framework, runs without hardware)
ctest --test-dir build --output-on-failure

# Format
./format_code.sh                 # fix
./format_code.sh --check         # CI mode
./format_code.sh --changed       # only changed files
```

- Dependencies: `libmosquitto`, `json-c`, `pthread`, Unity (vendored).
- Requires `clang-format-14` for format checks.
- Pre-commit hook: `./install-git-hooks.sh` (one-time).

## Code Standards

Full standards in @CODING_STYLE_GUIDE.md. Critical gotchas specific to ECHO:

- **Return codes**: `SUCCESS` (0) / `FAILURE` (1) — never negative. Specific error codes > 1.
- **Logging**: use `OLOG_INFO` / `OLOG_WARNING` / `OLOG_ERROR` (ECHO's convention).
- **Naming**: `snake_case` functions/vars, `UPPER_CASE` constants, `_t` suffix on types.
- **Memory**: prefer static allocation; null-check after malloc; `free(ptr); ptr = NULL;`.
- **Functions**: soft target < 50 lines, inputs first / outputs last.

## Threading (hard constraints)

Three threads — know which one you're in:

- **Main thread**: command-queue drain, telemetry polling (10s), heartbeat (30s).
- **URC reader thread**: blocking serial reads, line parsing, URC classification, AT response delivery via condvar.
- **Mosquitto thread**: network I/O, message callbacks (queues commands to main thread).

**Never call `at_command_send()` from the URC reader thread.** Use the command queue. Doing so deadlocks the condvar.

- Single serial reader: URC reader owns **all** reads. Main thread only writes AT commands.
- CMTI handling: SMS reads queued from URC → main thread to avoid deadlock.
- Call state: `__atomic` builtins on `g_call_state` (3 threads touch it).

## AT Command Types

| Type | Function | Behavior |
|------|----------|----------|
| Sync | `at_command_send()` | Block on condvar until OK/ERROR/timeout |
| Async | `at_command_send_async()` | Write and return; result comes as URC |
| SMS | `at_command_send_sms()` | Two-phase: wait for `>` prompt, then body+Ctrl-Z |

## MQTT Topics

| Topic | Dir | Content |
|-------|-----|---------|
| `echo/telemetry` | out | Signal, network, call state (every 10s) |
| `echo/events` | out | Incoming call, SMS, call ended |
| `echo/response` | out | Command responses with `request_id` |
| `echo/status` | out | Online/offline (LWT) |
| `echo/cmd` | in | Commands from DAWN |

All messages conform to OCP v1.4 (`ocp_get_timestamp_ms()` for ms timestamps, `msg_type` field on every message).

## SIM7600 Hardware Notes

Discoveries from live hardware testing:

- Modem kept in default UCS2 charset — `AT+CSMP=17,167,0,8` sets DCS=8. Enables full Unicode/emoji SMS.
- Phone numbers and SMS bodies are UCS2 hex-encoded for `AT+CMGS` and decoded from `AT+CMGR`. CLIP and ATD use plain ASCII.
- `AT+CPMS="ME","ME","ME"` required — default SMS read storage is "SR" (status reports).
- `AT+CHUP` for hangup, not `ATH` — works reliably in all call states.
- `AT+CECM=1` only works during active calls — sent per-call, not at init.
- `VOICE CALL: BEGIN` / `VOICE CALL: END` are SIM7600-specific URCs (not standard `CONNECT`).
- Modem sends `VOICE CALL: END` + `NO CARRIER` back-to-back — duplicate suppressed in event handler.
- Current firmware (`LE20B04SIM7600G22`) does **not** include MMS AT commands. See `~/code/The-OASIS-Project/dawn/docs/UNIFIED_IMAGE_STORE_DESIGN.md` §Phase 4 for unblock paths.

## Development Lifecycle

1. **Implement** — build + format check after each chunk: `make -C build -j8` + `./format_code.sh --check`.
2. **Test** — `ctest --test-dir build --output-on-failure`.
3. **Review** — run review agents on the diff (architecture-reviewer, embedded-efficiency-reviewer, security-auditor).
4. **Manual test** — verify on live hardware if touching AT commands, URC handling, or MQTT.
5. **Format** — `./format_code.sh` one final time.
6. **Commit** — provide `git add` + commit message; **developer runs git commands**.

## Design Documents

Phone/SMS integration design: `~/code/The-OASIS-Project/dawn/docs/PHONE_SMS_DESIGN.md`.

## License

GPLv3 or later. Every new source file includes the GPL header block.
