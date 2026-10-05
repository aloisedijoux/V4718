/*
 * set_tdc_header.c
 *
 * CONFIRME SUR LE MATERIEL (05/10/2026) : "off" casse l'acquisition sur ce
 * module -- plus aucun mot ecrit dans l'Output Buffer du tout (pas juste
 * les triggers sans hit), meme apres plusieurs secondes d'acquisition
 * continue. Ne PAS utiliser "off" pendant une vraie acquisition. Garde ici
 * comme outil de lecture/diagnostic seulement -- daq_gui.py n'expose plus
 * de case pour le desactiver.
 *
 * Compilation :
 *   gcc -o set_tdc_header set_tdc_header.c -lCAENComm
 *
 * Usage :
 *   ./set_tdc_header <PID_or_IP_V4718> <base_address_hex> <on|off|read>
 *
 * Exemples :
 *   ./set_tdc_header 64324 0x03E00000 off
 *   ./set_tdc_header 192.168.1.254 0x03E00000 read
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <CAENComm.h>
#include "caen_open.h"

#define REG_MICRO            0x102E
#define REG_MICRO_HANDSHAKE  0x1030

#define HANDSHAKE_WRITE_OK   (1 << 0)
#define HANDSHAKE_READ_OK    (1 << 1)

#define OPCODE_EN_HEAD_TRAILER    0x3000
#define OPCODE_DIS_HEAD_TRAILER   0x3100
#define OPCODE_READ_HEAD_TRAILER  0x3200

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

/* Reads READ_HEAD_TRAILER. Stores 1 (enabled) or 0 (disabled) in *enabled. */
static int read_header_status(int handle, int *enabled)
{
    uint16_t reply;

    printf("Verification: sending READ_HEAD_TRAILER (0x%04X)...\n", OPCODE_READ_HEAD_TRAILER);
    if (micro_write(handle, OPCODE_READ_HEAD_TRAILER) != 0)
        return -1;
    if (micro_read(handle, &reply) != 0)
        return -1;

    *enabled = reply & 0x1;
    printf("  TDC Header/Trailer = %s (reply=0x%04X)\n",
           *enabled ? "ENABLED" : "DISABLED", reply);
    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t vme_base;
    CAENComm_ErrorCode err;
    int handle;
    int want = -1;      /* 1 = on, 0 = off, -1 = read only */
    int enabled = 0;
    int rc = 0;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> <base_address_hex> <on|off|read>\n", argv[0]);
        fprintf(stderr, "Example (USB): %s 64324 0x03E00000 off\n", argv[0]);
        fprintf(stderr, "Example (ETH): %s 192.168.1.254 0x03E00000 read\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[3], "on") == 0)
        want = 1;
    else if (strcmp(argv[3], "off") == 0)
        want = 0;
    else if (strcmp(argv[3], "read") != 0) {
        fprintf(stderr, "Unknown action '%s' (expected on, off or read)\n", argv[3]);
        return EXIT_FAILURE;
    }

    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    printf("=== TDC Header/Trailer in readout : %s ===\n",
           want == 1 ? "enable" : want == 0 ? "disable" : "read status");
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

    if (want >= 0) {
        uint16_t opcode = want ? OPCODE_EN_HEAD_TRAILER : OPCODE_DIS_HEAD_TRAILER;

        printf("Sending opcode %s (0x%04X)...\n",
               want ? "EN_HEAD_TRAILER" : "DIS_HEAD_TRAILER", opcode);
        if (micro_write(handle, opcode) != 0) {
            fprintf(stderr, "Failed to send the opcode\n");
            rc = -1;
            goto done;
        }
    }

    if (read_header_status(handle, &enabled) != 0) {
        fprintf(stderr, "Failed to read back the TDC Header/Trailer status\n");
        rc = -1;
        goto done;
    }

    if (want >= 0 && enabled != want) {
        fprintf(stderr, "\nWARNING: the setting read back does not match the requested one.\n");
        rc = -1;
    }

done:
    CAENComm_CloseDevice(handle);
    printf("\nConnection closed.\n");

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
