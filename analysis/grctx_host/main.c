#include "core/gpuobj.h"
#include "nv50.h"
int main(int argc, char **argv)
{
	struct nvkm_device dev; struct nvkm_gpuobj mem; u32 size = 0, i, nz = 0;
	u32 prog[512];
	memset(&dev, 0, sizeof dev); dev.chipset = (u8)strtoul(argv[1], 0, 16); dev.units = strtoul(argv[2], 0, 16); dev.ctxprog = prog;
	nv50_grctx_init(&dev, &size);
	mem.size = size / 4; mem.data = calloc(mem.size, 4);
	nv50_grctx_fill(&dev, &mem);
	printf("chipset %02x units %08x: ctxprog %u dwords, grctx %u bytes (%u dwords)\n", dev.chipset, dev.units, dev.ctxprog_len, size, size / 4);
	printf("CTXPROG\n"); for (i = 0; i < dev.ctxprog_len; i++) printf("%u 0x%08x\n", i, prog[i]);
	printf("CTXVALS\n"); for (i = 0; i < mem.size; i++) if (mem.data[i]) { printf("%u 0x%08x\n", i, mem.data[i]); nz++; }
	printf("END nonzero %u\n", nz);
	return 0;
}
