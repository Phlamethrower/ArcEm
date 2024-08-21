/* Display and keyboard interface for the Arc emulator */
/* (c) David Alan Gilbert 1995-1999 - see Readme file for copying info */

#ifndef DISPKBD_HEADER
#define DISPKBD_HEADER

#include "../armdefs.h"

#define POLLGAP 125

#define UPDATEBLOCKSIZE 256

/* Set this to something different if you're not using 16bpp output */
typedef unsigned short HostPixel;

struct DisplayInfo {
  /* Raw VIDC registers, except where noted */
  struct {
    unsigned int Palette[16];
    HostPixel BorderCol; /* Border colour in host format */ 
    unsigned int CursorPalette[3];
    unsigned int Horiz_Cycle;
    unsigned int Horiz_SyncWidth;
    unsigned int Horiz_BorderStart;
    unsigned int Horiz_DisplayStart;
    unsigned int Horiz_DisplayEnd;
    unsigned int Horiz_BorderEnd;
    unsigned int Horiz_CursorStart;
    unsigned int Horiz_Interlace;
    unsigned int Vert_Cycle;
    unsigned int Vert_SyncWidth;
    unsigned int Vert_BorderStart;
    unsigned int Vert_DisplayStart;
    unsigned int Vert_DisplayEnd;
    unsigned int Vert_BorderEnd;
    unsigned int Vert_CursorStart;
    unsigned int Vert_CursorEnd;
    unsigned int SoundFreq;
    unsigned int ControlReg;
    unsigned int StereoImageReg[8];
  } Vidc;

  struct {
    /* Values which get updated by external code */

    unsigned int DirtyPalette; /* Bit flags of which palette entries have been modified */
    char ModeChanged; /* Set if any registers change which may require the host to change mode. Remains set until valid mode is available from host (suspends all display output) */

    /* Values that must only get updated by the event queue/screen blit code */
    
    char ForceRefresh; /* =1 for the entire frame if the mode has just changed */
    char DMAEn; /* 1/0 whether video DMA is enabled for this frame */
    char FLYBK; /* Flyback signal (i.e. whether we've triggered VSync IRQ this frame) */ 
    int LastHostWidth,LastHostHeight,LastHostHz; /* Values we used to request host mode */
    int LastRow; /* Row last event was scheduled to run up to */
    int NextRow; /* Row next event is scheduled to run up to */
    int MaxRow; /* Row to stop at for this frame */
    unsigned int VIDC_CR; /* Control register value in use for this frame */
    unsigned int LineRate; /* Line rate, measured in EmuRate clock cycles */
    unsigned int Vptr; /* DMA pointer, in bits, as offset from start of phys RAM */
    unsigned int LastVinit; /* Last Vinit, so we can sync changes with the frame start */
  } Control;

  struct {
    /* The host must update these on DisplayKbd_HostChangeMode */
    int Width,Height,XScale,YScale; /* Host display mode */

    /* The core handles these */
    int XOffset,YOffset; /* X & Y offset of first display pixel in host */
    HostPixel Palette[256]; /* Host palette */
    HostPixel BorderCol[1024]; /* Last border colour used for each scanline */
    unsigned int RefreshFlags[1024/32]; /* Bit flags of which display scanlines need full refresh due to Vstart/Vend/palette changes */
    unsigned int UpdateFlags[1024][(512*1024)/UPDATEBLOCKSIZE]; /* Flags for each scanline (ouch!) */
  } HostDisplay;
};


#define DISPLAYINFO (*(state->Display))
#define VIDC (DISPLAYINFO.Vidc)
/* Use this in gdb: state->Display->HostDisplay */
#define HOSTDISPLAY (DISPLAYINFO.HostDisplay)
#define DISPLAYCONTROL (DISPLAYINFO.Control)

#define VideoRelUpdateAndForce(flag, writeto, from) \
{\
  if ((writeto) != (from)) { \
    (writeto) = (from);\
    flag = 1;\
  };\
};

/*----------------------------------------------------------------------------*/

/* Functions each Host must provide */
void DisplayKbd_InitHost(ARMul_State *state);
void DisplayKbd_PollHostDisplay(ARMul_State *state); /* Called at start of each frame */
HostPixel DisplayKbd_HostColour(ARMul_State *state,unsigned int col); /* Convert 13-bit VIDC physical colour to host format */
void DisplayKbd_HostChangeMode(ARMul_State *state,int width,int height,int hz); /* Try and change to the given screen mode */
HostPixel *DisplayKbd_GetScanline(ARMul_State *state,int line); /* Return pointer to start of host scanline in host video memory */


/* Functions defined in DispKbdShared.c */
void DisplayKbd_Init(ARMul_State *state);
void VIDC_PutVal(ARMul_State *state,ARMword address, ARMword data,int bNw);

#endif
