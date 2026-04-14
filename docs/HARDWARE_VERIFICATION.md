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

4/4 passing, 76 assertions total.

```
test_at_command .... Passed (14 assertions)
test_sms ........... Passed (24 assertions)
test_urc_handler ... Passed (22 assertions)
test_mqtt_messages . Passed (16 assertions)
```

## Issues Found and Fixed During Testing

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| SMS send fails with ERROR | Modem charset was UCS2, not GSM | Added `AT+CSCS="GSM"` to init |
| SMS read fails with CMS ERROR | Read storage was "SR" (status reports) | Added `AT+CPMS="ME","ME","ME"` to init |
| ATH doesn't hang up reliably | SIM7600 needs AT+CHUP in all call states | Changed hangup to use `AT+CHUP` |
| call_state stays ringing_in after answer | SIM7600 sends VOICE CALL: BEGIN not CONNECT | Added URC_VOICE_CALL_BEGIN classification |
| Duplicate call_ended events | VOICE CALL: END + NO CARRIER arrive together | Suppress if already idle |
| Duplicate incoming_call events | RING repeats every ~5s | Suppress if already RINGING_IN |
| AT+CECM=1 fails at init | Only works during active call | Moved to per-call setup via command queue |
