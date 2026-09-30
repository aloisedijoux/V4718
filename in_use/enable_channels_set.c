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
#include "caen_open.h"

#define REG_MICRO            0x102E
#define REG_MICRO_HANDSHAKE  0x1030

#define HANDSHAKE_WRITE_OK   (1 << 0)
#define HANDSHAKE_READ_OK    (1 << 1)

#define OPCODE_DIS_ALL_CH        0x4300
#define OPCODE_EN_CHANNEL_BASE   0x4000
#define OPCODE_READ_EN_PATTERN   0x4500

#define MAX_POLL_ATTEMPTS   100000
#define MAX_CHANNELS        32  /* EN_CHANNEL (0x40nn) takes the channel index as a plain byte,
                                    so it works unchanged up to channel 31 (V1290A). The actual
                                    model is auto-detected at runtime (see caen_v1290_is_model_a
                                    in caen_open.h) to know how many channels/readback words to
                                    expect; this is just the syntactic upper bound before we have
                                    a handle to check against. */

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
    uint16_t word0, word1 = 0;
    uint32_t pattern, expected = 0;
    int is_model_a;
    int i;

    is_model_a = caen_v1290_is_model_a(handle);
    if (is_model_a < 0) {
        fprintf(stderr, "Failed to detect module model (Configuration ROM read)\n");
        return -1;
    }
    if (!is_model_a) {
        for (i = 0; i < n_channels; i++) {
            if (channels[i] > 15) {
                fprintf(stderr, "Channel %d is out of range for the detected V1290N (0-15)\n",
                        channels[i]);
                return -1;
            }
        }
    }

    printf("Sending opcode DIS_ALL_CH (0x%04X)...\n", OPCODE_DIS_ALL_CH);
    if (micro_write(handle, OPCODE_DIS_ALL_CH) != 0) {
        fprintf(stderr, "Failed to send DIS_ALL_CH\n");
        return -1;
    }

    for (i = 0; i < n_channels; i++) {
        /* EN_CHANNEL (manual Sec.5.6.1, opcode 0x40nn) takes the channel index as a plain
           byte operand and is documented identically for the 16- and 32-channel models,
           so this works unmodified for channels 0-31. */
        uint16_t opcode_en = (uint16_t)(OPCODE_EN_CHANNEL_BASE | (channels[i] & 0xFF));

        printf("Sending opcode EN_CHANNEL (0x%04X, channel=%d)...\n", opcode_en, channels[i]);
        if (micro_write(handle, opcode_en) != 0) {
            fprintf(stderr, "Failed to send EN_CHANNEL for channel %d\n", channels[i]);
            return -1;
        }
        expected |= (1u << channels[i]);
    }

    /* READ_EN_PATTERN (0x45xx, manual Sec.5.6.6) answers with ONE 16-bit word on a
       V1290N but TWO on a V1290A (channels 0-15, then 16-31) -- reading the wrong
       number of words leaves the microcontroller waiting for the rest and
       desyncs every command that follows, so the word count must follow the
       detected model, not which channels were requested. */
    printf("Verification : sending READ_EN_PATTERN (0x%04X)...\n", OPCODE_READ_EN_PATTERN);
    if (micro_write(handle, OPCODE_READ_EN_PATTERN) != 0) {
        fprintf(stderr, "Failed to send READ_EN_PATTERN\n");
        return -1;
    }
    if (micro_read(handle, &word0) != 0) {
        fprintf(stderr, "Failed to read activation pattern (word 0)\n");
        return -1;
    }
    if (is_model_a) {
        if (micro_read(handle, &word1) != 0) {
            fprintf(stderr, "Failed to read activation pattern (word 1, channels 16-31)\n");
            return -1;
        }
    }

    pattern = (uint32_t)word0 | ((uint32_t)word1 << 16);
    if (is_model_a)
        printf("  Activation pattern (channels 0-31) = 0x%08X (word0=0x%04X, word1=0x%04X)\n",
               pattern, word0, word1);
    else
        printf("  Activation pattern (channels 0-15) = 0x%04X\n", word0);

    if (pattern != expected) {
        fprintf(stderr, "ATTENTION : unexpected pattern (expected 0x%08X, got 0x%08X)\n",
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
    uint32_t vme_base;
    int channels[MAX_CHANNELS];
    int n_channels = 0;
    CAENComm_ErrorCode err;
    int handle, rc, i;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> <base_address_hex> <ch1> [ch2] [ch3] ...\n", argv[0]);
        fprintf(stderr, "Example (USB) : %s 64324 0x03000000 13 4\n", argv[0]);
        fprintf(stderr, "Example (ETH) : %s 192.168.1.254 0x03000000 13 4\n", argv[0]);
        return EXIT_FAILURE;
    }

    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    for (i = 3; i < argc && n_channels < MAX_CHANNELS; i++) {
        int ch = atoi(argv[i]);
        if (ch < 0 || ch > 31) {
            fprintf(stderr, "Invalid channel (0-31: 0-15 on a V1290N, 0-31 on a V1290A): %d\n", ch);
            return EXIT_FAILURE;
        }
        channels[n_channels++] = ch;
    }

    if (n_channels == 0) {
        fprintf(stderr, "No valid channel provided.\n");
        return EXIT_FAILURE;
    }

    printf("=== Activation of a set of channels ===\n");
    printf("V4718 PID/IP=%s\n", argv[1]);
    printf("--------------------------------------------------\n");
    printf("Module at address 0x%08X, %d target channel(s)\n", vme_base, n_channels);
    printf("--------------------------------------------------\n");

    err = caen_open_v4718(argv[1], vme_base, &handle);
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
