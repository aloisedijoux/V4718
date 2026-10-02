/*
 * read_channel_pattern.c
 *
 * Outil PUREMENT EN LECTURE : relit le pattern d'activation des channels
 * d'un V1290N (via le pont V4718 en USB), sans ecrire aucun opcode de
 * configuration (ni DIS_ALL_CH, ni EN_ALL_CH, ni EN_CHANNEL/DIS_CHANNEL).
 * Utile pour verifier l'etat reel des channels apres une commande
 * d'activation qui aurait pu echouer en cours de route.
 *
 * Opcode utilise (protocole microcontroleur, manuel V1290 UM Rev.16,
 * §5.6.6) :
 *   0x4500  READ_EN_PATTERN : relit le pattern d'activation. Le nombre de
 *           mots de 16 bits lus depend du MODELE du module (pas des
 *           channels demandes) : un seul mot (channels 0-15) pour le
 *           V1290N, DEUX mots (0-15 puis 16-31) pour le V1290A -- lire le
 *           mauvais nombre de mots laisse le microcontroleur "en attente"
 *           et desynchronise toutes les commandes suivantes (voir
 *           caen_v1290_is_model_a() dans caen_open.h). Le modele est
 *           detecte automatiquement via la Configuration ROM (registre
 *           VME direct, pas le microcontroleur).
 *
 * Compilation :
 *   gcc -o read_channel_pattern read_channel_pattern.c -lCAENComm
 *
 * Usage :
 *   ./read_channel_pattern <PID_V4718> <base_address_hex>
 *
 * Exemple :
 *   ./read_channel_pattern 64324 0x03000000
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

#define OPCODE_READ_EN_PATTERN  0x4500

#define MAX_POLL_ATTEMPTS   100000

static int micro_write(int handle, uint16_t word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;

    do {
        err = CAENComm_Read16(handle, REG_MICRO_HANDSHAKE, &handshake);
        if (err != CAENComm_Success) {
            fprintf(stderr, "  [micro_write] Erreur lecture handshake : %d\n", err);
            return -1;
        }
        if (++attempts > MAX_POLL_ATTEMPTS) {
            fprintf(stderr, "  [micro_write] Timeout WRITE_OK (handshake=0x%04X)\n", handshake);
            return -1;
        }
    } while (!(handshake & HANDSHAKE_WRITE_OK));

    err = CAENComm_Write16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) {
        fprintf(stderr, "  [micro_write] Erreur ecriture MICRO : %d\n", err);
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
            fprintf(stderr, "  [micro_read] Erreur lecture handshake : %d\n", err);
            return -1;
        }
        if (++attempts > MAX_POLL_ATTEMPTS) {
            fprintf(stderr, "  [micro_read] Timeout READ_OK (handshake=0x%04X)\n", handshake);
            return -1;
        }
    } while (!(handshake & HANDSHAKE_READ_OK));

    err = CAENComm_Read16(handle, REG_MICRO, word);
    if (err != CAENComm_Success) {
        fprintf(stderr, "  [micro_read] Erreur lecture MICRO : %d\n", err);
        return -1;
    }

    return 0;
}

static void print_active_channels(uint32_t pattern, int n_channels)
{
    int ch;
    int first = 1;

    printf("  Channels actifs : ");
    if (pattern == 0) {
        printf("aucun\n");
        return;
    }
    for (ch = 0; ch < n_channels; ch++) {
        if (pattern & (1u << ch)) {
            if (!first)
                printf(", ");
            printf("%d", ch);
            first = 0;
        }
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    uint32_t vme_base;
    CAENComm_ErrorCode err;
    int handle;
    uint16_t word0 = 0, word1 = 0;
    uint32_t pattern;
    int is_model_a;
    int rc = 0;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> <base_address_hex>\n", argv[0]);
        fprintf(stderr, "Exemple (USB) : %s 64324 0x03000000\n", argv[0]);
        fprintf(stderr, "Exemple (ETH) : %s 192.168.1.254 0x03000000\n", argv[0]);
        return EXIT_FAILURE;
    }

    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    printf("=== Lecture (seule) du pattern d'activation des channels ===\n");
    printf("V4718 PID/IP=%s\n", argv[1]);
    printf("--------------------------------------------------\n");
    printf("Module a l'adresse 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = caen_open_v4718(argv[1], vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return EXIT_FAILURE;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    is_model_a = caen_v1290_is_model_a(handle);
    if (is_model_a < 0) {
        fprintf(stderr, "Echec detection du modele (lecture Configuration ROM) : code %d\n", is_model_a);
        rc = -1;
        goto done;
    }
    printf("Modele detecte : V1290%s\n", is_model_a ? "A (32 channels)" : "N (16 channels)");

    printf("Envoi opcode READ_EN_PATTERN (0x%04X)...\n", OPCODE_READ_EN_PATTERN);
    if (micro_write(handle, OPCODE_READ_EN_PATTERN) != 0) {
        fprintf(stderr, "Echec envoi READ_EN_PATTERN\n");
        rc = -1;
        goto done;
    }
    if (micro_read(handle, &word0) != 0) {
        fprintf(stderr, "Echec lecture pattern d'activation (mot 0)\n");
        rc = -1;
        goto done;
    }
    if (is_model_a) {
        /* V1290A: READ_EN_PATTERN always answers with a SECOND word
           (channels 16-31) -- it must be read here or the microcontroller
           stays wedged waiting for it, desyncing every command after this
           one (manual §5.6.6). */
        if (micro_read(handle, &word1) != 0) {
            fprintf(stderr, "Echec lecture pattern d'activation (mot 1, channels 16-31)\n");
            rc = -1;
            goto done;
        }
    }

    pattern = (uint32_t)word0 | ((uint32_t)word1 << 16);
    if (is_model_a) {
        printf("\n  PATTERN (channels 0-31) = 0x%08X  (mot0=0x%04X, mot1=0x%04X)\n",
               pattern, word0, word1);
        print_active_channels(pattern, 32);
    } else {
        printf("\n  PATTERN (channels 0-15) = 0x%04X\n", pattern);
        print_active_channels(pattern, 16);
    }

done:
    CAENComm_CloseDevice(handle);
    printf("\nConnexion fermee proprement.\n");

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
