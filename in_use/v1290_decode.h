/*
 * v1290_decode.h
 *
 * Decodage partage des mots (32 bits) de l'Output Buffer d'un V1290N,
 * conforme au manuel technique V1290 (UM Rev.16, §6.2, Fig. 6.1 a 6.8).
 * Verifie contre les figures reelles du manuel (pas seulement le texte
 * extrait) -- voir historique du depot pour le detail des corrections
 * (channel/measure et GEO/event_count etaient inverses/mal positionnes
 * dans une version anterieure).
 *
 * Format des mots, identifie par les bits [31:27] :
 *   01000  Global Header    (bits [26:5]=Event count (22 bits), [4:0]=Geo)
 *   10000  Global Trailer   (bits [26:24]=status, [18:5]=Word count, [4:0]=Geo)
 *   00001  TDC Header       (bits [25:24]=TDC number, [23:12]=Event ID, [11:0]=Bunch ID)
 *   00000  TDC Measurement  (bit [26]=leading/trailing, [25:21]=Channel (5 bits),
 *                            [20:0]=Measure (21 bits))
 *   00011  TDC Trailer      (bits [25:24]=TDC number, [23:12]=Event ID, [11:0]=Word count)
 *   00100  TDC Error        (bits [25:24]=TDC number, [14:0]=Error flags, voir Fig. 6.5)
 *   11000  Filler
 *
 * Le mot Global Trigger Time Tag (10001) est affiche en brut ("UNKNOWN").
 *
 * Utilise par read_output_buffer.c (D32 single-word polling) et
 * read_output_buffer_blt.c (BLT32 block transfer).
 */

#ifndef V1290_DECODE_H
#define V1290_DECODE_H

#include <stdint.h>
#include <stdio.h>

/* Type de mot identifie par les 5 bits de poids fort [31:27] */
#define WORD_TYPE(w)  (((w) >> 27) & 0x1F)

/* Resolution assumee pour convertir "mesure" en temps reel (picosecondes
   par LSB). Correspond a RES[1:0]=11 (25 ps, reglage par defaut du
   V1290N -- confirme via decode_status.c sur ce module). Si la
   resolution est changee (SET_DETECTION / channel resolution, voir §5.4
   du manuel), cette constante doit etre mise a jour en consequence :
   RES[1:0] : 00=800ps, 01=200ps, 10=100ps, 11=25ps (defaut). */
#define ASSUMED_RESOLUTION_PS  25

#define TYPE_GLOBAL_HEADER   0x08  /* 01000 */
#define TYPE_GLOBAL_TRAILER  0x10  /* 10000 */
#define TYPE_TDC_HEADER      0x01  /* 00001 */
#define TYPE_TDC_MEASUREMENT 0x00  /* 00000 (bit 26 distingue leading/trailing) */
#define TYPE_TDC_ERROR       0x04  /* 00100 */
#define TYPE_TDC_TRAILER     0x03  /* 00011 */
#define TYPE_FILLER          0x18  /* 11000 */

/* Fig. 6.5 du manuel : signification des bits [14:0] du mot TDC Error. */
static const char *const TDC_ERROR_FLAG_NAMES[] = {
    "Hit lost in group 0 from read-out FIFO overflow",
    "Hit lost in group 0 from L1 buffer overflow",
    "Hit error detected in group 0",
    "Hit lost in group 1 from read-out FIFO overflow",
    "Hit lost in group 1 from L1 buffer overflow",
    "Hit error detected in group 1",
    "Hit data lost in group 2 from read-out FIFO overflow",
    "Hit lost in group 2 from L1 buffer overflow",
    "Hit error detected in group 2",
    "Hit lost in group 3 from read-out FIFO overflow",
    "Hit lost in group 3 from L1 buffer overflow",
    "Hit error detected in group 3",
    "Hits rejected because of programmed event size limit",
    "Event lost (trigger FIFO overflow)",
    "Internal fatal chip error detected",
};
#define N_TDC_ERROR_FLAGS (sizeof(TDC_ERROR_FLAG_NAMES) / sizeof(TDC_ERROR_FLAG_NAMES[0]))

static inline const char *word_type_name(uint32_t w)
{
    uint32_t t = WORD_TYPE(w);
    switch (t) {
        case TYPE_GLOBAL_HEADER:   return "GLOBAL HEADER";
        case TYPE_GLOBAL_TRAILER:  return "GLOBAL TRAILER";
        case TYPE_TDC_HEADER:      return "TDC HEADER";
        case TYPE_TDC_ERROR:       return "TDC ERROR";
        case TYPE_TDC_TRAILER:     return "TDC TRAILER";
        case TYPE_FILLER:          return "FILLER";
        default:
            /* Les mots de mesure ont le bit 26 variable selon
               leading/trailing edge; on les identifie par elimination. */
            if ((t & 0x1E) == 0x00) return "TDC MEASUREMENT";
            return "UNKNOWN";
    }
}

static inline void decode_and_print_word(int index, uint32_t w)
{
    const char *type = word_type_name(w);

    printf("  [%3d] 0x%08X  %s", index, w, type);

    if (WORD_TYPE(w) == TYPE_GLOBAL_HEADER) {
        /* Fig. 6.1 : EVENT COUNT = bits [26:5] (22 bits), GEO = bits [4:0] (5 bits) */
        uint32_t event_count = (w >> 5) & 0x3FFFFF;
        uint32_t geo = w & 0x1F;
        printf("  (GEO=%u, event_count=%u)", geo, event_count);
    } else if (WORD_TYPE(w) == TYPE_TDC_HEADER) {
        uint32_t tdc_num  = (w >> 24) & 0x3;
        uint32_t event_id = (w >> 12) & 0xFFF;
        uint32_t bunch_id = w & 0xFFF;
        printf("  (TDC=%u, event_id=%u, bunch_id=%u)", tdc_num, event_id, bunch_id);
    } else if (WORD_TYPE(w) == TYPE_TDC_ERROR) {
        /* Fig. 6.5 : TDC = bits [25:24], ERROR FLAGS = bits [14:0] */
        uint32_t tdc_num = (w >> 24) & 0x3;
        uint32_t flags = w & 0x7FFF;
        unsigned i;
        printf("  (TDC=%u, error_flags=0x%04X)", tdc_num, flags);
        for (i = 0; i < N_TDC_ERROR_FLAGS; i++) {
            if (flags & (1u << i))
                printf("\n        [%u] %s", i, TDC_ERROR_FLAG_NAMES[i]);
        }
    } else if ((WORD_TYPE(w) & 0x1E) == 0x00 && WORD_TYPE(w) != TYPE_TDC_HEADER) {
        /* TDC Measurement (manuel Fig. 6.3) : bit 26 = 1 -> trailing edge,
           0 -> leading edge ; CHANNEL = bits [25:21] (5 bits) ;
           MEASUREMENT = bits [20:0] (21 bits). */
        uint32_t edge_type = (w >> 26) & 0x1;
        uint32_t channel   = (w >> 21) & 0x1F;
        uint32_t measure   = w & 0x1FFFFF;
        double measure_ns  = (double)measure * ASSUMED_RESOLUTION_PS / 1000.0;
        printf("  (%s, channel=%u, mesure=%u [x %dps] = %.3f ns"
               " depuis debut fenetre si SUB_TRG actif, sinon horloge libre)",
               edge_type ? "trailing" : "leading", channel, measure,
               ASSUMED_RESOLUTION_PS, measure_ns);
    } else if (WORD_TYPE(w) == TYPE_TDC_TRAILER) {
        uint32_t tdc_num   = (w >> 24) & 0x3;
        uint32_t event_id  = (w >> 12) & 0xFFF;
        uint32_t word_cnt  = w & 0xFFF;
        printf("  (TDC=%u, event_id=%u, word_count=%u)", tdc_num, event_id, word_cnt);
    } else if (WORD_TYPE(w) == TYPE_GLOBAL_TRAILER) {
        uint32_t word_cnt = (w >> 5) & 0x3FFF;
        uint32_t status_bits = (w >> 24) & 0x7;
        printf("  (word_count=%u, status_bits=0x%X)", word_cnt, status_bits);
    }

    printf("\n");
}

#endif /* V1290_DECODE_H */
