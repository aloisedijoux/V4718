#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CAENComm.h>
#include "caen_open.h"
#define REG_MICRO            0x102E
#define REG_MICRO_HANDSHAKE  0x1030
#define HANDSHAKE_WRITE_OK   (1 << 0)
#define HANDSHAKE_READ_OK    (1 << 1)
#define OPCODE_READ_DETECTION 0x2300
#define MAX_POLL_ATTEMPTS    100000

static int micro_write(int handle, uint16_t word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;
    do {
        err = CAENComm_Read16(handle, REG_MICRO_HANDSHAKE, &handshake);
        if (err != CAENComm_Success) { fprintf(stderr, "  [micro_write] Erreur lecture handshake : %d\n", err); return -1; }
        if (++attempts > MAX_POLL_ATTEMPTS) { fprintf(stderr, "  [micro_write] Timeout WRITE_OK (handshake=0x%04X)\n", handshake); return -1; }
    } while (!(handshake & HANDSHAKE_WRITE_OK));
    err = CAENComm_Write16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) { fprintf(stderr, "  [micro_write] Erreur ecriture MICRO : %d\n", err); return -1; }
    return 0;
}

static int micro_read(int handle, uint16_t *word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;
    do {
        err = CAENComm_Read16(handle, REG_MICRO_HANDSHAKE, &handshake);
        if (err != CAENComm_Success) { fprintf(stderr, "  [micro_read] Erreur lecture handshake : %d\n", err); return -1; }
        if (++attempts > MAX_POLL_ATTEMPTS) { fprintf(stderr, "  [micro_read] Timeout READ_OK (handshake=0x%04X)\n", handshake); return -1; }
    } while (!(handshake & HANDSHAKE_READ_OK));
    err = CAENComm_Read16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) { fprintf(stderr, "  [micro_read] Erreur lecture MICRO : %d\n", err); return -1; }
    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t vme_base;
    CAENComm_ErrorCode err;
    int handle;
    uint16_t word;
    int is_model_a;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> <base_address_hex>\n", argv[0]);
        fprintf(stderr, "Exemple (ETH) : %s 192.168.1.254 0x03E00000\n", argv[0]);
        return EXIT_FAILURE;
    }

    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    printf("=== Lecture du mode de detection des fronts (READ_DETECTION) ===\n");
    printf("V4718 PID/IP=%s\n", argv[1]);
    printf("Module a l'adresse 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = caen_open_v4718(argv[1], vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return EXIT_FAILURE;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    is_model_a = caen_v1290_is_model_a(handle);
    if (is_model_a >= 0) {
        printf("Modele detecte : V1290%s\n", is_model_a ? "A" : "N");
    }

    if (micro_write(handle, OPCODE_READ_DETECTION) != 0) {
        fprintf(stderr, "Echec envoi READ_DETECTION\n");
        CAENComm_CloseDevice(handle);
        return EXIT_FAILURE;
    }
    if (micro_read(handle, &word) != 0) {
        fprintf(stderr, "Echec lecture du mode de detection\n");
        CAENComm_CloseDevice(handle);
        return EXIT_FAILURE;
    }

    printf("Reponse brute = 0x%04X\n", word);
    switch (word & 0x3) {
        case 0x0:
            printf("  Mode = PAIR MODE (00) -- uniquement en lower resolution mode\n");
            break;
        case 0x1:
            printf("  Mode = TRAILING SEULEMENT (01)\n");
            break;
        case 0x2:
            printf("  Mode = LEADING SEULEMENT (10)  <- reglage normal pour 1 hit par impulsion\n");
            break;
        case 0x3:
            printf("  Mode = TRAILING ET LEADING (11)  <- ATTENTION : 2 hits par impulsion reelle !\n");
            break;
    }

    CAENComm_CloseDevice(handle);
    printf("\nConnexion fermee proprement.\n");
    return EXIT_SUCCESS;
}
 
 