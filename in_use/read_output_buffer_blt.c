/*
 * read_output_buffer_blt.c
 *
 * Lit l'Output Buffer d'un V1290N (via le pont V4718 en USB) en BLT32
 * (Block Transfer), au lieu du D32 mot-a-mot de read_output_buffer.c.
 * Necessaire pour suivre un taux de trigger eleve (ex: trigger externe
 * a plusieurs MHz) : chaque acces D32 est un aller-retour USB complet
 * (quelques centaines a quelques milliers de mots/s au mieux), alors
 * qu'un BLT32 rapatrie plusieurs milliers de mots par appel.
 *
 * ATTENTION DEBIT (a lire avant de pousser le taux de trigger) :
 * meme en BLT32, un evenement VIDE (aucun hit dans la fenetre) pese deja
 * Global Header + Global Trailer + (TDC Header + TDC Trailer) x nb TDC
 * actifs, soit 6 mots = 24 octets pour 2 TDC. A 10 MHz de trigger, ca
 * fait 240 Mo/s de pur overhead de structure, DEJA au-dessus des 120
 * Mo/s annonces pour le 2eSST (le plus rapide dispo sur le V4718) --
 * le BLT32 utilise ici est plus lent que ca. Pour reduire l'overhead :
 * desactiver HEADER_EN (TDC Header/Trailer, bit 4 du registre CONTROL,
 * voir §6.3) coupe le poids d'un evenement vide a peu pres en deux.
 * Sans quoi, a haut taux, une perte de donnees (TDC Error, FULL,
 * TRIGGER_LOST) est attendue et n'est pas forcement le signe d'un bug.
 *
 * Protocole BLT32 utilise (manuel V1290 UM Rev.16, §4.6.3) : le bit
 * BERREN (Control Register, Base+0x1000, bit 0, voir §6.3) est active
 * au demarrage. Avec BERREN=1, le module termine chaque cycle BLT par
 * un Bus Error (BERR) des que le buffer est vide -- c'est le mecanisme
 * NORMAL de fin de bloc, pas une erreur. CAENComm_BLTRead renvoie alors
 * CAENComm_VMEBusError (ou, constate sur ce V4718 -- pont USB-VME plutot
 * que controleur VME natif -- CAENComm_Terminated) avec le nombre de
 * mots reellement transferes dans nw ; les deux sont traites comme une
 * fin de bloc valide, pas un echec.
 *
 * Compilation :
 *   gcc -o read_output_buffer_blt read_output_buffer_blt.c -lCAENComm
 *
 * Usage :
 *   ./read_output_buffer_blt <PID_V4718> <base_address_hex> [blt_words] [-c] [-v] [-s offset_ns]
 *
 *   blt_words : taille de chaque appel BLT32, en mots de 32 bits. Defaut : 4096
 *   -c        : lecture en continu (Ctrl+C pour arreter), affiche des
 *               stats de debit (mots/s, evenements/s) toutes les ~1s
 *               au lieu de decoder chaque mot (bien trop volumineux a
 *               haut taux).
 *   -v        : decode et affiche aussi chaque mot individuellement (en
 *               plus des stats en mode -c). A n'utiliser qu'a taux
 *               faible/modere pour verification -- inutilisable a
 *               plusieurs MHz (le terminal ne suivra pas).
 *   -s offset_ns : mode "stream" pour plot en temps reel. stdout ne
 *               contient PLUS QUE, pour chaque hit, une ligne CSV
 *               "numero_trigger,channel,delai_ns" (numero_trigger =
 *               event_count du GLOBAL HEADER de cet evenement --
 *               litteralement "trigger #i" ; channel = numero de
 *               channel (0-15) de ce hit ; delai_ns = offset_ns +
 *               mesure convertie en ns, necessite SUB_TRG actif, voir
 *               enable_trigger_subtraction.c). Pense a etre pipe vers
 *               un script de trace (voir live_histogram.py, qui ne
 *               garde que le dernier champ). Tous les messages
 *               informatifs passent sur stderr dans ce mode.
 *   -b fichier : enregistre EN PLUS, en parallele de tout le reste (-c,
 *               -v, -s), les mots bruts (32 bits, tels que lus en
 *               BLT32) dans un fichier binaire -- format le plus
 *               compact possible (4 octets/mot, aucun overhead texte),
 *               relisable ensuite avec decode_binary.py pour une
 *               analyse a posteriori sans avoir perdu aucune donnee
 *               (contrairement au CSV du mode stream qui ne garde que
 *               les TDC MEASUREMENT).
 *
 * Exemples :
 *   ./read_output_buffer_blt 64324 0x03000000
 *       -> une seule passe BLT32, decode et affiche tout ce qui est present
 *
 *   ./read_output_buffer_blt 64324 0x03000000 8192 -c
 *       -> lecture continue par blocs de 8192 mots, stats de debit seulement
 *
 *   ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -1000 | python3 live_histogram.py
 *       -> histogramme en temps reel du delai hit<->trigger (offset -1000 ns)
 *
 *   ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -1000 -b run001.bin | python3 live_histogram.py
 *       -> pareil, plus un enregistrement binaire complet en parallele
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <CAENComm.h>
#include <time.h>

#include "v1290_decode.h"

#define REG_CONTROL         0x1000
#define REG_STATUS          0x1002
#define REG_EVENT_STORED    0x1020
#define REG_OUTPUT_BUFFER   0x0000

#define CONTROL_BERREN_BIT  (1 << 0)
#define STATUS_DATA_READY_BIT (1 << 0)
#define STATUS_FULL_BIT        (1 << 2)

#define DEFAULT_BLT_WORDS   4096
#define STATS_INTERVAL_SEC  1.0
#define IDLE_SLEEP_US       1000  /* pause courte si un appel BLT ne ramene rien */

static volatile sig_atomic_t g_stop_requested = 0;
static int g_stream_mode = 0;          /* si actif, stdout = flux de delais purs */
static double g_stream_offset_ns = 0.0;
static long g_current_event = -1;      /* event_count du dernier GLOBAL HEADER vu (numero de trigger) */
static FILE *g_binary_file = NULL;     /* si non-NULL, dump binaire brut des mots BLT en parallele */

static void handle_sigint(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

/* Messages informatifs : stdout normalement, stderr en mode stream (pour
   laisser stdout propre a piper vers un script de trace). */
static FILE *info_stream(void)
{
    return g_stream_mode ? stderr : stdout;
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

/* Active BERREN (Control Register, bit 0) par lecture-modification-ecriture,
   sans toucher aux autres bits (HEADER_EN, ALIGN64, etc.). */
static int enable_berr(int handle)
{
    CAENComm_ErrorCode err;
    uint16_t ctrl;

    err = CAENComm_Read16(handle, REG_CONTROL, &ctrl);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec lecture CONTROL : code %d\n", err);
        return -1;
    }

    if (ctrl & CONTROL_BERREN_BIT) {
        fprintf(info_stream(), "BERREN deja actif (CONTROL=0x%04X).\n", ctrl);
        return 0;
    }

    ctrl |= CONTROL_BERREN_BIT;
    err = CAENComm_Write16(handle, REG_CONTROL, ctrl);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ecriture CONTROL (BERREN) : code %d\n", err);
        return -1;
    }

    fprintf(info_stream(), "BERREN active (CONTROL=0x%04X).\n", ctrl);
    return 0;
}

/* Un appel BLT32. Avec BERREN=1, le bloc se termine normalement soit par
   CAENComm_VMEBusError (BERR VME "brut", vu par un controleur VME
   natif), soit -- constate sur ce V4718, un pont USB-VME -- par
   CAENComm_Terminated ("Communication terminated by the Device") : le
   pont semble traduire le BERR recu du bus VME externe en ce code
   plutot qu'en VMEBusError. Les deux sont traites comme une fin de
   bloc normale, pas une erreur. Retourne 0 si l'appel s'est deroule
   normalement, -1 pour toute autre erreur. *nw recoit le nombre de
   mots reellement lus. */
static int blt_read_once(int handle, uint32_t *buf, int blt_words, int *nw)
{
    CAENComm_ErrorCode err;

    err = CAENComm_BLTRead(handle, REG_OUTPUT_BUFFER, buf, blt_words, nw);
    if (err != CAENComm_Success && err != CAENComm_VMEBusError &&
        err != CAENComm_Terminated) {
        fprintf(stderr, "Echec BLT32 read : code %d\n", err);
        return -1;
    }

    return 0;
}

/* Decode chaque mot du bloc lu et compte les evenements (Global Trailer)
   et les mots TDC Error avec perte de donnees (pour un indicateur rapide
   de sante en mode continu). En mode stream, imprime sur stdout, pour
   chaque TDC MEASUREMENT : le numero du trigger (event_count du GLOBAL
   HEADER courant) suivi du delai (ns) par rapport au trigger, en CSV
   "event,delai_ns" -- un flottant flush immediat par ligne (pipe temps
   reel). g_current_event est mis a jour a chaque GLOBAL HEADER rencontre
   et persiste entre deux appels (utile quand un bloc BLT coupe un
   evenement en deux). */
static void account_block(const uint32_t *buf, int n, int verbose,
                           long *events, long *error_words)
{
    int i;

    if (g_binary_file)
        fwrite(buf, sizeof(uint32_t), (size_t)n, g_binary_file);

    for (i = 0; i < n; i++) {
        if (verbose)
            decode_and_print_word(i, buf[i]);

        if (WORD_TYPE(buf[i]) == TYPE_GLOBAL_HEADER)
            g_current_event = (buf[i] >> 5) & 0x3FFFFF;  /* Fig. 6.1 : EVENT COUNT */
        if (WORD_TYPE(buf[i]) == TYPE_GLOBAL_TRAILER)
            (*events)++;
        if (WORD_TYPE(buf[i]) == TYPE_TDC_ERROR)
            (*error_words)++;

        if (g_stream_mode && WORD_TYPE(buf[i]) == TYPE_TDC_MEASUREMENT) {
            /* TDC MEASUREMENT (Fig. 6.3) : channel = bits[25:21], mesure = bits[20:0] */
            uint32_t channel = (buf[i] >> 21) & 0x1F;
            uint32_t measure = buf[i] & 0x1FFFFF;
            double measure_ns = (double)measure * ASSUMED_RESOLUTION_PS / 1000.0;
            printf("%ld,%u,%.3f\n", g_current_event, channel, g_stream_offset_ns + measure_ns);
        }
    }
    if (g_stream_mode)
        fflush(stdout);
    if (g_binary_file)
        fflush(g_binary_file);
}

static int run_oneshot(int handle, int blt_words, int verbose)
{
    uint32_t *buf;
    long total_words = 0, total_events = 0, total_errors = 0;
    int n_calls = 0;

    buf = malloc((size_t)blt_words * sizeof(uint32_t));
    if (!buf) {
        fprintf(stderr, "Echec allocation buffer (%d mots)\n", blt_words);
        return -1;
    }

    for (;;) {
        int nw = 0;

        if (blt_read_once(handle, buf, blt_words, &nw) != 0) {
            free(buf);
            return -1;
        }
        n_calls++;

        if (nw > 0)
            account_block(buf, nw, verbose, &total_events, &total_errors);

        total_words += nw;

        /* nw < blt_words (ou 0) : le bloc s'est termine par BERR avant
           d'avoir rempli la taille demandee -> plus rien a lire. */
        if (nw < blt_words)
            break;
    }

    free(buf);

    fprintf(info_stream(), "\n%d appel(s) BLT32, %ld mot(s), %ld evenement(s), "
           "%ld mot(s) TDC Error.\n",
           n_calls, total_words, total_events, total_errors);

    return 0;
}

static int run_continuous(int handle, int blt_words, int verbose)
{
    uint32_t *buf;
    long total_words = 0, total_events = 0, total_errors = 0;
    long interval_words = 0;
    double t_start, t_last_report;

    buf = malloc((size_t)blt_words * sizeof(uint32_t));
    if (!buf) {
        fprintf(stderr, "Echec allocation buffer (%d mots)\n", blt_words);
        return -1;
    }

    signal(SIGINT, handle_sigint);

    fprintf(info_stream(), "Lecture continue en BLT32 (blocs de %d mots). Ctrl+C pour arreter.\n\n",
           blt_words);

    t_start = t_last_report = now_seconds();

    while (!g_stop_requested) {
        int nw = 0;
        double t_now;

        if (blt_read_once(handle, buf, blt_words, &nw) != 0) {
            free(buf);
            return -1;
        }

        if (nw > 0) {
            account_block(buf, nw, verbose, &total_events, &total_errors);
            total_words += nw;
            interval_words += nw;
        } else {
            usleep(IDLE_SLEEP_US);
        }

        t_now = now_seconds();
        if (t_now - t_last_report >= STATS_INTERVAL_SEC) {
            double dt = t_now - t_last_report;
            fprintf(info_stream(), "[t=%6.1fs] %8ld mots/s  %8ld evt cumules  "
                   "%6ld mots TDC Error cumules  (total mots=%ld)\n",
                   t_now - t_start,
                   (long)(interval_words / dt),
                   total_events,
                   total_errors,
                   total_words);
            interval_words = 0;
            t_last_report = t_now;
        }
    }

    free(buf);

    fprintf(info_stream(), "\nArret demande. %ld mot(s), %ld evenement(s), "
           "%ld mot(s) TDC Error au total (%.1fs).\n",
           total_words, total_events, total_errors, now_seconds() - t_start);

    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t usb_link, vme_base;
    int blt_words = DEFAULT_BLT_WORDS;
    int continuous_mode = 0;
    int verbose = 0;
    char *binary_path = NULL;
    char *positional[3];
    int n_positional = 0;
    int i, handle, rc;
    CAENComm_ErrorCode err;
    uint16_t status;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            continuous_mode = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-s exige un argument offset_ns\n");
                return EXIT_FAILURE;
            }
            g_stream_mode = 1;
            g_stream_offset_ns = atof(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-b exige un argument fichier\n");
                return EXIT_FAILURE;
            }
            binary_path = argv[++i];
        } else if (n_positional < 3) {
            positional[n_positional++] = argv[i];
        }
    }

    if (n_positional < 2) {
        fprintf(stderr, "Usage: %s <PID_V4718> <base_address_hex> [blt_words] [-c] [-v] [-s offset_ns] [-b fichier]\n", argv[0]);
        fprintf(stderr, "Exemple (1 passe)        : %s 64324 0x03000000\n", argv[0]);
        fprintf(stderr, "Exemple (continu, stats) : %s 64324 0x03000000 8192 -c\n", argv[0]);
        fprintf(stderr, "Exemple (stream -> plot) : %s 64324 0x03000000 4096 -c -s -1000 | python3 live_histogram.py\n", argv[0]);
        fprintf(stderr, "Exemple (+ binaire)      : %s 64324 0x03000000 4096 -c -s -1000 -b run.bin | python3 live_histogram.py\n", argv[0]);
        return EXIT_FAILURE;
    }

    usb_link = (uint32_t)strtoul(positional[0], NULL, 10);
    vme_base = (uint32_t)strtoul(positional[1], NULL, 16);
    if (n_positional >= 3)
        blt_words = atoi(positional[2]);

    if (blt_words <= 0) {
        fprintf(stderr, "blt_words invalide : %d\n", blt_words);
        return EXIT_FAILURE;
    }

    fprintf(info_stream(), "=== Lecture BLT32 de l'Output Buffer ===\n");
    fprintf(info_stream(), "V4718 PID=%u, module 0x%08X, blocs de %d mots\n\n",
           usb_link, vme_base, blt_words);
    if (g_stream_mode)
        fprintf(info_stream(), "Mode stream actif (offset=%.3f ns) -- stdout = delais purs.\n\n",
               g_stream_offset_ns);

    if (binary_path) {
        g_binary_file = fopen(binary_path, "wb");
        if (!g_binary_file) {
            fprintf(stderr, "Echec ouverture fichier binaire '%s'\n", binary_path);
            return EXIT_FAILURE;
        }
        fprintf(info_stream(), "Enregistrement binaire actif -> %s (mots 32 bits bruts)\n\n",
               binary_path);
    }

    err = CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, &handle);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ouverture : code %d\n", err);
        if (g_binary_file)
            fclose(g_binary_file);
        return EXIT_FAILURE;
    }
    fprintf(info_stream(), "Connexion etablie. Handle = %d\n\n", handle);

    if (enable_berr(handle) != 0) {
        CAENComm_CloseDevice(handle);
        if (g_binary_file)
            fclose(g_binary_file);
        return EXIT_FAILURE;
    }

    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err == CAENComm_Success) {
        fprintf(info_stream(), "STATUS = 0x%04X (DATA_READY=%d, FULL=%d)\n\n", status,
               (status & STATUS_DATA_READY_BIT) ? 1 : 0,
               (status & STATUS_FULL_BIT) ? 1 : 0);
    }

    if (continuous_mode)
        rc = run_continuous(handle, blt_words, verbose);
    else
        rc = run_oneshot(handle, blt_words, verbose);

    CAENComm_CloseDevice(handle);
    if (g_binary_file) {
        fclose(g_binary_file);
        fprintf(info_stream(), "Fichier binaire ferme (%s).\n", binary_path);
    }
    fprintf(info_stream(), "Connexion fermee proprement.\n");

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
