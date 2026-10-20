// Checks the C KS encoders against upstream protocol/golden_packets.json
// (copied verbatim below) plus frame CRC/parse behaviour.
#include <stdio.h>
#include <string.h>
#include "ksa_frame.h"
#include "ksa_protocol.h"

static int fails;

static void expect(const char *name, const uint8_t *got, size_t n, const char *hex)
{
    char buf[64] = {0};
    for (size_t i = 0; i < n; i++) sprintf(buf + 2 * i, "%02X", got[i]);
    if (strcmp(buf, hex) != 0) {
        printf("FAIL %s: got %s want %s\n", name, buf, hex);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    uint8_t out[KS_MAX_PACKET];
    const ks_profile_t *std = ks_profile_for_name("KS03-ABCD");
    const ks_profile_t *ext = ks_profile_for_name("KS03~1234");
    if (!std || std->fmt != KS_FMT_STANDARD || std->service != 0xFFF0) { puts("FAIL KS03- profile"); fails++; }
    if (!ext || ext->fmt != KS_FMT_EXTENDED || ext->write_chr != 0xAFD1) { puts("FAIL KS03~ profile"); fails++; }
    if (ks_profile_for_name("ELK-BLEDOM")) { puts("FAIL non-KS matched"); fails++; }

    expect("on", out, ks_pkt_power(true, out), "5BF001B5");
    expect("off", out, ks_pkt_power(false, out), "5B0F01B5");
    expect("standard red", out, ks_pkt_rgb(std, 255, 0, 0, 100, out), "7E070503FF000000EF");
    expect("standard blue", out, ks_pkt_rgb(std, 0, 0, 255, 100, out), "7E0705030000FF00EF");
    expect("standard black", out, ks_pkt_rgb(std, 0, 0, 0, 100, out), "7E07050300000000EF");
    expect("standard white", out, ks_pkt_rgb(std, 255, 255, 255, 100, out), "7E070503FFFFFF00EF");
    expect("extended red", out, ks_pkt_rgb(ext, 255, 0, 0, 255, out), "5A0001FF000000FF00A5");
    expect("extended dim blue", out, ks_pkt_rgb(ext, 0, 0, 255, 127, out), "5A00010000FF007F00A5");
    expect("extended zero brightness", out, ks_pkt_rgb(ext, 255, 255, 255, 0, out), "5A0001FFFFFF000000A5");
    expect("white full", out, ks_pkt_white(ext, 255, out), "5A000200000000FF00A5");
    expect("white zero", out, ks_pkt_white(ext, 0, out), "5A0002000000000000A5");
    // not in golden file: derived from protocol.py native_effect()
    expect("effect purple", out, ks_pkt_effect(ext, 0x89, 40, 100, out), "5C0089286400C5");
    if (ks_pkt_effect(std, 0x89, 40, 100, out) != 0) { puts("FAIL effect on standard"); fails++; }
    expect("standard half red", out, ks_pkt_rgb(std, 255, 0, 0, 50, out), "7E07050380000000EF");

    uint8_t frame[] = {0x52, 0xFF, 0x88, 0x00, 100, 0};
    frame[5] = ksa_crc8(frame, 5);
    ksa_cmd_t cmd;
    if (ksa_frame_parse(frame, 6, &cmd) || cmd.op != 2 || cmd.args[1] != 0x88) { puts("FAIL parse"); fails++; }
    frame[2] ^= 1;
    if (ksa_frame_parse(frame, 6, &cmd) != -2) { puts("FAIL crc reject"); fails++; }

    printf(fails ? "%d FAILED\n" : "all protocol tests passed\n", fails);
    return fails != 0;
}
