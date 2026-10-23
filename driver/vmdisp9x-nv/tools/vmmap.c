/* vmmap - dump the user address space of this process (Win9x friendly). */
#include <windows.h>
#include <stdio.h>
int main(void)
{
	MEMORY_BASIC_INFORMATION mbi; char *p = 0; char name[MAX_PATH];
	while((DWORD)p < 0xC0000000UL)
	{
		if(!VirtualQuery(p, &mbi, sizeof(mbi))) { p += 0x10000; continue; }
		if(mbi.State != MEM_FREE)
		{
			name[0] = 0;
			if(mbi.Type == MEM_IMAGE && mbi.AllocationBase == mbi.BaseAddress)
				GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name));
			printf("%08lX-%08lX %s %s %s\n", (DWORD)mbi.BaseAddress, (DWORD)mbi.BaseAddress + mbi.RegionSize,
				mbi.State == MEM_COMMIT ? "commit " : "reserve",
				mbi.Type == MEM_IMAGE ? "image  " : mbi.Type == MEM_MAPPED ? "mapped " : "private", name);
		}
		p = (char*)mbi.BaseAddress + mbi.RegionSize;
		if(!p) break;
	}
	return 0;
}
