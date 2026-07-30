#!/bin/bash

# Configuration
DEVICE_ID="64324"
BASE_ADDR="0x03000000"

echo "=== Device Configuration Script ==="
echo "Device ID: $DEVICE_ID"
echo "Base Address: $BASE_ADDR"
echo ""

# Step 1: Module Reset
echo "Step 1: Resetting module..."
./module_reset "$DEVICE_ID" "$BASE_ADDR"

# Step 2: Configure Trigger Matching
echo "Step 2: Configuring trigger matching..."
./config_trigger_matching "$DEVICE_ID" "$BASE_ADDR" 2000 -2000 8 4

# Step 3: Enable Channels
echo "Step 3: Enabling channels..."
read -p "Enter channels to enable (comma-separated, e.g., ch1,ch2): " channels_input

# Convert comma-separated input to array
IFS=',' read -ra CHANNELS <<< "$channels_input"

# Build channel arguments
channel_args=""
for ch in "${CHANNELS[@]}"; do
    channel_args="$channel_args $(echo $ch | xargs)"  # trim whitespace
done

./enable_channels_set "$DEVICE_ID" "$BASE_ADDR" $channel_args

# Step 4: Enable Trigger Subtraction
echo "Step 4: Enabling trigger subtraction..."
./enable_trigger_subtraction "$DEVICE_ID" "$BASE_ADDR"

# Step 5: Clear Buffer
echo "Step 5: Clearing buffer..."
./clear_buffer "$DEVICE_ID" "$BASE_ADDR"


echo "=== Configuration Complete ==="