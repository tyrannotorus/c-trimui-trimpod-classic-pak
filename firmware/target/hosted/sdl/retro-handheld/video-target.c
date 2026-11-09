/***************************************************************************
 * Trimpod: runs TrimUI's stock hardware video player (tplayerslave) as a child.
 *
 * The player decodes to a display layer below the app window and takes commands
 * through a FIFO.  Two plugins from trimpod/video extend it: libpassthrua.so
 * (AC-3, found on LD_LIBRARY_PATH) and libdivx_shim.so (DivX, LD_PRELOAD).
 ****************************************************************************/
#include <sys/types.h>
#include <sys/wait.h>
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
    envp[n++] = "LD_PRELOAD=" PLUGIN_DIR "/libdivx_shim.so";
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
    pid = fork();
    if (pid == 0)
        child_exec(PLAYER_BIN, argv, envp, null_fd, maxfd);
    close(null_fd);
    if (pid < 0)
        return false;
    player_pid = pid;

    /* No start-position option, but a seek queued before the file loads runs as
     * soon as playback starts. */
    if (start_ms > 0)
        for (int i = 0; i < OPEN_POLLS && !player_command("seek %d", start_ms); i++)
            usleep(OPEN_POLL_US);
    return true;
}

bool retrohh_video_running(void)
{
    if (player_pid < 0)
        return false;
    if (waitpid(player_pid, NULL, WNOHANG) == player_pid)
    {
        player_pid = -1;
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
    if (player_pid >= 0)                /* it ignored the request */
    {
        kill(player_pid, SIGKILL);
        waitpid(player_pid, NULL, 0);
        player_pid = -1;
    }
    remove(STATUS_FILE);
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
    bool wider;

    if (aspect == RETROHH_VIDEO_STRETCH || ratio_num <= 0 || ratio_den <= 0)
    {
        player_command("full");
        return;
    }
    SDL_GL_GetDrawableSize(sdlWindow, &screen_w, &screen_h);
    wider = ratio_num * screen_h > ratio_den * screen_w;
    /* Fit matches the limiting edge to the screen; Fill the other one. */
    if (wider == (aspect == RETROHH_VIDEO_FIT))
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
