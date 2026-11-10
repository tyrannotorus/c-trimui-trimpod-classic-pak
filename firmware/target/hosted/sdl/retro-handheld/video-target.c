/***************************************************************************
 * Trimpod: runs TrimUI's stock hardware video player (tplayerslave) as a child.
 *
 * The player decodes to a display layer below the app window and takes commands
 * through a FIFO.  Plugins from trimpod/video extend it: libpassthrua.so (AC-3,
 * found on LD_LIBRARY_PATH), and libdivx_shim.so (DivX) and the sound's route
 * to Trimpod (video-audio.c), both preloaded.
 ****************************************************************************/
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>

#include "config.h"
#include "window-sdl.h"
#include "video-audio.h"
#include "video-target.h"

#define PLAYER_BIN  "/usr/bin/tplayerslave"
#define PROBE_BIN   "/usr/bin/ffprobe"
#define PLAYER_DIR  "/tmp/trimui_player"
#define CMD_FIFO    PLAYER_DIR "/cmd"
#define STATUS_FILE PLAYER_DIR "/status"
#define PLUGIN_DIR  ROCKBOX_DIR "/video"
#define ENV_MAX     128
#define PROBE_BYTES 8192

/* Wait up to two seconds for "exit" to work. */
#define STOP_POLL_US 20000
#define STOP_POLLS   100

/* Wait up to a second for the player's FIFO. */
#define OPEN_POLL_US 10000
#define OPEN_POLLS   100

/* The picture is display-engine channel 0, layer 0, and only the player's own
 * exit takes it down.  Left up after any other death, it reads freed buffers and
 * within seconds the engine stops producing frames until a reboot.  Switched off
 * at once, the engine recovers.  An all-zero layer config is that layer, off. */
#define DISP_DEV              "/dev/disp"
#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_CONFIG_BYTES     512       /* more than struct disp_layer_config */
#define DISP_LATCH_US         50000     /* a config applies at a vsync */

/* SIGSTOP lands as each thread leaves the kernel. */
#define FREEZE_POLL_US  5000
#define FREEZE_POLLS    20

extern char **environ;

static pid_t player_pid = -1;

static bool player_command(const char *fmt, ...);

/* Runs in the forked child, so only async-signal-safe calls. */
static void child_exec(const char *bin, char *const argv[], char *const envp[],
                       int out_fd, long maxfd)
{
    dup2(out_fd, STDOUT_FILENO);
    dup2(out_fd, STDERR_FILENO);
    for (long fd = 3; fd < maxfd; fd++)
        close(fd);
    execve(bin, argv, envp);
    _exit(127);
}

bool retrohh_video_probe(const char *path, struct retrohh_video_info *info)
{
    static char output[PROBE_BYTES];
    char *const argv[] = {
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
        "stream=codec_name,width,height,display_aspect_ratio:chapter=start_time",
        "-of", "default=nw=1", (char *)path, NULL
    };
    long maxfd = sysconf(_SC_OPEN_MAX);
    int fds[2], used = 0;
    ssize_t got;
    pid_t pid;

    memset(info, 0, sizeof *info);
    if (pipe(fds) < 0)
        return false;
    pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0)
        child_exec(PROBE_BIN, argv, environ, fds[1], maxfd);

    close(fds[1]);
    while (used < PROBE_BYTES - 1 &&
           (got = read(fds[0], output + used, PROBE_BYTES - 1 - used)) > 0)
        used += got;
    close(fds[0]);
    waitpid(pid, NULL, 0);
    output[used] = '\0';
    if (!used)
        return false;

    for (char *line = strtok(output, "\n"); line; line = strtok(NULL, "\n"))
    {
        if (!strncmp(line, "codec_name=", 11))
            info->hevc = !strcmp(line + 11, "hevc");
        else if (!strncmp(line, "width=", 6))
            info->width = atoi(line + 6);
        else if (!strncmp(line, "height=", 7))
            info->height = atoi(line + 7);
        else if (!strncmp(line, "display_aspect_ratio=", 21))
            sscanf(line + 21, "%d:%d", &info->ratio_num, &info->ratio_den);
        else if (!strncmp(line, "start_time=", 11) &&
                 info->chapters < RETROHH_VIDEO_MAX_CHAPTERS)
            info->chapter_ms[info->chapters++] = (int)(atof(line + 11) * 1000);
    }
    return true;
}

/* The child's environment: ours, plus the plugins. */
static char **player_environment(void)
{
    static char *envp[ENV_MAX];
    static char lib_path[1024];
    const char *old = getenv("LD_LIBRARY_PATH");
    int n = 0;

    snprintf(lib_path, sizeof lib_path,
             "LD_LIBRARY_PATH=" PLUGIN_DIR ":/usr/trimui/lib:/usr/lib%s%s",
             old ? ":" : "", old ? old : "");
    envp[n++] = lib_path;
    envp[n++] = "LD_PRELOAD=" PLUGIN_DIR "/libdivx_shim.so:"
                PLUGIN_DIR "/libasound_module_pcm_trimpod.so";
    for (char **e = environ; *e && n < ENV_MAX - 1; e++)
        if (strncmp(*e, "LD_LIBRARY_PATH=", 16) && strncmp(*e, "LD_PRELOAD=", 11))
            envp[n++] = *e;
    envp[n] = NULL;
    return envp;
}

bool retrohh_video_start(const char *path, int start_ms)
{
    char *const argv[] = { "tplayerslave", (char *)path, NULL };
    char **envp = player_environment();
    long maxfd = sysconf(_SC_OPEN_MAX);
    int null_fd;
    pid_t pid;

    /* A stale status would read as this video already playing. */
    remove(STATUS_FILE);

    null_fd = open("/dev/null", O_WRONLY);      /* the player's log is very chatty */
    if (null_fd < 0)
        return false;
    if (!video_audio_open())
        fprintf(stderr, "video: no route for the player's sound\n");
    pid = fork();
    if (pid == 0)
        child_exec(PLAYER_BIN, argv, envp, null_fd, maxfd);
    close(null_fd);
    if (pid < 0)
    {
        video_audio_close();
        return false;
    }
    player_pid = pid;

    /* No start-position option, but a seek queued before the file loads runs as
     * soon as playback starts. */
    if (start_ms > 0)
        for (int i = 0; i < OPEN_POLLS && !player_command("seek %d", start_ms); i++)
            usleep(OPEN_POLL_US);
    return true;
}

/* Stops every thread of the player, so none can put its layer back. */
static void freeze_player(void)
{
    int st;

    kill(player_pid, SIGSTOP);
    for (int i = 0; i < FREEZE_POLLS; i++)
    {
        if (waitpid(player_pid, &st, WNOHANG | WUNTRACED) == player_pid)
        {
            if (!WIFSTOPPED(st))
                player_pid = -1;        /* it exited */
            return;
        }
        usleep(FREEZE_POLL_US);
    }
}

static void hide_player_layer(void)
{
    static const unsigned char off[DISP_CONFIG_BYTES];
    unsigned long args[4] = { 0, (unsigned long)off, 1, 0 };
    int fd = open(DISP_DEV, O_RDWR);

    if (fd < 0)
        return;
    if (ioctl(fd, DISP_LAYER_SET_CONFIG, args) < 0)
        fprintf(stderr, "video: can't hide the player's layer: %s\n", strerror(errno));
    close(fd);
    usleep(DISP_LATCH_US);
}

bool retrohh_video_running(void)
{
    int st;

    if (player_pid < 0)
        return false;
    if (waitpid(player_pid, &st, WNOHANG) == player_pid)
    {
        player_pid = -1;
        if (!WIFEXITED(st) || WEXITSTATUS(st))
            hide_player_layer();        /* it didn't take the layer down */
        return false;
    }
    return true;
}

/* False until the player has opened the FIFO. */
static bool player_command(const char *fmt, ...)
{
    char cmd[64];
    va_list ap;
    int fd, len;

    va_start(ap, fmt);
    len = vsnprintf(cmd, sizeof cmd - 1, fmt, ap);
    va_end(ap);
    if (len < 0 || len >= (int)sizeof cmd - 1)
        return false;
    cmd[len++] = '\n';

    fd = open(CMD_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return false;
    len = write(fd, cmd, len);
    close(fd);
    return len > 0;
}

void retrohh_video_stop(void)
{
    if (retrohh_video_running())
    {
        player_command("exit");
        for (int i = 0; i < STOP_POLLS && retrohh_video_running(); i++)
            usleep(STOP_POLL_US);
    }
    if (player_pid > 0)                 /* it ignored the request */
    {
        fprintf(stderr, "video: the player ignored exit; killing it\n");
        freeze_player();                /* hidden before its buffers go: no glitch */
        hide_player_layer();
        if (player_pid > 0)
        {
            kill(player_pid, SIGKILL);
            waitpid(player_pid, NULL, 0);
            player_pid = -1;
        }
    }
    remove(STATUS_FILE);
    video_audio_close();
}

static bool json_int(const char *json, const char *key, int *value)
{
    const char *found = strstr(json, key);

    if (!found)
        return false;
    *value = atoi(found + strlen(key));
    return true;
}

bool retrohh_video_status(int *position_ms, int *duration_ms, bool *playing)
{
    char json[192];
    int fd = open(STATUS_FILE, O_RDONLY);
    ssize_t got;

    if (fd < 0)
        return false;
    got = read(fd, json, sizeof json - 1);
    close(fd);
    if (got <= 0)                       /* it's rewritten in place: can be empty */
        return false;
    json[got] = '\0';
    if (!json_int(json, "\"position\":", position_ms) ||
        !json_int(json, "\"duration\":", duration_ms))
        return false;
    *playing = strstr(json, "\"status\":\"playing\"") != NULL;
    return true;
}

void retrohh_video_pause(bool paused)
{
    player_command(paused ? "pause" : "unpause");
}

void retrohh_video_seek(int position_ms)
{
    player_command("seek %d", position_ms);
}

void retrohh_video_set_aspect(const struct retrohh_video_info *info,
                              enum retrohh_video_aspect aspect)
{
    long ratio_num = info->ratio_num ? info->ratio_num : info->width;
    long ratio_den = info->ratio_den ? info->ratio_den : info->height;
    int screen_w, screen_h;
    long w, h;

    if (aspect == RETROHH_VIDEO_STRETCH || ratio_num <= 0 || ratio_den <= 0)
    {
        player_command("full");
        return;
    }
    SDL_GL_GetDrawableSize(sdlWindow, &screen_w, &screen_h);
    /* The limiting edge matches the screen.  The window must stay on it: the
     * display shows nothing of a layer that runs past an edge. */
    if (ratio_num * screen_h > ratio_den * screen_w)
    {
        w = screen_w;
        h = w * ratio_den / ratio_num;
    }
    else
    {
        h = screen_h;
        w = h * ratio_num / ratio_den;
    }
    player_command("win %ld %ld %ld %ld", (screen_w - w) / 2, (screen_h - h) / 2,
                   w, h);
}

void retrohh_video_set_overlay(bool video_visible, int overlay_rows)
{
    sdl_gl_set_video_through(video_visible, overlay_rows);
}
