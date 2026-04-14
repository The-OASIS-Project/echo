# Hardware Verification — SIM7600G-H

Verified 2026-04-13 on Jetson (Linux 5.15.148-tegra) with Waveshare SIM7600G-H 4G HAT, US Mobile SIM (T-Mobile MVNO).

## Modem Init

| Command | Result | Notes |
|---------|--------|-------|
| `AT` | OK | |
| `ATE0` | OK | |
| `AT+CMEE=2` | OK | |
| `AT+CSCS="GSM"` | OK | Required — modem defaults to UCS2 |
| `AT+CLIP=1` | OK | |
| `AT+CMGF=1` | OK | |
| `AT+CPMS="ME","ME","ME"` | OK | Required — default read storage is "SR" |
| `AT+CNMI=2,1,0,0,0` | OK | |
| `AT+CREG=1` | OK | |
| `AT+CSDVC=1` | OK | |
| `AT+CLVL=3` | OK | |
| `AT+CECM=1` | ERROR | Only works during active call — sent per-call |
| `AT+CSQ` | OK | -71 dBm, CSQ 21, 3 bars |
| `AT+COPS?` | OK | T-Mobile |

## Commands

| # | Command | Result | Details |
|---|---------|--------|---------|
| 1 | `signal` | Pass | AT+CSQ round-trip, response JSON with signal_dbm and csq |
| 2 | `send_sms` | Pass | Two-phase AT+CMGS, SMS delivered to phone |
| 3 | `dial` | Pass | Outbound call, phone rang, answered, voice audio working |
| 4 | `hangup` | Pass | AT+CHUP terminates call in all states |
| 5 | `answer` | Pass | Answered incoming call via ATA |
| 6 | `read_sms` | Pass | AT+CMGR returns sender, timestamp, body |
| 7 | `delete_sms` | Pass | AT+CMGD deletes from SIM storage |
| 8 | `dtmf` | Pass | AT+VTS sent during active call |
| 9 | `query_call` | Pass | AT+CLCC returns empty when idle |

## Events (Incoming/Passive)

| Event | Result | Details |
|-------|--------|---------|
| Incoming call (RING+CLIP) | Pass | Single event published, caller ID extracted |
| Call connected (VOICE CALL: BEGIN) | Pass | call_state transitions to "active" |
| Remote hangup (VOICE CALL: END) | Pass | Single call_ended event, no duplicate from NO CARRIER |
| Incoming SMS (+CMTI) | Pass | Deferred to main thread, read via AT+CMGR, published to echo/events |
| Echo cancellation (AT+CECM=1) | Pass | Fires on call connect (both inbound and outbound) |
| Telemetry (10s interval) | Pass | Signal, registration, operator, call state, SIM status |
| Online/offline status (LWT) | Pass | Published on connect, LWT on disconnect |

## Duplicate Suppression

| Scenario | Result |
|----------|--------|
| Multiple RINGs while ringing | Only first publishes incoming_call |
| VOICE CALL: END + NO CARRIER back-to-back | Only first publishes call_ended |

## Telemetry Sample

```json
{
  "device": "echo",
  "signal_dbm": -71,
  "signal_bars": 3,
  "csq": 21,
  "registration": "registered_home",
  "operator": "T-Mobile",
  "network_type": "LTE",
  "call_state": "idle",
  "sim_status": "ready",
  "timestamp": 1776118841
}
```

## Unit Tests

4/4 passing, 86 assertions total.

```
test_at_command .... Passed (14 assertions)
test_sms ........... Passed (34 assertions)
test_urc_handler ... Passed (22 assertions)
test_mqtt_messages . Passed (16 assertions)
```

## Issues Found and Fixed During Testing

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| SMS send fails with ERROR | Modem charset was UCS2, not GSM | Initially added `AT+CSCS="GSM"`, later replaced with UCS2 approach (see below) |
| SMS read fails with CMS ERROR | Read storage was "SR" (status reports) | Added `AT+CPMS="ME","ME","ME"` to init |
| ATH doesn't hang up reliably | SIM7600 needs AT+CHUP in all call states | Changed hangup to use `AT+CHUP` |
| call_state stays ringing_in after answer | SIM7600 sends VOICE CALL: BEGIN not CONNECT | Added URC_VOICE_CALL_BEGIN classification |
| Duplicate call_ended events | VOICE CALL: END + NO CARRIER arrive together | Suppress if already idle |
| Duplicate incoming_call events | RING repeats every ~5s | Suppress if already RINGING_IN |
| AT+CECM=1 fails at init | Only works during active call | Moved to per-call setup via command queue |
| Emoji SMS garbled | GSM charset doesn't support Unicode | Switched to UCS2 with AT+CSMP DCS=8 (see below) |

## UCS2 / Emoji SMS Support (2026-04-14)

Replaced `AT+CSCS="GSM"` approach with native UCS2 encoding for full Unicode/emoji support.

**Root cause**: GSM 7-bit charset only supports basic ASCII + European chars. Emoji (U+1F600+) requires UCS2 encoding.

**Solution**: Keep modem in default UCS2 charset. Add `AT+CSMP=17,167,0,8` to init (DCS=8 tells network body is UCS2-encoded). Encode phone numbers and SMS bodies as UCS2 hex for AT+CMGS. Decode UCS2 hex responses from AT+CMGR.

**Key discovery**: In UCS2 mode, only AT+CMGS parameters are hex-encoded. ATD, AT+CHUP, +CLIP, AT+COPS, AT+CSQ all use plain ASCII.

| Test | Result | Details |
|------|--------|---------|
| Send ASCII SMS in UCS2 | Pass | "Hi" encoded as `00480069`, delivered correctly |
| Send BMP symbol (☺ U+263A) | Pass | With AT+CSMP DCS=8 |
| Send emoji (🎯 U+1F3AF) | Pass | Surrogate pair D83C DFAF, delivered correctly |
| Send multi-emoji SMS | Pass | "Jarvis 🤖⚙️🦾🔴🚀" — all emoji arrived on phone |
| Receive emoji SMS | Pass | "Friday!!! 🙂😅😁" decoded from UCS2 hex to UTF-8 |
| Receive SMS sender decode | Pass | UCS2 hex sender decoded to "+16786432695" |
| MMS (image) receive | Blank | Empty sender/body — MMS requires separate MMSC fetch (Phase 6) |
| UCS2 encode/decode roundtrip | Pass | Unit tested: ASCII, phone numbers, emoji, surrogate pairs (10 assertions) |
