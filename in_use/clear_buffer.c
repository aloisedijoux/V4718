/*
 * clear_buffer.c
 *
 * Ecrit dans le registre SOFTWARE CLEAR d'un ou deux V1290N (via le
 * pont V4718 en USB) pour vider l'Output Buffer accumule (utile apres
 * avoir arrete une source de hits trop rapide -- voir stop_pulser.c --
 * qui a laisse le buffer plein/en retard).
 *
 * Registre utilise (manuel technique V1290 UM Rev.16, §6.14) :
 *   Base + 0x1016  SOFTWARE CLEAR (write only, D16). Un acces en
 *   ecriture (valeur indifferente) provoque :
 *     - le clear des TDCs ;
 *     - le clear de l'Output Buffer ;
 *     - la remise a 0 du compteur d'evenements ;
 *     - un TDC Global Reset.
 *
 * Par defaut (sans adresse en argument), vide les deux V1290N connus
 * (slot 2 @ 0x08000000 et slot 3 @ 0x03000000). Une adresse unique peut
 * aussi etre ciblee.
 *
 * Compilation :
 *   gcc -o clear_buffer clear_buffer.c -lCAENComm
 *
 * Usage :
 *   ./clear_buffer <PID_V4718> [base_address_hex]
 *
 * Exemples :
 *   ./clear_buffer 64324                 -> vide les 2 TDC connus
 *   ./clear_buffer 64324 0x03000000      -> vide seulement le slot 3
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CAENComm.h>

#define REG_SOFTWARE_CLEAR  0x1016
#define REG_STATUS          0x1002
#define REG_EVENT_STORED    0x1020

#define STATUS_DATA_READY_BIT (1 << 0)
#define STATUS_FULL_BIT        (1 << 2)

static int process_module(uint32_t usb_link, uint32_t vme_base)
{
    CAENComm_ErrorCode err;
    int handle;
    uint16_t status;
    uint16_t n_events;
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

    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err == CAENComm_Success) {
        printf("STATUS avant clear = 0x%04X (DATA_READY=%d, FULL=%d)\n",
               status, (status & STATUS_DATA_READY_BIT) ? 1 : 0,
               (status & STATUS_FULL_BIT) ? 1 : 0);
    }

    printf("Envoi SOFTWARE CLEAR (0x%04X)...\n", REG_SOFTWARE_CLEAR);
    err = CAENComm_Write16(handle, REG_SOFTWARE_CLEAR, 0x0001);
    if (err != CAENComm_Success) {
        fprintf(stderr, "Echec ecriture SOFTWARE CLEAR : code %d\n", err);
        CAENComm_CloseDevice(handle);
        return -1;
    }

    err = CAENComm_Read16(handle, REG_STATUS, &status);
    if (err == CAENComm_Success) {
        printf("STATUS apres clear  = 0x%04X (DATA_READY=%d, FULL=%d)\n",
               status, (status & STATUS_DATA_READY_BIT) ? 1 : 0,
               (status & STATUS_FULL_BIT) ? 1 : 0);
    } else {
        fprintf(stderr, "Echec relecture STATUS : code %d\n", err);
        rc = -1;
    }

    err = CAENComm_Read16(handle, REG_EVENT_STORED, &n_events);
    if (err == CAENComm_Success) {
        printf("EVENT STORED apres clear = %u\n", n_events);
    }

    if (status & (STATUS_DATA_READY_BIT | STATUS_FULL_BIT)) {
        fprintf(stderr,
            "\nATTENTION : DATA_READY et/ou FULL toujours actif juste apres le clear.\n"
            "Si une source de hits rapide (pulser, bruit) est TOUJOURS active, le\n"
            "buffer se re-remplit immediatement -- arretez la source avant de\n"
            "relire ce registre (voir stop_pulser.c).\n");
    } else {
        printf("\nBuffer vide et confirme (DATA_READY=0, FULL=0).\n");
    }

    CAENComm_CloseDevice(handle);
    printf("Connexion fermee proprement.\n\n");

    return rc;
}

int main(int argc, char *argv[])
{
    uint32_t usb_link;
    int overall_rc = 0;

    static const uint32_t default_bases[] = { 0x08000000, 0x03000000 };

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <PID_V4718> [base_address_hex]\n", argv[0]);
        fprintf(stderr, "Exemple (2 TDC par defaut) : %s 64324\n", argv[0]);
        fprintf(stderr, "Exemple (1 TDC cible)       : %s 64324 0x03000000\n", argv[0]);
        return EXIT_FAILURE;
    }

    usb_link = (uint32_t)strtoul(argv[1], NULL, 10);

    printf("=== Software Clear de l'Output Buffer ===\n");
    printf("V4718 PID=%u\n\n", usb_link);

    if (argc >= 3) {
        uint32_t vme_base = (uint32_t)strtoul(argv[2], NULL, 16);
        overall_rc = process_module(usb_link, vme_base);
    } else {
        int i;
        for (i = 0; i < 2; i++) {
            int rc = process_module(usb_link, default_bases[i]);
            if (rc != 0)
                overall_rc = rc;
        }
    }

    if (overall_rc == 0) {
        printf("=== Clear termine sur tous les modules cibles. ===\n");
    } else {
        printf("=== Au moins un module a rencontre un probleme. Voir logs ci-dessus. ===\n");
    }

    return (overall_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
