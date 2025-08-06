/*
 * MotionWall - Advanced Desktop Background Animation Tool
 * Copyright © 2025 MotionWall Project
 *
 * Based on original xwinwrap code with extensive improvements:
 * - Multi-monitor support with Xrandr
 * - Desktop environment detection and integration
 * - Playlist support for multiple videos
 * - Auto-resolution detection and resize handling
 * - Better compositor integration
 * - Seamless video transitions
 * - Configuration file support
 * - Per-monitor different content support
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
#define VERSION "1.0.2"
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
static int lock_fd = -1;
static int randr_event_base = 0;
static int randr_error_base = 0;

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
    int duration;
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
    pid_t player_pid;
    pid_t fade_pid;  // PID for fade transition process
    bool player_active;
    bool fade_active;
    time_t player_start_time;
    bool needs_resize;
    playlist *monitor_playlist;  // Playlist específica para este monitor
    int playlist_index;         // Índice actual en la playlist del monitor
} window_info;

typedef struct {
    bool multi_monitor;
    bool auto_resolution;
    bool playlist_mode;
    bool compositor_aware;
    bool seamless_transitions;
    bool per_monitor_content;   // Nueva opción para contenido diferente por monitor
    char config_file[MAX_PATH];
    char media_player[256];
    char player_args[1024];
    desktop_environment de;
    playlist media_playlist;
    monitor_setup monitors;
    window_info *windows;
    int window_count;
    char **monitor_paths;       // Array de paths para cada monitor
    int monitor_path_count;
} motionwall_config;

static motionwall_config config = {0};

// Function prototypes
static void init_x11(void);
static void detect_desktop_environment(void);
static int detect_monitors(void);
static bool check_monitor_changes(void);
static void handle_screen_resize(void);
static void resize_window_for_monitor(int window_index, int monitor_id);
static void create_playlist(const char *path);
static void create_monitor_playlists(char **paths, int path_count);
static void setup_compositor_integration(void);
static void create_window_for_monitor(int monitor_id);
static void start_media_player(int window_index);
static void start_fade_transition(int window_index, const char *next_file);
static void check_and_restart_players(void);
static void terminate_all_players(void);
static void terminate_player(int window_index);
static void terminate_fade_process(int window_index);
static bool is_process_healthy(pid_t pid);
static void playlist_next(void);
static void monitor_playlist_next(int window_index);
static void signal_handler(int sig);
static void cleanup_and_exit(void);
static void load_config_file(const char *config_path);
static void save_config_file(void);
static bool safe_path_join(char *dest, size_t dest_size, const char *base, const char *append);
static int create_lock_file(void);
static void force_windows_to_background(void);

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
    
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (write(fd, pid_str, strlen(pid_str)) < 0) {
        if (debug) {
            perror(NAME ": write lock file");
        }
    }
    
    return fd;
}

// Verificar cambios en monitores
static bool check_monitor_changes(void) {
    monitor_setup new_monitors = {0};
    XRRScreenResources *screen_resources;
    XRROutputInfo *output_info;
    XRRCrtcInfo *crtc_info;
    int i;
    bool changed = false;
    
    screen_resources = XRRGetScreenResources(display, DefaultRootWindow(display));
    if (!screen_resources) {
        return false;
    }
    
    // Detectar configuración actual
    for (i = 0; i < screen_resources->noutput && new_monitors.count < MAX_MONITORS; i++) {
        output_info = XRRGetOutputInfo(display, screen_resources, screen_resources->outputs[i]);
        if (!output_info) continue;
        
        if (output_info->connection == RR_Connected && output_info->crtc) {
            crtc_info = XRRGetCrtcInfo(display, screen_resources, output_info->crtc);
            if (crtc_info) {
                monitor_info *mon = &new_monitors.monitors[new_monitors.count];
                
                strncpy(mon->name, output_info->name, sizeof(mon->name) - 1);
                mon->name[sizeof(mon->name) - 1] = '\0';
                mon->x = crtc_info->x;
                mon->y = crtc_info->y;
                mon->width = crtc_info->width;
                mon->height = crtc_info->height;
                mon->connected = true;
                
                RROutput primary = XRRGetOutputPrimary(display, DefaultRootWindow(display));
                mon->primary = (screen_resources->outputs[i] == primary);
                
                if (mon->primary) {
                    new_monitors.primary_index = new_monitors.count;
                }
                
                new_monitors.count++;
                XRRFreeCrtcInfo(crtc_info);
            }
        }
        XRRFreeOutputInfo(output_info);
    }
    
    XRRFreeScreenResources(screen_resources);
    
    if (new_monitors.primary_index == -1 && new_monitors.count > 0) {
        new_monitors.primary_index = 0;
        new_monitors.monitors[0].primary = true;
    }
    
    // Comparar con configuración actual
    if (new_monitors.count != config.monitors.count) {
        changed = true;
    } else {
        for (i = 0; i < new_monitors.count; i++) {
            monitor_info *old_mon = &config.monitors.monitors[i];
            monitor_info *new_mon = &new_monitors.monitors[i];
            
            if (old_mon->x != new_mon->x ||
                old_mon->y != new_mon->y ||
                old_mon->width != new_mon->width ||
                old_mon->height != new_mon->height) {
                changed = true;
                break;
            }
        }
    }
    
    if (changed) {
        if (debug) {
            fprintf(stderr, NAME ": Monitor configuration changed\n");
        }
        config.monitors = new_monitors;
    }
    
    return changed;
}

// Manejar redimensionamiento de pantalla
static void handle_screen_resize(void) {
    if (debug) {
        fprintf(stderr, NAME ": Handling screen resize\n");
    }
    
    // Actualizar información de monitores
    if (!check_monitor_changes()) {
        return;
    }
    
    // Redimensionar ventanas existentes
    for (int i = 0; i < config.window_count && i < config.monitors.count; i++) {
        resize_window_for_monitor(i, i);
    }
    
    // Si tenemos más monitores que ventanas y multi-monitor está habilitado
    if (config.multi_monitor && config.monitors.count > config.window_count) {
        // Crear ventanas adicionales
        config.windows = realloc(config.windows, config.monitors.count * sizeof(window_info));
        if (config.windows) {
            for (int i = config.window_count; i < config.monitors.count; i++) {
                create_window_for_monitor(i);
                if (config.windows[i].window != None) {
                    start_media_player(i);
                }
            }
            config.window_count = config.monitors.count;
        }
    }
    
    // Forzar ventanas al fondo después del redimensionamiento
    usleep(500000);
    force_windows_to_background();
}

// Redimensionar ventana para monitor específico
static void resize_window_for_monitor(int window_index, int monitor_id) {
    if (window_index >= config.window_count || monitor_id >= config.monitors.count) {
        return;
    }
    
    window_info *win = &config.windows[window_index];
    monitor_info *mon = &config.monitors.monitors[monitor_id];
    
    if (win->window == None) {
        return;
    }
    
    if (debug) {
        fprintf(stderr, NAME ": Resizing window %d to match monitor %d (%dx%d+%d+%d)\n",
                window_index, monitor_id, mon->width, mon->height, mon->x, mon->y);
    }
    
    // Actualizar información de la ventana
    win->x = mon->x;
    win->y = mon->y;
    win->width = mon->width;
    win->height = mon->height;
    win->monitor_id = monitor_id;
    
    // Redimensionar y mover la ventana
    XMoveResizeWindow(display, win->window, mon->x, mon->y, mon->width, mon->height);
    
    // Actualizar hints de tamaño
    XSizeHints size_hints;
    size_hints.flags = PPosition | PSize | PMinSize | PMaxSize;
    size_hints.x = mon->x;
    size_hints.y = mon->y;
    size_hints.width = mon->width;
    size_hints.height = mon->height;
    size_hints.min_width = mon->width;
    size_hints.min_height = mon->height;
    size_hints.max_width = mon->width;
    size_hints.max_height = mon->height;
    XSetWMNormalHints(display, win->window, &size_hints);
    
    XSync(display, False);
    win->needs_resize = true;
}

// Verificar si un proceso está saludable
static bool is_process_healthy(pid_t pid) {
    if (pid <= 0) return false;
    
    // Verificar si el proceso existe usando kill(pid, 0)
    if (kill(pid, 0) != 0) {
        if (errno == ESRCH) {
            // Proceso no existe
            return false;
        } else if (errno == EPERM) {
            // Proceso existe pero no tenemos permisos (esto está bien)
            return true;
        }
        // Otros errores, asumir que no está saludable
        return false;
    }
    
    // El proceso existe, ahora verificar si es realmente nuestro reproductor
    char proc_path[256];
    char exe_path[1024];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", pid);
    
    ssize_t len = readlink(proc_path, exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        
        // Verificar si el ejecutable contiene el nombre del reproductor
        char *basename_exe = basename(exe_path);
        if (strstr(basename_exe, "mpv") != NULL || 
            strstr(basename_exe, "mplayer") != NULL || 
            strstr(basename_exe, "vlc") != NULL) {
            return true;
        }
        
        // También verificar la ruta completa
        if (strstr(exe_path, config.media_player) != NULL) {
            return true;
        }
    }
    
    // Como fallback, verificar el comando en /proc/PID/cmdline
    char cmdline_path[256];
    snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);
    
    FILE *cmdline_file = fopen(cmdline_path, "r");
    if (cmdline_file) {
        char cmdline[1024];
        size_t read_len = fread(cmdline, 1, sizeof(cmdline) - 1, cmdline_file);
        fclose(cmdline_file);
        
        if (read_len > 0) {
            cmdline[read_len] = '\0';
            // Los argumentos están separados por null bytes, convertir el primero
            for (size_t i = 0; i < read_len; i++) {
                if (cmdline[i] == '\0') {
                    cmdline[i] = ' ';
                    break;
                }
            }
            
            if (strstr(cmdline, "mpv") != NULL || 
                strstr(cmdline, "mplayer") != NULL || 
                strstr(cmdline, "vlc") != NULL) {
                return true;
            }
        }
    }
    
    // Si llegamos aquí, el proceso existe pero no pudimos verificar que sea nuestro reproductor
    // En caso de duda, asumir que está bien (para evitar reinicios innecesarios)
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
        
        kill(win->player_pid, SIGTERM);
        usleep(300000);
        
        if (kill(win->player_pid, 0) == 0) {
            if (debug) {
                fprintf(stderr, NAME ": Force killing player PID %d\n", win->player_pid);
            }
            kill(win->player_pid, SIGKILL);
        }
        
        waitpid(win->player_pid, NULL, WNOHANG);
        
        win->player_pid = 0;
        win->player_active = false;
        win->player_start_time = 0;
    }
}

// Terminar proceso de transición fade
static void terminate_fade_process(int window_index) {
    if (window_index < 0 || window_index >= config.window_count) {
        return;
    }
    
    window_info *win = &config.windows[window_index];
    
    if (win->fade_active && win->fade_pid > 0) {
        if (debug) {
            fprintf(stderr, NAME ": Terminating fade process PID %d for window %d\n", 
                    win->fade_pid, window_index);
        }
        
        kill(win->fade_pid, SIGTERM);
        usleep(200000);
        
        if (kill(win->fade_pid, 0) == 0) {
            kill(win->fade_pid, SIGKILL);
        }
        
        waitpid(win->fade_pid, NULL, WNOHANG);
        
        win->fade_pid = 0;
        win->fade_active = false;
    }
}

// Terminar todos los reproductores
static void terminate_all_players(void) {
    if (debug) {
        fprintf(stderr, NAME ": Terminating all players\n");
    }
    
    for (int i = 0; i < config.window_count; i++) {
        terminate_player(i);
        terminate_fade_process(i);
    }
    
    usleep(200000);
}

// Iniciar transición suave con fade
static void start_fade_transition(int window_index, const char *next_file) {
    if (!config.seamless_transitions || window_index < 0 || window_index >= config.window_count) {
        return;
    }
    
    window_info *win = &config.windows[window_index];
    
    if (win->fade_active) {
        terminate_fade_process(window_index);
    }
    
    if (win->window == None || !next_file) {
        return;
    }
    
    if (debug) {
        fprintf(stderr, NAME ": Starting fade transition for window %d to: %s\n", 
                window_index, next_file);
    }
    
    // Crear un script temporal para la transición suave
    char script_path[256];
    snprintf(script_path, sizeof(script_path), "/tmp/motionwall_fade_%d.sh", window_index);
    
    FILE *script = fopen(script_path, "w");
    if (!script) {
        if (debug) {
            fprintf(stderr, NAME ": Could not create fade script\n");
        }
        return;
    }
    
    // Script para transición suave usando ffmpeg si está disponible, sino mpv simple
    fprintf(script, "#!/bin/bash\n");
    fprintf(script, "WID=0x%lx\n", win->window);
    fprintf(script, "NEXT_FILE='%s'\n", next_file);
    
    if (strstr(config.media_player, "mpv")) {
        // Usar mpv con fade in/out para transición suave
        fprintf(script, "# Fade out current video\n");
        fprintf(script, "sleep 0.2\n");
        fprintf(script, "# Start new video with fade in\n");
        fprintf(script, "%s --wid=$WID --really-quiet --no-audio --loop-file=inf \\\n", config.media_player);
        fprintf(script, "  --panscan=1.0 --keepaspect=no --no-input-default-bindings \\\n");
        fprintf(script, "  --no-osc --no-input-cursor --no-cursor-autohide \\\n");
        fprintf(script, "  --hwdec=auto --no-terminal --no-config \\\n");
        fprintf(script, "  --vf=fade=in:0:10 \\\n");  // Fade in effect
        fprintf(script, "  \"$NEXT_FILE\" &\n");
        fprintf(script, "NEW_PID=$!\n");
        fprintf(script, "echo $NEW_PID > /tmp/motionwall_new_pid_%d\n", window_index);
    } else {
        // Fallback para otros reproductores
        fprintf(script, "sleep 0.3\n");
        fprintf(script, "%s", config.media_player);
        if (strstr(config.media_player, "mplayer")) {
            fprintf(script, " -wid 0x%lx -nosound -quiet -vo xv -zoom -panscan 1.0 -framedrop -cache 8192 -fs -loop 0", win->window);
        } else if (strstr(config.media_player, "vlc")) {
            fprintf(script, " --drawable-xid=0x%lx --intf dummy --no-video-title-show --no-audio --quiet --no-osd --loop", win->window);
        }
        fprintf(script, " \"$NEXT_FILE\" &\n");
        fprintf(script, "NEW_PID=$!\n");
        fprintf(script, "echo $NEW_PID > /tmp/motionwall_new_pid_%d\n", window_index);
    }
    
    fclose(script);
    chmod(script_path, 0755);
    
    // Ejecutar el script de transición
    pid_t fade_pid = fork();
    if (fade_pid == 0) {
        if (!debug) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull != -1) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        
        setsid();
        execl("/bin/bash", "bash", script_path, NULL);
        _exit(2);
    } else if (fade_pid > 0) {
        win->fade_pid = fade_pid;
        win->fade_active = true;
        
        // Esperar a que la transición se complete
        sleep(1);
        
        // Leer el PID del nuevo reproductor
        char pid_file[256];
        snprintf(pid_file, sizeof(pid_file), "/tmp/motionwall_new_pid_%d", window_index);
        
        FILE *pf = fopen(pid_file, "r");
        if (pf) {
            pid_t new_pid = 0;
            if (fscanf(pf, "%d", &new_pid) == 1 && new_pid > 0) {
                // Terminar reproductor anterior
                if (win->player_active && win->player_pid > 0) {
                    terminate_player(window_index);
                }
                
                // Establecer nuevo reproductor
                win->player_pid = new_pid;
                win->player_active = true;
                win->player_start_time = time(NULL);
                
                if (debug) {
                    fprintf(stderr, NAME ": Fade transition completed, new player PID: %d\n", new_pid);
                }
            }
            fclose(pf);
            unlink(pid_file);
        }
        
        // Limpiar transición
        terminate_fade_process(window_index);
        unlink(script_path);
    }
}

// Verificar y reiniciar reproductores muertos
static void check_and_restart_players(void) {
    time_t now = time(NULL);
    
    for (int i = 0; i < config.window_count; i++) {
        window_info *win = &config.windows[i];
        
        // Verificar reproductor principal
        if (win->player_active && win->player_pid > 0) {
            // Dar tiempo al proceso para iniciarse completamente
            if (win->player_start_time > 0 && (now - win->player_start_time) < 5) {
                if (debug) {
                    fprintf(stderr, NAME ": Player for window %d is still starting, skipping health check\n", i);
                }
                continue;
            }
            
            if (!is_process_healthy(win->player_pid)) {
                if (debug) {
                    fprintf(stderr, NAME ": Player for window %d (PID %d) is unhealthy or dead\n", 
                            i, win->player_pid);
                }
                
                waitpid(win->player_pid, NULL, WNOHANG);
                win->player_pid = 0;
                win->player_active = false;
                win->player_start_time = 0;
                
                sleep(1);
                
                if (debug) {
                    fprintf(stderr, NAME ": Restarting player for window %d\n", i);
                }
                start_media_player(i);
            } else if (win->needs_resize) {
                // Reiniciar reproductor si la ventana fue redimensionada
                if (debug) {
                    fprintf(stderr, NAME ": Restarting player for window %d due to resize\n", i);
                }
                terminate_player(i);
                sleep(1);
                start_media_player(i);
                win->needs_resize = false;
            }
        } else if (!win->player_active && win->window != None) {
            if (debug) {
                fprintf(stderr, NAME ": Window %d has no active player, starting one\n", i);
            }
            start_media_player(i);
        }
        
        // Verificar timeout del reproductor (aumentado a 10 minutos)
        if (win->player_active && win->player_start_time > 0) {
            if (now - win->player_start_time > 600) { // 10 minutos
                if (debug) {
                    fprintf(stderr, NAME ": Player for window %d running too long, checking health\n", i);
                }
                
                if (!is_process_healthy(win->player_pid)) {
                    terminate_player(i);
                    usleep(500000);
                    start_media_player(i);
                }
                
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

// Crear playlists específicas para cada monitor
static void create_monitor_playlists(char **paths, int path_count) {
    if (!config.per_monitor_content || !paths || path_count == 0) {
        return;
    }
    
    if (debug) {
        fprintf(stderr, NAME ": Creating per-monitor playlists for %d monitors\n", config.monitors.count);
    }
    
    for (int monitor = 0; monitor < config.monitors.count && monitor < config.window_count; monitor++) {
        window_info *win = &config.windows[monitor];
       
       // Asignar playlist específica para este monitor
       win->monitor_playlist = malloc(sizeof(playlist));
       if (!win->monitor_playlist) {
           fprintf(stderr, NAME ": Error: Could not allocate playlist for monitor %d\n", monitor);
           continue;
       }
       
       memset(win->monitor_playlist, 0, sizeof(playlist));
       
       // Usar el path correspondiente al monitor (circular si hay menos paths que monitores)
       const char *monitor_path = paths[monitor % path_count];
       
       struct stat path_stat;
       if (stat(monitor_path, &path_stat) != 0) {
           fprintf(stderr, NAME ": Error: Cannot access path for monitor %d: %s\n", monitor, monitor_path);
           free(win->monitor_playlist);
           win->monitor_playlist = NULL;
           continue;
       }
       
       if (S_ISDIR(path_stat.st_mode)) {
           // Directory - find all video files
           glob_t glob_result;
           const char *extensions[] = {"*.mp4", "*.avi", "*.mkv", "*.mov", "*.webm", "*.gif", "*.mp3", "*.wav"};
           const int ext_count = sizeof(extensions) / sizeof(extensions[0]);
           
           for (int ext = 0; ext < ext_count; ext++) {
               char pattern[MAX_PATH];
               if (!safe_path_join(pattern, sizeof(pattern), monitor_path, extensions[ext])) {
                   continue;
               }
               
               if (glob(pattern, (ext == 0) ? 0 : GLOB_APPEND, NULL, &glob_result) == 0) {
                   for (int i = 0; i < (int)glob_result.gl_pathc && win->monitor_playlist->count < MAX_PLAYLIST; i++) {
                       strncpy(win->monitor_playlist->paths[win->monitor_playlist->count], 
                              glob_result.gl_pathv[i], MAX_PATH - 1);
                       win->monitor_playlist->paths[win->monitor_playlist->count][MAX_PATH - 1] = '\0';
                       win->monitor_playlist->count++;
                   }
               }
           }
           globfree(&glob_result);
       } else {
           // Single file
           strncpy(win->monitor_playlist->paths[0], monitor_path, MAX_PATH - 1);
           win->monitor_playlist->paths[0][MAX_PATH - 1] = '\0';
           win->monitor_playlist->count = 1;
       }
       
       // Configurar playlist
       win->monitor_playlist->current = 0;
       win->monitor_playlist->duration = config.media_playlist.duration;
       win->monitor_playlist->shuffle = config.media_playlist.shuffle;
       win->monitor_playlist->loop = config.media_playlist.loop;
       win->playlist_index = 0;
       
       if (debug) {
           fprintf(stderr, NAME ": Monitor %d playlist created with %d items from: %s\n", 
                   monitor, win->monitor_playlist->count, monitor_path);
       }
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
       fprintf(stderr, NAME ": Created main playlist with %d items\n", config.media_playlist.count);
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
       
       // 2. Establecer estado de ventana
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
           long desktop = -1;
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
           Atom muffin_hints = XInternAtom(display, "_MUFFIN_HINTS", False);
           if (muffin_hints != None) {
               const char* hint = "desktop";
               XChangeProperty(display, window, muffin_hints,
                             XA_STRING, 8, PropModeReplace,
                             (unsigned char*)hint, strlen(hint));
           }
       }
       
       // 7. Mover la ventana al fondo
       XLowerWindow(display, window);
       
       if (debug) {
           fprintf(stderr, NAME ": Configured window %d (0x%lx) for desktop background\n", i, window);
       }
   }
   
   XSync(display, False);
   usleep(500000);
   
   // Segundo intento
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
   win->fade_pid = 0;
   win->player_active = false;
   win->fade_active = false;
   win->player_start_time = 0;
   win->needs_resize = false;
   win->monitor_playlist = NULL;
   win->playlist_index = 0;
   
   win->visual = DefaultVisual(display, screen);
   win->colourmap = DefaultColormap(display, screen);
   win->root = DefaultRootWindow(display);
   win->desktop = win->root;
   
   // Configurar atributos de ventana
   XSetWindowAttributes attrs;
   memset(&attrs, 0, sizeof(attrs));
   
   attrs.background_pixel = BlackPixel(display, screen);
   attrs.backing_store = NotUseful;
   attrs.save_under = False;
   attrs.event_mask = StructureNotifyMask | ExposureMask;
   attrs.override_redirect = False;
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
   
   // Configurar hints de WM
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
   
   XWMHints wm_hints;
   wm_hints.flags = InputHint | StateHint;
   wm_hints.input = False;
   wm_hints.initial_state = NormalState;
   XSetWMHints(display, win->window, &wm_hints);
   
   XStoreName(display, win->window, "MotionWall Background");
   
   // Mapear ventana
   XMapWindow(display, win->window);
   XLowerWindow(display, win->window);
   XSync(display, False);
   
   if (debug) {
       fprintf(stderr, NAME ": Window created successfully: 0x%lx\n", win->window);
   }
   
   usleep(200000);
}

// Start media player for specific window
static void start_media_player(int window_index) {
   if (window_index < 0 || window_index >= config.window_count) {
       fprintf(stderr, NAME ": Error: Invalid window index %d\n", window_index);
       return;
   }
   
   window_info *win = &config.windows[window_index];
   
   // Verificar si ya hay un reproductor activo
   if (win->player_active && win->player_pid > 0) {
       if (is_process_healthy(win->player_pid)) {
           if (debug) {
               fprintf(stderr, NAME ": Player already active for window %d (PID %d)\n", 
                       window_index, win->player_pid);
           }
           return;
       } else {
           terminate_player(window_index);
           usleep(500000);
       }
   }
   
   // Determinar qué playlist usar y obtener el archivo actual
   const char *current_file = NULL;
   
   if (config.per_monitor_content && win->monitor_playlist && win->monitor_playlist->count > 0) {
       // Usar playlist específica del monitor
       current_file = win->monitor_playlist->paths[win->monitor_playlist->current];
       if (debug) {
           fprintf(stderr, NAME ": Using monitor-specific file for window %d: %s\n", 
                   window_index, current_file);
       }
   } else if (config.media_playlist.count > 0) {
       // Usar playlist global
       current_file = config.media_playlist.paths[config.media_playlist.current];
       if (debug) {
           fprintf(stderr, NAME ": Using global playlist file for window %d: %s\n", 
                   window_index, current_file);
       }
   }
   
   if (!current_file) {
       fprintf(stderr, NAME ": Error: No media files available for window %d\n", window_index);
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
       args[argc++] = "--geometry";
       char geometry[64];
       snprintf(geometry, sizeof(geometry), "%dx%d+%d+%d", 
               win->width, win->height, win->x, win->y);
       args[argc++] = geometry;
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
       args[argc++] = "-loop";
       args[argc++] = "0";
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
       args[argc++] = "--loop";
   }
   
   // Add current media file
   if (argc < MAX_CMD_ARGS - 1) {
       args[argc++] = (char*)current_file;
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
       if (!debug) {
           int devnull = open("/dev/null", O_WRONLY);
           if (devnull != -1) {
               dup2(devnull, STDOUT_FILENO);
               dup2(devnull, STDERR_FILENO);
               close(devnull);
           }
       }
       
       setsid();
       execvp(args[0], args);
       perror(args[0]);
       _exit(2);
   } else if (pid > 0) {
        // Parent process
        win->player_pid = pid;
        win->player_active = true;
        win->player_start_time = time(NULL);
        
        if (debug) {
            fprintf(stderr, NAME ": Started %s (PID %d) for window %d with file: %s\n",
                    config.media_player, pid, window_index, current_file);
            fprintf(stderr, NAME ": Waiting 3 seconds for player to initialize...\n");
        }
        
        // Dar tiempo al reproductor para inicializarse antes de verificar salud
        sleep(3);
        
        // Verificar una vez que el proceso se haya iniciado correctamente
        if (!is_process_healthy(pid)) {
            if (debug) {
                fprintf(stderr, NAME ": Warning: Player PID %d may not have started correctly\n", pid);
            }
            // No terminamos inmediatamente, daremos otra oportunidad en el próximo ciclo
        }
    } else {
        perror("fork");
        win->player_pid = 0;
        win->player_active = false;
        win->player_start_time = 0;
    }
}

// Playlist management para playlist global
static void playlist_next(void) {
   if (config.media_playlist.count <= 1) return;
   
   if (config.media_playlist.shuffle) {
       config.media_playlist.current = rand() % config.media_playlist.count;
   } else {
       config.media_playlist.current = (config.media_playlist.current + 1) % config.media_playlist.count;
   }
   
   if (debug) {
       fprintf(stderr, NAME ": Global playlist switching to: %s\n", 
               config.media_playlist.paths[config.media_playlist.current]);
   }
}

// Playlist management para monitor específico
static void monitor_playlist_next(int window_index) {
   if (window_index < 0 || window_index >= config.window_count) {
       return;
   }
   
   window_info *win = &config.windows[window_index];
   
   if (!win->monitor_playlist || win->monitor_playlist->count <= 1) {
       return;
   }
   
   if (win->monitor_playlist->shuffle) {
       win->monitor_playlist->current = rand() % win->monitor_playlist->count;
   } else {
       win->monitor_playlist->current = (win->monitor_playlist->current + 1) % win->monitor_playlist->count;
   }
   
   if (debug) {
       fprintf(stderr, NAME ": Monitor %d playlist switching to: %s\n", 
               window_index, win->monitor_playlist->paths[win->monitor_playlist->current]);
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

// Cleanup and exit
static void cleanup_and_exit(void) {
   running = false;
   
   if (debug) {
       fprintf(stderr, NAME ": Cleaning up...\n");
   }
   
   terminate_all_players();
   
   if (config.windows && display) {
       for (int i = 0; i < config.window_count; i++) {
           if (config.windows[i].window != None) {
               XDestroyWindow(display, config.windows[i].window);
           }
           
           // Liberar playlist específica del monitor
           if (config.windows[i].monitor_playlist) {
               free(config.windows[i].monitor_playlist);
               config.windows[i].monitor_playlist = NULL;
           }
       }
       XSync(display, False);
       free(config.windows);
       config.windows = NULL;
   }
   
   // Liberar paths de monitores
   if (config.monitor_paths) {
       for (int i = 0; i < config.monitor_path_count; i++) {
           if (config.monitor_paths[i]) {
               free(config.monitor_paths[i]);
           }
       }
       free(config.monitor_paths);
       config.monitor_paths = NULL;
   }
   
   if (display) {
       XCloseDisplay(display);
       display = NULL;
   }
   
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

// Configuration file support
static void load_config_file(const char *config_path) {
   FILE *file = fopen(config_path, "r");
   if (!file) return;
   
   char line[1024];
   while (fgets(line, sizeof(line), file) != NULL) {
       line[strcspn(line, "\n")] = '\0';
       
       if (line[0] == '#' || line[0] == '\0') continue;
       
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
       } else if (strcmp(key, "seamless_transitions") == 0) {
           config.seamless_transitions = (strcmp(value, "true") == 0);
       } else if (strcmp(key, "per_monitor_content") == 0) {
           config.per_monitor_content = (strcmp(value, "true") == 0);
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
   
   if (!safe_path_join(config_dir, sizeof(config_dir), home, CONFIG_DIR)) {
       fprintf(stderr, NAME ": Error: Config directory path too long\n");
       return;
   }
   
   if (mkdir(config_dir, 0755) != 0 && errno != EEXIST) {
       if (debug) {
           perror(NAME ": mkdir config_dir");
       }
   }
   
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
   fprintf(file, "seamless_transitions=%s\n", config.seamless_transitions ? "true" : "false");
   fprintf(file, "per_monitor_content=%s\n", config.per_monitor_content ? "true" : "false");
   
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
   
   // Configurar extensión Xrandr para detección de cambios de pantalla
   if (XRRQueryExtension(display, &randr_event_base, &randr_error_base)) {
       XRRSelectInput(display, DefaultRootWindow(display), RRScreenChangeNotifyMask);
       if (debug) {
           fprintf(stderr, NAME ": Xrandr extension initialized for screen change detection\n");
       }
   } else {
       if (debug) {
           fprintf(stderr, NAME ": Warning: Xrandr extension not available\n");
       }
   }
   
   XSetErrorHandler(NULL);
   
   if (debug) {
       fprintf(stderr, NAME ": X11 initialized successfully\n");
   }
}

// Usage information
static void usage(void) {
   fprintf(stderr, "%s v%s - Advanced Desktop Background Animation Tool\n", NAME, VERSION);
   fprintf(stderr, "\nUsage: %s [OPTIONS] <media-file-or-directory> [monitor2-path] [monitor3-path] ...\n\n", NAME);
   fprintf(stderr, "Options:\n");
   fprintf(stderr, "  -m, --multi-monitor    Enable multi-monitor support\n");
   fprintf(stderr, "  -p, --player PLAYER    Media player to use (mpv, mplayer, vlc)\n");
   fprintf(stderr, "  -s, --shuffle          Shuffle playlist\n");
   fprintf(stderr, "  -l, --loop             Loop playlist\n");
   fprintf(stderr, "  -d, --duration SEC     Duration per video in playlist (enables transitions)\n");
   fprintf(stderr, "  -c, --config FILE      Use custom config file\n");
   fprintf(stderr, "  --smooth               Enable smooth video transitions (no black screen)\n");
   fprintf(stderr, "  --per-monitor          Different content for each monitor (use with -m)\n");
   fprintf(stderr, "  --auto-res             Auto-detect and use native resolution\n");
   fprintf(stderr, "  --daemon               Run as daemon\n");
   fprintf(stderr, "  --debug                Enable debug output\n");
   fprintf(stderr, "  -h, --help             Show this help\n");
   fprintf(stderr, "\nExamples:\n");
   fprintf(stderr, "  %s video.mp4                    # Single video\n", NAME);
   fprintf(stderr, "  %s -m ~/Videos/                 # Multi-monitor with same content\n", NAME);
   fprintf(stderr, "  %s -m --per-monitor ~/Videos1/ ~/Videos2/ # Different content per monitor\n", NAME);
   fprintf(stderr, "  %s -d 30 --smooth ~/Videos/     # 30 second duration with smooth transitions\n", NAME);
   fprintf(stderr, "  %s -m -d 10 --smooth --per-monitor ~/Vid1/ ~/Vid2/ # Per-monitor with smooth transitions\n", NAME);
}

// Forzar ventanas al fondo
static void force_windows_to_background(void) {
   if (debug) {
       fprintf(stderr, NAME ": Forcing windows to background\n");
   }
   
   for (int i = 0; i < config.window_count; i++) {
       if (config.windows[i].window != None) {
           for (int attempt = 0; attempt < 3; attempt++) {
               XLowerWindow(display, config.windows[i].window);
               XSync(display, False);
               usleep(100000);
           }
       }
   }
   
   if (debug) {
       fprintf(stderr, NAME ": Windows forced to background\n");
   }
}

// MAIN FUNCTION
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
   config.seamless_transitions = false;
   config.per_monitor_content = false;
   config.monitor_paths = NULL;
   config.monitor_path_count = 0;
   
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
               if (config.media_playlist.duration > 0) {
                   config.playlist_mode = true;
               }
           }
       } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
           if (++i < argc) {
               load_config_file(argv[i]);
           }
       } else if (strcmp(argv[i], "--smooth") == 0) {
           config.seamless_transitions = true;
       } else if (strcmp(argv[i], "--per-monitor") == 0) {
           config.per_monitor_content = true;
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
           // Esto es un path de media
           if (strlen(media_path) == 0) {
               // Primer path (principal)
               strncpy(media_path, argv[i], sizeof(media_path) - 1);
               media_path[sizeof(media_path) - 1] = '\0';
           } else if (config.per_monitor_content) {
               // Paths adicionales para monitores específicos
               config.monitor_paths = realloc(config.monitor_paths, 
                                             (config.monitor_path_count + 1) * sizeof(char*));
               if (config.monitor_paths) {
                   config.monitor_paths[config.monitor_path_count] = malloc(strlen(argv[i]) + 1);
                   if (config.monitor_paths[config.monitor_path_count]) {
                       strcpy(config.monitor_paths[config.monitor_path_count], argv[i]);
                       config.monitor_path_count++;
                   }
               }
           }
       }
   }
   
   if (strlen(media_path) == 0) {
       fprintf(stderr, NAME ": Error: No media file or directory specified\n");
       usage();
       return 1;
   }
   
   // Si se especifica per-monitor pero no hay paths adicionales, usar el principal para todos
   if (config.per_monitor_content && config.monitor_path_count == 0) {
       config.monitor_paths = malloc(sizeof(char*));
       if (config.monitor_paths) {
           config.monitor_paths[0] = malloc(strlen(media_path) + 1);
           if (config.monitor_paths[0]) {
               strcpy(config.monitor_paths[0], media_path);
               config.monitor_path_count = 1;
           }
       }
   }
   
   // Crear lock de instancia única
   lock_fd = create_lock_file();
   if (lock_fd < 0) {
       return 1;
   }
   
   // Verificar reproductor
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
   
   // Verificar soporte para transiciones suaves
   if (config.seamless_transitions && !strstr(config.media_player, "mpv")) {
       fprintf(stderr, NAME ": Warning: Smooth transitions work best with MPV\n");
       fprintf(stderr, NAME ": Continuing with basic transitions for %s\n", config.media_player);
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
   
   // Initialize random seed
   srand((unsigned int)time(NULL));
   
   // Set up signal handlers
   signal(SIGTERM, signal_handler);
   signal(SIGINT, signal_handler);
   signal(SIGCHLD, SIG_IGN);
   signal(SIGPIPE, SIG_IGN);
   
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
   
   // Create playlist(s)
   if (config.per_monitor_content && config.monitor_path_count > 0) {
       // Agregar el path principal a la lista si no está ya
       bool main_path_in_list = false;
       for (int i = 0; i < config.monitor_path_count; i++) {
           if (strcmp(config.monitor_paths[i], media_path) == 0) {
               main_path_in_list = true;
               break;
           }
       }
       
       if (!main_path_in_list) {
           config.monitor_paths = realloc(config.monitor_paths, 
                                         (config.monitor_path_count + 1) * sizeof(char*));
           if (config.monitor_paths) {
               // Insertar al principio
               memmove(&config.monitor_paths[1], &config.monitor_paths[0], 
                       config.monitor_path_count * sizeof(char*));
               config.monitor_paths[0] = malloc(strlen(media_path) + 1);
               if (config.monitor_paths[0]) {
                   strcpy(config.monitor_paths[0], media_path);
                   config.monitor_path_count++;
               }
           }
       }
       
       if (debug) {
           fprintf(stderr, NAME ": Per-monitor mode with %d paths:\n", config.monitor_path_count);
           for (int i = 0; i < config.monitor_path_count; i++) {
               fprintf(stderr, "  Monitor %d: %s\n", i, config.monitor_paths[i]);
           }
       }
   } else {
       // Crear playlist global
       create_playlist(media_path);
       if (config.media_playlist.count == 0) {
           fprintf(stderr, NAME ": Error: No compatible media files found\n");
           cleanup_and_exit();
           return 1;
       }
   }
   
   // Determine window count
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
   
   // Create windows
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
       int primary = config.monitors.primary_index;
       if (primary == -1) primary = 0;
       create_window_for_monitor(primary);
       if (config.windows[0].window == None) {
           fprintf(stderr, NAME ": Failed to create primary window\n");
           cleanup_and_exit();
           return 1;
       }
   }
   
   // Setup per-monitor playlists if needed
   if (config.per_monitor_content && config.monitor_path_count > 0) {
       create_monitor_playlists(config.monitor_paths, config.monitor_path_count);
   }
   
   // Setup compositor integration
   setup_compositor_integration();
   
   // Delay para que las ventanas se establezcan
   usleep(500000);
   
   // Start media players
   for (i = 0; i < config.window_count; i++) {
       start_media_player(i);
       usleep(200000);
   }
   
   // Esperar establecimiento de reproductores
   sleep(2);
   
   // Force windows to background
   force_windows_to_background();
   
   // Save configuration
   save_config_file();
   
   if (debug) {
       fprintf(stderr, NAME ": Setup complete. Running with %d window(s) and %d player(s).\n", 
               config.window_count, config.window_count);
       if (config.seamless_transitions) {
           fprintf(stderr, NAME ": Smooth transitions enabled\n");
       }
       if (config.per_monitor_content) {
           fprintf(stderr, NAME ": Per-monitor content enabled\n");
       }
       if (config.playlist_mode) {
           fprintf(stderr, NAME ": Playlist mode with %d second duration\n", config.media_playlist.duration);
       }
   }
   
   // MAIN LOOP MEJORADO
   time_t last_change = time(NULL);
   time_t last_check = time(NULL);
   time_t last_health_check = time(NULL);
   time_t last_screen_check = time(NULL);
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
       
       // PROCESAMIENTO DE EVENTOS X11
       if (display && XPending(display) > 0) {
           int events_processed = 0;
           
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
                   
                   usleep(10000);
                   continue;
               }
               
               consecutive_errors = 0;
               events_processed++;
               
               // Manejo de eventos
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
                       if (debug) {
                           fprintf(stderr, NAME ": Window configuration changed\n");
                       }
                       break;
                       
                   default:
                       // Verificar si es evento de Xrandr (cambio de resolución)
                       if (randr_event_base > 0 && 
                           event.type == randr_event_base + RRScreenChangeNotify) {
                           if (debug) {
                               fprintf(stderr, NAME ": Screen change detected via Xrandr\n");
                           }
                           handle_screen_resize();
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
       
       // Verificar cambios de pantalla cada 5 segundos (fallback)
       if (now - last_screen_check >= 5) {
           if (check_monitor_changes()) {
               handle_screen_resize();
           }
           last_screen_check = now;
       }
       
       // Verificación de salud de reproductores cada 3 segundos
       if (now - last_health_check >= 10) {
           check_and_restart_players();
           last_health_check = now;
       }
       
       // Handle playlist changes CON TRANSICIONES SUAVES MEJORADAS
       if (config.playlist_mode && config.media_playlist.duration > 0) {
           if (now - last_change >= config.media_playlist.duration) {
               if (debug) {
                   fprintf(stderr, NAME ": Time to switch playlist item\n");
               }
               
               if (config.seamless_transitions) {
                   // TRANSICIONES SUAVES MEJORADAS
                   for (i = 0; i < config.window_count; i++) {
                       window_info *win = &config.windows[i];
                       const char *next_file = NULL;
                       
                       // Determinar siguiente archivo
                       if (config.per_monitor_content && win->monitor_playlist && win->monitor_playlist->count > 1) {
                           // Avanzar playlist del monitor
                           monitor_playlist_next(i);
                           next_file = win->monitor_playlist->paths[win->monitor_playlist->current];
                       } else if (config.media_playlist.count > 1) {
                           // Calcular siguiente archivo de playlist global
                           int next_index;
                           if (config.media_playlist.shuffle) {
                               next_index = rand() % config.media_playlist.count;
                           } else {
                               next_index = (config.media_playlist.current + 1) % config.media_playlist.count;
                           }
                           next_file = config.media_playlist.paths[next_index];
                       }
                       
                       if (next_file) {
                           if (debug) {
                               fprintf(stderr, NAME ": Starting smooth transition for window %d to: %s\n", i, next_file);
                           }
                           start_fade_transition(i, next_file);
                       }
                   }
                   
                   // Actualizar playlist global si no es per-monitor
                   if (!config.per_monitor_content) {
                       playlist_next();
                   }
               } else {
                   // TRANSICIONES NORMALES (rápidas pero pueden tener parpadeo)
                   terminate_all_players();
                   
                   // Avanzar playlists
                   if (config.per_monitor_content) {
                       for (i = 0; i < config.window_count; i++) {
                           monitor_playlist_next(i);
                       }
                   } else {
                       playlist_next();
                   }
                   
                   sleep(1); // Breve pausa
                   
                   for (i = 0; i < config.window_count; i++) {
                       start_media_player(i);
                       usleep(200000);
                   }
               }
               
               last_change = now;
           }
       }
       
       // SLEEP inteligente
       if (config.playlist_mode && config.media_playlist.duration > 0) {
           time_t time_until_change = config.media_playlist.duration - (now - last_change);
           if (time_until_change <= 5) {
               usleep(200000); // 200ms durante los últimos 5 segundos
           } else {
               usleep(1000000); // 1 segundo normalmente
           }
       } else {
           usleep(1000000); // 1 segundo por defecto
       }
       
       // Verificación de seguridad
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
