#include "armdefs.h"
#include "arch/armarc.h"
#include "arch/keyboard.h"
#include "arch/DispKbd.h"
#include "arch/archio.h"
#include "arch/hdc63463.h"
#ifdef SOUND_SUPPORT
#include "arch/sound.h"
#endif

#include "ControlPane.h"

#define DC DISPLAYCONTROL

void DisplayKbd_InitHost(ARMul_State *state)
{
	return;
}

int DisplayKbd_PollHost(ARMul_State *state)
{
	return 0;
}

void VIDC_PutVal(ARMul_State *state,ARMword address, ARMword data,int bNw)
{
  unsigned int addr, val;

  addr=(data>>24) & 255;
  val=data & 0xffffff;

  if (!(addr & 0xc0)) {
    int Log, Sup,Red,Green,Blue;

    /* This lot presumes it's not 8bpp mode! */
    Log=(addr>>2) & 15;
    Sup=(val >> 12) & 1;
    Blue=(val >> 8) & 15;
    Green=(val >> 4) & 15;
    Red=val & 15;
#ifdef DEBUG_VIDCREGS
    fprintf(stderr,"VIDC Palette write: Logical=%d Physical=(%d,%d,%d,%d)\n",
      Log,Sup,Red,Green,Blue);
#endif
    VideoRelUpdateAndForce(DC.MustResetPalette,VIDC.Palette[Log],(val & 0x1fff));
    return;
  };

  addr&=~3;
  switch (addr) {
    case 0x40: /* Border col */
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC border colour write val=0x%x\n",val);
#endif
      VideoRelUpdateAndForce(DC.MustResetPalette,VIDC.BorderCol,(val & 0x1fff));
      break;

    case 0x44: /* Cursor palette log col 1 */
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC cursor log col 1 write val=0x%x\n",val);
#endif
      VideoRelUpdateAndForce(DC.MustResetPalette,VIDC.CursorPalette[0],(val & 0x1fff));
      break;

    case 0x48: /* Cursor palette log col 2 */
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC cursor log col 2 write val=0x%x\n",val);
#endif
      VideoRelUpdateAndForce(DC.MustResetPalette,VIDC.CursorPalette[1],(val & 0x1fff));
      break;

    case 0x4c: /* Cursor palette log col 3 */
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC cursor log col 3 write val=0x%x\n",val);
#endif
      VideoRelUpdateAndForce(DC.MustResetPalette,VIDC.CursorPalette[2],(val & 0x1fff));
      break;

    case 0x60: /* Stereo image reg 7 */
    case 0x64: /* Stereo image reg 0 */
    case 0x68: /* Stereo image reg 1 */
    case 0x6c: /* Stereo image reg 2 */
    case 0x70: /* Stereo image reg 3 */
    case 0x74: /* Stereo image reg 4 */
    case 0x78: /* Stereo image reg 5 */
    case 0x7c: /* Stereo image reg 6 */
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC stereo image reg write val=0x%x\n",val);
#endif
      VIDC.StereoImageReg[(addr==0x60)?7:((addr-0x64)/4)]=val & 7;
      break;

    case 0x80:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz cycle register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Horiz_Cycle,(val>>14) & 0x3ff);
      break;

    case 0x84:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz sync width register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Horiz_SyncWidth,(val>>14) & 0x3ff);
      break;

    case 0x88:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz border start register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Horiz_BorderStart,(val>>14) & 0x3ff);
      break;

    case 0x8c:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz display start register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Horiz_DisplayStart,(val>>14) & 0x3ff);
      break;

    case 0x90:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz display end register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Horiz_DisplayEnd,(val>>14) & 0x3ff);
      break;

    case 0x94:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC horizontal border end register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Horiz_BorderEnd,(val>>14) & 0x3ff);
      break;

    case 0x98:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC horiz cursor start register val=%d\n",val>>13);
#endif
      VIDC.Horiz_CursorStart=(val>>13) & 0x7ff;
      break;

    case 0x9c:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC horiz interlace register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Horiz_Interlace,(val>>14) & 0x3ff);
      break;

    case 0xa0:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert cycle register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Vert_Cycle,(val>>14) & 0x3ff);
      break;

    case 0xa4:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert sync width register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Vert_SyncWidth,(val>>14) & 0x3ff);
      break;

    case 0xa8:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert border start register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Vert_BorderStart,(val>>14) & 0x3ff);
      break;

    case 0xac:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert disp start register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Vert_DisplayStart,((val>>14) & 0x3ff));
      break;

    case 0xb0:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert disp end register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Vert_DisplayEnd,(val>>14) & 0x3ff);
      break;

    case 0xb4:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert Border end register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.Vert_BorderEnd,(val>>14) & 0x3ff);
      break;

    case 0xb8:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert cursor start register val=%d\n",val>>14);
#endif
      VIDC.Vert_CursorStart=(val>>14) & 0x3ff;
      break;

    case 0xbc:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert cursor end register val=%d\n",val>>14);
#endif
      VIDC.Vert_CursorEnd=(val>>14) & 0x3ff;
      break;

    case 0xc0:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Sound freq register val=%d\n",val);
#endif
      VIDC.SoundFreq=val & 0xff;
      break;

    case 0xe0:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Control register val=0x%x\n",val);
#endif
      VideoRelUpdateAndForce(DC.MustRedraw,VIDC.ControlReg,val & 0xffff);
      break;

    default:
      fprintf(stderr,"Write to unknown VIDC register reg=0x%x val=0x%x\n",addr,val);
      break;

  }; /* Register switch */
}
