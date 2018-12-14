#include "wmhack.h"
#include <poll.h>
#include <X11/Xatom.h>
#include <X11/keysymdef.h>
#include <sys/time.h>
#include <string>
#include <string.h>
#include <map>
#include <set>

static int intarg() { return atoi(optarg); } // XXX: use strtol and invoke usage()
static bool nodo = false;
static unsigned int border = 2;
static bool glide = true;

constexpr int MAXIDLE = 3000;

extern char readme_txt[];
static void
usage(std::ostream &stream)
{
    stream << readme_txt;
    exit(1);
}

static void
adjustForStruts(const X11Env &x11, Geometry *g, long targetDesktop)
{
    Atom actualType;
    int actualFormat;
    unsigned long itemCount;
    unsigned long afterBytes;
    unsigned char *winlist;
    // get a list of all clients, so we can adjust monitor sizes for extents.
    int rc = XGetWindowProperty(x11, x11.root, x11.NetClientList,
            0, std::numeric_limits<long>::max(), False, x11.AWindow,
            &actualType, &actualFormat, &itemCount, &afterBytes, &winlist);
    if (rc != 0 || actualFormat != 32 || itemCount <= 0 ) {
        std::cerr << "can't list clients to do strut processing" << std::endl;
        return;
    }

    Window *w = (Window *)winlist;
    for (size_t i = itemCount; i-- > 0;) {
        auto clipDesktop = x11.desktopForWindow(w[i]);
        unsigned char *prop;
        if (clipDesktop == targetDesktop || clipDesktop == -1 || targetDesktop == -1) {
            rc = XGetWindowProperty(x11, w[i], x11.NetWmStrutPartial,
                0, std::numeric_limits<long>::max(), False, x11.Cardinal,
                &actualType, &actualFormat, &itemCount, &afterBytes, &prop);
            if (rc == 0) {
                if (itemCount == 12 && actualFormat == 32) {
                    PartialStrut *strut = (PartialStrut *)prop;
                    strut->box(x11, *g);
                }
                XFree(prop);
            } else {
                unsigned char *prop;
                rc = XGetWindowProperty(x11, w[i], x11.NetWmStrut,
                    0, std::numeric_limits<long>::max(), False, x11.Cardinal,
                    &actualType, &actualFormat, &itemCount, &afterBytes, &prop);
                if (rc == 0) {
                    std::clog << "TODO: deal with legacy strut\n";
                    XFree(prop);
                }
            }
        }
    }
    XFree(winlist);
}

static void
setOpacityRaw(const X11Env &x11, Window w, unsigned long opacity)
{
    XChangeProperty(x11, w, x11.NetWmOpacity, XA_CARDINAL, 32, PropModeReplace,
          (unsigned char *)&opacity, 1);
    XFlush(x11);
}

static void
setOpacity(const X11Env &x11, Window w, double opacity)
{
    setOpacityRaw(x11, w, opacity * std::numeric_limits<uint32_t>::max());
}

static double
getOpacity(const X11Env &x11, Window w)
{

    Atom actualType;
    int actualFormat;
    unsigned char *property;
    unsigned long items, size;

    int rc = XGetWindowProperty(x11, w, x11.NetWmOpacity, 0, 1, False, XA_CARDINAL,
            &actualType, &actualFormat, &items, &size, &property);
    unsigned long propertyValue = rc == 0 && items == 1 ? *(unsigned long *)property : std::numeric_limits<uint32_t>::max();
    propertyValue &= 0xffffffff; // XXX: Seems to get sign extended.
    return double(propertyValue) / std::numeric_limits<uint32_t>::max();
}


static void
setOpacityDelta(const X11Env &x11, Window w, double opacity)
{
    setOpacity(x11, w, std::max(0.0, std::min(1.0, getOpacity(x11, w) + opacity)));
}


static void
setWorkdir(const X11Env &x11, Window w, const char *value)
{
    XChangeProperty(x11, w, x11.WorkDir, XA_STRING, 8, PropModeReplace,
               (const unsigned char *)value, strlen(value));
    XFlush(x11);
}

void
resizeWindow(X11Env &x11,
      long desktop, Geometry &geom,
      Window win,
      unsigned *border,
      const long *frame,
      const char *location)
{
    char curChar;

    for (const char *path = location; (curChar = *path) != 0; ++path) {
        double num = 1, denom = 2;
        if (isdigit(curChar) || curChar == '.') {
            char *newpath;
            num = 1;
            denom = strtod(path, &newpath);
            path = newpath;
            curChar = *path;
            if (curChar == '/') {
               num = denom;
               denom = strtod(path + 1, &newpath);
               path = newpath;
               curChar = *path;
            }
        }

        switch (curChar) {
            case 'b':
                *border = denom;
                break;
            case 'r':
                // move to right
                geom.x += geom.size.width - geom.size.width * (num / denom);
                // and then...
            case 'l':
                // cut out right hand side.
                geom.size.width = geom.size.width * num / denom;
                break;
            case 'd':
                geom.y += geom.size.height - geom.size.height * (num / denom);
                // and then...
            case 'u':
                geom.size.height = geom.size.height * (num / denom);
                break;
            case 'h': {
                // reduce horizontal size and centre
                int newsize = geom.size.width * (num / denom);
                geom.x += (geom.size.width - newsize) / 2;
                geom.size.width = newsize;
                break;
            }
            case 'v': {
                // reduce vertical size and centre
                int newsize = geom.size.height * (num / denom);
                geom.y += (geom.size.height - newsize) / 2;
                geom.size.height = newsize;
                break;
            }
            default:
                usage(std::cerr);
        }
    }
    adjustForStruts(x11, &geom, desktop);

    // Adjust the geometry downwards to account for the frame around the window, and our border.
    geom.size.width -= frame[0] + frame[1] + *border * 2;
    geom.size.height -= frame[2] + frame[3] + *border * 2;
    geom.x += frame[0] + *border;
    geom.y += frame[2] + *border;

    Geometry oldgeom = x11.getGeometry(win);
    int duration = 150000;
    int sleeptime = 500000 / 60;
    int iters = duration / sleeptime;
    if (glide) {
#define update(f) next.f = (oldgeom.f * (iters - i) + geom.f * i) / iters
        for (auto i = 1;; ++i) {
              Geometry next;
              update(size.width);
              update(size.height);
              update(x);
              update(y);
              x11.setGeometry(win, next);
              if (i == iters)
                 break;
              usleep(sleeptime);
        }
#undef update 
    } else {
        x11.setGeometry(win, geom);
    }
}


long
msecDiff(const timeval &l, const timeval &r)
{
   suseconds_t usec = l.tv_usec - r.tv_usec;
   time_t sec = l.tv_sec - r.tv_sec;
   if (l.tv_usec < r.tv_usec) {
      usec += 1000000;
      sec -= 1;
   }
   return usec / 1000 + sec * 1000;
}


int
catchmain(int argc, char *argv[])
{
    int screen = -1, c;
    int verbose = 0;
    bool doPick = false;
    bool interactive = false;
    double opacity = -1;
    double opacityDelta = 0;
    bool windowRelative = false;
    Window win = 0;
    const char *workdir = 0;
    std::set<Atom> toggles;

    Display *display = XOpenDisplay(0);
    if (display == 0) {
        std::clog << "failed to open display: set DISPLAY environment variable" << std::endl;
        return 1;
    }
    X11Env x11(display);

    if (argc == 1)
        usage(std::cerr);
    while ((c = getopt(argc, argv, "o:s:w:W:abfghimnpuvx_O:")) != -1) {
        switch (c) {
            case 'p':
                doPick = true;
                break;
            case 's':
                screen = intarg();
                break;
            case 'n':
                nodo = true;
                break;
            case 'f':
                toggles.insert(x11.NetWmStateFullscreen);
                break;
            case 'm':
                toggles.insert(x11.NetWmStateMaximizedHoriz);
                break;
            case 'h':
                usage(std::cout);
                break;
            case '_':
                toggles.insert(x11.NetWmStateShaded);
                break;
            case 'a':
                toggles.insert(x11.NetWmStateAbove);
                break;
            case 'u':
                toggles.insert(x11.NetWmStateBelow);
                break;

            case 'o':
               opacity = strtod(optarg, 0);
               if (opacity < 0.0 || opacity > 1)
                  usage(std::cerr);
               break;

            case 'O':
               opacityDelta = strtod(optarg, 0);
               break;

            case 'w':
               win = intarg();
               break;
            case 'x':
               windowRelative = true;
               border = 0; // assume the window already has adequate space around it.
               break;
            case 'W':
               workdir = optarg;
               break;
            case 'v':
               verbose++;
               break;
            case 'i':
               interactive = true;
               break;
            case 'g':
               glide = !glide;
               break;
            default:
               usage(std::cerr);
               break;
        }
    }

    // Which window are we modifying?
    if (win == 0)
       win = doPick ? x11.pick() : x11.active();
    if (win == 0) {
        std::cerr << "no window selected\n";
        return 0;
    }

    // If we're doing state toggles/misc changes to window, do it now.
    if (opacity >= 0.0)
        setOpacity(x11, win, opacity);
    if (opacityDelta != 0.0)
        setOpacityDelta(x11, win, opacityDelta);
    if (workdir != 0)
        setWorkdir(x11, win, workdir);
    for (auto atom : toggles)
        x11.updateState(win, atom, X11Env::TOGGLE);

    // If nothing else to do, just exit.
    if (argc == optind && !interactive)
        return 0;

    if (screen == -1)
        screen = x11.monitorForWindow(win);

    /*
     * get the extent of the frame around the window: we assume the new frame
     * will have the same extents when we resize it, and use that to adjust the
     * position of the client window so its frame abuts the edge of the screen.
     */
    Atom actualType;
    int actualFormat;
    unsigned long itemCount;
    unsigned long afterBytes;
    unsigned char *prop;
    long desktop;
    int rc;
    const long *frame;
    rc = XGetWindowProperty(x11, win, x11.NetFrameExtents,
            0, std::numeric_limits<long>::max(), False, x11.Cardinal,
            &actualType, &actualFormat, &itemCount, &afterBytes, &prop);
    bool haveFrame = rc == 0;
    if (rc != 0 || actualFormat != 32 || itemCount != 4) {
        std::cerr << "can't find frame sizes" << std::endl;
        static long defaultFrame[] = { 0, 0, 0, 0 };
        frame = defaultFrame;
    } else {
        frame = (long *)prop;
    }

    // Work out starting geometry - either existing size, or entire window
    Geometry window;
    if (windowRelative) {
       window = x11.getGeometry(win);
       if (rc == 0)
           XFree(prop);
    } else {
       window = x11.monitors[screen];
    }

    /*
     * Find desktop of the window in question - we ignore windows on other
     * desktops for struts avoidance, etc.
     */
    desktop = x11.desktopForWindow(win);
    // Remove any toggles that make the window size moot.
    x11.updateState(win, x11.NetWmStateShaded, X11Env::REMOVE);
    x11.updateState(win, x11.NetWmStateMaximizedHoriz, X11Env::REMOVE);
    x11.updateState(win, x11.NetWmStateFullscreen, X11Env::REMOVE);

    if (interactive) {
        auto keyWin = XCreateSimpleWindow(x11, x11.root, 0, 0, 1, 1, 0, 0, 0);
        if (keyWin == 0)
           abort();

        XMapWindow(x11, keyWin);
        XFlush(x11);
        XSelectInput(x11, keyWin, ExposureMask | KeyPressMask);

        int symsPerKey, minCodes, maxCodes;
        auto codes = XDisplayKeycodes(x11, &minCodes, &maxCodes);
        if (!codes)
           abort();
        auto keySyms = XGetKeyboardMapping(x11, minCodes, maxCodes - minCodes, &symsPerKey);

        static std::map<int, const char *> keyToOperation = {

           { XK_Up, "u" },
           { XK_Down, "d" },
           { XK_Left, "l" },
           { XK_Right, "r" },

           { XK_KP_8, "u" },
           { XK_KP_2, "d" },
           { XK_KP_4, "l" },
           { XK_KP_6, "r" },

           { XK_KP_7, "ul" },
           { XK_KP_9, "ur" },
           { XK_KP_1, "dl" },
           { XK_KP_3, "dr" },

           { XK_KP_Up, "u" },
           { XK_KP_Down, "d" },
           { XK_KP_Left, "l" },
           { XK_KP_Right, "r" },

           { XK_KP_Home, "ul" },
           { XK_KP_Page_Up, "ur" },
           { XK_KP_End, "dl" },
           { XK_KP_Page_Down, "dr" }
        };


        int fd = ConnectionNumber(x11.display);
        struct pollfd pfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pfd.fd = fd;
 
        struct timeval lastKey;
        gettimeofday(&lastKey, 0);
        for (bool done = false; !done;) {
           struct timeval now;
            gettimeofday(&now, 0);
            long wait = MAXIDLE - msecDiff(now, lastKey);
            poll(&pfd, 1, wait);
            gettimeofday(&now, 0);
            if (msecDiff(now, lastKey) > MAXIDLE)
               exit(0);
            XEvent event;
            XNextEvent(x11, &event);
            switch (event.type) {
            case KeyPress:
                gettimeofday(&lastKey, 0);
                auto i = (event.xkey.keycode - minCodes) * symsPerKey;
                auto todo = keyToOperation.find(keySyms[i]);
                if (todo == keyToOperation.end())
                    exit(0);
                resizeWindow(x11, desktop, window, win, &border, frame, todo->second);
                break;
            }
        }
    } else {
        static std::map<std::string, const char *> aliases = {
            { "top",        "u" },
            { "bottom",     "d" },
            { "left",       "l" },
            { "right",      "r" },
            { "topleft",    "ul" },
            { "topright",   "ur" },
            { "bottomleft", "dl" },
            { "bottomright", "dr" },
        };
        const char *location = argv[optind];
        auto alias = aliases.find(location);
        if (alias != aliases.end())
            location = alias->second;
        resizeWindow(x11, desktop, window, win, &border, frame, location);
    }
    if (haveFrame)
       XFree((unsigned char *)frame);
    XCloseDisplay(display);
    return 0;
}

int
main(int argc, char *argv[])
{
   try {
      return catchmain(argc, argv);
   }
   catch (const char *msg) {
      std::clog << "internal error: " << msg << "\n";
      return 1;
   }
}
