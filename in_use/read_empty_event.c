#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CAENComm.h>
#include "caen_open.h"

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <PID_or_IP> <base_hex>\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint32_t base = (uint32_t)strtoul(argv[2], NULL, 16);
    int handle;
    if (caen_open_v4718(argv[1], base, &handle) != CAENComm_Success) {
        fprintf(stderr, "open failed\n");
        return EXIT_FAILURE;
    }

    uint16_t ctrl;
    CAENComm_ErrorCode err = CAENComm_Read16(handle, 0x1000, &ctrl);
    CAENComm_CloseDevice(handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "read failed: code %d\n", err);
        return EXIT_FAILURE;
    }

    int empty_event = (ctrl >> 3) & 1;
    printf("Control register = 0x%04X\n", ctrl);
    printf("EMPTY_EVENT = %d (%s)\n", empty_event,
           empty_event ? "header+trailer written even with no data" : "nothing written if no data (default)");
    return EXIT_SUCCESS;
}
