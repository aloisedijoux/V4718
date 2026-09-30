/*
 * config_trigger_matching.c
 * Compilation:
 *   gcc -o config_trigger_matching config_trigger_matching.c -lCAENComm
 *
 * Usage:
 *   ./config_trigger_matching <PID_V4718> [base_address_hex] [width] [offset] [search_margin] [reject_margin]
 *
 * Examples:
 *   ./config_trigger_matching 64324
 *       -> trigger matching mode using the module's default settings
 *          (width 500ns / offset -1us / search margin 200ns / reject margin 100ns)
 *          on the 2 known TDCs.
 *
 *   ./config_trigger_matching 64324 0x08000000 1 1 1 1
 *       -> trigger matching mode on slot 2 only, with the 4 parameters
 *          set to 1 clock cycle (25 ns) each.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <CAENComm.h>
#include "caen_open.h"

#define REG_MICRO            0x102E
#define REG_MICRO_HANDSHAKE  0x1030
#define REG_STATUS           0x1002

#define HANDSHAKE_WRITE_OK   (1 << 0)
#define HANDSHAKE_READ_OK    (1 << 1)

#define OPCODE_TRG_MATCH       0x0000
#define OPCODE_SET_WIN_WIDTH   0x1000
#define OPCODE_SET_WIN_OFFS    0x1100
#define OPCODE_SET_SW_MARGIN   0x1200
#define OPCODE_SET_REJ_MARGIN  0x1300
#define OPCODE_LOAD_DEF_CONFIG 0x0500
#define OPCODE_READ_ACQ_MOD    0x0200
#define OPCODE_READ_TRG_CONF   0x1600

#define MAX_POLL_ATTEMPTS   100000

/* Waits for the WRITE_OK bit to be active, then writes the 16-bit word
   to MICRO. Returns 0 on success, -1 on timeout or VME error. */
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

/* Waits for the READ_OK bit to be active, then reads a 16-bit word
   from MICRO. Returns 0 on success, -1 on timeout or VME error. */
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

/* Sends an opcode without an operand (e.g., TRG_MATCH). */
static int send_opcode(int handle, uint16_t opcode, const char *label)
{
    printf("Sending opcode %s (0x%04X)...\n", label, opcode);
    if (micro_write(handle, opcode) != 0) {
        fprintf(stderr, "Failed to send opcode %s\n", label);
        return -1;
    }
    return 0;
}

/* Sends an opcode followed by a 16-bit operand (e.g., SET_WIN_WIDTH). */
static int send_opcode_with_operand(int handle, uint16_t opcode,
                                     uint16_t operand, const char *label)
{
    printf("Sending opcode %s (0x%04X) with operand 0x%04X...\n",
           label, opcode, operand);
    if (micro_write(handle, opcode) != 0) {
        fprintf(stderr, "Failed to send opcode %s\n", label);
        return -1;
    }
    if (micro_write(handle, operand) != 0) {
        fprintf(stderr, "Failed to send operand for %s\n", label);
        return -1;
    }
    return 0;
}

/* Reads the currently active acquisition mode and displays it. */
static int verify_acquisition_mode(int handle)
{
    uint16_t reply;

    printf("Verification : sending READ_ACQ_MOD (0x%04X)...\n", OPCODE_READ_ACQ_MOD);
    if (micro_write(handle, OPCODE_READ_ACQ_MOD) != 0)
        return -1;

    if (micro_read(handle, &reply) != 0)
        return -1;

    printf("  Acquisition mode read = 0x%04X -> %s\n", reply,
           (reply & 0x1) ? "TRIGGER MATCHING" : "CONTINUOUS STORAGE");

    return (reply & 0x1) ? 0 : -1;  /* -1 if the mode is not Trigger Matching */
}

/* Reads the 5 trigger configuration words (width, offset, search margin,
   reject margin, trigger time subtraction flag) and displays them to
   confirm that the configuration has been correctly applied by the
   microcontroller (and not just accepted in writing). */
static int verify_trigger_config(int handle)
{
    uint16_t words[5];
    int i;

    printf("Verification : sending READ_TRG_CONF (0x%04X)...\n", OPCODE_READ_TRG_CONF);
    if (micro_write(handle, OPCODE_READ_TRG_CONF) != 0)
        return -1;

    for (i = 0; i < 5; i++) {
        if (micro_read(handle, &words[i]) != 0) {
            fprintf(stderr, "  Failed to read word %d/5 of READ_TRG_CONF\n", i + 1);
            return -1;
        }
    }

    /* Sign extension for 12-bit width/offset/margins (bits [11:0]) */
    int16_t width  = (int16_t)(words[0] << 4) >> 4;
    int16_t offset = (int16_t)(words[1] << 4) >> 4;
    int16_t margin = (int16_t)(words[2] << 4) >> 4;
    int16_t reject = (int16_t)(words[3] << 4) >> 4;

    printf("  Window width        = %d (x25ns = %d ns)\n", width, width * 25);
    printf("  Window offset       = %d (x25ns = %d ns)\n", offset, offset * 25);
    printf("  Search margin       = %d (x25ns = %d ns)\n", margin, margin * 25);
    printf("  Reject margin       = %d (x25ns = %d ns)\n", reject, reject * 25);
    printf("  Trigger subtraction = %s\n",
           (words[4] & 0x1) ? "active" : "inactive");

    return 0;
}


static int configure_trigger_matching(int handle, int has_window,
                                       int16_t win_width, int16_t win_offset,
                                       int16_t search_margin, int16_t reject_margin)
{
    /* 1. Switch to Trigger Matching Mode.
          NB: this opcode reprograms the "setup" registers of the TDC, which
          implies a clear of the data (normal behavior, see §5.2.1). */
    if (send_opcode(handle, OPCODE_TRG_MATCH, "TRG_MATCH") != 0)
        return -1;

    if (!has_window)
        return 0;

    /* 2. Window width for matching. Unit = 25 ns/clock cycle. */
    if (send_opcode_with_operand(handle, OPCODE_SET_WIN_WIDTH,
                                  (uint16_t)win_width, "SET_WIN_WIDTH") != 0)
        return -1;

    /* 3. Window offset. 12-bit signed value, transferred as 16 bits
          (sign extension). Unit = 25 ns/clock cycle. */
    if (send_opcode_with_operand(handle, OPCODE_SET_WIN_OFFS,
                                  (uint16_t)win_offset, "SET_WIN_OFFS") != 0)
        return -1;

    /* 4. Extra Search Margin. Unit = 25 ns/clock cycle. */
    if (send_opcode_with_operand(handle, OPCODE_SET_SW_MARGIN,
                                  (uint16_t)search_margin, "SET_SW_MARGIN") != 0)
        return -1;

    /* 5. Reject Margin. Unit = 25 ns/clock cycle, recommended >= 1. */
    if (send_opcode_with_operand(handle, OPCODE_SET_REJ_MARGIN,
                                  (uint16_t)reject_margin, "SET_REJ_MARGIN") != 0)
        return -1;

    return 0;
}

/* Configure and verify a V1290N module at the given address. Returns 0
   if everything went well (config + verification), -1 otherwise. */
static int process_module(const char *conn_arg, uint32_t vme_base,
                           int has_window, int16_t win_width, int16_t win_offset,
                           int16_t search_margin, int16_t reject_margin)
{
    CAENComm_ErrorCode err;
    int handle;
    uint16_t status;
    int rc = 0;

    printf("--------------------------------------------------\n");
    printf("Module at address 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = caen_open_v4718(conn_arg, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Failed to open device: code %d\n", err);
        return -1;
    }
    printf("Connection established. Handle = %d\n\n", handle);

    if (configure_trigger_matching(handle, has_window, win_width, win_offset,
                                    search_margin, reject_margin) != 0) {
        fprintf(stderr, "\nFailed to configure device.\n");
        CAENComm_CloseDevice(handle);
        return -1;
    }

    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err == CAENComm_Success) {
        printf("\nSTATUS after configuration = 0x%04X\n", status);
    }

    printf("\n");
    if (verify_acquisition_mode(handle) != 0) {
        fprintf(stderr, "WARNING: Trigger Matching mode is not confirmed active!\n");
        rc = -1;
    }

    printf("\n");
    if (has_window) {
        if (verify_trigger_config(handle) != 0) {
            fprintf(stderr, "WARNING: Unable to read back trigger configuration.\n");
            rc = -1;
        }
    }

    if (rc == 0) {
        printf("\nModule 0x%08X : configuration confirmed successfully.\n", vme_base);
    }

    CAENComm_CloseDevice(handle);
    printf("Connection closed properly.\n\n");

    return rc;
}

int main(int argc, char *argv[])
{
    int has_window = 0;
    int16_t win_width = 40;
    int16_t win_offset = 10;
    int16_t search_margin = 20;
    int16_t reject_margin = 20;
    int overall_rc = 0;

    /* Default addresses of the two known V1290N modules (slot 2 and slot 3) */
    static const uint32_t default_bases[] = { 0x08000000, 0x03000000 };

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> [base_address_hex] [width] [offset] [search_margin] [reject_margin]\n", argv[0]);
        fprintf(stderr, "Example (2 TDCs, factory settings, USB) : %s 64324\n", argv[0]);
        fprintf(stderr, "Example (1 TDC, 1 tick=25ns, ETH)        : %s 192.168.1.254 0x08000000 1 1 1 1\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("=== Configuration Trigger Matching Mode ===\n");
    printf("V4718 PID/IP=%s\n\n", argv[1]);

    if (argc >= 3) {
        /* Explicitly targeted unique address */
        uint32_t vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

        if (argc >= 7) {
            has_window    = 1;
            win_width     = (int16_t)atoi(argv[3]);
            win_offset    = (int16_t)atoi(argv[4]);
            search_margin = (int16_t)atoi(argv[5]);
            reject_margin = (int16_t)atoi(argv[6]);
        }

        overall_rc = process_module(argv[1], vme_base, has_window,
                                     win_width, win_offset, search_margin, reject_margin);
    } else {
        /* No address provided: process the two known TDCs with the
           module's default window settings. */
        int i;
        for (i = 0; i < 2; i++) {
            int rc = process_module(argv[1], default_bases[i], has_window,
                                     win_width, win_offset, search_margin, reject_margin);
            if (rc != 0)
                overall_rc = rc;
        }
    }

    if (overall_rc == 0) {
        printf("=== All modules have been configured and verified successfully. ===\n");
    } else {
        printf("=== At least one module encountered a problem. See logs above. ===\n");
    }

    return (overall_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}