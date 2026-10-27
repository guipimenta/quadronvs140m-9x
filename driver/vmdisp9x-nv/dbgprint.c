/*****************************************************************************

Copyright (c) 2022  Michal Necasek
              2023  Jaroslav Hensl

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*****************************************************************************/

/* Homegrown printf support. The C runtime printf() is hard to use without
 * dragging in much of the library, and much worse, the small model runtime
 * can't operate with SS != DS. So we roll our own very simplified printf()
 * subset, good enough for what we need.
 */

#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include "dbgring.h"
#ifndef VXD32
#include <conio.h>
#include "vxd.h"
#else
#define IO_IN8
#define IO_OUT8
#include "io32.h"
#include "code32.h"
#endif


/* Backdoor logging I/O ports. */
#if defined(COM2)
# define INFO_PORT   0x2F8 /* COM2 I/O */
#elif defined(COM3)
# define INFO_PORT   0x3E8 /* COM3 I/O */
#elif defined(COM4)
# define INFO_PORT   0x2E8 /* COM3 I/O */
#elif defined(COM0)
/* no debug output */
#else
# define INFO_PORT   0x3F8 /* default COM1 I/O */
#endif

#define FULL_SERIAL
/* ^ inicialize serial and waiting for transmit same way as with real HW serial
 *   if defined. VirtualBox usually require it.
 */

#pragma code_seg( _INIT );

#ifdef DBGRING
/* ---- in-memory ring instead of a COM port (see dbgring.h) ---------------- */
#ifdef VXD32
static char     dbg_ring[DBG_RING_SIZE];
static uint32_t dbg_ring_head  = 0;   /* next write position            */
static uint32_t dbg_ring_total = 0;   /* characters ever written        */

void dbg_ring_putc( char c )
{
	dbg_ring[dbg_ring_head] = c;
	dbg_ring_head++;
	if(dbg_ring_head >= DBG_RING_SIZE) dbg_ring_head = 0;
	dbg_ring_total++;
}

/* Append a ring-3 string (from the libdrm shim in mesa3d.dll, via OP_NV_DBG_PUTS)
 * to the ring, prefixing each line with "S> " so shim lines are distinct from
 * the VxD's "V> " and the 16-bit driver's "D> ". */
void dbg_ring_puts( const char *s, uint32_t len )
{
	static int bol = 1;
	uint32_t i;
	for(i = 0; i < len && s[i]; i++)
	{
		if(bol) { dbg_ring_putc('S'); dbg_ring_putc('>'); dbg_ring_putc(' '); bol = 0; }
		dbg_ring_putc(s[i]);
		outp(0xE9, s[i]);
		if(s[i] == '\n') bol = 1;
	}
}

/* Copy the ring, oldest character first, into out (at most max bytes).
 * Returns the number of bytes copied. */
uint32_t dbg_ring_read( char *out, uint32_t max )
{
	uint32_t avail = dbg_ring_total < DBG_RING_SIZE ? dbg_ring_total : DBG_RING_SIZE;
	uint32_t start = dbg_ring_total < DBG_RING_SIZE ? 0 : dbg_ring_head;
	uint32_t n = 0;
	if(avail > max) { start = (start + (avail - max)) % DBG_RING_SIZE; avail = max; }
	while(n < avail)
	{
		out[n++] = dbg_ring[start++];
		if(start >= DBG_RING_SIZE) start = 0;
	}
	return n;
}

/* 3049: same as dbg_ring_read, preceded by the running total of characters
 * ever written, so a reader can tell exactly how many bytes are new (and
 * how many were lost) between two polls even when the text repeats. */
uint32_t dbg_ring_read2( char *out, uint32_t max )
{
	uint32_t n;
	if(max < 4) return 0;
	*(uint32_t*)out = dbg_ring_total;
	n = dbg_ring_read(out + 4, max - 4);
	return n + 4;
}

static void prt_ch( char c )
{
	static int bol = 1;
	if(bol) { dbg_ring_putc('V'); dbg_ring_putc('>'); dbg_ring_putc(' '); outp(0xE9,'V'); outp(0xE9,'>'); outp(0xE9,' '); bol = 0; }
	dbg_ring_putc(c);
	outp(0xE9, c);   /* QEMU -debugcon / Bochs port E9; no effect on real hardware */
	if(c == '\n') bol = 1;
}
#else /* 16-bit display driver: push every character into the VxD's ring */
static uint32_t dbg_vxd_ep = 0;   /* VxD PM16 API far entry (int 2Fh/1684h) */

static void dbg_vxd_connect( void )
{
	_asm {
		push es
		push ax
		push bx
		push di
		xor  di, di
		mov  es, di
		mov  ax, 1684h
		mov  bx, VXD_DEVICE_ID
		int  2Fh
		mov  word ptr [dbg_vxd_ep], di
		mov  word ptr [dbg_vxd_ep+2], es
		pop  di
		pop  bx
		pop  ax
		pop  es
	}
}

static void dbg_vxd_putc( char c )
{
	_asm {
		.386
		push eax
		push ebx
		push ecx
		push edx
		push esi
		push edi
		mov  edx, OP_NV_DBG_PUTC
		xor  ecx, ecx
		mov  cl, byte ptr [c]
		call dword ptr [dbg_vxd_ep]
		pop  edi
		pop  esi
		pop  edx
		pop  ecx
		pop  ebx
		pop  eax
	}
}

/* Second, independent sink: C:\NVMINI.LOG, appended one line at a time
 * through KERNEL's _lopen/_lwrite (plain DOS calls, available from the first
 * line of DriverInit onwards). It is capped so a chatty path cannot hammer
 * the disk. Used in addition to the ring so that "nvmini.drv ran but never
 * reached the VxD" leaves evidence somewhere. */
#define DBG_FILE_PATH      "C:\\NVMINI.LOG"
#define DBG_FILE_MAXLINES  600
#define DBG_LINE_MAX       200
int           __far __pascal _lopen ( const char __far *path, int mode );
int           __far __pascal _lcreat( const char __far *path, int attr );
long          __far __pascal _llseek( int h, long off, int origin );
unsigned int  __far __pascal _lwrite( int h, const char __far *buf, unsigned int len );
int           __far __pascal _lclose( int h );

static char     dbg_line[DBG_LINE_MAX];
static unsigned dbg_line_len   = 0;
static unsigned dbg_file_lines = 0;

static void dbg_file_flush( void )
{
	int h;
	if(dbg_line_len == 0) return;
	if(dbg_file_lines < DBG_FILE_MAXLINES)
	{
		h = _lopen((const char __far *)DBG_FILE_PATH, 2 /* OF_READWRITE */);
		if(h == -1)
			h = _lcreat((const char __far *)DBG_FILE_PATH, 0);
		if(h != -1)
		{
			_llseek(h, 0L, 2 /* FILE_END */);
			_lwrite(h, (const char __far *)dbg_line, dbg_line_len);
			_lclose(h);
			dbg_file_lines++;
		}
	}
	dbg_line_len = 0;
}

static void prt_ch( char c )
{
	static int bol = 1;
	if(bol)
	{
		dbg_line[0] = 'D'; dbg_line[1] = '>'; dbg_line[2] = ' '; dbg_line_len = 3;
		outp(0xE9,'D'); outp(0xE9,'>'); outp(0xE9,' ');
		bol = 0;
	}
	outp(0xE9, c);   /* QEMU -debugcon / Bochs port E9; no effect on real hardware */
	if(c == '\n')
	{
		if(dbg_line_len < DBG_LINE_MAX - 2) { dbg_line[dbg_line_len++] = '\r'; dbg_line[dbg_line_len++] = '\n'; }
	}
	else if(dbg_line_len < DBG_LINE_MAX - 2)
		dbg_line[dbg_line_len++] = c;

	if(dbg_vxd_ep == 0)
		dbg_vxd_connect();          /* retried per character until the VxD answers */
	if(dbg_vxd_ep != 0)
	{
		if(dbg_line_len == 4 && c != '\n') { dbg_vxd_putc('D'); dbg_vxd_putc('>'); dbg_vxd_putc(' '); }
		dbg_vxd_putc(c);
	}
	if(c == '\n') { dbg_file_flush(); bol = 1; }
}
#endif /* VXD32 */
#else  /* !DBGRING: original COM-port sink */

#ifdef FULL_SERIAL
static int serial_inited = 0;

static void init_serial() {
#ifdef INFO_PORT
   outp(INFO_PORT + 1, 0x00);    // Disable all interrupts
   outp(INFO_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
   outp(INFO_PORT + 0, 0x01);    // Set divisor to 3 (lo byte) 38400 baud 0x0C = 9600, 0x01 = 115200
   outp(INFO_PORT + 1, 0x00);    //                  (hi byte)
   outp(INFO_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
   outp(INFO_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
   //outp(INFO_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
#endif
}

static int is_transmit_empty()
{
#ifdef INFO_PORT
	return inp(INFO_PORT + 5) & 0x20;
#else
	return 1;
#endif
}
#endif

static void prt_ch( char c )
{
#ifdef FULL_SERIAL
	if(serial_inited == 0)
	{
		init_serial();
		serial_inited = 1;
	}
	
	while (is_transmit_empty() == 0);
#endif
#ifdef INFO_PORT
	outp(INFO_PORT, c);
#endif
}
#endif /* DBGRING */

#ifdef VXD32
int dbg_isdigit( int c )
{
	if(c >= '0' && c <= '9')
	{
		return 1;
	}
	return 0;
}
#else
#define dbg_isdigit isdigit
#endif

static void prt_u32( uint32_t val, int width )
{
    uint32_t    uval;
    uint32_t    base;
    char        buf[16];    /* It's 12 digits max. */
    int         n = 0;

    uval = val;

    do {
        base = uval / 10;
        buf[n++] = uval - base * 10 + '0';
        uval = base;
    } while( uval );

    /* Pad with spaces. */
    while( width > n ) {
        prt_ch( ' ' );
        --width;
    }

    do {
        prt_ch( buf[--n] );
    } while( n );
}

static void prt_i32( int32_t val, int width )
{
    uint32_t    uval;

    if( val < 0 ) {
        prt_ch( '-' );
        uval = -val;
        if( width > 0 )
            --width;
    } else {
        uval = val;
    }

    prt_u32( uval, width );
}

static void prt_hex32( uint32_t val, int width, char hexa )
{
    char    buf[8]; /* Enough for a dword. */
    int     n = 0;
    int     nibble;

    do {
        nibble = val & 0xF;
        val >>= 4;
        buf[n++] = nibble + (nibble > 9 ? hexa - 10 : '0');
    } while( val );

    /* Pad with zeros. */
    while( width > n ) {
        prt_ch( '0' );
        --width;
    }

    do {
        prt_ch( buf[--n] );
    } while( n );
}

static void prt_str( const char __far *s )
{
    while( *s )
        prt_ch( *s++ );
}

void dbg_printf( const char *s, ... )
{
    char        type_len;
    char        conv;
    char        hexa;
    int         width;
    uint16_t    word;
    uint32_t    dword;
    va_list     args;

    va_start( args, s );

    /* Go until end of format string. */
    while( *s ) {
        /* Is it a format specifier? */
        if( *s == '%' ) {
            ++s;    /* Eat the % sign. */
            conv     = 0;
            width    = 0;
            type_len = 0;

            /* Process field width, if any. */
            while( dbg_isdigit( *s ) ) {
                width *= 10;
                width += *s++ - '0';
            }

            /* Process type length specifier, if any. */
            switch( *s ) {
            case 'l':   /* Long integers. */
            case 'W':   /* Far pointers. */
                type_len = *s++;
                break;
            default:    /* Do nothing. */
                break;
            }

            /* Now check if a supported conversion is there. */
            switch( *s ) {
            case 'c':
            case 'd':
            case 'u':
            case 'p':
            case 'P':
            case 's':
            case 'x':
            case 'X':
                conv = *s++;
                break;
            default:    /* Do nothing. */
                break;
            }

            if( conv ) {
                /* Time to start grabbing stuff off the stack. */
#ifdef VXD32
                /* in 32-bit are all args 32bit... */
                dword = va_arg( args, uint32_t );
                word  = (dword & 0xFFFF);
#else
                word = va_arg( args, uint16_t );
                /* If argument is double wide, build a doubleword. */
                if( type_len == 'l' || type_len == 'W' ) {
                    dword = va_arg( args, uint16_t );
                    dword <<= 16;
                    dword |= word;
                }
#endif
                if( conv == 'c' ) {
                    prt_ch( word );
                } else if( conv == 'd' ) {
                    if( type_len == 'l' )
                        prt_i32( dword, width );
                    else
                        prt_i32( (int16_t)word, width );
                } else if( conv == 'u' ) {
                    if( type_len == 'l' )
                        prt_u32( dword, width );
                    else
                        prt_u32( word, width );
                } else if( conv == 'p' || conv == 'P' ) {
                    hexa = conv - 'P' + 'A';
                    if( type_len == 'W' ) {
                        prt_hex32( dword >> 16, 4, hexa );
                        prt_ch( ':' );
                        prt_hex32( word, 4, hexa );
                    } else {
                        prt_hex32( word, 4, hexa );
                    }
                } else if( conv == 's' ) {
#ifdef VXD32
                    prt_str((const char *)dword);
#else
                    if( type_len == 'W' )
                        prt_str( (const char __far *)dword );
                    else
                        prt_str( (const char *)word );
#endif
                } else if( conv == 'x' || conv == 'X' ) {
                    hexa = conv - 'X' + 'A';
#ifdef VXD32
                    if( !width )
                        width = 8;
                    prt_hex32( dword, width, hexa );
#else
                    if( type_len == 'l' ) {
                        if( !width )
                            width = 8;
                        prt_hex32( dword, width, hexa );
                    } else {
                        if( !width )
                            width = 4;
                        prt_hex32( word, width, hexa );
                    }
#endif
                }
            } else {
                /* Just print whatever is there. */
                prt_ch( *s++ );
            }
        } else {
            prt_ch( *s++ );
        }
    }
    va_end( args );
}
