/*
 * module_reset.c
 * Registre utilise :
 *   Base + 0x1014  MODULE RESET (write only, D16). Un acces en
 *   ecriture (valeur indifferente) declenche le reset.
 *
 * Compilation :
 *   gcc -o module_reset module_reset.c -lCAENComm
 *
 * Usage :
 *   ./module_reset <PID_V4718> <base_address_hex>
 *
 * Exemple :
 *   ./module_reset 64324 0x03000000
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CAENComm.h>

#define REG_MODULE_RESET  0x1014

int main(int argc, char *argv[])
{
    uint32_t usb_link, vme_base;
    CAENComm_ErrorCode err;
    int handle;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <PID_V4718> <base_address_hex>\n", argv[0]);
        fprintf(stderr, "Example : %s 64324 0x03000000\n", argv[0]);
        return EXIT_FAILURE;
    }

    usb_link = (uint32_t)strtoul(argv[1], NULL, 10);
    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    printf("=== Complete software RESET of the module ===\n");
    printf("V4718 PID=%u\n", usb_link);
    printf("--------------------------------------------------\n");
    printf("Module at address 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Open failure: code %d\n", err);
        return EXIT_FAILURE;
    }
    printf("Connection established. Handle = %d\n\n", handle);

    printf("Sending RESET MODULE (0x%04X)...\n", REG_MODULE_RESET);
    err = CAENComm_Write16(handle, REG_MODULE_RESET, 0x0001);
    if (err != CAENComm_Success) {
        fprintf(stderr, "MODULE RESET write failure: code %d\n", err);
        CAENComm_CloseDevice(handle);
        return EXIT_FAILURE;
    }

    CAENComm_CloseDevice(handle);
    printf("\nConnection closed.\n");

    return EXIT_SUCCESS;
}
