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

	// BUILD MARKER (v99, diagnostic): proves this exact picaGL build
	// executed on-device. Written once at pglInit. If sdmc:/3ds/d1/
	// BUILD_v99.txt exists after a run, v99 actually ran (not a stale
	// installed title). The strobe fix + stereo path live in d1; this
	// just confirms the instrumented picaGL is the one booted.
	{
		FILE *bm = fopen("sdmc:/3ds/d1/BUILD_v99.txt", "w");
		if (bm) { fprintf(bm, "picaGL BUILD v99 (stereo-enabled, lum_trace active)\n"); fclose(bm); }
	}

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

void pglTextureReset(void)
{
	/* Force a clean re-bind of every texture on the next draw. After a
	 * system applet / menu has uploaded its own textures (e.g. a menu PCX
	 * background) picaGL's textureBound pointer can point at the wrong
	 * texture, so wall textures render black. Clearing textureBound +
	 * flagging the change makes the next draw re-establish the textures. */
	if (!pglState)
		return;

	pglState->textureBound[0] = NULL;
	pglState->textureBound[1] = NULL;
	pglState->textureChanged = GL_TRUE;
	pglState->changes |= STATE_TEXTURE_CHANGE;
}

void pglReacquire(void)
{
	/* Recovery after a system applet (swkbd, Home Menu, etc.) takes over the
	 * GPU and returns: picaGL's cached GX queue, render buffers, texture-env
	 * and shader state are stale, so the next GPU draw would data-abort.
	 * Replays the exact re-init that APTHOOK_ONRESTORE performs, without
	 * touching the power-off flag. */
	if (!pglState)
		return;

	GX_BindQueue(&pglState->gxQueue);
	gxCmdQueueRun(&pglState->gxQueue);

	_picaRenderBuffer(pglState->colorBuffer, pglState->depthBuffer);
	_picaAttribBuffersLocation((void*)__ctru_linear_heap);

	for (int i = 1; i < 6; i++)
		_picaTextureEnvSet(i, &pglState->texenv[PGL_TEXENV_DUMMY]);

	shaderProgramUse(&pglState->basicShader);
	pglState->changes |= 0xFFFFFFFF;

	/* The applet/menu also disturbed the texture bindings (walls render
	 * black afterwards), so re-bind every texture on the next draw. */
	pglTextureReset();
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
		// Wait for the GX display-transfer to FINISH before the game
		// overwrites colorBuffer with the next eye (and before we flip).
		gspWaitForPPF();
		// v99h: STROBE + STUCK-BANNER ROOT CAUSE. Previously this branch
		// called gfxScreenSwapBuffers(GFX_TOP, ...) after EACH eye -> TWO
		// swaps per frame. The TOP screen is double-buffered, so two swaps
		// toggle the back-buffer index TWICE per frame: the game keeps
		// rendering into one bank while the hardware keeps DISPLAYING the
		// other (the stale banner bank) -> banner stuck on screen + strobe
		// from partial/alternating flips. Correct 3DS stereo discipline:
		// fill the LEFT back buffer (no swap), fill the RIGHT back buffer,
		// then swap ONCE with hasStereo=true to present both eyes together.
		// So: only swap on the RIGHT eye (the second present of the pair).
		{
			unsigned long cbsum=0; unsigned n=0;
			const uint32_t *p=(const uint32_t*)pglState->colorBuffer;
			for (int i=0;i<(240*400);i+=64){uint32_t c=p[i];cbsum+=(c&0xFF)+((c>>8)&0xFF)+((c>>16)&0xFF);n+=3;}
			FILE *sf = fopen("sdmc:/3ds/d1/pgl_trace.txt", "a");
			if (sf) {
				fprintf(sf, "STEREO side=%d fb=%p cblum=%lu swap=%d\n",
					(int)pglState->display_side, (void*)fb, cbsum/(n?n:1),
					(pglState->display_side == GFX_RIGHT) ? 1 : 0);
				fclose(sf);
			}
		}
		if (pglState->display_side == GFX_RIGHT)
			gfxScreenSwapBuffers(GFX_TOP, true);
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
			fprintf(sf, "swap=%d cblum=%d fblum=%d fb=%p fmt=%d cbhash=%08X disp=%d side=%d stereo=%d\n",
				swap_count, cblum, fblum, (void*)output_framebuffer, (int)output_format, (unsigned)h,
				(int)pglState->display, (int)pglState->display_side, (int)pglState->stereo);
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