#include <stdio.h>
#include <stdlib.h>

#include "../armdefs.h"
#include "../arch/sound.h"

#include "kernel.h"
#include "swis.h"

/* SharedSound SWI numbers */
#define SharedSound_InstallHandler	0x4B440
#define SharedSound_SampleRate		0x4B446
#define SharedSound_RemoveHandler	0x4B441

int sound_handler_id=0; /* ID of our sound handler (0 if uninstalled) */

#define BLOCK_SIZE (16*2) /* In samples */
#define BUFFER_SAMPLES (128*BLOCK_SIZE) /* 128 64-byte blocks */

SoundData sound_buffer[BUFFER_SAMPLES+BLOCK_SIZE]; /* 1 block overhead for wrapping */
volatile int sound_buffer_in=BUFFER_SAMPLES; /* Number of samples we've placed in the buffer */
volatile int sound_buffer_out=0; /* Number of samples read out by the IRQ routine */
int sound_rate=0; /* 8.24 fixed point value for how fast we should step through the data */
int sound_buff_mask=BUFFER_SAMPLES-1; /* For benefit of assembler code */

extern void buffer_fill(void); /* Assembler function for performing the buffer fills */
extern void error_handler(void); /* Assembler function attached to ErrorV */

void shutdown_sharedsound(void);

int init_sharedsound(void)
{
	/* Try to register sharedsound handler, return nonzero on failure */
	_kernel_swi_regs regs;
	_kernel_oserror *err;
	/* First load SharedSound if it isn't already loaded
	   A *command is the easiest way to do this (usually in your programs !Run file, but this demo doesn't have one) */
	system("RMEnsure SharedSound 0.00 IfThere System:Modules.SSound Then RMLoad System:Modules.SSound");
	/* Now check whether it is loaded, so we can print a more friendly error message than 'SWI xxx not found' */
	regs.r[0] = 18;
	regs.r[1] = (int) "SharedSound";
	if ((err = _kernel_swi(OS_Module,&regs,&regs)))
	{
		printf("Error: SharedSound module not loaded, and not in System:Modules!\n");
		return 1;
	}
	/* Now we can register our handler */
	regs.r[0] = (int) buffer_fill;
	regs.r[1] = 0;
	regs.r[2] = 1;
	regs.r[3] = (int) "Sound demo";
	regs.r[4] = 0;
	if ((err = _kernel_swi(SharedSound_InstallHandler,&regs,&regs)))
	{
		printf("Error: SharedSound_InstallHandler returned error %d, %s\n",err->errnum,err->errmess);
		return 1;
	}
	sound_handler_id = regs.r[0];
	/* Install error handlers */
	atexit(shutdown_sharedsound);
	regs.r[0] = 1;
	regs.r[1] = (int) error_handler;
	regs.r[2] = 0;
	_kernel_swi(OS_Claim,&regs,&regs);
	sound_setSampleRate(0);
	return 0;
}

void shutdown_sharedsound(void)
{
	_kernel_swi_regs regs;
	/* Deregister sharedsound handler */
	if(sound_handler_id)
	{
		regs.r[0] = sound_handler_id;
		_kernel_swi(SharedSound_RemoveHandler,&regs,&regs);
		sound_handler_id = 0;
	}
	/* Remove error handler */
	regs.r[0] = 1;
	regs.r[1] = (int) error_handler;
	regs.r[2] = 0;
	_kernel_swi(OS_Release,&regs,&regs); 
}

extern unsigned int numberOfChannels;

void
sound_poll(ARMul_State *state)
{
  
  int in_pos = sound_buffer_in;

//  static int pr = 100;
//  if(!pr--)
//  {
//    printf("p%d\n",in_pos-sound_buffer_out);
//    pr = 100;
//  }

  while((in_pos-sound_buffer_out < BUFFER_SAMPLES-BLOCK_SIZE) && (MEMC.NextSoundBufferValid))
  {
    int offset = in_pos & (BUFFER_SAMPLES-1);
    if (SoundDMAFetch(sound_buffer + offset,state) == 1) {
      /* sound DMA off, don't add anything to output buffer */
      sound_buffer_in = in_pos;
      break;
    }
    if(offset+BLOCK_SIZE > BUFFER_SAMPLES)
    {
      /* Copy wrapped area */
      memcpy(sound_buffer,sound_buffer+BUFFER_SAMPLES,(offset+BLOCK_SIZE-BUFFER_SAMPLES)*2);
    }
    /* Update */
    in_pos += BLOCK_SIZE;
    sound_buffer_in = in_pos;
  }
//  printf("<%d %d\n",in_pos,sound_buffer_out);

//  if(in_pos-sound_buffer_out >= BUFFER_SAMPLES-BLOCK_SIZE)
//    sound_buffer_out += BLOCK_SIZE;
}

int
sound_init(void)
{
  if (init_sharedsound())
  {
    printf("Error: Couldn't register sound handler\n");
    return -1;
  }

  return 0;
}

/**
 * sound_setSampleRate
 *
 * Set the sample rate of sound, using
 * the period specified in microseconds.
 *
 * @param period period of sample in microseconds
 */
void
sound_setSampleRate(unsigned long period)
{
  ARMword sampleRate;
  /* freq = 1 / (period * 10^-6) */
  if (period != 0) {
    sampleRate = 1000000 / period;
  } else {
    sampleRate = 44100;
  }
#if 1
  /* Calculate correct rate for generating output data */
#if 0
  _swix(SharedSound_SampleRate,_INR(0,1)|_OUT(3),sound_handler_id,sampleRate,&sound_rate);
#else
  ARMword rate2;
  _swix(SharedSound_SampleRate,_INR(0,1)|_OUT(1),sound_handler_id,0,&rate2);
  sound_rate = (((long long) sampleRate) << 34)/rate2;
#endif
  printf("rate %d*%d %d -> %08x\n",period,numberOfChannels,sampleRate,sound_rate);
#else
  printf("rate %d\n",sampleRate);
#endif
}
