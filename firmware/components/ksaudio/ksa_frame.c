#include "ksa_frame.h"
#include <string.h>

uint8_t ksa_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static int expected_args(uint8_t op)
{
    switch (op) {
    case KSA_OP_POWER:  return 1;
    case KSA_OP_RGB:    return 4;
    case KSA_OP_EFFECT: return 3;
    case KSA_OP_WHITE:  return 1;
    default:            return -1;
    }
}

int ksa_frame_parse(const uint8_t *bytes, size_t len, ksa_cmd_t *cmd)
{
    if (len < 3 || len > KSA_MAX_FRAME_BYTES) return -1;
    if (ksa_crc8(bytes, len - 1) != bytes[len - 1]) return -2;
    if ((bytes[0] >> 4) != KSA_VERSION_NIBBLE) return -3;
    uint8_t op = bytes[0] & 0x0F;
    size_t nargs = len - 2;
    if (expected_args(op) != (int)nargs) return -4;
    cmd->op = op;
    cmd->nargs = nargs;
    memcpy(cmd->args, bytes + 1, nargs);
    return 0;
}
