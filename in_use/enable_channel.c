/*
 * enable_channel.c
 *
 * Active UN SEUL channel (tous les autres desactives) d'un V1290N via
 * le pont V4718 en USB, en utilisant le protocole microcontroleur
 * (registres MICRO / MICRO_HANDSHAKE) decrit dans le manuel technique
 * V1290 (UM Rev.16, §5.6).
 *
 * Opcodes utilises :
 *   0x43xx  DIS_ALL_CH       : desactive tous les channels (0 operande)
 *   0x40nn  EN_CHANNEL       : active le channel nn -- le numero de channel
 *                              est encode directement dans l'octet bas de
 *                              l'opcode (PAS d'operande separe), plage
 *                              0-15 pour le V1290N, 0-31 pour le V1290A
 *   0x45xx  READ_EN_PATTERN  : relit le pattern d'activation. 1 mot pour le
 *                              V1290N (channels 0-15), 2 mots pour le
 *                              V1290A (channels 0-15 puis 16-31) -- lire le
 *                              mauvais nombre de mots desynchronise le
 *                              microcontroleur pour toute commande suivante
 *                              (manuel §5.6.6) ; modele detecte via
 *                              caen_v1290_is_model_a() (caen_open.h).
 *
 * Compilation :
 *   gcc -o enable_channel enable_channel.c -lCAENComm
 *
 * Usage :
 *   ./enable_channel <PID_V4718> <base_address_hex> <channel>
 *
 * Exemple :
 *   ./enable_channel 64324 0x03000000 0   -> active seulement le channel 0 sur le slot 3
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

#define OPCODE_DIS_ALL_CH        0x4300
#define OPCODE_EN_CHANNEL_BASE   0x4000  /* + numero de channel (0-15) dans l'octet bas */
#define OPCODE_READ_EN_PATTERN   0x4500  /* 1 mot pour le V1290N -> channels [15:0] */

#define MAX_POLL_ATTEMPTS   100000

static int micro_write(int handle, uint16_t word)
{
    CAENComm_ErrorCode err;
    uint16_t handshake;
    int attempts = 0;

    /* REG_MICRO fonctionne comme une boite aux lettres : le microcontroleur
       du V1290 n'est pret a recevoir un mot que lorsque WRITE_OK est a 1.
       On poll donc le handshake avant d'ecrire, sous peine de corrompre
       la sequence d'opcodes. */
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

    /* Symetrique de micro_write : la reponse du microcontroleur n'est
       disponible dans REG_MICRO qu'apres que READ_OK passe a 1. */
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

/* Desactive tous les channels, active uniquement "channel", puis relit
   le pattern d'activation pour confirmer. Le nombre de mots lus en reponse
   a READ_EN_PATTERN depend du modele (1 pour le V1290N, 2 pour le V1290A,
   manuel §5.6.6) -- en lire le mauvais nombre desynchronise le
   microcontroleur pour toute commande suivante, d'ou la detection du
   modele avant de savoir combien de mots consommer. */
static int enable_single_channel(int handle, int channel)
{
    uint16_t word0, word1 = 0;
    uint32_t pattern, expected;
    int is_model_a;

    is_model_a = caen_v1290_is_model_a(handle);
    if (is_model_a < 0) {
        fprintf(stderr, "Echec detection du modele (lecture Configuration ROM)\n");
        return -1;
    }
    if (!is_model_a && channel > 15) {
        fprintf(stderr, "Channel %d hors plage pour le V1290N detecte (0-15)\n", channel);
        return -1;
    }

    printf("Envoi opcode DIS_ALL_CH (0x%04X)...\n", OPCODE_DIS_ALL_CH);
    if (micro_write(handle, OPCODE_DIS_ALL_CH) != 0) {
        fprintf(stderr, "Echec envoi DIS_ALL_CH\n");
        return -1;
    }

    {
        /* Pas d'operande separe pour EN_CHANNEL : le numero de canal est
           directement encode dans l'octet bas de l'opcode (0x40 | channel). */
        uint16_t opcode_en = (uint16_t)(OPCODE_EN_CHANNEL_BASE | (channel & 0xFF));

        printf("Envoi opcode EN_CHANNEL (0x%04X, channel=%d)...\n", opcode_en, channel);
        if (micro_write(handle, opcode_en) != 0) {
            fprintf(stderr, "Echec envoi EN_CHANNEL\n");
            return -1;
        }
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
    expected = (1u << channel);

    if (is_model_a)
        printf("  Pattern d'activation (channels 0-31) = 0x%08X (mot0=0x%04X, mot1=0x%04X)\n",
               pattern, word0, word1);
    else
        printf("  Pattern d'activation (channels 0-15) = 0x%04X\n", word0);

    /* Un seul bit doit etre a 1 : celui du canal demande. */
    if (pattern != expected) {
        fprintf(stderr, "ATTENTION : pattern inattendu (attendu 0x%08X pour "
                        "channel %d seul, obtenu 0x%08X)\n",
                expected, channel, pattern);
        return -1;
    }

    printf("  Channel %d actif, tous les autres desactives.\n", channel);
    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t vme_base;
    int channel;
    CAENComm_ErrorCode err;
    int handle;
    int rc;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <PID_or_IP_V4718> <base_address_hex> <channel>\n", argv[0]);
        fprintf(stderr, "Exemple (USB) : %s 64324 0x03000000 0\n", argv[0]);
        fprintf(stderr, "Exemple (ETH) : %s 192.168.1.254 0x03000000 0\n", argv[0]);
        return EXIT_FAILURE;
    }

    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);
    channel  = atoi(argv[3]); // récupère le numéro de channel en argument : conve

    if (channel < 0 || channel > 31) { // 0-15 sur V1290N, 0-31 sur V1290A (verifie apres detection du modele)
        fprintf(stderr, "Channel invalide (0-31 : 0-15 sur V1290N, 0-31 sur V1290A) : %d\n", channel);
        return EXIT_FAILURE;
    }

    printf("=== Activation d'un channel unique ===\n");
    printf("V4718 PID/IP=%s\n", argv[1]);
    printf("--------------------------------------------------\n");
    printf("Module a l'adresse 0x%08X, channel cible = %d\n", vme_base, channel);
    printf("--------------------------------------------------\n");

    err = caen_open_v4718(argv[1], vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return EXIT_FAILURE;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    rc = enable_single_channel(handle, channel);

    if (rc == 0) {
        printf("\nModule 0x%08X : channel %d active et confirme (seul actif).\n",
               vme_base, channel);
    } else {
        fprintf(stderr, "\nModule 0x%08X : echec de l'activation du channel %d.\n",
                vme_base, channel);
    }

    CAENComm_CloseDevice(handle);
    printf("Connexion fermee proprement.\n");

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
