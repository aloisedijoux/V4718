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
 *   0x0200  READ_ACQ_MOD     : relit le mode d'acquisition (1 mot, LSB: 1=TrgMatch, 0=ContStor)
 *   0x1600  READ_TRG_CONF    : relit 5 mots (largeur, offset, marge recherche,
 *                              marge rejet, flag soustraction trigger time)
 *
 * Le programme configure et verifie automatiquement les DEUX V1290N
 * connus (slot 2 @ 0x08000000 et slot 3 @ 0x03000000) si aucune adresse
 * n'est passee en argument. Une adresse unique peut aussi etre ciblee.
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
#define OPCODE_READ_ACQ_MOD    0x0200
#define OPCODE_READ_TRG_CONF   0x1600

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

/* Lit le mode d'acquisition actuellement actif et l'affiche. */
static int verify_acquisition_mode(int handle)
{
    uint16_t reply;

    printf("Verification : envoi READ_ACQ_MOD (0x%04X)...\n", OPCODE_READ_ACQ_MOD);
    if (micro_write(handle, OPCODE_READ_ACQ_MOD) != 0)
        return -1;

    if (micro_read(handle, &reply) != 0)
        return -1;

    printf("  Mode d'acquisition lu = 0x%04X -> %s\n", reply,
           (reply & 0x1) ? "TRIGGER MATCHING" : "CONTINUOUS STORAGE");

    return (reply & 0x1) ? 0 : -1;  /* -1 si le mode n'est pas Trigger Matching */
}

/* Relit les 5 mots de configuration trigger (largeur, offset, marge de
   recherche, marge de rejet, flag de soustraction du temps trigger) et
   les affiche, pour confirmer que la configuration a bien ete prise en
   compte par le microcontroleur (et pas seulement acceptee en ecriture). */
static int verify_trigger_config(int handle)
{
    uint16_t words[5];
    int i;

    printf("Verification : envoi READ_TRG_CONF (0x%04X)...\n", OPCODE_READ_TRG_CONF);
    if (micro_write(handle, OPCODE_READ_TRG_CONF) != 0)
        return -1;

    for (i = 0; i < 5; i++) {
        if (micro_read(handle, &words[i]) != 0) {
            fprintf(stderr, "  Echec lecture mot %d/5 de READ_TRG_CONF\n", i + 1);
            return -1;
        }
    }

    /* Extension de signe sur 12 bits pour largeur/offset/marges (bits [11:0]) */
    int16_t width  = (int16_t)(words[0] << 4) >> 4;
    int16_t offset = (int16_t)(words[1] << 4) >> 4;
    int16_t margin = (int16_t)(words[2] << 4) >> 4;
    int16_t reject = (int16_t)(words[3] << 4) >> 4;

    printf("  Largeur fenetre     = %d (x25ns = %d ns)\n", width, width * 25);
    printf("  Offset fenetre      = %d (x25ns = %d ns)\n", offset, offset * 25);
    printf("  Marge recherche     = %d (x25ns = %d ns)\n", margin, margin * 25);
    printf("  Marge rejet         = %d (x25ns = %d ns)\n", reject, reject * 25);
    printf("  Soustraction trigger= %s\n",
           (words[4] & 0x1) ? "activee" : "desactivee");

    return 0;
}


static int configure_trigger_matching(int handle, int has_window,
                                       int16_t win_width, int16_t win_offset)
{
    /* 1. Passage en Trigger Matching Mode.
          NB: cet opcode reprogramme les registres "setup" du TDC, ce qui
          implique un clear des donnees (comportement normal, voir 5.2.1). */
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

/* Configure ET verifie un module V1290N a l'adresse donnee. Retourne 0
   si tout s'est bien passe (config + verification), -1 sinon. */
static int process_module(uint32_t usb_link, uint32_t vme_base,
                           int has_window, int16_t win_width, int16_t win_offset)
{
    CAENComm_ErrorCode err;
    int handle;
    uint16_t status;
    int rc = 0;

    printf("--------------------------------------------------\n");
    printf("Module a l'adresse 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return -1;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    if (configure_trigger_matching(handle, has_window, win_width, win_offset) != 0) {
        fprintf(stderr, "\nEchec de la configuration.\n");
        CAENComm_CloseDevice(handle);
        return -1;
    }

    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err == CAENComm_Success) {
        printf("\nSTATUS apres configuration = 0x%04X\n", status);
    }

    printf("\n");
    if (verify_acquisition_mode(handle) != 0) {
        fprintf(stderr, "ATTENTION : le mode Trigger Matching n'est pas confirme actif !\n");
        rc = -1;
    }

    printf("\n");
    if (has_window) {
        if (verify_trigger_config(handle) != 0) {
            fprintf(stderr, "ATTENTION : impossible de relire la configuration trigger.\n");
            rc = -1;
        }
    }

    if (rc == 0) {
        printf("\nModule 0x%08X : configuration confirmee avec succes.\n", vme_base);
    }

    CAENComm_CloseDevice(handle);
    printf("Connexion fermee proprement.\n\n");

    return rc;
}

int main(int argc, char *argv[])
{
    uint32_t usb_link;
    int has_window = 0;
    int16_t win_width = 0;
    int16_t win_offset = 0;
    int overall_rc = 0;

    /* Adresses par defaut des deux V1290N connus (slot 2 et slot 3) */
    static const uint32_t default_bases[] = { 0x08000000, 0x03000000 };

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <PID_V4718> [base_address_hex] [win_width] [win_offset]\n", argv[0]);
        fprintf(stderr, "Exemple (2 TDC par defaut) : %s 64324\n", argv[0]);
        fprintf(stderr, "Exemple (1 TDC cible)       : %s 64324 0x08000000 40 -10\n", argv[0]);
        return EXIT_FAILURE;
    }

    usb_link = (uint32_t)strtoul(argv[1], NULL, 10);

    printf("=== Configuration Trigger Matching Mode ===\n");
    printf("V4718 PID=%u\n\n", usb_link);

    if (argc >= 3) {
        /* Adresse unique ciblee explicitement */
        uint32_t vme_base = (uint32_t)strtoul(argv[2], NULL, 16);

        if (argc >= 5) {
            has_window = 1;
            win_width  = (int16_t)atoi(argv[3]);
            win_offset = (int16_t)atoi(argv[4]);
        }

        overall_rc = process_module(usb_link, vme_base, has_window, win_width, win_offset);
    } else {
        /* Pas d'adresse fournie : on traite les deux TDC connus, avec les
           reglages de fenetre par defaut du module. */
        int i;
        for (i = 0; i < 2; i++) {
            int rc = process_module(usb_link, default_bases[i], has_window, win_width, win_offset);
            if (rc != 0)
                overall_rc = rc;
        }
    }

    if (overall_rc == 0) {
        printf("=== Tous les modules ont ete configures et verifies avec succes. ===\n");
    } else {
        printf("=== Au moins un module a rencontre un probleme. Voir logs ci-dessus. ===\n");
    }

    return (overall_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}