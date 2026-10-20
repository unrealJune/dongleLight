// Wire format shared by the devbox transmitter (host/ks_tx.py) and the dongle.
//
// Each DTMF symbol carries one hex nibble:
//   0-9 -> '0'-'9', 0xA-0xD -> 'A'-'D', 0xE -> '*', 0xF -> '#'
// A frame is: [0x5<<4 | op] [args...] [crc8], sent high nibble first.
// Frames are delimited by silence (>= KSA_FRAME_END_MS with no tone).
// CRC-8 poly 0x07, init 0x00, over every byte before the CRC.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KSA_VERSION_NIBBLE 0x5
#define KSA_MAX_FRAME_BYTES 16

enum {
    KSA_OP_POWER  = 0x1, // [on(0/1)]
    KSA_OP_RGB    = 0x2, // [r][g][b][brightness 0..100]
    KSA_OP_EFFECT = 0x3, // [effect id 0x82..0x8A][speed 0..100][brightness 1..100]
    KSA_OP_WHITE  = 0x4, // [level 0..255]
    KSA_OP_SIGNAL = 0x5, // [source id][state][name: 0..8 ASCII bytes]
    KSA_OP_ACK    = 0x6, // [] acknowledge all attention signals (same as the button)
};

// Agent signal states, lowest to highest urgency.
enum {
    KSA_SIG_IDLE = 0,     // clears the source
    KSA_SIG_WORKING = 1,  // screen only, never touches the lamp
    KSA_SIG_DONE = 2,     // lamp: green, expires on its own
    KSA_SIG_ERROR = 3,    // lamp: red until acknowledged
    KSA_SIG_NEEDS_YOU = 4,// lamp: pulsing amber until acknowledged
    KSA_SIG_COUNT
};

#define KSA_SIGNAL_NAME_MAX 8

typedef struct {
    uint8_t op;
    uint8_t args[KSA_MAX_FRAME_BYTES];
    size_t nargs;
} ksa_cmd_t;

uint8_t ksa_crc8(const uint8_t *data, size_t len);

// Validate raw frame bytes (including trailing CRC). Returns 0 and fills cmd on
// success, negative on bad length/CRC/version/arg count.
int ksa_frame_parse(const uint8_t *bytes, size_t len, ksa_cmd_t *cmd);

// Validate a body without version/CRC (the smart card path carries the same
// [op][args] body inside an APDU; USB already guarantees integrity).
int ksa_cmd_from_body(const uint8_t *body, size_t len, ksa_cmd_t *cmd);

#ifdef __cplusplus
}
#endif
