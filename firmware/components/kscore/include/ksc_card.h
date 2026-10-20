// Smart card transport: a minimal USB CCID reader with a permanently inserted
// "card" that runs one tiny applet. Portable; the USB plumbing lives in the
// card app. The card deliberately does NOT look like a PIV/GIDS/login card:
// its own ATR, its own proprietary AID, and it answers 6A82 to every other
// SELECT, so Windows treats it as an unknown card and leaves it alone.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ksa_frame.h"
#include "ksb_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KSC_MAX_MSG 271           // 10-byte CCID header + short APDU
#define KSC_AID_LEN 8
extern const uint8_t KSC_AID[KSC_AID_LEN]; // F0 'K' 'S' 'L' 'I' 'G' 'H' 'T'

enum {
    KSC_INS_SUBMIT = 0x10,        // 80 10 00 00 Lc [op|args]  -> gen(2) 9000
    KSC_INS_STATUS = 0x20,        // 80 20 00 00 00            -> status 9000
};

// Applet hooks provided by the firmware.
typedef struct {
    // Queue a validated command; fill *gen with the generation that will
    // include it. Return 0 on success.
    int (*submit)(const ksa_cmd_t *cmd, uint16_t *gen, void *ctx);
    // Write the status record (see README), return its length.
    size_t (*status)(uint8_t *out, size_t cap, void *ctx);
    void *ctx;
    bool selected;
} ksc_app_t;

enum { KSC_RESULT_OK = 0, KSC_RESULT_NO_LAMP = 1, KSC_RESULT_WRITE_FAILED = 2,
       KSC_RESULT_UNSUPPORTED = 3, KSC_RESULT_NONE = 0xFF };

typedef struct {
    bool claimed, connected;
    char name[20];
} ksc_lamp_info_t;

// Status record v2:
//   [0] version=2  [1] link (0 boot, 1 scanning, 2 any lamp connected)
//   [2..3] desired gen (BE)  [4..5] applied gen (BE)  [6] last write result
//   [7] n sources, then per source: id, state, acked, lamp (0 = all, 1..4), name_len, name...
//   then n lamps, per lamp: flags (bit0 claimed, bit1 connected), name_len, name...
size_t ksc_status_encode(uint8_t *out, size_t cap, const ksb_model_t *m, uint8_t link,
                         uint16_t desired_gen, uint16_t applied_gen, uint8_t result,
                         const ksc_lamp_info_t *lamps, int n_lamps);

// Process one APDU, write response (data + SW1 SW2). Returns response length.
size_t ksc_app_apdu(ksc_app_t *app, const uint8_t *apdu, size_t len, uint8_t *resp, size_t cap);

typedef struct {
    ksc_app_t *app;
    bool powered;
} ksc_ccid_t;

const uint8_t *ksc_atr(size_t *len);

// Process one complete PC_to_RDR message; write the RDR_to_PC reply.
// Returns reply length (0 only if `in` is not a complete message).
size_t ksc_ccid_handle(ksc_ccid_t *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap);

// Total length of the message at `in` once its header is available, else 0.
size_t ksc_ccid_msg_len(const uint8_t *in, size_t have);

#ifdef __cplusplus
}
#endif
