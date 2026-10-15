/* Development update over SysEx (docs/35): protocol, staging + manifest, guarded boot-area flasher.
 * Pure C: file access and NAND access go through callbacks, so the same code runs in the host tests (in-memory
 * file system, fake NAND) and later in the firmware (YAFFS2 API, nand.cpp primitives).
 * Hard rule: NAND blocks 0..3 (te-boot) are never erased or written; every NAND call passes du_guard(). */
#ifndef DU_H
#define DU_H
#include <stdint.h>

/* ---- SysEx framing: F0 00 20 76 7D <cmd> <seq lo> <seq hi> <7-bit packed payload> F7 ---- */
#define DU_SYSEX_HDR      5                 /* F0 00 20 76 7D */
#define DU_MAX_MSG        256               /* the firmware's SysEx buffer has no length check: stay well below */
#define DU_MAX_DATA       192               /* raw bytes per DATA message (offset 4 + data packed -> <= 230 bytes) */
enum { DU_HELLO = 1, DU_BEGIN = 2, DU_DATA = 3, DU_END = 4, DU_COMMIT = 5, DU_ABORT = 6 };
enum { DU_REPLY = 0x40 };                   /* reply: cmd | 0x40, seq, status, payload */
enum { DU_OK = 0, DU_E_FORMAT = 1, DU_E_STATE = 2, DU_E_PATH = 3, DU_E_SIZE = 4, DU_E_OFFSET = 5, DU_E_CRC = 6,
       DU_E_IO = 7, DU_E_FULL = 8, DU_WAIT_KEY = 9 };

uint32_t du_crc32(uint32_t crc, const uint8_t *p, uint32_t n);    /* crc = 0 to start (zlib CRC-32) */
int du_pack7(const uint8_t *in, int n, uint8_t *out);             /* 7 bytes -> 8 (MSBs first); returns length */
int du_unpack7(const uint8_t *in, int n, uint8_t *out);           /* inverse; -1 if a byte has bit 7 set */
/* build a message (host / replies); returns its length or -1 if it would exceed DU_MAX_MSG */
int du_build(uint8_t *msg, int cmd, int seq, const uint8_t *payload, int n);
int du_path_ok(const char *path);                                 /* allow-list: OP1_vdk.ldr, content/...; never te-boot */

/* ---- device side session ---- */
#define DU_MAX_FILES 64
#define DU_MAX_PATH  96
typedef struct {                            /* file system callbacks (firmware: YAFFS2) */
    int (*open_w)(void *ctx, const char *path);                   /* create / truncate; 0 ok */
    int (*write)(void *ctx, const uint8_t *p, uint32_t n);         /* append to the open file; 0 ok */
    int (*close)(void *ctx);
    int (*remove)(void *ctx, const char *path);
    int (*key_pressed)(void *ctx);                                /* the user confirmed the commit on the device */
    void (*reboot)(void *ctx);
    void *ctx;
} du_fs;
typedef struct {
    char path[DU_MAX_PATH];
    uint32_t size, crc;
} du_file;
typedef struct {
    const du_fs *fs;
    int open;                               /* a file is being received */
    uint32_t written, crc;                  /* of the open file */
    int n;                                  /* files completed (staged) */
    du_file f[DU_MAX_FILES + 1];
} du_session;
#define DU_STAGE_DIR  "/yaffs2/op1r_stage"
#define DU_MANIFEST   "/yaffs2/op1r_stage/manifest"

void du_init(du_session *s, const du_fs *fs);
void du_abort(du_session *s);                                     /* drop the session + whole staging dir */
/* one received SysEx message (with F0 / F7); writes the reply into rep, returns its length (0: not ours) */
int du_handle(du_session *s, const uint8_t *msg, int n, uint8_t *rep);
/* manifest text for the staged files (written on COMMIT); parse: 0 ok, files into f[], count into *n */
int du_manifest_write(const du_session *s, char *out, int cap);
int du_manifest_parse(const char *text, du_file *f, int *n);
void du_stage_name(int i, char *out);                             /* /yaffs2/op1r_stage/fNN */

/* ---- guarded boot-area flasher ---- */
#define DU_BLOCK        0x20000             /* 128 KB per boot block (64 x 2 KB pages) */
#define DU_FIRST_BLOCK  4                   /* main firmware starts here; 0..3 = te-boot, NEVER touched */
#define DU_LAST_BLOCK   131                 /* last block of the boot area (YAFFS2 from 132) */
typedef struct {                            /* NAND callbacks, physical block numbers (firmware: nand.cpp) */
    int (*is_bad)(void *ctx, int block);                          /* factory / runtime bad block */
    int (*erase)(void *ctx, int block);                           /* 0 ok */
    int (*write)(void *ctx, int block, const uint8_t *data);       /* one 128 KB block; 0 ok */
    int (*read)(void *ctx, int block, uint8_t *data);              /* 0 ok */
    void *ctx;
} du_nand;
int du_guard(int block);                    /* 1 if block may be touched (DU_FIRST_BLOCK..DU_LAST_BLOCK) */
int du_ldr_ok(const uint8_t *ldr, uint32_t size);                 /* every LDR block header valid, chain ends at size */
typedef struct { uint32_t addr, count, crc; } du_fp;              /* a block that must be present unchanged */
extern const du_fp du_r246_fp[];                                  /* encrypted R246 OP1_vdk blocks no build changes */
extern const int du_r246_fp_n;
int du_ldr_has(const uint8_t *ldr, uint32_t size, const du_fp *fp, int n);   /* 1 if all n blocks are in ldr */
enum { DU_F_OK = 0, DU_F_BADLDR = 1, DU_F_NOSPACE = 2, DU_F_GUARD = 3, DU_F_ERASE = 4, DU_F_WRITE = 5, DU_F_VERIFY = 6,
       DU_F_NOTOP1 = 7 };
/* all image checks, done before anything is changed: DU_F_OK, DU_F_BADLDR (structure / size) or DU_F_NOTOP1 (not an
 * OP1_vdk.ldr built from R246: te-boot, another OS, another device's firmware) */
int du_ldr_check(const uint8_t *ldr, uint32_t size, const du_fp *fp, int n);
int du_blocks_needed(uint32_t size);                              /* data blocks + one 0xFF end block if the last is full */
/* host model of the flasher (tests): ldr into the boot area from block 4, skipping bad blocks; buf: 128 KB scratch.
 * Checks the image and plans the blocks before touching. The firmware (du_fw.c) has to erase 4..131 with the stock
 * routine first (it marks failing blocks bad), so it checks the image before the erase and the plan right after it. */
int du_flash_ldr(const du_nand *nand, const uint8_t *ldr, uint32_t size, uint8_t *buf, const du_fp *fp, int n);
#endif
