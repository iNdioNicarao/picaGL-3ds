#include "internal.h"

// Power-off flag: when set, _queueWaitAndClear() must NOT block waiting for
// the GPU command queue to drain, because on power-off the display is gone
// and the queue will never complete (it would deadlock gfxExit and leave
// libctru's GSP service thread stuck, which then faults on its DSP-mapped
// stack at process exit). Set via pglSetPoweredOff() from the app.
volatile bool pgl_powered_off = false;

void pglSetPoweredOff(void)
{
	pgl_powered_off = true;
}

bool pglIsPoweredOff(void)
{
	return pgl_powered_off;
}

void _queueWaitAndClear()
{
	if (pgl_powered_off)
	{
		// Display already off: do not wait. Just stop/clear the queue so
		// gfxExit() can join the GSP thread and the process can exit.
		gxCmdQueueStop(&pglState->gxQueue);
		gxCmdQueueClear(&pglState->gxQueue);
		return;
	}
	gxCmdQueueWait (&pglState->gxQueue, -1);
	gxCmdQueueStop (&pglState->gxQueue);
	gxCmdQueueClear(&pglState->gxQueue);
}

void _queueRun(bool async)
{
	gxCmdQueueRun(&pglState->gxQueue);
	
	if(!async)
		_queueWaitAndClear();
}