#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CAENComm.h>
#include "caen_open.h"

#define REG_MICRO 0x102E
#define REG_MICRO_HANDSHAKE 0x1030
#define HANDSHAKE_WRITE_OK (1 << 0)
#define HANDSHAKE_READ_OK (1 << 1)
#define OPCODE_CONT_STOR 0x0100
#define OPCODE_READ_ACQ_MOD 0x0200
#define MAX_POLL_ATTEMPTS 100000

static int micro_write(int handle, uint16_t word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;

    do {
        err = CAENComm_Read16(handle, REG_MICRO_HANDSHAKE, &handshake);
        if (err != CAENComm_Success) return -1;
        attempts++;
        if (attempts > MAX_POLL_ATTEMPTS) return -1;
    } while (!(handshake & HANDSHAKE_WRITE_OK));

    err = CAENComm_Write16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) return -1;

    return 0;
}

static int micro_read(int handle, uint16_t *word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;

    do {
        err = CAENComm_Read16(handle, REG_MICRO_HANDSHAKE, &handshake);
        if (err != CAENComm_Success) return -1;
        attempts++;
        if (attempts > MAX_POLL_ATTEMPTS) return -1;
    } while (!(handshake & HANDSHAKE_READ_OK));

    err = CAENComm_Read16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) return -1;

    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t vme_base;
    CAENComm_ErrorCode err;
    int handle;
    uint16_t reply;

    if (argc != 3) return EXIT_FAILURE;

    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    err = caen_open_v4718(argv[1], vme_base, &handle);
    if (err != CAENComm_Success) return EXIT_FAILURE;

    if (micro_write(handle, OPCODE_CONT_STOR) != 0) return EXIT_FAILURE;
    if (micro_write(handle, OPCODE_READ_ACQ_MOD) != 0) return EXIT_FAILURE;
    if (micro_read(handle, &reply) != 0) return EXIT_FAILURE;

    CAENComm_CloseDevice(handle);

    if (reply & 0x1) return EXIT_FAILURE;

    printf("Continuous Storage active\n");

    return EXIT_SUCCESS;
}