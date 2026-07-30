/*
 * enable_channels_set.c
 * Compilation :
 *   gcc -o enable_channels_set enable_channels_set.c -lCAENComm
 *
 * Usage :
 *   ./enable_channels_set <PID_V4718> <base_address_hex> <ch1> [ch2] [ch3] ...
 *
 * Exemple :
 *   ./enable_channels_set 64324 0x03000000 13 4   -> active channels 13 et 4 seuls
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CAENComm.h>

#define REG_MICRO            0x102E
#define REG_MICRO_HANDSHAKE  0x1030

#define HANDSHAKE_WRITE_OK   (1 << 0)
#define HANDSHAKE_READ_OK    (1 << 1)

#define OPCODE_DIS_ALL_CH        0x4300
#define OPCODE_EN_CHANNEL_BASE   0x4000
#define OPCODE_READ_EN_PATTERN   0x4500

#define MAX_POLL_ATTEMPTS   100000
#define MAX_CHANNELS        16

static int micro_write(int handle, uint16_t word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;

    do {
        err = CAENComm_Read16(handle, REG_MICRO_HANDSHAKE, &handshake);
        if (err != CAENComm_Success) {
            fprintf(stderr, "  [micro_write] Error reading handshake: %d\n", err);
            return -1;
        }
        if (++attempts > MAX_POLL_ATTEMPTS) {
            fprintf(stderr, "  [micro_write] Timeout WRITE_OK (handshake=0x%04X)\n", handshake);
            return -1;
        }
    } while (!(handshake & HANDSHAKE_WRITE_OK));

    err = CAENComm_Write16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) {
        fprintf(stderr, "  [micro_write] Error writing MICRO: %d\n", err);
        return -1;
    }

    return 0;
}

static int micro_read(int handle, uint16_t *word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;

    do {
        err = CAENComm_Read16(handle, REG_MICRO_HANDSHAKE, &handshake);
        if (err != CAENComm_Success) {
            fprintf(stderr, "  [micro_read] Error reading handshake: %d\n", err);
            return -1;
        }
        if (++attempts > MAX_POLL_ATTEMPTS) {
            fprintf(stderr, "  [micro_read] Timeout READ_OK (handshake=0x%04X)\n", handshake);
            return -1;
        }
    } while (!(handshake & HANDSHAKE_READ_OK));

    err = CAENComm_Read16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) {
        fprintf(stderr, "  [micro_read] Error reading MICRO: %d\n", err);
        return -1;
    }

    return 0;
}

static int enable_channel_set(int handle, const int *channels, int n_channels)
{
    uint16_t pattern;
    uint16_t expected = 0;
    int i;

    printf("Sending opcode DIS_ALL_CH (0x%04X)...\n", OPCODE_DIS_ALL_CH);
    if (micro_write(handle, OPCODE_DIS_ALL_CH) != 0) {
        fprintf(stderr, "Failed to send DIS_ALL_CH\n");
        return -1;
    }

    for (i = 0; i < n_channels; i++) {
        uint16_t opcode_en = (uint16_t)(OPCODE_EN_CHANNEL_BASE | (channels[i] & 0xFF));

        printf("Sending opcode EN_CHANNEL (0x%04X, channel=%d)...\n", opcode_en, channels[i]);
        if (micro_write(handle, opcode_en) != 0) {
            fprintf(stderr, "Failed to send EN_CHANNEL for channel %d\n", channels[i]);
            return -1;
        }
        expected |= (uint16_t)(1u << channels[i]);
    }

    printf("Verification : sending READ_EN_PATTERN (0x%04X)...\n", OPCODE_READ_EN_PATTERN);
    if (micro_write(handle, OPCODE_READ_EN_PATTERN) != 0) {
        fprintf(stderr, "Failed to send READ_EN_PATTERN\n");
        return -1;
    }
    if (micro_read(handle, &pattern) != 0) {
        fprintf(stderr, "Failed to read activation pattern\n");
        return -1;
    }

    printf("  Activation pattern (channels 0-15) = 0x%04X\n", pattern);

    if (pattern != expected) {
        fprintf(stderr, "ATTENTION : unexpected pattern (expected 0x%04X, got 0x%04X)\n",
                expected, pattern);
        return -1;
    }

    printf("  Active channels : ");
    for (i = 0; i < n_channels; i++)
        printf("%d%s", channels[i], (i < n_channels - 1) ? ", " : "\n");

    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t usb_link, vme_base;
    int channels[MAX_CHANNELS];
    int n_channels = 0;
    CAENComm_ErrorCode err;
    int handle, rc, i;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <PID_V4718> <base_address_hex> <ch1> [ch2] [ch3] ...\n", argv[0]);
        fprintf(stderr, "Example : %s 64324 0x03000000 13 4\n", argv[0]);
        return EXIT_FAILURE;
    }

    usb_link = (uint32_t)strtoul(argv[1], NULL, 10);
    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    for (i = 3; i < argc && n_channels < MAX_CHANNELS; i++) {
        int ch = atoi(argv[i]);
        if (ch < 0 || ch > 15) {
            fprintf(stderr, "Invalid channel (0-15 for V1290N): %d\n", ch);
            return EXIT_FAILURE;
        }
        channels[n_channels++] = ch;
    }

    if (n_channels == 0) {
        fprintf(stderr, "No valid channel provided.\n");
        return EXIT_FAILURE;
    }

    printf("=== Activation of a set of channels ===\n");
    printf("V4718 PID=%u\n", usb_link);
    printf("--------------------------------------------------\n");
    printf("Module at address 0x%08X, %d target channel(s)\n", vme_base, n_channels);
    printf("--------------------------------------------------\n");

    err = CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Failed to open device: code %d\n", err);
        return EXIT_FAILURE;
    }
    printf("Connection established. Handle = %d\n\n", handle);

    rc = enable_channel_set(handle, channels, n_channels);

    if (rc == 0) {
        printf("\nModule 0x%08X : %d channel(s) active(s) et confirme(s).\n",
               vme_base, n_channels);
    } else {
        fprintf(stderr, "\nModule 0x%08X : failed to activate channels.\n", vme_base);
    }

    CAENComm_CloseDevice(handle);
    printf("Connection closed properly.\n");

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
