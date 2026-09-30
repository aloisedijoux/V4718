#ifndef CAEN_OPEN_H
#define CAEN_OPEN_H
/*
 * caen_open.h -- shared helper to open a V4718, over USB or Ethernet,
 * from the same command-line argument the tools in this folder already
 * take as argv[1] ("PID_or_IP").
 *
 * Historically every tool here hardcoded CAENComm_USB_V4718 and parsed
 * argv[1] as a numeric USB PID. Since the V4718 also has a Gigabit
 * Ethernet port (CAENComm_ETH_V4718, where CAENComm_OpenDevice2() takes
 * the IP address/hostname as a string instead of a PID -- see
 * CAENComm.h), this helper auto-detects which one to use:
 *
 *   - conn_arg is all digits  -> USB, treated as PID   (e.g. "64324")
 *   - anything else           -> Ethernet, treated as an IP/hostname
 *                                (e.g. "192.168.1.254")
 *
 * so existing scripts/usages with a numeric PID keep working unchanged.
 */
#include <CAENComm.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int caen_conn_is_usb_pid(const char *conn_arg)
{
    size_t i, len = strlen(conn_arg);
    if (len == 0) {
        return 1; /* fall back to USB/PID parsing (will just be PID 0) */
    }
    for (i = 0; i < len; i++) {
        if (!isdigit((unsigned char)conn_arg[i])) {
            return 0;
        }
    }
    return 1;
}

static inline CAENComm_ErrorCode caen_open_v4718(const char *conn_arg,
                                                   uint32_t vme_base,
                                                   int *handle)
{
    if (caen_conn_is_usb_pid(conn_arg)) {
        uint32_t usb_link = (uint32_t)strtoul(conn_arg, NULL, 10);
        return CAENComm_OpenDevice2(CAENComm_USB_V4718, &usb_link, 0, vme_base, handle);
    }
    return CAENComm_OpenDevice2(CAENComm_ETH_V4718, conn_arg, 0, vme_base, handle);
}

/*
 * caen_v1290_is_model_a() -- tell a V1290A apart from a V1290N.
 *
 * Reads the "vers" byte of the Configuration ROM (Base + 0x4030, D16,
 * read-only -- a plain VME register, NOT the microcontroller/MICRO
 * protocol) : manual §6.1.1 / Table 6.2 documents 0x00 for the V1290A,
 * 0x02 for the V1290N.
 *
 * This matters for more than just channel count: several microcontroller
 * opcodes respond with a DIFFERENT NUMBER OF WORDS depending on the model
 * -- e.g. READ_EN_PATTERN (0x45xx, manual §5.6.6) returns ONE 16-bit word
 * on a V1290N but TWO on a V1290A. Reading the wrong number of words
 * leaves the microcontroller "in a wait status" (manual's own words) for
 * the rest, which desyncs the handshake for every command after it --
 * this was the root cause of the intermittent WRITE_OK/READ_OK timeouts
 * seen when driving a V1290A with tools written only against the word
 * counts documented for the N.
 *
 * Returns 1 for a V1290A, 0 for a V1290N, -1 if the ROM register could
 * not be read (caller should treat that as a hard error rather than
 * guess, since guessing wrong here is exactly what causes the lockup).
 */
static inline int caen_v1290_is_model_a(int handle)
{
    uint16_t vers;
    CAENComm_ErrorCode err = CAENComm_Read16(handle, 0x4030, &vers);
    if (err != CAENComm_Success)
        return -1;
    return (vers == 0x00) ? 1 : 0;
}

#endif /* CAEN_OPEN_H */
