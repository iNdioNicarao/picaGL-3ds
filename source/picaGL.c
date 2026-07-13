#include <stdio.h>
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
	// Wait for VBlank BEFORE building/transferring the frame. Without this,
	// the GX_DisplayTransfer into the LCD framebuffer can cross the VBlank
	// boundary and the displayed scan shows a half-written (darker, torn)
	// frame -> the "flashing/strobe" artifact. Waiting up front guarantees
	// the whole transfer lands inside one refresh window.
	gspWaitForVBlank();

	glFlush();

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

	{
		FILE *sf = fopen("sdmc:/3ds/d1/pgl_trace.txt", "a");
		if (sf) {
			fprintf(sf, "PGL swap display=%d side=%d fb=%p hasStereo=0 fmt=%d\n",
				(int)pglState->display, (int)pglState->display_side, (void*)output_framebuffer, (int)output_format);
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