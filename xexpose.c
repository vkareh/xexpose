#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xft/Xft.h>

#define PADDING      20
#define TITLE_HEIGHT 24
#define TAB_HEIGHT   36
#define BG_ALPHA     0x8000

#define ICON_SIZE   96
#define ICON_ALPHA  0xB333

#define FOCUS_WINDOWS 0
#define FOCUS_TABS    1

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
    unsigned long desktop;
    char         *title;
    int           cell_x, cell_y;
    int           thumb_w, thumb_h;
} WinInfo;

static Display *dpy;
static int      scr;
static Window   root;
static int      grid_cols;

static Atom atom_client_list;
static Atom atom_active_window;
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
static Atom atom_wm_name;
static Atom atom_wm_icon;
static Atom atom_utf8_string;

static void
intern_atoms(void)
{
    atom_client_list      = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    atom_active_window    = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
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
    atom_wm_name          = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_wm_icon          = XInternAtom(dpy, "_NET_WM_ICON", False);
    atom_utf8_string      = XInternAtom(dpy, "UTF8_STRING", False);
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
    unsigned long best_diff = ~0UL;

    unsigned long *p = icon_data;
    unsigned long *end = icon_data + nitems;
    while (p + 2 <= end) {
        unsigned long w = p[0];
        unsigned long h = p[1];
        if (w == 0 || h == 0 || w > 1024 || h > 1024 || p + 2 + w * h > end)
            break;
        unsigned long diff = (w > ICON_SIZE ? w - ICON_SIZE : ICON_SIZE - w)
                           + (h > ICON_SIZE ? h - ICON_SIZE : ICON_SIZE - h);
        if (diff < best_diff) {
            best_diff = diff;
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

    XRenderPictFormat *argb_fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    wi->icon_pm = XCreatePixmap(dpy, root, best_w, best_h, 32);
    wi->icon_pic = XRenderCreatePicture(dpy, wi->icon_pm, argb_fmt, 0, NULL);

    XRenderColor clear = { 0, 0, 0, 0 };
    XRenderFillRectangle(dpy, PictOpSrc, wi->icon_pic, &clear,
                         0, 0, best_w, best_h);

    for (unsigned long y = 0; y < best_h; y++) {
        for (unsigned long x = 0; x < best_w; x++) {
            uint32_t pixel = (uint32_t)best[y * best_w + x];
            uint8_t a = (pixel >> 24) & 0xFF;
            if (a == 0) continue;
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >>  8) & 0xFF;
            uint8_t b =  pixel        & 0xFF;
            XRenderColor c = {
                (unsigned short)((r << 8) | r),
                (unsigned short)((g << 8) | g),
                (unsigned short)((b << 8) | b),
                (unsigned short)((a << 8) | a)
            };
            XRenderFillRectangle(dpy, PictOpSrc, wi->icon_pic, &c, x, y, 1, 1);
        }
    }

    wi->icon_w = best_w;
    wi->icon_h = best_h;

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
        list[count].title   = get_window_title(w);
        list[count].pixmap  = None;

        if (frame != w) {
            XWindowAttributes fwa;
            if (XGetWindowAttributes(dpy, frame, &fwa)) {
                list[count].x      = fwa.x;
                list[count].y      = fwa.y;
                list[count].width  = fwa.width;
                list[count].height = fwa.height;
                list[count].visual = fwa.visual;
                list[count].depth  = fwa.depth;
            } else {
                list[count].x      = wa.x;
                list[count].y      = wa.y;
                list[count].width  = wa.width;
                list[count].height = wa.height;
                list[count].visual = wa.visual;
                list[count].depth  = wa.depth;
            }
        } else {
            list[count].x      = wa.x;
            list[count].y      = wa.y;
            list[count].width  = wa.width;
            list[count].height = wa.height;
            list[count].visual = wa.visual;
            list[count].depth  = wa.depth;
        }
        load_window_icon(&list[count]);
        count++;
    }
    XFree(data);

    *out = list;
    *out_count = count;
    return 0;
}

static int
build_visible(WinInfo *wins, int total, unsigned long tab, int *vis, int max)
{
    int n = 0;
    for (int i = 0; i < total && n < max; i++) {
        if (wins[i].desktop == tab || wins[i].desktop == ~0UL)
            vis[n++] = i;
    }
    return n;
}

static void
compute_grid_layout(WinInfo *wins, int *vis, int vis_count, int scr_w, int scr_h, int tab_h)
{
    if (vis_count == 0) { grid_cols = 1; return; }

    int cols = (int)ceil(sqrt((double)vis_count));
    int rows = (int)ceil((double)vis_count / cols);
    grid_cols = cols;

    int avail_h = scr_h - tab_h;
    int cell_w = (scr_w - PADDING * (cols + 1)) / cols;
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

        int cx = PADDING + col * (cell_w + PADDING) + (cell_w - thumb_w) / 2;
        int cy = PADDING + row * (cell_h + TITLE_HEIGHT + PADDING) + (cell_h - thumb_h) / 2;

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
                  int scr_w, int scr_h, XftFont *font, int selected,
                  int num_desktops, char **desk_names, DeskLayout layout,
                  int cur_tab, int focus_mode, int tab_highlight)
{
    Visual *dvis = DefaultVisual(dpy, scr);
    int ddepth = DefaultDepth(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    XRenderPictFormat *fmt_overlay = XRenderFindVisualFormat(dpy, dvis);

    Pixmap back_pm = XCreatePixmap(dpy, overlay, scr_w, scr_h, ddepth);
    Picture back_pic = XRenderCreatePicture(dpy, back_pm, fmt_overlay, 0, NULL);

    Pixmap root_pm = get_root_pixmap();
    if (root_pm != None) {
        Picture root_pic = XRenderCreatePicture(dpy, root_pm, fmt_overlay, 0, NULL);
        XRenderComposite(dpy, PictOpSrc, root_pic, None, back_pic,
                         0, 0, 0, 0, 0, 0, scr_w, scr_h);
        XRenderFreePicture(dpy, root_pic);

        XRenderColor tint = { 0x0000, 0x0000, 0x0000, BG_ALPHA };
        XRenderFillRectangle(dpy, PictOpOver, back_pic, &tint,
                             0, 0, scr_w, scr_h);
    } else {
        XRenderColor bg = { 0x1000, 0x1000, 0x1800, 0xFFFF };
        XRenderFillRectangle(dpy, PictOpSrc, back_pic, &bg,
                             0, 0, scr_w, scr_h);
    }

    XRenderColor border_color    = { 0x4000, 0x4000, 0x5000, 0xFFFF };
    XRenderColor highlight_color = { 0xCCCC, 0xCCCC, 0xFFFF, 0xFFFF };
    XRenderColor sticky_color    = { 0xDDDD, 0xAAAA, 0x3333, 0xFFFF };

    XErrorHandler old_handler = XSetErrorHandler(error_handler);

    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];
        if (wins[i].pixmap == None)
            continue;

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

        XTransform xform = {{
            { XDoubleToFixed((double)wins[i].width  / wins[i].thumb_w), 0, 0 },
            { 0, XDoubleToFixed((double)wins[i].height / wins[i].thumb_h), 0 },
            { 0, 0, XDoubleToFixed(1.0) }
        }};
        XRenderSetPictureTransform(dpy, src, &xform);
        XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);

        int is_selected = (focus_mode == FOCUS_WINDOWS && vi == selected);
        int is_sticky = (wins[i].desktop == ~0UL);
        XRenderColor *bc = is_selected ? &highlight_color
                         : is_sticky   ? &sticky_color
                         :               &border_color;
        int bw = is_selected ? 4 : is_sticky ? 3 : 2;

        XRenderFillRectangle(dpy, PictOpOver, back_pic, bc,
                             wins[i].cell_x - bw, wins[i].cell_y - bw,
                             wins[i].thumb_w + bw * 2, wins[i].thumb_h + bw * 2);

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
    XRenderColor rc = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    XftColorAllocValue(dpy, dvis, cmap, &rc, &text_color);

    XftColor shadow_color;
    XRenderColor sc = { 0x0000, 0x0000, 0x0000, 0xCCCC };
    XftColorAllocValue(dpy, dvis, cmap, &sc, &shadow_color);

    for (int vi = 0; vi < vis_count; vi++) {
        int i = vis[vi];
        int tx = wins[i].cell_x;
        int ty = wins[i].cell_y + wins[i].thumb_h + TITLE_HEIGHT;
        int max_w = wins[i].thumb_w;

        char *title = wins[i].title;
        int full_len = strlen(title);
        int len = full_len;
        int truncated = 0;

        XGlyphInfo extents;
        XftTextExtentsUtf8(dpy, font, (FcChar8 *)title, len, &extents);

        if (extents.xOff > max_w) {
            XGlyphInfo ellipsis_ext;
            XftTextExtentsUtf8(dpy, font, (FcChar8 *)"\xe2\x80\xa6", 3, &ellipsis_ext);
            int avail = max_w - ellipsis_ext.xOff;

            while (len > 1) {
                len--;
                XftTextExtentsUtf8(dpy, font, (FcChar8 *)title, len, &extents);
                if (extents.xOff <= avail) break;
            }
            truncated = 1;
        }

        XftDrawStringUtf8(xftdraw, &shadow_color, font,
                          tx + 1, ty + 1, (FcChar8 *)title, len);
        XftDrawStringUtf8(xftdraw, &text_color, font,
                          tx, ty, (FcChar8 *)title, len);
        if (truncated) {
            XftDrawStringUtf8(xftdraw, &shadow_color, font,
                              tx + extents.xOff + 1, ty + 1,
                              (FcChar8 *)"\xe2\x80\xa6", 3);
            XftDrawStringUtf8(xftdraw, &text_color, font,
                              tx + extents.xOff, ty,
                              (FcChar8 *)"\xe2\x80\xa6", 3);
        }
    }

    XftColorFree(dpy, dvis, cmap, &shadow_color);

    int tab_total_h = TAB_HEIGHT * layout.rows;
    int tab_w = scr_w / layout.cols;

    XRenderColor tab_bg      = { 0x2000, 0x2000, 0x2800, 0xFFFF };
    XRenderColor tab_active  = { 0x4000, 0x4000, 0x5000, 0xFFFF };
    XRenderColor tab_focused = { 0x6000, 0x6000, 0x8000, 0xFFFF };

    for (int t = 0; t < num_desktops; t++) {
        int tc_col = t % layout.cols;
        int tc_row = t / layout.cols;
        int tx = tc_col * tab_w;
        int ty = scr_h - tab_total_h + tc_row * TAB_HEIGHT;

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
        XftDrawStringUtf8(xftdraw, &text_color, font,
                          text_x, text_y, (FcChar8 *)name, len);
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
find_tab_at(int mx, int my, int num_desktops, DeskLayout layout, int scr_w, int scr_h)
{
    int tab_total_h = TAB_HEIGHT * layout.rows;
    if (my < scr_h - tab_total_h)
        return -1;
    int tab_w = scr_w / layout.cols;
    int tc_col = mx / tab_w;
    int tc_row = (my - (scr_h - tab_total_h)) / TAB_HEIGHT;
    if (tc_col >= layout.cols) tc_col = layout.cols - 1;
    if (tc_row >= layout.rows) tc_row = layout.rows - 1;
    int t = tc_row * layout.cols + tc_col;
    if (t >= num_desktops) return -1;
    return t;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

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

    int num_desktops = (int)get_cardinal(root, atom_num_desktops);
    if (num_desktops < 1) num_desktops = 1;

    int name_count;
    char **desk_names = get_desktop_names(num_desktops, &name_count);
    DeskLayout desk_layout = get_desktop_layout(num_desktops);

    unsigned long cur_desktop = get_cardinal(root, atom_cur_desktop);
    int tab_total_h = TAB_HEIGHT * desk_layout.rows;
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

    int *vis = calloc(total, sizeof(int));
    int vis_count = build_visible(wins, total, cur_tab, vis, total);

    if (vis_count == 1 && num_desktops == 1) {
        activate_window(wins[vis[0]].xwin, wins[vis[0]].desktop, CurrentTime);
        for (int i = 0; i < total; i++) {
            free(wins[i].title);
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

    compute_grid_layout(wins, vis, vis_count, scr_w, scr_h, tab_total_h);
    grab_visible_pixmaps(wins, vis, vis_count);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.event_mask = ExposureMask | ButtonPressMask | KeyPressMask | KeyReleaseMask | PointerMotionMask;
    swa.background_pixel = BlackPixel(dpy, scr);

    Window overlay = XCreateWindow(dpy, root,
        0, 0, scr_w, scr_h, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &swa);

    XMapRaised(dpy, overlay);

    XGrabKeyboard(dpy, overlay, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    XGrabPointer(dpy, overlay, True,
                 ButtonPressMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync,
                 overlay, None, CurrentTime);

    XftFont *font = XftFontOpenName(dpy, scr, "sans-10");
    if (!font)
        font = XftFontOpenName(dpy, scr, "fixed");
    if (!font) {
        fprintf(stderr, "xexpose: cannot open any font\n");
        ungrab_visible_pixmaps(wins, vis, vis_count);
        XDestroyWindow(dpy, overlay);
        goto cleanup;
    }

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
    scr_w, scr_h, font, selected, num_desktops, desk_names, \
    desk_layout, cur_tab, focus_mode, tab_highlight)

#define SWITCH_TAB(new_tab) do { \
    ungrab_visible_pixmaps(wins, vis, vis_count); \
    cur_tab = (new_tab); \
    tab_highlight = cur_tab; \
    switch_desktop(cur_tab); \
    vis_count = build_visible(wins, total, cur_tab, vis, total); \
    if (vis_count > 0) \
        wait_for_map(); \
    compute_grid_layout(wins, vis, vis_count, scr_w, scr_h, tab_total_h); \
    grab_visible_pixmaps(wins, vis, vis_count); \
    selected = 0; \
    focus_mode = FOCUS_WINDOWS; \
} while(0)

    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

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
                int tab = find_tab_at(ev.xmotion.x, ev.xmotion.y, num_desktops, desk_layout, scr_w, scr_h);
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
            int tab = find_tab_at(ev.xbutton.x, ev.xbutton.y, num_desktops, desk_layout, scr_w, scr_h);
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
                    } else if (num_desktops > 1) {
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
                    if (cur_tab < num_desktops - 1) {
                        SWITCH_TAB(cur_tab + 1);
                        redraw = 1;
                    }
                    break;
                case XK_Page_Up:
                    if (cur_tab > 0) {
                        SWITCH_TAB(cur_tab - 1);
                        redraw = 1;
                    }
                    break;
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
        }
    }

    if (font) XftFontClose(dpy, font);

    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XDestroyWindow(dpy, overlay);

cleanup:
    ungrab_visible_pixmaps(wins, vis, vis_count);
    for (int i = 0; i < total; i++) {
        free(wins[i].title);
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
    return 0;
}
