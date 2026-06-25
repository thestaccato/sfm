/* See LICENSE file for copyright and license details. */

/* appearance */
static const char *colornormal  = "\033[7m";    /* reverse video */
static const char *colordir     = "\033[34m";   /* blue */
static const char *colorexe     = "\033[32m";   /* green */
static const char *colorsym     = "\033[36m";   /* cyan */
static const char *colorfile    = "\033[0m";    /* default */

static const char *dateformat   = "%b %e %Y";

/* 0=name 1=size 2=time 3=extension */
static int  sortmode     = 0;
static int  sortreverse  = 0;

/* basic settings */
static int  showhidden   = 0;
static int  followsymlinks = 0;
static int  wraparound   = 1;

/* keep this many lines visible above/below the cursor */
static int  scrolloff    = 5;

/* truncate filenames to this width; 0 = use full terminal */
static int  maxfnwidth   = 0;

/* what runs when you press Enter on a file
 * falls back to editor, then xdg-open
 * override with FM_OPENER or OPENER
 * examples: "xdg-open", "vi", "less", "mpv" */
static const char *opener     = NULL;

/* used for bulk rename and as a fallback opener
 * override with FM_EDITOR or EDITOR */
static const char *editor     = "vi";

static int  entrybufsz   = 256;

/* show permissions, size, and date next to filenames */
static int  showdetails  = 1;

/* little prefix chars: dirs, exes, symlinks, and the arrow for targets */
static const char *dircprefix   = "/";
static const char *exeprefix    = "*";
static const char *symprefix    = "@";
static const char *lnkprefix    = "->";

/* keybindings: { keysym, mod, action, "label" }
 * mod: 0=none 1=shift 2=ctrl 3=alt 4=meta
 * see ACTION_* enums in sfm.c, the label shows up in the status bar */

#define MOD_NONE  0
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   3
#define MOD_META  4

struct Key {
    int ch;
    int mod;
    int action;
    const char *label;
};

/* Ctrl+letter codes for keybindings:
 *   ^A=1 ^B=2 ^C=3 ^D=4 ^E=5 ^F=6 ^G=7 ^H=8  ^I=9  ^J=10
 *   ^K=11 ^L=12 ^M=13 ^N=14 ^O=15 ^P=16 ^Q=17 ^R=18 ^S=19 ^T=20
 *   ^U=21 ^V=22 ^W=23 ^X=24 ^Y=25 ^Z=26
 *
 * example: { 0x12, MOD_NONE, ACTION_RENAME, "rename" },  (that's ^R) */
static struct Key keys[] = {
    /* navigation */
    { 'j',       MOD_NONE,   ACTION_DOWN,         "down"          },
    { 'k',       MOD_NONE,   ACTION_UP,           "up"            },
    { 'J',       MOD_SHIFT,  ACTION_PGDN,         "page down"     },
    { 'K',       MOD_SHIFT,  ACTION_PGUP,         "page up"       },
    { 'g',       MOD_NONE,   ACTION_HOME,         "top"           },
    { 'G',       MOD_SHIFT,  ACTION_END,          "bottom"        },
    { 0x15,      MOD_CTRL,   ACTION_UP,           "up"            }, /* ^U */
    { 0x04,      MOD_CTRL,   ACTION_DOWN,         "down"          }, /* ^D */
    { 0x16,      MOD_CTRL,   ACTION_PGUP,         "page up"       }, /* ^V */
    { 0x02,      MOD_CTRL,   ACTION_PGDN,         "page down"     }, /* ^B */

    /* open / go back */
    { '\r',      MOD_NONE,   ACTION_ENTER,        "open"          },
    { '\n',      MOD_NONE,   ACTION_ENTER,        "open"          },
    { 127,       MOD_NONE,   ACTION_BACK,         "parent"        },
    { 0x08,      MOD_NONE,   ACTION_BACK,         "parent"        }, /* ^H  */
    { 0x7f,      MOD_NONE,   ACTION_BACK,         "parent"        },
    { 'h',       MOD_NONE,   ACTION_PARENT_DIR,   "parent"        },
    { 'l',       MOD_NONE,   ACTION_ENTER,        "open"          },

    /* file ops */
    { 'c',       MOD_NONE,   ACTION_COPY,         "copy"          },
    { 'p',       MOD_NONE,   ACTION_PASTE,        "paste"         },
    { 'm',       MOD_NONE,   ACTION_MOVE,         "move"          },
    { 'd',       MOD_NONE,   ACTION_DELETE,       "delete"        },
    { 'D',       MOD_SHIFT,  ACTION_DELETE,       "force delete"  },
    { 'a',       MOD_NONE,   ACTION_RENAME,       "rename"        },
    { 'A',       MOD_SHIFT,  ACTION_BULK_RENAME,  "bulk rename"  },
    { 'n',       MOD_NONE,   ACTION_MKDIR,        "mkdir"         },
    { 'N',       MOD_SHIFT,  ACTION_NEWFILE,      "new file"      },
    { 0x12,      MOD_NONE,   ACTION_RENAME,       "rename"        }, /* ^R */
    { 0x18,      MOD_NONE,   ACTION_MOVE,         "move"          }, /* ^X */
    { 0x10,      MOD_NONE,   ACTION_PASTE,        "paste"         }, /* ^P */
    { 0x0e,      MOD_NONE,   ACTION_MKDIR,        "mkdir"         }, /* ^N */

    /* sorting */
    { 's',       MOD_NONE,   ACTION_SORT_NAME,    "sort name"     },
    { 'S',       MOD_SHIFT,  ACTION_SORT_SIZE,    "sort size"     },
    { 't',       MOD_NONE,   ACTION_SORT_TIME,    "sort time"     },
    { 'e',       MOD_NONE,   ACTION_SORT_EXT,     "sort ext"      },
    { 'r',       MOD_NONE,   ACTION_TOGGLE_SORT_REVERSE, "reverse" },

    /* filtering */
    { '/',       MOD_NONE,   ACTION_FILTER,       "filter"        },
    { 'n',       MOD_CTRL,   ACTION_FILTER_NEXT,  "next match"    },
    { 'N',       MOD_CTRL,   ACTION_FILTER_PREV,  "prev match"    },

    /* display */
    { '.',       MOD_NONE,   ACTION_TOGGLE_HIDDEN, "hidden"       },
    { 'i',       MOD_NONE,   ACTION_TOGGLE_DETAILS,"details"      },

    /* selection */
    { ' ',       MOD_NONE,   ACTION_SEL_TOGGLE,   "select"        },
    { 'v',       MOD_NONE,   ACTION_SEL_INVERT,   "invert sel"    },
    { 'V',       MOD_SHIFT,  ACTION_SEL_ALL,      "select all"    },
    { 0x0f,      MOD_CTRL,   ACTION_SEL_NONE,     "clear sel"     }, /* ^O */

    /* other */
    { 'o',       MOD_NONE,   ACTION_OPEN_WITH,    "open with..."  },
    { '~',       MOD_SHIFT,  ACTION_CHDIR,        "chdir"         },
    { 'q',       MOD_NONE,   ACTION_QUIT,         "quit"          },
    { 0x03,      MOD_CTRL,   ACTION_QUIT,         "quit"          }, /* ^C */
    { 0x0c,      MOD_CTRL,   ACTION_REFRESH,      "refresh"       }, /* ^L */
};
