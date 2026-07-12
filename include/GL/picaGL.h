#ifndef __PICAGL_H__
#define __PICAGL_H__

#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

void pglInit();
void pglExit();
void pglSwapBuffers();
void pglSelectScreen(unsigned display, unsigned side);

// Power-off fix: when the 3DS is powering down, the display/GPU is already
// gone, so picaGL's _queueWaitAndClear() (called from pglExit and the
// APTHOOK_ONSUSPEND handler) would block forever waiting for a GPU command
// queue that will never drain. Signal picaGL so it skips the blocking wait;
// the OS reclaims the queue on process exit. Without this, gfxExit() (which
// must run to cleanly join libctru's GSP service thread) deadlocks, and the
// GSP thread later faults on its DSP-mapped stack -> ARM11 data abort.
void pglSetPoweredOff(void);

#ifdef __cplusplus
}
#endif

#endif