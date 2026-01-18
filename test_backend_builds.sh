#!/bin/bash
# Test script to verify both backends build correctly
# Usage: ./test_backend_builds.sh [DEVICE]

set -e

DEVICE=${1:-tanmatsu}
CONFIG_FILE="main/mod_backend_config.h"
BACKUP_FILE="${CONFIG_FILE}.backup"

echo "Testing MOD backend builds for device: ${DEVICE}"
echo "================================================"

# Backup original config
if [ ! -f "${BACKUP_FILE}" ]; then
    cp "${CONFIG_FILE}" "${BACKUP_FILE}"
    echo "Backed up ${CONFIG_FILE} to ${BACKUP_FILE}"
fi

# Function to restore config
restore_config() {
    if [ -f "${BACKUP_FILE}" ]; then
        cp "${BACKUP_FILE}" "${CONFIG_FILE}"
        echo "Restored ${CONFIG_FILE} from backup"
    fi
}

# Function to set backend
set_backend() {
    local backend=$1
    if [ "${backend}" = "XMP" ]; then
        sed -i 's/^#define MOD_BACKEND_OPENMPT/\/\/ #define MOD_BACKEND_OPENMPT/' "${CONFIG_FILE}"
        sed -i 's/^\/\/ #define MOD_BACKEND_XMP/#define MOD_BACKEND_XMP/' "${CONFIG_FILE}"
    else
        sed -i 's/^#define MOD_BACKEND_XMP/\/\/ #define MOD_BACKEND_XMP/' "${CONFIG_FILE}"
        sed -i 's/^\/\/ #define MOD_BACKEND_OPENMPT/#define MOD_BACKEND_OPENMPT/' "${CONFIG_FILE}"
    fi
}

# Check if libxmp is enabled
if [ ! -f "components/libxmp/CMakeLists.txt" ]; then
    echo "⚠️  WARNING: libxmp component is disabled (CMakeLists.txt.disabled)"
    echo "   To test libxmp backend, run:"
    echo "   cd components/libxmp && mv CMakeLists.txt.disabled CMakeLists.txt"
    echo ""
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Test 1: libopenmpt backend
echo ""
echo "Test 1: Building with libopenmpt backend..."
set_backend "OPENMPT"
if make -j$(nproc) build DEVICE=${DEVICE} > /tmp/build_openmpt.log 2>&1; then
    echo "✅ libopenmpt build: SUCCESS"
else
    echo "❌ libopenmpt build: FAILED"
    echo "   Check /tmp/build_openmpt.log for details"
    restore_config
    exit 1
fi

# Test 2: libxmp backend
echo ""
echo "Test 2: Building with libxmp backend..."
set_backend "XMP"
if make -j$(nproc) build DEVICE=${DEVICE} > /tmp/build_xmp.log 2>&1; then
    echo "✅ libxmp build: SUCCESS"
else
    echo "❌ libxmp build: FAILED"
    echo "   Check /tmp/build_xmp.log for details"
    restore_config
    exit 1
fi

# Restore original config
restore_config

echo ""
echo "================================================"
echo "✅ Both backends built successfully!"
echo ""
