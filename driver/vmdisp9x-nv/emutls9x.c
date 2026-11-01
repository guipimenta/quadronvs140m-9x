/*
 * emutls9x.c - GCC emulated TLS for mesa3d.dll, fast on Windows 9x.
 *
 * mingw GCC compiles every `thread_local` / `__thread` access (Mesa's current GL
 * context and dispatch table, u_thread state, ...) into a call to
 * __emutls_get_address(). libgcc's version (libgcc_eh.a emutls.o) looks the
 * per-thread array up with pthread_getspecific(), which in winpthreads saves
 * and restores GetLastError, takes TlsGetValue for its own thread record and
 * locks/unlocks a spinlock - for EVERY thread-local access. A T61 profile of
 * Soldier of Fortune (immediate-mode GL, tens of thousands of GL calls a frame)
 * had ~45% of the game's samples in pthread_spin_lock/unlock,
 * pthread_getspecific and __pthread_self_lite.
 *
 * This object defines the same two entry points, so libgcc's emutls.o is never
 * linked: the per-thread array lives in a Win32 TLS slot of our own. On Windows
 * 9x the slot is read straight from the thread information block (TIB +0x88 +
 * 4*index, the layout Win98's TlsGetValue reads, verified at start-up by a
 * TlsSetValue/read-back); elsewhere TlsGetValue with the last error preserved
 * (it clears it). The slow paths (first access of a variable in a thread,
 * array growth) follow libgcc: offsets handed out under a lock, storage
 * aligned and initialised from the template, freed by a pthread key destructor
 * at thread exit.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef unsigned int emutls_word __attribute__((mode(word)));
typedef unsigned int emutls_pointer __attribute__((mode(pointer)));

struct __emutls_object
{
	emutls_word size;
	emutls_word align;
	union { emutls_pointer offset; void *ptr; } loc;
	void *templ;
};

struct emutls_array
{
	emutls_pointer size;
	void *data[];
};

void *__emutls_get_address(struct __emutls_object *obj);
void __emutls_register_common(struct __emutls_object *obj, emutls_word size, emutls_word align, void *templ);

static DWORD          tls_index = TLS_OUT_OF_INDEXES;
static DWORD          tib_slot;          /* byte offset of our slot in the 9x TIB, 0 = TlsGetValue */
static volatile LONG  init_state;        /* 0 = not yet, 1 = running, 2 = done */
static volatile LONG  slow_lock;
static emutls_pointer emutls_size;       /* offsets handed out so far */
static pthread_key_t  emutls_key;

static void lock(void)   { while(InterlockedExchange(&slow_lock, 1)) Sleep(0); }
static void unlock(void) { InterlockedExchange(&slow_lock, 0); }

static void *slot_get(void)
{
	if(tib_slot)
	{
		char *tib;
		__asm__ __volatile__("movl %%fs:0x18, %0" : "=r"(tib));
		return *(void **)(tib + tib_slot);
	}
	{
		DWORD e = GetLastError();
		void *v = TlsGetValue(tls_index);
		SetLastError(e);
		return v;
	}
}

static void slot_set(void *v)
{
	DWORD e = GetLastError();
	TlsSetValue(tls_index, v);
	SetLastError(e);
}

static void emutls_destroy(void *p)
{
	struct emutls_array *arr = (struct emutls_array *)p;
	emutls_pointer i;
	for(i = 0; i < arr->size; i++)
		if(arr->data[i])
			free(((void **)arr->data[i])[-1]);
	free(arr);
	slot_set(NULL);
}

static void emutls_init(void)
{
	tls_index = TlsAlloc();
	if(tls_index == TLS_OUT_OF_INDEXES)
		abort();
	if(pthread_key_create(&emutls_key, emutls_destroy) != 0)
		abort();
	/* Windows 9x: TlsGetValue(i) = *(TIB + 0x88 + 4*i) for i < 80. Use it only
	 * if a value written through the API reads back from there. */
	if((GetVersion() & 0x80000000UL) && tls_index < 80)
	{
		char *tib;
		DWORD off = 0x88 + tls_index * 4;
		__asm__ __volatile__("movl %%fs:0x18, %0" : "=r"(tib));
		TlsSetValue(tls_index, (void *)0x5a3c96e1UL);
		if(*(void **)(tib + off) == (void *)0x5a3c96e1UL)
			tib_slot = off;
		TlsSetValue(tls_index, NULL);
	}
}

static void ensure_init(void)
{
	if(init_state == 2)
		return;
	if(InterlockedCompareExchange(&init_state, 1, 0) == 0)
	{
		emutls_init();
		InterlockedExchange(&init_state, 2);
	}
	else
		while(init_state != 2)
			Sleep(0);
}

static void *emutls_alloc(struct __emutls_object *obj)
{
	void *ptr, *ret;
	if(obj->align <= sizeof(void *))
	{
		ptr = malloc(obj->size + sizeof(void *));
		if(!ptr) abort();
		((void **)ptr)[0] = ptr;
		ret = (char *)ptr + sizeof(void *);
	}
	else
	{
		ptr = malloc(obj->size + sizeof(void *) + obj->align - 1);
		if(!ptr) abort();
		ret = (void *)(((emutls_pointer)((char *)ptr + sizeof(void *) + obj->align - 1)) & ~(emutls_pointer)(obj->align - 1));
		((void **)ret)[-1] = ptr;
	}
	if(obj->templ)
		memcpy(ret, obj->templ, obj->size);
	else
		memset(ret, 0, obj->size);
	return ret;
}

static void *emutls_slow(struct __emutls_object *obj, emutls_pointer offset, struct emutls_array *arr)
{
	void *ret;
	if(!arr || offset > arr->size)
	{
		emutls_pointer old = arr ? arr->size : 0, size = old * 2;
		struct emutls_array *n;
		if(size < offset + 32)
			size = offset + 32;
		n = (struct emutls_array *)realloc(arr, sizeof(*n) + size * sizeof(void *));
		if(!n) abort();
		memset(n->data + old, 0, (size - old) * sizeof(void *));
		n->size = size;
		arr = n;
		slot_set(arr);
		pthread_setspecific(emutls_key, arr);
	}
	ret = arr->data[offset - 1];
	if(!ret)
	{
		ret = emutls_alloc(obj);
		arr->data[offset - 1] = ret;
	}
	return ret;
}

void *__emutls_get_address(struct __emutls_object *obj)
{
	emutls_pointer offset = __atomic_load_n(&obj->loc.offset, __ATOMIC_ACQUIRE);
	struct emutls_array *arr;
	void *ret;
	if(__builtin_expect(offset == 0, 0))
	{
		ensure_init();
		lock();
		offset = obj->loc.offset;
		if(offset == 0)
		{
			offset = ++emutls_size;
			__atomic_store_n(&obj->loc.offset, offset, __ATOMIC_RELEASE);
		}
		unlock();
	}
	arr = (struct emutls_array *)slot_get();
	if(__builtin_expect(arr != NULL && offset <= arr->size, 1))
	{
		ret = arr->data[offset - 1];
		if(__builtin_expect(ret != NULL, 1))
			return ret;
	}
	return emutls_slow(obj, offset, arr);
}

void __emutls_register_common(struct __emutls_object *obj, emutls_word size, emutls_word align, void *templ)
{
	if(obj->size < size)
	{
		obj->size = size;
		obj->templ = NULL;
	}
	if(obj->align < align)
		obj->align = align;
	if(templ && size == obj->size)
		obj->templ = templ;
}
