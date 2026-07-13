#include <stdio.h>
#include <3ds/types.h>            // u32 etc. for gspgpu.h
#include <3ds/services/gspgpu.h> // gspWaitForPPF()
#include "internal.h"

static aptHookCookie _hookCookie;

static void _AptEventHook(APT_HookType type, void* param)
{

	switch (type)
	{
		case APTHOOK_ONSUSPEND:
		{
			_queueWaitAndClear();
			break;
		}
		case APTHOOK_ONRESTORE:
		{
			GX_BindQueue(&pglState->gxQueue);
			gxCmdQueueRun(&pglState->gxQueue);

			_picaRenderBuffer(pglState->colorBuffer, pglState->depthBuffer);
			_picaAttribBuffersLocation((void*)__ctru_linear_heap);

			for(int i = 1; i < 6; i++)
				_picaTextureEnvSet(i, &pglState->texenv[PGL_TEXENV_DUMMY]);

			shaderProgramUse(&pglState->basicShader);
			pglState->changes |= 0xFFFFFFFF;
			break;
		}
		default:
			break;
	}
}

void pglInit()
{
	static int pgl_initialized = 0;

	if(pgl_initialized)
		return;

	pglState = malloc(sizeof(picaGLState));
	memset(pglState, 0, sizeof(picaGLState));
	
	_stateInitialize();
	_stateDefault();

	aptHook(&_hookCookie, _AptEventHook, NULL);
}

void pglExit()
{
	aptUnhook(&_hookCookie);

	_queueWaitAndClear();
	GX_BindQueue(NULL);

	//TODO: Clear memory
}

void pglSetStereo(bool enable)
{
	pglState->stereo = enable;
}

void pglBindFramebuffer(void)
{
	_picaRenderBuffer(pglState->colorBuffer, pglState->depthBuffer);
	_picaAttribBuffersLocation((void*)__ctru_linear_heap);
}

void pglSwapBuffers()
{
	// NOTE: gspWaitForVBlank() was added here in v89 but it hung power-off
	// (blocks for a VBlank that never arrives during shutdown) and did NOT
	// fix the strobe. Removed. The single gfxSwapBuffers() below is the
	// correct, VBlank-synced present.

	glFlush();

	// Flush the CPU/GPU caches for the LCD framebuffer BEFORE transferring
	// into it (standard libctru practice). Without this the display may show
	// stale/partial cache lines -> tearing/strobe even with correct content
	// and a correct present (proven by the constant cbhash in v90).
	gfxFlushBuffers();

	uint32_t *output_framebuffer = (uint32_t*)gfxGetFramebuffer(pglState->display, pglState->display_side, NULL, NULL);
	uint8_t output_format = gfxGetScreenFormat(pglState->display);

	if (pglState->stereo && pglState->display == GFX_TOP)
	{
		uint32_t *fb = (uint32_t*)gfxGetFramebuffer(GFX_TOP, pglState->display_side, NULL, NULL);
		GX_DisplayTransfer(
			(u32*)pglState->colorBuffer, GX_BUFFER_DIM(240, 400),
			fb, GX_BUFFER_DIM(240, 400),
			GX_TRANSFER_OUT_FORMAT(output_format));
		_queueRun(false);
		// Wait for the GX display-transfer to FINISH before flipping.
		// _queueRun(false) only waits for the GPU command queue (rendering);
		// the GX_DisplayTransfer goes to the GSP transfer unit, a different
		// queue. Without this wait, gfxSwapBuffers() flips the framebuffer
		// while the transfer is still writing -> the top screen shows a mix
		// of new + stale/partial lines = the strobe/tearing.
		gspWaitForPPF();
		{
			FILE *sf = fopen("sdmc:/3ds/d1/pgl_trace.txt", "a");
			if (sf) {
				fprintf(sf, "PGL swap stereo side=%d fb=%p hasStereo=%d fmt=%d\n",
					(int)pglState->display_side, (void*)fb,
					(pglState->display_side == GFX_RIGHT) ? 1 : 0, (int)output_format);
				fclose(sf);
			}
		}
		// Present the TOP screen only (the one we just rendered+transferred).
		// Swap the correct screen with hasStereo matching the eye side, per
		// upstream picaGL. Do NOT call gfxSwapBuffers() here: that flips BOTH
		// screens, but we only transferred the TOP this call, so the bottom
		// would be flipped to an unfilled buffer -> blank/strobe.
		gfxScreenSwapBuffers(GFX_TOP, pglState->display_side == GFX_RIGHT);
		return;
	}

	if(pglState->display == GFX_TOP)
	{
		GX_DisplayTransfer(
			(u32*)pglState->colorBuffer, GX_BUFFER_DIM(240, 400),
			output_framebuffer, GX_BUFFER_DIM(240, 400),
			GX_TRANSFER_OUT_FORMAT(output_format));
	}
	else
	{
		GX_DisplayTransfer(
			(u32*)pglState->colorBuffer + (240*80),GX_BUFFER_DIM(240, 320),
			output_framebuffer, GX_BUFFER_DIM(240, 320),
			GX_TRANSFER_OUT_FORMAT(output_format));
	}

	_queueRun(false);
	// Wait for the GX display-transfer to FINISH before flipping (see note
	// above the stereo branch). This is the primary strobe fix for mono.
	gspWaitForPPF();
	{
		static volatile uint32_t h = 0x811C9DC5u;
		const uint32_t *p = (const uint32_t*)pglState->colorBuffer;
		for (int i = 0; i < (240*400); i++) {
			h ^= p[i];
			h *= 0x01000193u;
		}
		// DIAGNOSTIC (camera-independent): sample mean luminance of the
		// rendered colorBuffer AND the transferred LCD buffer. This tells us
		// whether any brightness pulse originates in the rendered content
		// (colorBuffer) or in the present/transfer (output_framebuffer), and
		// is immune to phone-camera capture artifacts.
		unsigned long cbsum=0, fbsum=0, n=0;
		const uint32_t *fb = (const uint32_t*)output_framebuffer;
		for (int i = 0; i < (240*400); i += 64) {
			uint32_t c = p[i];
			cbsum += (c&0xFF) + ((c>>8)&0xFF) + ((c>>16)&0xFF);
			uint32_t o = fb[i];
			fbsum += (o&0xFF) + ((o>>8)&0xFF) + ((o>>16)&0xFF);
			n += 3;
		}
		int cblum = (int)(cbsum/n);
		int fblum = (int)(fbsum/n);
		static int swap_count = 0;
		swap_count++;
		FILE *sf = fopen("sdmc:/3ds/d1/lum_trace.txt", "a");
		if (sf) {
			fprintf(sf, "swap=%d cblum=%d fblum=%d fb=%p fmt=%d cbhash=%08X\n",
				swap_count, cblum, fblum, (void*)output_framebuffer, (int)output_format, (unsigned)h);
			fclose(sf);
		}
	}

	gfxScreenSwapBuffers(pglState->display, false);
}

void pglSelectScreen(unsigned display, unsigned side)
{
	pglState->display = display;
	pglState->display_side = side;
}