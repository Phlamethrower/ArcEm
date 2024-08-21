/* (c) David Alan Gilbert 1995-1999 - see Readme file for copying info */
/* Display and keyboard interface for the Arc emulator */

// #define MOUSEKEY XK_KP_Add

#define KEYREENABLEDELAY 1000

/*#define DEBUG_VIDCREGS*/

#include <stdio.h>
#include <limits.h>
#include <time.h>

#include "kernel.h"
#include "swis.h"

#define  ArcEmKey_GetKey 0x53340

#include "../armdefs.h"
#include "armarc.h"
#include "arch/keyboard.h"
#include "archio.h"
#include "hdc63463.h"
#include "../armemu.h"
#include "arch/displaydev.h"

#include "ControlPane.h"

static void UpdateCursorPos(ARMul_State *state);
static void SelectROScreenMode(int x, int y, int bpp);

static void set_cursor_palette(unsigned int *pal);

static int MonitorWidth;
static int MonitorHeight;
int MonitorBpp;


#ifndef PROFILE_ENABLED /* Profiling code uses a nasty hack to estimate program size, which will only work if we're using the wimpslot for our heap */
const char * const __dynamic_da_name = "ArcEm Heap";
#endif

static const ARMword ModeVarsIn[5] = {
 11, /* Width-1 */
 12, /* Height-1 */
 6, /* Bytes per line */
 148, /* Address */
 -1,
};

static ARMword ModeVarsOut[4];

/* ------------------------------------------------------------------ */

/* Standard display device */

typedef unsigned short SDD_HostColour;
#define SDD_Name(x) sdd_##x
static const int SDD_RowsAtOnce = 1;
typedef SDD_HostColour *SDD_Row;


static SDD_HostColour SDD_Name(Host_GetColour)(ARMul_State *state,unsigned int col)
{
  /* Convert to 5-bit component values */
  int r = (col & 0x00f) << 1;
  int g = (col & 0x0f0) >> 3;
  int b = (col & 0xf00) >> 7;
  /* May want to tweak this a bit at some point? */
  r |= r>>4;
  g |= g>>4;
  b |= b>>4;
#if 0
  /* Red/blue swapped Iyonix :( */
  return (r<<10) | (g<<5) | (b);
#else
  return (r) | (g<<5) | (b<<10);
#endif
}  

static void SDD_Name(Host_ChangeMode)(ARMul_State *state,int width,int height,int hz);

static inline SDD_Row SDD_Name(Host_BeginRow)(ARMul_State *state,int row,int offset)
{
  return ((SDD_Row) (ModeVarsOut[3] + ModeVarsOut[2]*row))+offset;
}

static inline void SDD_Name(Host_EndRow)(ARMul_State *state,SDD_Row *row) { /* nothing */ };

static inline void SDD_Name(Host_BeginUpdate)(ARMul_State *state,SDD_Row *row,unsigned int count) { /* nothing */ };

static inline void SDD_Name(Host_EndUpdate)(ARMul_State *state,SDD_Row *row) { /* nothing */ };

static inline void SDD_Name(Host_SkipPixels)(ARMul_State *state,SDD_Row *row,unsigned int count) { (*row) += count; }

static inline void SDD_Name(Host_WritePixel)(ARMul_State *state,SDD_Row *row,SDD_HostColour pix) { *(*row)++ = pix; }

static inline void SDD_Name(Host_WritePixels)(ARMul_State *state,SDD_Row *row,SDD_HostColour pix,unsigned int count) { while(count--) *(*row)++ = pix; }

void SDD_Name(Host_PollDisplay)(ARMul_State *state);

#include "../arch/stddisplaydev.c"

static void SDD_Name(Host_ChangeMode)(ARMul_State *state,int width,int height,int hz)
{
  /* TODO - Try and change mode
     For now, just use the current mode */
  _swix(OS_ReadVduVariables,_INR(0,1),ModeVarsIn,ModeVarsOut);
  HD.Width = ModeVarsOut[0]+1;
  HD.Height = ModeVarsOut[1]+1;
  HD.XScale = 1;
  HD.YScale = 1;
  /* Try and detect rectangular pixel modes */
  if((width >= height*2) && (height*2 <= HD.Height))
    HD.YScale = 2;
#if 0 /* Too slow at the moment, mainly due to lame vertical scaling */
  /* Apply global 2* scaling if possible */
  if((width*2 <= HD.Width) && (height*(HD.YScale+1) <= HD.Height))
  {
    HD.XScale++;
    HD.YScale++;
  }
#endif
  /* Screen is expected to be cleared */
  _swix(OS_WriteC,_IN(0),12);
}


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

  /* TODO - Support for more cursor scales, e.g. 2x2 */
  if(height && (height <= 16) && (HOSTDISPLAY.YScale == 2) && (HOSTDISPLAY.XScale == 1))
  {
    /* line-double the cursor image */
    static ARMword double_data[2*32];
    int i;
    for(i=0;i<height;i++)
    {
      double_data[i*4+0] = double_data[i*4+2] = pointer_data[i*2];
      double_data[i*4+1] = double_data[i*4+3] = pointer_data[i*2+1];
    }
    height *= 2;
    pointer_data = double_data;
  }    

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
SDD_Name(Host_PollDisplay)(ARMul_State *state)
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
int
DisplayDev_Init(ARMul_State *state)
{
//  SelectROScreenMode(640, 480, 4);
  SelectROScreenMode(800, 600, 4);
  return DisplayDev_Set(state,&SDD_DisplayDev);
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

  internal_x=(VIDC.Horiz_CursorStart-(VIDC.Horiz_DisplayStart*2))*HOSTDISPLAY.XScale+HOSTDISPLAY.XOffset;
  internal_y=(VIDC.Vert_CursorStart-VIDC.Vert_DisplayStart)*HOSTDISPLAY.YScale+HOSTDISPLAY.YOffset;

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
Kbd_PollHostKbd(ARMul_State *state)
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

