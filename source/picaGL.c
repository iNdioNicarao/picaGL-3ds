#include <stdio.h>
#include <unistd.h>
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

	// DIAGNOSTIC: solid-white toggle (see below). Read once; survives across
	// the if/else blocks so the log can report it.
	static int solid_mode = -1; // -1 = not yet checked

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
		{
			FILE *sf = fopen("sdmc:/3ds/d1/pgl_trace.txt", "a");
			if (sf) {
				fprintf(sf, "PGL swap stereo side=%d fb=%p hasStereo=%d fmt=%d\n",
					(int)pglState->display_side, (void*)fb,
					(pglState->display_side == GFX_RIGHT) ? 1 : 0, (int)output_format);
				fclose(sf);
			}
		}
		// Present on EVERY swap so the top screen always updates:
		//  - LEFT eye  -> present mono (hasStereo=false): top shows the scene.
		//  - RIGHT eye -> present as stereo pair (hasStereo=true).
		// This restores a visible top screen even when the game renders only
		// one eye, and shows a correct pair when both eyes are rendered.
		// Use a SINGLE VBlank-synced gfxSwapBuffers() (NOT gfxScreenSwapBuffers
		// + gfxSwapBuffers, which double-swaps and desyncs libctru's framebuffer
		// counter -> blank screen). gfxSwapBuffers waits for VBlank and swaps
		// both screens correctly.
		gfxSwapBuffers();
		return;
	}

	if(pglState->display == GFX_TOP)
	{
		// DIAGNOSTIC TOGGLE: if sdmc:/3ds/d1/SOLID exists, overwrite the
		// colorBuffer with solid white so the DISPLAYED result is guaranteed
		// constant regardless of the 3D world. If the screen STILL strobes
		// with SOLID present -> the bug is 100% in the present/transfer path
		// (3D world exonerated). If SOLID is stable -> the 3D world's
		// rendering corrupts the displayed result.
		if (solid_mode == -1)
			solid_mode = (access("sdmc:/3ds/d1/SOLID", F_OK) == 0) ? 1 : 0;
		if (solid_mode) {
			static const uint32_t w = 0xFFFFFFFFu;
			uint32_t *cb = (uint32_t*)pglState->colorBuffer;
			for (int i = 0; i < (240*400); i++) cb[i] = w;
		}

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

	// DIAGNOSTIC: hash the rendered colorBuffer + count swaps.
	{
		static volatile uint32_t h = 0x811C9DC5u;
		const uint32_t *p = (const uint32_t*)pglState->colorBuffer;
		for (int i = 0; i < (240*400); i++) {
			h ^= p[i];
			h *= 0x01000193u;
		}
		static int swap_count = 0;
		swap_count++;
		FILE *sf = fopen("sdmc:/3ds/d1/pgl_trace.txt", "a");
		if (sf) {
			fprintf(sf, "PGL swap #%d display=%d side=%d fb=%p hasStereo=0 fmt=%d cbhash=%08X solid=%d\n",
				swap_count, (int)pglState->display, (int)pglState->display_side, (void*)output_framebuffer, (int)output_format, (unsigned)h, solid_mode);
			fclose(sf);
		}
	}

	gfxSwapBuffers();
}

void pglSelectScreen(unsigned display, unsigned side)
{
	pglState->display = display;
	pglState->display_side = side;
}