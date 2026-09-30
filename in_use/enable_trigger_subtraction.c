/*
 * enable_trigger_subtraction.c
 * Compilation :
 *   gcc -o enable_trigger_subtraction enable_trigger_subtraction.c -lCAENComm
 *
 * Usage :
 *   ./enable_trigger_subtraction <PID_V4718> <base_address_hex>
 *
 * Exemple :
 *   ./enable_trigger_subtraction 64324 0x03000000
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

#define OPCODE_EN_SUB_TRG     0x1400
#define OPCODE_READ_TRG_CONF  0x1600

#define MAX_POLL_ATTEMPTS   100000

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

int main(int argc, char *argv[])
{
    uint32_t vme_base;
    CAENComm_ErrorCode err;
    int handle;
    uint16_t words[5];
    int i;
    int rc = 0;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> <base_address_hex>\n", argv[0]);
        fprintf(stderr, "Example (USB): %s 64324 0x03000000\n", argv[0]);
        fprintf(stderr, "Example (ETH): %s 192.168.1.254 0x03000000\n", argv[0]);
        return EXIT_FAILURE;
    }

    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    printf("=== Enabling trigger time subtraction ===\n");
    printf("V4718 PID/IP=%s\n", argv[1]);
    printf("--------------------------------------------------\n");
    printf("Module at address 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = caen_open_v4718(argv[1], vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Open failed: code %d\n", err);
        return EXIT_FAILURE;
    }
    printf("Connection established. Handle = %d\n\n", handle);

    printf("Sending opcode EN_SUB_TRG (0x%04X)...\n", OPCODE_EN_SUB_TRG);
    if (micro_write(handle, OPCODE_EN_SUB_TRG) != 0) {
        fprintf(stderr, "Failed to send EN_SUB_TRG\n");
        rc = -1;
        goto done;
    }

    printf("Verification: sending READ_TRG_CONF (0x%04X)...\n", OPCODE_READ_TRG_CONF);
    if (micro_write(handle, OPCODE_READ_TRG_CONF) != 0) {
        fprintf(stderr, "Failed to send READ_TRG_CONF\n");
        rc = -1;
        goto done;
    }
    for (i = 0; i < 5; i++) {
        if (micro_read(handle, &words[i]) != 0) {
            fprintf(stderr, "Failed to read word %d/5 of READ_TRG_CONF\n", i + 1);
            rc = -1;
            goto done;
        }
    }

    {
        int16_t width  = (int16_t)(words[0] << 4) >> 4;
        int16_t offset = (int16_t)(words[1] << 4) >> 4;
        int sub_active = words[4] & 0x1;

        printf("\n  Window width = %d (x25ns = %d ns)\n", width, width * 25);
        printf("  Window offset = %d (x25ns = %d ns)\n", offset, offset * 25);
        printf("  Trigger time subtraction = %s\n",
               sub_active ? "ACTIVE" : "INACTIVE");

        if (!sub_active) {
            fprintf(stderr,
                "\nWARNING: trigger subtraction does not appear to be active after the opcode.\n");
            rc = -1;
        } else {
            printf("\nFrom now on, the 'measure' field of TDC MEASUREMENT words\n"
                   "represents the time since the START OF THE MATCHING WINDOW (which\n"
                   "starts at trigger_time + offset, here %d ns). Delay relative to\n"
                   "the trigger = %d ns + (measure x resolution_ps / 1000).\n",
                   offset * 25, offset * 25);
        }
    }

done:
    CAENComm_CloseDevice(handle);
    printf("\nConnection closed.\n");

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
