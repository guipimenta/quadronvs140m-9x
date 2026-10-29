#!/usr/bin/env python3
"""Reproduce stale zero uploads using nvshim.c's actual map/flush functions.

No GPU, Wine, or Windows headers are required. Only the transport is replaced
with a byte array; the BO layout, flags, map and flush code come from the tree.
The reused physical range deliberately contains data from a previous upload.

Run: python3 tools/repro_nvshim_zero_upload.py
Exit 1 means that a CPU upload did not reach simulated VRAM. A passing run
does not validate rendering, tiling, synchronization, or the real transport.
"""

from pathlib import Path
import subprocess
import tempfile


DRIVER = Path(__file__).resolve().parents[1] / "driver" / "vmdisp9x-nv"


def source_between(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    source = (DRIVER / "nvshim.c").read_text()
    bo = source_between(source, "struct shim_bo {", "\n#define SHIM_MAXBO")
    mapping = source_between(source, "int nouveau_bo_map(", "\nvoid nouveau_bo_ref(")
    flushing = source_between(
        source, "#define SHIM_FLUSH_CHUNK", "\n/* 3097: NVSHIM_VERIFYVRAM - after"
    )
    harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NVSHIM_STANDALONE
#include "nvshim.h"
static uint32_t g_kgen, g_st_flush_dw;
static uint64_t g_st_kc[5];
static unsigned char vram[3072];
static int fullflush;
static uint64_t shim_tsc(void) { return 0; }
static int shim_fullflush(void) { return fullflush; }
static void vxd_vram_read(uint32_t off, uint32_t *dst, uint32_t n)
{ memcpy(dst, vram + off, n * 4); }
static void vxd_vram_write(uint32_t off, const uint32_t *src, uint32_t n)
{ memcpy(vram + off, src, n * 4); }
'''
    harness += bo + r'''
static void shim_kick_bo(struct shim_bo *b, int src) { (void)b; (void)src; }
'''
    harness += mapping + flushing + r'''
static int run_case(const char *label, int full, uint32_t access, int zeros)
{
    struct shim_bo old = {0}, fresh = {0};
    unsigned i, bad = 0;
    fullflush = full;
    old.base.size = sizeof(vram);
    if (nouveau_bo_map(&old.base, NOUVEAU_BO_WR, NULL)) return 1;
    memset(old.base.map, 0xa5, sizeof(vram));
    shim_bo_flush(&old);
    free(old.base.map); free(old.clean);

    /* Model a free/reallocation: a new BO gets the same physical range.
     * The transport retains old bytes, just as VRAM does between BOs. */
    fresh.base.size = sizeof(vram);
    if (nouveau_bo_map(&fresh.base, access, NULL)) return 1;
    memset(fresh.base.map, zeros ? 0 : 0x7e, sizeof(vram));
    memset((char *)fresh.base.map + 1024, 0x3c, 1024);
    shim_bo_flush(&fresh);
    for (i = 0; i < sizeof(vram); i++)
        if (vram[i] != ((unsigned char *)fresh.base.map)[i]) bad++;
    printf("%s: stale bytes=%u/3072; shadow equals clean=%d\n", label, bad,
           memcmp(fresh.base.map, fresh.clean, sizeof(vram)) == 0);
    free(fresh.base.map); free(fresh.clean);
    return bad != 0;
}
int main(void)
{
    int bad = 0;
    bad |= run_case("WR, zero chunks", 0, NOUVEAU_BO_WR, 1);
    bad |= run_case("WR, zero chunks, FULLFLUSH", 1, NOUVEAU_BO_WR, 1);
    bad |= run_case("RDWR baseline control", 0, NOUVEAU_BO_RDWR, 1);
    bad |= run_case("WR, nonzero control", 0, NOUVEAU_BO_WR, 0);
    return bad;
}
'''
    with tempfile.TemporaryDirectory(prefix="nvshim-shadow-") as directory:
        root = Path(directory)
        fixture = root / "repro.c"
        binary = root / "repro"
        fixture.write_text(harness)
        subprocess.run(
            ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(DRIVER),
             str(fixture), "-o", str(binary)], check=True
        )
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
