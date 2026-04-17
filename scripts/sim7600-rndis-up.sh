#!/usr/bin/env bash
set -euo pipefail

IF=usb0
ATDEV=""

# Wait for usb0 up to 30s
for i in {1..30}; do ip link show "$IF" >/dev/null 2>&1 && break || sleep 1; done

# On Jetson L4T, usb0 is typically unmanaged by NetworkManager by design
# (reserved for the Jetson's USB-gadget interface). We keep a best-effort NM
# profile for systems where NM does manage usb0, but the actual DHCP client
# for this script is dhclient below.
if ! nmcli -t -f NAME con show | grep -qx simcom-rndis; then
  nmcli con add type ethernet ifname "$IF" con-name simcom-rndis ipv4.method auto ipv6.method auto
fi
nmcli con mod simcom-rndis ipv4.route-metric 20100 ipv6.route-metric 20100 802-3-ethernet.mtu 1420 || true
nmcli con up simcom-rndis || true

# Wait up to 40s for an AT port to appear (/dev/ttyUSB2 or /dev/ttyUSB3)
for i in {1..40}; do
  for cand in /dev/ttyUSB2 /dev/ttyUSB3; do
    if [ -e "$cand" ]; then ATDEV="$cand"; break 2; fi
  done
  sleep 1
done

# If no AT port (rare), we still have DHCP via RNDIS; exit cleanly
[ -n "$ATDEV" ] || exit 0

# Prep serial and nudge the modem's data side
stty -F "$ATDEV" 115200 -echo -echoe -echok -crtscts || true
printf "AT\r" > "$ATDEV"; sleep 1
printf "AT+CGATT=1\r" > "$ATDEV"; sleep 1
printf "AT+CGDCONT=1,\"IPV4V6\",\"fast.t-mobile.com\"\r" > "$ATDEV"; sleep 1
printf "AT+CGDCONT=6,\"IPV4V6\",\"fast.t-mobile.com\"\r" > "$ATDEV"; sleep 1
printf "AT+NETOPEN\r" > "$ATDEV"; sleep 2

# Ensure DHCP, MTU, and DNS on usb0.
# Clean up any prior dhclient on this interface so service restarts don't pile
# up zombies with stale sockets (the cause of "send_packet: No such device"
# errors when the modem re-enumerates).
pkill -f "dhclient.*$IF" 2>/dev/null || true
dhclient -r "$IF" >/dev/null 2>&1 || true
dhclient -nw "$IF" || true
ip link set dev "$IF" mtu 1420 || true
if command -v resolvectl >/dev/null 2>&1; then
  resolvectl dns "$IF" 1.1.1.1 8.8.8.8 || true
  resolvectl domain "$IF" ~. || true
fi
exit 0
