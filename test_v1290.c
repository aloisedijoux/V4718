/*
 * test_v1290.c
 *
 * Test de connexion au V1290N (Multi-Hit TDC) via le pont V4718 en USB,
 * en utilisant directement CAENComm_OpenDevice2 (couche haut niveau,
 * celle utilisee par CAEN Toolbox en interne).
 *
 * Usage :
 *   ./test_v1290 <PID_V4718> <base_address_hex>
 *
 * Exemple (V1290N en slot 2, adresse 0x08000000) :
 *   ./test_v1290 64324 0x08000000
 *
 * Compilation :
 *   gcc -o test_v1290 test_v1290.c -lCAENComm
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CAENComm.h>

/* Offset du registre STATUS du V1290N (registre standard CAEN, 16 bits) */
#define REG_STATUS_OFFSET  0x1002

/* Offset des registres d'identification (Firmware) - a ajuster
   selon le manuel du V1290N si besoin d'aller plus loin */
#define REG_FIRMWARE_REV   0x1026

int main(int argc, char *argv[])
{
    CAENComm_ErrorCode err;
    int handle;
    uint32_t usb_link;
    uint32_t vme_base;
    uint16_t data;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <PID_V4718> <base_address_hex>\n", argv[0]);
        fprintf(stderr, "Exemple: %s 64324 0x08000000\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* PID du V4718 (celui utilise dans scan_full, ex: 64324) */
    usb_link = (uint32_t)strtoul(argv[1], NULL, 10);

    /* Adresse de base VME du V1290N, ex: 0x08000000 ou 0x03000000 */
    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    printf("Ouverture V1290N via V4718 (PID=%u) a l'adresse 0x%08X...\n",
           usb_link, vme_base);

    /* ConetNode = 0 : sans objet en USB direct (pertinent seulement en
       CONET/optique avec plusieurs modules en daisy chain) */
    err = CAENComm_OpenDevice2(
        CAENComm_USB_V4718,
        &usb_link,
        0,
        vme_base,
        &handle
    );

    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return EXIT_FAILURE;
    }

    printf("Connexion etablie. Handle = %d\n\n", handle);

    /* Lecture du registre STATUS (16 bits) */
    err = CAENComm_Read16(handle, REG_STATUS_OFFSET, &data);
    if (err == CAENComm_Success) {
        printf("STATUS (offset 0x%04X) = 0x%04X\n", REG_STATUS_OFFSET, data);
    } else {
        fprintf(stderr, "Echec lecture STATUS : code %d\n", err);
    }

    /* Lecture du registre Firmware Revision (16 bits) */
    err = CAENComm_Read16(handle, REG_FIRMWARE_REV, &data);
    if (err == CAENComm_Success) {
        printf("FIRMWARE_REV (offset 0x%04X) = 0x%04X\n", REG_FIRMWARE_REV, data);
    } else {
        fprintf(stderr, "Echec lecture FIRMWARE_REV : code %d\n", err);
    }

    CAENComm_CloseDevice(handle);
    printf("\nConnexion fermee proprement.\n");

    return EXIT_SUCCESS;
}