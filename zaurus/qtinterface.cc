#include <qpe/qpeapplication.h>
#include <qwidget.h>
#include <qpopupmenu.h>
#include <qmenubar.h>
#include <qpainter.h>
#include <qnamespace.h>
#include <qscrollview.h>
#include <qstatusbar.h>
#include "qtinterface.h"
#include <time.h>

extern "C" {

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
void zaurus_execute();
void ARMul_Emulate26();

}

#define AUTOREFRESHPOLL 2500
#define DC DISPLAYCONTROL

static QMainWindow *mainwind=0; /* Main window */
static QImage *display=0,*cursor_pic=0; /* VDU and cursor images */
static QtVDU *dwidget = 0; /* The display widget */
static QScrollView *sview = 0; /* Scrolly thing which our widget sits in */
static int cursor_x=0,cursor_y=0; /* Old location of mouse cursor */
static int cursor_width=1; /* Old width */
static int cursor_minx=0; /* X pos of the first pixel in the cursor; used to help the stylus-based mouse emulation */

/* Timer variables for MHz/FPS display */
static unsigned int timer_start=0;
static unsigned int timer_last=0;
static int timer_loops=0;
static int timer_fps=0;
#define NUM_TIMER_LOOPS 20

int ZAURUS_CYCLE_LIMIT = 50000; /* How many cycles to perform per ARMul_Emulate26() */
int ZAURUS_VIDEO_RATE = 0; /* Frameskip value; currently 0 because emulator core is the bottleneck */

FrmMain::FrmMain(QWidget* parent, const char* name, WFlags fl)
    : QMainWindow(parent, name, fl) 
  { 
  setCaption("ArcEm"); 
                                                                                 
/* No point having this menu bar at the moment
  QMenuBar *menubar = this->menuBar();
  QPopupMenu *mnuFile = new QPopupMenu(this, "MenuFile"); 
  menubar->insertItem("&File", mnuFile); 
  mnuFile->insertItem("&Quit", this, 
    SLOT(cmdFileQuit()), 0, 1); */

  sview = new QScrollView(this,0,Qt::WRepaintNoErase);
  dwidget = new QtVDU(sview,"display");
  sview->addChild(dwidget);
  
  display = new QImage(1,1,8);
  cursor_pic = new QImage(32,1,8);
  cursor_pic->setAlphaBuffer(true);
  cursor_pic->setNumColors(4);
  cursor_pic->setColor(0,qRgba(0,0,0,0)); /* Ensure cursor is transparent by default */
  setCentralWidget(sview);
  startTimer(0); /* Without this, timerEvent() below won't get called and we won't emulate anything */
  }

void FrmMain::cmdFileQuit() 
  { 
  this->close(); 
  }

void FrmMain::timerEvent(QTimerEvent *e)
{
	ARMul_Emulate26();
	if(timer_loops--)
		return;
	timer_last = clock()-timer_start;
	timer_start += timer_last;
	timer_loops = NUM_TIMER_LOOPS-1;
	/* Update title with new clock speed */
	float speed = ((float) ZAURUS_CYCLE_LIMIT*NUM_TIMER_LOOPS*CLOCKS_PER_SEC)/((float) timer_last);
	char msg[32];
	sprintf(msg,"ArcEm %.3fMHz %dfps",speed/1000000,(timer_fps*CLOCKS_PER_SEC)/timer_last);
	timer_fps = 0;
	mainwind->setCaption(msg);
}


QtVDU::QtVDU(QWidget *parent,const char *name) : QWidget(parent,name)
{
	setFocusPolicy(StrongFocus);
	setWFlags(Qt::WRepaintNoErase);
	setMouseTracking(true);
	return;
}

void QtVDU::paintEvent(QPaintEvent *e)
{
	QPainter p(this);
	p.setClipRect(e->rect());
	p.setClipRegion(e->region());
	p.drawImage(0,0,*display);
	p.drawImage(cursor_x,cursor_y,*cursor_pic);
}

QSizePolicy QtVDU::sizePolicy() const
{
	return QSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
}

/*

				KEYBOARD

*/

/* Some extra zaurus keys:
calendar: 1038 - F9
address: 1039 - F10
mail: 103c - F13
home: 103b - F12
menu: 103a - F11
OK: 1004 (both) - return
Cancel: currently untrapped!
jog: 1013/1015 - up/down
FN: 1045 - F22
kanji 1 - 1049 - F26
FN+ka 1 - 104e - F31
kanji 2 - 1044 - F21
FN+ka 2 - 1048 - F25
FN+space - 1046 - F23
left-up - a2 - cent
right-down - a3 - sterling
'dot' - a5 - yen
asterisk - 2a - asterisk
yen - 5c - backslash (!)
bar - 7c - bar
FN+1 - 200F
FN+2 - 2010
FN+3 - 2012
FN+4 - 2011
FN+5 - 201B
FN+6 - 200A
FN+7 - 200B
FN+8 - 200C
FN+9 - 2007
FN+0 - 2008 */

typedef struct {
	Qt::Key key;
	arch_key_id kid;
} key_to_arch_key;

#define X(sym, kid) { Qt::Key_ ## sym, ARCH_KEY_ ## kid },
static const key_to_arch_key key_to_arch_key_map[] = {
	X(Escape, escape)
	X(Tab, tab)
//	Backtab
	X(Backspace, backspace)
	X(Return, return)
	X(Enter, return) // Maybe kp_enter?
	X(Insert, insert)
	X(Delete, delete)
	X(Pause, break)
	X(Print, print)
//	SysRq
	X(Home, home)
	X(End, copy)
	X(Left, left)
	X(Up, up)
	X(Right, right)
	X(Down, down)
	X(PageUp, page_up)
	X(PageDown, page_down)
	X(Shift, shift_l)
	X(Control, control_l)
//	Meta
	X(Alt, alt_l)
	X(CapsLock, caps_lock)
	X(NumLock, num_lock)
	X(ScrollLock, scroll_lock)
/* These don't work right on the Zaurus
	X(F1, f1)
	X(F2, f2)
	X(F3, f3)
	X(F4, f4)
	X(F5, f5)
	X(F6, f6)
	X(F7, f7)
	X(F8, f8)
	X(F9, f9)
	X(F10, f10)
	X(F11, f11)
	X(F12, f12) */
//	Function key mappings - using Fn+number keys for F1-F10, Fn+kanji for F11, F12
	{ (Qt::Key) 0x200F, ARCH_KEY_f1 },
	{ (Qt::Key) 0x2010, ARCH_KEY_f2 },
	{ (Qt::Key) 0x2012, ARCH_KEY_f3 },
	{ (Qt::Key) 0x2011, ARCH_KEY_f4 },
	{ (Qt::Key) 0x201B, ARCH_KEY_f5 },
	{ (Qt::Key) 0x200A, ARCH_KEY_f6 },
	{ (Qt::Key) 0x200B, ARCH_KEY_f7 },
	{ (Qt::Key) 0x200C, ARCH_KEY_f8 },
	{ (Qt::Key) 0x2007, ARCH_KEY_f9 },
	{ (Qt::Key) 0x2008, ARCH_KEY_f10 },
	X(F31, f11) // FN+kanji 1
	X(F25, f12) // FN+kanji 2
	X(F26, escape) // Kanji 1
	X(F21, break) // Kanji 2
//	Quick mouse hack
	X(yen, button_1)
	X(cent, button_2)
	X(sterling, button_3)
//	F13 - F35
//	Super_L, Super_R, Menu, Hyper_L, Hyper_R - WTF?
//	blah blah some international keys...
	X(Space, space)
	X(Plus, kp_plus)
	X(Comma, comma)
	X(Minus, minus)
	X(Period, period)
	X(Slash, slash)
	X(0, 0)
	X(1, 1)
	X(2, 2)
	X(3, 3)
	X(4, 4)
	X(5, 5)
	X(6, 6)
	X(7, 7)
	X(8, 8)
	X(9, 9)
//	Colon
	X(Semicolon, semicolon)
	X(Less, slash)
	X(Greater, alt_l)
//	Question
	X(Equal, equal)
	X(At, apostrophe) // ick
	X(A, a)
	X(B, b)
	X(C, c)
	X(D, d)
	X(E, e)
	X(F, f)
	X(G, g)
	X(H, h)
	X(I, i)
	X(J, j)
	X(K, k)
	X(L, l)
	X(M, m)
	X(N, n)
	X(O, o)
	X(P, p)
	X(Q, q)
	X(R, r)
	X(S, s)
	X(T, t)
	X(U, u)
	X(V, v)
	X(W, w)
	X(X, x)
	X(Y, y)
	X(Z, z)
	X(BracketLeft, bracket_l)
	X(Backslash, backslash)
	X(BracketRight, bracket_r)
	X(AsciiCircum, grave) // Roughly the right place...
//	Underscore, QuoteLeft, BraceLeft, Bar, BraceRight, Asciitilde
//	blah blah loads more keys
	{ Qt::Key_unknown } // List terminator
};

static bool keypress(Qt::Key code,bool press)
{
	const key_to_arch_key *k;
	for(k=key_to_arch_key_map;k->key != Qt::Key_unknown;k++)
		if(k->key == code) {
			keyboard_key_changed((struct arch_keyboard *) &(PRIVD->Display.Kbd), k->kid, !press);
			return true;
		}
	return false;
}

void QtVDU::keyPressEvent(QKeyEvent *e)
{
	if(!keypress((Qt::Key) e->key(),true))
		e->ignore();
}

void QtVDU::keyReleaseEvent(QKeyEvent *e)
{
	if(!keypress((Qt::Key) e->key(),false))
		e->ignore();
}

void QtVDU::mouseMoveEvent(QMouseEvent *e)
{
	/* This mouse emulation compares the stylus/mouse position to the
	   on-screen location of the top-left non-transparent pixel of the
	   hardware cursor.
	   It then feeds a delta into the emulator, in an attempt to move the
	   cursor closer to the correct location.
	   Thus the code will fail miserably if the hardware cursor is
	   transparent, or isn't attached to the mouse. But with stylus-based
	   input, and no reliance on extension ROMs, I think this is the best
	   solution */ 
	int xdiff,ydiff;
	/* Don't do anything if there are still mouse events waiting to be
	   processed by the emulator */
	if(KBD.MouseXCount || KBD.MouseYCount)
		return;
	/* Else, work out the delta */
	xdiff = (e->x()-(cursor_x+cursor_minx))/2; /* Decimate by 2 to try and reduce overshooting */
	ydiff = (cursor_y-e->y())/2;
	if (xdiff > 63)
		xdiff = 63;
	else if (xdiff < -63)
		xdiff = -63;
	if (ydiff > 63)
		ydiff = 63;
	else if (ydiff < -63)
		ydiff = -63;
	KBD.MouseXCount = xdiff & 127;
	KBD.MouseYCount = ydiff & 127;
}


/*

				MOUSE

*/

static void refreshmouse()
{
	int x,y,height;
	int memptr;
	int old_x,old_y,old_w,old_h;
	bool redraw = false;
	/* Cache the old mouse pos */
	old_x = cursor_x+cursor_minx;
	old_y = cursor_y;
	old_w = cursor_width-cursor_minx;
	old_h = cursor_pic->height();
	/* Mostly bodged from X11 version... */
	height = (VIDC.Vert_CursorEnd - VIDC.Vert_CursorStart) + 1;
	/* simple sanity check */
	if(height > 256)
		height = 256;
	if(height != cursor_pic->height())
	{
		/* Resize mouse image */
		delete cursor_pic;
		cursor_pic = new QImage(32,height,8);
		cursor_pic->setAlphaBuffer(true);
		cursor_pic->setNumColors(4);
		cursor_pic->setColor(0,qRgba(0,0,0,0));
		DC.MustResetPalette = 1;
	}
	/* Update palette */
	if(DC.MustResetPalette)
	{
		int r,g,b;
		for(x=0;x<3;x++)
		{
			r=(VIDC.CursorPalette[x] & 0xF)*17;
			g=((VIDC.CursorPalette[x] & 0xF0) >> 4)*17;
			b=((VIDC.CursorPalette[x] & 0xF00) >> 8)*17;
			cursor_pic->setColor(x+1,qRgba(r,g,b,255));
		}
		DC.MustResetPalette = 0;
		redraw = true;
	}
	/* Update mouse pixels */
	memptr = MEMC.Cinit * 4;
	cursor_width = 1;
	cursor_minx = 32;
	for(y=0;y<height;y++,memptr+=2)
	{
		unsigned int tmp[2];
		tmp[0] = MEMC.PhysRam[memptr];
		tmp[1] = MEMC.PhysRam[memptr+1];
		unsigned char c,*row = cursor_pic->scanLine(y);
		for(x=0;x<32;x++,row++)
		{
			c = (tmp[x/16] >> ((x & 15) * 2)) & 3;
			if(c != *row)
			{
				*row = c;
				redraw = true;
			}
			if(c)
			{
				if(x >= cursor_width)
					cursor_width = x+1;
				if(x < cursor_minx)
					cursor_minx = x;
			}
		}
	}
	if(cursor_minx == 32)
		cursor_minx = 0;
	/* Get new cursor pos */
	cursor_x = (int)VIDC.Horiz_CursorStart - (int)VIDC.Horiz_DisplayStart*2;
	cursor_y = (int)VIDC.Vert_CursorStart - (int)VIDC.Vert_DisplayStart;
	/* Has anything changed? */
	if((!redraw) && (old_x == cursor_x+cursor_minx) && (old_y == cursor_y))
		return;
	/* Redraw! */
	dwidget->update(old_x,old_y,old_w,old_h);
	dwidget->update(cursor_x+cursor_minx,cursor_y,cursor_width-cursor_minx,cursor_pic->height());
}

/*

				VDU

*/

int displayrefreshcount = 0;

void RefreshDisplay(ARMul_State *state)
{
	DC.AutoRefresh=AUTOREFRESHPOLL;
	ioc.IRQStatus|=8; /* VSync */
	ioc.IRQStatus |= 0x20; /* Sound - just an experiment */
	IO_UpdateNirq();
	/* Refresh display etc. */
	if(!display || displayrefreshcount--)
		return;
	timer_fps++;
	displayrefreshcount = ZAURUS_VIDEO_RATE;
	int width = (VIDC.Horiz_DisplayEnd - VIDC.Horiz_DisplayStart)*2;
	int height = VIDC.Vert_DisplayEnd - VIDC.Vert_DisplayStart;
	int bpp = (VIDC.ControlReg & 0xc) >> 2;
	bool refresh = DC.MustRedraw;
	if((width <= 0) || (height <= 0) || (width > 1024) || (height > 1024))
		return; /* Bad mode - ignore */
	if((width != display->width()) || (height != display->height()))
	{
		/* Resize image */
		delete display;
		display = new QImage(width,height,8);
		display->setNumColors(256);
		dwidget->setFixedSize(width,height);
		refresh = true;
	}
	/* Set palette */
	int x,y,memoffset=0;
	if(DC.MustResetPalette || refresh)
	{
		int col,l4,l65,l7;
		col=2;
		for(l4=0;l4<bpp;l4++)
			col=col*col;
		for(col--;col>=0;col--)
		{
			x=col & 15;
			l4=(col>>4)&1;
			l65=(col>>5)&3;
			l7=(col>>7)&1;
			display->setColor(col,qRgb(((VIDC.Palette[x] & 15) | (l4<<3))*17,(((VIDC.Palette[x]>>4) & 15) | (l65<<2))*17,(((VIDC.Palette[x]>>8) & 15) | (l7<<3))*17));
		}
		/* Can't clear DC.MustResetPalette here - it may have indicated a mouse palette change, so instead we will let the mouse code handle it (below) */
	}
	/* Scan for screen updates */
	int bytes=width >> (3-bpp);
	unsigned char *row = (unsigned char *) malloc(bytes); /* Temp buffer :( */
	for(y=0;y<height;y++,memoffset+=bytes)
	{
		if((refresh) || (QueryRamChange(0,memoffset,bytes)))
		{
			unsigned char *drow = display->scanLine(y);
			int minx=width,maxx=0;
			CopyScreenRAM(0 /* Parameter currently unused */,memoffset,bytes,(char *) row);
			/* Convert pixels */
			for(x=0;x<width;x++)
			{
				int val;
				if(bpp == 3)
					val = row[x];
				else if(bpp == 2)
					val = ((row[x>>1]) >> ((x & 1)*4)) & 0xF;
				else if(bpp == 1)
					val = ((row[x>>2]) >> ((x & 3)*2)) & 0x3;
				else
					val = ((row[x>>3]) >> (x & 7)) & 1;
				if(*drow != val)
				{
					*drow = val;
					maxx=x;
					if(minx == width)
						minx = x;
				}
				drow++;
			}
			/* Trigger a redraw of the correct area of the scanline (Unless we're refreshing the entire screen) */
			if((!refresh) && (minx != width))
				dwidget->update(minx,y,(maxx-minx)+1,1);
		}
	}
	free(row);
	/* Do full refresh if needed */
	if(refresh)
		dwidget->update(0,0,width,height);
	DC.MustRedraw = 0;
	MarkAsUpdated(0,memoffset);
	/* Mouse update */
	refreshmouse();
}

void zaurus_execute() {
	/* This is basically a standard Qt main() func, except it won't have access to any command line args. And isn't called main(). */
	int argc = 0;
	char *argv = 0;
	QPEApplication a(argc,&argv);
	mainwind = new FrmMain();
	a.showMainWidget(mainwind);
	a.exec();
}
