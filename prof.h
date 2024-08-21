#ifndef prof_h
#define prof_h

#define PROFILE_ENABLED

#ifdef PROFILE_ENABLED

extern void Prof_Init();
extern void Prof_Dump();
extern void Prof_BeginFunc(const void *);
extern void Prof_EndFunc(const void *);
extern void Prof_Begin(const char *);
extern void Prof_End(const char *);

#else

#define Prof_Init() 0
#define Prof_Dump() 0
#define Prof_BeginFunc(x) 0
#define Prof_EndFunc(x) 0
#define Prof_Begin(x) 0
#define Prof_End(x) 0

#endif

#endif
