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

// Stereo-3D: when enabled, pglSwapBuffers presents a LEFT+RIGHT framebuffer
// pair (gfxScreenSwapBuffers(GFX_TOP, true)) so the 3DS parallax barrier shows
// depth. The game must render the scene twice (per-eye) before swapping.
void pglSetStereo(bool enable);

// Re-bind the render target (colorBuffer + depthBuffer) to the GPU each
// frame. citro3d requires C3D_FrameDrawOn(target) per frame; picaGL only
// bound it once at init, so after the first gfxScreenSwapBuffers the GPU's
// framebuffer registers are stale and every subsequent glClear/glDrawArrays
// writes nowhere -> the screen freezes on the last pre-swap image. Calling
// this at the start of each frame restores a live render target.
void pglBindFramebuffer(void);

#ifdef __cplusplus
}
#endif

#endif