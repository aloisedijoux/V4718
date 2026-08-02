/*
 * read_output_buffer.c
 *
 * Lit et decode le contenu de l'Output Buffer d'un V1290N (via le pont
 * V4718 en USB), en mode Trigger Matching, D32 single-word readout
 * (voir manuel technique V1290, §4.6.1 et §6.2).
 *
 * Format des mots (32 bits), identifie par les bits [31:27] (voir §6.2,
 * Fig. 6.1 a 6.8 du manuel) :
 *   01000  Global Header    (bits [26:5]=Event count (22 bits), [4:0]=Geo)
 *   10000  Global Trailer   (bits [26:24]=status (TDC error/buffer overflow/
 *                            trigger lost), [18:5]=Word count, [4:0]=Geo)
 *   00001  TDC Header       (bits [25:24]=TDC number, [23:12]=Event ID, [11:0]=Bunch ID)
 *   00000  TDC Measurement  (bit [26]=leading/trailing, [25:21]=Channel (5 bits),
 *                            [20:0]=Measure (21 bits))
 *   00011  TDC Trailer      (bits [25:24]=TDC number, [23:12]=Event ID, [11:0]=Word count)
 *   00100  TDC Error        (bits [25:24]=TDC number, [14:0]=Error flags,
 *                            voir Fig. 6.5 pour la signification de chaque bit)
 *   11000  Filler
 *
 * NB : ce decodage suit la structure documentee au §6.2 du manuel;
 * seuls les champs les plus utiles sont extraits ici (type de mot,
 * canal, valeur de mesure, flags d'erreur). Le mot Global Trigger Time
 * Tag (10001) est affiche en brut (type "UNKNOWN") sans decodage
 * detaille des champs.
 *
 * NB sur le "nombre de mots en memoire" : le V1290 n'expose pas de
 * registre donnant directement un compte de MOTS restants dans
 * l'Output Buffer. Le registre EVENT STORED (Base + 0x1020, D16, §6.18)
 * donne le nombre d'EVENEMENTS actuellement stockes (chaque evenement
 * occupant un nombre variable de mots : header + N mesures + trailer).
 * C'est ce registre qui est lu et affiche ici avant chaque lecture.
 *
 * Compilation :
 *   gcc -o read_output_buffer read_output_buffer.c -lCAENComm
 *
 * Usage :
 *   ./read_output_buffer <PID_V4718> [base_address_hex] [max_words] [-c]
 *
 * Exemples :
 *   ./read_output_buffer 64324                  -> lit les 2 TDC connus (jusqu'a 64 mots chacun)
 *   ./read_output_buffer 64324 0x08000000 256   -> lit jusqu'a 256 mots sur le slot 2 seul
 *   ./read_output_buffer 64324 0x03000000 64 -c -> lecture en continu sur le slot 3
 *                                                   (Ctrl+C pour arreter proprement)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <CAENComm.h>

#include "v1290_decode.h"

#define REG_STATUS          0x1002
#define REG_EVENT_STORED    0x1020   /* nombre d'evenements dans l'Output Buffer (D16, RO, §6.18) */
#define REG_OUTPUT_BUFFER   0x0000   /* n'importe quelle adresse dans 0x0000-0x0FFC */

#define STATUS_DATA_READY_BIT (1 << 0)

#define DEFAULT_MAX_WORDS   64
#define POLL_INTERVAL_US    10000  /* 10 ms entre deux verifications DATA_READY en mode continu */

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

/* Lit le registre EVENT STORED (nombre d'EVENEMENTS actuellement dans
   l'Output Buffer, pas un compte de mots -- voir §6.18) et l'affiche. */
static int print_event_stored(int handle)
{
    CAENComm_ErrorCode err;
    uint16_t n_events;

    err = CAENComm_Read16(handle, REG_EVENT_STORED, &n_events);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec lecture EVENT STORED : code %d\n", err);
        return -1;
    }
    printf("EVENT STORED [0x1020] = %u evenement(s) actuellement dans l'Output Buffer\n",
           n_events);
    return 0;
}

/* Lit et decode jusqu'a max_words mots depuis l'Output Buffer, tant que
   DATA_READY reste actif. Retourne le nombre de mots lus, ou -1 en cas
   d'erreur VME. */
static int read_and_decode_available(int handle, int max_words)
{
    CAENComm_ErrorCode err;
    uint16_t status;
    int n_read = 0;

    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec lecture STATUS : code %d\n", err);
        return -1;
    }

    if (!(status & STATUS_DATA_READY_BIT))
        return 0;  /* rien a lire, pas une erreur */

    while (n_read < max_words) {
        uint32_t word;

        err = CAENComm_Read32(handle, REG_OUTPUT_BUFFER, &word);
        if (err != CAENComm_Success) {
            fprintf(stderr, "Echec lecture mot %d : code %d\n", n_read, err);
            return -1;
        }

        decode_and_print_word(n_read, word);
        n_read++;

        if (WORD_TYPE(word) == TYPE_GLOBAL_TRAILER) {
            printf("Global Trailer atteint : fin de l'evenement courant.\n");
            break;
        }

        /* Revérifie DATA_READY pour savoir si on doit continuer (evite
           de boucler indefiniment si le buffer est plus court que
           max_words). */
        err = CAENComm_Read16(handle, REG_STATUS, &status);
        if (err == CAENComm_Success && !(status & STATUS_DATA_READY_BIT))
            break;
    }

    return n_read;
}

static int process_module(uint32_t usb_link, uint32_t vme_base, int max_words)
{
    CAENComm_ErrorCode err;
    int handle;
    uint16_t status;
    int rc = 0;
    int n_read;

    printf("--------------------------------------------------\n");
    printf("Module a l'adresse 0x%08X\n", vme_base);
    printf("--------------------------------------------------\n");

    err = CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return -1;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec lecture STATUS : code %d\n", err);
        CAENComm_CloseDevice(handle);
        return -1;
    }
    printf("STATUS = 0x%04X (DATA_READY = %d)\n", status,
           (status & STATUS_DATA_READY_BIT) ? 1 : 0);
    print_event_stored(handle);
    printf("\n");

    if (!(status & STATUS_DATA_READY_BIT)) {
        printf("Aucun evenement disponible dans l'Output Buffer.\n");
        printf("(Envoyez d'abord un trigger avec software_trigger, ou verifiez\n");
        printf(" que des hits sont bien arrives dans la fenetre de matching.)\n");
        CAENComm_CloseDevice(handle);
        printf("\nConnexion fermee proprement.\n\n");
        return 0;  /* pas une erreur en soi, juste rien a lire */
    }

    printf("Lecture de l'Output Buffer (D32, jusqu'a %d mots)...\n\n", max_words);

    n_read = read_and_decode_available(handle, max_words);
    rc = (n_read < 0) ? -1 : 0;
    if (n_read < 0)
        n_read = 0;

    printf("\n%d mot(s) lu(s) au total.\n", n_read);

    CAENComm_CloseDevice(handle);
    printf("Connexion fermee proprement.\n\n");

    return rc;
}

/* Boucle de lecture en continu sur un seul module : poll DATA_READY,
   lit/decode chaque evenement des qu'il arrive, jusqu'a Ctrl+C. */
static int process_module_continuous(uint32_t usb_link, uint32_t vme_base, int max_words)
{
    CAENComm_ErrorCode err;
    int handle;
    long total_words = 0;
    long total_events = 0;

    printf("--------------------------------------------------\n");
    printf("Module a l'adresse 0x%08X -- lecture en continu (Ctrl+C pour arreter)\n", vme_base);
    printf("--------------------------------------------------\n");

    err = CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        return -1;
    }
    printf("Connexion etablie. Handle = %d\n\n", handle);

    signal(SIGINT, handle_sigint);

    while (!g_stop_requested) {
        uint16_t status;
        int n_read;

        err = CAENComm_Read16(handle, REG_STATUS, &status);
        if (err != CAENComm_Success) {
            fprintf(stderr, "Erreur lecture STATUS : code %d, arret de la boucle continue.\n", err);
            break;
        }
        if (!(status & STATUS_DATA_READY_BIT)) {
            usleep(POLL_INTERVAL_US);
            continue;
        }

        print_event_stored(handle);

        n_read = read_and_decode_available(handle, max_words);
        if (n_read < 0) {
            fprintf(stderr, "Erreur de lecture, arret de la boucle continue.\n");
            break;
        }

        total_words += n_read;
        total_events++;
        printf("\n");
    }

    printf("\nArret demande. %ld evenement(s), %ld mot(s) lu(s) au total.\n",
           total_events, total_words);

    CAENComm_CloseDevice(handle);
    printf("Connexion fermee proprement.\n\n");

    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t usb_link;
    int max_words = DEFAULT_MAX_WORDS;
    int overall_rc = 0;
    int continuous_mode = 0;
    char *positional[4];
    int n_positional = 0;
    int i;

    static const uint32_t default_bases[] = { 0x08000000, 0x03000000 };

    /* Separe le flag -c (mode continu) des arguments positionnels,
       quelle que soit sa place dans la ligne de commande. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            continuous_mode = 1;
        } else if (n_positional < 4) {
            positional[n_positional++] = argv[i];
        }
    }

    if (n_positional < 1) {
        fprintf(stderr, "Usage: %s <PID_V4718> [base_address_hex] [max_words] [-c]\n", argv[0]);
        fprintf(stderr, "Exemple (2 TDC par defaut)       : %s 64324\n", argv[0]);
        fprintf(stderr, "Exemple (1 TDC, 256 mots)        : %s 64324 0x08000000 256\n", argv[0]);
        fprintf(stderr, "Exemple (1 TDC, lecture continue) : %s 64324 0x03000000 64 -c\n", argv[0]);
        return EXIT_FAILURE;
    }

    usb_link = (uint32_t)strtoul(positional[0], NULL, 10);

    printf("=== Lecture de l'Output Buffer ===\n");
    printf("V4718 PID=%u\n\n", usb_link);

    if (continuous_mode) {
        if (n_positional < 2) {
            fprintf(stderr, "Le mode continu (-c) exige une adresse de base explicite.\n");
            fprintf(stderr, "Exemple : %s 64324 0x03000000 64 -c\n", argv[0]);
            return EXIT_FAILURE;
        }

        uint32_t vme_base = (uint32_t)strtoul(positional[1], NULL, 16);
        if (n_positional >= 3)
            max_words = atoi(positional[2]);

        overall_rc = process_module_continuous(usb_link, vme_base, max_words);
        return (overall_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (n_positional >= 2) {
        uint32_t vme_base = (uint32_t)strtoul(positional[1], NULL, 16);

        if (n_positional >= 3)
            max_words = atoi(positional[2]);

        overall_rc = process_module(usb_link, vme_base, max_words);
    } else {
        for (i = 0; i < 2; i++) {
            int rc = process_module(usb_link, default_bases[i], max_words);
            if (rc != 0)
                overall_rc = rc;
        }
    }

    if (overall_rc == 0) {
        printf("=== Lecture terminee sur tous les modules cibles. ===\n");
    } else {
        printf("=== Au moins un module a rencontre un probleme. Voir logs ci-dessus. ===\n");
    }

    return (overall_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}