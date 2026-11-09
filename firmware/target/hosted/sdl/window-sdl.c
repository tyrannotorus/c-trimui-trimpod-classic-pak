/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <SDL.h>
#include "sim-ui-defines.h"
#include "window-sdl.h"
#include "lcd-sdl.h"
#include "misc.h"
#include "lcd.h"        /* lcd_panel_is_dark: no presents while the panel is off */
#include "panic.h"

/* Trimpod renders through an OpenGL ES 2/3 context (not an SDL_Renderer) so the
 * Milkdrop visualizer (projectM) can draw into the same window. In normal UI
 * mode we upload the 320x240 LCD surface to a texture and draw it as a
 * full-window quad (nearest-filtered 3.2x upscale). */
#include <GLES2/gl2.h>

extern SDL_Surface *lcd_surface;

SDL_Surface  *sim_lcd_surface;

SDL_mutex *window_mutex;

SDL_Window   *sdlWindow;

static bool window_adjustment_needed;
double display_zoom = 1;

/* When the Milkdrop visualizer is running it owns the GL context and the window;
 * suppress the normal LCD presentation so other Rockbox threads' lcd_update()
 * calls don't fight for the context. */
volatile bool trimpod_viz_active = false;

static SDL_GLContext gl_ctx = NULL;
static GLuint gl_lcd_tex = 0, gl_prog = 0, gl_vbo = 0;
static GLint  gl_u_fade = -1;
static int gl_tex_w = 0, gl_tex_h = 0; /* size gl_lcd_tex is currently allocated for */

/* A video plays on the display layer below this window: everything is
 * transparent except the bottom video_overlay_rows LCD rows. */
static bool video_through = false;
static int  video_overlay_rows = 0;

static const char *gl_vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *gl_fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_fade;\n"   /* 1 = normal, 0 = black (visualizer entry fade) */
    /* .bgr, not .rgb: the 24-bit Rockbox framebuffer stores blue first (lcd.h:
     * fb_data is {b, g, r}) and it is uploaded verbatim as GL_RGB, so the swizzle
     * here puts the channels back in order -- free, vs. a per-frame conversion. */
    "void main(){ gl_FragColor = vec4(texture2D(u_tex, v_uv).bgr * u_fade, 1.0); }\n";

static GLuint gl_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        panicf("GL shader compile failed: %s", log);
    }
    return s;
}

static void gl_init_present(void)
{
    /* full-window quad: pos.xy, uv.xy; v flipped so texture row 0 is at top */
    static const GLfloat verts[] = {
        -1.f, -1.f, 0.f, 1.f,
         1.f, -1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 0.f,
         1.f,  1.f, 1.f, 0.f,
    };
    GLuint vs = gl_compile(GL_VERTEX_SHADER, gl_vs_src);
    GLuint fs = gl_compile(GL_FRAGMENT_SHADER, gl_fs_src);
    gl_prog = glCreateProgram();
    glAttachShader(gl_prog, vs);
    glAttachShader(gl_prog, fs);
    glBindAttribLocation(gl_prog, 0, "a_pos");
    glBindAttribLocation(gl_prog, 1, "a_uv");
    glLinkProgram(gl_prog);
    gl_u_fade = glGetUniformLocation(gl_prog, "u_fade");

    glGenBuffers(1, &gl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glGenTextures(1, &gl_lcd_tex);
    gl_tex_w = gl_tex_h = 0;   /* fresh texture: next present must allocate it */
    glBindTexture(GL_TEXTURE_2D, gl_lcd_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Ensure our GL context is current on the calling (Rockbox) thread. Rockbox sim
 * threads are serialized, but lcd_update() may run on different threads over
 * time, so re-bind defensively before issuing GL calls. */
void sdl_gl_make_current(void)
{
    if (gl_ctx)
        SDL_GL_MakeCurrent(sdlWindow, gl_ctx);
}

SDL_GLContext sdl_gl_get_context(void)
{
    return gl_ctx;
}

/* Upload an LCD surface and draw it to fill the window (fade 1=normal, 0=black),
 * then swap. */
static void gl_present_lcd_fade(SDL_Surface *src, float fade)
{
    int w, h;

    SDL_GL_GetDrawableSize(sdlWindow, &w, &h);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);   /* projectM may leave these on */
    glDisable(GL_BLEND);
    glClearColor(0.f, 0.f, 0.f, video_through ? 0.f : 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (video_through)
    {
        if (!video_overlay_rows)
        {
            SDL_GL_SwapWindow(sdlWindow);
            return;
        }
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, w, video_overlay_rows * h / src->h);
    }

    glUseProgram(gl_prog);
    glUniform1f(gl_u_fade, fade);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_lcd_tex);
    /* The LCD surface is 24bpp RGB888 -- byte order R,G,B, exactly what
     * GL_RGB/GL_UNSIGNED_BYTE expects -- so it uploads verbatim: no conversion
     * pass, and a quarter less data than RGBA.  SDL pads surface rows to 4 bytes
     * and GL's default unpack alignment is 4, so the strides agree for any width
     * (the alignment is left alone -- it is global state projectM shares).
     * Only a size change re-specifies the texture (that reallocates); otherwise
     * update in place. */
    if (src->w != gl_tex_w || src->h != gl_tex_h)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, src->w, src->h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, src->pixels);
        gl_tex_w = src->w;
        gl_tex_h = src->h;
    }
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src->w, src->h,
                        GL_RGB, GL_UNSIGNED_BYTE, src->pixels);

    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (void *)(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);

    SDL_GL_SwapWindow(sdlWindow);
}

void sdl_gl_set_video_through(bool on, int overlay_rows)
{
    video_through = on;
    video_overlay_rows = overlay_rows;
}

/* Present the current LCD framebuffer faded toward black (1=normal, 0=black) and
 * swap.  Used for the visualizer entry fade-out (same fade technique as the
 * visualizer's own preset transitions, applied to the Now Playing image). */
void sdl_gl_present_lcd_fade(float fade)
{
    sdl_gl_make_current();
    gl_present_lcd_fade(sim_lcd_surface, fade);
}

static void get_window_dimensions(int *w, int *h)
{
    if (background)
    {
        *w = UI_WIDTH;
        *h = UI_HEIGHT;
    }
    else
    {
        {
            *w = SIM_LCD_WIDTH;
            *h = SIM_LCD_HEIGHT;
        }
    }
}


void sdl_window_render(void)
{
    if (trimpod_viz_active)
        return;   /* visualizer owns the GL context/window right now */
    if (lcd_panel_is_dark())
        return;   /* panel off: the surfaces stay current, the present is waste */
    sdl_gl_make_current();
    gl_present_lcd_fade(sim_lcd_surface, 1.0f);
}

bool sdl_window_adjust(void)
{
    int w, h;

    if (!window_adjustment_needed)
        return false;
    window_adjustment_needed = false;

    get_window_dimensions(&w, &h);

    if (!(SDL_GetWindowFlags(sdlWindow) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
        && display_zoom)
    {
        SDL_SetWindowSize(sdlWindow, display_zoom * w, display_zoom * h);
    }
    display_zoom = 0;
    sdl_window_render();
    return true;
}

void sdl_window_adjustment_needed(void)
{
    window_adjustment_needed = true;

    /* For MacOS and Windows, we're on a main or
    display thread already, and can immediately
    adjust the window.
    On Linux, we have to defer the update, until
    it is handled by the main thread later. */
}

void sdl_window_setup(void)
{
    int width, height;
    int depth = LCD_DEPTH < 8 ? 16 : LCD_DEPTH;
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;

    if (display_zoom == 1)
        flags |= SDL_WINDOW_RESIZABLE;

    /* No window-background bitmap on this target: the LCD fills the window.
     * (`background` defaults to true, so it must be cleared here.) */
    background = false;

    get_window_dimensions(&width, &height);

    /* GLES 3.0 context (projectM needs it); the LCD is presented via GL too. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);   /* video shows through alpha 0 */
    flags |= SDL_WINDOW_OPENGL;

    if ((sdlWindow = SDL_CreateWindow(UI_TITLE, SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, width * display_zoom,
                                   height * display_zoom , flags)) == NULL)
        panicf("%s", SDL_GetError());
    if ((gl_ctx = SDL_GL_CreateContext(sdlWindow)) == NULL)
        panicf("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_MakeCurrent(sdlWindow, gl_ctx);
    SDL_GL_SetSwapInterval(1);
    gl_init_present();

    /* Surface for LCD content only. Needs to fit largest LCD */
    if ((sim_lcd_surface = SDL_CreateRGBSurface(0,
                                                SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                                                depth, 0, 0, 0, 0)) == NULL)
        panicf("%s", SDL_GetError());

    display_zoom = 0; /* reset to 0 unless/until user requests a scale level change */
    window_mutex = SDL_CreateMutex();
}
