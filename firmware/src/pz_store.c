/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* Flash storage (SPI NOR, 4 KiB sectors), power-fail safe.
 *
 *   0x97000 .. 0xC7000   16 pattern slots, 3 sectors each
 *   0xC7000 .. 0xCA000   the journal slot (a pattern on its way to its slot)
 *   0xCA000 / 0xCB000    the kits (A / B copies, the newer valid one wins)
 *   0xCC000 / 0xCD000    the system settings (A / B)
 * All inside the application's data window (FL_DATA, fm1_flash.h); the update staging area
 * (0xE0000..) and the firmware are never touched.
 *
 * Every object is a 32-byte header (the commit record, written last) and its payload from
 * offset 256. A pattern is written to the journal first, then to its slot, then the journal is
 * invalidated; at power-on a valid journal is copied to its slot again: a write cut at any
 * point leaves either the old or the new pattern. Flash is only written while stopped (an erase
 * stops the audio for ~40 ms). */
#define FS_PAT0 0x97000u
#define FS_SLOT 0x3000u
#define FS_JOURNAL (FS_PAT0 + PZ_NPAT * FS_SLOT)
#define FS_KITS 0xCA000u
#define FS_SYS 0xCC000u
#define FS_MAGIC 0x5A4C5350u                  /* "PSLZ" */
#define FS_PAYLOAD 256u
enum { OBJ_PAT = 1, OBJ_KITS, OBJ_SYS };
_Static_assert(sizeof(pattern_t) + FS_PAYLOAD <= FS_SLOT, "a pattern fits its slot");
_Static_assert(sizeof(kit_t) * PZ_NKIT + FS_PAYLOAD <= 0x1000u, "the kits fit a sector");
_Static_assert(FS_JOURNAL + FS_SLOT <= FS_KITS, "flash map");

typedef struct {
    uint32_t magic;
    uint16_t kind, slot;
    uint32_t seq, len, crc, ver, rsv;
    uint32_t hcrc;
} fs_hdr_t;

static uint32_t fs_seq;

static uint32_t crc32(const void *p, uint32_t n)
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}

static int fs_hdr_read(uint32_t off, fs_hdr_t *h)
{
    if (plat_flash_read(off, h, sizeof *h))
        return -1;
    if (h->magic != FS_MAGIC || h->hcrc != crc32(h, sizeof *h - 4u))
        return -1;
    if (h->seq > fs_seq)
        fs_seq = h->seq;
    return 0;
}

/* erase n sectors, write the payload, then the header */
static int fs_put(uint32_t off, uint32_t sectors, uint32_t kind, uint32_t slot, const void *p, uint32_t len, uint32_t seq)
{
    fs_hdr_t h;
    uint32_t i;
    for (i = 0; i < sectors; i++)
        if (plat_flash_erase(off + i * 0x1000u))
            return -1;
    if (plat_flash_prog(off + FS_PAYLOAD, p, len))
        return -2;
    h.magic = FS_MAGIC;
    h.kind = (uint16_t)kind;
    h.slot = (uint16_t)slot;
    h.seq = seq;
    h.len = len;
    h.crc = crc32(p, len);
    h.ver = 1;
    h.rsv = 0;
    h.hcrc = crc32(&h, sizeof h - 4u);
    if (plat_flash_prog(off, &h, sizeof h))
        return -3;
    return 0;
}

/* read a payload into dst and check it; -1 if missing or damaged (dst is then undefined) */
static int fs_get(uint32_t off, uint32_t kind, void *dst, uint32_t len, fs_hdr_t *hp)
{
    fs_hdr_t h;
    if (fs_hdr_read(off, &h) || h.kind != kind || h.len != len)
        return -1;
    if (plat_flash_read(off + FS_PAYLOAD, dst, len) || crc32(dst, len) != h.crc)
        return -1;
    if (hp)
        *hp = h;
    return 0;
}

static int store_pattern(uint32_t n)
{
    int rc;
    pattern_t *p = &pats[n];
    uint32_t seq = ++fs_seq;
    p->magic = PAT_MAGIC;
    rc = fs_put(FS_JOURNAL, 3, OBJ_PAT, n, p, sizeof *p, seq);
    if (rc)
        return rc;
    rc = fs_put(FS_PAT0 + n * FS_SLOT, 3, OBJ_PAT, n, p, sizeof *p, seq);
    plat_flash_erase(FS_JOURNAL);              /* the journal's work is done */
    return rc;
}

static int store_kit(uint32_t n)               /* all kits go in one object (2 KiB) */
{
    fs_hdr_t a, b;
    int va, vb;
    uint32_t off;
    (void)n;
    va = fs_hdr_read(FS_KITS, &a) == 0;
    vb = fs_hdr_read(FS_KITS + 0x1000u, &b) == 0;
    off = !va ? FS_KITS : !vb ? FS_KITS + 0x1000u : a.seq < b.seq ? FS_KITS : FS_KITS + 0x1000u;
    return fs_put(off, 1, OBJ_KITS, 0, kits, sizeof kits, ++fs_seq);
}

static int store_sys(void)
{
    fs_hdr_t a, b;
    int va, vb;
    uint32_t off;
    sys_t cur;
    if (fs_get(FS_SYS, OBJ_SYS, &cur, sizeof cur, &a) == 0 || fs_get(FS_SYS + 0x1000u, OBJ_SYS, &cur, sizeof cur, &a) == 0) {
        const uint8_t *x = (const uint8_t *)&cur, *y = (const uint8_t *)&sys;
        uint32_t i, same = 1;
        for (i = 0; i < sizeof cur; i++)
            same &= x[i] == y[i];
        if (same)
            return 0;                          /* unchanged: no erase */
    }
    va = fs_hdr_read(FS_SYS, &a) == 0;
    vb = fs_hdr_read(FS_SYS + 0x1000u, &b) == 0;
    off = !va ? FS_SYS : !vb ? FS_SYS + 0x1000u : a.seq < b.seq ? FS_SYS : FS_SYS + 0x1000u;
    return fs_put(off, 1, OBJ_SYS, 0, &sys, sizeof sys, ++fs_seq);
}

/* power-on: finish an interrupted pattern write, then load everything (missing = factory) */
static void store_load(void)
{
    fs_hdr_t h, a, b;
    uint32_t i;
    int va, vb;
    /* the sequence numbers first: every header */
    for (i = 0; i < PZ_NPAT; i++)
        fs_hdr_read(FS_PAT0 + i * FS_SLOT, &h);
    /* the journal */
    if (fs_get(FS_JOURNAL, OBJ_PAT, &pats[0], sizeof pats[0], &h) == 0 && h.slot < PZ_NPAT) {
        fs_hdr_t s;
        if (fs_hdr_read(FS_PAT0 + h.slot * FS_SLOT, &s) || s.seq != h.seq)
            fs_put(FS_PAT0 + h.slot * FS_SLOT, 3, OBJ_PAT, h.slot, &pats[0], sizeof pats[0], h.seq);
        plat_flash_erase(FS_JOURNAL);
    }
    for (i = 0; i < PZ_NPAT; i++)
        if (fs_get(FS_PAT0 + i * FS_SLOT, OBJ_PAT, &pats[i], sizeof pats[i], 0) || pats[i].magic != PAT_MAGIC)
            pat_init(&pats[i], i);
    /* kits: the newer valid copy */
    va = fs_get(FS_KITS, OBJ_KITS, kits, sizeof kits, &a) == 0;
    vb = fs_get(FS_KITS + 0x1000u, OBJ_KITS, kits, sizeof kits, &b) == 0;
    if (va && (!vb || a.seq > b.seq))
        va = fs_get(FS_KITS, OBJ_KITS, kits, sizeof kits, &a) == 0;
    if (!va && !vb)
        for (i = 0; i < PZ_NKIT; i++)
            kit_factory(&kits[i], i);
    for (i = 0; i < PZ_NKIT; i++)
        if (kits[i].magic != KIT_MAGIC)
            kit_factory(&kits[i], i);
    /* the system settings */
    {
        sys_t s0, s1;
        va = fs_get(FS_SYS, OBJ_SYS, &s0, sizeof s0, &a) == 0;
        vb = fs_get(FS_SYS + 0x1000u, OBJ_SYS, &s1, sizeof s1, &b) == 0;
        if (va && (!vb || a.seq > b.seq))
            sys = s0;
        else if (vb)
            sys = s1;
        sys.pat %= PZ_NPAT;
        sys.kitno %= PZ_NKIT;
        sys.theme &= 1u;
        sys.sync %= 3u;
        sys.midi_ch &= 15u;
        if (sys.tempo10 < 400u || sys.tempo10 > 3000u)
            sys.tempo10 = 1200;
    }
}
