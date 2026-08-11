/* obbar config - copied to config.h on first build, edit config.h (not this) */

/* appearance */
static const unsigned int barheight = 20;   /* px */
static const int topbar             = 1;    /* 1 = dock at top, 0 = bottom */

/* fonts: fontconfig pattern(s), first is primary, rest are fallbacks
 * "sans" resolves to the system default Sans-serif family. */
static const char *fonts[] = {
	"sans:size=10",
};

/* color schemes: { fg, bg } */
enum { SchemeNorm, SchemeSel, SchemeUrg };
static const char *colors[][2] = {
	/*               fg          bg        */
	[SchemeNorm] = { "#ffffff", "#000000" }, /* default: white on black */
	[SchemeSel]  = { "#000000", "#ffffff" }, /* active window */
	[SchemeUrg]  = { "#ffffff", "#cc3333" }, /* urgent window */
};

/* taskbar (left side): one button per managed window, from _NET_CLIENT_LIST.
 * every window always gets a button - the available width is divided
 * evenly across all of them, shrinking as more windows open rather than
 * dropping any. */
static const unsigned int taskbtnminw = 40;   /* px, buttons narrower than this show icon-only, no title */
static const unsigned int taskbtnmaxw = 200;  /* px, a single window never gets a wider button than this */
static const unsigned int taskbtniconpad = 4; /* px, padding around the icon inside a button */
static const int taskgroupbyclass = 1;        /* 1 = collapse same-WM_CLASS windows (e.g. all Brave
                                                * windows) into one button with a click-down list;
                                                * 0 = always one button per window */

/* workspace picker (far left, before the taskbar buttons): one small
 * clickable button per workspace, from _NET_NUMBER_OF_DESKTOPS - click a
 * button to jump straight to it, current one highlighted. Independent of
 * showworkspace below (that's just the "W<n>" text label by the clock). */
static const int showworkspacepicker   = 1;
static const unsigned int wspickerbtnw = 20;  /* px, fixed width per workspace button */

/* mouse scroll anywhere on the bar cycles to the next/previous workspace.
 * a status block under the pointer gets first refusal (it already sees
 * scroll as $BUTTON 4/5, e.g. for a volume block) before this fires. */
static const int scrollswitchesws = 1;

/* status area: current workspace number ("W<n>", 1-based), shown to the
 * left of the blocks[] below; updates instantly on workspace switch via
 * _NET_CURRENT_DESKTOP, no polling. */
static const int showworkspace = 1;

/* status blocks: each is an independent little program. "icon" is a plain
 * text/glyph prefix, not an image - real Imlib2 image icons are what the
 * taskbar buttons above use, for window icons; command is run via `sh -c`.
 *
 * interval: seconds between automatic re-runs, 0 = signal-only.
 * signal:   re-run instantly on `pkill -RTMIN+<signal> obbar`, 0 = none.
 * Both can be set on the same block - e.g. a battery block might tick
 * every 60s but also jump to signal 4 for an instant update on AC
 * plug/unplug from a udev/acpi hook. */
typedef struct {
	const char *icon;
	const char *command;
	unsigned int interval;
	int signal;
} Block;

static const Block blocks[] = {
	/* icon  command             interval  signal */
	{ "",    "date +%H:%M:%S",   1,        0 },
};

/* scrolling ticker/marquee, far right of the bar (right of the blocks
 * above). Off by default - flip tickerenabled on and set a message. */
static const int tickerenabled          = 0;
static const char *tickertext           = "your message here";
static const unsigned int tickerwidth   = 150; /* px, fixed viewport width */
static const unsigned int tickergap     = 24;  /* px of blank space between loops of the text */
static const unsigned int tickerinterval = 100; /* ms between scroll steps */
static const unsigned int tickerstep    = 1;   /* px per scroll step */
