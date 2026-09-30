/*
 * enable_channels.c
 *
 * Active tous les channels des V1290N (via le pont V4718 en USB), en
 * utilisant le protocole microcontroleur (registres MICRO / MICRO_
 * HANDSHAKE) decrit dans le manuel technique V1290 (UM Rev.12, §5).
 *
 * Opcodes utilises (§5.6 du manuel V1290 rev.16) :
 *   0x4200  EN_ALL_CH        : active tous les channels (0 operande, PR=c,
 *                              c-a-d reprogrammation des "control registers"
 *                              du TDC, PAS de clear des donnees)
 *   0x4500  READ_EN_PATTERN  : relit le pattern d'activation (1 mot pour
 *                              le modele N, channels 0-15)
 *
 * Par defaut (sans adresse en argument), traite les deux V1290N connus
 * (slot 2 @ 0x08000000 et slot 3 @ 0x03000000). Une adresse unique peut
 * aussi etre ciblee.
 *
 * Compilation :
 *   gcc -o enable_channels enable_channels.c -lCAENComm
 *
 * Usage :
 *   ./enable_channels <PID_V4718> [base_address_hex]
 *
 * Exemples :
 *   ./enable_channels 64324                 -> active les 2 TDC connus
 *   ./enable_channels 64324 0x08000000      -> active seulement le slot 2
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

#define OPCODE_EN_ALL_CH     0x4200
#define OPCODE_READ_EN_PATTERN  0x4500  /* CODE 45xx, 1 mot pour le V1290N -> channels [15:0] */

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

/* Envoie EN_ALL_CH puis relit le pattern d'activation pour confirmer que
   tous les channels sont bien actifs. Le nombre de mots lus en reponse a
   READ_EN_PATTERN depend du MODELE du module (1 mot/16 channels pour le
   V1290N, 2 mots/32 channels pour le V1290A, manuel §5.6.6) : lire le
   mauvais nombre de mots laisse le microcontroleur en attente et
   desynchronise toute commande suivante -- d'ou la detection du modele
   via caen_v1290_is_model_a() (registre VME direct, pas le
   microcontroleur) avant de savoir combien de mots consommer ici. */
static int enable_all_channels(int handle)
{
    uint16_t word0, word1 = 0;
    uint32_t pattern, expected;
    int is_model_a;

    is_model_a = caen_v1290_is_model_a(handle);
    if (is_model_a < 0) {
        fprintf(stderr, "Echec detection du modele (lecture Configuration ROM)\n");
        return -1;
    }

    printf("Envoi opcode EN_ALL_CH (0x%04X)...\n", OPCODE_EN_ALL_CH);
    if (micro_write(handle, OPCODE_EN_ALL_CH) != 0) {
        fprintf(stderr, "Echec envoi EN_ALL_CH\n");
        return -1;
    }

    printf("Verification : envoi READ_EN_PATTERN (0x%04X)...\n",
           OPCODE_READ_EN_PATTERN);
    if (micro_write(handle, OPCODE_READ_EN_PATTERN) != 0) {
        fprintf(stderr, "Echec envoi READ_EN_PATTERN\n");
        return -1;
    }

    if (micro_read(handle, &word0) != 0) {
        fprintf(stderr, "Echec lecture pattern d'activation (mot 0)\n");
        return -1;
    }
    if (is_model_a) {
        if (micro_read(handle, &word1) != 0) {
            fprintf(stderr, "Echec lecture pattern d'activation (mot 1, channels 16-31)\n");
            return -1;
        }
    }

    pattern = (uint32_t)word0 | ((uint32_t)word1 << 16);
    expected = is_model_a ? 0xFFFFFFFFu : 0xFFFFu;

    if (is_model_a)
        printf("  Pattern d'activation (channels 0-31) = 0x%08X (mot0=0x%04X, mot1=0x%04X)\n",
               pattern, word0, word1);
    else
        printf("  Pattern d'activation (channels 0-15) = 0x%04X\n", word0);

    if (pattern != expected) {
        fprintf(stderr, "ATTENTION : tous les channels ne sont pas actifs "
                        "(attendu 0x%08X, obtenu 0x%08X)\n", expected, pattern);
        return -1;
    }

    printf("  Tous les %d channels sont actifs.\n", is_model_a ? 32 : 16);
    return 0;
}

static int process_module(const char *conn_arg, uint32_t vme_base)
{
    CAENComm_ErrorCode err;
    int handle;
    int rc;

    printf("--------------------------------------------------\n");
    printf("Module a l'adresse 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = caen_open_v4718(conn_arg, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return -1;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    rc = enable_all_channels(handle);

    if (rc == 0) {
        printf("\nModule 0x%08X : tous les channels actives et confirmes.\n", vme_base);
    } else {
        fprintf(stderr, "\nModule 0x%08X : echec de l'activation des channels.\n", vme_base);
    }

    CAENComm_CloseDevice(handle);
    printf("Connexion fermee proprement.\n\n");

    return rc;
}

int main(int argc, char *argv[])
{
    int overall_rc = 0;

    static const uint32_t default_bases[] = { 0x08000000, 0x03000000 };

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> [base_address_hex]\n", argv[0]);
        fprintf(stderr, "Exemple (2 TDC par defaut, USB) : %s 64324\n", argv[0]);
        fprintf(stderr, "Exemple (1 TDC cible, ETH)      : %s 192.168.1.254 0x08000000\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("=== Activation de tous les channels ===\n");
    printf("V4718 PID/IP=%s\n\n", argv[1]);

    if (argc >= 3) {
        uint32_t vme_base = (uint32_t)strtoul(argv[2], NULL, 16);
        overall_rc = process_module(argv[1], vme_base);
    } else {
        int i;
        for (i = 0; i < 2; i++) {
            int rc = process_module(argv[1], default_bases[i]);
            if (rc != 0)
                overall_rc = rc;
        }
    }

    if (overall_rc == 0) {
        printf("=== Tous les modules ont leurs channels actives avec succes. ===\n");
    } else {
        printf("=== Au moins un module a rencontre un probleme. Voir logs ci-dessus. ===\n");
    }

    return (overall_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}