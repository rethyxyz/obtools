/* obbar - suckless-style status/task bar for openbox
 * see DESIGN.md at the repo root for the full design and change history.
 *
 * A dockable bar window (EWMH strut + PPosition/PSize, pinned to the
 * primary monitor via RandR) with a few independently-updating parts:
 *
 *   - a workspace picker (far left), one button per _NET_NUMBER_OF_DESKTOPS,
 *     click to switch; mouse scroll anywhere on the bar cycles workspaces;
 *   - a live taskbar built from _NET_CLIENT_LIST, grouped by WM_CLASS
 *     (config.h taskgroupbyclass), draggable left/right, with a
 *     click-down list for grouped windows;
 *   - status blocks (config.h blocks[]), each re-run on its own timer
 *     and/or a dedicated realtime signal (`pkill -RTMIN+N obbar`);
 *   - the current workspace number and an optional scrolling ticker.
 *
 * One poll() loop multiplexes the X11 connection, the blocks' timerfd,
 * a signalfd (SIGINT/SIGTERM + each block's signal), and the ticker's
 * own timerfd - no threads, no busy-waiting. All drawing goes through an
 * off-screen Pixmap blitted on with a single XCopyArea per frame, to
 * avoid visible flicker.
 *
 * oblist (the standalone scriptable popup list) is milestone 5, not yet
 * built - see DESIGN.md.
 */
#define _GNU_SOURCE /* signalfd/timerfd + sigaction/strdup/setenv under -std=c99 */

#include <Imlib2.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

#define LENGTH(x) (sizeof(x) / sizeof((x)[0]))
#define NBLOCKS ((int)LENGTH(blocks))
#define PAD 8            /* px, right edge margin */
#define GAP 12           /* px, between adjacent status blocks */
#define TASKGAP 16       /* px, between the taskbar and the status blocks */
#define DRAGTHRESHOLD 6  /* px of pointer movement before a Button1 press becomes a drag */
#define MAXGROUPORDER 64 /* cap on how many custom-ordered groups are remembered */
#define MAXDESKTOPS 64   /* cap on workspace picker buttons rendered */

typedef struct {
	int x1, x2; /* screen-space click region, valid after the last draw() */
} BlockRegion;

typedef struct {
	Window win;
	char title[256];
	char class[128]; /* WM_CLASS res_class, e.g. "Brave-browser" - for grouping */
	Imlib_Image icon; /* NULL if the window has no _NET_WM_ICON */
	int active;
	int urgent;
	BlockRegion region; /* only meaningful for single-window groups' own button */
} TaskWin;

/* one or more TaskWins sharing a WM_CLASS (see config.h taskgroupbyclass),
 * rendered as a single taskbar button; membership is looked up on demand
 * by matching class names (see openpopup) rather than a stored index -
 * taskgroups[] gets reordered after classification (drag-to-reorder, and
 * reapplying the saved order on every rebuild), so an index recorded at
 * classification time would go stale the moment either one runs. */
typedef struct {
	char class[128];
	int nmembers;
	int firstmember; /* index into taskwins[] of the first member seen */
	int active;       /* any member active */
	int urgent;       /* any member urgent */
	Imlib_Image icon; /* borrowed from a member, not separately owned/freed */
	BlockRegion region;
} TaskGroup;

static Display *dpy;
static int screen;
static Window root, win;
static int sw, sh;                     /* full (possibly multi-head) X screen size, fallback only */
static int monx, mony, monw, monh;     /* the monitor obbar actually occupies */
static Pixmap bbuf;                    /* off-screen draw target, blitted onto win each redraw */
static GC gc;
static XftFont *font;
static XftDraw *xftdraw;
static XftColor colscheme[3][2]; /* [SchemeNorm/Sel/Urg][fg/bg] */
static int running = 1;
static int curdesktop = 0; /* 0-based _NET_CURRENT_DESKTOP, refreshed by getcurdesktop() */
static int numdesktops = 1; /* _NET_NUMBER_OF_DESKTOPS, refreshed by getnumdesktops() */

static BlockRegion wsregions[MAXDESKTOPS]; /* picker button regions, valid after the last draw() */
static int nwsregions = 0;
static int pickerw = 0; /* picker's total on-screen width, valid after the last draw(); 0 if disabled */

static int tfd, sfd; /* timerfd, signalfd */

static int tickerfd = -1;    /* its own timerfd, entirely separate from tfd/blocks */
static unsigned int tickeroff = 0; /* current scroll offset, px */
static int tickerloopw = 0;  /* tickertext's rendered width + tickergap - the scroll period */
static char *blocktext[NBLOCKS];
static unsigned int countdown[NBLOCKS];
static BlockRegion blockregions[NBLOCKS];

static TaskWin *taskwins = NULL;
static int ntaskwins = 0;
static TaskGroup *taskgroups = NULL;
static int ntaskgroups = 0;

/* left-to-right group order the user has dragged into place, keyed by
 * class name since that's what's stable across taskgroups[] rebuilds -
 * session-only by design, nothing here ever touches disk (see DESIGN.md). */
static char grouporder[MAXGROUPORDER][128];
static int ngrouporder = 0;

/* Button1-press-then-release-or-drag state for taskbar buttons */
static int dragcandidate = -1; /* taskgroups[] index the press landed on, -1 = none */
static int dragging = 0;
static int dragstartx;

/* the group dropdown popup: at most one open at a time. poprows[] maps a
 * row index in the popup to an index into taskwins[]. */
static Window popupwin = None;
static XftDraw *popupdraw = NULL;
static int popupw;
static int opengroup = -1; /* index into taskgroups[], -1 = no popup open */
static int poprows[64];
static int npoprows;

static Atom netwmwindowtype, netwmwindowtypedock, netwmwindowtypedesktop, netwmwindowtypesplash,
             netwmstrut, netwmstrutpartial,
             netwmstate, netwmstateskiptaskbar, netwmstateskippager, netwmstatedemandsattention,
             netclientlist, netactivewindow, netclosewindow,
             netwmname, netwmicon, netwmdesktop, netcurrentdesktop, netnumberofdesktops, utf8string;

static void
die(const char *msg)
{
	fprintf(stderr, "obbar: %s\n", msg);
	exit(1);
}

/* X errors are expected here: a window from _NET_CLIENT_LIST can vanish
 * between us listing it and us querying its properties. Log and move on
 * instead of dying, like dwm's xerrordummy. */
static int
xerrordummy(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	return 0;
}

static void
initatoms(void)
{
	netwmwindowtype        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netwmwindowtypedock    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	netwmwindowtypedesktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
	netwmwindowtypesplash  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
	netwmstrut              = XInternAtom(dpy, "_NET_WM_STRUT", False);
	netwmstrutpartial       = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
	netwmstate              = XInternAtom(dpy, "_NET_WM_STATE", False);
	netwmstateskiptaskbar   = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
	netwmstateskippager     = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
	netwmstatedemandsattention = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	netclientlist           = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	netactivewindow         = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netclosewindow          = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
	netwmname               = XInternAtom(dpy, "_NET_WM_NAME", False);
	netwmicon               = XInternAtom(dpy, "_NET_WM_ICON", False);
	netwmdesktop             = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
	netcurrentdesktop       = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
	netnumberofdesktops     = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
	utf8string              = XInternAtom(dpy, "UTF8_STRING", False);
}

/* find which monitor obbar should occupy: whichever RandR marks primary,
 * or the first reported monitor if none is marked, or (no RandR monitor
 * info at all) the full X screen as a last resort. */
static void
initmonitor(void)
{
	XRRMonitorInfo *mons;
	int i, n = 0;

	monx = 0;
	mony = 0;
	monw = sw;
	monh = sh;

	if (!(mons = XRRGetMonitors(dpy, root, True, &n)) || n <= 0) {
		if (mons)
			XRRFreeMonitors(mons);
		return;
	}

	for (i = 0; i < n; i++) {
		if (mons[i].primary) {
			monx = mons[i].x;
			mony = mons[i].y;
			monw = mons[i].width;
			monh = mons[i].height;
			XRRFreeMonitors(mons);
			return;
		}
	}

	monx = mons[0].x;
	mony = mons[0].y;
	monw = mons[0].width;
	monh = mons[0].height;
	XRRFreeMonitors(mons);
}

/* reserve screen space so openbox doesn't place windows under the bar.
 *
 * EWMH struts always reserve space from an edge of the ROOT window, not
 * of an individual monitor - that only lines up with where our bar
 * actually is when the monitor we're confined to (see initmonitor) is
 * itself flush against that edge of the full virtual screen. If your
 * primary monitor sits away from the corresponding root edge (e.g.
 * vertically between two taller monitors), there's no strut that
 * correctly describes that; skip reserving space rather than reserving
 * the wrong region of the screen. */
static void
setstrut(void)
{
	long strut[4] = { 0, 0, 0, 0 };   /* left, right, top, bottom */
	long partial[12] = { 0 };

	if (topbar && mony == 0) {
		strut[2] = barheight;
		partial[2] = barheight;      /* top */
		partial[8] = monx;           /* top_start_x */
		partial[9] = monx + monw - 1; /* top_end_x */
	} else if (!topbar && mony + monh == sh) {
		strut[3] = barheight;
		partial[3] = barheight;      /* bottom */
		partial[10] = monx;          /* bottom_start_x */
		partial[11] = monx + monw - 1; /* bottom_end_x */
	}

	XChangeProperty(dpy, win, netwmstrut, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)strut, 4);
	XChangeProperty(dpy, win, netwmstrutpartial, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)partial, 12);
}

static void
setupwin(void)
{
	XSetWindowAttributes wa = { 0 };
	XClassHint ch = { "obbar", "obbar" };
	Atom states[2];
	int y = topbar ? mony : mony + (int)monh - (int)barheight;

	wa.override_redirect = False; /* a normal, undecorated dock window */
	wa.background_pixel = BlackPixel(dpy, screen);
	wa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
	                Button1MotionMask | StructureNotifyMask;

	win = XCreateWindow(dpy, root, monx, y, monw, barheight, 0,
	                     DefaultDepth(dpy, screen), CopyFromParent,
	                     DefaultVisual(dpy, screen),
	                     CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);

	XStoreName(dpy, win, "obbar");
	XSetClassHint(dpy, win, &ch);

	/* without an explicit PPosition/PSize hint, a WM is free to apply its
	 * own placement policy to our window instead of honoring the exact
	 * geometry we asked for in XCreateWindow - this is what pins the bar
	 * to the requested monitor's exact edge instead of drifting. */
	{
		XSizeHints hints = { 0 };
		hints.flags = PPosition | PSize;
		hints.x = monx;
		hints.y = y;
		hints.width = monw;
		hints.height = (int)barheight;
		XSetWMNormalHints(dpy, win, &hints);
	}

	XChangeProperty(dpy, win, netwmwindowtype, XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)&netwmwindowtypedock, 1);

	states[0] = netwmstateskiptaskbar;
	states[1] = netwmstateskippager;
	XChangeProperty(dpy, win, netwmstate, XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)states, 2);

	/* 0xFFFFFFFF is the EWMH sentinel for "all desktops" - without this
	 * the bar defaults to whichever workspace was active when it was
	 * mapped and disappears on every other one. */
	{
		long alldesktops = 0xFFFFFFFFL;
		XChangeProperty(dpy, win, netwmdesktop, XA_CARDINAL, 32,
		                PropModeReplace, (unsigned char *)&alldesktops, 1);
	}

	setstrut();

	XMapRaised(dpy, win);

	/* watch for windows opening/closing/focus changing */
	XSelectInput(dpy, root, PropertyChangeMask);
}

static void
initfont(void)
{
	if (!(font = XftFontOpenName(dpy, screen, fonts[0])))
		die("cannot load font, check config.h fonts[]");
}

static void
initcolors(void)
{
	Colormap cmap = DefaultColormap(dpy, screen);
	Visual *vis = DefaultVisual(dpy, screen);
	size_t i;

	for (i = 0; i < LENGTH(colors); i++) {
		if (!XftColorAllocName(dpy, vis, cmap, colors[i][0], &colscheme[i][0]))
			die("cannot allocate fg color, check config.h colors[]");
		if (!XftColorAllocName(dpy, vis, cmap, colors[i][1], &colscheme[i][1]))
			die("cannot allocate bg color, check config.h colors[]");
	}
}

static void
initimlib(void)
{
	imlib_context_set_display(dpy);
	imlib_context_set_visual(DefaultVisual(dpy, screen));
	imlib_context_set_colormap(DefaultColormap(dpy, screen));
	imlib_context_set_drawable(bbuf); /* off-screen buffer, not the window - see draw() */
	imlib_context_set_blend(1);
}

/* run a block's command via /bin/sh -c, capturing stdout as its new text.
 * btn is 0 for timer/signal-triggered runs, or the X button number (1-5)
 * for click-triggered runs, exposed to the command as $BUTTON.
 *
 * this blocks the whole bar until the command exits - fine for the fast
 * one-liners status blocks are meant to be, not fine for anything slow;
 * that tradeoff is the same one dwmblocks makes. */
static void
runblock(int i, int btn)
{
	int fd[2];
	pid_t pid;
	ssize_t n;
	char buf[256];

	if (pipe(fd) < 0)
		return;

	if ((pid = fork()) < 0) {
		close(fd[0]);
		close(fd[1]);
		return;
	}

	if (pid == 0) { /* child */
		char btnstr[4];
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		close(fd[1]);
		if (btn) {
			snprintf(btnstr, sizeof(btnstr), "%d", btn);
			setenv("BUTTON", btnstr, 1);
		}
		execl("/bin/sh", "sh", "-c", blocks[i].command, (char *)NULL);
		_exit(127);
	}

	/* parent: read to EOF or a full buffer, not just once - a single
	 * read() on a pipe can return early with only part of the output if
	 * the child writes it in more than one piece, which a one-shot read
	 * would silently truncate. */
	close(fd[1]);
	n = 0;
	while ((size_t)n < sizeof(buf) - 1) {
		ssize_t r = read(fd[0], buf + n, sizeof(buf) - 1 - (size_t)n);
		if (r <= 0)
			break;
		n += r;
	}
	close(fd[0]);
	waitpid(pid, NULL, 0);

	if (n < 0)
		n = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	buf[n] = '\0';

	free(blocktext[i]);
	blocktext[i] = strdup(buf);
}

static void
initblocks(void)
{
	int i;

	for (i = 0; i < NBLOCKS; i++) {
		countdown[i] = blocks[i].interval;
		runblock(i, 0); /* populate initial text before the first draw */
	}
}

/* one-second timer tick: decrement every timed block's countdown, re-run
 * any that hit zero. signal-only blocks (interval == 0) are untouched
 * here entirely - they only change via runblock() from the signalfd path. */
static int
tick(void)
{
	int i, dirty = 0;

	for (i = 0; i < NBLOCKS; i++) {
		if (!blocks[i].interval)
			continue;
		if (--countdown[i] == 0) {
			runblock(i, 0);
			countdown[i] = blocks[i].interval;
			dirty = 1;
		}
	}
	return dirty;
}

/* prefer _NET_WM_NAME (UTF-8); fall back to WM_NAME. WM_NAME is assumed
 * latin1/ascii - no charset conversion is attempted, a known limitation
 * for legacy clients using an exotic WM_NAME encoding. */
static void
gettitle(Window w, char *buf, size_t bufsz)
{
	Atom type;
	int fmt;
	unsigned long n, rest;
	unsigned char *prop = NULL;

	buf[0] = '\0';

	if (XGetWindowProperty(dpy, w, netwmname, 0, 1024, False, utf8string,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		snprintf(buf, bufsz, "%s", (char *)prop);
		XFree(prop);
		if (buf[0])
			return;
	}

	if (XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 1024, False, AnyPropertyType,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		snprintf(buf, bufsz, "%s", (char *)prop);
		XFree(prop);
	}
}

/* WM_CLASS's res_class (e.g. "Brave-browser", "kitty") - the standard
 * ICCCM way to identify "windows of the same application". */
static void
getclass(Window w, char *buf, size_t bufsz)
{
	XClassHint ch = { NULL, NULL };

	buf[0] = '\0';
	if (XGetClassHint(dpy, w, &ch)) {
		if (ch.res_class)
			snprintf(buf, bufsz, "%s", ch.res_class);
		if (ch.res_name)
			XFree(ch.res_name);
		if (ch.res_class)
			XFree(ch.res_class);
	}
}

/* _NET_WM_ICON is one or more { width, height, width*height ARGB pixels }
 * blocks concatenated together; each pixel is packed into the low 32 bits
 * of a (possibly 64-bit) CARDINAL. Pick the largest available icon and let
 * Imlib2 downscale it - that generally looks better than upscaling a
 * small one. */
static Imlib_Image
loadicon(Window w)
{
	Atom type;
	int fmt;
	unsigned long n, rest, i, bestoff = 0, bestpx = 0, width, height, px;
	unsigned char *prop = NULL;
	long *data;
	DATA32 *pixels;
	Imlib_Image img = NULL;

	if (XGetWindowProperty(dpy, w, netwmicon, 0, LONG_MAX, False, XA_CARDINAL,
	                        &type, &fmt, &n, &rest, &prop) != Success || !prop)
		return NULL;

	data = (long *)prop;
	i = 0;
	while (i + 2 <= n) {
		width = (unsigned long)data[i];
		height = (unsigned long)data[i + 1];
		if (width == 0 || height == 0 || i + 2 + width * height > n)
			break;
		px = width * height;
		if (px > bestpx) {
			bestpx = px;
			bestoff = i;
		}
		i += 2 + px;
	}

	if (bestpx == 0) {
		XFree(prop);
		return NULL;
	}

	width = (unsigned long)data[bestoff];
	height = (unsigned long)data[bestoff + 1];
	if (!(pixels = malloc(bestpx * sizeof(DATA32)))) {
		XFree(prop);
		return NULL;
	}
	for (i = 0; i < bestpx; i++)
		pixels[i] = (DATA32)(data[bestoff + 2 + i] & 0xffffffffUL);

	img = imlib_create_image_using_copied_data((int)width, (int)height, pixels);
	free(pixels);
	XFree(prop);
	return img;
}

/* two flags a window needs during a taskbar rebuild: should it be
 * skipped entirely (docks/desktops/splash screens, or anything
 * explicitly marked _NET_WM_STATE_SKIP_TASKBAR), and is it urgent. Both
 * come from atoms inside _NET_WM_STATE, so this fetches that property
 * once and checks it for both rather than the two separate round-trips
 * two separate functions would each make for the same property. */
static void
getwinflags(Window w, int *skip, int *urgent)
{
	Atom type, *atoms;
	int fmt;
	unsigned long n, rest, i;
	unsigned char *prop = NULL;

	*skip = 0;
	*urgent = 0;

	if (XGetWindowProperty(dpy, w, netwmstate, 0, 64, False, XA_ATOM,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		atoms = (Atom *)prop;
		for (i = 0; i < n; i++) {
			if (atoms[i] == netwmstateskiptaskbar)
				*skip = 1;
			else if (atoms[i] == netwmstatedemandsattention)
				*urgent = 1;
		}
		XFree(prop);
	}
	if (*skip)
		return;

	if (XGetWindowProperty(dpy, w, netwmwindowtype, 0, 16, False, XA_ATOM,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		atoms = (Atom *)prop;
		for (i = 0; i < n; i++)
			if (atoms[i] == netwmwindowtypedock || atoms[i] == netwmwindowtypedesktop ||
			    atoms[i] == netwmwindowtypesplash)
				*skip = 1;
		XFree(prop);
	}
}

static void
sendclientmsg(Window target, Atom msgatom, long l0, long l1, long l2)
{
	XClientMessageEvent e = { 0 };

	e.type = ClientMessage;
	e.window = target;
	e.message_type = msgatom;
	e.format = 32;
	e.data.l[0] = l0;
	e.data.l[1] = l1;
	e.data.l[2] = l2;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&e);
}

/* read the WM's current workspace straight from the root window - no
 * polling needed, root already has PropertyChangeMask selected (see
 * setupwin), so a workspace switch just shows up as a PropertyNotify. */
static void
getcurdesktop(void)
{
	Atom type;
	int fmt;
	unsigned long n, rest;
	unsigned char *prop = NULL;

	if (XGetWindowProperty(dpy, root, netcurrentdesktop, 0, 1, False, XA_CARDINAL,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		if (n > 0)
			curdesktop = (int)*(unsigned long *)prop;
		XFree(prop);
	}
}

/* how many workspaces exist, for the picker - refreshed the same
 * event-driven way as getcurdesktop(), off a PropertyNotify on root. */
static void
getnumdesktops(void)
{
	Atom type;
	int fmt;
	unsigned long n, rest;
	unsigned char *prop = NULL;

	numdesktops = 1;
	if (XGetWindowProperty(dpy, root, netnumberofdesktops, 0, 1, False, XA_CARDINAL,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		if (n > 0 && *(unsigned long *)prop > 0)
			numdesktops = (int)*(unsigned long *)prop;
		XFree(prop);
	}
}

/* jump delta workspaces relative to curdesktop, wrapping around - used by
 * both the picker's scroll fallback and the global bar-scroll (see
 * handlebuttonpress). curdesktop itself is only updated once the WM
 * confirms via the _NET_CURRENT_DESKTOP PropertyNotify this triggers. */
static void
switchworkspace(int delta)
{
	int n = numdesktops > 0 ? numdesktops : 1;
	int target = ((curdesktop + delta) % n + n) % n;

	sendclientmsg(root, netcurrentdesktop, target, CurrentTime, 0);
}

/* activate a window that may be on a different workspace. The EWMH spec
 * says a WM "should" switch to the window's desktop when it honors
 * _NET_ACTIVE_WINDOW, but not every WM actually does that as part of
 * handling the activate message - so switch explicitly first instead of
 * relying on it, by reading the window's _NET_WM_DESKTOP and sending a
 * _NET_CURRENT_DESKTOP message if it differs from the current one. */
static void
activatewindow(Window w)
{
	Atom type;
	int fmt;
	unsigned long n, rest, desktop = (unsigned long)-1;
	unsigned char *prop = NULL;

	if (XGetWindowProperty(dpy, w, netwmdesktop, 0, 1, False, XA_CARDINAL,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		if (n > 0)
			desktop = *(unsigned long *)prop;
		XFree(prop);
	}

	if (desktop != (unsigned long)-1 && desktop != 0xFFFFFFFFUL)
		sendclientmsg(root, netcurrentdesktop, (long)desktop, CurrentTime, 0);

	sendclientmsg(w, netactivewindow, 2, CurrentTime, 0);
}

static void
freetaskicons(void)
{
	int i;

	for (i = 0; i < ntaskwins; i++) {
		if (taskwins[i].icon) {
			imlib_context_set_image(taskwins[i].icon);
			imlib_free_image();
		}
	}
	free(taskwins);
	taskwins = NULL;
	ntaskwins = 0;
}

/* group icons are borrowed from a member's TaskWin.icon (freed there via
 * freetaskicons), so this only ever frees the array itself. */
static void
freetaskgroups(void)
{
	free(taskgroups);
	taskgroups = NULL;
	ntaskgroups = 0;
}

/* truncate title into dst so it fits maxw px, appending "...". Not
 * UTF-8-boundary aware - fine for ascii titles, may clip a multi-byte
 * character on others (documented limitation, see DESIGN.md milestone 6). */
static void
fittext(const char *src, char *dst, size_t dstsz, int maxw)
{
	XGlyphInfo ext;
	size_t len;

	if (maxw <= 0) {
		dst[0] = '\0';
		return;
	}

	snprintf(dst, dstsz, "%s", src);
	XftTextExtentsUtf8(dpy, font, (const FcChar8 *)dst, (int)strlen(dst), &ext);
	if (ext.xOff <= maxw)
		return;

	for (len = strlen(dst); len > 0; len--) {
		dst[len] = '\0';
		if (len >= 3)
			memcpy(dst + len - 3, "...", 3);
		XftTextExtentsUtf8(dpy, font, (const FcChar8 *)dst, (int)strlen(dst), &ext);
		if (ext.xOff <= maxw)
			return;
	}
}

static void
closepopup(void)
{
	if (popupwin == None)
		return;
	XUngrabPointer(dpy, CurrentTime);
	XftDrawDestroy(popupdraw);
	XDestroyWindow(dpy, popupwin);
	popupwin = None;
	popupdraw = NULL;
	opengroup = -1;
}

static void
drawpopup(void)
{
	int i, y = 0, iconsize, scheme, textx;
	char label[256];

	iconsize = (int)barheight - 2 * (int)taskbtniconpad;
	if (iconsize < 0)
		iconsize = 0;
	imlib_context_set_drawable(popupwin);

	for (i = 0; i < npoprows; i++) {
		TaskWin *tw = &taskwins[poprows[i]];

		scheme = tw->active ? SchemeSel : tw->urgent ? SchemeUrg : SchemeNorm;
		XSetForeground(dpy, gc, colscheme[scheme][1].pixel);
		XFillRectangle(dpy, popupwin, gc, 0, y, (unsigned)popupw, barheight);

		textx = (int)taskbtniconpad;
		if (tw->icon && iconsize > 0) {
			imlib_context_set_image(tw->icon);
			imlib_render_image_on_drawable_at_size((int)taskbtniconpad,
			                                        y + ((int)barheight - iconsize) / 2,
			                                        iconsize, iconsize);
			textx += iconsize + (int)taskbtniconpad;
		}

		fittext(tw->title, label, sizeof(label), popupw - textx - (int)taskbtniconpad);
		if (label[0])
			XftDrawStringUtf8(popupdraw, &colscheme[scheme][0], font, textx,
			                   y + ((int)barheight + font->ascent - font->descent) / 2,
			                   (const FcChar8 *)label, (int)strlen(label));
		y += (int)barheight;
	}
	XFlush(dpy);
}

/* opens (or, on a repeat click of the same group, closes) the dropdown
 * listing every window in group gi, positioned directly below its button.
 * A pointer grab makes every subsequent click - inside or outside the
 * popup - land on handlepopupclick() instead of wherever it visually is,
 * which is what lets a click anywhere else dismiss it. */
static void
openpopup(int gi)
{
	XSetWindowAttributes wa = { 0 };
	int i, y;

	if (opengroup == gi) {
		closepopup();
		return;
	}
	closepopup();

	/* match by class, not a stored group index - taskgroups[] gets
	 * reordered after classification (drag-to-reorder, and reapplying
	 * the saved order on every rebuild), which would leave a stored
	 * index pointing at the wrong slot. Only reached when nmembers > 1,
	 * i.e. only when grouping is genuinely on, where "same class" and
	 * "same group" are exactly equivalent - no ambiguity. */
	npoprows = 0;
	for (i = 0; i < ntaskwins && npoprows < (int)LENGTH(poprows); i++)
		if (strcmp(taskwins[i].class, taskgroups[gi].class) == 0)
			poprows[npoprows++] = i;
	if (npoprows == 0)
		return;

	popupw = taskgroups[gi].region.x2 - taskgroups[gi].region.x1;
	if (popupw < 120)
		popupw = 120;
	y = topbar ? mony + (int)barheight
	           : mony + (int)monh - (int)barheight - npoprows * (int)barheight;

	wa.override_redirect = True;
	wa.background_pixel = BlackPixel(dpy, screen);
	wa.event_mask = ExposureMask | ButtonPressMask;
	popupwin = XCreateWindow(dpy, root, monx + taskgroups[gi].region.x1, y,
	                          (unsigned)popupw, (unsigned)(npoprows * (int)barheight), 0,
	                          DefaultDepth(dpy, screen), CopyFromParent, DefaultVisual(dpy, screen),
	                          CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
	popupdraw = XftDrawCreate(dpy, popupwin, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));

	XMapRaised(dpy, popupwin);
	XGrabPointer(dpy, popupwin, False, ButtonPressMask, GrabModeAsync, GrabModeAsync,
	             None, None, CurrentTime);

	opengroup = gi;
	drawpopup();
}

/* with the pointer grabbed, every click's coordinates are reported
 * relative to popupwin regardless of where it actually landed - outside
 * popupwin's own bounds means "clicked elsewhere", i.e. dismiss. */
static void
handlepopupclick(XButtonEvent *e)
{
	int row;

	if (e->x < 0 || e->x >= popupw || e->y < 0 || e->y >= npoprows * (int)barheight) {
		closepopup();
		return;
	}

	row = e->y / (int)barheight;
	if (row >= 0 && row < npoprows) {
		TaskWin *tw = &taskwins[poprows[row]];

		if (e->button == Button2)
			sendclientmsg(tw->win, netclosewindow, CurrentTime, 2, 0);
		else
			activatewindow(tw->win);
	}
	closepopup();
}

/* apply the user's dragged left-to-right order to a freshly built groups
 * array, in place: each remembered class (in order) is moved to the next
 * output slot if still present; anything left over (never ordered, or
 * grouping just turned it into a new group) keeps its natural order at
 * the end. Classes with no match are simply skipped - no pruning needed. */
static void
applygrouporder(TaskGroup *g, int ng)
{
	TaskGroup *out;
	int *placed;
	int i, j, oi = 0;

	if (ng <= 0 || !(out = malloc((size_t)ng * sizeof(TaskGroup))))
		return;
	if (!(placed = calloc((size_t)ng, sizeof(int)))) {
		free(out);
		return;
	}

	for (i = 0; i < ngrouporder; i++) {
		for (j = 0; j < ng; j++) {
			if (!placed[j] && strcmp(g[j].class, grouporder[i]) == 0) {
				out[oi++] = g[j];
				placed[j] = 1;
				break;
			}
		}
	}
	for (j = 0; j < ng; j++)
		if (!placed[j])
			out[oi++] = g[j];

	memcpy(g, out, (size_t)ng * sizeof(TaskGroup));
	free(out);
	free(placed);
}

/* cluster nw[0..cnt) into groups by WM_CLASS (or one window per group if
 * taskgroupbyclass is off). O(cnt^2) class-name comparisons, irrelevant
 * at taskbar-sized counts. */
static void
groupwindows(TaskWin *nw, int cnt)
{
	TaskGroup *g;
	int i, j, ng = 0, gi;

	g = calloc(cnt ? cnt : 1, sizeof(TaskGroup)); /* worst case: all distinct */
	if (!g)
		return;

	for (i = 0; i < cnt; i++) {
		gi = -1;
		if (taskgroupbyclass)
			for (j = 0; j < ng; j++)
				if (strcmp(g[j].class, nw[i].class) == 0) {
					gi = j;
					break;
				}
		if (gi < 0) {
			gi = ng++;
			snprintf(g[gi].class, sizeof(g[gi].class), "%s", nw[i].class);
			g[gi].firstmember = i;
		}
		g[gi].nmembers++;
		g[gi].active |= nw[i].active;
		g[gi].urgent |= nw[i].urgent;
		if (!g[gi].icon)
			g[gi].icon = nw[i].icon;
	}
	applygrouporder(g, ng);

	freetaskgroups();
	taskgroups = g;
	ntaskgroups = ng;
}

/* remember the on-screen left-to-right order (by class) after a drag, so
 * the next taskgroups[] rebuild (see applygrouporder) preserves it. */
static void
savegrouporder(void)
{
	int i;

	ngrouporder = ntaskgroups < MAXGROUPORDER ? ntaskgroups : MAXGROUPORDER;
	for (i = 0; i < ngrouporder; i++)
		snprintf(grouporder[i], sizeof(grouporder[i]), "%s", taskgroups[i].class);
}

/* rebuild the taskbar's window list from scratch: _NET_CLIENT_LIST plus
 * per-window name/icon/state, then re-cluster into taskgroups[]. Called
 * on startup and whenever a PropertyNotify suggests the old list might be
 * stale - not polled. */
static void
updatetasklist(void)
{
	Atom type;
	int fmt;
	unsigned long n, rest, i;
	unsigned char *prop = NULL;
	Window *wins = NULL;
	Window active = None;
	TaskWin *nw;
	int cnt = 0;

	/* the popup and any in-progress drag reference indices into the
	 * taskwins[]/taskgroups[] arrays we're about to rebuild - stale
	 * indices would be a use-after-free */
	closepopup();
	dragcandidate = -1;
	dragging = 0;

	if (XGetWindowProperty(dpy, root, netactivewindow, 0, 1, False, XA_WINDOW,
	                        &type, &fmt, &n, &rest, &prop) == Success && prop) {
		if (n > 0)
			active = *(Window *)prop;
		XFree(prop);
	}

	prop = NULL;
	if (XGetWindowProperty(dpy, root, netclientlist, 0, 4096, False, XA_WINDOW,
	                        &type, &fmt, &n, &rest, &prop) != Success || !prop)
		n = 0;
	if (prop)
		wins = (Window *)prop;

	nw = calloc(n ? n : 1, sizeof(TaskWin));
	if (!nw) {
		if (prop)
			XFree(prop);
		return;
	}

	for (i = 0; i < n; i++) {
		Window w = wins[i];
		int skip, urgent;

		getwinflags(w, &skip, &urgent);
		if (skip)
			continue;
		nw[cnt].win = w;
		gettitle(w, nw[cnt].title, sizeof(nw[cnt].title));
		getclass(w, nw[cnt].class, sizeof(nw[cnt].class));
		nw[cnt].icon = loadicon(w);
		nw[cnt].active = (w == active);
		nw[cnt].urgent = urgent;
		XSelectInput(dpy, w, PropertyChangeMask);
		cnt++;
	}
	if (prop)
		XFree(prop);

	freetaskicons();
	taskwins = nw;
	ntaskwins = cnt;
	groupwindows(nw, cnt);
}

/* far-left workspace picker: one fixed-width button per workspace (from
 * _NET_NUMBER_OF_DESKTOPS, capped at MAXDESKTOPS), current one highlighted
 * - click a button to jump straight to it. Populates wsregions[]/pickerw
 * for handlebuttonpress()/draw() to use; a no-op leaving pickerw at 0 when
 * disabled, so callers don't need their own showworkspacepicker check. */
static void
drawpicker(void)
{
	XGlyphInfo ext;
	char label[8];
	int i, x = 0, scheme, n, tx;

	pickerw = 0;
	nwsregions = 0;
	if (!showworkspacepicker)
		return;

	n = numdesktops < MAXDESKTOPS ? numdesktops : MAXDESKTOPS;

	for (i = 0; i < n; i++) {
		scheme = (i == curdesktop) ? SchemeSel : SchemeNorm;
		XSetForeground(dpy, gc, colscheme[scheme][1].pixel);
		XFillRectangle(dpy, bbuf, gc, x, 0, wspickerbtnw, barheight);

		snprintf(label, sizeof(label), "%d", i + 1);
		XftTextExtentsUtf8(dpy, font, (const FcChar8 *)label, (int)strlen(label), &ext);
		tx = x + ((int)wspickerbtnw - ext.xOff) / 2;
		XftDrawStringUtf8(xftdraw, &colscheme[scheme][0], font, tx,
		                   ((int)barheight + font->ascent - font->descent) / 2,
		                   (const FcChar8 *)label, (int)strlen(label));

		wsregions[i].x1 = x;
		wsregions[i].x2 = x + (int)wspickerbtnw;
		x += (int)wspickerbtnw;
	}
	nwsregions = n;
	pickerw = x;
}

/* left-aligned, Windows-taskbar style: every button is taskbtnmaxw wide
 * and packed directly against the next, leaving unused space blank at
 * the right - unless that doesn't fit, in which case (and only then)
 * every button shrinks evenly to the same width so everything still
 * fits, via cumulative column boundaries (i*availw/n) rather than a
 * fixed per-button width so integer rounding doesn't drift or leave a
 * gap at the end. Buttons narrower than taskbtnminw fall back to
 * icon-only (no title) rather than showing an unreadable text sliver.
 * xstart offsets everything past the workspace picker (see drawpicker),
 * 0 when that's disabled; availw is the width budget for tasks alone. */
static void
drawtaskbar(int xstart, int availw)
{
	int x = xstart, i, iconsize, scheme, shrink;
	char raw[256], label[256];

	iconsize = (int)barheight - 2 * (int)taskbtniconpad;
	if (iconsize < 0)
		iconsize = 0;
	if (availw < 0)
		availw = 0;
	shrink = ntaskgroups > 0 && (long)ntaskgroups * taskbtnmaxw > availw;

	imlib_context_set_drawable(bbuf);

	for (i = 0; i < ntaskgroups; i++) {
		TaskGroup *g = &taskgroups[i];
		int w, textx;

		if (shrink) {
			int x2 = xstart + (int)(((long)(i + 1) * availw) / ntaskgroups);
			w = x2 - x;
		} else {
			w = (int)taskbtnmaxw;
			if (x + w > xstart + availw)
				w = xstart + availw - x;
		}
		if (w < 0)
			w = 0;

		g->region.x1 = x;
		g->region.x2 = x + w;

		scheme = g->active ? SchemeSel : g->urgent ? SchemeUrg : SchemeNorm;
		XSetForeground(dpy, gc, colscheme[scheme][1].pixel);
		XFillRectangle(dpy, bbuf, gc, x, 0, w, barheight);

		textx = x + (int)taskbtniconpad;
		if (g->icon && iconsize > 0 && w >= iconsize + 2 * (int)taskbtniconpad) {
			imlib_context_set_image(g->icon);
			imlib_render_image_on_drawable_at_size(x + (int)taskbtniconpad,
			                                        ((int)barheight - iconsize) / 2,
			                                        iconsize, iconsize);
			textx += iconsize + (int)taskbtniconpad;
		}

		if (w >= (int)taskbtnminw) {
			if (g->nmembers > 1)
				snprintf(raw, sizeof(raw), "%s (%d)", g->class, g->nmembers);
			else
				snprintf(raw, sizeof(raw), "%s", taskwins[g->firstmember].title);
			fittext(raw, label, sizeof(label), x + w - textx - (int)taskbtniconpad);
			if (label[0])
				XftDrawStringUtf8(xftdraw, &colscheme[scheme][0], font, textx,
				                   ((int)barheight + font->ascent - font->descent) / 2,
				                   (const FcChar8 *)label, (int)strlen(label));
		}

		x += w;
	}
}

/* draws into the off-screen buffer bbuf, then blits it onto the visible
 * window in one XCopyArea - drawing (clear, then redraw piece by piece)
 * directly onto the window instead would flash briefly-blank on every
 * block tick, since the user would see the intermediate states. */
static void
draw(void)
{
	char labels[NBLOCKS][512], wslabel[16];
	int widths[NBLOCKS];
	int totalw = 0, x, i, availw, wsw = 0, taskx;
	int rightmargin = PAD + (tickerenabled ? (int)tickerwidth + GAP : 0);
	XGlyphInfo ext;

	XSetForeground(dpy, gc, colscheme[SchemeNorm][1].pixel);
	XFillRectangle(dpy, bbuf, gc, 0, 0, monw, barheight);

	drawpicker();

	if (showworkspace) {
		snprintf(wslabel, sizeof(wslabel), "W%d", curdesktop + 1);
		XftTextExtentsUtf8(dpy, font, (const FcChar8 *)wslabel, (int)strlen(wslabel), &ext);
		wsw = ext.xOff;
		totalw += wsw + (NBLOCKS > 0 ? GAP : 0);
	}

	/* build each block's label once and reuse it for both the width
	 * measurement below and the actual draw further down, instead of
	 * formatting the same string twice - besides the redundant work,
	 * two copies of the same format code could otherwise drift apart. */
	for (i = 0; i < NBLOCKS; i++) {
		snprintf(labels[i], sizeof(labels[i]), "%s%s%s", blocks[i].icon,
		         blocks[i].icon[0] ? " " : "", blocktext[i] ? blocktext[i] : "");
		XftTextExtentsUtf8(dpy, font, (const FcChar8 *)labels[i], (int)strlen(labels[i]), &ext);
		widths[i] = ext.xOff;
		totalw += widths[i] + (i > 0 ? GAP : 0);
	}

	x = monw - rightmargin - totalw;

	if (showworkspace) {
		XftDrawStringUtf8(xftdraw, &colscheme[SchemeNorm][0], font, x,
		                   ((int)barheight + font->ascent - font->descent) / 2,
		                   (const FcChar8 *)wslabel, (int)strlen(wslabel));
		x += wsw + (NBLOCKS > 0 ? GAP : 0);
	}

	for (i = 0; i < NBLOCKS; i++) {
		blockregions[i].x1 = x;
		blockregions[i].x2 = x + widths[i];

		XftDrawStringUtf8(xftdraw, &colscheme[SchemeNorm][0], font, x,
		                   ((int)barheight + font->ascent - font->descent) / 2,
		                   (const FcChar8 *)labels[i], (int)strlen(labels[i]));
		x += widths[i] + GAP;
	}

	/* taskx: past the picker plus its own gap, or 0 flush against the
	 * left edge when the picker is disabled (pickerw == 0) - matches the
	 * pre-picker layout exactly in that case. */
	taskx = pickerw > 0 ? pickerw + TASKGAP : 0;
	availw = monw - rightmargin - totalw - TASKGAP - taskx;
	if (availw < 0)
		availw = 0;
	drawtaskbar(taskx, availw);

	/* scrolling ticker, in the space reserved for it via rightmargin
	 * above: draw tickertext twice, tickerloopw px apart, clipped to a
	 * fixed-width viewport, so as one copy scrolls off the left the
	 * other is already lined up to continue seamlessly - no per-frame
	 * string-slicing needed. */
	if (tickerenabled) {
		int vx = monw - PAD - (int)tickerwidth;
		int tx = vx - (int)(tickeroff % (unsigned)tickerloopw);
		int ty = ((int)barheight + font->ascent - font->descent) / 2;
		XRectangle rect = { (short)vx, 0, (unsigned short)tickerwidth, (unsigned short)barheight };

		XftDrawSetClipRectangles(xftdraw, 0, 0, &rect, 1);
		XftDrawStringUtf8(xftdraw, &colscheme[SchemeNorm][0], font, tx, ty,
		                   (const FcChar8 *)tickertext, (int)strlen(tickertext));
		XftDrawStringUtf8(xftdraw, &colscheme[SchemeNorm][0], font, tx + tickerloopw, ty,
		                   (const FcChar8 *)tickertext, (int)strlen(tickertext));
		XftDrawSetClip(xftdraw, None);
	}

	XCopyArea(dpy, bbuf, win, gc, 0, 0, monw, barheight, 0, 0);
	XFlush(dpy);
}

static void
inittimer(void)
{
	struct itimerspec its = { 0 };

	if ((tfd = timerfd_create(CLOCK_MONOTONIC, 0)) < 0)
		die("timerfd_create failed");

	its.it_value.tv_sec = 1;
	its.it_interval.tv_sec = 1;
	if (timerfd_settime(tfd, 0, &its, NULL) < 0)
		die("timerfd_settime failed");
}

/* a second, independent timerfd for the scrolling ticker - deliberately
 * not sharing tfd/tick() with the status blocks, so the block engine
 * (already verified working) stays untouched regardless of whether the
 * ticker is even enabled. */
static void
initticker(void)
{
	XGlyphInfo ext;
	struct itimerspec its = { 0 };
	long ns;

	if (!tickerenabled)
		return;

	XftTextExtentsUtf8(dpy, font, (const FcChar8 *)tickertext, (int)strlen(tickertext), &ext);
	tickerloopw = ext.xOff + (int)tickergap;
	if (tickerloopw <= 0)
		tickerloopw = 1; /* guard against an empty tickertext */

	if ((tickerfd = timerfd_create(CLOCK_MONOTONIC, 0)) < 0)
		die("timerfd_create (ticker) failed");

	ns = (long)tickerinterval * 1000000L;
	its.it_value.tv_sec = ns / 1000000000L;
	its.it_value.tv_nsec = ns % 1000000000L;
	its.it_interval = its.it_value;
	if (timerfd_settime(tickerfd, 0, &its, NULL) < 0)
		die("timerfd_settime (ticker) failed");
}

/* block SIGINT/SIGTERM and every block's realtime signal, then read them
 * back via signalfd from the main poll() loop instead of an async
 * signal handler - keeps the whole engine single-threaded and simple. */
static void
initsignals(void)
{
	sigset_t mask;
	int i;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	for (i = 0; i < NBLOCKS; i++)
		if (blocks[i].signal)
			sigaddset(&mask, SIGRTMIN + blocks[i].signal);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		die("sigprocmask failed");

	if ((sfd = signalfd(-1, &mask, SFD_NONBLOCK)) < 0)
		die("signalfd failed");
}

/* left-click activates (or, for a multi-window group, toggles the
 * dropdown); middle-click closes. Shared between the immediate-action
 * paths (button != Button1) and the deferred one (a Button1 press that
 * turned out not to be a drag - see handlebuttonrelease). */
static void
taskgroupaction(int gi, int button)
{
	TaskGroup *g = &taskgroups[gi];

	if (g->nmembers > 1) {
		openpopup(gi);
		return;
	}
	if (button == Button2)
		sendclientmsg(taskwins[g->firstmember].win, netclosewindow, CurrentTime, 2, 0);
	else
		activatewindow(taskwins[g->firstmember].win);
}

static void
handlebuttonpress(XButtonEvent *e)
{
	int i;

	for (i = 0; i < NBLOCKS; i++) {
		if (e->x >= blockregions[i].x1 && e->x < blockregions[i].x2) {
			runblock(i, (int)e->button);
			draw();
			return;
		}
	}

	/* scroll anywhere else on the bar cycles the workspace - a block
	 * above gets first refusal (it already sees scroll as $BUTTON 4/5),
	 * this is everywhere past that: the picker, taskbar, empty space. */
	if (scrollswitchesws && (e->button == Button4 || e->button == Button5)) {
		switchworkspace(e->button == Button4 ? -1 : 1);
		return;
	}

	for (i = 0; i < nwsregions; i++) {
		if (e->x >= wsregions[i].x1 && e->x < wsregions[i].x2) {
			sendclientmsg(root, netcurrentdesktop, i, CurrentTime, 0);
			return;
		}
	}

	for (i = 0; i < ntaskgroups; i++) {
		TaskGroup *g = &taskgroups[i];

		if (g->region.x2 <= g->region.x1)
			continue;
		if (e->x >= g->region.x1 && e->x < g->region.x2) {
			if (e->button == Button1) {
				/* don't act yet - this might turn into a drag instead
				 * of a click, see handlemotion/handlebuttonrelease */
				dragcandidate = i;
				dragstartx = e->x;
				dragging = 0;
			} else {
				taskgroupaction(i, (int)e->button);
			}
			return;
		}
	}
}

/* while a Button1 press on a taskbar button is held: past DRAGTHRESHOLD
 * of movement, it's a drag - swap the dragged button with whichever
 * button the pointer is now over. A plain array-element swap (rather
 * than a floating drag visual) keeps this simple; since it only ever
 * swaps within taskgroups[], the dragged button can never cross into the
 * status-block area. */
static void
handlemotion(XMotionEvent *e)
{
	int i, cur = -1;

	if (dragcandidate < 0)
		return;

	if (!dragging) {
		if (abs(e->x - dragstartx) < DRAGTHRESHOLD)
			return;
		dragging = 1;
	}

	for (i = 0; i < ntaskgroups; i++) {
		if (e->x >= taskgroups[i].region.x1 && e->x < taskgroups[i].region.x2) {
			cur = i;
			break;
		}
	}
	if (cur >= 0 && cur != dragcandidate) {
		TaskGroup tmp = taskgroups[dragcandidate];
		taskgroups[dragcandidate] = taskgroups[cur];
		taskgroups[cur] = tmp;
		dragcandidate = cur;
		draw();
	}
}

static void
handlebuttonrelease(XButtonEvent *e)
{
	(void)e;

	if (dragcandidate < 0)
		return;

	if (dragging)
		savegrouporder();
	else
		taskgroupaction(dragcandidate, Button1);

	dragcandidate = -1;
	dragging = 0;
}

static void
handlesignals(void)
{
	struct signalfd_siginfo si;
	int i;

	while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
		if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
			running = 0;
			continue;
		}
		for (i = 0; i < NBLOCKS; i++) {
			if (blocks[i].signal && (int)si.ssi_signo == SIGRTMIN + blocks[i].signal) {
				runblock(i, 0);
				draw();
			}
		}
	}
}

/* is this a property change worth a full taskbar rebuild? root's client
 * list/active window, or any tracked window's name/state/icon. Not
 * netwmdesktop: a window changing which workspace it's on doesn't change
 * anything the taskbar shows (windows from every desktop are listed
 * already, and no per-window desktop info is displayed), so rebuilding
 * for it would just be pointless work. */
static int
istaskrelevant(Atom a)
{
	return a == netclientlist || a == netactivewindow || a == netwmname ||
	       a == XA_WM_NAME || a == netwmstate || a == netwmicon;
}

static void
run(void)
{
	struct pollfd fds[4];
	XEvent ev;
	uint64_t expirations;

	fds[0].fd = ConnectionNumber(dpy);
	fds[0].events = POLLIN;
	fds[1].fd = tfd;
	fds[1].events = POLLIN;
	fds[2].fd = sfd;
	fds[2].events = POLLIN;
	fds[3].fd = tickerenabled ? tickerfd : -1; /* poll() ignores negative fds */
	fds[3].events = POLLIN;

	while (running) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			switch (ev.type) {
			case Expose:
				if (ev.xexpose.count != 0)
					break;
				if (ev.xexpose.window == popupwin)
					drawpopup();
				else
					draw();
				break;
			case ButtonPress:
				if (popupwin != None)
					handlepopupclick(&ev.xbutton);
				else
					handlebuttonpress(&ev.xbutton);
				break;
			case MotionNotify:
				handlemotion(&ev.xmotion);
				break;
			case ButtonRelease:
				handlebuttonrelease(&ev.xbutton);
				break;
			case PropertyNotify:
				if (ev.xproperty.atom == netcurrentdesktop) {
					getcurdesktop();
					draw();
				} else if (ev.xproperty.atom == netnumberofdesktops) {
					getnumdesktops();
					draw();
				} else if (istaskrelevant(ev.xproperty.atom)) {
					updatetasklist();
					draw();
				}
				break;
			default:
				break;
			}
		}
		if (!running)
			break;

		if (poll(fds, 4, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (fds[1].revents & POLLIN) {
			read(tfd, &expirations, sizeof(expirations));
			if (tick())
				draw();
		}
		if (fds[2].revents & POLLIN)
			handlesignals();
		if (fds[3].revents & POLLIN) {
			read(tickerfd, &expirations, sizeof(expirations));
			tickeroff += tickerstep;
			draw();
		}
	}
}

static void
cleanup(void)
{
	int i;

	close(tfd);
	close(sfd);
	if (tickerenabled)
		close(tickerfd);
	for (i = 0; i < NBLOCKS; i++)
		free(blocktext[i]);
	closepopup();
	freetaskicons();
	freetaskgroups();

	for (i = 0; i < (int)LENGTH(colors); i++) {
		XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &colscheme[i][0]);
		XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &colscheme[i][1]);
	}
	XftFontClose(dpy, font);
	XftDrawDestroy(xftdraw);
	XFreePixmap(dpy, bbuf);
	XFreeGC(dpy, gc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
}

int
main(void)
{
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");

	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);

	initatoms();
	initmonitor();
	setupwin();
	initfont();
	initcolors();

	gc = XCreateGC(dpy, win, 0, NULL);
	bbuf = XCreatePixmap(dpy, win, (unsigned)monw, barheight, DefaultDepth(dpy, screen));
	xftdraw = XftDrawCreate(dpy, bbuf, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));
	initimlib();

	initsignals();
	inittimer();
	initticker();

	/* from here on, property queries race against windows that can
	 * disappear mid-query (BadWindow) - tolerate that instead of dying */
	XSetErrorHandler(xerrordummy);

	initblocks();
	getcurdesktop();
	getnumdesktops();
	updatetasklist();
	draw();
	run();
	cleanup();

	return 0;
}
