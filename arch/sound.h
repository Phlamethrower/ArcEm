#ifndef SOUND_H
#define SOUND_H

/* TODO Needs to be made 16 bit on any platform. */
typedef unsigned short int SoundData;

extern int Sound_BatchSize; /* How many 16*2 sample batches to attempt to deliver to the platform code at once */
extern unsigned long Sound_DMARate; /* How many cycles between DMA fetches */
extern int Sound_FudgeRate; /* Extra fudge factor applied to Sound_DMARate */
extern unsigned long Sound_MaxDesiredRate; /* Maximum sample rate the platform code is happy to deal with. Specified in units of 1/1024Hz */

typedef enum {
  Stereo_LeftRight, /* Data is ordered with left channel first */
  Stereo_RightLeft, /* Data is ordered with right channel first */
} Sound_StereoSense;

extern Sound_StereoSense eSound_StereoSense;

extern int Sound_Init(ARMul_State *state);

extern void Sound_UpdateDMARate(ARMul_State *state);

#ifdef SOUND_SUPPORT
/* These calls are made by DispKbdShared when the corresponding registers are updated */
extern void Sound_SoundFreqUpdated(ARMul_State *state);
extern void Sound_StereoUpdated(ARMul_State *state);

/* This call is made to the platform code upon initialisation */
extern int Sound_InitHost(ARMul_State *state);

/* This is the call made to the platform code, once new sound data is ready
   numSamples is the number of stereo pairs
   samplePeriod is measured in audio clock ticks (with there being DisplayDev_GetVIDCClockIn()/24 ticks per second) */
extern void Sound_HandleData(const SoundData *buffer,int numSamples,int samplePeriod);
#endif

#endif
