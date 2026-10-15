/* Host tests for devupd/du.c (docs/35): framing, path allow-list, session + staging + manifest, the guarded flasher
 * on a fake NAND (te-boot blocks 0..3 and the YAFFS2 area must stay byte-identical in every scenario).
 *   cc -O1 -I.. devupd_test.c ../devupd/du.c -o build/devupd_test
 *   build/devupd_test [session.bin tree]      (session.bin from tools/op1push.py --dump: replayed and checked) */
#include "../devupd/du.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- in-memory file system ---- */
#define NF 80
static struct { char name[128]; uint8_t *data; uint32_t size; int used; } fsf[NF];
static int cur = -1, key_ok, reboots;
static int find(const char *n) { for (int i = 0; i < NF; i++) if (fsf[i].used && !strcmp(fsf[i].name, n)) return i; return -1; }
static int f_open(void *c, const char *n)
{
    (void)c; assert(!strstr(n, "te-boot"));
    int i = find(n);
    if (i < 0) for (i = 0; i < NF && fsf[i].used; i++) ;
    assert(i < NF);
    snprintf(fsf[i].name, sizeof fsf[i].name, "%s", n); free(fsf[i].data); fsf[i].data = 0; fsf[i].size = 0; fsf[i].used = 1;
    cur = i; return 0;
}
static int f_write(void *c, const uint8_t *p, uint32_t n)
{
    (void)c; assert(cur >= 0);
    fsf[cur].data = realloc(fsf[cur].data, fsf[cur].size + n);
    memcpy(fsf[cur].data + fsf[cur].size, p, n); fsf[cur].size += n; return 0;
}
static int f_close(void *c) { (void)c; cur = -1; return 0; }
static int f_remove(void *c, const char *n) { (void)c; int i = find(n); if (i >= 0) { free(fsf[i].data); fsf[i].data = 0; fsf[i].used = 0; } return 0; }
static int f_key(void *c) { (void)c; return key_ok; }
static void f_reboot(void *c) { (void)c; reboots++; }
static const du_fs fs = { f_open, f_write, f_close, f_remove, f_key, f_reboot, 0 };
static int nfiles(void) { int n = 0; for (int i = 0; i < NF; i++) n += fsf[i].used; return n; }

static uint8_t rep[DU_MAX_MSG];
static int send(du_session *s, int cmd, int seq, const uint8_t *pl, int n, int want)
{
    uint8_t m[DU_MAX_MSG];
    int len = du_build(m, cmd, seq, pl, n);
    assert(len > 0);
    int r = du_handle(s, m, len, rep);
    assert(r > 0 && rep[5] == (cmd | DU_REPLY) && (rep[6] | rep[7] << 7) == seq);
    uint8_t st[16]; du_unpack7(rep + 8, r - 9, st);
    if (st[0] != want) { printf("cmd %d: status %d, want %d\n", cmd, st[0], want); exit(1); }
    return st[0];
}
static int begin(du_session *s, const char *path, const uint8_t *d, uint32_t n, int want)
{
    uint8_t pl[128]; uint32_t c = n <= 5000 ? du_crc32(0, d, n) : 0;   /* (oversize cases: no data) */
    memcpy(pl, &n, 4); memcpy(pl + 4, &c, 4); strcpy((char *)pl + 8, path);
    return send(s, DU_BEGIN, 1, pl, 8 + (int)strlen(path) + 1, want);
}
static void put_file(du_session *s, const char *path, const uint8_t *d, uint32_t n)
{
    begin(s, path, d, n, DU_OK);
    for (uint32_t o = 0; o < n; o += DU_MAX_DATA) {
        uint8_t pl[4 + DU_MAX_DATA]; uint32_t k = n - o < DU_MAX_DATA ? n - o : DU_MAX_DATA;
        memcpy(pl, &o, 4); memcpy(pl + 4, d + o, k);
        send(s, DU_DATA, 2, pl, 4 + (int)k, DU_OK);
    }
    send(s, DU_END, 3, 0, 0, DU_OK);
}

/* ---- fake NAND: boot area + some YAFFS2 blocks; any call outside 4..131 fails the test ---- */
#define NB 140
static uint8_t *nand[NB]; static int bad[NB], fail_write = -1, corrupt_write = -1, calls_outside;
static int n_is_bad(void *c, int b) { (void)c; if (!du_guard(b)) calls_outside++; return bad[b]; }
static int n_erase(void *c, int b) { (void)c; if (!du_guard(b)) { calls_outside++; return -1; } memset(nand[b], 0xFF, DU_BLOCK); return 0; }
static int n_write(void *c, int b, const uint8_t *p)
{
    (void)c; if (!du_guard(b)) { calls_outside++; return -1; }
    if (b == fail_write) return -1;
    memcpy(nand[b], p, DU_BLOCK);
    if (b == corrupt_write) nand[b][1000] ^= 1;
    return 0;
}
static int n_read(void *c, int b, uint8_t *p) { (void)c; if (!du_guard(b)) { calls_outside++; return -1; } memcpy(p, nand[b], DU_BLOCK); return 0; }
static const du_nand nd = { n_is_bad, n_erase, n_write, n_read, 0 };
static uint8_t *ref_teboot, *ref_yaffs;
static void nand_reset(void)
{
    for (int b = 0; b < NB; b++) { if (!nand[b]) nand[b] = malloc(DU_BLOCK); memset(nand[b], (uint8_t)(0x30 + b), DU_BLOCK); bad[b] = 0; }
    fail_write = corrupt_write = -1; calls_outside = 0;
}
static void check_untouched(void)
{
    for (int b = 0; b < 4; b++) assert(!memcmp(nand[b], ref_teboot + b * DU_BLOCK, DU_BLOCK));
    for (int b = 132; b < NB; b++) assert(!memcmp(nand[b], ref_yaffs + (b - 132) * DU_BLOCK, DU_BLOCK));
    assert(calls_outside == 0);
}
static int boot_area_unchanged(void) { for (int b = 4; b <= DU_LAST_BLOCK; b++) for (int k = 0; k < DU_BLOCK; k++) if (nand[b][k] != (uint8_t)(0x30 + b)) return 0; return 1; }

/* synthetic LDR of exactly size bytes: a valid block chain, block 0 = the test fingerprint block */
static du_fp test_fp;
static void make_ldr(uint8_t *l, uint32_t size, int seed)
{
    uint32_t o = 0, addr = 0x01000000;
    for (int b = 0; o < size; b++) {
        uint32_t rem = size - o - 16, count = b == 0 ? 256 : rem < 60000 ? rem : 60000;
        if (rem - count > 0 && rem - count < 16) count -= 16;
        uint8_t *h = l + o, x = 0;
        uint32_t a = b == 0 ? 0xFF901000u : addr;
        memset(h, 0, 16); h[0] = 6; h[1] = b == 0 ? 0x24 : 0; h[3] = 0xAD;
        memcpy(h + 4, &a, 4); memcpy(h + 8, &count, 4);
        for (int i = 0; i < 16; i++) x ^= h[i];
        h[2] = x;
        for (uint32_t i = 0; i < count; i++) l[o + 16 + i] = (uint8_t)((o + i) * 13 + (uint32_t)seed);
        if (b == 0) { test_fp.addr = a; test_fp.count = count; test_fp.crc = du_crc32(0, l + o + 16, count); }
        o += 16 + count; addr += count;
    }
}

int main(int argc, char **argv)
{
    /* framing */
    assert(du_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    srand(1);
    for (int n = 0; n < 200; n++) {
        uint8_t a[200], p[240], b[200];
        for (int i = 0; i < n; i++) a[i] = (uint8_t)rand();
        int m = du_pack7(a, n, p);
        for (int i = 0; i < m; i++) assert(p[i] < 0x80);
        assert(du_unpack7(p, m, b) == n && !memcmp(a, b, (size_t)n));
    }
    uint8_t big[4 + DU_MAX_DATA] = { 0 }, msg[DU_MAX_MSG];
    assert(du_build(msg, DU_DATA, 1, big, sizeof big) > 0 && du_build(msg, DU_DATA, 1, big, 300) < 0);
    /* paths */
    assert(du_path_ok("OP1_vdk.ldr") && du_path_ok("content/display/a.svg") && du_path_ok("content/op1_factory.db"));
    const char *badp[] = { "te-boot.ldr", "content/te-boot.op1", "content/TE-BOOT.op1", "content/te_boot.ldr", "/content/x",
                           "content/../te-boot.ldr", "OP1_vdk.ldr.x", "x/OP1_vdk.ldr", "content/", "", "content//x", "content/a b" };
    for (unsigned i = 0; i < sizeof badp / sizeof *badp; i++) assert(!du_path_ok(badp[i]));
    printf("framing, CRC, paths ok\n");

    /* session */
    du_session s; du_init(&s, &fs);
    uint8_t data[5000]; for (int i = 0; i < 5000; i++) data[i] = (uint8_t)(i * 7);
    send(&s, DU_HELLO, 0, 0, 0, DU_OK);
    begin(&s, "te-boot.ldr", data, 10, DU_E_PATH);
    begin(&s, "content/te-boot.op1", data, 10, DU_E_PATH);
    begin(&s, "OP1_vdk.ldr", data, 128u * DU_BLOCK, DU_E_SIZE);
    send(&s, DU_COMMIT, 4, 0, 0, DU_E_STATE);                       /* nothing staged */
    put_file(&s, "content/display/a.svg", data, 5000);
    begin(&s, "content/display/a.svg", data, 5000, DU_E_STATE);    /* same path twice */
    begin(&s, "OP1_vdk.ldr", data, 1000, DU_OK);
    { uint8_t pl[8] = { 10, 0, 0, 0, 1, 2, 3, 4 }; send(&s, DU_DATA, 5, pl, 8, DU_E_OFFSET); }   /* out of order */
    { uint8_t pl[4 + 100]; uint32_t o = 0; memcpy(pl, &o, 4); memcpy(pl + 4, data, 100); pl[50] ^= 1;   /* corrupted */
      send(&s, DU_DATA, 6, pl, 104, DU_OK); }
    for (uint32_t o = 100; o < 1000; o += 100) { uint8_t pl[104]; memcpy(pl, &o, 4); memcpy(pl + 4, data + o, 100); send(&s, DU_DATA, 7, pl, 104, DU_OK); }
    send(&s, DU_END, 8, 0, 0, DU_E_CRC);                           /* CRC catches it, staged file removed */
    put_file(&s, "OP1_vdk.ldr", data, 1000);
    key_ok = 0; send(&s, DU_COMMIT, 9, 0, 0, DU_WAIT_KEY); assert(reboots == 0);
    key_ok = 1; send(&s, DU_COMMIT, 10, 0, 0, DU_OK); assert(reboots == 1);
    int mi = find(DU_MANIFEST); assert(mi >= 0);
    char text[4096]; memcpy(text, fsf[mi].data, fsf[mi].size); text[fsf[mi].size] = 0;
    du_file mf[DU_MAX_FILES]; int mn;
    assert(du_manifest_parse(text, mf, &mn) == 0 && mn == 2 && !strcmp(mf[0].path, "content/display/a.svg") &&
           mf[1].size == 1000 && mf[1].crc == du_crc32(0, data, 1000));
    char name[40]; du_stage_name(1, name); assert(find(name) >= 0 && fsf[find(name)].size == 1000);
    text[20] ^= 1; assert(du_manifest_parse(text, mf, &mn) != 0);  /* any change breaks the manifest checksum */
    send(&s, DU_ABORT, 11, 0, 0, DU_OK); assert(nfiles() == 0);
    printf("session: refusals, offsets, CRC, key prompt, manifest, abort ok\n");

    /* replay a session from tools/op1push.py */
    if (argc == 3) {
        FILE *f = fopen(argv[1], "rb"); assert(f);
        du_init(&s, &fs); key_ok = 1; int msgs = 0;
        uint16_t len;
        while (fread(&len, 2, 1, f) == 1) {
            uint8_t m[DU_MAX_MSG]; assert(len <= DU_MAX_MSG && fread(m, 1, len, f) == len);
            int r = du_handle(&s, m, len, rep); assert(r > 0);
            uint8_t st[16]; du_unpack7(rep + 8, r - 9, st); assert(st[0] == DU_OK);
            msgs++;
        }
        fclose(f);
        mi = find(DU_MANIFEST); assert(mi >= 0);
        memcpy(text, fsf[mi].data, fsf[mi].size); text[fsf[mi].size] = 0;
        assert(du_manifest_parse(text, mf, &mn) == 0);
        for (int i = 0; i < mn; i++) {
            char p[512]; snprintf(p, sizeof p, "%s/%s", argv[2], mf[i].path);
            FILE *g = fopen(p, "rb"); assert(g);
            uint8_t *b = malloc(mf[i].size + 1); assert(fread(b, 1, mf[i].size + 1, g) == mf[i].size); fclose(g);
            du_stage_name(i, name); int k = find(name);
            assert(k >= 0 && fsf[k].size == mf[i].size && !memcmp(fsf[k].data, b, mf[i].size));
            free(b);
        }
        printf("op1push session replayed: %d messages, %d files staged byte-identical\n", msgs, mn);
    }

    /* flasher */
    nand_reset();
    ref_teboot = malloc(4 * DU_BLOCK); ref_yaffs = malloc((NB - 132) * DU_BLOCK);
    for (int b = 0; b < 4; b++) memcpy(ref_teboot + b * DU_BLOCK, nand[b], DU_BLOCK);
    for (int b = 132; b < NB; b++) memcpy(ref_yaffs + (b - 132) * DU_BLOCK, nand[b], DU_BLOCK);
    uint32_t lsize = 2355612; uint8_t *ldr = malloc(127u * DU_BLOCK), *buf = malloc(DU_BLOCK);
    make_ldr(ldr, lsize, 0);
    const du_fp *fp = &test_fp;
    assert(du_ldr_ok(ldr, lsize) && du_ldr_check(ldr, lsize, fp, 1) == DU_F_OK);
    assert(du_ldr_check(ldr, lsize, du_r246_fp, du_r246_fp_n) == DU_F_NOTOP1);   /* synthetic: not R246 */
    assert(!du_ldr_ok(ldr, lsize - 5) && !du_ldr_ok(ldr, lsize + 16) && !du_ldr_ok(ldr, 15));
    int nb = (int)((lsize + DU_BLOCK - 1) / DU_BLOCK);
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_OK); check_untouched();
    for (int i = 0; i < nb; i++) assert(!memcmp(nand[4 + i], ldr + (uint32_t)i * DU_BLOCK, i < nb - 1 ? DU_BLOCK : lsize - (uint32_t)i * DU_BLOCK));
    printf("flash: %u bytes -> blocks 4..%d, te-boot and YAFFS2 untouched\n", lsize, 3 + nb);
    nand_reset(); bad[5] = bad[9] = 1;                              /* bad blocks are skipped */
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_OK); check_untouched();
    assert(!memcmp(nand[4], ldr, DU_BLOCK) && !memcmp(nand[6], ldr + DU_BLOCK, DU_BLOCK) && !memcmp(nand[10], ldr + 4u * DU_BLOCK, DU_BLOCK));
    nand_reset(); uint8_t keep = ldr[3]; ldr[3] = 0;                  /* broken header: nothing touched */
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_BADLDR && boot_area_unchanged()); check_untouched(); ldr[3] = keep;
    nand_reset(); for (int b = 4; b <= 120; b++) bad[b] = 1;        /* not enough good blocks: nothing touched */
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_NOSPACE && boot_area_unchanged()); check_untouched();
    nand_reset(); fail_write = 7;
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_WRITE); check_untouched();
    nand_reset(); corrupt_write = 8;
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_VERIFY); check_untouched();
    /* image checks: every case fails before the boot area is touched */
    nand_reset();                                                   /* truncated (CRC of the cut file would match) */
    assert(du_flash_ldr(&nd, ldr, lsize - 1000, buf, fp, 1) == DU_F_BADLDR && boot_area_unchanged()); check_untouched();
    nand_reset(); ldr[272 + 60016 + 9] ^= 0x40;                /* third header's count: chain broken */
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_BADLDR && boot_area_unchanged()); check_untouched();
    ldr[272 + 60016 + 9] ^= 0x40;
    nand_reset(); ldr[16 + 100] ^= 1;                               /* fingerprint block changed */
    assert(du_flash_ldr(&nd, ldr, lsize, buf, fp, 1) == DU_F_NOTOP1 && boot_area_unchanged()); check_untouched();
    ldr[16 + 100] ^= 1;
    nand_reset();                                                   /* valid LDR, but not this firmware (no R246 blocks) */
    assert(du_flash_ldr(&nd, ldr, lsize, buf, du_r246_fp, du_r246_fp_n) == DU_F_NOTOP1 && boot_area_unchanged()); check_untouched();
    printf("checks: truncated, broken chain, changed or missing fingerprint blocks refused, boot area untouched\n");
    nand_reset();                                                   /* full last block: one extra 0xFF block (te-boot) */
    uint32_t full = 3u * DU_BLOCK - 10;
    {
        uint8_t *l2 = malloc(full);
        make_ldr(l2, full, 5);
        assert(du_flash_ldr(&nd, l2, full, buf, &test_fp, 1) == DU_F_OK); check_untouched();
        for (int k = 0; k < DU_BLOCK; k++) assert(nand[7][k] == 0xFF);
        free(l2);
    }
    if (getenv("DU_R246_LDRS")) {                                   /* real OP1_vdk.ldr files (R246 and builds from it) */
        char list[4096]; snprintf(list, sizeof list, "%s", getenv("DU_R246_LDRS"));
        for (char *p = strtok(list, ":"); p; p = strtok(0, ":")) {
            FILE *g = fopen(p, "rb"); assert(g);
            uint32_t n = (uint32_t)fread(ldr, 1, 127u * DU_BLOCK, g); fclose(g);
            int c = du_ldr_check(ldr, n, du_r246_fp, du_r246_fp_n);
            printf("  %s: %u bytes, check %d (%s)\n", p, n, c, c == DU_F_OK ? "R246 OP1_vdk" : c == DU_F_NOTOP1 ? "not R246" : "bad LDR");
        }
    }
    assert(!du_guard(0) && !du_guard(3) && du_guard(4) && du_guard(131) && !du_guard(132) && !du_guard(-1));
    printf("flash: bad blocks, broken LDR, no space, write / verify errors, end block: te-boot never touched\n");
    free(ldr); free(buf); free(ref_teboot); free(ref_yaffs);
    for (int b = 0; b < NB; b++) free(nand[b]);
    for (int i = 0; i < NF; i++) free(fsf[i].data);
    printf("OK: devupd\n");
    return 0;
}
