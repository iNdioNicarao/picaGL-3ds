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

// Re-acquire the GPU after a system applet (swkbd, Home Menu, etc.) has taken
// over and returned. picaGL's cached GX queue / render buffers / texture-env /
// shader state are stale, so without this the next GPU write data-aborts.
// Replays the APTHOOK_ONRESTORE re-init. Safe to call even if nothing was
// taken over.
void pglReacquire(void);
// After a system applet / menu has uploaded its own textures, picaGL's bound
// texture pointer and GPU texture objects can be left pointing at the wrong
// (or freed) texture, so wall textures render black. Force a clean re-bind of
// every texture on the next draw (clears textureBound + flags the change).
// Call this after any menu that renders a background bitmap, or after swkbd.
void pglTextureReset(void);
// Returns true once the system is powering off / suspending (set in
// APTHOOK_ONSUSPEND and by pglSetPoweredOff). Consumers that touch the GPU
// directly (e.g. the bottom-screen flush) must bail when this is true to
// avoid a Data Abort on the GPU/LCD register space during teardown.
bool pglIsPoweredOff(void);

#ifdef __cplusplus
}
#endif

#endif