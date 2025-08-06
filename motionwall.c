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
#include <sys/file.h>
#include <dirent.h>
#include <time.h>
#include <glob.h>
#include <libgen.h>
#include <errno.h>
#include <fcntl.h>

#define NAME "motionwall"
#define VERSION "1.0.0"
#define CONFIG_DIR ".config/motionwall"
#define MAX_MONITORS 16
#define MAX_PLAYLIST 1024
#define MAX_PATH 8192
#define MAX_CMD_ARGS 64
#define MAX_ARG_LEN 256

#define ATOM(a) XInternAtom(display, #a, False)

Display *display = NULL;
int screen;
bool debug = false;
volatile bool running = true;
static int lock_fd = -1; // File descriptor para lock de instancia única

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
    pid_t player_pid;    // PID del reproductor para esta ventana
    bool player_active;  // Estado del reproductor
    time_t player_start_time; // Tiempo de inicio del reproductor
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

// Function prototypes
static void init_x11(void);
static void detect_desktop_environment(void);
static int detect_monitors(void);
static void create_playlist(const char *path);
static void setup_compositor_integration(void);
static void create_window_for_monitor(int monitor_id);
static void start_media_player(int window_index);
static void check_and_restart_players(void);
static void terminate_all_players(void);
static void terminate_player(int window_index);
static bool is_process_healthy(pid_t pid);
static void playlist_next(void);
static void signal_handler(int sig);
static void cleanup_and_exit(void);
static void load_config_file(const char *config_path);
static void save_config_file(void);
static bool safe_path_join(char *dest, size_t dest_size, const char *base, const char *append);
static int create_lock_file(void);
static void force_windows_to_background(void);
static bool screen_config_changed = false;
static int xrandr_event_base = 0;
static int xrandr_error_base = 0;

// Definir constantes XRandR si no están disponibles
#ifndef RRScreenChangeNotify
#define RRScreenChangeNotify 0
#endif

#ifndef RRCrtcChangeNotify  
#define RRCrtcChangeNotify 1
#endif

#ifndef RROutputChangeNotify
#define RROutputChangeNotify 2
#endif

#ifndef RRNumberEvents
#define RRNumberEvents 3
#endif
// Función para crear archivo de lock de instancia única
static int create_lock_file(void) {
    int fd = open("/tmp/motionwall.lock", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        if (debug) {
            perror(NAME ": open lock file");
        }
        return -1;
    }
    
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, NAME ": Another instance is already running\n");
        } else if (debug) {
            perror(NAME ": flock");
        }
        return -1;
    }
    
    // Escribir PID al archivo
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (write(fd, pid_str, strlen(pid_str)) < 0) {
        if (debug) {
            perror(NAME ": write lock file");
        }
    }
    
    return fd;
}

// Verificar si un proceso está saludable
static bool is_process_healthy(pid_t pid) {
    if (pid <= 0) return false;
    
    // Verificar si el proceso existe
    if (kill(pid, 0) != 0) {
        return false;
    }
    
    // Verificar que sea realmente el reproductor correcto
    char proc_path[256];
    char exe_path[256];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", pid);
    
    ssize_t len = readlink(proc_path, exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        // Verificar que el ejecutable contenga el nombre del reproductor
        return (strstr(exe_path, config.media_player) != NULL);
    }
    
    // Si no podemos verificar, asumir que está bien
    return true;
}

// Terminar reproductor específico
static void terminate_player(int window_index) {
    if (window_index < 0 || window_index >= config.window_count) {
        return;
    }
    
    window_info *win = &config.windows[window_index];
    
    if (win->player_active && win->player_pid > 0) {
        if (debug) {
            fprintf(stderr, NAME ": Terminating player PID %d for window %d\n", 
                    win->player_pid, window_index);
        }
        
        // Terminación amigable primero
        kill(win->player_pid, SIGTERM);
        usleep(500000); // 500ms para terminación amigable
        
        // Verificar si aún vive
        if (kill(win->player_pid, 0) == 0) {
            if (debug) {
                fprintf(stderr, NAME ": Force killing player PID %d\n", win->player_pid);
            }
            kill(win->player_pid, SIGKILL);
        }
        
        // Limpiar zombie
        waitpid(win->player_pid, NULL, WNOHANG);
        
        win->player_pid = 0;
        win->player_active = false;
        win->player_start_time = 0;
    }
}

// Terminar todos los reproductores
static void terminate_all_players(void) {
    if (debug) {
        fprintf(stderr, NAME ": Terminating all players\n");
    }
    
    for (int i = 0; i < config.window_count; i++) {
        terminate_player(i);
    }
    
    // Pausa adicional para asegurar limpieza
    usleep(200000); // 200ms
}

// Verificar y reiniciar reproductores muertos
static void check_and_restart_players(void) {
    time_t now = time(NULL);
    
    for (int i = 0; i < config.window_count; i++) {
        window_info *win = &config.windows[i];
        
        if (win->player_active && win->player_pid > 0) {
            // Verificar si el proceso está vivo y saludable
            if (!is_process_healthy(win->player_pid)) {
                if (debug) {
                    fprintf(stderr, NAME ": Player for window %d (PID %d) is unhealthy or dead\n", 
                            i, win->player_pid);
                }
                
                // Limpiar proceso muerto
                waitpid(win->player_pid, NULL, WNOHANG);
                win->player_pid = 0;
                win->player_active = false;
                win->player_start_time = 0;
                
                // Esperar antes de reiniciar
                sleep(1);
                
                // Reiniciar SOLO este reproductor
                if (debug) {
                    fprintf(stderr, NAME ": Restarting player for window %d\n", i);
                }
                start_media_player(i);
            }
        } else if (!win->player_active && win->window != None) {
            // Ventana sin reproductor activo - reiniciar si es necesario
            if (debug) {
                fprintf(stderr, NAME ": Window %d has no active player, starting one\n", i);
            }
            start_media_player(i);
        }
        
        // Verificar si un reproductor ha estado ejecutándose demasiado tiempo sin respuesta
        if (win->player_active && win->player_start_time > 0) {
            if (now - win->player_start_time > 300) { // 5 minutos
                if (debug) {
                    fprintf(stderr, NAME ": Player for window %d running too long, checking health\n", i);
                }
                
                if (!is_process_healthy(win->player_pid)) {
                    terminate_player(i);
                    usleep(500000); // 500ms
                    start_media_player(i);
                }
                
                // Reset timer
                win->player_start_time = now;
            }
        }
    }
}

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
static bool monitors_have_changed(monitor_setup *old_setup, monitor_setup *new_setup) {
    if (old_setup->count != new_setup->count) {
        return true;
    }
    
    for (int i = 0; i < old_setup->count; i++) {
        monitor_info *old_mon = &old_setup->monitors[i];
        monitor_info *new_mon = &new_setup->monitors[i];
        
        if (old_mon->width != new_mon->width ||
            old_mon->height != new_mon->height ||
            old_mon->x != new_mon->x ||
            old_mon->y != new_mon->y ||
            old_mon->connected != new_mon->connected) {
            return true;
        }
    }
    
    return false;
}
static void handle_screen_change(void) {
    if (debug) {
        fprintf(stderr, NAME ": Screen configuration changed, adapting...\n");
    }
    
    // Guardar configuración anterior
    monitor_setup old_monitors = config.monitors;
    
    // Detectar nueva configuración
    if (!detect_monitors()) {
        fprintf(stderr, NAME ": Error re-detecting monitors after screen change\n");
        return;
    }
    
    // Verificar si realmente cambió algo importante
    if (!monitors_have_changed(&old_monitors, &config.monitors)) {
        if (debug) {
            fprintf(stderr, NAME ": False alarm, no significant changes detected\n");
        }
        return;
    }
    
    if (debug) {
        fprintf(stderr, NAME ": Significant monitor changes detected:\n");
        fprintf(stderr, "  Old setup: %d monitors\n", old_monitors.count);
        fprintf(stderr, "  New setup: %d monitors\n", config.monitors.count);
    }
    
    // Terminar reproductores existentes
    terminate_all_players();
    
    // Destruir ventanas existentes
    if (config.windows) {
        for (int i = 0; i < config.window_count; i++) {
            if (config.windows[i].window != None) {
                XDestroyWindow(display, config.windows[i].window);
                config.windows[i].window = None;
            }
        }
        XSync(display, False);
    }
    
    // Esperar a que se complete la destrucción
    usleep(500000); // 500ms
    
    // Reconfigurar número de ventanas
    int new_window_count;
    if (config.multi_monitor) {
        new_window_count = config.monitors.count;
    } else {
        new_window_count = 1;
    }
    
    // Reallocar memoria para ventanas si es necesario
    if (new_window_count != config.window_count) {
        free(config.windows);
        config.windows = calloc(new_window_count, sizeof(window_info));
        if (!config.windows) {
            fprintf(stderr, NAME ": Error: Memory allocation failed during screen change\n");
            cleanup_and_exit();
            return;
        }
        config.window_count = new_window_count;
    } else {
        // Limpiar estructuras existentes
        memset(config.windows, 0, config.window_count * sizeof(window_info));
    }
    
    // Recrear ventanas con nuevas configuraciones
    if (config.multi_monitor) {
        for (int i = 0; i < config.monitors.count; i++) {
            create_window_for_monitor(i);
            if (config.windows[i].window == None) {
                fprintf(stderr, NAME ": Failed to recreate window for monitor %d\n", i);
            }
        }
    } else {
        // Usar monitor primario
        int primary = config.monitors.primary_index;
        if (primary == -1) primary = 0;
        create_window_for_monitor(primary);
        if (config.windows[0].window == None) {
            fprintf(stderr, NAME ": Failed to recreate primary window\n");
        }
    }
    
    // Reconfigurar integración con compositor
    setup_compositor_integration();
    
    // Esperar a que las ventanas se establezcan
    usleep(500000); // 500ms
    
    // Reiniciar reproductores
    for (int i = 0; i < config.window_count; i++) {
        if (config.windows[i].window != None) {
            start_media_player(i);
            usleep(200000); // 200ms entre inicios
        }
    }
    
    // Forzar ventanas al fondo
    sleep(1);
    force_windows_to_background();
    
    if (debug) {
        fprintf(stderr, NAME ": Screen change adaptation complete\n");
    }
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

// Setup compositor integration
static void setup_compositor_integration(void) {
    if (debug) {
        fprintf(stderr, NAME ": Setting up compositor integration to place window below desktop\n");
    }
    
    // Verificar que tenemos ventanas válidas
    if (!config.windows || config.window_count == 0) {
        if (debug) {
            fprintf(stderr, NAME ": No windows to configure\n");
        }
        return;
    }
    
    for (int i = 0; i < config.window_count; i++) {
        if (config.windows[i].window == None) {
            if (debug) {
                fprintf(stderr, NAME ": Skipping invalid window %d\n", i);
            }
            continue;
        }
        
        Window window = config.windows[i].window;
        
        // 1. Establecer tipo de ventana como DESKTOP
        Atom wm_window_type = ATOM(_NET_WM_WINDOW_TYPE);
        if (wm_window_type != None) {
            Atom desktop_type = ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
            XChangeProperty(display, window, wm_window_type,
                          XA_ATOM, 32, PropModeReplace,
                          (unsigned char*)&desktop_type, 1);
        }
        
        // 2. Establecer estado de ventana: BELOW y SKIP_TASKBAR y SKIP_PAGER
        Atom wm_state = ATOM(_NET_WM_STATE);
        if (wm_state != None) {
            Atom states[4];
            int state_count = 0;
            
            Atom below = ATOM(_NET_WM_STATE_BELOW);
            if (below != None) states[state_count++] = below;
            
            Atom skip_taskbar = ATOM(_NET_WM_STATE_SKIP_TASKBAR);
            if (skip_taskbar != None) states[state_count++] = skip_taskbar;
            
            Atom skip_pager = ATOM(_NET_WM_STATE_SKIP_PAGER);
            if (skip_pager != None) states[state_count++] = skip_pager;
            
            Atom sticky = ATOM(_NET_WM_STATE_STICKY);
            if (sticky != None) states[state_count++] = sticky;
            
            if (state_count > 0) {
                XChangeProperty(display, window, wm_state,
                              XA_ATOM, 32, PropModeReplace,
                              (unsigned char*)states, state_count);
            }
        }
        
        // 3. Establecer desktop como -1 (visible en todos los escritorios)
        Atom wm_desktop = ATOM(_NET_WM_DESKTOP);
        if (wm_desktop != None) {
            long desktop = -1; // Todos los escritorios
            XChangeProperty(display, window, wm_desktop,
                          XA_CARDINAL, 32, PropModeReplace,
                          (unsigned char*)&desktop, 1);
        }
        
        // 4. Establecer clase de ventana
        XClassHint class_hint;
        class_hint.res_name = "motionwall";
        class_hint.res_class = "MotionWall";
        XSetClassHint(display, window, &class_hint);
        
        // 5. Establecer nombre de ventana
        XStoreName(display, window, "MotionWall Background");
        
        // 6. Configurar propiedades adicionales para Cinnamon
        if (config.de == DE_CINNAMON) {
            // Intentar hacer la ventana parte del fondo
            Atom muffin_hints = XInternAtom(display, "_MUFFIN_HINTS", False);
            if (muffin_hints != None) {
                const char* hint = "desktop";
                XChangeProperty(display, window, muffin_hints,
                              XA_STRING, 8, PropModeReplace,
                              (unsigned char*)hint, strlen(hint));
            }
        }
        
        // 7. Mover la ventana al fondo usando XLowerWindow
        XLowerWindow(display, window);
        
        // 8. Para Cinnamon y GNOME, también intentar enviar mensaje al WM
        Atom active_window = ATOM(_NET_ACTIVE_WINDOW);
        if (active_window != None) {
            // Enviar mensaje para que NO sea la ventana activa
            XEvent xev;
            memset(&xev, 0, sizeof(xev));
            xev.type = ClientMessage;
            xev.xclient.window = window;
            xev.xclient.message_type = wm_state;
            xev.xclient.format = 32;
            xev.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
            xev.xclient.data.l[1] = ATOM(_NET_WM_STATE_BELOW);
            xev.xclient.data.l[2] = 0;
            xev.xclient.data.l[3] = 1; // Normal application
            xev.xclient.data.l[4] = 0;
            
            XSendEvent(display, DefaultRootWindow(display), False,
                      SubstructureRedirectMask | SubstructureNotifyMask, &xev);
        }
        
        if (debug) {
            fprintf(stderr, NAME ": Configured window %d (0x%lx) for desktop background\n", i, window);
        }
    }
    
    XSync(display, False);
    
    // Pausa adicional para que el WM procese los cambios
    usleep(500000); // 500ms
    
    // Segundo intento de bajar las ventanas
    for (int i = 0; i < config.window_count; i++) {
        if (config.windows[i].window != None) {
            XLowerWindow(display, config.windows[i].window);
        }
    }
    
    XSync(display, False);
    
    if (debug) {
        fprintf(stderr, NAME ": Compositor integration setup complete\n");
    }
}

// Create window for monitor
static void create_window_for_monitor(int monitor_id) {
    if (monitor_id >= config.monitors.count || monitor_id < 0) {
        fprintf(stderr, NAME ": Error: Invalid monitor ID %d\n", monitor_id);
        return;
    }
    
    monitor_info *mon = &config.monitors.monitors[monitor_id];
    window_info *win = &config.windows[monitor_id];
    
    if (debug) {
        fprintf(stderr, NAME ": Creating window for monitor %d: %s (%dx%d+%d+%d)\n",
                monitor_id, mon->name, mon->width, mon->height, mon->x, mon->y);
    }
    
    // Inicializar estructura de ventana
    memset(win, 0, sizeof(window_info));
    win->monitor_id = monitor_id;
    win->x = mon->x;
    win->y = mon->y;
    win->width = mon->width;
    win->height = mon->height;
    win->player_pid = 0;
    win->player_active = false;
    win->player_start_time = 0;
    
    // Usar configuración visual simple y segura
    win->visual = DefaultVisual(display, screen);
    win->colourmap = DefaultColormap(display, screen);
    win->root = DefaultRootWindow(display);
    win->desktop = win->root;
    
    // Configurar atributos de ventana para fondo
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    
    attrs.background_pixel = BlackPixel(display, screen);
    attrs.backing_store = NotUseful;
    attrs.save_under = False;
    attrs.event_mask = StructureNotifyMask | ExposureMask;
    attrs.override_redirect = False;  // False para permitir WM control
    attrs.colormap = win->colourmap;
    
    unsigned long attr_mask = CWBackPixel | CWBackingStore | CWSaveUnder | 
                             CWEventMask | CWOverrideRedirect | CWColormap;
    
    // Crear ventana
    win->window = XCreateWindow(display, win->root, 
                               win->x, win->y, win->width, win->height,
                               0, DefaultDepth(display, screen),
                               InputOutput, win->visual, 
                               attr_mask, &attrs);
    
    if (win->window == None) {
        fprintf(stderr, NAME ": Error: Failed to create window for monitor %d\n", monitor_id);
        return;
    }
    
    // Configurar hints de WM ANTES de mapear
    XSizeHints size_hints;
    size_hints.flags = PPosition | PSize | PMinSize | PMaxSize;
    size_hints.x = win->x;
    size_hints.y = win->y;
    size_hints.width = win->width;
    size_hints.height = win->height;
    size_hints.min_width = win->width;
    size_hints.min_height = win->height;
    size_hints.max_width = win->width;
    size_hints.max_height = win->height;
    XSetWMNormalHints(display, win->window, &size_hints);
    
    // Configurar WM hints
    XWMHints wm_hints;
    wm_hints.flags = InputHint | StateHint;
    wm_hints.input = False;  // No queremos input
    wm_hints.initial_state = NormalState;
    XSetWMHints(display, win->window, &wm_hints);
    
    // Configurar nombre de ventana
    XStoreName(display, win->window, "MotionWall Background");
    
    // Mapear ventana
    XMapWindow(display, win->window);
    
    // Inmediatamente bajar la ventana
    XLowerWindow(display, win->window);
    
    // Sincronizar con servidor X
    XSync(display, False);
    
    if (debug) {
        fprintf(stderr, NAME ": Window created successfully: 0x%lx\n", win->window);
    }
    
    // Pausa para que la ventana se establezca
    usleep(200000); // 200ms
}

// Start media player for specific window - VERSIÓN MEJORADA
static void start_media_player(int window_index) {
    if (window_index < 0 || window_index >= config.window_count) {
        fprintf(stderr, NAME ": Error: Invalid window index %d\n", window_index);
        return;
    }
    
    window_info *win = &config.windows[window_index];
    
    // Verificar si ya hay un reproductor activo para esta ventana
    if (win->player_active && win->player_pid > 0) {
        if (is_process_healthy(win->player_pid)) {
            if (debug) {
                fprintf(stderr, NAME ": Player already active for window %d (PID %d)\n", 
                        window_index, win->player_pid);
            }
            return; // Ya hay un reproductor saludable ejecutándose
        } else {
            // El proceso existe pero no está saludable, terminarlo
            terminate_player(window_index);
            usleep(500000); // 500ms para limpieza
        }
    }
    
    if (config.media_playlist.count == 0) {
        fprintf(stderr, NAME ": Error: No media files in playlist\n");
        return;
    }
    
    if (win->window == None) {
        fprintf(stderr, NAME ": Error: Invalid window for index %d\n", window_index);
        return;
    }
    
    char wid_arg[64];
    char *args[MAX_CMD_ARGS];
    int argc = 0;
    
    int ret = snprintf(wid_arg, sizeof(wid_arg), "0x%lx", win->window);
    if (ret >= (int)sizeof(wid_arg)) {
        fprintf(stderr, NAME ": Error: Window ID too long\n");
        return;
    }
    
    // Build command line
    args[argc++] = config.media_player;
    
    // Add player-specific arguments
   if (strstr(config.media_player, "mpv")) {
       char mpv_wid_arg[64];
       snprintf(mpv_wid_arg, sizeof(mpv_wid_arg), "--wid=0x%lx", win->window);
       args[argc++] = mpv_wid_arg;
       args[argc++] = "--really-quiet";
       args[argc++] = "--no-audio";
       args[argc++] = "--loop-file=inf";
       args[argc++] = "--panscan=1.0";
       args[argc++] = "--keepaspect=no";
       args[argc++] = "--no-input-default-bindings";
       args[argc++] = "--no-osc";
       args[argc++] = "--no-input-cursor";
       args[argc++] = "--no-cursor-autohide";
       args[argc++] = "--hwdec=auto";
       args[argc++] = "--no-terminal";
       args[argc++] = "--no-config";
   } else if (strstr(config.media_player, "mplayer")) {
       args[argc++] = "-wid";
       args[argc++] = wid_arg;
       args[argc++] = "-nosound";
       args[argc++] = "-quiet";
       args[argc++] = "-vo";
       args[argc++] = "xv";
       args[argc++] = "-zoom";
       args[argc++] = "-panscan";
       args[argc++] = "1.0";
       args[argc++] = "-framedrop";
       args[argc++] = "-cache";
       args[argc++] = "8192";
       args[argc++] = "-fs";
       if (config.media_playlist.loop) {
           args[argc++] = "-loop";
           args[argc++] = "0";
       }
   } else if (strstr(config.media_player, "vlc")) {
       char drawable_arg[64];
       snprintf(drawable_arg, sizeof(drawable_arg), "--drawable-xid=0x%lx", win->window);
       
       args[argc++] = "--intf";
       args[argc++] = "dummy";
       args[argc++] = "--no-video-title-show";
       args[argc++] = "--no-audio";
       args[argc++] = "--quiet";
       args[argc++] = "--no-osd";
       args[argc++] = "--no-spu";
       args[argc++] = "--no-stats";
       args[argc++] = "--no-snapshot-preview";
       args[argc++] = "--vout";
       args[argc++] = "x11";
       args[argc++] = drawable_arg;
       args[argc++] = "--no-embedded-video";
       args[argc++] = "--video-on-top";
       args[argc++] = "--fullscreen";
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
   
   // Debug: print the complete command line
   if (debug) {
       fprintf(stderr, NAME ": Starting player for window %d: ", window_index);
       for (int i = 0; i < argc; i++) {
           fprintf(stderr, "%s ", args[i]);
       }
       fprintf(stderr, "\n");
   }
   
   pid_t pid = fork();
   if (pid == 0) {
       // Child process
       // Redirigir stderr y stdout si no estamos en debug
       if (!debug) {
           int devnull = open("/dev/null", O_WRONLY);
           if (devnull != -1) {
               dup2(devnull, STDOUT_FILENO);
               dup2(devnull, STDERR_FILENO);
               close(devnull);
           }
       }
       
       // Establecer nueva sesión para evitar señales del padre
       setsid();
       
       execvp(args[0], args);
       perror(args[0]);
       _exit(2);
   } else if (pid > 0) {
       // Parent process - GESTIÓN MEJORADA
       win->player_pid = pid;
       win->player_active = true;
       win->player_start_time = time(NULL);
       
       if (debug) {
           fprintf(stderr, NAME ": Started %s (PID %d) for window %d with file: %s\n",
                   config.media_player, pid, window_index,
                   config.media_playlist.paths[config.media_playlist.current]);
       }
   } else {
       perror("fork");
       win->player_pid = 0;
       win->player_active = false;
       win->player_start_time = 0;
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
   running = false;
   cleanup_and_exit();
}

// Cleanup and exit - VERSIÓN MEJORADA
static void cleanup_and_exit(void) {
   running = false;
   
   if (debug) {
       fprintf(stderr, NAME ": Cleaning up...\n");
   }
   
   // Terminar todos los reproductores de forma controlada
   terminate_all_players();
   
   // Destruir todas las ventanas
   if (config.windows && display) {
       for (int i = 0; i < config.window_count; i++) {
           if (config.windows[i].window != None) {
               XDestroyWindow(display, config.windows[i].window);
           }
       }
       XSync(display, False);
       free(config.windows);
       config.windows = NULL;
   }
   
   // Cerrar display X11
   if (display) {
       XCloseDisplay(display);
       display = NULL;
   }
   
   // Liberar archivo de lock
   if (lock_fd >= 0) {
       flock(lock_fd, LOCK_UN);
       close(lock_fd);
       unlink("/tmp/motionwall.lock");
       lock_fd = -1;
   }
   
   if (debug) {
       fprintf(stderr, NAME ": Cleanup complete, exiting\n");
   }
   
   exit(0);
}
static void log_monitor_changes(monitor_setup *old_setup, monitor_setup *new_setup) {
    if (!debug) return;
    
    fprintf(stderr, NAME ": Monitor configuration changes:\n");
    
    // Monitores que cambiaron
    for (int i = 0; i < new_setup->count && i < old_setup->count; i++) {
        monitor_info *old_mon = &old_setup->monitors[i];
        monitor_info *new_mon = &new_setup->monitors[i];
        
        if (old_mon->width != new_mon->width || old_mon->height != new_mon->height) {
            fprintf(stderr, "  %s: %dx%d -> %dx%d\n", 
                    new_mon->name, old_mon->width, old_mon->height,
                    new_mon->width, new_mon->height);
        }
        
        if (old_mon->x != new_mon->x || old_mon->y != new_mon->y) {
            fprintf(stderr, "  %s: position %d,%d -> %d,%d\n",
                    new_mon->name, old_mon->x, old_mon->y,
                    new_mon->x, new_mon->y);
        }
    }
    
    // Monitores nuevos
    if (new_setup->count > old_setup->count) {
        for (int i = old_setup->count; i < new_setup->count; i++) {
            monitor_info *mon = &new_setup->monitors[i];
            fprintf(stderr, "  Added: %s (%dx%d+%d+%d)\n",
                    mon->name, mon->width, mon->height, mon->x, mon->y);
        }
    }
    
    // Monitores removidos
    if (old_setup->count > new_setup->count) {
        for (int i = new_setup->count; i < old_setup->count; i++) {
            monitor_info *mon = &old_setup->monitors[i];
            fprintf(stderr, "  Removed: %s (%dx%d+%d+%d)\n",
                    mon->name, mon->width, mon->height, mon->x, mon->y);
        }
    }
}
// Configuration file support
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

// Save configuration file
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
    
    // Verificar soporte XRandR
    if (!XRRQueryExtension(display, &xrandr_event_base, &xrandr_error_base)) {
        fprintf(stderr, NAME ": Warning: XRandR extension not available\n");
        xrandr_event_base = 0;
    } else {
        int major_version, minor_version;
        if (XRRQueryVersion(display, &major_version, &minor_version)) {
            if (debug) {
                fprintf(stderr, NAME ": XRandR %d.%d available\n", major_version, minor_version);
            }
            
            // Registrar interés en eventos de cambio de pantalla
            XRRSelectInput(display, DefaultRootWindow(display), 
                          RRScreenChangeNotifyMask | 
                          RRCrtcChangeNotifyMask |
                          RROutputChangeNotifyMask);
        } else {
            fprintf(stderr, NAME ": Warning: Could not query XRandR version\n");
            xrandr_event_base = 0;
        }
    }
    
    // Configurar manejo de errores X11
    XSetErrorHandler(NULL); // Usar handler por defecto
    
    if (debug) {
        fprintf(stderr, NAME ": X11 initialized successfully\n");
    }
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

// Nueva función para forzar ventanas al fondo
static void force_windows_to_background(void) {
   if (debug) {
       fprintf(stderr, NAME ": Forcing windows to background\n");
   }
   
   for (int i = 0; i < config.window_count; i++) {
       if (config.windows[i].window != None) {
           // Múltiples intentos para bajar la ventana
           for (int attempt = 0; attempt < 3; attempt++) {
               XLowerWindow(display, config.windows[i].window);
               XSync(display, False);
               usleep(100000); // 100ms entre intentos
           }
       }
   }
   
   if (debug) {
       fprintf(stderr, NAME ": Windows forced to background\n");
   }
}

// MAIN FUNCTION COMPLETAMENTE REESCRITA Y MEJORADA
int main(int argc, char **argv) {
   int i;
   bool daemon_mode = false;
   char media_path[MAX_PATH] = {0};
   
   // Initialize configuration with defaults
   memset(&config, 0, sizeof(config));
   strcpy(config.media_player, "mpv");
   config.media_playlist.duration = 30;
   config.media_playlist.shuffle = false;
   config.media_playlist.loop = true;
   config.multi_monitor = false;
   config.auto_resolution = true;
   config.playlist_mode = false;
   config.compositor_aware = false;
   
   // Load default config
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
   
   // Crear lock de instancia única ANTES de hacer cualquier otra cosa
   lock_fd = create_lock_file();
   if (lock_fd < 0) {
       return 1;
   }
   
   // Verificar que el reproductor existe
   if (system(NULL) == 0) {
       fprintf(stderr, NAME ": Error: Shell not available\n");
       return 1;
   }
   
   char test_cmd[512];
   snprintf(test_cmd, sizeof(test_cmd), "which %s >/dev/null 2>&1", config.media_player);
   if (system(test_cmd) != 0) {
       fprintf(stderr, NAME ": Error: Media player '%s' not found\n", config.media_player);
       fprintf(stderr, NAME ": Please install %s or specify another player with -p\n", config.media_player);
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
   srand((unsigned int)time(NULL));
   
   // Set up signal handlers ANTES de inicializar X11
   signal(SIGTERM, signal_handler);
   signal(SIGINT, signal_handler);
   signal(SIGCHLD, SIG_IGN); // Prevent zombie processes
   signal(SIGPIPE, SIG_IGN); // Ignore broken pipe
   
   // Initialize X11
   init_x11();
   
   // Detect desktop environment
   detect_desktop_environment();
   
   // Detect monitors
   if (!detect_monitors()) {
       fprintf(stderr, NAME ": Error: No monitors detected\n");
       cleanup_and_exit();
       return 1;
   }
   
   // Create playlist
   create_playlist(media_path);
   if (config.media_playlist.count == 0) {
       fprintf(stderr, NAME ": Error: No compatible media files found\n");
       cleanup_and_exit();
       return 1;
   }
   
   // Determine how many windows to create
   if (config.multi_monitor) {
       config.window_count = config.monitors.count;
   } else {
       config.window_count = 1;
   }
   
   // Allocate memory for windows
   config.windows = calloc(config.window_count, sizeof(window_info));
   if (!config.windows) {
       fprintf(stderr, NAME ": Error: Memory allocation failed\n");
       cleanup_and_exit();
       return 1;
   }
   
   // Create windows for each monitor (or just primary)
   if (config.multi_monitor) {
       for (i = 0; i < config.monitors.count; i++) {
           create_window_for_monitor(i);
           if (config.windows[i].window == None) {
               fprintf(stderr, NAME ": Failed to create window for monitor %d\n", i);
               cleanup_and_exit();
               return 1;
           }
       }
   } else {
       // Use primary monitor only
       int primary = config.monitors.primary_index;
       if (primary == -1) primary = 0;
       create_window_for_monitor(primary);
       if (config.windows[0].window == None) {
           fprintf(stderr, NAME ": Failed to create primary window\n");
           cleanup_and_exit();
           return 1;
       }
   }
   
   // Setup compositor integration AFTER windows are created
   setup_compositor_integration();
   
   // Small delay to let windows settle
   usleep(500000); // 500ms
   
   // Start media players - UNO POR VENTANA
   for (i = 0; i < config.window_count; i++) {
       start_media_player(i);
       usleep(200000); // 200ms between starts para evitar condiciones de carrera
   }
   
   // Esperar a que los reproductores se establezcan
   sleep(2);
   
   // Forzar ventanas al fondo
   force_windows_to_background();
   
   // Save current configuration
   save_config_file();
   
   if (debug) {
       fprintf(stderr, NAME ": Setup complete. Running with %d window(s) and %d player(s).\n", 
               config.window_count, config.window_count);
   }
   
   // MAIN LOOP COMPLETAMENTE REESCRITO Y SEGURO
   time_t last_change = time(NULL);
   time_t last_check = time(NULL);
   time_t last_health_check = time(NULL);
   int consecutive_errors = 0;
   const int MAX_CONSECUTIVE_ERRORS = 10;
   const int MAX_EVENTS_PER_CYCLE = 5;
   
   running = true;
   
   while (running) {
       time_t now = time(NULL);
       
       // Verificar conexión X11 cada 10 segundos
       if (now - last_check >= 10) {
           if (!display) {
               fprintf(stderr, NAME ": X11 connection lost\n");
               break;
           }
           last_check = now;
       }
       
       // PROCESAMIENTO SEGURO DE EVENTOS X11 CON LÍMITES
       int events_processed = 0;
       
       if (display && XPending(display) > 0) {
           int pending_events = XPending(display);
           
           if (debug && pending_events > 5) {
               fprintf(stderr, NAME ": Processing %d pending events\n", pending_events);
           }
           
           while (XPending(display) && events_processed < MAX_EVENTS_PER_CYCLE && running) {
               XEvent event;
               
               if (XPending(display) == 0) break;
               
               int result = XNextEvent(display, &event);
               if (result != 0) {
                   consecutive_errors++;
                   if (debug) {
                       fprintf(stderr, NAME ": XNextEvent error %d (consecutive: %d)\n", 
                               result, consecutive_errors);
                   }
                   
                   if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                       fprintf(stderr, NAME ": Too many X11 errors, exiting\n");
                       running = false;
                       break;
                   }
                   
                   usleep(10000); // 10ms delay on error
                   continue;
               }
               
               consecutive_errors = 0;
               events_processed++;
               
               // Manejo MÍNIMO de eventos críticos
               switch (event.type) {
                   case DestroyNotify:
                       if (debug) {
                           fprintf(stderr, NAME ": Window destroyed, exiting\n");
                       }
                       running = false;
                       break;
                       
                   case ClientMessage:
                       if (event.xclient.message_type == ATOM(WM_PROTOCOLS)) {
                           if (debug) {
                               fprintf(stderr, NAME ": WM close request received\n");
                           }
                           running = false;
                       }
                       break;
                       
                   case ConfigureNotify:
                       // Solo log en debug
                       if (debug) {
                           fprintf(stderr, NAME ": Window configuration changed\n");
                       }
                       break;
                       
                   // AGREGAR ESTE NUEVO CASO:
                   default:
                       // Verificar eventos XRandR
                       if (xrandr_event_base > 0 && 
                           event.type >= xrandr_event_base &&
                           event.type < xrandr_event_base + RRNumberEvents) {
                           
                           if (debug) {
                               fprintf(stderr, NAME ": XRandR event detected (type: %d)\n", 
                                       event.type - xrandr_event_base);
                           }
                           
                           switch (event.type - xrandr_event_base) {
                               case RRScreenChangeNotify:
                                   if (debug) {
                                       fprintf(stderr, NAME ": Screen configuration changed\n");
                                   }
                                   screen_config_changed = true;
                                   break;
                                   
                               case RRCrtcChangeNotify:
                                   if (debug) {
                                       fprintf(stderr, NAME ": CRTC configuration changed\n");
                                   }
                                   screen_config_changed = true;
                                   break;
                                   
                               case RROutputChangeNotify:
                                   if (debug) {
                                       fprintf(stderr, NAME ": Output configuration changed\n");
                                   }
                                   screen_config_changed = true;
                                   break;
                           }
                       }
                       break;
               }
               
               if (!running) break;
           }
           
           if (display) {
               XFlush(display);
           }
       }
       
       if (!running) break;
       
       // Verificación de salud de reproductores cada 5 segundos
       if (now - last_health_check >= 5) {
           check_and_restart_players();
           last_health_check = now;
       }
        if (screen_config_changed) {
           screen_config_changed = false;
           
           // Esperar un momento para que se estabilice la configuración
           usleep(500000); // 500ms
           
           // Solo proceder si no hemos manejado un cambio reciente
           static time_t last_screen_change = 0;
           if (now - last_screen_change >= 2) { // Al menos 2 segundos entre cambios
               handle_screen_change();
               last_screen_change = now;
           } else if (debug) {
               fprintf(stderr, NAME ": Ignoring rapid screen change event\n");
           }
       }
       
       // Handle playlist changes
       if (config.media_playlist.count > 1 && config.media_playlist.duration > 0) {
           if (now - last_change >= config.media_playlist.duration) {
               if (debug) {
                   fprintf(stderr, NAME ": Time to switch playlist item\n");
               }
               
               // Terminar reproductores existentes de forma controlada
               terminate_all_players();
               
               // Cambiar playlist
               playlist_next();
               
               // Esperar antes de reiniciar
               sleep(2);
               
               // Reiniciar todos con nuevo medio
               for (i = 0; i < config.window_count; i++) {
                   start_media_player(i);
                   usleep(200000); // 200ms between starts
               }
               
               last_change = now;
           }
       }
       
       // SLEEP CRÍTICO para evitar busy waiting
       usleep(500000); // 500ms - sleep más largo para reducir carga del sistema
       
       // Verificación adicional de seguridad
       if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
           fprintf(stderr, NAME ": Too many consecutive errors, exiting for safety\n");
           break;
       }
   }
   
   if (debug) {
       fprintf(stderr, NAME ": Main loop exited, cleaning up\n");
   }
   
   cleanup_and_exit();
   return 0;
}
