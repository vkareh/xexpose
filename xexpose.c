#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xft/Xft.h>

#define PADDING     20
#define TITLE_HEIGHT 24
#define BG_ALPHA   0x8000

typedef struct {
    Window     xwin;
    Window     frame;
    Pixmap     pixmap;
    Visual    *visual;
    int        depth;
    int        x, y;
    unsigned   width, height;
    char      *title;
    int        cell_x, cell_y;
    int        thumb_w, thumb_h;
} WinInfo;

static Display *dpy;
static int      scr;
static Window   root;
static int      grid_cols;

static Atom atom_client_list;
static Atom atom_active_window;
static Atom atom_wm_desktop;
static Atom atom_cur_desktop;
static Atom atom_wm_type;
static Atom atom_type_dock;
static Atom atom_type_desktop;
static Atom atom_wm_state;
static Atom atom_state_hidden;
static Atom atom_wm_name;
static Atom atom_utf8_string;

static void
intern_atoms(void)
{
    atom_client_list   = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    atom_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_wm_desktop    = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    atom_cur_desktop   = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    atom_wm_type       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_type_dock     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    atom_type_desktop  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    atom_wm_state      = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_state_hidden  = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    atom_wm_name       = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_utf8_string   = XInternAtom(dpy, "UTF8_STRING", False);
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

static int
get_window_list(WinInfo **out, int *out_count, int use_frames)
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

    unsigned long cur_desktop = get_cardinal(root, atom_cur_desktop);
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

        unsigned long desk = get_cardinal(w, atom_wm_desktop);
        if (desk != cur_desktop && desk != 0xFFFFFFFF)
            continue;

        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa))
            continue;
        if (wa.map_state != IsViewable)
            continue;

        Window parent, qroot;
        Window *children;
        unsigned int nchildren;
        Window frame = w;
        if (XQueryTree(dpy, w, &qroot, &parent, &children, &nchildren)) {
            if (children) XFree(children);
            if (parent != root)
                frame = parent;
        }

        list[count].xwin  = w;
        list[count].frame = frame;
        list[count].title = get_window_title(w);
        list[count].pixmap = None;

        if (use_frames && frame != w) {
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
        count++;
    }
    XFree(data);

    *out = list;
    *out_count = count;
    return 0;
}

static void
compute_grid_layout(WinInfo *wins, int count, int scr_w, int scr_h)
{
    int cols = (int)ceil(sqrt((double)count));
    int rows = (int)ceil((double)count / cols);
    grid_cols = cols;

    int cell_w = (scr_w - PADDING * (cols + 1)) / cols;
    int cell_h = (scr_h - PADDING * (rows + 1)) / (rows) - TITLE_HEIGHT;

    for (int i = 0; i < count; i++) {
        int col = i % cols;
        int row = i / cols;

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
grab_pixmaps(WinInfo *wins, int count)
{
    XErrorHandler old = XSetErrorHandler(error_handler);

    for (int i = 0; i < count; i++) {
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
ungrab_pixmaps(WinInfo *wins, int count)
{
    XErrorHandler old = XSetErrorHandler(error_handler);

    for (int i = 0; i < count; i++) {
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
activate_window(Window win)
{
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = win;
    ev.xclient.message_type = atom_active_window;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 2;
    ev.xclient.data.l[1]    = CurrentTime;
    ev.xclient.data.l[2]    = 0;

    XSendEvent(dpy, root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
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

static void
render_thumbnails(Window overlay, WinInfo *wins, int count,
                  int scr_w, int scr_h, XftFont *font, int selected)
{
    Visual *vis = DefaultVisual(dpy, scr);
    int depth = DefaultDepth(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    XRenderPictFormat *fmt_overlay = XRenderFindVisualFormat(dpy, vis);

    Pixmap back_pm = XCreatePixmap(dpy, overlay, scr_w, scr_h, depth);
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

    XRenderColor border_color = { 0x4000, 0x4000, 0x5000, 0xFFFF };
    XRenderColor highlight_color = { 0xCCCC, 0xCCCC, 0xFFFF, 0xFFFF };

    XErrorHandler old_handler = XSetErrorHandler(error_handler);

    for (int i = 0; i < count; i++) {
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

        int is_selected = (i == selected);
        XRenderColor *bc = is_selected ? &highlight_color : &border_color;
        int bw = is_selected ? 4 : 2;

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

        XRenderFreePicture(dpy, src);
    }

    XSetErrorHandler(old_handler);

    XftDraw *xftdraw = XftDrawCreate(dpy, back_pm, vis, cmap);
    XftColor text_color;
    XRenderColor rc = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    XftColorAllocValue(dpy, vis, cmap, &rc, &text_color);

    for (int i = 0; i < count; i++) {
        int tx = wins[i].cell_x;
        int ty = wins[i].cell_y + wins[i].thumb_h + TITLE_HEIGHT - 4;
        int max_w = wins[i].thumb_w;

        char *title = wins[i].title;
        int len = strlen(title);

        XGlyphInfo extents;
        XftTextExtentsUtf8(dpy, font, (FcChar8 *)title, len, &extents);

        while (len > 1 && extents.xOff > max_w) {
            len--;
            XftTextExtentsUtf8(dpy, font, (FcChar8 *)title, len, &extents);
        }

        XftDrawStringUtf8(xftdraw, &text_color, font,
                          tx, ty, (FcChar8 *)title, len);
    }

    XftDrawDestroy(xftdraw);
    XftColorFree(dpy, vis, cmap, &text_color);

    GC gc = XCreateGC(dpy, overlay, 0, NULL);
    XCopyArea(dpy, back_pm, overlay, gc, 0, 0, scr_w, scr_h, 0, 0);
    XFreeGC(dpy, gc);

    XRenderFreePicture(dpy, back_pic);
    XFreePixmap(dpy, back_pm);
}

static int
find_window_at(WinInfo *wins, int count, int mx, int my)
{
    for (int i = 0; i < count; i++) {
        if (mx >= wins[i].cell_x && mx < wins[i].cell_x + wins[i].thumb_w &&
            my >= wins[i].cell_y && my < wins[i].cell_y + wins[i].thumb_h) {
            return i;
        }
    }
    return -1;
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

    WinInfo *wins = NULL;
    int count = 0;
    if (get_window_list(&wins, &count, 1) < 0 || count == 0) {
        free(wins);
        XCloseDisplay(dpy);
        return 0;
    }

    if (count == 1) {
        activate_window(wins[0].xwin);
        free(wins[0].title);
        free(wins);
        XCloseDisplay(dpy);
        return 0;
    }

    int scr_w = DisplayWidth(dpy, scr);
    int scr_h = DisplayHeight(dpy, scr);

    compute_grid_layout(wins, count, scr_w, scr_h);
    grab_pixmaps(wins, count);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.event_mask = ExposureMask | ButtonPressMask | KeyPressMask;
    swa.background_pixel = BlackPixel(dpy, scr);

    Window overlay = XCreateWindow(dpy, root,
        0, 0, scr_w, scr_h, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &swa);

    XMapRaised(dpy, overlay);

    XGrabKeyboard(dpy, overlay, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    XGrabPointer(dpy, overlay, True,
                 ButtonPressMask, GrabModeAsync, GrabModeAsync,
                 overlay, None, CurrentTime);

    XftFont *font = XftFontOpenName(dpy, scr, "sans-10");
    if (!font)
        font = XftFontOpenName(dpy, scr, "fixed");

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
            for (int i = 0; i < count; i++) {
                if (wins[i].xwin == active) {
                    selected = i;
                    break;
                }
            }
        } else if (data) {
            XFree(data);
        }
    }
    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                render_thumbnails(overlay, wins, count, scr_w, scr_h, font, selected);
            break;

        case ButtonPress: {
            int idx = find_window_at(wins, count, ev.xbutton.x, ev.xbutton.y);
            if (idx >= 0)
                activate_window(wins[idx].xwin);
            running = 0;
            break;
        }

        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int redraw = 0;

            switch (ks) {
            case XK_Escape:
                running = 0;
                break;
            case XK_Return:
            case XK_KP_Enter:
                activate_window(wins[selected].xwin);
                running = 0;
                break;
            case XK_Left:
                if (selected > 0) { selected--; redraw = 1; }
                break;
            case XK_Right:
                if (selected < count - 1) { selected++; redraw = 1; }
                break;
            case XK_Up:
                if (selected - grid_cols >= 0) { selected -= grid_cols; redraw = 1; }
                break;
            case XK_Down:
                if (selected + grid_cols < count) { selected += grid_cols; redraw = 1; }
                break;
            case XK_Tab:
                if (ev.xkey.state & ShiftMask)
                    selected = (selected - 1 + count) % count;
                else
                    selected = (selected + 1) % count;
                redraw = 1;
                break;
            }

            if (redraw)
                render_thumbnails(overlay, wins, count, scr_w, scr_h, font, selected);
            break;
        }
        }
    }

    if (font) XftFontClose(dpy, font);

    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XDestroyWindow(dpy, overlay);

    ungrab_pixmaps(wins, count);
    for (int i = 0; i < count; i++)
        free(wins[i].title);
    free(wins);

    XCloseDisplay(dpy);
    return 0;
}
