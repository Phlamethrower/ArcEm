/*
   arch/paldisplaydev.c

   (c) 2011 Jeffrey Lee <me@phlamethrower.co.uk>
   Based in part on original video code by David Alan Gilbert et al 

   Part of Arcem, covered under the GNU GPL, see file COPYING for details

   This is the core implementation of the palettised display driver. It's a
   driver for use with palettised output modes. Overall it's faster and uses
   less memory than the standard display driver, but it lacks the ability to
   cope with advanced things like mid-frame palette swaps. Border and cursor
   display is the responsiblity of the host.

   The output buffer is updated once per frame, at the start of the vblank
   period. 
*/

/*
   This file is intended to be #included directly by another source file in
   order to generate a working display driver.

   Before including the file, be sure to define the following
   types/symbols/macros:

   PDD_Name(x)
    - Macro used to convert symbol name 'x' into an instance-specific version
      of the symbol. e.g.
      "#define PDD_Name(x) MyPDD_##x"
   void PDD_Name(Host_ChangeMode)(ARMul_State *state,int width,int height,int depth,int hz)
    - Function that gets called when the display mode needs changing.
   void PDD_Name(Host_SetPaletteEntry)(ARMul_State *state,int i,unsigned int phys)
    - Function that gets called when a palette entry needs updating
   ARMword *PDD_Name(Host_GetRow)(ARMul_State *state,int row,int offset,int *outoffset)
    - Function that gets called to get a pointer to a row.
    - The returned pointer must be ARMword-aligned, and offset must be the bit offset to start rendering at (0-31)
   void PDD_Name(Host_PollDisplay)(ARMul_State *state)
    - Function that gets called after rendering each frame

*/




/*

  Main struct

  A pointer to this is placed in state->Display

  TODO - 'Control' and 'HostDisplay' should probably be merged together (or
  just disposed of entirely, so everything is directly under DisplayInfo)

*/

struct PDD_Name(DisplayInfo) {
  /* Raw VIDC registers - must come first! */
  struct Vidc_Regs Vidc;

  struct {
    /* Values which get updated by VIDCPutVal */

    unsigned int DirtyPalette; /* Bit flags of which palette entries have been modified */
    char ModeChanged; /* Set if any registers change which may require the host to change mode. Remains set until valid mode is available from host (suspends all display output) */
    char ForceRefresh; /* Set if the entire screen needs redrawing */

    /* Values that must only get updated by the event queue/screen blit code */
    
    char DMAEn; /* 1/0 whether video DMA is enabled for this frame */
    int LastHostWidth,LastHostHeight,LastHostDepth,LastHostHz; /* Values we used to request host mode */
    int BitWidth; /* Width of display area, in bits */
    unsigned int VIDC_CR; /* Control register value in use for this frame */
    unsigned int Vptr; /* DMA pointer, in bits, as offset from start of phys RAM */
  } Control;

  struct {
    /* The host must update these on calls to Host_ChangeMode */
    int Width,Height,XScale,YScale; /* Host display mode */
    ARMword *ExpandTable; /* Expansion table to use, 0 for none */
    unsigned int ExpandFactor;

    /* The core handles these */
    int XOffset,YOffset; /* X & Y offset of first display pixel in host */
    unsigned int UpdateFlags[(512*1024)/UPDATEBLOCKSIZE]; /* Update flags to track display writes */
  } HostDisplay;
};


/*

  General macros

*/

#ifdef DISPLAYINFO
#undef DISPLAYINFO
#endif
#define DISPLAYINFO (*((struct PDD_Name(DisplayInfo) *) state->Display))

#ifdef HOSTDISPLAY
#undef HOSTDISPLAY
#endif
#define HOSTDISPLAY (DISPLAYINFO.HostDisplay)

#ifdef DISPLAYCONTROL
#undef DISPLAYCONTROL
#endif
#define DISPLAYCONTROL (DISPLAYINFO.Control)

#ifdef DC
#undef DC
#endif
#define DC DISPLAYCONTROL

#ifdef HD
#undef HD
#endif
#define HD HOSTDISPLAY

#define VideoRelUpdateAndForce(flag, writeto, from) \
{\
  if ((writeto) != (from)) { \
    (writeto) = (from);\
    flag = 1;\
  };\
};

#define ROWFUNC_FORCE 0x1 /* Force row to be fully redrawn */

#define ROWFUNC_UPDATED 0x2 /* Flag used internally by rowfuncs to indicate whether anything was done */

/*

  Routine to copy a bitstream from one bit-aligned location to another

  dest & src pointers must be ARMword aligned
  destalign & srcalign must be 0 <= x < 32
  count is in bits

*/

static void BitCopy(ARMword *dest,int destalign,const ARMword *src,int srcalign,int count)
{
  /* Get the destination word aligned */
  int invdestalign = 32-destalign;
  int invsrcalign = 32-srcalign;
  ARMword tempsrc1 = *src++;
  ARMword tempsrc2 = *src++;
  if(destalign)
  {
    ARMword tempdest = *dest;

    ARMword mask = (count<=invdestalign?(1<<count):0)-1;
    mask = mask<<destalign;
    tempdest &= ~mask;
    ARMword tempsrc3 = (tempsrc1>>srcalign);
    if(srcalign)
      tempsrc3 |= (tempsrc2<<invsrcalign);
    tempdest |= (tempsrc1<<destalign) & mask;
    *dest++ = tempdest;

    count -= invdestalign;
    
    if(count <= 0)
    {
      /* Nothing else to copy */
      return;
    }

    srcalign += invdestalign;
    if(srcalign >= 32)
    {
      tempsrc1 = tempsrc2;
      tempsrc2 = *src++;
      srcalign &= 0x1f;
    }
    invsrcalign = 32-srcalign;
  }
  if(!srcalign && (count>64)) /* TODO - Tweak this value */
  {
    /* Matching alignment, memcpy up to the end */
    int temp = count>>5;
    memcpy(dest,src-2,temp<<2);
    dest += temp;
    tempsrc1 = src[temp-2];
    count &= 0x1f;
  }
  else while(count >= 32)
  {
    *dest++ = (tempsrc1>>srcalign) | (tempsrc2<<invsrcalign);
    tempsrc1 = tempsrc2;
    tempsrc2 = *src++;
    count -= 32;
  }
  if(count)
  {
    /* End bits */
    ARMword tempdest = *dest;
    ARMword mask = 0xffffffff<<count;
    tempdest &= mask;
    tempsrc1 = (tempsrc1>>srcalign);
    if(srcalign)
      tempsrc1 |= (tempsrc2<<invsrcalign);
    tempdest |= tempsrc1 &~ mask;
    *dest = tempdest;
  }
}

/*

  Routine to generate lookup tables for fast upscaling/expansion

  dest = destination buffer, must be appropriate size
  srcbpp = src BPP (1,2,4,8)
  factor = log2 of how much bigger each dest pixel is (1 <= factor <= log2(32/srcbpp))
  mul = multiplier (0x1 for simple BPP conversion, 0x11 for 4-bit 2X upscale, etc.)

*/

static void GenExpandTable(ARMword *dest,unsigned int srcbpp,unsigned int factor,unsigned int mul)
{
  unsigned int destbpp = srcbpp<<factor;
  unsigned int pixels = 1;
  while((destbpp*pixels < 32) && (srcbpp*pixels < 8))
    pixels <<= 1;
  unsigned int srcbits = 1<<(srcbpp*pixels);
  ARMword i,j;
  for(i=0;i<srcbits;i++)
  {
    ARMword out = 0;
    for(j=0;j<pixels;j++)
    {
      ARMword pixel = (i>>(j*srcbpp)) & ((1<<srcbpp)-1);
      pixel = pixel*mul;
      out |= pixel<<(j*destbpp);
    }
    *dest++ = out;
  }
}

/*

  Return size (entry count) for expand table

*/

static int GetExpandTableSize(unsigned int srcbpp,unsigned int factor)
{
  /* Work out how many source pixels we can lookup before we reach:
     (a) 32 output bits
     or
     (b) 256 table entries
  */
  unsigned int destbpp = srcbpp<<factor;
  unsigned int pixels = 1;
  while((destbpp*pixels < 32) && (srcbpp*pixels < 8))
    pixels <<= 1; /* Should only need to consider powers of two */
  unsigned int srcbits = 1<<(srcbpp*pixels);
  return srcbits;
}

/*

  Routine to copy a bitstream from one bit-aligned location to another, while
  expanding the data via a lookup table

  dest & src pointers must be ARMword aligned
  destalign & srcalign must be 0 <= x < 32
  count = available source data, in bits, multiple of srcbpp
  srcbpp = src BPP (1,2,4,8)
  factor = log2 of how much bigger each dest pixel is (1 <= factor <= log2(32/srcbpp))
  destalign must be multiple of srcbpp<<factor
*/

static void BitCopyExpand(ARMword *dest,int destalign,const ARMword *src,int srcalign,int count,const ARMword *expandtable,unsigned int srcbpp,unsigned int factor)
{
  unsigned int destbpp = srcbpp<<factor;
  unsigned int pixels = 1;
  while((destbpp*pixels < 32) && (srcbpp*pixels < 8))
    pixels <<= 1;
  unsigned int srcbits = srcbpp*pixels;
  unsigned int destbits = destbpp*pixels;
  ARMword srcmask = (1<<srcbits)-1;
  /* Get the destination word aligned */
  int invdestalign = 32-destalign;
  int invsrcalign = 32-srcalign;
  ARMword tempsrc1 = *src++;
  ARMword tempsrc2 = *src++;
  if(destalign)
  {
    ARMword tempdest = *dest;
    ARMword destmask = (count<=invdestalign?(1<<count):0)-1;
    destmask = destmask<<destalign;
    tempdest &= ~destmask;
    ARMword tempsrc3 = (tempsrc1>>srcalign);
    if(srcalign)
      tempsrc3 |= (tempsrc2<<invsrcalign);
    while(destalign < 32)
    {
      ARMword in = expandtable[tempsrc3 & ((1<<srcbpp)-1)]; /* One pixel at a time for simplicity */
      tempdest |= in<<destalign;
      srcalign += srcbpp;
      destalign += destbpp;
      tempsrc3 >>= srcbpp;
      count -= srcbpp;
    }
    *dest++ = tempdest;
    if(srcalign >= 32)
    {
      srcalign &= 0x1f;
      tempsrc1 = tempsrc2;
      tempsrc2 = *src++;
    }
    invsrcalign = 32-srcalign;
  }
  if(destbits < 32)
  {
    /* Table isn't big enough for us to get one full word per entry */
    ARMword tempsrc3 = (tempsrc1>>srcalign);
    if(srcalign)
      tempsrc3 |= (tempsrc2<<invsrcalign);
    int remaining = 32;
    ARMword tempdest = 0;
    int outshift = 0;
    /* There will be (count<<factor)>>5 full words available, and we want to stop when there's less than one full word left */
    int bitsleft = (count<<factor)&0x1f; 
    while(count > bitsleft)
    {
      tempdest |= expandtable[tempsrc3 & srcmask]<<outshift;
      outshift += destbits;
      if(outshift >= 32)
      {
        *dest++ = tempdest;
        tempdest = 0;
        outshift = 0;
      }
      tempsrc3 >>= srcbits;
      remaining -= srcbits;
      count -= srcbits;
      srcalign += srcbits;
      if(!remaining)
      {
        if(srcalign >= 32)
        {
          srcalign &= 0x1f;
          tempsrc1 = tempsrc2;
          tempsrc2 = *src++;
        }
        invsrcalign = 32-srcalign;
        tempsrc3 = (tempsrc1>>srcalign);
        if(srcalign)
          tempsrc3 |= (tempsrc2<<invsrcalign);
        remaining = 32;
      }
    }
  }
  else
  {
    /* One output word for every srcbits */
    ARMword tempsrc3 = (tempsrc1>>srcalign);
    if(srcalign)
      tempsrc3 |= (tempsrc2<<invsrcalign);
    int remaining = 32;
    while(count >= srcbits)
    {
      *dest++ = expandtable[tempsrc3 & srcmask];
      tempsrc3 >>= srcbits;
      remaining -= srcbits;
      count -= srcbits;
      srcalign += srcbits;
      if(!remaining)
      {
        if(srcalign >= 32)
        {
          srcalign &= 0x1f;
          tempsrc1 = tempsrc2;
          tempsrc2 = *src++;
        }
        invsrcalign = 32-srcalign;
        tempsrc3 = (tempsrc1>>srcalign);
        if(srcalign)
          tempsrc3 |= (tempsrc2<<invsrcalign);
        remaining = 32;
      }
    }
  }
  if(count)
  {
    /* End bits */
    ARMword tempdest = *dest;
    ARMword destmask = 0xffffffff<<(count<<factor);
    tempdest &= destmask;
    ARMword tempsrc3 = (tempsrc1>>srcalign);
    if(srcalign)
      tempsrc3 |= (tempsrc2<<invsrcalign);
    int outshift = 0;
    while(count)
    {    
      ARMword in = expandtable[tempsrc3 & ((1<<srcbpp)-1)]; /* One pixel at a time for simplicity */
      tempdest |= in<<outshift;
      srcalign += srcbpp;
      outshift += destbpp;
      tempsrc3 >>= srcbpp;
      count -= srcbpp;
    }
    *dest = tempdest;
  }
}

/*

  Row output for 1X horizontal scaling to same-depth display buffer

*/

static inline int PDD_Name(RowFunc1XSameBitAligned)(ARMul_State *state,ARMword *out,int outoffset,int flags)
{
  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const ARMword *RAM = MEMC.PhysRam;
  int Remaining = DC.BitWidth;

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));

    if((flags & ROWFUNC_FORCE) || (HD.UpdateFlags[FlagsOffset] != MEMC.UpdateFlags[FlagsOffset]))
    {
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      BitCopy(out,outoffset,RAM+(Vptr>>5),Vptr&0x1f,Available);
    }
    outoffset += Available;
    out += outoffset>>5;
    outoffset &= 0x1f;
    Vptr += Available;
    Remaining -= Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }

  DC.Vptr = Vptr;

  return (flags & ROWFUNC_UPDATED);
}

static inline int PDD_Name(RowFunc1XSameByteAligned)(ARMul_State *state,char *out,int flags)
{
  unsigned int Vptr = DC.Vptr>>3;
  unsigned int Vstart = MEMC.Vstart<<4;
  unsigned int Vend = (MEMC.Vend+1)<<4; /* Point to pixel after end */
  const char *RAM = (char *) MEMC.PhysRam;
  int Remaining = DC.BitWidth>>3;

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/UPDATEBLOCKSIZE;
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));

    if((flags & ROWFUNC_FORCE) || (HD.UpdateFlags[FlagsOffset] != MEMC.UpdateFlags[FlagsOffset]))
    {
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      memcpy(out,RAM+Vptr,Available);
    }
    out += Available;
    Vptr += Available;
    Remaining -= Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }

  DC.Vptr = Vptr<<3;

  return (flags & ROWFUNC_UPDATED);
}

/*

  Row output via ExpandTable

*/

static inline int PDD_Name(RowFuncExpandTable)(ARMul_State *state,ARMword *out,int outoffset,int flags)
{
  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const ARMword *RAM = MEMC.PhysRam;
  int Remaining = DC.BitWidth;

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));

    if((flags & ROWFUNC_FORCE) || (HD.UpdateFlags[FlagsOffset] != MEMC.UpdateFlags[FlagsOffset]))
    {
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      BitCopyExpand(out,outoffset,RAM+(Vptr>>5),Vptr&0x1f,Available,HD.ExpandTable,1<<DC.LastHostDepth,HD.ExpandFactor);
    }
    outoffset += Available<<HD.ExpandFactor;
    out += outoffset>>5;
    outoffset &= 0x1f;
    Vptr += Available;
    Remaining -= Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }

  DC.Vptr = Vptr;

  return (flags & ROWFUNC_UPDATED);
}


/*

  EventQ funcs

*/

static void PDD_Name(EventFunc)(ARMul_State *state,CycleCount nowtime)
{
  /* Assuming a multiplier of 2, these are the required clock dividers
     (selected via bottom two bits of VIDC.ControlReg): */
  static const unsigned char ClockDividers[4] = {
  /* Source rates:     24.0MHz     25.0MHz      36.0MHz */
    6, /* 1/3      ->   8.0MHz      8.3MHz      12.0MHz */
    4, /* 1/2      ->  12.0MHz     12.5MHz      18.0MHz */
    3, /* 2/3      ->  16.0MHz     16.6MHz      24.0MHz */
    2, /* 1/1      ->  24.0MHz     25.0MHz      36.0MHz */
  };

  const unsigned int NewCR = VIDC.ControlReg;
  const unsigned long ClockIn = 2*DisplayDev_GetVIDCClockIn();
  const unsigned char ClockDivider = ClockDividers[NewCR&3]; 

  /* Work out when to reschedule ourselves */
  long FramePeriod = (VIDC.Horiz_Cycle*2+2)*(VIDC.Vert_Cycle+1);
  CycleCount framelength = (((unsigned long long) ARMul_EmuRate)*FramePeriod)*ClockDivider/ClockIn;
  framelength = MAX(framelength,1000);
  EventQ_RescheduleHead(state,nowtime+framelength,PDD_Name(EventFunc));

  /* Trigger VSync interrupt */
  ioc.IRQStatus|=IRQA_VFLYBK;
  IO_UpdateNirq(state);

  /* Ensure mode changes if pixel clock changed */
  DC.ModeChanged |= (DC.VIDC_CR & 3) != (NewCR & 3);

  /* Force full refresh if DMA just toggled on/off */
  char newDMAEn = (MEMC.ControlReg>>10)&1;
  DC.ForceRefresh |= (newDMAEn ^ DC.DMAEn);
  DC.DMAEn = newDMAEn;

  /* Ensure full palette rebuild on BPP change */
  if((DC.VIDC_CR & 0xc) != (NewCR & 0xc))
  {
    DC.DirtyPalette = 65535;
  }

  DC.VIDC_CR = NewCR;

  int Depth = (NewCR>>2)&0x3;
  int Width = (VIDC.Horiz_DisplayEnd-VIDC.Horiz_DisplayStart)*2;
  int Height = (VIDC.Vert_DisplayEnd-VIDC.Vert_DisplayStart);
  int BPP = 1<<Depth;
  DC.BitWidth = Width*BPP;

  /* Handle any mode changes */
  if(DC.ModeChanged)
  {
    /* Work out new screen parameters
       TODO - Using display area isn't always appropriate, e.g. Stunt Racer 2000
       does some screen transitions by adjusting the display area height over
       time.
       Determining screen size via the borders would be better, but wouldn't
       allow the host to hide the borders and so use better display modes/scale
       factors. */
    if(Height <= 0)
    {
      /* Display output has been forced off by setting end addr before start
         Try using border size instead */
      Height = VIDC.Vert_BorderEnd-VIDC.Vert_BorderStart;
    }
    int FrameRate = ClockIn/(FramePeriod*ClockDivider);
    
    if((Width != DC.LastHostWidth) || (Height != DC.LastHostHeight) || (FrameRate != DC.LastHostHz) || (Depth != DC.LastHostDepth))
    {
      fprintf(stderr,"New mode: %dx%d, %dHz (CR %x ClockIn %dMhz)\n",Width,Height,FrameRate,NewCR,(int)(ClockIn/2000000));
      /* Try selecting new mode */
      if((Width < 1) || (Height < 1))
      {
        /* Bad mode; skip rendering */
        return;
      }
      
      DC.LastHostWidth = Width;
      DC.LastHostHeight = Height;
      DC.LastHostHz = FrameRate;
      DC.LastHostDepth = Depth;
      PDD_Name(Host_ChangeMode)(state,Width,Height,Depth,FrameRate);

      /* Calculate display offsets, for start of first display pixel */
      HD.XOffset = (HD.Width-Width*HD.XScale)/2;
      HD.YOffset = (HD.Height-Height*HD.YScale)/2;
      DC.ForceRefresh = 1;
    }
    DC.ModeChanged = 0;
  }

  /* Update host palette */
  if(DC.DirtyPalette)
  {
    int i;
    if(Depth == 3)
    {
      for(i=0;i<16;i++)
      {
        if(DC.DirtyPalette & (1<<i))
        {
          int j;
          /* Deal with the funky 8bpp palette */
          unsigned int Base = VIDC.Palette[i] & 0x1737; /* Only these bits of the palette entry are used in 8bpp modes */
          static const unsigned int ExtraPal[16] = {
            0x000, 0x008, 0x040, 0x048, 0x080, 0x088, 0x0c0, 0x0c8,
            0x800, 0x808, 0x840, 0x848, 0x880, 0x888, 0x8c0, 0x8c8
          };
          for(j=0;j<16;j++)
          {
            PDD_Name(Host_SetPaletteEntry)(state,i+(j<<4),Base | ExtraPal[j]);
          }
        }
      }
    }
    else
    {
      int num = 1<<BPP;
      for(i=0;i<num;i++)
      {
        if(DC.DirtyPalette & (1<<i))
        {
          PDD_Name(Host_SetPaletteEntry)(state,i,VIDC.Palette[i]);
        }
      }
    }
    DC.DirtyPalette = 0;
  }

  /* Set up DMA */
  DC.Vptr = MEMC.Vinit<<7;

  /* Render */
  int i;
  int flags = (DC.ForceRefresh?ROWFUNC_FORCE:0);
  for(i=0;i<Height;i++)
  {
    int hoststart = i*HD.YScale+HD.YOffset;
    int hostend = hoststart+HD.YScale;
    if(hoststart < 0)
      hoststart = 0;
    if(hostend > HD.Height)
      hostend = HD.Height;
    ARMword Vptr = DC.Vptr;
    while(hoststart < hostend)
    {
      DC.Vptr = Vptr;
      int outoffset;
      int updated;
      ARMword *out = PDD_Name(Host_GetRow)(state,hoststart++,HD.XOffset,&outoffset);
      if(HD.ExpandTable)
      {
        updated = PDD_Name(RowFuncExpandTable)(state,out,outoffset,flags);
      }
      else if(!(outoffset & 0x7) && !(Vptr & 0x7) && !((Width<<BPP) & 0x7))
      {
        updated = PDD_Name(RowFunc1XSameByteAligned)(state,((char *) out)+(outoffset>>3),flags);
      }
      else
      {
        updated = PDD_Name(RowFunc1XSameBitAligned)(state,out,outoffset,flags);
      }
      if(updated)
        flags |= ROWFUNC_UPDATED;
      else
        break;
    }
  }
  DC.ForceRefresh = 0;

  /* Update UpdateFlags */
  if(flags & ROWFUNC_UPDATED)
  {
    /* Only need to update between MIN(Vinit,Vstart) and Vend */
    int start = MIN(MEMC.Vinit,MEMC.Vstart)/(UPDATEBLOCKSIZE/16);
    int end = (MEMC.Vend/(UPDATEBLOCKSIZE/16))+1;
    memcpy(HD.UpdateFlags+start,MEMC.UpdateFlags+start,(end-start)*sizeof(unsigned int));
  }  

  /* Update host */
  PDD_Name(Host_PollDisplay)(state);

  /* Done! */
}

/*

  VIDC/IOEB write handler

*/

static void PDD_Name(VIDCPutVal)(ARMul_State *state,ARMword address, ARMword data,int bNw) {
  unsigned int addr, val;

  addr=(data>>24) & 255;
  val=data & 0xffffff;

  if (!(addr & 0xc0)) {
    int Log;
    int Phy;

    Log=(addr>>2) & 15;
    Phy = (val & 0x1fff);
    if(VIDC.Palette[Log] != Phy)
    {
      VIDC.Palette[Log] = Phy;
      DC.DirtyPalette |= (1<<Log);
    }
    return;
  };

  addr&=~3;
  switch (addr) {
    case 0x40: /* Border col */
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC border colour write val=0x%x\n",val);
#endif
      val &= 0x1fff;
      if(VIDC.BorderCol != val)
      {
        VIDC.BorderCol = val;
//        HD.BorderCol = SDD_Name(Host_GetColour)(state,val);
      }
      break;

    case 0x44: /* Cursor palette log col 1 */
    case 0x48: /* Cursor palette log col 2 */
    case 0x4c: /* Cursor palette log col 3 */
      addr = (addr-0x44)>>2;
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC cursor log col %d write val=0x%x\n",addr+1,val);
#endif
      VIDC.CursorPalette[addr] = val & 0x1fff;
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
#ifdef SOUND_SUPPORT
      Sound_StereoUpdated(state);
#endif
      break;

    case 0x80:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz cycle register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Horiz_Cycle,(val>>14) & 0x3ff);
      break;

    case 0x84:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz sync width register val=%d\n",val>>14);
#endif
      VIDC.Horiz_SyncWidth = (val>>14) & 0x3ff;
      break;

    case 0x88:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz border start register val=%d\n",val>>14);
#endif
      VIDC.Horiz_BorderStart = (val>>14) & 0x3ff;
      break;

    case 0x8c:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz display start register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Horiz_DisplayStart,(val>>14) & 0x3ff);
      break;

    case 0x90:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Horiz display end register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Horiz_DisplayEnd,(val>>14) & 0x3ff);
      break;

    case 0x94:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC horizontal border end register val=%d\n",val>>14);
#endif
      VIDC.Horiz_BorderEnd = (val>>14) & 0x3ff;
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
      VIDC.Horiz_Interlace = (val>>14) & 0x3ff;
      break;

    case 0xa0:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert cycle register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Vert_Cycle,(val>>14) & 0x3ff);
      break;

    case 0xa4:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert sync width register val=%d\n",val>>14);
#endif
      VIDC.Vert_SyncWidth = (val>>14) & 0x3ff;
      break;

    case 0xa8:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert border start register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Vert_BorderStart,((val>>14) & 0x3ff));
      break;

    case 0xac:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert disp start register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Vert_DisplayStart,((val>>14) & 0x3ff));
      break;

    case 0xb0:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert disp end register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Vert_DisplayEnd,(val>>14) & 0x3ff);
      break;

    case 0xb4:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Vert Border end register val=%d\n",val>>14);
#endif
      VideoRelUpdateAndForce(DC.ModeChanged,VIDC.Vert_BorderEnd,((val>>14) & 0x3ff));
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
#ifdef SOUND_SUPPORT
      Sound_SoundFreqUpdated(state);
#endif
      break;

    case 0xe0:
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC Control register val=0x%x\n",val);
#endif
      VIDC.ControlReg = val & 0xffff;
      break;

    default:
      fprintf(stderr,"Write to unknown VIDC register reg=0x%x val=0x%x\n",addr,val);
      break;

  }; /* Register switch */
}; /* PutValVIDC */

static void PDD_Name(IOEBCRWrite)(ARMul_State *state,ARMword data) {
  DC.ModeChanged = 1;
};

/*

  DisplayDev wrapper

*/

static int PDD_Name(Init)(ARMul_State *state,const struct Vidc_Regs *Vidc)
{
  state->Display = calloc(sizeof(struct PDD_Name(DisplayInfo)),1);
  if(!state->Display) {
    fprintf(stderr,"Failed to allocate DisplayInfo\n");
    return -1;
  }

  VIDC = *Vidc;

  DC.ModeChanged = 1;
  DC.LastHostWidth = DC.LastHostHeight = DC.LastHostHz = DC.LastHostDepth = -1;
  DC.DirtyPalette = 65535;
  DC.VIDC_CR = 0;
  DC.DMAEn = 0;

  memset(HOSTDISPLAY.UpdateFlags,0,sizeof(HOSTDISPLAY.UpdateFlags)); /* Initial value in MEMC.UpdateFlags is 1 */   

  /* Schedule first update event */
  EventQ_Insert(state,ARMul_Time+100,PDD_Name(EventFunc));

  return 0;
}

static void PDD_Name(Shutdown)(ARMul_State *state)
{
  int idx = EventQ_Find(state,PDD_Name(EventFunc));
  if(idx >= 0)
    EventQ_Remove(state,idx);
  free(state->Display);
  state->Display = NULL;
}

static void PDD_Name(DAGWrite)(ARMul_State *state,int reg,ARMword val)
{
  /* Vinit, Vstart & Vend changes require a full screen refresh */
  DC.ForceRefresh = 1;
}

const DisplayDev PDD_DisplayDev = {
  PDD_Name(Init),
  PDD_Name(Shutdown),
  PDD_Name(VIDCPutVal),
  PDD_Name(DAGWrite),
  PDD_Name(IOEBCRWrite),
};

/*

  The end

*/

#undef VideoRelUpdateAndForce
#undef ROWFUNC_FORCE
#undef ROWFUNC_UPDATEFLAGS
#undef ROWFUNC_UPDATED

