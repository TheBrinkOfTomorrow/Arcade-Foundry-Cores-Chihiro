// The Chihiro baseboard as the game sees it over USB: two Cypress AN2131
// microcontrollers on the OHCI root hub.
//
//   port 1, AN2131QC (vendor 0x0CA3 product 0x0002): the JVS master. Vendor
//     requests read the baseboard's I2C EEPROMs, send a batch of JVS packets
//     (0x20, data on bulk endpoint 4 OUT) and fetch their answers (0x19,
//     data on bulk endpoint 4 IN). Byte 2 of every request's answer
//     (PINSB) carries the JVS sense line: 3 while a node has no address.
//   port 2, AN2131SC (product 0x0003): two UARTs, one of them to the drive
//     or sound board (four-byte commands, one-byte answers).
//
// One JVS node hangs off the master: a Sega 837-13551 I/O board (two
// players, 13 switches each, 2 coin slots, 8 analog channels, 6 outputs),
// read from jvs_inputs.
//
// The command set, descriptors and answers follow MAME's chihiro.cpp and
// jvs13551.cpp/jvsdev.cpp/jvshost.cpp (BSD-3-Clause, Samuele Zannoli and
// Olivier Galibert); see THIRD_PARTY.md.
//
// The EEPROM contents come from the owner's Chihiro BIOS set (MAME's
// chihiro.zip: ic10_g24lc64.bin, ic11_24lc024.bin, pc20_g24lc64.bin), read
// at start from $CH_BIOS (chihiro.zip itself, or a folder), else beside the
// game folder, in its bios/ folder, in the game folder or one folder up:
// each place as chihiro.zip or as the files unzipped (load_eeproms).
#include "af_debug.h"
#include "xbox.h"
#include "usb.h"
#include "baseboard.h"
#include "afcommon/zip_c.h"
#include <string.h>
#include <pthread.h>

int baseboard_panel_buttons;
jvs_input_state jvs_inputs = { .analog = { 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000 } };

// the EEPROMs, 16-bit words swapped as MAME loads them: ic10 at 0, ic11 at 0x2000 (MAME: 0x80 bytes), pc20 at 0x2100
static uint8_t eeprom[0x4100];

// the dumps hold 16-bit words the other way round
static void put_eeprom(const uint8_t *tmp, size_t n, uint32_t at)
{
    for (size_t i = 0; i + 1 < n; i += 2) eeprom[at + i] = tmp[i + 1], eeprom[at + i + 1] = tmp[i];
}

static const struct { const char *name; uint32_t at, size; } eeprom_files[] = {
    { "ic10_g24lc64.bin", 0, 0x2000 },        // the first is required, the others optional
    { "ic11_24lc024.bin", 0x2000, 0x100 },    // a 24LC024 holds 256 bytes; the dump may have only 128
    { "pc20_g24lc64.bin", 0x2100, 0x2000 },
};

static int load_from_folder(const char *dir)
{
    for (size_t k = 0; k < sizeof eeprom_files / sizeof eeprom_files[0]; k++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, eeprom_files[k].name);
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (k == 0) return 0;
            continue;
        }
        uint8_t tmp[0x2000];
        size_t n = fread(tmp, 1, eeprom_files[k].size, f);
        fclose(f);
        put_eeprom(tmp, n, eeprom_files[k].at);
    }
    return 1;
}

static int load_from_zip(const char *path)
{
    afc_zip *z = afc_zip_open(path, NULL, 0);
    if (!z) return 0;
    char err[256];
    for (size_t k = 0; k < sizeof eeprom_files / sizeof eeprom_files[0]; k++) {
        uint8_t tmp[0x2000];
        long long n = afc_zip_read(z, eeprom_files[k].name, tmp, eeprom_files[k].size, err, sizeof err);
        if (n < 0) {
            hle_log("baseboard: %s: %s\n", path, err);
            if (k == 0) {
                afc_zip_close(z);
                return 0;
            }
            continue;
        }
        put_eeprom(tmp, (size_t)n < eeprom_files[k].size ? (size_t)n : eeprom_files[k].size, eeprom_files[k].at);
    }
    afc_zip_close(z);
    return 1;
}

static int ends_with_zip(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && (s[n - 4] == '.') && (s[n - 3] | 32) == 'z' && (s[n - 2] | 32) == 'i' && (s[n - 1] | 32) == 'p';
}

// The AN2131QC's two EEPROMs as the games last wrote them (requests 0x1d and
// 0x1e), kept in the save folder so what the games store there survives a
// restart. Read over the BIOS set's dumps at start.
static void eeprom_saved_path(char *path, size_t n) { snprintf(path, n, "%s/baseboard-eeprom.bin", save_root); }

static void load_saved_eeprom(void)
{
    char path[1100];
    eeprom_saved_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(eeprom, 1, 0x2100, f);
    fclose(f);
    hle_log("baseboard: EEPROM as last written (%zu bytes) from %s\n", n, path);
}

static void save_eeprom(void)
{
    char path[1100], tmp[1110];
    eeprom_saved_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(eeprom, 1, 0x2100, f) == 0x2100;
    ok &= fclose(f) == 0;
    if (ok) rename(tmp, path);
}

// The AN2131QC's external memory (MAME: 0x18 reads it on endpoint 3 IN,
// 0x1f writes it on endpoint 3 OUT; addresses from 0x8000): the games keep
// their backup data there (Crazy Taxi: its operator settings, through Sega's
// backup library). Kept in <save>/baseboard-memory.bin.
static uint8_t qc_xmem[0x10000];

static void xmem_path(char *path, size_t n) { snprintf(path, n, "%s/baseboard-memory.bin", save_root); }

static void load_xmem(void)
{
    char path[1100];
    xmem_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(qc_xmem, 1, sizeof qc_xmem, f);
    fclose(f);
    hle_log("baseboard: backup memory (%zu bytes) from %s\n", n, path);
}

static void save_xmem(void)
{
    char path[1100], tmp[1110];
    xmem_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(qc_xmem, 1, sizeof qc_xmem, f) == sizeof qc_xmem;
    ok &= fclose(f) == 0;
    if (ok) rename(tmp, path);
}

static void load_eeproms(void)
{
    char dirs[6][1100];
    int nd = 0;
    const char *env = af_debug_env("CH_BIOS");
    if (env && *env) {
        if (ends_with_zip(env)) {
            if (load_from_zip(env)) {
                hle_log("baseboard: EEPROMs from %s\n", env);
                return;
            }
        } else {
            snprintf(dirs[nd++], sizeof dirs[0], "%s", env);
        }
    }
    snprintf(dirs[nd++], sizeof dirs[0], "%s/..", game_root);         // chihiro.zip beside the game (ROMs/Chihiro/)
    snprintf(dirs[nd++], sizeof dirs[0], "%s/../chihiro", game_root); // the set unzipped beside the game
    snprintf(dirs[nd++], sizeof dirs[0], "%s/../bios", game_root);
    snprintf(dirs[nd++], sizeof dirs[0], "%s", game_root);
    snprintf(dirs[nd++], sizeof dirs[0], "%s/../..", game_root);      // the ROM folder above a board folder
    for (int i = 0; i < nd; i++) {
        char zip[1200];
        snprintf(zip, sizeof zip, "%s/chihiro.zip", dirs[i]);
        if (load_from_zip(zip)) {
            hle_log("baseboard: EEPROMs from %s\n", zip);
            return;
        }
        if (load_from_folder(dirs[i])) {
            hle_log("baseboard: EEPROMs from %s\n", dirs[i]);
            return;
        }
    }
    hle_log("baseboard: no Chihiro BIOS set (chihiro.zip) found: fresh operator settings; put it beside the game or set CH_BIOS\n");
}

// ---- the JVS I/O board (837-13551) ---------------------------------------------------------------------
static uint8_t jvs_address = 0xff;
static int jvs_reset_count;

static int jvs_command(const uint8_t *s, uint32_t n, uint8_t **out)
{
    uint8_t *d = *out;
    int r = 0;
    switch (s[0]) {
    case 0xf0:   // reset (twice in a row)
        if (n < 2 || s[1] != 0xd9) return 0;
        if (++jvs_reset_count >= 2) jvs_address = 0xff, jvs_reset_count = 0;
        return 2;
    case 0x10: {
        static const char id[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551";
        *d++ = 1;
        memcpy(d, id, sizeof id);
        d += sizeof id;
        r = 1;
        break;
    }
    case 0x11: *d++ = 1, *d++ = 0x11, r = 1; break;
    case 0x12: *d++ = 1, *d++ = 0x20, r = 1; break;
    case 0x13: *d++ = 1, *d++ = 0x10, r = 1; break;
    case 0x14: {
        static const uint8_t f[] = { 0x01, 2, 13, 0, 0x02, 2, 0, 0, 0x03, 8, 16, 0, 0x12, 6, 0, 0, 0x00 };
        *d++ = 1;
        memcpy(d, f, sizeof f);
        d += sizeof f;
        r = 1;
        break;
    }
    case 0x20: {   // switches: players, bytes each
        if (n < 3 || s[1] > 2 || s[2] > 2) return 0;
        *d++ = 1;
        *d++ = jvs_inputs.system;
        for (int p = 0; p < s[1]; p++)
            for (int b = 0; b < s[2]; b++) *d++ = (uint8_t)(jvs_inputs.player[p] >> (8 * (1 - b)));
        r = 3;
        break;
    }
    case 0x21:   // coins
        if (n < 2 || s[1] > 2) return 0;
        *d++ = 1;
        for (int c = 0; c < s[1]; c++) *d++ = jvs_inputs.coins[c] >> 8 & 0x3f, *d++ = (uint8_t)jvs_inputs.coins[c];
        r = 2;
        break;
    case 0x22:   // analog channels
        if (n < 2 || s[1] > 8) return 0;
        *d++ = 1;
        for (int c = 0; c < s[1]; c++) *d++ = jvs_inputs.analog[c] >> 8, *d++ = (uint8_t)jvs_inputs.analog[c];
        r = 2;
        break;
    case 0x30:   // coins used
    case 0x31: { // coins added
        if (n < 4 || s[1] < 1 || s[1] > 2) return 0;
        int v = s[2] << 8 | s[3];
        jvs_inputs.coins[s[1] - 1] += s[0] == 0x30 ? -v : v;
        *d++ = 1;
        r = 4;
        break;
    }
    case 0x32:   // general-purpose outputs
        if (n < 2 || n < 2u + s[1] || s[1] > 1) return 0;
        jvs_inputs.outputs = s[1] ? s[2] & 0xfc : 0;
        *d++ = 1;
        r = 2 + s[1];
        break;
    case 0x38:
        if (n < 3) return 0;
        *d++ = 1;
        r = 3;
        break;
    default:
        if (hle_verbose > 0) hle_log("jvs: command 0x%02x not handled\n", s[0]);
        return 0;
    }
    jvs_reset_count = 0;
    *out = d;
    return r;
}

// one packet's body (after destination and length) to the I/O board; its reply body, or 0 bytes
static uint32_t jvs_message(uint8_t dest, const uint8_t *s, uint32_t n, uint8_t *reply)
{
    if (n == 2 && s[0] == 0xf1) {   // set address
        jvs_address = s[1];
        reply[0] = 1, reply[1] = 1;
        return 2;
    }
    if (dest != 0xff && dest != jvs_address) return 0;
    uint8_t *d = reply;
    *d++ = 1;   // status: normal
    const uint8_t *p = s;
    while (p < s + n) {
        int used = jvs_command(p, s + n - p, &d);
        if (used <= 0) {
            *d++ = 2;   // report: parameter error
            break;
        }
        p += used;
    }
    return d - reply;
}

// ---- the AN2131QC ----------------------------------------------------------------------------------------
static const uint8_t qc_dev[] = { 0x12, 0x01, 0x00, 0x01, 0x60, 0x00, 0x00, 0x40, 0xa3, 0x0c,
                                  0x02, 0x00, 0x08, 0x01, 0x01, 0x02, 0x00, 0x01 };
static const uint8_t qc_conf[] = {
    0x09, 0x02, 0x58, 0x00, 0x01, 0x01, 0x00, 0x80, 0x96,
    0x09, 0x04, 0x00, 0x00, 0x0a, 0xff, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x03, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x04, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x05, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x84, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x85, 0x02, 0x40, 0x00, 0x00,
};
static const uint8_t str0[] = { 0x04, 0x03, 0x00, 0x00 };
static const uint8_t str1[] = { 0x0a, 0x03, 'S', 0, 'E', 0, 'G', 0, 'A', 0 };
static const uint8_t qc_str2[] = { 0x0e, 0x03, 'B', 0, 'A', 0, 'S', 0, 'E', 0, 'B', 0x03, 0xff, 0x0b };
static const uint8_t *const qc_strings[] = { str0, str1, qc_str2 };

static struct {
    uint8_t in[32768];
    int in_expected;
    uint8_t out[32768];
    int out_used, out_packets;
} jvs;

// CH_VERBOSE=3: the bytes of a JVS batch or answer
static void jvs_dump(const char *what, const uint8_t *b, int n)
{
    if (hle_verbose < 3) return;
    char line[3 * 96 + 8];
    int k = 0;
    for (int i = 0; i < n && i < 96; i++) k += snprintf(line + k, sizeof line - k, " %02x", b[i]);
    hle_log("jvs: %s %d bytes:%s%s\n", what, n, line, n > 96 ? " ..." : "");
}

static void process_jvs_packets(void)
{
    jvs_dump("batch in", jvs.in, jvs.in_expected);
    // the batch's first byte numbers it; the answers carry the same number back
    // (a JVS layer may match answers to batches by it; suspected for Crazy
    // Taxi's test program, which retries only every 16 s)
    jvs.out[0] = jvs.in[0];
    int npk = jvs.in[1], p = 2;
    for (int k = 0; k < npk; k++) {
        p++;   // a zero byte
        if (jvs.in[p] != 0xe0) break;
        p++;
        int dest = jvs.in[p++], len = jvs.in[p++];
        if (p + len > jvs.in_expected || len < 1) break;
        int sum = dest + len;
        for (int m = 0; m < len - 1; m++) sum += jvs.in[p + m];
        if ((sum & 0xff) != jvs.in[p + len - 1]) {
            if (hle_verbose > 2) hle_log("jvs: packet %d to %02x: bad sum %02x, %02x expected\n", k, dest, jvs.in[p + len - 1], sum & 0xff);
            p += len;
            continue;
        }
        uint8_t reply[512];
        uint32_t n = jvs_message((uint8_t)dest, jvs.in + p, len - 1, reply);
        if (n && jvs.out_used + 8 + (int)n < (int)sizeof jvs.out) {
            // node, 0, length (LE) of what follows, then E0 00 len body sum
            uint8_t *o = jvs.out + jvs.out_used;
            int body = n + 1;   // the length byte counts the checksum
            o[0] = dest == 0xff ? 0 : (uint8_t)dest;
            o[1] = 0;
            o[4] = 0xe0;
            o[5] = 0x00;
            o[6] = (uint8_t)body;
            memcpy(o + 7, reply, n);
            int s2 = 0x00 + body;
            for (uint32_t m = 0; m < n; m++) s2 += reply[m];
            o[7 + n] = (uint8_t)s2;
            int follow = 4 + n;   // E0, 00, len, body, sum
            o[2] = (uint8_t)follow, o[3] = (uint8_t)(follow >> 8);
            jvs.out_used += 4 + follow;
            jvs.out[1] = (uint8_t)++jvs.out_packets;
        }
        p += len;
    }
}

static int qc_vendor(usb_device *d, const uint8_t *s)
{
    usb_endpoint *e0 = &d->ep[0];
    int req = s[1];
    uint16_t value = s[2] | s[3] << 8, index = s[4] | s[5] << 8, length = s[6] | s[7] << 8;
    if (length > sizeof e0->buf) length = sizeof e0->buf;
    for (int i = 0; i < length; i++) e0->buf[i] = 0x50 ^ i;
    if (hle_verbose > 1) hle_log("an2131qc: request 0x%02x type 0x%02x value 0x%x index 0x%x length %u\n", req, s[0], value, index, length);
    if (hle_verbose > 3 && af_debug_env("CH_STACKSCAN")) {   // crude backtrace of the waiting thread
        uint32_t lo = strtoul(af_debug_env("CH_STACKSCAN"), NULL, 16);
        for (uint32_t a = lo; a < lo + 0x600; a += 4) {
            uint32_t v = rd32(a);
            if (v >= 0x11000 && v < 0x190000) hle_log("  stack %08x: %08x\n", a, v);
        }
    }
    // PINSA: dip switches in the low bits; the baseboard's own TEST (0x40)
    // and SERVICE (0x80) buttons, active low. Crazy Taxi's test program reads
    // its buttons only here. VC3 and HotD3 read them here as well as from the
    // JVS I/O board, so feeding both steps their menus twice a press: the
    // buttons come here only while a game's own test program runs.
    if (baseboard_panel_buttons)
        e0->buf[1] = 0x0b | (jvs_inputs.system & 0x80 ? 0 : 0x40) | (jvs_inputs.player[0] & 0x4000 ? 0 : 0x80);
    else
        e0->buf[1] = 0x4b;
    e0->buf[2] = 0x52 | (jvs_address == 0xff ? 3 : 0);   // PINSB: the sense line
    e0->buf[3] = 0x53;   // OUTB
    e0->buf[0] = 0;
    switch (req) {
    case 0x16:   // read the first EEPROM, out on endpoint 1
        d->ep[1].pos = eeprom + (value & 0x1fff);
        d->ep[1].remain = index & 0xff;
        break;
    case 0x17:   // the second, on endpoint 2
        d->ep[2].pos = eeprom + 0x2000 + (value & 0xff);
        d->ep[2].remain = index & 0xff;
        break;
    case 0x19:   // the answers to the last 0x20, on endpoint 4
        jvs_dump("answers out", jvs.out, jvs.out_used);
        e0->buf[4] = (uint8_t)jvs.out_used;
        e0->buf[5] = (uint8_t)(jvs.out_used >> 8);
        memcpy(d->ep[4].buf, jvs.out, jvs.out_used < (int)sizeof d->ep[4].buf ? jvs.out_used : (int)sizeof d->ep[4].buf);
        d->ep[4].pos = d->ep[4].buf;
        d->ep[4].remain = jvs.out_used;
        jvs.out_used = 0;
        break;
    case 0x20:   // JVS packets follow on endpoint 4
        if (jvs.out_used == 0) {
            jvs.out_packets = 0;
            jvs.out[0] = 0, jvs.out[1] = 0;
            jvs.out_used = 2;
        }
        jvs.in_expected = index < sizeof jvs.in ? index : sizeof jvs.in;
        d->ep[4].pos = jvs.in;
        d->ep[4].remain = jvs.in_expected;
        break;
    case 0x1d:   // write the first EEPROM: the bytes follow on endpoint 1 (OUT)
        d->ep[1].pos = eeprom + (value & 0x1fff);
        d->ep[1].remain = index & 0xff;
        if ((value & 0x1fff) + d->ep[1].remain > 0x2000) d->ep[1].remain = 0x2000 - (value & 0x1fff);
        break;
    case 0x1e:   // write the second (8 bytes at most): on endpoint 2 (OUT)
        d->ep[2].pos = eeprom + 0x2000 + (value & 0xff);
        d->ep[2].remain = index & 0xff;
        if ((value & 0xff) + d->ep[2].remain > 0x100) d->ep[2].remain = 0x100 - (value & 0xff);
        break;
    case 0x18:   // read external memory, on endpoint 3 (IN)
    case 0x1f: { // write it, on endpoint 3 (OUT)
        uint32_t at = value, n = index;
        if (at + n > sizeof qc_xmem) n = at < sizeof qc_xmem ? (uint32_t)sizeof qc_xmem - at : 0;
        d->ep[3].pos = qc_xmem + (at < sizeof qc_xmem ? at : 0);
        d->ep[3].remain = n;
        break;
    }
    case 0x1c: case 0x24:
        break;
    case 0x30:   // external interrupt 0 on or off, and its counter
        e0->buf[4] = (value & 0xff) == 0 ? 0 : (value & 0xff) < 128 ? 1 : e0->buf[4];
        e0->buf[5] = 0;
        break;
    default:
        e0->buf[0] = 0x99;
        break;
    }
    e0->pos = e0->buf;
    e0->remain = length;
    return 0;
}

static int qc_bulk(usb_device *d, int ep, int pid, uint8_t *buf, int size)
{
    usb_endpoint *e = &d->ep[ep];
    int n = size < e->remain ? size : e->remain;
    if (n < 0) n = 0;
    if (pid == USB_IN && (ep == 1 || ep == 2 || ep == 3 || ep == 4)) {
        if (n) memcpy(buf, e->pos, n);
        e->pos += n, e->remain -= n;
        return n;
    }
    if (pid == USB_OUT && ep == 4) {
        if (n) {
            memcpy(e->pos, buf, n);
            e->pos += n, e->remain -= n;
            if (e->remain == 0) process_jvs_packets();
        }
        return size;
    }
    if (pid == USB_OUT && ep == 3 && n) {   // backup memory bytes (0x1f): saved once the write is complete
        memcpy(e->pos, buf, n);
        e->pos += n, e->remain -= n;
        if (e->remain == 0) save_xmem();
        return size;
    }
    if (pid == USB_OUT && (ep == 1 || ep == 2) && n) {   // EEPROM bytes (0x1d, 0x1e)
        memcpy(e->pos, buf, n);
        e->pos += n, e->remain -= n;
        if (hle_verbose > 1) hle_log("an2131qc: EEPROM %d: %d bytes written\n", ep, n);
        save_eeprom();
    }
    return pid == USB_IN ? 0 : size;
}

static usb_device qc = { qc_dev, qc_conf, qc_strings, 3, qc_vendor, qc_bulk, NULL };

// ---- the AN2131SC ----------------------------------------------------------------------------------------
static const uint8_t sc_dev[] = { 0x12, 0x01, 0x00, 0x01, 0x60, 0x01, 0x00, 0x40, 0xa3, 0x0c,
                                  0x03, 0x00, 0x10, 0x01, 0x01, 0x02, 0x00, 0x01 };
static const uint8_t sc_conf[] = {
    0x09, 0x02, 0x3c, 0x00, 0x01, 0x01, 0x00, 0x80, 0x96,
    0x09, 0x04, 0x00, 0x00, 0x06, 0xff, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x03, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,
};
static const uint8_t sc_str2[] = { 0x0e, 0x03, 'B', 0, 'A', 0, 'S', 0, 'E', 0, 'B', 0, 'D', 0 };
static const uint8_t *const sc_strings[] = { str0, str1, sc_str2 };
static uint8_t sc_midi_rs232, sc_response[256], sc_packet[4];
static int sc_response_size, sc_step;

static void sc_process_packet(void)
{
    uint8_t r = 0;
    switch (sc_packet[0]) {
    case 0xff: r = 2; break;
    case 0x81: r = 1; break;
    case 0xfc: r = 3; break;
    }
    if (sc_response_size < 256) sc_response[sc_response_size++] = r | r << 4;
}

static int sc_vendor(usb_device *d, const uint8_t *s)
{
    usb_endpoint *e0 = &d->ep[0];
    int req = s[1];
    uint16_t value = s[2] | s[3] << 8, index = s[4] | s[5] << 8, length = s[6] | s[7] << 8;
    if (length > sizeof e0->buf) length = sizeof e0->buf;
    for (int i = 0; i < length; i++) e0->buf[i] = 0x50 ^ i;
    if (hle_verbose > 1) hle_log("an2131sc: request 0x%02x value 0x%x index 0x%x length %u\n", req, value, index, length);
    e0->buf[1] = 0;
    e0->buf[2] = 0x52, e0->buf[3] = 0x53, e0->buf[4] = 0, e0->buf[5] = 0, e0->buf[6] = 0x56, e0->buf[7] = 0x57;
    e0->buf[0] = 0;
    switch (req) {
    case 0x16:
        d->ep[1].pos = eeprom + 0x2100 + (value & 0x1fff);
        d->ep[1].remain = index & 0xff;
        break;
    case 0x17: case 0x1e:
        e0->buf[0] = 0x90;
        break;
    case 0x1b: {   // what UART 1 received, on endpoint 3
        int n = (index == 0 || index > sc_response_size) ? sc_response_size : index;
        e0->buf[4] = (uint8_t)n, e0->buf[5] = (uint8_t)(n >> 8);
        memcpy(d->ep[3].buf, sc_response, n);
        memmove(sc_response, sc_response + n, sc_response_size - n);
        sc_response_size -= n;
        d->ep[3].pos = d->ep[3].buf;
        d->ep[3].remain = index;
        break;
    }
    case 0x23:   // bytes for UART 1, on endpoint 3
        d->ep[3].pos = d->ep[3].buf;
        d->ep[3].remain = index;
        break;
    case 0x1d: case 0x25: case 0x26: case 0x27: case 0x31:
        break;
    case 0x2e:
        sc_midi_rs232 = value & 0xff;
        break;
    case 0x2f:
        e0->buf[4] = sc_midi_rs232;
        break;
    case 0x30:
        e0->buf[4] = (value & 0xff) == 0 ? 0 : (value & 0xff) < 128 ? 1 : e0->buf[4];
        e0->buf[5] = 0;
        break;
    default:
        e0->buf[0] = 0x99;
        break;
    }
    e0->pos = e0->buf;
    e0->remain = length;
    return 0;
}

static int sc_bulk(usb_device *d, int ep, int pid, uint8_t *buf, int size)
{
    usb_endpoint *e = &d->ep[ep];
    int n = size < e->remain ? size : e->remain;
    if (n < 0) n = 0;
    if (pid == USB_IN && (ep == 1 || ep == 2 || ep == 3)) {
        if (n) memcpy(buf, e->pos, n);
        e->pos += n, e->remain -= n;
        return n;
    }
    if (pid == USB_OUT && ep == 3) {
        if (n) memcpy(e->pos, buf, n), e->pos += n, e->remain -= n;
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (sc_step == 0) {
                if (b & 0x80) sc_packet[0] = b, sc_step = 1;
            } else {
                sc_packet[sc_step++] = b;
                if (sc_step == 4) sc_step = 0, sc_process_packet();
            }
        }
        return size;
    }
    return pid == USB_IN ? 0 : size;
}

static usb_device sc = { sc_dev, sc_conf, sc_strings, 3, sc_vendor, sc_bulk, NULL };

// ---- the media board's command channel (\??\mbcom:) ----------------------------------------------------
// As MAME's ide_baseboard_device and baseboard_ide_event: two sectors of
// the mbcom: partition, 0x4800 (the answer) and 0x4801 (the command). A
// write of sector 0x4801 runs the command: words 0-1 echo back, word 1 of
// the answer is 0x8001, and the command number is in bytes 2-3. The DIMM
// board's serial number is MEDIA BD ID on the boot screen.
#define MB_ANSWER 0x4800
#define MB_COMMAND 0x4801
static uint8_t mb_answer[512], mb_command[512];
static pthread_mutex_t mb_lock = PTHREAD_MUTEX_INITIALIZER;

static void put32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

static void mb_run(void)
{
    if (!mb_command[0] && !mb_command[1]) return;
    uint32_t cmd = mb_command[2] | mb_command[3] << 8;
    memset(mb_answer, 0, 32);
    mb_answer[0] = mb_command[0], mb_answer[1] = mb_command[1];
    mb_answer[2] = 0x01, mb_answer[3] = 0x80;
    switch (cmd) {
    case 0x0001: put32(mb_answer + 4, 0x00f00000); break;
    case 0x0100: put32(mb_answer + 4, 5); put32(mb_answer + 8, 0); break;   // loading phase 5: done, 0 %
    case 0x0101:   // DIMM board firmware version (BCD, 12.34 in MAME)
        mb_answer[4] = 0x34, mb_answer[5] = 0x12, mb_answer[6] = 0x67, mb_answer[7] = 0x45;
        break;
    case 0x0102: put32(mb_answer + 4, 0); break;   // bit 16: development mode
    case 0x0103: memcpy(mb_answer + 4, "-abc-abc12345678", 16); break;   // serial number, as MAME's
    default: hle_log("media board: command 0x%04x not handled\n", cmd); break;
    }
    if (hle_verbose > 1) hle_log("media board: command 0x%04x\n", cmd);
    mb_command[0] = mb_command[1] = mb_command[2] = mb_command[3] = 0;
    ke_raise_interrupt(10);
}

void mediaboard_read(uint64_t at, uint8_t *buf, uint32_t len)
{
    pthread_mutex_lock(&mb_lock);
    for (uint32_t i = 0; i < len; i++) {
        uint64_t sector = (at + i) / 512, o = (at + i) % 512;
        buf[i] = sector == MB_ANSWER ? mb_answer[o] : sector == MB_COMMAND ? mb_command[o] : 0;
    }
    pthread_mutex_unlock(&mb_lock);
    if (hle_verbose > 1 && at / 512 != MB_ANSWER) hle_log("media board: read 0x%x at 0x%llx\n", len, (unsigned long long)at);
}

void mediaboard_write(uint64_t at, const uint8_t *buf, uint32_t len)
{
    pthread_mutex_lock(&mb_lock);
    int command = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint64_t sector = (at + i) / 512, o = (at + i) % 512;
        if (sector == MB_ANSWER) mb_answer[o] = buf[i];
        else if (sector == MB_COMMAND) mb_command[o] = buf[i], command = 1;
    }
    if (command) mb_run();
    pthread_mutex_unlock(&mb_lock);
    if (hle_verbose > 1 && !command && at / 512 != MB_ANSWER) hle_log("media board: write 0x%x at 0x%llx\n", len, (unsigned long long)at);
}

// back to power-on state (the cabinet's controls stay as the front end set them)
void baseboard_reset(void)
{
    memset(eeprom, 0, sizeof eeprom);
    memset(qc_xmem, 0, sizeof qc_xmem);
    jvs_address = 0xff;
    jvs_reset_count = 0;
    memset(&jvs, 0, sizeof jvs);
    sc_midi_rs232 = 0;
    memset(sc_response, 0, sizeof sc_response);
    memset(sc_packet, 0, sizeof sc_packet);
    sc_response_size = sc_step = 0;
    memset(mb_answer, 0, sizeof mb_answer);
    memset(mb_command, 0, sizeof mb_command);
    jvs_inputs.outputs = 0;
}

void baseboard_init(void)
{
    load_eeproms();
    load_saved_eeprom();
    load_xmem();
    usb_device_reset(&qc);
    usb_device_reset(&sc);
    ohci_plug(0, &qc);
    ohci_plug(1, &sc);
}
