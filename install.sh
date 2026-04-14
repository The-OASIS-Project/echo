#!/usr/bin/env bash
set -euo pipefail

# OASIS ECHO — Install script
# Installs the oasis-echo binary, config files, and systemd services.

INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"
CONFIG_DIR="/etc/oasis"
SYSTEMD_DIR="/etc/systemd/system"

echo "=== OASIS ECHO Installer ==="

# Build if not already built
if [ ! -f build/oasis-echo ]; then
   echo "Building oasis-echo..."
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   make -C build -j"$(nproc)"
fi

# Install binary
echo "Installing binary to ${INSTALL_PREFIX}/bin/"
sudo install -m 755 build/oasis-echo "${INSTALL_PREFIX}/bin/"

# Install config (don't overwrite existing)
echo "Installing config to ${CONFIG_DIR}/"
sudo mkdir -p "${CONFIG_DIR}"
if [ ! -f "${CONFIG_DIR}/echo.conf" ]; then
   sudo install -m 640 config/echo.conf "${CONFIG_DIR}/"
   echo "  echo.conf installed (edit credentials before starting)"
else
   echo "  echo.conf already exists, skipping"
fi

# Install RNDIS script
echo "Installing RNDIS script to ${INSTALL_PREFIX}/sbin/"
sudo install -m 755 scripts/sim7600-rndis-up.sh "${INSTALL_PREFIX}/sbin/"

# Install systemd services
echo "Installing systemd services to ${SYSTEMD_DIR}/"
sudo install -m 644 config/oasis-echo.service "${SYSTEMD_DIR}/"
sudo install -m 644 config/sim7600-rndis.service "${SYSTEMD_DIR}/"

# Reload systemd
sudo systemctl daemon-reload

echo ""
echo "=== Installation complete ==="
echo ""
echo "Next steps:"
echo "  1. Edit ${CONFIG_DIR}/echo.conf with your MQTT credentials"
echo "  2. sudo systemctl enable --now sim7600-rndis.service"
echo "  3. sudo systemctl enable --now oasis-echo.service"
echo "  4. journalctl -u oasis-echo -f"
