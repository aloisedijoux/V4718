/*
 * config_trigger_matching.c
 *
 * Configure un ou deux V1290N (via le pont V4718 en USB) en Trigger
 * Matching Mode, avec une fenetre de matching (largeur + offset) au
 * choix, en suivant le protocole microcontroleur decrit dans le manuel
 * technique V1290 (UM Rev.12, Chap. 5) :
 *
 *   - Les registres MICRO (offset 0x102E) et MICRO_HANDSHAKE (0x1030)
 *     forment un canal de commande vers le microcontroleur embarque.
 *   - Avant CHAQUE ecriture dans MICRO, on doit attendre que le bit
 *     WRITE_OK (bit 0) de MICRO_HANDSHAKE soit a 1.
 *   - Avant CHAQUE lecture dans MICRO (reponse), on doit attendre que
 *     le bit READ_OK (bit 1) de MICRO_HANDSHAKE soit a 1.
 *   - Un OPCODE tient sur 16 bits ; certains opcodes attendent ensuite
 *     un ou plusieurs operandes de 16 bits (nW), toujours au meme
 *     offset MICRO.
 *
 * Opcodes utilises ici (Table 5.1 du manuel) :
 *   0x0000  TRG_MATCH        : passe en Trigger Matching Mode (0 operande)
 *   0x1000  SET_WIN_WIDTH    : regle la largeur de fenetre (1 operande, 12 bits)
 *   0x1100  SET_WIN_OFFS     : regle l'offset de fenetre   (1 operande, 12 bits signe)
 *   0x0500  LOAD_DEF_CONFIG  : recharge la config par defaut (0 operande) [optionnel]
 *
 * Compilation :
 *   gcc -o config_trigger_matching config_trigger_matching.c -lCAENComm
 *
 * Usage :
 *   ./config_trigger_matching <PID_V4718> <base_address_hex> [win_width] [win_offset]
 *
 * Exemples :
 *   ./config_trigger_matching 64324 0x08000000
 *       -> trigger matching mode avec les reglages par defaut du module
 *          (largeur 500ns / offset -1us), juste apres avoir force le mode.
 *
 *   ./config_trigger_matching 64324 0x08000000 40 -10
 *       -> trigger matching mode, largeur = 40 x 25ns = 1000ns,
 *          offset = -10 x 25ns = -250ns
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <CAENComm.h>

#define REG_MICRO            0x102E
#define REG_MICRO_HANDSHAKE  0x1030
#define REG_STATUS           0x1002

#define HANDSHAKE_WRITE_OK   (1 << 0)
#define HANDSHAKE_READ_OK    (1 << 1)

#define OPCODE_TRG_MATCH       0x0000
#define OPCODE_SET_WIN_WIDTH   0x1000
#define OPCODE_SET_WIN_OFFS    0x1100
#define OPCODE_LOAD_DEF_CONFIG 0x0500

#define MAX_POLL_ATTEMPTS   100000

/* Attend que le bit WRITE_OK soit actif, puis ecrit le mot de 16 bits
   dans MICRO. Retourne 0 si succes, -1 en cas de timeout ou d'erreur
   VME. */
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

/* Attend que le bit READ_OK soit actif, puis lit un mot de 16 bits
   depuis MICRO. Retourne 0 si succes, -1 en cas de timeout ou d'erreur
   VME. */
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

/* Envoie un opcode sans operande (ex: TRG_MATCH). */
static int send_opcode(int handle, uint16_t opcode, const char *label)
{
    printf("Envoi opcode %s (0x%04X)...\n", label, opcode);
    if (micro_write(handle, opcode) != 0) {
        fprintf(stderr, "Echec envoi opcode %s\n", label);
        return -1;
    }
    return 0;
}

/* Envoie un opcode suivi d'un operande 16 bits (ex: SET_WIN_WIDTH). */
static int send_opcode_with_operand(int handle, uint16_t opcode,
                                     uint16_t operand, const char *label)
{
    printf("Envoi opcode %s (0x%04X) avec operande 0x%04X...\n",
           label, opcode, operand);
    if (micro_write(handle, opcode) != 0) {
        fprintf(stderr, "Echec envoi opcode %s\n", label);
        return -1;
    }
    if (micro_write(handle, operand) != 0) {
        fprintf(stderr, "Echec envoi operande pour %s\n", label);
        return -1;
    }
    return 0;
}

/* Configure un module V1290N en Trigger Matching Mode, avec fenetre
   optionnelle. Retourne 0 si succes. */
static int configure_trigger_matching(int handle, int has_window,
                                       int16_t win_width, int16_t win_offset)
{
    /* 1. Passage en Trigger Matching Mode.
          NB: cet opcode reprogramme les registres "setup" du TDC, ce qui
          implique un clear des donnees (comportement normal, voir §5.2.1). */
    if (send_opcode(handle, OPCODE_TRG_MATCH, "TRG_MATCH") != 0)
        return -1;

    /* 2. Optionnel : reglage de la largeur de fenetre de matching.
          Valeur en unites de 25 ns, plage [1 ; 4095]. */
    if (has_window) {
        if (send_opcode_with_operand(handle, OPCODE_SET_WIN_WIDTH,
                                      (uint16_t)win_width, "SET_WIN_WIDTH") != 0)
            return -1;

        /* 3. Optionnel : reglage de l'offset de fenetre.
              Valeur signee 12 bits, transferee sur 16 bits (extension de signe). */
        if (send_opcode_with_operand(handle, OPCODE_SET_WIN_OFFS,
                                      (uint16_t)win_offset, "SET_WIN_OFFS") != 0)
            return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    CAENComm_ErrorCode err;
    int handle;
    uint32_t usb_link;
    uint32_t vme_base;
    uint16_t status;
    int has_window = 0;
    int16_t win_width = 0;
    int16_t win_offset = 0;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <PID_V4718> <base_address_hex> [win_width] [win_offset]\n", argv[0]);
        fprintf(stderr, "Exemple: %s 64324 0x08000000 40 -10\n", argv[0]);
        return EXIT_FAILURE;
    }

    usb_link = (uint32_t)strtoul(argv[1], NULL, 10);
    vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

    if (argc >= 5) {
        has_window = 1;
        win_width  = (int16_t)atoi(argv[3]);
        win_offset = (int16_t)atoi(argv[4]);
    }

    printf("=== Configuration Trigger Matching Mode ===\n");
    printf("V4718 PID=%u, module a l'adresse 0x%08X\n\n", usb_link, vme_base);

    err = CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return EXIT_FAILURE;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    if (configure_trigger_matching(handle, has_window, win_width, win_offset) != 0) {
        fprintf(stderr, "\nEchec de la configuration.\n");
        CAENComm_CloseDevice(handle);
        return EXIT_FAILURE;
    }

    /* Verification : lecture du registre STATUS pour un controle visuel */
    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err == CAENComm_Success) {
        printf("\nSTATUS apres configuration = 0x%04X\n", status);
    }

    printf("\nTrigger Matching Mode configure avec succes.\n");
    if (has_window) {
        printf("Fenetre : largeur = %d x 25ns = %d ns, offset = %d x 25ns = %d ns\n",
               win_width, win_width * 25, win_offset, win_offset * 25);
    } else {
        printf("Fenetre laissee aux valeurs par defaut (largeur 500ns / offset -1us).\n");
    }

    CAENComm_CloseDevice(handle);
    printf("\nConnexion fermee proprement.\n");

    return EXIT_SUCCESS;
}