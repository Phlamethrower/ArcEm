/*
  DispKbdShared.c

  Shared between all platform ports. Shared Parts of Display and Input
  layer

  (c) 1996-2005 D Gilbert, P Howkins, et al

  Part of Arcem, covered under the GNU GPL, see file COPYING for details

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch/armarc.h"
#ifdef SOUND_SUPPORT
#include "arch/sound.h"
#endif
#include "../armemu.h"

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#define DC DISPLAYCONTROL
#define HD HOSTDISPLAY

/* How many rows for video to process at once */
#define ROWS_AT_ONCE 1

//#define VIDEO_STATS

#ifdef VIDEO_STATS
#define VIDEO_STAT(STAT,COND,AMT) if(COND) {vidstats[vidstat_##STAT] += AMT;}
#else
#define VIDEO_STAT(STAT,COND,AMT) ((void)0)
#endif

#ifdef VIDEO_STATS
enum vidstat {
  vidstat_BorderRedraw,
  vidstat_BorderRedrawForced,
  vidstat_BorderRedrawColourChanged,
  vidstat_DisplayRowRedraw,
  vidstat_DisplayRedraw,
  vidstat_DisplayRedrawForced,
  vidstat_DisplayRedrawUpdated,
  vidstat_DisplayBits,
  vidstat_DisplayRowForce,
  vidstat_DisplayFullForce,
  vidstat_DisplayRows,
  vidstat_DisplayFrames,
  vidstat_ForceRefreshDMA,
  vidstat_ForceRefreshBPP,
  vidstat_RefreshFlagsVinit,
  vidstat_RefreshFlagsPalette,
  vidstat_MAX,
};
static unsigned int vidstats[vidstat_MAX];
static const char *vidstatnames[vidstat_MAX] = {
 "BorderRedraw: Total border redraws",
 "BorderRedrawForced: Total forced border redraws",
 "BorderRedrawColourChanged: Total border redraws due to colour change",
 "DisplayRowRedraw: Number of rows where display data was updated",
 "DisplayRedraw: Number of blocks/sections updated",
 "DisplayRedrawForced: Number of forced blocks/sections updated",
 "DisplayRedrawUpdated: Number of blocks/sections updated",
 "DisplayBits: Number of display bits updated",
 "DisplayRowForce: Number of display row redraws due to DC.RefreshFlags",
 "DisplayFullForce: Number of display row redraws due to DC.ForceRefresh",
 "DisplayRows: Total number of rows processed",
 "DisplayFrames: Total number of frames processed",
 "ForceRefreshDMA: Frames where ForceRefresh was set due to DMA enable toggle",
 "ForceRefreshBPP: Frames where ForceRefresh was set due to BPP change",
 "RefreshFlagsVinit: Frames where RefreshFlags were set due to Vinit change",
 "RefreshFlagsPalette: Palette writes causing RefreshFlags to be set",
};

static void vidstats_Dump(const char *c)
{
  fprintf(stderr,"%s\n",c);
  int i;
  for(i=0;i<vidstat_MAX;i++)
  {
    fprintf(stderr,"%12u %s\n",vidstats[i],vidstatnames[i]);
    vidstats[i] = 0;
  }
}
#endif

/*

  Display - Palette updates

*/

static inline void Display_PaletteUpdate(ARMul_State *state,HostPixel *Palette,int num)
{
  /* Might be better if caller does this check? */
  if(DC.DirtyPalette)
  {
    int i;
    for(i=0;i<num;i++)
    {
      if(DC.DirtyPalette & (1<<i))
      {
        Palette[i] = DisplayKbd_HostColour(state,VIDC.Palette[i]);
      }
    }
    DC.DirtyPalette = 0;
  }
}

static inline void Display_PaletteUpdate8bpp(ARMul_State *state,HostPixel *Palette)
{
  /* Might be better if caller does this check? */
  if(DC.DirtyPalette)
  {
    int i;
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
          Palette[i+(j<<4)] = DisplayKbd_HostColour(state,Base | ExtraPal[j]);
        }
      }
    }
    DC.DirtyPalette = 0;
  }
}

/*

  Display - Host framebuffer interface

*/

#ifdef SYSTEM_X
#include "X/disphost.h"
#else
/* The implementation here is suitable for hosts that provide direct framebuffer access, and don't require notification of any region updates */

/* DHRow is a type that effectively acts as an iterator over the contents of a row/scanline in the host display
   It should iterate from the leftmost pixel to the right, and be tolerant of being copied for passing to functions */
typedef HostPixel *DHRow;

/* Return a DHRow suitable for accessing the indicated row, starting from the given X offset */
static inline DHRow DHRow_BeginRow(ARMul_State *state,int row,int offset)
{
  return DisplayKbd_GetScanline(state,row)+offset;
}

/* End the use of a DHRow instance. This will not be called for DHRow copies that were passed to the rendering functions. */
static inline void DHRow_EndRow(ARMul_State *state,DHRow *row) { /* nothing */ };

/* Indicate that we're about to begin updating 'count' pixels, starting from the current position */
static inline void DHRow_BeginUpdate(ARMul_State *state,DHRow *row,unsigned int count) { /* nothing */ };

/* Indicate that we've finished updating the pixels */
static inline void DHRow_EndUpdate(ARMul_State *state,DHRow *row) { /* nothing */ };

/* Skip ahead 'count' pixels */
static inline void DHRow_SkipPixels(ARMul_State *state,DHRow *row,unsigned int count) { (*row) += count; }

/* Write a single pixel and move ahead one */
static inline void DHRow_WritePixel(ARMul_State *state,DHRow *row,HostPixel pix) { *(*row)++ = pix; }

/* Set N consecutive pixels to the same value. 'count' may be zero. */
static inline void DHRow_WritePixels(ARMul_State *state,DHRow *row,HostPixel pix,unsigned int count) { while(count--) *(*row)++ = pix; }
#endif

/*

  Display - Screen output general

*/

/* Prototype of a function used for updating the display area of a row.
   'dhrow' is expected to already be pointing to the start of the display area
   Returns non-zero if the row was updated
*/
typedef int (*Display_RowFunc)(ARMul_State *state,int row,DHRow dhrow,int flags);


#define ROWFUNC_FORCE 0x1 /* Force row to be fully redrawn */
#define ROWFUNC_UPDATEFLAGS 0x2 /* Update the UpdateFlags */

#define ROWFUNC_UPDATED 0x4 /* Flag used internally by rowfuncs to indicate whether anything was done */

/*

  Display - Screen output for 1X horizontal scaling

*/

static int Display_RowFunc1bpp1X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate(state,Palette,2);

  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth;

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,(flags & ROWFUNC_FORCE),1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available);
      const unsigned char *In = RAM+(Vptr>>3);
      unsigned int Bit = 1<<(Vptr & 7);
      for(i=0;i<Available;i++)
      {
        int idx = (*In & Bit)?1:0;
        DHRow_WritePixel(state,&dhrow,Palette[idx]);
        Bit <<= 1;
        if(Bit == 256)
        {
          Bit = 1;
          In++;
        }
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

static int Display_RowFunc2bpp1X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate(state,Palette,4);

  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth*2; /* Scale up to account for everything else counting in bits */

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    /* Note: This is the number of available bits, not pixels */
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,(flags & ROWFUNC_FORCE),1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available>>1);
      const unsigned char *In = RAM+(Vptr>>3);
      unsigned int Shift = (Vptr & 7);
      for(i=0;i<Available;i+=2)
      {
        int idx = (*In >> Shift) & 3;
        DHRow_WritePixel(state,&dhrow,Palette[idx]);
        Shift += 2;
        if(Shift == 8)
        {
          Shift = 0;
          In++;
        }
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available>>1);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

static int Display_RowFunc4bpp1X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate(state,Palette,16);


  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth*4; /* Scale up to account for everything else counting in bits */

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    /* Note: This is the number of available bits, not pixels */
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,(flags & ROWFUNC_FORCE),1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available>>2);

      /* Display will always be a multiple of 2 pixels wide, so we can simplify things a bit compared to 1/2bpp case */
      const unsigned char *In = RAM+(Vptr>>3);      
      for(i=0;i<Available;i+=8)
      {
        unsigned char Pixel = *In++;
        DHRow_WritePixel(state,&dhrow,Palette[Pixel & 0xf]);
        DHRow_WritePixel(state,&dhrow,Palette[Pixel>>4]);
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available>>2);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

static int Display_RowFunc8bpp1X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate8bpp(state,Palette);

  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth*8; /* Scale up to account for everything else counting in bits */

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    /* Note: This is the number of available bits, not pixels */
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,force,1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available>>3);

      /* Display will always be a multiple of 2 pixels wide, so we can simplify things a bit compared to 1/2bpp case */
      const unsigned char *In = RAM+(Vptr>>3);      
      for(i=0;i<Available;i+=16)
      {
        DHRow_WritePixel(state,&dhrow,Palette[*In++]);
        DHRow_WritePixel(state,&dhrow,Palette[*In++]);
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available>>3);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

/*

  Display - Screen output for 2X horizontal scaling

*/

static int Display_RowFunc1bpp2X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate(state,Palette,2);

  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth;

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,(flags & ROWFUNC_FORCE),1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available<<1);
      const unsigned char *In = RAM+(Vptr>>3);
      unsigned int Bit = 1<<(Vptr & 7);
      for(i=0;i<Available;i++)
      {
        int idx = (*In & Bit)?1:0;
        DHRow_WritePixels(state,&dhrow,Palette[idx],2);
        Bit <<= 1;
        if(Bit == 256)
        {
          Bit = 1;
          In++;
        }
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available<<1);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

static int Display_RowFunc2bpp2X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate(state,Palette,4);

  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth*2; /* Scale up to account for everything else counting in bits */

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    /* Note: This is the number of available bits, not pixels */
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,(flags & ROWFUNC_FORCE),1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available);
      const unsigned char *In = RAM+(Vptr>>3);
      unsigned int Shift = (Vptr & 7);
      for(i=0;i<Available;i+=2)
      {
        int idx = (*In >> Shift) & 3;
        DHRow_WritePixels(state,&dhrow,Palette[idx],2);
        Shift += 2;
        if(Shift == 8)
        {
          Shift = 0;
          In++;
        }
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

static int Display_RowFunc4bpp2X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate(state,Palette,16);


  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth*4; /* Scale up to account for everything else counting in bits */

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    /* Note: This is the number of available bits, not pixels */
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,(flags & ROWFUNC_FORCE),1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available>>1);

      /* Display will always be a multiple of 2 pixels wide, so we can simplify things a bit compared to 1/2bpp case */
      const unsigned char *In = RAM+(Vptr>>3);      
      for(i=0;i<Available;i+=8)
      {
        unsigned char Pixel = *In++;
        DHRow_WritePixels(state,&dhrow,Palette[Pixel & 0xf],2);
        DHRow_WritePixels(state,&dhrow,Palette[Pixel>>4],2);
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available>>1);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

static int Display_RowFunc8bpp2X(ARMul_State *state,int row,DHRow dhrow,int flags)
{
  int i;
  HostPixel *Palette = HD.Palette;
  /* Handle palette updates */
  Display_PaletteUpdate8bpp(state,Palette);

  unsigned int Vptr = DC.Vptr;
  unsigned int Vstart = MEMC.Vstart<<7;
  unsigned int Vend = (MEMC.Vend+1)<<7; /* Point to pixel after end */
  const unsigned char *RAM = (unsigned char *) MEMC.PhysRam;
  int Remaining = DC.LastHostWidth*8; /* Scale up to account for everything else counting in bits */

  /* Sanity checks to avoid looping forever */
  if((Vptr >= Vend) || (Vstart >= Vend))
    return 0;
  if(Vptr >= Vend)
    Vptr = Vstart;

  /* Process the row */
  unsigned int startVptr = Vptr;
  int startRemain = Remaining;
  const unsigned int *MEMC_UpdateFlags = MEMC.UpdateFlags;
  unsigned int *HD_UpdateFlags = HD.UpdateFlags[row];
  while(Remaining > 0)
  {
    unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
    /* Note: This is the number of available bits, not pixels */
    int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
      
    if((flags & ROWFUNC_FORCE) || (HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]))
    {
      VIDEO_STAT(DisplayRedraw,1,1);
      VIDEO_STAT(DisplayRedrawForced,(flags & ROWFUNC_FORCE),1);
      VIDEO_STAT(DisplayRedrawUpdated,(HD_UpdateFlags[FlagsOffset] != MEMC_UpdateFlags[FlagsOffset]),1);
      VIDEO_STAT(DisplayBits,1,Available);
      flags |= ROWFUNC_UPDATED;
      /* Process the pixels in this region, stopping at end of row/update block/Vend */
      DHRow_BeginUpdate(state,&dhrow,Available>>2);

      /* Display will always be a multiple of 2 pixels wide, so we can simplify things a bit compared to 1/2bpp case */
      const unsigned char *In = RAM+(Vptr>>3);      
      for(i=0;i<Available;i+=16)
      {
        DHRow_WritePixels(state,&dhrow,Palette[*In++],2);
        DHRow_WritePixels(state,&dhrow,Palette[*In++],2);
      }
      DHRow_EndUpdate(state,&dhrow);
    }
    else
      DHRow_SkipPixels(state,&dhrow,Available>>2);

    Remaining -= Available;      
    Vptr += Available;
    if(Vptr >= Vend)
      Vptr = Vstart;
  }
  DC.Vptr = Vptr;
        
  /* If we updated anything, copy over the updated flags (Done last in case the same flags block is encountered multiple times in the same row) */
  if((flags & (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS)) == (ROWFUNC_UPDATED | ROWFUNC_UPDATEFLAGS))
  {
    Vptr = startVptr;
    Remaining = startRemain;
    while(Remaining > 0)
    {
      unsigned int FlagsOffset = Vptr/(8*UPDATEBLOCKSIZE);
      int Available = MIN(Remaining,MIN(((FlagsOffset+1)*8*UPDATEBLOCKSIZE)-Vptr,Vend-Vptr));
  
      HD_UpdateFlags[FlagsOffset] = MEMC_UpdateFlags[FlagsOffset];
      
      Remaining -= Available;      
      Vptr += Available;
      if(Vptr >= Vend)
        Vptr = Vstart;
    }
  }

  return (flags & ROWFUNC_UPDATED);
}

/*

  Display - Screen output other funcs

*/

static const unsigned int PixelClocks[4] = {8000000,12000000,16000000,24000000};

static void Display_BorderRow(ARMul_State *state,int row)
{
  /* Render a border row */
  HostPixel col = VIDC.BorderCol;
  if(!DC.ForceRefresh && (HD.BorderCol[row] == col))
    return;
  VIDEO_STAT(BorderRedraw,1,1);
  VIDEO_STAT(BorderRedrawForced,DC.ForceRefresh,1);
  VIDEO_STAT(BorderRedrawColourChanged,(HD.BorderCol[row] != col),1);
  HD.BorderCol[row] = col;
  int hoststart = (row-(VIDC.Vert_DisplayStart+1))*HD.YScale+HD.YOffset;
  int hostend = hoststart + HD.YScale;
  if(hoststart < 0)
    hoststart = 0;
  if(hostend > HD.Height)
    hostend = HD.Height;
  while(hoststart < hostend)
  {
    DHRow dhrow = DHRow_BeginRow(state,hoststart++,0);
    DHRow_BeginUpdate(state,&dhrow,HD.Width);
    DHRow_WritePixels(state,&dhrow,col,HD.Width);
    DHRow_EndUpdate(state,&dhrow);
    DHRow_EndRow(state,&dhrow);
  }
}

static const Display_RowFunc RowFuncs[2][4] = {
 { /* 1X horizontal scaling */
   Display_RowFunc1bpp1X,
   Display_RowFunc2bpp1X,
   Display_RowFunc4bpp1X,
   Display_RowFunc8bpp1X
 },
 { /* 2X horizontal scaling */
   Display_RowFunc1bpp2X,
   Display_RowFunc2bpp2X,
   Display_RowFunc4bpp2X,
   Display_RowFunc8bpp2X,
 }
};

static void Display_DisplayRow(ARMul_State *state,int row)
{
  /* Render a display row */
  int hoststart = (row-(VIDC.Vert_DisplayStart+1))*HD.YScale+HD.YOffset;
  int hostend = hoststart + HD.YScale;
  if(hoststart < 0)
    hoststart = 0;
  if(hostend > HD.Height)
    hostend = HD.Height;
  if(hoststart >= hostend)
    return;

  /* Handle border colour updates */
  int rowflags = (DC.ForceRefresh?ROWFUNC_FORCE:0);
  VIDEO_STAT(DisplayFullForce,force,1);
  HostPixel col = VIDC.BorderCol;
  
  if(rowflags || (HD.BorderCol[row] != col))
  {
    VIDEO_STAT(BorderRedraw,1,1);
    VIDEO_STAT(BorderRedrawForced,rowflags,1);
    VIDEO_STAT(BorderRedrawColourChanged,(HD.BorderCol[row] != col),1);
    HD.BorderCol[row] = col;
    int i;
    for(i=hoststart;i<hostend;i++)
    {
      DHRow dhrow = DHRow_BeginRow(state,i,0);
      DHRow_BeginUpdate(state,&dhrow,HD.XOffset);
      DHRow_WritePixels(state,&dhrow,col,HD.XOffset);
      DHRow_EndUpdate(state,&dhrow);
      int displaywidth = HD.XScale*DC.LastHostWidth;
      DHRow_SkipPixels(state,&dhrow,displaywidth);
      int rightborder = HD.Width-(displaywidth+HD.XOffset);
      DHRow_BeginUpdate(state,&dhrow,rightborder);
      DHRow_WritePixels(state,&dhrow,col,rightborder);
      DHRow_EndUpdate(state,&dhrow);
      DHRow_EndRow(state,&dhrow);
    }
  }

  /* Display area */

  unsigned int flags = HD.RefreshFlags[row>>5];
  unsigned int bit = 1<<(row&31);
  if(flags & bit)
  {
    VIDEO_STAT(DisplayRowForce,1,1);
    rowflags = ROWFUNC_FORCE;
    HD.RefreshFlags[row>>5] = (flags &~ bit);
  }

  DHRow dhrow = DHRow_BeginRow(state,hoststart++,HD.XOffset);
  const Display_RowFunc rf = RowFuncs[HD.XScale-1][(DC.VIDC_CR&0xc)>>2];
  if(hoststart == hostend)
  {
    if((rf)(state,row,dhrow,rowflags | ROWFUNC_UPDATEFLAGS))
    {
      VIDEO_STAT(DisplayRowRedraw,1,1);
    }
    DHRow_EndRow(state,&dhrow);
  }
  else
  {
    /* Remember current Vptr */
    unsigned int Vptr = DC.Vptr;
    int updated = (rf)(state,row,dhrow,rowflags);
    DHRow_EndRow(state,&dhrow);
    if(updated)
    {
      VIDEO_STAT(DisplayRowRedraw,1,1);
      /* Call the same func again on the same source data to update the copies of this scanline */
      while(hoststart < hostend)
      {
        DC.Vptr = Vptr;
        dhrow = DHRow_BeginRow(state,hoststart++,HD.XOffset);
        if(hoststart == hostend)
          rowflags |= ROWFUNC_UPDATEFLAGS;
        (rf)(state,row,dhrow,rowflags);
        DHRow_EndRow(state,&dhrow);
      }
    }
  }
}

/*

  Display - EventQ funcs

*/

static void Display_FrameEnd(ARMul_State *state,CycleCount nowtime); /* Trigger vsync interrupt */
static void Display_FrameStart(ARMul_State *state,CycleCount nowtime); /* End of vsync, prepare for new frame */
static void Display_RowStart(ARMul_State *state,CycleCount nowtime); /* Fill in a display/border row */

static void Display_Flyback(ARMul_State *state)
{
  if(DC.FLYBK)
    return;

  /* Trigger VSync interrupt */
  DC.FLYBK = 1;
  ioc.IRQStatus|=IRQA_VFLYBK;
  IO_UpdateNirq(state);
}

static void Display_Reschedule(ARMul_State *state,CycleCount nowtime,EventQ_Func func,int row)
{
  /* Force frame end just in case registers have been poked mid-frame */
  if(row >= VIDC.Vert_Cycle+1)
  {
    func = Display_FrameEnd;
    Display_Flyback(state);
  }
  int rows = row-DC.NextRow;
  if(rows < 1)
    rows = 1;
  DC.LastRow = DC.NextRow;
  DC.NextRow = row;
  EventQ_RescheduleHead(state,nowtime+rows*DC.LineRate,func);
}

static void Display_FrameStart(ARMul_State *state,CycleCount nowtime)
{
  const unsigned int NewCR = VIDC.ControlReg;

  /* Calculate new line rate */
  DC.LineRate = ARMul_EmuRate/(PixelClocks[NewCR&3]/(VIDC.Horiz_Cycle*2+2));
  if(DC.LineRate < 100)
    DC.LineRate = 100; /* Clamp to safe minimum value */

  /* Ensure mode changes if pixel clock changed */
  DC.ModeChanged |= (DC.VIDC_CR & 3) != (NewCR & 3);

  /* Force full refresh if DMA just toggled on/off */
  char newDMAEn = (MEMC.ControlReg>>10)&1;
  DC.ForceRefresh = (newDMAEn ^ DC.DMAEn);
  DC.DMAEn = newDMAEn;
  VIDEO_STAT(ForceRefreshDMA,DC.ForceRefresh,1);

  /* Ensure full palette rebuild & screen refresh on BPP change */
  if((DC.VIDC_CR & 0xc) != (NewCR & 0xc))
  {
    VIDEO_STAT(ForceRefreshBPP,1,1);
    DC.DirtyPalette = 65535;
    DC.ForceRefresh = 1;
  }

  /* Vinit changes require a full refresh also */
  if(DC.LastVinit != MEMC.Vinit)
  {
    DC.LastVinit = MEMC.Vinit;
    if(!DC.ForceRefresh) /* No point setting RefreshFlags if already doing full refresh */
    {
      VIDEO_STAT(RefreshFlagsVinit,1,1);
      memset(HD.RefreshFlags,0xff,sizeof(HD.RefreshFlags));
    }
  }

  DC.VIDC_CR = NewCR;

  DC.FLYBK = 0;

  /* Handle any mode changes */
  if(DC.ModeChanged)
  {
    /* Work out new screen parameters */
    int Width = (VIDC.Horiz_DisplayEnd-VIDC.Horiz_DisplayStart)*2;
    int Height = (VIDC.Vert_DisplayEnd-VIDC.Vert_DisplayStart);
    int FramePeriod = (VIDC.Horiz_Cycle*2+2)*(VIDC.Vert_Cycle+1);
    int FrameRate = PixelClocks[NewCR&3]/FramePeriod;
    
    if((Width != DC.LastHostWidth) || (Height != DC.LastHostHeight) || (FrameRate != DC.LastHostHz))
    {
      fprintf(stderr,"New mode: %dx%d, %dHz (CR %x)\n",Width,Height,FrameRate,NewCR);
#ifdef VIDEO_STATS
      vidstats_Dump("Stats for previous mode");
#endif
      /* Try selecting new mode */
      if((Width < 1) || (Height < 1))
      {
        /* Bad mode; skip straight to FrameEnd state */
        Display_Reschedule(state,nowtime,Display_FrameEnd,(VIDC.Vert_Cycle+1));
        return;
      }
      
      DC.LastHostWidth = Width;
      DC.LastHostHeight = Height;
      DC.LastHostHz = FrameRate;
      DisplayKbd_HostChangeMode(state,Width,Height,FrameRate);

      /* Calculate display offsets, for start of first display pixel */
      HD.XOffset = (HD.Width-Width*HD.XScale)/2;
      HD.YOffset = (HD.Height-Height*HD.YScale)/2;
      DC.ForceRefresh = 1;
    }
    DC.ModeChanged = 0;
  }

#ifdef VIDEO_STATS
  if(vidstats[vidstat_DisplayFrames] >= 100)
    vidstats_Dump("Stats for last 100 frames");
#endif

  /* Set up DMA */
  DC.Vptr = MEMC.Vinit<<7;

  /* Schedule for first border row */
  Display_Reschedule(state,nowtime,Display_RowStart,VIDC.Vert_BorderStart+1);
  
  /* Update host */
  DisplayKbd_PollHostDisplay(state);
}

static void Display_FrameEnd(ARMul_State *state,CycleCount nowtime)
{
  VIDEO_STAT(DisplayFrames,1,1);

  Display_Flyback(state); /* Paranoia */

  /* Set up the next frame */
  DC.LastRow = 0;
  DC.NextRow = VIDC.Vert_SyncWidth+1;
  EventQ_RescheduleHead(state,nowtime+DC.NextRow*DC.LineRate,Display_FrameStart);
}

static void Display_RowStart(ARMul_State *state,CycleCount nowtime)
{
  int row = DC.LastRow;
  if(row < VIDC.Vert_BorderStart+1)
    row = VIDC.Vert_BorderStart+1; /* Skip pre-border rows */
  int stop = DC.NextRow;
  int dmaen = DC.DMAEn;
  while(row < stop)
  {
    if(row < (VIDC.Vert_DisplayStart+1))
    {
      /* Border region */
      Display_BorderRow(state,row);
    }
    else if(dmaen && (row < (VIDC.Vert_DisplayEnd+1)))
    {
      /* Display */
      Display_DisplayRow(state,row);
    }
    else if(row < (VIDC.Vert_BorderEnd+1))
    {
      /* Border again */
      Display_Flyback(state);
      Display_BorderRow(state,row);
    }
    else
    {
      /* Reached end of screen */
      Display_Reschedule(state,nowtime,Display_FrameEnd,VIDC.Vert_Cycle+1);
      return;
    }
    VIDEO_STAT(DisplayRows,1,1);
    row++;
  }
  /* Skip ahead to next row */
  int nextrow = row+ROWS_AT_ONCE;
#if ROWS_AT_ONCE > 1
  if((row+1 <= VIDC.Vert_Cycle+1) && (nextrow > VIDC.Vert_Cycle+1))
    nextrow = VIDC.Vert_Cycle+1;
#endif
  Display_Reschedule(state,nowtime,Display_RowStart,nextrow);
}

/*

  Display - VIDC write handler

*/

void VIDC_PutVal(ARMul_State *state,ARMword address, ARMword data,int bNw) {
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
      if(!(DC.DirtyPalette & (1<<Log)))
      {
        VIDEO_STAT(RefreshFlagsPalette,1,1);
        DC.DirtyPalette |= (1<<Log);
        memset(HD.RefreshFlags,0xff,sizeof(HD.RefreshFlags));
      }
    }
    return;
  };

  addr&=~3;
  switch (addr) {
    case 0x40: /* Border col */
#ifdef DEBUG_VIDCREGS
      fprintf(stderr,"VIDC border colour write val=0x%x\n",val);
#endif
      VIDC.BorderCol = DisplayKbd_HostColour(state,val & 0x1fff);
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
      VIDC.Vert_BorderStart = (val>>14) & 0x3ff;
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
      VIDC.Vert_BorderEnd = (val>>14) & 0x3ff;
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

/*

  Display - General

*/

void
DisplayKbd_Init(ARMul_State *state)
{
  /* Call Host-specific routine */
  DisplayKbd_InitHost(state);

  DC.ModeChanged = 1;
  DC.LastHostWidth = DC.LastHostHeight = DC.LastHostHz = -1;
  DC.DirtyPalette = 65535;
  DC.NextRow = 0;
  DC.LastRow = 0;
  DC.MaxRow = 0;
  DC.VIDC_CR = 0;
  DC.DMAEn = 0;
  DC.FLYBK = 0;
  DC.LineRate = 10000;
  DC.LastVinit = MEMC.Vinit;

  memset(HOSTDISPLAY.RefreshFlags,~0,sizeof(HOSTDISPLAY.RefreshFlags));
  memset(HOSTDISPLAY.UpdateFlags,0,sizeof(HOSTDISPLAY.UpdateFlags)); /* Initial value in MEMC.UpdateFlags is 1 */   

  /* Schedule update events */
  EventQ_Insert(state,ARMul_Time+POLLGAP*100,Display_FrameStart);
} /* DisplayKbd_Init */
