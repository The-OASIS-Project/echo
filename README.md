# ECHO — Enhanced Cellular Handling Operations

ECHO is the OASIS modem daemon for the Waveshare SIM7600G-H 4G modem. It owns the serial port, handles all AT command traffic, and communicates with the rest of the OASIS ecosystem via MQTT.

Part of [The OASIS Project](https://github.com/The-OASIS-Project/).

## What It Does

- **Phone calls**: Dial, answer, hang up, DTMF tones, call state tracking
- **SMS**: Send, receive, read, delete — with AT command injection prevention
- **Telemetry**: Signal strength, network registration, operator, call state — published every 10 seconds
- **Health monitoring**: Heartbeat checks, modem lost/reconnected events
- **Rate limiting**: 5 calls/hour, 20 SMS/hour (configurable, defense in depth)

## Hardware

- **Modem**: Waveshare SIM7600G-H 4G HAT (USB, SIMCom chipset)
- **Serial**: `/dev/ttyUSB2` at 115200 baud (AT command port)
- **Network**: RNDIS via `usb0` (tertiary internet, route-metric 20100)
- **Audio**: 3.5mm analog jack with crossover cable to USB sound card
- **SIM**: US Mobile (T-Mobile MVNO)

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j8
```

## Running

```bash
# Interactive (console logging)
./build/oasis-echo \
  --serial-port /dev/ttyUSB2 \
  --mqtt-host localhost --mqtt-port 1883 \
  --mqtt-username oasis --mqtt-password 'yourpass'

# Service mode (syslog)
./build/oasis-echo --service \
  --mqtt-host localhost --mqtt-port 1883
```

## Testing

```bash
# Unit tests (no hardware needed)
ctest --test-dir build --output-on-failure

# Individual test
./build/tests/test_sms
```

Four test modules with 70+ assertions:

| Test | What it covers |
|------|---------------|
| `test_at_command` | Response terminator parsing, status strings |
| `test_sms` | Phone number validation, SMS body sanitization, CLIP sanitization |
| `test_urc_handler` | URC classification, RING+CLIP merge, VOICE CALL URCs |
| `test_mqtt_messages` | Telemetry/event/response JSON construction, command parsing |

## MQTT Topics

| Topic | Dir | QoS | Retain | Content |
|-------|-----|-----|--------|---------|
| `echo/telemetry` | out | 0 | yes | Signal, network, call state (every 10s) |
| `echo/events` | out | 1 | no | Incoming call, SMS received, call ended |
| `echo/response` | out | 1 | no | Command responses with `request_id` |
| `echo/status` | out | 1 | yes | Online/offline (LWT) |
| `echo/cmd` | in | — | — | Commands from DAWN |

All messages conform to OCP v1.3.

## Commands

Send JSON to `echo/cmd`:

```json
{"device":"echo","action":"dial","value":"+15551234567","request_id":"w42","timestamp":0}
{"device":"echo","action":"answer","request_id":"w43","timestamp":0}
{"device":"echo","action":"hangup","request_id":"w44","timestamp":0}
{"device":"echo","action":"send_sms","value":"+15551234567","data":{"content":"Hello"},"request_id":"w45","timestamp":0}
{"device":"echo","action":"read_sms","value":"3","request_id":"w46","timestamp":0}
{"device":"echo","action":"delete_sms","value":"3","request_id":"w47","timestamp":0}
{"device":"echo","action":"signal","request_id":"w48","timestamp":0}
{"device":"echo","action":"dtmf","value":"5","request_id":"w49","timestamp":0}
{"device":"echo","action":"query_call","request_id":"w50","timestamp":0}
```

## Installation

```bash
./install.sh
# Then edit /etc/oasis/echo.conf with MQTT credentials
sudo systemctl enable --now oasis-echo
```

## Security

- Phone number validation: `[+*#0-9]{1,20}` — rejects injection characters
- SMS body sanitization: Ctrl-Z (0x1A) and ESC (0x1B) rejected entirely — prevents AT command injection
- SMS index validation: digits only (0-999) for read/delete
- DTMF validation: `[0-9*#A-D]` only
- Serial port path: allowlist `/dev/ttyUSB[0-9]` and `/dev/ttyACM[0-9]`
- Rate limiting: configurable calls/hour and SMS/hour
- MQTT: TLS + authentication supported, LWT for status
- systemd: NoNewPrivileges, ProtectSystem, ProtectHome, non-root user

## License

GPLv3 or later. See [LICENSE](LICENSE).
