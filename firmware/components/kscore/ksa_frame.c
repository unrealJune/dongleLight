#include "ksa_frame.h"
#include <stdbool.h>
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

static bool args_ok(uint8_t op, const uint8_t *args, size_t n)
{
    switch (op) {
    case KSA_OP_POWER:  return n == 1;
    case KSA_OP_RGB:    return n == 4;
    case KSA_OP_EFFECT: return n == 3;
    case KSA_OP_WHITE:  return n == 1;
    case KSA_OP_ACK:    return n == 0;
    case KSA_OP_SIGNAL:
        if (n < 2 || n > 2 + KSA_SIGNAL_NAME_MAX || args[1] >= KSA_SIG_COUNT) return false;
        for (size_t i = 2; i < n; i++) {
            if (args[i] < 0x20 || args[i] > 0x7E) return false; // printable ASCII only
        }
        return true;
    default:
        return false;
    }
}

int ksa_frame_parse(const uint8_t *bytes, size_t len, ksa_cmd_t *cmd)
{
    if (len < 2 || len > KSA_MAX_FRAME_BYTES) return -1;
    if (ksa_crc8(bytes, len - 1) != bytes[len - 1]) return -2;
    if ((bytes[0] >> 4) != KSA_VERSION_NIBBLE) return -3;
    return ksa_cmd_from_body(bytes, len - 1, cmd);
}

int ksa_cmd_from_body(const uint8_t *body, size_t len, ksa_cmd_t *cmd)
{
    if (len < 1 || len > KSA_MAX_FRAME_BYTES - 1) return -1;
    uint8_t op = body[0] & 0x0F;
    size_t nargs = len - 1;
    if (!args_ok(op, body + 1, nargs)) return -4;
    cmd->op = op;
    cmd->nargs = nargs;
    memcpy(cmd->args, body + 1, nargs);
    return 0;
}
