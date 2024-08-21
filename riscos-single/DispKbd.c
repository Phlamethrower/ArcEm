/* (c) David Alan Gilbert 1995-1999 - see Readme file for copying info */
/* Display and keyboard interface for the Arc emulator */

// #define MOUSEKEY XK_KP_Add

#define KEYREENABLEDELAY 1000

/*#define DEBUG_VIDCREGS*/
/* NOTE: Can't use ARMul's refresh function because it has a small
   limit on the time delay from posting the event to it executing   */
/* It's actually decremented once every POLLGAP - that is called
   with the ARMul scheduler */

#include <stdio.h>
#include <limits.h>
#include <time.h>

#include "kernel.h"
#include "swis.h"

#define  ArcEmKey_GetKey 0x53340

#include "../armdefs.h"
#include "armarc.h"
#include "arch/keyboard.h"
#include "DispKbd.h"
#include "archio.h"
#include "hdc63463.h"


#include "ControlPane.h"

#define ControlHeight 30
#define CURSORCOLBASE 250

#define HD HOSTDISPLAY
#define DC DISPLAYCONTROL

static void UpdateCursorPos(ARMul_State *state);
static void SelectROScreenMode(int x, int y, int bpp);

static void set_cursor_palette(unsigned int *pal);

static int MonitorWidth;
static int MonitorHeight;
int MonitorBpp;


#ifndef PROFILE_ENABLED /* Profiling code uses a nasty hack to estimate program size, which will only work if we're using the wimpslot for our heap */
const char * const __dynamic_da_name = "ArcEm Heap";
#endif

/* ------------------------------------------------------------------ */

static void set_cursor_palette(unsigned int *pal)
{
    int c;

    for(c = 0; c < 3; c++) {
        _swi(OS_WriteI + 19, 0);
        _swi(OS_WriteI + c + 1, 0); /* For `real' pointer colours. */
        _swi(OS_WriteI + 25, 0);
        _swi(OS_WriteC, _IN(0), (pal[c] & 0xf) * 0x11);
        _swi(OS_WriteC, _IN(0), (pal[c] >> 4 & 0xf) * 0x11);
        _swi(OS_WriteC, _IN(0), (pal[c] >> 8 & 0xf) * 0x11);
    }

    return;
}

/* ------------------------------------------------------------------ */

/* Refresh the mouse's image                                                    */
static void RefreshMouse(ARMul_State *state) {
  int height;
  ARMword *pointer_data = MEMC.PhysRam + ((MEMC.Cinit * 16)/4);

  height = VIDC.Vert_CursorEnd - VIDC.Vert_CursorStart;

  {
    char block[10];

    block[0] = 0;
    block[1] = 2;
    block[2] = 8;
    block[3] = height;
    block[4] = 0;
    block[5] = 0;
    block[6] = ((int) pointer_data) & 0x000000FF;
    block[7] = (((int) pointer_data) & 0x0000FF00)>>8;
    block[8] = (((int) pointer_data) & 0x00FF0000)>>16;
    block[9] = (((int) pointer_data) & 0xFF000000)>>24;
    _swi(OS_Word,_INR(0,1), 21, &block);
  }

  _swi(OS_Byte, _INR(0,1), 106, 2+(1<<7));

  UpdateCursorPos(state);
  set_cursor_palette(VIDC.CursorPalette);
}; /* RefreshMouse */


void
DisplayKbd_PollHostDisplay(ARMul_State *state)
{
  RefreshMouse(state);

#if 1
  static clock_t oldtime;
  static ARMword oldcycles;
  clock_t nowtime2 = clock();
  if((nowtime2-oldtime) > CLOCKS_PER_SEC)
  {
    const float scale = ((float)CLOCKS_PER_SEC)/1000000.0f;
    float mhz = scale*((float)(ARMul_Time-oldcycles))/((float)(nowtime2-oldtime));
    printf("\x1e%.2fMHz\n",mhz);
    oldcycles = ARMul_Time;
    oldtime = nowtime2;
  }
#endif
}; /* DisplayKbd_PollHostDisplay */

/*-----------------------------------------------------------------------------*/
void
DisplayKbd_InitHost(ARMul_State *state)
{
//  SelectROScreenMode(640, 480, 4);
  SelectROScreenMode(800, 600, 4);
} /* DisplayKbd_InitHost */


/*-----------------------------------------------------------------------------*/
static void ProcessKey(ARMul_State *state, int key, int transition) {
      /* Now add it to the buffer */
      KBD.Buffer[KBD.BuffOcc].KeyColToSend=key % 16;
      KBD.Buffer[KBD.BuffOcc].KeyRowToSend=key / 16;
      KBD.Buffer[KBD.BuffOcc].KeyUpNDown=transition ? 0 : 1;
      KBD.BuffOcc++;
#ifdef DEBUG_KBD
      fprintf(stderr,"ProcessKey: Got Col,Row=%d,%d UpNDown=%d BuffOcc=%d\n",
              KBD.Buffer[KBD.BuffOcc].KeyColToSend,
               KBD.Buffer[KBD.BuffOcc].KeyRowToSend,
               KBD.Buffer[KBD.BuffOcc].KeyUpNDown,
              KBD.BuffOcc);
#endif
}; /* ProcessKey */

/*-----------------------------------------------------------------------------*/
/* Move the Control pane window                                                */
static void UpdateCursorPos(ARMul_State *state) {
  int internal_x, internal_y;
  char block[5];
  int xeig=_swi(OS_ReadModeVariable,_INR(0,1)|_RETURN(2),-1,4);
  int yeig=_swi(OS_ReadModeVariable,_INR(0,1)|_RETURN(2),-1,5);

  internal_x=(VIDC.Horiz_CursorStart-(VIDC.Horiz_DisplayStart*2))+HOSTDISPLAY.XOffset;
  internal_y=(VIDC.Vert_CursorStart-VIDC.Vert_DisplayStart)+HOSTDISPLAY.YOffset;

  block[0]=5;
  {
    short x = internal_x << xeig;
    block[1] = x & 255;
    block[2] = x >> 8;
  }
  {
    short y = (MonitorHeight-internal_y) << yeig;
    block[3] = y & 255;
    block[4] = y >> 8;
  }

  _swi(OS_Word, _INR(0,1), 21, &block);

}; /* UpdateCursorPos */

/*-----------------------------------------------------------------------------*/
/* Called on an X motion event */
static void MouseMoved(ARMul_State *state, int mousex, int mousey/*,XMotionEvent *xmotion*/) {
  int xdiff,ydiff;
  /* Well the coordinates of the mouse cursor are now in xmotion->x and
    xmotion->y, I'm going to compare those against the cursor position
    and transmit the difference.  This can't possibly take care of the OS's
    hotspot offsets */

  /* We are now only using differences from the reference position */
  if ((mousex==MonitorWidth/2) && (mousey==MonitorHeight/2)) return;

  {
    char block[5];
    int x=MonitorWidth/2;
    int y=MonitorHeight/2;

    block[0]=3;
    block[1]=x % 256;
    block[2]=x / 256;
    block[3]=y % 256;
    block[4]=y / 256;

    _swi(OS_Word, _INR(0,1), 21, &block);
  }

#ifdef DEBUG_MOUSEMOVEMENT
  fprintf(stderr,"MouseMoved: CursorStart=%d xmotion->x=%d\n",
          VIDC.Horiz_CursorStart,mousex);
#endif
  xdiff=mousex-MonitorWidth/2;
  if (KBD.MouseXCount!=0) {
    if (KBD.MouseXCount & 64) {
      signed char tmpC;
      int tmpI;
      tmpC=KBD.MouseXCount | 128;
      tmpI=(signed int)tmpC;
      xdiff+=tmpI;
    } else {
      xdiff+=KBD.MouseXCount;
    };
  };

  if (xdiff>63) xdiff=63;
  if (xdiff<-63) xdiff=-63;

  ydiff=mousey-MonitorHeight/2;
  if (KBD.MouseYCount & 64) {
    signed char tmpC;
    tmpC=KBD.MouseYCount | 128; /* Sign extend */
    ydiff+=tmpC;
  } else {
    ydiff+=KBD.MouseYCount;
  };
  if (ydiff>63) ydiff=63;
  if (ydiff<-63) ydiff=-63;

  KBD.MouseXCount=xdiff & 127;
  KBD.MouseYCount=ydiff & 127;

#ifdef DEBUG_MOUSEMOVEMENT
  fprintf(stderr,"MouseMoved: generated counts %d,%d xdiff=%d ydifff=%d\n",KBD.MouseXCount,KBD.MouseYCount,xdiff,ydiff);
#endif
}; /* MouseMoved */

/*----------------------------------------------------------------------------*/
int
DisplayKbd_PollHostKbd(ARMul_State *state)
{
  /* Keyboard handling */
  {
    int key;
    int transition;

    if (_swi (ArcEmKey_GetKey, _RETURN(0)|_OUTR(1,2), &transition, &key))
    {
      //printf("Processing key %d, transition %d\n",key, transition);
      ProcessKey(state, key, transition);
    }
  }

  /* Mouse handling */
  {
    int mousex;
    int mousey;
    _swi(OS_Mouse, _OUTR(0,1), &mousex, &mousey);

    MouseMoved(state, mousex, mousey);
  }

  return 0;
} /* DisplayKbd_PollHostKbd */


/*-----------------------------------------------------------------------------*/

static void SelectROScreenMode(int x, int y, int bpp)
{
  int block[8];

//  if (x<0 || x>1024 || y<0 || y>768 || bpp<0 || bpp>3)
//    return;

  if (x==MonitorWidth && y==MonitorHeight && bpp==MonitorBpp)
    return;

  printf("setting screen mode to %dx%d at %d bpp\n",x, y, bpp);

  MonitorWidth  = x;
  MonitorHeight = y;
  MonitorBpp    = bpp;

  block[0] = 1;
  block[1] = x;
  block[2] = y;
  block[3] = bpp;
  block[4] = -1;

  switch (bpp)
  {
    case 3:
      block[5] = 3;
      block[6] = 255;
      break;

    default:
      block[5] =-1;
      break;
  }

  block[7] = -1;
  _swix(OS_ScreenMode, _INR(0,1), 0, &block);

  /* Remove text cursor from real RO */
  _swi(OS_RemoveCursors, 0);
}

HostPixel DisplayKbd_HostColour(ARMul_State *state,unsigned int col)
{
  /* Convert to 5-bit component values */
  int r = (col & 0x00f) << 1;
  int g = (col & 0x0f0) >> 3;
  int b = (col & 0xf00) >> 7;
  /* May want to tweak this a bit at some point? */
  r |= r>>4;
  g |= g>>4;
  b |= b>>4;
#if 1
  /* Red/blue swapped Iyonix :( */
  return (r<<10) | (g<<5) | (b);
#else
  return (r) | (g<<5) | (b<<10);
#endif
}

static const ARMword ModeVarsIn[5] = {
 11, /* Width-1 */
 12, /* Height-1 */
 6, /* Bytes per line */
 148, /* Address */
 -1,
};

static ARMword ModeVarsOut[4];
  

void DisplayKbd_HostChangeMode(ARMul_State *state,int width,int height,int hz)
{
  /* TODO - Try and change mode
     For now, just use the current mode */
  _swix(OS_ReadVduVariables,_INR(0,1),ModeVarsIn,ModeVarsOut);
  HD.Width = ModeVarsOut[0]+1;
  HD.Height = ModeVarsOut[1]+1;
  HD.XScale = 1;
  HD.YScale = 1;
#if 0
  /* Scale up to fill screen */
  HD.XScale = HD.Width/width;
  HD.YScale = HD.Height/height;
#endif
  /* Screen is expected to be cleared */
  _swix(OS_WriteC,_IN(0),12);
}

HostPixel *DisplayKbd_GetScanline(ARMul_State *state,int line)
{
  return (HostPixel *) (ModeVarsOut[3] + ModeVarsOut[2]*line);
}
