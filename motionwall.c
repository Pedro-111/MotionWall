/*
 * MotionWall - Advanced Desktop Background Animation Tool
 * Copyright © 2025 MotionWall Project
 *
 * Based on original xwinwrap code with extensive improvements:
 * - Multi-monitor support with Xrandr
 * - Desktop environment detection and integration
 * - Playlist support for multiple videos
 * - Auto-resolution detection
 * - Better compositor integration
 * - Configuration file support
 * 
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation.
 */

#define _XOPEN_SOURCE 500  // Para usleep

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <glob.h>
#include <libgen.h>

#define NAME "motionwall"
#define VERSION "1.0.0"
#define CONFIG_DIR ".config/motionwall"
#define MAX_MONITORS 16
#define MAX_PLAYLIST 1024
#define MAX_PATH 8192  // Aumentado para evitar truncation warnings
#define MAX_CMD_ARGS 64
#define MAX_ARG_LEN 256

#define ATOM(a) XInternAtom(display, #a, False)

Display *display = NULL;
int screen;
bool debug = false;

typedef enum {
    SHAPE_RECT = 0,
    SHAPE_CIRCLE,
    SHAPE_TRIANGLE,
} win_shape;

typedef enum {
    DE_UNKNOWN = 0,
    DE_GNOME,
    DE_KDE,
    DE_XFCE,
    DE_CINNAMON,
    DE_MATE,
    DE_LXDE,
    DE_I3,
    DE_AWESOME
} desktop_environment;

typedef struct {
    char name[256];
    int x, y;
    unsigned int width, height;
    bool primary;
    bool connected;
} monitor_info;

typedef struct {
    monitor_info monitors[MAX_MONITORS];
    int count;
    int primary_index;
} monitor_setup;

typedef struct {
    char paths[MAX_PLAYLIST][MAX_PATH];
    int count;
    int current;
    int duration;  // seconds per video
    bool shuffle;
    bool loop;
} playlist;

typedef struct {
    Window root, window, desktop;
    Drawable drawable;
    Visual *visual;
    Colormap colourmap;
    unsigned int width;
    unsigned int height;
    int x;
    int y;
    int monitor_id;
} window_info;

typedef struct {
    bool multi_monitor;
    bool auto_resolution;
    bool playlist_mode;
    bool compositor_aware;
    char config_file[MAX_PATH];
    char media_player[256];
    char player_args[1024];
    desktop_environment de;
    playlist media_playlist;
    monitor_setup monitors;
    window_info *windows;
    int window_count;
} motionwall_config;

static motionwall_config config = {0};
static pid_t *child_pids = NULL;
static int child_count = 0;

// Function prototypes
static void init_x11(void);
static void detect_desktop_environment(void);
static int detect_monitors(void);
static void create_playlist(const char *path);
static void setup_compositor_integration(void);
static Window find_desktop_window(Window *p_root, Window *p_desktop, int monitor_id);
static void create_window_for_monitor(int monitor_id);
static void start_media_player(int window_index);
static void playlist_next(void);
static void signal_handler(int sig);
static void cleanup_and_exit(void);
static void load_config_file(const char *config_path);
static void save_config_file(void);
static bool safe_path_join(char *dest, size_t dest_size, const char *base, const char *append);

// Safe path joining function
static bool safe_path_join(char *dest, size_t dest_size, const char *base, const char *append) {
    if (!dest || !base || !append || dest_size == 0) {
        return false;
    }
    
    int ret = snprintf(dest, dest_size, "%s/%s", base, append);
    if (ret < 0 || ret >= (int)dest_size) {
        if (debug) {
            fprintf(stderr, NAME ": Error: Path too long when joining '%s' and '%s'\n", base, append);
        }
        return false;
    }
    
    return true;
}

// Desktop environment detection
static void detect_desktop_environment(void) {
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    const char *session = getenv("DESKTOP_SESSION");
    
    config.de = DE_UNKNOWN;
    
    if (desktop) {
        if (strstr(desktop, "GNOME")) config.de = DE_GNOME;
        else if (strstr(desktop, "KDE")) config.de = DE_KDE;
        else if (strstr(desktop, "XFCE")) config.de = DE_XFCE;
        else if (strstr(desktop, "X-Cinnamon")) config.de = DE_CINNAMON;
        else if (strstr(desktop, "MATE")) config.de = DE_MATE;
        else if (strstr(desktop, "LXDE")) config.de = DE_LXDE;
    }
    
    if (config.de == DE_UNKNOWN && session) {
        if (strstr(session, "gnome")) config.de = DE_GNOME;
        else if (strstr(session, "kde")) config.de = DE_KDE;
        else if (strstr(session, "xfce")) config.de = DE_XFCE;
        else if (strstr(session, "cinnamon")) config.de = DE_CINNAMON;
        else if (strstr(session, "mate")) config.de = DE_MATE;
        else if (strstr(session, "i3")) config.de = DE_I3;
    }
    
    if (debug) {
        const char *de_names[] = {"Unknown", "GNOME", "KDE", "XFCE", "Cinnamon", "MATE", "LXDE", "i3", "Awesome"};
        fprintf(stderr, NAME ": Detected desktop environment: %s\n", de_names[config.de]);
    }
}

// Multi-monitor detection using Xrandr
static int detect_monitors(void) {
    XRRScreenResources *screen_resources;
    XRROutputInfo *output_info;
    XRRCrtcInfo *crtc_info;
    int i;
    
    config.monitors.count = 0;
    config.monitors.primary_index = -1;
    
    screen_resources = XRRGetScreenResources(display, DefaultRootWindow(display));
    if (!screen_resources) {
        fprintf(stderr, NAME ": Error: Could not get screen resources\n");
        return 0;
    }
    
    for (i = 0; i < screen_resources->noutput && config.monitors.count < MAX_MONITORS; i++) {
        output_info = XRRGetOutputInfo(display, screen_resources, screen_resources->outputs[i]);
        if (!output_info) continue;
        
        if (output_info->connection == RR_Connected && output_info->crtc) {
            crtc_info = XRRGetCrtcInfo(display, screen_resources, output_info->crtc);
            if (crtc_info) {
                monitor_info *mon = &config.monitors.monitors[config.monitors.count];
                
                strncpy(mon->name, output_info->name, sizeof(mon->name) - 1);
                mon->name[sizeof(mon->name) - 1] = '\0';
                mon->x = crtc_info->x;
                mon->y = crtc_info->y;
                mon->width = crtc_info->width;
                mon->height = crtc_info->height;
                mon->connected = true;
                
                // Check if this is the primary monitor
                RROutput primary = XRRGetOutputPrimary(display, DefaultRootWindow(display));
                mon->primary = (screen_resources->outputs[i] == primary);
                
                if (mon->primary) {
                    config.monitors.primary_index = config.monitors.count;
                }
                
                if (debug) {
                    fprintf(stderr, NAME ": Monitor %d: %s (%dx%d+%d+%d) %s\n",
                            config.monitors.count, mon->name, mon->width, mon->height,
                            mon->x, mon->y, mon->primary ? "(primary)" : "");
                }
                
                config.monitors.count++;
                XRRFreeCrtcInfo(crtc_info);
            }
        }
        XRRFreeOutputInfo(output_info);
    }
    
    XRRFreeScreenResources(screen_resources);
    
    if (config.monitors.primary_index == -1 && config.monitors.count > 0) {
        config.monitors.primary_index = 0;
        config.monitors.monitors[0].primary = true;
    }
    
    return config.monitors.count;
}

// Playlist creation from directory or file list
static void create_playlist(const char *path) {
    struct stat path_stat;
    glob_t glob_result;
    int i;
    
    config.media_playlist.count = 0;
    config.media_playlist.current = 0;
    
    if (stat(path, &path_stat) != 0) {
        fprintf(stderr, NAME ": Error: Cannot access path: %s\n", path);
        return;
    }
    
    if (S_ISDIR(path_stat.st_mode)) {
        // Directory - find all video files
        char pattern[MAX_PATH];
        const char *extensions[] = {"*.mp4", "*.avi", "*.mkv", "*.mov", "*.webm", "*.gif", "*.mp3", "*.wav"};
        const int ext_count = sizeof(extensions) / sizeof(extensions[0]);
        
        for (int ext = 0; ext < ext_count; ext++) {
            if (!safe_path_join(pattern, sizeof(pattern), path, extensions[ext])) {
                continue;
            }
            
            if (glob(pattern, (ext == 0) ? 0 : GLOB_APPEND, NULL, &glob_result) == 0) {
                for (i = 0; i < (int)glob_result.gl_pathc && config.media_playlist.count < MAX_PLAYLIST; i++) {
                    strncpy(config.media_playlist.paths[config.media_playlist.count], 
                           glob_result.gl_pathv[i], MAX_PATH - 1);
                    config.media_playlist.paths[config.media_playlist.count][MAX_PATH - 1] = '\0';
                    config.media_playlist.count++;
                }
            }
        }
        globfree(&glob_result);
    } else {
        // Single file
        strncpy(config.media_playlist.paths[0], path, MAX_PATH - 1);
        config.media_playlist.paths[0][MAX_PATH - 1] = '\0';
        config.media_playlist.count = 1;
    }
    
    if (debug) {
        fprintf(stderr, NAME ": Created playlist with %d items\n", config.media_playlist.count);
        for (i = 0; i < config.media_playlist.count; i++) {
            fprintf(stderr, "  %d: %s\n", i, config.media_playlist.paths[i]);
        }
    }
}

// Compositor integration setup
static void setup_compositor_integration(void) {
    // Check for running compositors
    Window comp_window;
    
    // Check for picom/compton
    comp_window = XGetSelectionOwner(display, ATOM(_NET_WM_CM_S0));
    if (comp_window != None) {
        if (debug) fprintf(stderr, NAME ": Detected compositor, adjusting integration\n");
        config.compositor_aware = true;
        
        // Set properties for better compositor integration
        switch (config.de) {
            case DE_GNOME: {
                // GNOME Shell specific optimizations
                if (debug) fprintf(stderr, NAME ": Applying GNOME Shell optimizations\n");
                
                // Set GNOME-specific window properties
                Atom gnome_panel_atom = ATOM(_GNOME_PANEL_DESKTOP_AREA);
                if (gnome_panel_atom != None) {
                    Atom actual_type;
                    int actual_format;
                    unsigned long nitems, bytes_after;
                    unsigned char *prop_data = NULL;
                    
                    if (XGetWindowProperty(display, DefaultRootWindow(display),
                                 gnome_panel_atom, 0, 4, False, XA_CARDINAL,
                                 &actual_type, &actual_format, &nitems,
                                 &bytes_after, &prop_data) == Success) {
                        
                        if (prop_data && nitems == 4) {
                            // Solo para debug
                            if (debug) {
                                long *area = (long*)prop_data;
                                fprintf(stderr, NAME ": GNOME desktop area: %ldx%ld+%ld+%ld\n",
                                        area[2], area[3], area[0], area[1]);
                            }
                        }
                        if (prop_data) XFree(prop_data);
                    }
                }
                
                // Set window class for GNOME recognition
                XClassHint class_hint;
                class_hint.res_name = "motionwall";
                class_hint.res_class = "MotionWall";
                
                for (int i = 0; i < config.window_count; i++) {
                    XSetClassHint(display, config.windows[i].window, &class_hint);
                    
                    // Set GNOME-specific properties
                    Atom atom = ATOM(_GNOME_WM_SKIP_ANIMATIONS);
                    if (atom != None) {
                        long value = 1;
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_CARDINAL, 32, PropModeReplace,
                                      (unsigned char*)&value, 1);
                    }
                }
                break;
            }
            
            case DE_KDE: {
                // KWin specific optimizations
                if (debug) fprintf(stderr, NAME ": Applying KDE/KWin optimizations\n");
                
                for (int i = 0; i < config.window_count; i++) {
                    // Set KDE-specific window properties
                    Atom atom = ATOM(_KDE_NET_WM_ACTIVITIES);
                    if (atom != None) {
                        // Set to work on all activities
                        const char *all_activities = "00000000-0000-0000-0000-000000000000";
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_STRING, 8, PropModeReplace,
                                      (unsigned char*)all_activities, strlen(all_activities));
                    }
                    
                    // KWincompositor integration
                    atom = ATOM(_KDE_NET_WM_BLUR_BEHIND_REGION);
                    if (atom != None) {
                        // Disable blur behind for performance
                        XDeleteProperty(display, config.windows[i].window, atom);
                    }
                    
                    // Set window to desktop layer
                    atom = ATOM(_NET_WM_WINDOW_TYPE);
                    Atom desktop_type = ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
                    XChangeProperty(display, config.windows[i].window, atom,
                                  XA_ATOM, 32, PropModeReplace,
                                  (unsigned char*)&desktop_type, 1);
                    
                    // KWin specific bypass compositor hint
                    atom = ATOM(_KDE_NET_WM_BYPASS_COMPOSITOR);
                    if (atom != None) {
                        long value = 1; // Bypass compositor for better performance
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_CARDINAL, 32, PropModeReplace,
                                      (unsigned char*)&value, 1);
                    }
                }
                break;
            }
            
            case DE_CINNAMON: {
                // Muffin (Cinnamon's compositor) specific optimizations
                if (debug) fprintf(stderr, NAME ": Applying Cinnamon/Muffin optimizations\n");
                
                for (int i = 0; i < config.window_count; i++) {
                    // Cinnamon desktop integration
                    Atom atom = ATOM(_MUFFIN_HINTS);
                    if (atom != None) {
                        // Set Muffin-specific hints
                        long hints = 0x1; // Background window hint
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_CARDINAL, 32, PropModeReplace,
                                      (unsigned char*)&hints, 1);
                    }
                    
                    // Ensure proper stacking with Nemo desktop
                    atom = ATOM(_NET_WM_STATE);
                    if (atom != None) {
                        Atom states[2] = {
                            ATOM(_NET_WM_STATE_BELOW),
                            ATOM(_NET_WM_STATE_SKIP_TASKBAR)
                        };
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_ATOM, 32, PropModeReplace,
                                      (unsigned char*)states, 2);
                    }
                    
                    // Set desktop type for proper Nemo integration
                    atom = ATOM(_NET_WM_WINDOW_TYPE);
                    Atom desktop_type = ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
                    XChangeProperty(display, config.windows[i].window, atom,
                                  XA_ATOM, 32, PropModeReplace,
                                  (unsigned char*)&desktop_type, 1);
                }
                break;
            }
            
            case DE_XFCE: {
                // Xfwm4 and Xfce specific optimizations
                if (debug) fprintf(stderr, NAME ": Applying XFCE optimizations\n");
                
                for (int i = 0; i < config.window_count; i++) {
                    // XFCE desktop integration
                    Atom atom = ATOM(_XFCE_DESKTOP_WINDOW);
                    if (atom != None) {
                        long value = 1;
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_CARDINAL, 32, PropModeReplace,
                                      (unsigned char*)&value, 1);
                    }
                    
                    // Ensure window is below desktop icons
                    atom = ATOM(_NET_WM_STATE);
                    if (atom != None) {
                        Atom below = ATOM(_NET_WM_STATE_BELOW);
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_ATOM, 32, PropModeReplace,
                                      (unsigned char*)&below, 1);
                    }
                }
                break;
            }
            
            case DE_MATE: {
                // MATE desktop specific optimizations
                if (debug) fprintf(stderr, NAME ": Applying MATE optimizations\n");
                
                for (int i = 0; i < config.window_count; i++) {
                    // MATE/Marco compositor integration
                    Atom atom = ATOM(_MATE_DESKTOP_WINDOW);
                    if (atom != None) {
                        long value = 1;
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_CARDINAL, 32, PropModeReplace,
                                      (unsigned char*)&value, 1);
                    }
                }
                break;
            }
            
            case DE_I3: {
                // i3wm specific optimizations
                if (debug) fprintf(stderr, NAME ": Applying i3wm optimizations\n");
                
                for (int i = 0; i < config.window_count; i++) {
                    // i3 doesn't use traditional desktop concepts
                    // Set window to be floating and positioned correctly
                    Atom atom = ATOM(_NET_WM_STATE);
                    if (atom != None) {
                        Atom states[3] = {
                            ATOM(_NET_WM_STATE_STICKY),
                            ATOM(_NET_WM_STATE_SKIP_TASKBAR),
                            ATOM(_NET_WM_STATE_SKIP_PAGER)
                        };
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_ATOM, 32, PropModeReplace,
                                      (unsigned char*)states, 3);
                    }
                }
                break;
            }
            
            default: {
                // Generic optimizations for unknown desktop environments
                if (debug) fprintf(stderr, NAME ": Applying generic desktop optimizations\n");
                
                for (int i = 0; i < config.window_count; i++) {
                    // Basic window properties that should work everywhere
                    Atom atom = ATOM(_NET_WM_STATE);
                    if (atom != None) {
                        Atom states[2] = {
                            ATOM(_NET_WM_STATE_BELOW),
                            ATOM(_NET_WM_STATE_SKIP_TASKBAR)
                        };
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_ATOM, 32, PropModeReplace,
                                      (unsigned char*)states, 2);
                    }
                    
                    // Set as desktop type window
                    atom = ATOM(_NET_WM_WINDOW_TYPE);
                    if (atom != None) {
                        Atom desktop_type = ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
                        XChangeProperty(display, config.windows[i].window, atom,
                                      XA_ATOM, 32, PropModeReplace,
                                      (unsigned char*)&desktop_type, 1);
                    }
                }
                break;
            }
        }

        // Additional compositor detection and optimization
        comp_window = XGetSelectionOwner(display, ATOM(_NET_WM_CM_S0));
        if (comp_window != None) {
            if (debug) fprintf(stderr, NAME ": Compositor detected, applying additional optimizations\n");
            
            for (int i = 0; i < config.window_count; i++) {
                // Set opacity for compositor
                Atom opacity_atom = ATOM(_NET_WM_WINDOW_OPACITY);
                if (opacity_atom != None) {
                    unsigned long opacity = 0xFFFFFFFF; // Fully opaque
                    XChangeProperty(display, config.windows[i].window, opacity_atom,
                                  XA_CARDINAL, 32, PropModeReplace,
                                  (unsigned char*)&opacity, 1);
                }
                
                // Disable window shadows if supported
                Atom shadow_atom = ATOM(_COMPTON_SHADOW);
                if (shadow_atom != None) {
                    long value = 0; // Disable shadows
                    XChangeProperty(display, config.windows[i].window, shadow_atom,
                                  XA_CARDINAL, 32, PropModeReplace,
                                  (unsigned char*)&value, 1);
                }
                
                // Disable window fading
                Atom fade_atom = ATOM(_COMPTON_FADE);
                if (fade_atom != None) {
                    long value = 0; // Disable fading
                    XChangeProperty(display, config.windows[i].window, fade_atom,
                                  XA_CARDINAL, 32, PropModeReplace,
                                  (unsigned char*)&value, 1);
                }
            }
        }
    }
}

// Enhanced desktop window finding with monitor support
static Window find_desktop_window(Window *p_root, Window *p_desktop, int monitor_id) {
    Atom type;
    int format, i;
    unsigned long nitems, bytes;
    unsigned int n;
    Window root = RootWindow(display, screen);
    Window win = root;
    Window troot, parent, *children;
    unsigned char *buf = NULL;
    monitor_info *mon = &config.monitors.monitors[monitor_id];

    if (!p_root || !p_desktop) {
        return 0;
    }

    // Look for desktop window in the specific monitor area
    XQueryTree(display, root, &troot, &parent, &children, &n);
    for (i = 0; i < (int)n; i++) {
        if (XGetWindowProperty(display, children[i], ATOM(__SWM_VROOT), 0, 1, False,
                               XA_WINDOW, &type, &format, &nitems, &bytes,
                               &buf) == Success && type == XA_WINDOW) {
            win = *(Window *)buf;
            XFree(buf);
            XFree(children);
            
            // Verify this window is in our monitor area
            XWindowAttributes attrs;
            if (XGetWindowAttributes(display, win, &attrs)) {
                if (attrs.x >= mon->x && attrs.x < mon->x + (int)mon->width &&
                    attrs.y >= mon->y && attrs.y < mon->y + (int)mon->height) {
                    *p_root = win;
                    *p_desktop = win;
                    return win;
                }
            }
        }
        if (buf) {
            XFree(buf);
            buf = 0;
        }
    }
    XFree(children);

    *p_root = root;
    *p_desktop = root;
    return root;
}

// Create window for specific monitor
static void create_window_for_monitor(int monitor_id) {
    monitor_info *mon = &config.monitors.monitors[monitor_id];
    window_info *win = &config.windows[monitor_id];
    
    win->monitor_id = monitor_id;
    win->x = mon->x;
    win->y = mon->y;
    win->width = mon->width;
    win->height = mon->height;
    
    // Get ARGB visual for transparency support
    XVisualInfo visual_template;
    XVisualInfo *visual_list;
    int nxvisuals = 0, depth = 24;
    bool have_argb = false;
    
    visual_template.screen = screen;
    visual_list = XGetVisualInfo(display, VisualScreenMask, &visual_template, &nxvisuals);
    
    for (int i = 0; i < nxvisuals; i++) {
        if (visual_list[i].depth == 32 && 
            (visual_list[i].red_mask == 0xff0000 &&
             visual_list[i].green_mask == 0x00ff00 &&
             visual_list[i].blue_mask == 0x0000ff)) {
            win->visual = visual_list[i].visual;
            depth = visual_list[i].depth;
            have_argb = true;
            break;
        }
    }
    
    if (!have_argb) {
        win->visual = DefaultVisual(display, screen);
        depth = DefaultDepth(display, screen);
    }
    
    if (have_argb) {
        win->colourmap = XCreateColormap(display, DefaultRootWindow(display),
                                        win->visual, AllocNone);
    } else {
        win->colourmap = DefaultColormap(display, screen);
    }
    
    XFree(visual_list);
    
    // Find desktop window for this monitor
    if (!find_desktop_window(&win->root, &win->desktop, monitor_id)) {
        fprintf(stderr, NAME ": Error: couldn't find desktop window for monitor %d\n", monitor_id);
        return;
    }
    
    // Create window with desktop integration
    XSetWindowAttributes attrs = {0};
    attrs.background_pixmap = ParentRelative;
    attrs.backing_store = Always;
    attrs.event_mask = StructureNotifyMask | ExposureMask;
    attrs.override_redirect = True;
    
    if (have_argb) {
        attrs.colormap = win->colourmap;
        attrs.border_pixel = 0;
    }
    
    unsigned long flags = CWBackingStore | CWEventMask | CWOverrideRedirect;
    if (have_argb) {
        flags |= CWColormap | CWBorderPixel;
    } else {
        flags |= CWBackPixmap;
    }
    
    win->window = XCreateWindow(display, win->desktop, win->x, win->y,
                               win->width, win->height, 0, depth,
                               InputOutput, win->visual, flags, &attrs);
    
    // Set window properties for desktop integration
    Atom xa = ATOM(_NET_WM_WINDOW_TYPE);
    Atom prop = ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
    XChangeProperty(display, win->window, xa, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&prop, 1);
    
    // Set window to bottom layer
    xa = ATOM(_NET_WM_STATE);
    if (xa != None) {
        Atom xa_prop = ATOM(_NET_WM_STATE_BELOW);
        XChangeProperty(display, win->window, xa, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&xa_prop, 1);
    }
    
    // Disable input on the window
    Region region = XCreateRegion();
    if (region) {
        XShapeCombineRegion(display, win->window, ShapeInput, 0, 0, region, ShapeSet);
        XDestroyRegion(region);
    }
    
    XMapWindow(display, win->window);
    XLowerWindow(display, win->window);
    XSync(display, False);
    
    if (debug) {
        fprintf(stderr, NAME ": Created window for monitor %d (%s): %dx%d+%d+%d\n",
                monitor_id, mon->name, win->width, win->height, win->x, win->y);
    }
}

// Start media player for specific window - CORREGIDO
static void start_media_player(int window_index) {
   char wid_arg[64];
   char *args[MAX_CMD_ARGS];
   int argc = 0;
   
   if (config.media_playlist.count == 0) {
       fprintf(stderr, NAME ": Error: No media files in playlist\n");
       return;
   }
   
   int ret = snprintf(wid_arg, sizeof(wid_arg), "0x%x", (unsigned int)config.windows[window_index].window);
   if (ret >= (int)sizeof(wid_arg)) {
       fprintf(stderr, NAME ": Error: Window ID too long\n");
       return;
   }
   
   // Build command line
   args[argc++] = config.media_player;
   
   // Add player-specific arguments
   if (strstr(config.media_player, "mpv")) {
       args[argc++] = "--wid";
       args[argc++] = wid_arg;
       args[argc++] = "--really-quiet";
       args[argc++] = "--no-audio";
       args[argc++] = "--loop-file";
       args[argc++] = config.media_playlist.loop ? "inf" : "no";
       args[argc++] = "--panscan=1.0";
       args[argc++] = "--keepaspect=no";
   } else if (strstr(config.media_player, "mplayer")) {
       args[argc++] = "-wid";
       args[argc++] = wid_arg;
       args[argc++] = "-nosound";
       args[argc++] = "-panscan";
       args[argc++] = "1.0";
       args[argc++] = "-framedrop";
       if (config.media_playlist.loop) {
           args[argc++] = "-loop";
           args[argc++] = "0";
       }
   } else if (strstr(config.media_player, "vlc")) {
       args[argc++] = "--intf";
       args[argc++] = "dummy";
       args[argc++] = "--no-audio";
       args[argc++] = "--drawable-xid";
       args[argc++] = wid_arg;
       if (config.media_playlist.loop) {
           args[argc++] = "--loop";
       }
   }
   
   // Add current media file
   if (argc < MAX_CMD_ARGS - 1) {
       args[argc++] = config.media_playlist.paths[config.media_playlist.current];
       args[argc] = NULL;
   } else {
       fprintf(stderr, NAME ": Error: Too many command arguments\n");
       return;
   }
   
   pid_t pid = fork();
   if (pid == 0) {
       // Child process
       execvp(args[0], args);
       perror(args[0]);
       exit(2);
   } else if (pid > 0) {
       // Parent process
       if (child_count < config.window_count * 2) {
           child_pids[child_count++] = pid;
       }
       if (debug) {
           fprintf(stderr, NAME ": Started %s (PID %d) for window %d with file: %s\n",
                   config.media_player, pid, window_index,
                   config.media_playlist.paths[config.media_playlist.current]);
       }
   } else {
       perror("fork");
   }
}

// Playlist management
static void playlist_next(void) {
   if (config.media_playlist.count <= 1) return;
   
   if (config.media_playlist.shuffle) {
       config.media_playlist.current = rand() % config.media_playlist.count;
   } else {
       config.media_playlist.current = (config.media_playlist.current + 1) % config.media_playlist.count;
   }
   
   if (debug) {
       fprintf(stderr, NAME ": Switching to: %s\n", 
               config.media_playlist.paths[config.media_playlist.current]);
   }
}

// Signal handler
static void signal_handler(int sig) {
   if (debug) {
       fprintf(stderr, NAME ": Received signal %d, cleaning up...\n", sig);
   }
   cleanup_and_exit();
}

// Cleanup and exit
static void cleanup_and_exit(void) {
   int i;
   
   // Kill all child processes
   for (i = 0; i < child_count; i++) {
       if (child_pids[i] > 0) {
           kill(child_pids[i], SIGTERM);
           waitpid(child_pids[i], NULL, WNOHANG);
       }
   }
   
   // Destroy all windows
   if (config.windows) {
       for (i = 0; i < config.window_count; i++) {
           if (config.windows[i].window) {
               XDestroyWindow(display, config.windows[i].window);
           }
       }
       free(config.windows);
   }
   
   if (child_pids) free(child_pids);
   if (display) XCloseDisplay(display);
   
   exit(0);
}

// Configuration file support - CORREGIDO
static void load_config_file(const char *config_path) {
   FILE *file = fopen(config_path, "r");
   if (!file) return;
   
   char line[1024];
   while (fgets(line, sizeof(line), file) != NULL) {
       // Remove newline
       line[strcspn(line, "\n")] = '\0';
       
       // Skip comments and empty lines
       if (line[0] == '#' || line[0] == '\0') continue;
       
       // Parse key=value pairs
       char *eq = strchr(line, '=');
       if (!eq) continue;
       
       *eq = '\0';
       char *key = line;
       char *value = eq + 1;
       
       if (strcmp(key, "media_player") == 0) {
           strncpy(config.media_player, value, sizeof(config.media_player) - 1);
           config.media_player[sizeof(config.media_player) - 1] = '\0';
       } else if (strcmp(key, "playlist_duration") == 0) {
           config.media_playlist.duration = atoi(value);
       } else if (strcmp(key, "playlist_shuffle") == 0) {
           config.media_playlist.shuffle = (strcmp(value, "true") == 0);
       } else if (strcmp(key, "playlist_loop") == 0) {
           config.media_playlist.loop = (strcmp(value, "true") == 0);
       } else if (strcmp(key, "multi_monitor") == 0) {
           config.multi_monitor = (strcmp(value, "true") == 0);
       }
   }
   
   fclose(file);
}

// Save configuration file - CORREGIDO
static void save_config_file(void) {
   const char *home = getenv("HOME");
   if (!home) {
       fprintf(stderr, NAME ": Error: HOME environment variable not set\n");
       return;
   }
   
   char config_dir[MAX_PATH];
   char config_path[MAX_PATH];
   
   // Safely construct config directory path
   if (!safe_path_join(config_dir, sizeof(config_dir), home, CONFIG_DIR)) {
       fprintf(stderr, NAME ": Error: Config directory path too long\n");
       return;
   }
   
   // Create config directory
   if (mkdir(config_dir, 0755) != 0 && errno != EEXIST) {
       if (debug) {
           perror(NAME ": mkdir config_dir");
       }
   }
   
   // Safely construct config file path
   if (!safe_path_join(config_path, sizeof(config_path), config_dir, "config")) {
       fprintf(stderr, NAME ": Error: Config file path too long\n");
       return;
   }
   
   FILE *file = fopen(config_path, "w");
   if (!file) {
       if (debug) {
           perror(NAME ": fopen config file");
       }
       return;
   }
   
   fprintf(file, "# MotionWall Configuration File\n");
   fprintf(file, "media_player=%s\n", config.media_player);
   fprintf(file, "playlist_duration=%d\n", config.media_playlist.duration);
   fprintf(file, "playlist_shuffle=%s\n", config.media_playlist.shuffle ? "true" : "false");
   fprintf(file, "playlist_loop=%s\n", config.media_playlist.loop ? "true" : "false");
   fprintf(file, "multi_monitor=%s\n", config.multi_monitor ? "true" : "false");
   
   fclose(file);
   
   if (debug) {
       fprintf(stderr, NAME ": Configuration saved to: %s\n", config_path);
   }
}

// Initialize X11
static void init_x11(void) {
   display = XOpenDisplay(NULL);
   if (!display) {
       fprintf(stderr, NAME ": Error: couldn't open display\n");
       exit(1);
   }
   screen = DefaultScreen(display);
}

// Usage information
static void usage(void) {
   fprintf(stderr, "%s v%s - Advanced Desktop Background Animation Tool\n", NAME, VERSION);
   fprintf(stderr, "\nUsage: %s [OPTIONS] <media-file-or-directory>\n\n", NAME);
   fprintf(stderr, "Options:\n");
   fprintf(stderr, "  -m, --multi-monitor    Enable multi-monitor support\n");
   fprintf(stderr, "  -p, --player PLAYER    Media player to use (mpv, mplayer, vlc)\n");
   fprintf(stderr, "  -s, --shuffle          Shuffle playlist\n");
   fprintf(stderr, "  -l, --loop             Loop playlist\n");
   fprintf(stderr, "  -d, --duration SEC     Duration per video in playlist (default: 30)\n");
   fprintf(stderr, "  -c, --config FILE      Use custom config file\n");
   fprintf(stderr, "  --auto-res             Auto-detect and use native resolution\n");
   fprintf(stderr, "  --daemon               Run as daemon\n");
   fprintf(stderr, "  --debug                Enable debug output\n");
   fprintf(stderr, "  -h, --help             Show this help\n");
   fprintf(stderr, "\nExamples:\n");
   fprintf(stderr, "  %s video.mp4                    # Single video\n", NAME);
   fprintf(stderr, "  %s -m ~/Videos/                 # Multi-monitor playlist\n", NAME);
   fprintf(stderr, "  %s -p mpv -s -l ~/Wallpapers/   # Shuffled looping playlist\n", NAME);
}

// Main function
int main(int argc, char **argv) {
   int i;
   bool daemon_mode = false;
   char media_path[MAX_PATH] = {0};
   
   // Initialize configuration with defaults
   strcpy(config.media_player, "mpv");
   config.media_playlist.duration = 30;
   config.media_playlist.shuffle = false;
   config.media_playlist.loop = true;
   config.multi_monitor = false;
   config.auto_resolution = true;
   config.playlist_mode = false;
   config.compositor_aware = false;
   
   // Load default config - CORREGIDO
   const char *home = getenv("HOME");
   if (home) {
       char default_config[MAX_PATH];
       if (safe_path_join(default_config, sizeof(default_config), home, CONFIG_DIR)) {
           char config_file[MAX_PATH];
           if (safe_path_join(config_file, sizeof(config_file), default_config, "config")) {
               load_config_file(config_file);
           }
       }
   }
   
   // Parse command line arguments
   for (i = 1; i < argc; i++) {
       if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--multi-monitor") == 0) {
           config.multi_monitor = true;
       } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--player") == 0) {
           if (++i < argc) {
               strncpy(config.media_player, argv[i], sizeof(config.media_player) - 1);
               config.media_player[sizeof(config.media_player) - 1] = '\0';
           }
       } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--shuffle") == 0) {
           config.media_playlist.shuffle = true;
       } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--loop") == 0) {
           config.media_playlist.loop = true;
       } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) {
           if (++i < argc) {
               config.media_playlist.duration = atoi(argv[i]);
           }
       } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
           if (++i < argc) {
               load_config_file(argv[i]);
           }
       } else if (strcmp(argv[i], "--auto-res") == 0) {
           config.auto_resolution = true;
       } else if (strcmp(argv[i], "--daemon") == 0) {
           daemon_mode = true;
       } else if (strcmp(argv[i], "--debug") == 0) {
           debug = true;
       } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
           usage();
           return 0;
       } else if (argv[i][0] != '-') {
           strncpy(media_path, argv[i], sizeof(media_path) - 1);
           media_path[sizeof(media_path) - 1] = '\0';
       }
   }
   
   if (strlen(media_path) == 0) {
       fprintf(stderr, NAME ": Error: No media file or directory specified\n");
       usage();
       return 1;
   }
  
  // Daemonize if requested
  if (daemon_mode) {
      pid_t pid = fork();
      if (pid < 0) {
          perror("fork");
          exit(1);
      }
      if (pid > 0) {
          printf("MotionWall daemon started with PID: %d\n", pid);
          exit(0);
      }
      
      umask(0);
      if (setsid() < 0) {
          exit(1);
      }
      
      if (chdir("/") < 0) {
          if (debug) perror("chdir");
      }
      if (!debug) {
          close(STDIN_FILENO);
          close(STDOUT_FILENO);
          close(STDERR_FILENO);
      }
  }
  
  // Initialize random seed for shuffle
  srand(time(NULL));
  
  // Initialize X11
  init_x11();
  
  // Detect desktop environment
  detect_desktop_environment();
  
  // Detect monitors
  if (!detect_monitors()) {
      fprintf(stderr, NAME ": Error: No monitors detected\n");
      return 1;
  }
  
  // Create playlist
  create_playlist(media_path);
  if (config.media_playlist.count == 0) {
      fprintf(stderr, NAME ": Error: No compatible media files found\n");
      return 1;
  }
  
  // Determine how many windows to create
  if (config.multi_monitor) {
      config.window_count = config.monitors.count;
  } else {
      config.window_count = 1;
  }
  
  // Allocate memory for windows and child PIDs
  config.windows = calloc(config.window_count, sizeof(window_info));
  child_pids = calloc(config.window_count * 2, sizeof(pid_t)); // Extra space for playlist changes
  
  if (!config.windows || !child_pids) {
      fprintf(stderr, NAME ": Error: Memory allocation failed\n");
      return 1;
  }
  
  // Set up signal handlers
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);
  signal(SIGCHLD, SIG_IGN); // Prevent zombie processes
  
  // Create windows for each monitor (or just primary)
  if (config.multi_monitor) {
      for (i = 0; i < config.monitors.count; i++) {
          create_window_for_monitor(i);
      }
  } else {
      // Use primary monitor only
      int primary = config.monitors.primary_index;
      if (primary == -1) primary = 0;
      create_window_for_monitor(primary);
  }
  
  // Setup compositor integration AFTER windows are created
  setup_compositor_integration();
  
  // Start media players
  for (i = 0; i < config.window_count; i++) {
      start_media_player(i);
  }
  
  // Save current configuration
  save_config_file();
  
  if (debug) {
      fprintf(stderr, NAME ": Setup complete. Running with %d window(s).\n", config.window_count);
  }
  
  // Main loop for playlist management
  time_t last_change = time(NULL);
  int status;
  
  while (1) {
      // Check for dead children and restart if needed
      for (i = 0; i < child_count; i++) {
          if (child_pids[i] > 0) {
              pid_t result = waitpid(child_pids[i], &status, WNOHANG);
              if (result > 0) {
                  if (debug) {
                      fprintf(stderr, NAME ": Child process %d exited\n", child_pids[i]);
                  }
                  child_pids[i] = 0;
                  
                  // Restart if not intentional exit
                  if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                      // Find which window this was for and restart
                      for (int j = 0; j < config.window_count; j++) {
                          start_media_player(j);
                          break;
                      }
                  }
              }
          }
      }
      
      // Handle playlist changes
      if (config.media_playlist.count > 1 && config.media_playlist.duration > 0) {
          time_t now = time(NULL);
          if (now - last_change >= config.media_playlist.duration) {
              // Kill current players
              for (i = 0; i < child_count; i++) {
                  if (child_pids[i] > 0) {
                      kill(child_pids[i], SIGTERM);
                      waitpid(child_pids[i], NULL, 0);
                      child_pids[i] = 0;
                  }
              }
              child_count = 0;
              
              // Switch to next in playlist
              playlist_next();
              
              // Restart players with new media
              for (i = 0; i < config.window_count; i++) {
                  start_media_player(i);
              }
              
              last_change = now;
          }
      }
      
      // Process X11 events
      while (XPending(display)) {
          XEvent event;
          XNextEvent(display, &event);
          
          switch (event.type) {
              case ConfigureNotify: {
                  // Handle window configuration changes
                  XConfigureEvent *ce = &event.xconfigure;
                  
                  // Find which window was configured
                  for (int j = 0; j < config.window_count; j++) {
                      if (config.windows[j].window == ce->window) {
                          // Check if size changed
                          if (ce->width != (int)config.windows[j].width || 
                              ce->height != (int)config.windows[j].height) {
                              
                              config.windows[j].width = ce->width;
                              config.windows[j].height = ce->height;
                              config.windows[j].x = ce->x;
                              config.windows[j].y = ce->y;
                              
                              if (debug) {
                                  fprintf(stderr, NAME ": Window %d resized to %dx%d+%d+%d\n",
                                          j, ce->width, ce->height, ce->x, ce->y);
                              }
                              
                              // Restart media player with new dimensions
                              for (int k = 0; k < child_count; k++) {
                                  if (child_pids[k] > 0) {
                                      kill(child_pids[k], SIGTERM);
                                      waitpid(child_pids[k], NULL, 0);
                                      child_pids[k] = 0;
                                  }
                              }
                              child_count = 0;
                              
                              // Small delay before restart
                              usleep(500000);
                              start_media_player(j);
                          }
                          break;
                      }
                  }
                  break;
              }
              
              case Expose: {
                  // Handle expose events
                  XExposeEvent *ee = &event.xexpose;
                  
                  // Only handle if this is the last expose event in the sequence
                  if (ee->count == 0) {
                      for (int j = 0; j < config.window_count; j++) {
                          if (config.windows[j].window == ee->window) {
                              // Clear the exposed area
                              XClearArea(display, config.windows[j].window,
                                       ee->x, ee->y, ee->width, ee->height, False);
                              
                              // Force window to bottom layer
                              XLowerWindow(display, config.windows[j].window);
                              
                              if (debug) {
                                  fprintf(stderr, NAME ": Expose event for window %d (%dx%d+%d+%d)\n",
                                          j, ee->width, ee->height, ee->x, ee->y);
                              }
                              break;
                          }
                      }
                  }
                  break;
              }
              
              case PropertyNotify: {
                  // Handle property changes (useful for desktop environment changes)
                  XPropertyEvent *pe = &event.xproperty;
                  
                  if (pe->atom == ATOM(_NET_CURRENT_DESKTOP) || 
                      pe->atom == ATOM(_NET_DESKTOP_GEOMETRY)) {
                      
                      if (debug) {
                          fprintf(stderr, NAME ": Desktop property changed, checking configuration\n");
                      }
                      
                      // Re-detect monitors in case of configuration change
                      int old_count = config.monitors.count;
                      detect_monitors();
                      
                      if (config.monitors.count != old_count) {
                          if (debug) {
                              fprintf(stderr, NAME ": Monitor configuration changed (%d -> %d)\n",
                                      old_count, config.monitors.count);
                          }
                          
                          // TODO: Handle monitor configuration changes
                          // This would involve recreating windows for new monitor setup
                      }
                  }
                  break;
              }
              
              case DestroyNotify: {
                  // Handle window destruction
                  XDestroyWindowEvent *de = &event.xdestroywindow;
                  
                  for (int j = 0; j < config.window_count; j++) {
                      if (config.windows[j].window == de->window) {
                          if (debug) {
                              fprintf(stderr, NAME ": Window %d was destroyed\n", j);
                          }
                          
                          // Mark window as invalid
                          config.windows[j].window = None;
                          
                          // Kill associated media player
                          for (int k = 0; k < child_count; k++) {
                              if (child_pids[k] > 0) {
                                  kill(child_pids[k], SIGTERM);
                                  child_pids[k] = 0;
                              }
                          }
                          break;
                      }
                  }
                  break;
              }
              
              case VisibilityNotify: {
                  // Handle visibility changes
                  XVisibilityEvent *ve = &event.xvisibility;
                  
                  for (int j = 0; j < config.window_count; j++) {
                      if (config.windows[j].window == ve->window) {
                          if (ve->state == VisibilityFullyObscured) {
                              if (debug) {
                                  fprintf(stderr, NAME ": Window %d fully obscured\n", j);
                              }
                              // Optionally pause media player to save resources
                          } else if (ve->state == VisibilityUnobscured) {
                              if (debug) {
                                  fprintf(stderr, NAME ": Window %d unobscured\n", j);
                              }
                              // Ensure window stays at bottom
                              XLowerWindow(display, config.windows[j].window);
                          }
                          break;
                      }
                  }
                  break;
              }
              
              case ClientMessage: {
                  // Handle client messages (useful for desktop environment communication)
                  XClientMessageEvent *cm = &event.xclient;
                  
                  if (cm->message_type == ATOM(_NET_WM_STATE)) {
                      if (debug) {
                          fprintf(stderr, NAME ": Received WM state change message\n");
                      }
                      // Handle window manager state changes
                  }
                  break;
              }
              
              default:
                  // Handle other events if needed
                  break;
          }
      }
      
      // Reduce CPU usage
      usleep(10000); // 10 ms
  }
  
  // Never reached, but needed for structure
  return 0;
}
