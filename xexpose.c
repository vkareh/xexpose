#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xinerama.h>
#include <X11/Xft/Xft.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo-xlib-xrender.h>

#define PADDING      20
#define TITLE_HEIGHT 24
#define TAB_HEIGHT   36
#define BG_ALPHA     0x8000

#define ICON_SIZE   96

#define FOCUS_WINDOWS 0
#define FOCUS_TABS    1

typedef struct {
    int x, y, w, h;
} MonitorRect;

typedef struct {
    Window        xwin;
    Window        frame;
    Pixmap        pixmap;
    Pixmap        icon_pm;
    Picture       icon_pic;
    int           icon_w, icon_h;
    Visual       *visual;
    int           depth;
    int           x, y;
    unsigned      width, height;
    unsigned      pw, ph;
    unsigned long desktop;
    char         *title;
    char         *wm_class;
    int           urgent;
    int           cell_x, cell_y;
    int           thumb_w, thumb_h;
} WinInfo;

static Display *dpy;
static int      scr;
static Window   root;
static int      grid_cols;

static volatile sig_atomic_t got_signal;

static void
signal_handler(int sig)
{
    (void)sig;
    got_signal = 1;
}

static Atom atom_client_list;
static Atom atom_active_window;
static Atom atom_close_window;
static Atom atom_wm_desktop;
static Atom atom_cur_desktop;
static Atom atom_num_desktops;
static Atom atom_desktop_names;
static Atom atom_desktop_layout;
static Atom atom_wm_type;
static Atom atom_type_dock;
static Atom atom_type_desktop;
static Atom atom_wm_state;
static Atom atom_state_hidden;
static Atom atom_state_skip_pager;
static Atom atom_state_attention;
static Atom atom_frame_extents;
static Atom atom_wm_name;
static Atom atom_wm_icon;
static Atom atom_utf8_string;

static void
intern_atoms(void)
{
    atom_client_list      = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    atom_active_window    = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_close_window     = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
    atom_wm_desktop       = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    atom_cur_desktop      = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    atom_num_desktops     = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_desktop_names    = XInternAtom(dpy, "_NET_DESKTOP_NAMES", False);
    atom_desktop_layout   = XInternAtom(dpy, "_NET_DESKTOP_LAYOUT", False);
    atom_wm_type          = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_type_dock        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    atom_type_desktop     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    atom_wm_state         = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_state_hidden     = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    atom_state_skip_pager = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    atom_state_attention  = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    atom_frame_extents    = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    atom_wm_name          = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_wm_icon          = XInternAtom(dpy, "_NET_WM_ICON", False);
    atom_utf8_string      = XInternAtom(dpy, "UTF8_STRING", False);
}

typedef struct {
    XRenderColor foreground;
    XRenderColor background;
    XRenderColor border;
    XRenderColor highlight;
    XRenderColor sticky;
    XRenderColor urgent;
    char         font[256];
} Config;

static int
parse_hex_color(const char *str, XRenderColor *out)
{
    unsigned int r, g, b;
    if (str[0] == '#' && strlen(str) == 7 &&
        sscanf(str, "#%02x%02x%02x", &r, &g, &b) == 3) {
        out->red   = (unsigned short)(r << 8 | r);
        out->green = (unsigned short)(g << 8 | g);
        out->blue  = (unsigned short)(b << 8 | b);
        out->alpha = 0xFFFF;
        return 1;
    }
    return 0;
}

static void
load_config(Config *cfg)
{
    /* Tango defaults */
    parse_hex_color("#eeeeec", &cfg->foreground);
    parse_hex_color("#2e3436", &cfg->background);
    parse_hex_color("#555753", &cfg->border);
    parse_hex_color("#eeeeec", &cfg->highlight);
    parse_hex_color("#edd400", &cfg->sticky);
    parse_hex_color("#ef2929", &cfg->urgent);
    strncpy(cfg->font, "sans-10", sizeof(cfg->font) - 1);

    XrmInitialize();
    char *res_str = XResourceManagerString(dpy);
    if (!res_str) return;

    XrmDatabase db = XrmGetStringDatabase(res_str);
    if (!db) return;

    char *type;
    XrmValue val;

    if (XrmGetResource(db, "xexpose.foreground", "Xexpose.Foreground", &type, &val))
        parse_hex_color(val.addr, &cfg->foreground);
    if (XrmGetResource(db, "xexpose.background", "Xexpose.Background", &type, &val))
        parse_hex_color(val.addr, &cfg->background);
    if (XrmGetResource(db, "xexpose.borderColor", "Xexpose.BorderColor", &type, &val))
        parse_hex_color(val.addr, &cfg->border);
    if (XrmGetResource(db, "xexpose.highlightColor", "Xexpose.HighlightColor", &type, &val))
        parse_hex_color(val.addr, &cfg->highlight);
    if (XrmGetResource(db, "xexpose.stickyColor", "Xexpose.StickyColor", &type, &val))
        parse_hex_color(val.addr, &cfg->sticky);
    if (XrmGetResource(db, "xexpose.urgentColor", "Xexpose.UrgentColor", &type, &val))
        parse_hex_color(val.addr, &cfg->urgent);
    if (XrmGetResource(db, "xexpose.font", "Xexpose.Font", &type, &val))
        strncpy(cfg->font, val.addr, sizeof(cfg->font) - 1);

    XrmDestroyDatabase(db);
}

static unsigned long
get_cardinal(Window win, Atom prop)
{
    Atom            type;
    int             fmt;
    unsigned long   nitems, bytes;
    unsigned char  *data = NULL;
    unsigned long   val = 0;

    if (XGetWindowProperty(dpy, win, prop, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &nitems, &bytes, &data) == Success
        && data) {
        if (nitems > 0)
            val = *(unsigned long *)data;
        XFree(data);
    }
    return val;
}

static int
has_atom_in_list(Window win, Atom prop, Atom target)
{
    Atom            type;
    int             fmt;
    unsigned long   nitems, bytes;
    unsigned char  *data = NULL;

    if (XGetWindowProperty(dpy, win, prop, 0, 64, False, XA_ATOM,
                           &type, &fmt, &nitems, &bytes, &data) == Success
        && data) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < nitems; i++) {
            if (atoms[i] == target) {
                XFree(data);
                return 1;
            }
        }
        XFree(data);
    }
    return 0;
}

static char *
get_window_title(Window win)
{
    Atom            type;
    int             fmt;
    unsigned long   nitems, bytes;
    unsigned char  *data = NULL;

    if (XGetWindowProperty(dpy, win, atom_wm_name, 0, 256, False,
                           atom_utf8_string, &type, &fmt, &nitems, &bytes,
                           &data) == Success && data && nitems > 0) {
        char *title = strdup((char *)data);
        XFree(data);
        return title;
    }
    if (data) XFree(data);

    XTextProperty tp;
    if (XGetWMName(dpy, win, &tp) && tp.value) {
        char *title = strdup((char *)tp.value);
        XFree(tp.value);
        return title;
    }
    return strdup("(untitled)");
}

static char *
get_window_class(Window win)
{
    XClassHint hint;
    if (XGetClassHint(dpy, win, &hint)) {
        char *cls = strdup(hint.res_class ? hint.res_class : "");
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        return cls;
    }
    return strdup("");
}

static void
load_window_icon(WinInfo *wi)
{
    wi->icon_pm  = None;
    wi->icon_pic = None;
    wi->icon_w   = 0;
    wi->icon_h   = 0;

    Atom type;
    int fmt;
    unsigned long nitems, bytes;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, wi->xwin, atom_wm_icon, 0, 0x7FFFFFFF, False,
                           XA_CARDINAL, &type, &fmt, &nitems, &bytes,
                           &data) != Success || !data || nitems == 0) {
        if (data) XFree(data);
        return;
    }

    unsigned long *icon_data = (unsigned long *)data;
    unsigned long *best = NULL;
    unsigned long best_w = 0, best_h = 0;

    unsigned long *p = icon_data;
    unsigned long *end = icon_data + nitems;
    while (p + 2 <= end) {
        unsigned long w = p[0];
        unsigned long h = p[1];
        if (w == 0 || h == 0 || w > 1024 || h > 1024 || p + 2 + w * h > end)
            break;
        if (w * h > best_w * best_h) {
            best_w = w;
            best_h = h;
            best = p + 2;
        }
        p += 2 + w * h;
    }

    if (!best) {
        XFree(data);
        return;
    }

    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                      best_w, best_h);
    cairo_surface_flush(img);
    uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img) / 4;

    for (unsigned long y = 0; y < best_h; y++) {
        for (unsigned long x = 0; x < best_w; x++) {
            uint32_t argb = (uint32_t)best[y * best_w + x];
            uint8_t a = (argb >> 24) & 0xFF;
            uint8_t r = ((argb >> 16) & 0xFF) * a / 255;
            uint8_t g = ((argb >>  8) & 0xFF) * a / 255;
            uint8_t b = ( argb        & 0xFF) * a / 255;
            pixels[y * stride + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16)
                                   | ((uint32_t)g << 8) | b;
        }
    }

    cairo_surface_mark_dirty(img);

    int out_w = ICON_SIZE, out_h = ICON_SIZE;
    if ((int)best_w < out_w) out_w = (int)best_w;
    if ((int)best_h < out_h) out_h = (int)best_h;

    XRenderPictFormat *argb_fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    wi->icon_pm = XCreatePixmap(dpy, root, out_w, out_h, 32);
    wi->icon_pic = XRenderCreatePicture(dpy, wi->icon_pm, argb_fmt, 0, NULL);

    cairo_surface_t *xsurf = cairo_xlib_surface_create_with_xrender_format(
        dpy, wi->icon_pm, DefaultScreenOfDisplay(dpy), argb_fmt, out_w, out_h);
    cairo_t *cr = cairo_create(xsurf);

    double sx = (double)out_w / best_w;
    double sy = (double)out_h / best_h;
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(xsurf);
    cairo_surface_destroy(img);

    wi->icon_w = out_w;
    wi->icon_h = out_h;

    XFree(data);
}

static int
get_window_list(WinInfo **out, int *out_count)
{
    Atom            type;
    int             fmt;
    unsigned long   nitems, bytes;
    unsigned char  *data = NULL;

    if (XGetWindowProperty(dpy, root, atom_client_list, 0, 4096, False,
                           XA_WINDOW, &type, &fmt, &nitems, &bytes,
                           &data) != Success || !data) {
        return -1;
    }

    Window *wins = (Window *)data;

    WinInfo *list = calloc(nitems, sizeof(WinInfo));
    int count = 0;

    for (unsigned long i = 0; i < nitems; i++) {
        Window w = wins[i];

        if (has_atom_in_list(w, atom_wm_type, atom_type_dock))
            continue;
        if (has_atom_in_list(w, atom_wm_type, atom_type_desktop))
            continue;
        if (has_atom_in_list(w, atom_wm_state, atom_state_hidden))
            continue;
        if (has_atom_in_list(w, atom_wm_state, atom_state_skip_pager))
            continue;

        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa))
            continue;

        unsigned long desk = get_cardinal(w, atom_wm_desktop);

        Window parent, qroot;
        Window *children;
        unsigned int nchildren;
        Window frame = w;
        if (XQueryTree(dpy, w, &qroot, &parent, &children, &nchildren)) {
            if (children) XFree(children);
            if (parent != root)
                frame = parent;
        }

        list[count].xwin    = w;
        list[count].frame   = frame;
        list[count].desktop = desk;
        list[count].title    = get_window_title(w);
        list[count].wm_class = get_window_class(w);
        list[count].urgent  = has_atom_in_list(w, atom_wm_state, atom_state_attention);
        list[count].pixmap  = None;
        list[count].x       = wa.x;
        list[count].y       = wa.y;
        if (frame != w) {
            XWindowAttributes fwa;
            if (XGetWindowAttributes(dpy, frame, &fwa)) {
                list[count].visual = fwa.visual;
                list[count].depth  = fwa.depth;
            } else {
                list[count].visual = wa.visual;
                list[count].depth  = wa.depth;
            }
        } else {
            list[count].visual = wa.visual;
            list[count].depth  = wa.depth;
        }

        Atom fe_type;
        int fe_fmt;
        unsigned long fe_nitems, fe_bytes;
        unsigned char *fe_data = NULL;
        if (frame != w
            && XGetWindowProperty(dpy, w, atom_frame_extents, 0, 4, False,
                                  XA_CARDINAL, &fe_type, &fe_fmt, &fe_nitems,
                                  &fe_bytes, &fe_data) == Success
            && fe_data && fe_nitems >= 4) {
            unsigned long *ext = (unsigned long *)fe_data;
            list[count].width  = wa.width  + ext[0] + ext[1];
            list[count].height = wa.height + ext[2] + ext[3];
            list[count].pw     = wa.x - (int)ext[0];
            list[count].ph     = wa.y - (int)ext[2];
        } else {
            list[count].width  = wa.width;
            list[count].height = wa.height;
            list[count].pw     = 0;
            list[count].ph     = 0;
        }
        if (fe_data) XFree(fe_data);

        load_window_icon(&list[count]);
        count++;
    }
    XFree(data);

    *out = list;
    *out_count = count;
    return 0;
}

static int
strcasestr_match(const char *haystack, const char *needle)
{
    if (!needle[0]) return 1;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static int
build_visible(WinInfo *wins, int total, unsigned long tab, int *vis, int max,
              int show_all, const char *filter)
{
    int n = 0;
    for (int i = 0; i < total && n < max; i++) {
        if (!(show_all || wins[i].desktop == tab || wins[i].desktop == ~0UL))
            continue;
        if (filter[0] && !strcasestr_match(wins[i].title, filter)
                      && !strcasestr_match(wins[i].wm_class, filter))
            continue;
        vis[n++] = i;
    }
    return n;
}

static void
compute_grid_layout(WinInfo *wins, int *vis, int vis_count, MonitorRect mon,
                    int top_h, int bottom_h)
{
    if (vis_count == 0) { grid_cols = 1; return; }

    int cols = (int)ceil(sqrt((double)vis_count));
    int rows = (int)ceil((double)vis_count / cols);
    grid_cols = cols;

    int avail_h = mon.h - top_h - bottom_h;
    int cell_w = (mon.w - PADDING * (cols + 1)) / cols;
    int cell_h = (avail_h - PADDING * (rows + 1)) / rows - TITLE_HEIGHT;

    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];
        int col = vi % cols;
        int row = vi / cols;

        double scale_x = (double)cell_w / wins[i].width;
        double scale_y = (double)cell_h / wins[i].height;
        double scale   = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale > 1.0) scale = 1.0;

        int thumb_w = (int)(wins[i].width  * scale);
        int thumb_h = (int)(wins[i].height * scale);

        int cx = mon.x + PADDING + col * (cell_w + PADDING) + (cell_w - thumb_w) / 2;
        int cy = mon.y + top_h + PADDING + row * (cell_h + TITLE_HEIGHT + PADDING) + (cell_h - thumb_h) / 2;

        wins[i].cell_x  = cx;
        wins[i].cell_y  = cy;
        wins[i].thumb_w = thumb_w;
        wins[i].thumb_h = thumb_h;
    }
}

static int x_error_occurred;

static int
error_handler(Display *d, XErrorEvent *ev)
{
    (void)d;
    (void)ev;
    x_error_occurred = 1;
    return 0;
}

static void
grab_visible_pixmaps(WinInfo *wins, int *vis, int vis_count)
{
    XErrorHandler old = XSetErrorHandler(error_handler);

    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];
        if (wins[i].pixmap != None)
            continue;

        Window target = wins[i].frame;

        x_error_occurred = 0;
        XCompositeRedirectWindow(dpy, target, CompositeRedirectAutomatic);
        XSync(dpy, False);
        if (x_error_occurred)
            continue;

        x_error_occurred = 0;
        wins[i].pixmap = XCompositeNameWindowPixmap(dpy, target);
        XSync(dpy, False);
        if (x_error_occurred)
            wins[i].pixmap = None;
    }

    XSetErrorHandler(old);
}

static void
ungrab_visible_pixmaps(WinInfo *wins, int *vis, int vis_count)
{
    XErrorHandler old = XSetErrorHandler(error_handler);

    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];
        if (wins[i].pixmap != None) {
            XFreePixmap(dpy, wins[i].pixmap);
            wins[i].pixmap = None;
        }
        x_error_occurred = 0;
        XCompositeUnredirectWindow(dpy, wins[i].frame, CompositeRedirectAutomatic);
        XSync(dpy, False);
    }

    XSetErrorHandler(old);
}

static void
activate_window(Window win, unsigned long desktop, Time timestamp)
{
    unsigned long cur = get_cardinal(root, atom_cur_desktop);
    if (desktop != cur && desktop != ~0UL) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type         = ClientMessage;
        ev.xclient.window       = root;
        ev.xclient.message_type = atom_cur_desktop;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = (long)desktop;
        ev.xclient.data.l[1]    = (long)timestamp;
        XSendEvent(dpy, root, False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    }

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = win;
    ev.xclient.message_type = atom_active_window;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 2;
    ev.xclient.data.l[1]    = (long)timestamp;
    ev.xclient.data.l[2]    = 0;

    XSendEvent(dpy, root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
}

static void
close_window(Window win, Time timestamp)
{
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = win;
    ev.xclient.message_type = atom_close_window;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = (long)timestamp;
    ev.xclient.data.l[1]    = 2;

    XSendEvent(dpy, root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
}

static void
switch_desktop(int desktop)
{
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = root;
    ev.xclient.message_type = atom_cur_desktop;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = desktop;
    ev.xclient.data.l[1]    = CurrentTime;
    XSendEvent(dpy, root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
    XSync(dpy, False);
}

static void
wait_for_map(void)
{
    usleep(50000);
    XSync(dpy, False);
}

static Pixmap
get_root_pixmap(void)
{
    Atom prop = XInternAtom(dpy, "_XROOTPMAP_ID", True);
    if (prop == None)
        prop = XInternAtom(dpy, "ESETROOT_PMAP_ID", True);
    if (prop == None)
        return None;

    Atom type;
    int fmt;
    unsigned long nitems, bytes;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_PIXMAP,
                           &type, &fmt, &nitems, &bytes, &data) != Success
        || !data || nitems == 0) {
        if (data) XFree(data);
        return None;
    }

    Pixmap pm = *(Pixmap *)data;
    XFree(data);
    return pm;
}

static MonitorRect
get_focused_monitor(void)
{
    MonitorRect mon = { 0, 0, DisplayWidth(dpy, scr), DisplayHeight(dpy, scr) };

    int nmons = 0;
    XineramaScreenInfo *mons = XineramaQueryScreens(dpy, &nmons);
    if (!mons || nmons <= 1) {
        if (mons) XFree(mons);
        return mon;
    }

    int focus_x = 0, focus_y = 0;
    int found = 0;

    Atom type;
    int fmt;
    unsigned long nitems, bytes;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, atom_active_window, 0, 1, False,
                           XA_WINDOW, &type, &fmt, &nitems, &bytes,
                           &data) == Success && data && nitems > 0) {
        Window active = *(Window *)data;
        XFree(data);
        if (!has_atom_in_list(active, atom_wm_type, atom_type_desktop)) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, active, &wa)) {
                Window child;
                XTranslateCoordinates(dpy, active, root, 0, 0,
                                      &focus_x, &focus_y, &child);
                found = 1;
            }
        }
    } else {
        if (data) XFree(data);
    }

    if (!found) {
        Window qroot, qchild;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(dpy, root, &qroot, &qchild, &rx, &ry, &wx, &wy, &mask);
        focus_x = rx;
        focus_y = ry;
    }

    for (int i = 0; i < nmons; i++) {
        if (focus_x >= mons[i].x_org && focus_x < mons[i].x_org + mons[i].width &&
            focus_y >= mons[i].y_org && focus_y < mons[i].y_org + mons[i].height) {
            mon.x = mons[i].x_org;
            mon.y = mons[i].y_org;
            mon.w = mons[i].width;
            mon.h = mons[i].height;
            break;
        }
    }

    XFree(mons);
    return mon;
}

typedef struct {
    int cols, rows;
} DeskLayout;

static DeskLayout
get_desktop_layout(int num_desktops)
{
    DeskLayout layout = { num_desktops, 1 };

    Atom type;
    int fmt;
    unsigned long nitems, bytes;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, root, atom_desktop_layout, 0, 4, False,
                           XA_CARDINAL, &type, &fmt, &nitems, &bytes,
                           &data) == Success && data && nitems >= 4) {
        unsigned long *vals = (unsigned long *)data;
        int cols = (int)vals[1];
        int rows = (int)vals[2];

        if (cols > 0 && rows > 0) {
            layout.cols = cols;
            layout.rows = rows;
        } else if (cols > 0) {
            layout.cols = cols;
            layout.rows = (num_desktops + cols - 1) / cols;
        } else if (rows > 0) {
            layout.rows = rows;
            layout.cols = (num_desktops + rows - 1) / rows;
        }
    }
    if (data) XFree(data);

    return layout;
}

static char **
get_desktop_names(int num_desktops, int *out_count)
{
    char **names = calloc(num_desktops, sizeof(char *));

    Atom type;
    int fmt;
    unsigned long nitems, bytes;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, root, atom_desktop_names, 0, 4096, False,
                           atom_utf8_string, &type, &fmt, &nitems, &bytes,
                           &data) == Success && data && nitems > 0) {
        int idx = 0;
        char *p = (char *)data;
        char *end = p + nitems;
        while (p < end && idx < num_desktops) {
            names[idx++] = strdup(p);
            p += strlen(p) + 1;
        }
        XFree(data);
    } else {
        if (data) XFree(data);
    }

    for (int i = 0; i < num_desktops; i++) {
        if (!names[i]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", i + 1);
            names[i] = strdup(buf);
        }
    }

    *out_count = num_desktops;
    return names;
}

static void
render_thumbnails(Window overlay, WinInfo *wins, int *vis, int vis_count,
                  int scr_w, int scr_h, MonitorRect mon, XftFont *font,
                  XftFont *bold_font, int selected, int num_desktops,
                  char **desk_names, DeskLayout layout, int cur_tab,
                  int focus_mode, int tab_highlight, int show_all,
                  const char *filter, const Config *cfg)
{
    Visual *dvis = DefaultVisual(dpy, scr);
    int ddepth = DefaultDepth(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    XRenderPictFormat *fmt_overlay = XRenderFindVisualFormat(dpy, dvis);

    Pixmap back_pm = XCreatePixmap(dpy, overlay, scr_w, scr_h, ddepth);
    Picture back_pic = XRenderCreatePicture(dpy, back_pm, fmt_overlay, 0, NULL);

    Pixmap root_pm = get_root_pixmap();
    if (root_pm != None) {
        Window pm_root;
        int pm_x, pm_y;
        unsigned pm_w, pm_h, pm_bw, pm_depth;
        XGetGeometry(dpy, root_pm, &pm_root, &pm_x, &pm_y,
                     &pm_w, &pm_h, &pm_bw, &pm_depth);

        Picture root_pic = XRenderCreatePicture(dpy, root_pm, fmt_overlay, 0, NULL);

        if (pm_w != (unsigned)scr_w || pm_h != (unsigned)scr_h) {
            XTransform xform = {{
                { XDoubleToFixed((double)pm_w / scr_w), 0, 0 },
                { 0, XDoubleToFixed((double)pm_h / scr_h), 0 },
                { 0, 0, XDoubleToFixed(1.0) }
            }};
            XRenderSetPictureTransform(dpy, root_pic, &xform);
            XRenderSetPictureFilter(dpy, root_pic, FilterBilinear, NULL, 0);
        }

        XRenderComposite(dpy, PictOpSrc, root_pic, None, back_pic,
                         0, 0, 0, 0, 0, 0, scr_w, scr_h);
        XRenderFreePicture(dpy, root_pic);

        XRenderColor tint = { 0x0000, 0x0000, 0x0000, BG_ALPHA };
        XRenderFillRectangle(dpy, PictOpOver, back_pic, &tint,
                             0, 0, scr_w, scr_h);
    } else {
        XRenderColor bg = cfg->background;
        XRenderFillRectangle(dpy, PictOpSrc, back_pic, &bg,
                             0, 0, scr_w, scr_h);
    }

    XRenderColor border_color    = cfg->border;
    XRenderColor highlight_color = cfg->highlight;
    XRenderColor sticky_color    = cfg->sticky;
    XRenderColor urgent_color    = cfg->urgent;

    XErrorHandler old_handler = XSetErrorHandler(error_handler);

    XRenderColor placeholder_bg = cfg->border;

    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];

        int is_selected = (focus_mode == FOCUS_WINDOWS && vi == selected);
        int is_sticky = (wins[i].desktop == ~0UL);
        int is_urgent = wins[i].urgent;
        XRenderColor *bc = is_selected ? &highlight_color
                         : is_urgent   ? &urgent_color
                         : is_sticky   ? &sticky_color
                         :               &border_color;
        int bw = is_selected ? 4 : (is_urgent || is_sticky) ? 3 : 2;

        XRenderFillRectangle(dpy, PictOpOver, back_pic, bc,
                             wins[i].cell_x - bw, wins[i].cell_y - bw,
                             wins[i].thumb_w + bw * 2, wins[i].thumb_h + bw * 2);

        if (wins[i].pixmap == None) {
            /* Placeholder for unmapped windows (e.g. on other desktops) */
            XRenderFillRectangle(dpy, PictOpSrc, back_pic, &placeholder_bg,
                                 wins[i].cell_x, wins[i].cell_y,
                                 wins[i].thumb_w, wins[i].thumb_h);

            if (wins[i].icon_pic != None) {
                int iw = ICON_SIZE, ih = ICON_SIZE;
                if (iw > wins[i].thumb_w - 16) iw = wins[i].thumb_w - 16;
                if (ih > wins[i].thumb_h - 16) ih = wins[i].thumb_h - 16;
                if (iw < 16) iw = 16;
                if (ih < 16) ih = 16;

                if (wins[i].icon_w != iw || wins[i].icon_h != ih) {
                    XTransform ixform = {{
                        { XDoubleToFixed((double)wins[i].icon_w / iw), 0, 0 },
                        { 0, XDoubleToFixed((double)wins[i].icon_h / ih), 0 },
                        { 0, 0, XDoubleToFixed(1.0) }
                    }};
                    XRenderSetPictureTransform(dpy, wins[i].icon_pic, &ixform);
                    XRenderSetPictureFilter(dpy, wins[i].icon_pic, FilterBilinear, NULL, 0);
                }

                int ix = wins[i].cell_x + (wins[i].thumb_w - iw) / 2;
                int iy = wins[i].cell_y + (wins[i].thumb_h - ih) / 2;

                XRenderComposite(dpy, PictOpOver, wins[i].icon_pic, None, back_pic,
                                 0, 0, 0, 0, ix, iy, iw, ih);
            }

            if (!is_selected) {
                XRenderColor dim = { 0x0000, 0x0000, 0x0000, 0x3000 };
                XRenderFillRectangle(dpy, PictOpOver, back_pic, &dim,
                                     wins[i].cell_x, wins[i].cell_y,
                                     wins[i].thumb_w, wins[i].thumb_h);
            }
            continue;
        }

        XRenderPictFormat *fmt_win = XRenderFindVisualFormat(dpy,
            wins[i].visual);
        if (!fmt_win)
            fmt_win = XRenderFindStandardFormat(dpy, PictStandardRGB24);

        XRenderPictureAttributes pa;
        pa.subwindow_mode = IncludeInferiors;

        x_error_occurred = 0;
        Picture src = XRenderCreatePicture(dpy, wins[i].pixmap, fmt_win,
                                           CPSubwindowMode, &pa);
        XSync(dpy, False);
        if (x_error_occurred)
            continue;

        double sx = (double)wins[i].width  / wins[i].thumb_w;
        double sy = (double)wins[i].height / wins[i].thumb_h;
        XTransform xform = {{
            { XDoubleToFixed(sx), 0, XDoubleToFixed(wins[i].pw) },
            { 0, XDoubleToFixed(sy), XDoubleToFixed(wins[i].ph) },
            { 0, 0, XDoubleToFixed(1.0) }
        }};
        XRenderSetPictureTransform(dpy, src, &xform);
        XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);

        XRenderComposite(dpy, PictOpOver, src, None, back_pic,
                         0, 0, 0, 0,
                         wins[i].cell_x, wins[i].cell_y,
                         wins[i].thumb_w, wins[i].thumb_h);

        if (!is_selected) {
            XRenderColor dim = { 0x0000, 0x0000, 0x0000, 0x3000 };
            XRenderFillRectangle(dpy, PictOpOver, back_pic, &dim,
                                 wins[i].cell_x, wins[i].cell_y,
                                 wins[i].thumb_w, wins[i].thumb_h);
        }

        if (wins[i].icon_pic != None) {
            int iw = ICON_SIZE, ih = ICON_SIZE;
            if (iw > wins[i].thumb_w / 2) iw = wins[i].thumb_w / 2;
            if (ih > wins[i].thumb_h / 2) ih = wins[i].thumb_h / 2;

            if (wins[i].icon_w != iw || wins[i].icon_h != ih) {
                XTransform ixform = {{
                    { XDoubleToFixed((double)wins[i].icon_w / iw), 0, 0 },
                    { 0, XDoubleToFixed((double)wins[i].icon_h / ih), 0 },
                    { 0, 0, XDoubleToFixed(1.0) }
                }};
                XRenderSetPictureTransform(dpy, wins[i].icon_pic, &ixform);
                XRenderSetPictureFilter(dpy, wins[i].icon_pic, FilterBilinear, NULL, 0);
            }

            int ix = wins[i].cell_x + wins[i].thumb_w - iw - 4;
            int iy = wins[i].cell_y + wins[i].thumb_h - ih - 4;

            XRenderComposite(dpy, PictOpOver, wins[i].icon_pic, None, back_pic,
                             0, 0, 0, 0, ix, iy, iw, ih);
        }

        XRenderFreePicture(dpy, src);
    }

    XSetErrorHandler(old_handler);

    XftDraw *xftdraw = XftDrawCreate(dpy, back_pm, dvis, cmap);
    XftColor text_color;
    XRenderColor rc = cfg->foreground;
    XftColorAllocValue(dpy, dvis, cmap, &rc, &text_color);

    XftColor shadow_color;
    XRenderColor sc = { 0x0000, 0x0000, 0x0000, 0xCCCC };
    XftColorAllocValue(dpy, dvis, cmap, &sc, &shadow_color);

    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];
        int tx = wins[i].cell_x;
        int ty = wins[i].cell_y + wins[i].thumb_h + TITLE_HEIGHT;
        int max_w = wins[i].thumb_w;
        XftFont *title_font = wins[i].urgent ? bold_font : font;

        char *title = wins[i].title;
        int full_len = strlen(title);
        int len = full_len;
        int truncated = 0;

        XGlyphInfo extents;
        XftTextExtentsUtf8(dpy, title_font, (FcChar8 *)title, len, &extents);

        if (extents.xOff > max_w) {
            XGlyphInfo ellipsis_ext;
            XftTextExtentsUtf8(dpy, title_font, (FcChar8 *)"\xe2\x80\xa6", 3, &ellipsis_ext);
            int avail = max_w - ellipsis_ext.xOff;

            while (len > 1) {
                len--;
                XftTextExtentsUtf8(dpy, title_font, (FcChar8 *)title, len, &extents);
                if (extents.xOff <= avail) break;
            }
            truncated = 1;
        }

        XftDrawStringUtf8(xftdraw, &shadow_color, title_font,
                          tx + 1, ty + 1, (FcChar8 *)title, len);
        XftDrawStringUtf8(xftdraw, &text_color, title_font,
                          tx, ty, (FcChar8 *)title, len);
        if (truncated) {
            XftDrawStringUtf8(xftdraw, &shadow_color, title_font,
                              tx + extents.xOff + 1, ty + 1,
                              (FcChar8 *)"\xe2\x80\xa6", 3);
            XftDrawStringUtf8(xftdraw, &text_color, title_font,
                              tx + extents.xOff, ty,
                              (FcChar8 *)"\xe2\x80\xa6", 3);
        }
    }

    XftColorFree(dpy, dvis, cmap, &shadow_color);

    if (filter[0]) {
        XRenderColor filter_bg = cfg->background;
        XRenderFillRectangle(dpy, PictOpSrc, back_pic, &filter_bg,
                             mon.x, mon.y, mon.w, TITLE_HEIGHT + PADDING);

        char label[280];
        snprintf(label, sizeof(label), "Filter: %s", filter);
        int flen = strlen(label);
        int fx = mon.x + PADDING;
        int fy = mon.y + (TITLE_HEIGHT + PADDING + font->ascent - font->descent) / 2;
        XftDrawStringUtf8(xftdraw, &text_color, font,
                          fx, fy, (FcChar8 *)label, flen);
    }

    int tab_total_h = TAB_HEIGHT * layout.rows;
    int tab_w = mon.w / layout.cols;

    if (!show_all) {
        XftColor tab_text_dark;
        XRenderColor tdc = cfg->background;
        XftColorAllocValue(dpy, dvis, cmap, &tdc, &tab_text_dark);

        XRenderColor tab_bg      = cfg->background;
        XRenderColor tab_active  = cfg->border;
        XRenderColor tab_focused = cfg->highlight;

        for (int t = 0; t < num_desktops; t++) {
            int tc_col = t % layout.cols;
            int tc_row = t / layout.cols;
            int tx = mon.x + tc_col * tab_w;
            int ty = mon.y + mon.h - tab_total_h + tc_row * TAB_HEIGHT;

            XRenderColor *tc;
            if (focus_mode == FOCUS_TABS && t == tab_highlight)
                tc = &tab_focused;
            else if (t == cur_tab)
                tc = &tab_active;
            else
                tc = &tab_bg;

            XRenderFillRectangle(dpy, PictOpSrc, back_pic, tc,
                                 tx, ty, tab_w - 1, TAB_HEIGHT - 1);

            char *name = desk_names[t];
            int len = strlen(name);
            XGlyphInfo ext;
            XftTextExtentsUtf8(dpy, font, (FcChar8 *)name, len, &ext);
            int text_x = tx + (tab_w - ext.xOff) / 2;
            int text_y = ty + (TAB_HEIGHT + font->ascent - font->descent) / 2;
            int is_focused_tab = (focus_mode == FOCUS_TABS && t == tab_highlight);
            XftDrawStringUtf8(xftdraw, is_focused_tab ? &tab_text_dark : &text_color,
                              font, text_x, text_y, (FcChar8 *)name, len);
        }

        XftColorFree(dpy, dvis, cmap, &tab_text_dark);
    }

    XftDrawDestroy(xftdraw);
    XftColorFree(dpy, dvis, cmap, &text_color);

    GC gc = XCreateGC(dpy, overlay, 0, NULL);
    XCopyArea(dpy, back_pm, overlay, gc, 0, 0, scr_w, scr_h, 0, 0);
    XFreeGC(dpy, gc);

    XRenderFreePicture(dpy, back_pic);
    XFreePixmap(dpy, back_pm);
}

static int
find_window_at(WinInfo *wins, int *vis, int vis_count, int mx, int my)
{
    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];
        if (mx >= wins[i].cell_x && mx < wins[i].cell_x + wins[i].thumb_w &&
            my >= wins[i].cell_y && my < wins[i].cell_y + wins[i].thumb_h) {
            return vi;
        }
    }
    return -1;
}

static int
find_tab_at(int mx, int my, int num_desktops, DeskLayout layout, MonitorRect mon)
{
    int tab_total_h = TAB_HEIGHT * layout.rows;
    int tab_top = mon.y + mon.h - tab_total_h;
    if (my < tab_top || mx < mon.x || mx >= mon.x + mon.w)
        return -1;
    int tab_w = mon.w / layout.cols;
    int tc_col = (mx - mon.x) / tab_w;
    int tc_row = (my - tab_top) / TAB_HEIGHT;
    if (tc_col >= layout.cols) tc_col = layout.cols - 1;
    if (tc_row >= layout.rows) tc_row = layout.rows - 1;
    int t = tc_row * layout.cols + tc_col;
    if (t >= num_desktops) return -1;
    return t;
}

int
main(int argc, char **argv)
{
    int show_all = 0;
    int allow_close = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0)
            show_all = 1;
        else if (strcmp(argv[i], "--allow-close") == 0)
            allow_close = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: xexpose [-a|--all] [--allow-close]\n"
                   "\n"
                   "Lightweight window picker for X11.\n"
                   "\n"
                   "Options:\n"
                   "  -a, --all       Show windows from all workspaces\n"
                   "  --allow-close   Enable middle-click and Delete to close windows\n"
                   "  -h, --help      Show this help\n"
                   "  -v, --version   Show version\n"
                   "\n"
                   "Controls:\n"
                   "  Arrows          Navigate between thumbnails\n"
                   "  Tab/Shift+Tab   Cycle through windows\n"
                   "  Enter           Activate selected window\n"
                   "  Escape          Close picker\n"
                   "  PgUp/PgDn       Switch workspace\n"
                   "  Type            Filter windows by title or class\n"
                   "  Backspace       Remove last filter character\n"
                   "  Ctrl+Backspace  Clear filter\n"
                   "  Delete          Close selected window (--allow-close)\n"
                   "  Middle-click    Close clicked window (--allow-close)\n"
                   "\n"
                   "Appearance can be customized via X resources:\n"
                   "  xexpose.foreground, xexpose.background, xexpose.borderColor,\n"
                   "  xexpose.highlightColor, xexpose.stickyColor, xexpose.urgentColor,\n"
                   "  xexpose.font\n");
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("xexpose 1.0\n");
            return 0;
        } else {
            fprintf(stderr, "xexpose: unknown option '%s'\n"
                            "Try 'xexpose --help' for more information.\n", argv[i]);
            return 1;
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xexpose: cannot open display\n");
        return 1;
    }

    scr  = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);

    int comp_major, comp_minor;
    if (!XCompositeQueryVersion(dpy, &comp_major, &comp_minor)
        || (comp_major == 0 && comp_minor < 3)) {
        fprintf(stderr, "xexpose: XComposite >= 0.3 required\n");
        XCloseDisplay(dpy);
        return 1;
    }

    int render_ev, render_err;
    if (!XRenderQueryExtension(dpy, &render_ev, &render_err)) {
        fprintf(stderr, "xexpose: XRender extension required\n");
        XCloseDisplay(dpy);
        return 1;
    }

    intern_atoms();

    Config cfg;
    load_config(&cfg);

    int num_desktops = (int)get_cardinal(root, atom_num_desktops);
    if (num_desktops < 1) num_desktops = 1;

    int name_count;
    char **desk_names = get_desktop_names(num_desktops, &name_count);
    DeskLayout desk_layout = get_desktop_layout(num_desktops);

    unsigned long cur_desktop = get_cardinal(root, atom_cur_desktop);
    int tab_total_h = show_all ? 0 : TAB_HEIGHT * desk_layout.rows;
    int cur_tab = (int)cur_desktop;

    WinInfo *wins = NULL;
    int total = 0;
    if (get_window_list(&wins, &total) < 0 || total == 0) {
        free(wins);
        for (int i = 0; i < num_desktops; i++) free(desk_names[i]);
        free(desk_names);
        XCloseDisplay(dpy);
        return 0;
    }

    int scr_w = DisplayWidth(dpy, scr);
    int scr_h = DisplayHeight(dpy, scr);
    MonitorRect mon = get_focused_monitor();

    int *vis = calloc(total, sizeof(int));
    char filter[256] = {0};
    int filter_len = 0;
#define FILTER_HEIGHT (filter[0] ? (TITLE_HEIGHT + PADDING) : 0)

    int vis_count = build_visible(wins, total, cur_tab, vis, total, show_all, filter);

    if (vis_count == 1 && (show_all || num_desktops == 1)) {
        activate_window(wins[vis[0]].xwin, wins[vis[0]].desktop, CurrentTime);
        for (int i = 0; i < total; i++) {
            free(wins[i].title);
            free(wins[i].wm_class);
            if (wins[i].icon_pic != None) XRenderFreePicture(dpy, wins[i].icon_pic);
            if (wins[i].icon_pm != None) XFreePixmap(dpy, wins[i].icon_pm);
        }
        free(wins);
        free(vis);
        for (int i = 0; i < num_desktops; i++) free(desk_names[i]);
        free(desk_names);
        XCloseDisplay(dpy);
        return 0;
    }

    compute_grid_layout(wins, vis, vis_count, mon, FILTER_HEIGHT, tab_total_h);
    grab_visible_pixmaps(wins, vis, vis_count);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.event_mask = ExposureMask | ButtonPressMask | KeyPressMask | KeyReleaseMask | PointerMotionMask | FocusChangeMask;
    swa.background_pixel = BlackPixel(dpy, scr);

    Window overlay = XCreateWindow(dpy, root,
        0, 0, scr_w, scr_h, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &swa);

    XMapRaised(dpy, overlay);

    int exit_code = 0;

    if (XGrabKeyboard(dpy, overlay, True, GrabModeAsync, GrabModeAsync,
                      CurrentTime) != GrabSuccess) {
        fprintf(stderr, "xexpose: cannot grab keyboard\n");
        ungrab_visible_pixmaps(wins, vis, vis_count);
        XDestroyWindow(dpy, overlay);
        exit_code = 1;
        goto cleanup;
    }
    if (XGrabPointer(dpy, overlay, True,
                     ButtonPressMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync,
                     overlay, None, CurrentTime) != GrabSuccess) {
        fprintf(stderr, "xexpose: cannot grab pointer\n");
        XUngrabKeyboard(dpy, CurrentTime);
        ungrab_visible_pixmaps(wins, vis, vis_count);
        XDestroyWindow(dpy, overlay);
        exit_code = 1;
        goto cleanup;
    }

    XftFont *font = XftFontOpenName(dpy, scr, cfg.font);
    if (!font)
        font = XftFontOpenName(dpy, scr, "sans-10");
    if (!font)
        font = XftFontOpenName(dpy, scr, "fixed");
    if (!font) {
        fprintf(stderr, "xexpose: cannot open any font\n");
        XDestroyWindow(dpy, overlay);
        exit_code = 1;
        goto cleanup;
    }

    char bold_font_name[270];
    snprintf(bold_font_name, sizeof(bold_font_name), "%s:bold", cfg.font);
    XftFont *bold_font = XftFontOpenName(dpy, scr, bold_font_name);
    if (!bold_font)
        bold_font = font;

    int selected = 0;
    {
        Atom type;
        int fmt;
        unsigned long nitems, bytes;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, atom_active_window, 0, 1, False,
                               XA_WINDOW, &type, &fmt, &nitems, &bytes,
                               &data) == Success && data && nitems > 0) {
            Window active = *(Window *)data;
            XFree(data);
            for (int vi = 0; vi < vis_count; vi++) {
                if (wins[vis[vi]].xwin == active) {
                    selected = vi;
                    break;
                }
            }
        } else if (data) {
            XFree(data);
        }
    }

    int focus_mode = FOCUS_WINDOWS;
    int tab_highlight = cur_tab;
    int super_tab_count = 0;

    int mouse_start_x = -1, mouse_start_y = -1;
    int mouse_active = 0;
    {
        Window qroot, qchild;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (XQueryPointer(dpy, overlay, &qroot, &qchild, &rx, &ry, &wx, &wy, &mask)) {
            mouse_start_x = wx;
            mouse_start_y = wy;
        }
    }

#define DO_RENDER() render_thumbnails(overlay, wins, vis, vis_count, \
    scr_w, scr_h, mon, font, bold_font, selected, num_desktops, desk_names, \
    desk_layout, cur_tab, focus_mode, tab_highlight, show_all, filter, &cfg)

#define REFILTER() do { \
    ungrab_visible_pixmaps(wins, vis, vis_count); \
    vis_count = build_visible(wins, total, cur_tab, vis, total, show_all, filter); \
    compute_grid_layout(wins, vis, vis_count, mon, FILTER_HEIGHT, tab_total_h); \
    grab_visible_pixmaps(wins, vis, vis_count); \
    if (selected >= vis_count) selected = vis_count > 0 ? vis_count - 1 : 0; \
    DO_RENDER(); \
} while(0)

#define CLOSE_WINDOW(idx, timestamp) do { \
    int _wi = vis[(idx)]; \
    close_window(wins[_wi].xwin, (timestamp)); \
    if (wins[_wi].pixmap != None) { \
        XCompositeUnredirectWindow(dpy, wins[_wi].frame, CompositeRedirectAutomatic); \
        XFreePixmap(dpy, wins[_wi].pixmap); \
        wins[_wi].pixmap = None; \
    } \
    if (wins[_wi].icon_pic != None) { \
        XRenderFreePicture(dpy, wins[_wi].icon_pic); \
        wins[_wi].icon_pic = None; \
    } \
    if (wins[_wi].icon_pm != None) { \
        XFreePixmap(dpy, wins[_wi].icon_pm); \
        wins[_wi].icon_pm = None; \
    } \
    for (int _v = (idx); _v < vis_count - 1; _v++) \
        vis[_v] = vis[_v + 1]; \
    vis_count--; \
    if (vis_count == 0) { running = 0; break; } \
    if (selected >= vis_count) selected = vis_count - 1; \
    compute_grid_layout(wins, vis, vis_count, mon, FILTER_HEIGHT, tab_total_h); \
    DO_RENDER(); \
} while(0)

#define SWITCH_TAB(new_tab) do { \
    ungrab_visible_pixmaps(wins, vis, vis_count); \
    cur_tab = (new_tab); \
    tab_highlight = cur_tab; \
    switch_desktop(cur_tab); \
    filter[0] = '\0'; filter_len = 0; \
    vis_count = build_visible(wins, total, cur_tab, vis, total, 0, filter); \
    if (vis_count > 0) \
        wait_for_map(); \
    compute_grid_layout(wins, vis, vis_count, mon, FILTER_HEIGHT, tab_total_h); \
    grab_visible_pixmaps(wins, vis, vis_count); \
    selected = 0; \
    focus_mode = FOCUS_WINDOWS; \
} while(0)

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int running = 1;
    while (running) {
        if (got_signal) break;
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (got_signal) break;

        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                DO_RENDER();
            break;

        case MotionNotify: {
            if (!mouse_active) {
                int dx = ev.xmotion.x - mouse_start_x;
                int dy = ev.xmotion.y - mouse_start_y;
                if (dx * dx + dy * dy > 100)
                    mouse_active = 1;
            }
            if (mouse_active) {
                int tab = show_all ? -1 : find_tab_at(ev.xmotion.x, ev.xmotion.y, num_desktops, desk_layout, mon);
                if (tab >= 0) {
                    if (focus_mode != FOCUS_TABS || tab_highlight != tab) {
                        focus_mode = FOCUS_TABS;
                        tab_highlight = tab;
                        DO_RENDER();
                    }
                } else {
                    int idx = find_window_at(wins, vis, vis_count, ev.xmotion.x, ev.xmotion.y);
                    if (idx >= 0 && (idx != selected || focus_mode != FOCUS_WINDOWS)) {
                        selected = idx;
                        focus_mode = FOCUS_WINDOWS;
                        DO_RENDER();
                    }
                }
            }
            break;
        }

        case ButtonPress: {
            if (allow_close && ev.xbutton.button == Button2) {
                int idx = find_window_at(wins, vis, vis_count, ev.xbutton.x, ev.xbutton.y);
                if (idx >= 0)
                    CLOSE_WINDOW(idx, ev.xbutton.time);
                break;
            }

            int tab = show_all ? -1 : find_tab_at(ev.xbutton.x, ev.xbutton.y, num_desktops, desk_layout, mon);
            if (tab >= 0 && tab != cur_tab) {
                SWITCH_TAB(tab);
                DO_RENDER();
                break;
            }

            int idx = find_window_at(wins, vis, vis_count, ev.xbutton.x, ev.xbutton.y);
            if (idx >= 0)
                activate_window(wins[vis[idx]].xwin, wins[vis[idx]].desktop, ev.xbutton.time);
            running = 0;
            break;
        }

        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int redraw = 0;

            if (focus_mode == FOCUS_TABS) {
                int th_col = tab_highlight % desk_layout.cols;
                int th_row = tab_highlight / desk_layout.cols;

                switch (ks) {
                case XK_Escape:
                    running = 0;
                    break;
                case XK_Left:
                case XK_Page_Up:
                    if (th_col > 0) {
                        tab_highlight--;
                        redraw = 1;
                    }
                    break;
                case XK_Right:
                case XK_Page_Down:
                    if (th_col < desk_layout.cols - 1 && tab_highlight + 1 < num_desktops) {
                        tab_highlight++;
                        redraw = 1;
                    }
                    break;
                case XK_Down:
                    if (th_row < desk_layout.rows - 1) {
                        int next = (th_row + 1) * desk_layout.cols + th_col;
                        if (next < num_desktops) {
                            tab_highlight = next;
                            redraw = 1;
                        }
                    }
                    break;
                case XK_Up:
                    if (th_row > 0) {
                        tab_highlight = (th_row - 1) * desk_layout.cols + th_col;
                        redraw = 1;
                    } else {
                        focus_mode = FOCUS_WINDOWS;
                        int last_row_start = (vis_count / grid_cols) * grid_cols;
                        if (last_row_start >= vis_count) last_row_start -= grid_cols;
                        if (last_row_start < 0) last_row_start = 0;
                        selected = last_row_start;
                        redraw = 1;
                    }
                    break;
                case XK_Return:
                case XK_KP_Enter:
                    if (tab_highlight != cur_tab) {
                        SWITCH_TAB(tab_highlight);
                    } else {
                        focus_mode = FOCUS_WINDOWS;
                        selected = 0;
                    }
                    redraw = 1;
                    break;
                }
            } else {
                switch (ks) {
                case XK_Escape:
                    running = 0;
                    break;
                case XK_Return:
                case XK_KP_Enter:
                    if (vis_count > 0) {
                        activate_window(wins[vis[selected]].xwin, wins[vis[selected]].desktop, ev.xkey.time);
                        running = 0;
                    }
                    break;
                case XK_Left:
                    if (selected > 0) { selected--; redraw = 1; }
                    break;
                case XK_Right:
                    if (selected < vis_count - 1) { selected++; redraw = 1; }
                    break;
                case XK_Up:
                    if (selected - grid_cols >= 0) { selected -= grid_cols; redraw = 1; }
                    break;
                case XK_Down:
                    if (selected + grid_cols < vis_count) {
                        selected += grid_cols;
                        redraw = 1;
                    } else if (!show_all && num_desktops > 1) {
                        focus_mode = FOCUS_TABS;
                        tab_highlight = cur_tab;
                        redraw = 1;
                    }
                    break;
                case XK_Tab:
                    if (vis_count == 0) break;
                    if (ev.xkey.state & ShiftMask)
                        selected = (selected - 1 + vis_count) % vis_count;
                    else
                        selected = (selected + 1) % vis_count;
                    if (ev.xkey.state & Mod4Mask)
                        super_tab_count++;
                    redraw = 1;
                    break;
                case XK_Page_Down:
                    if (!show_all && cur_tab < num_desktops - 1) {
                        SWITCH_TAB(cur_tab + 1);
                        redraw = 1;
                    }
                    break;
                case XK_Page_Up:
                    if (!show_all && cur_tab > 0) {
                        SWITCH_TAB(cur_tab - 1);
                        redraw = 1;
                    }
                    break;
                case XK_Delete:
                    if (allow_close && vis_count > 0)
                        CLOSE_WINDOW(selected, ev.xkey.time);
                    break;
                case XK_BackSpace:
                    if (filter_len > 0) {
                        if (ev.xkey.state & ControlMask) {
                            filter[0] = '\0';
                            filter_len = 0;
                        } else {
                            filter[--filter_len] = '\0';
                        }
                        REFILTER();
                    }
                    break;
                default: {
                    char buf[8];
                    int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, NULL, NULL);
                    if (n > 0 && buf[0] >= 0x20 && filter_len + n < (int)sizeof(filter) - 1) {
                        memcpy(filter + filter_len, buf, n);
                        filter_len += n;
                        filter[filter_len] = '\0';
                        REFILTER();
                    }
                    break;
                }
                }
            }

            if (redraw)
                DO_RENDER();
            break;
        }

        case KeyRelease: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if ((ks == XK_Super_L || ks == XK_Super_R) && super_tab_count > 0) {
                if (vis_count > 0 && focus_mode == FOCUS_WINDOWS)
                    activate_window(wins[vis[selected]].xwin, wins[vis[selected]].desktop, ev.xkey.time);
                running = 0;
            }
            break;
        }

        case FocusOut:
            if (ev.xfocus.mode == NotifyNormal)
                running = 0;
            break;
        }
    }

    if (bold_font && bold_font != font) XftFontClose(dpy, bold_font);
    if (font) XftFontClose(dpy, font);

    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XDestroyWindow(dpy, overlay);

cleanup:
    ungrab_visible_pixmaps(wins, vis, vis_count);
    for (int i = 0; i < total; i++) {
        free(wins[i].title);
        free(wins[i].wm_class);
        if (wins[i].icon_pic != None)
            XRenderFreePicture(dpy, wins[i].icon_pic);
        if (wins[i].icon_pm != None)
            XFreePixmap(dpy, wins[i].icon_pm);
    }
    free(wins);
    free(vis);
    for (int i = 0; i < num_desktops; i++) free(desk_names[i]);
    free(desk_names);

    XCloseDisplay(dpy);
    return exit_code;
}
