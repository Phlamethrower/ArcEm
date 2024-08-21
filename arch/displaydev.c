#include "armdefs.h"
#include "displaydev.h"

const DisplayDev *DisplayDev_Current = NULL;

int DisplayDev_Set(ARMul_State *state,const DisplayDev *dev)
{
  struct Vidc_Regs Vidc;
  if(dev == DisplayDev_Current)
    return 0;
  if(DisplayDev_Current)
  {
    Vidc = VIDC;
    (DisplayDev_Current->Shutdown)(state);
    DisplayDev_Current = NULL;
  }
  else
  {
    memset(&Vidc,0,sizeof(Vidc));
  }
  if(dev)
  {
    int ret = (dev->Init)(state,&Vidc);
    if(ret)
      return ret;
    DisplayDev_Current = dev;
  }
  return 0;
}

