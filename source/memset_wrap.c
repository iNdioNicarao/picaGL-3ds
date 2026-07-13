// Diagnostic wrapper (v93): intercept memset() to catch framebuffer-sized
// white clears of VRAM. With --wrap=memset, every memset() call in the app
// (including libc/portlib calls) routes here as __wrap_memset; the real libc
// memset is available as __real_memset.
//
// We only log when the clear is "framebuffer-sized" (>= one 400x240x4 buffer)
// so we don't spam. The log records the caller (return address) + args so the
// ARM11 crash dump's destroyed backtrace can be reconstructed.
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define FB_BYTES (400 * 240 * 4)   // 0x5dc00

extern void *__real_memset(void *s, int c, size_t n);

void *__wrap_memset(void *s, int c, size_t n)
{
    if (n >= FB_BYTES)
    {
        static int logged = 0;
        if (logged < 40)   // cap to avoid SD thrash
        {
            logged++;
            FILE *f = fopen("sdmc:/3ds/d1/memset_trace.txt", "a");
            if (f)
            {
                void *ra = __builtin_return_address(0);
                fprintf(f, "MEMSET dst=%p val=%d n=%zu (0x%zx) caller=%p\n",
                        s, c, n, (size_t)n, ra);
                fclose(f);
            }
        }
    }
    return __real_memset(s, c, n);
}
