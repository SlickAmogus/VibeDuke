/*
 * Xbox DirectSound audio driver for jfaudiolib
 * Uses RXDK's dsound.lib for hardware VP→GP→EP→AC97 pipeline.
 */

#ifndef __driver_dsound_xbox_h__
#define __driver_dsound_xbox_h__

#include "midifuncs.h"

int  XboxDSDrv_GetError(void);
const char *XboxDSDrv_ErrorString(int ErrorNumber);

int  XboxDSDrv_PCM_Init(int *mixrate, int *numchannels, int *samplebits, void *initdata);
void XboxDSDrv_PCM_Shutdown(void);
int  XboxDSDrv_PCM_BeginPlayback(char *BufferStart, int BufferSize,
                                  int NumDivisions, void (*CallBackFunc)(void));
void XboxDSDrv_PCM_StopPlayback(void);
void XboxDSDrv_PCM_Lock(void);
void XboxDSDrv_PCM_Unlock(void);

#endif
