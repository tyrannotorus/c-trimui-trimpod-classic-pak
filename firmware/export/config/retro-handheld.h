/*
 * This config file is for Retro Handhelds.
 * 
 * Currently supported CPU/Firmwares:
 *    - H700 devices (28XX, 34XX, 35XX, 40XX, CubeXX, ...)
 *        - MuOS
 *    - a133p devices (TrimUI Brick/Smart Pro, MagicX Zero28, ...)
 *        - MuoS
 *        - NextUI
 */

/* We don't run on hardware directly */
#define CONFIG_PLATFORM (PLATFORM_HOSTED|PLATFORM_SDL)
#define HAVE_FPU

/* For Rolo and boot loader */
#define MODEL_NUMBER 100

#define MODEL_NAME   "Retro Handheld"

#define USB_NONE

/* define this if you have a colour LCD */
#define HAVE_LCD_COLOR

/* define this to enable bitmap scaling */
#define HAVE_BMP_SCALING

/* define this to enable JPEG decoding */
#define HAVE_JPEG

/* Trimpod: the tag database (tagcache) is intentionally NOT built. Playback,
 * file browsing, playlists, bookmarks and album art do not need it, and it adds
 * user-facing maintenance complexity. Leaving the tag database disabled removes
 * the Database menu, its settings, the scanner and tagcache/tagtree sources. */

/* LCD dimensions */
#define LCD_WIDTH  320
#define LCD_HEIGHT 240
#define LCD_DEPTH  24
#define LCD_PIXELFORMAT RGB888

/* define this to indicate your device's keypad */
#define HAVE_BUTTON_DATA
#define HAVE_VOLUME_IN_LIST

/* define this if you have a real-time clock */
#define CONFIG_RTC APPLICATION

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x400000

/* The number of bytes reserved for loadable plugins */
#define PLUGIN_BUFFER_SIZE 0x800000

/* Battery stuff */
#define CONFIG_BATTERY_MEASURE PERCENTAGE_MEASURE
#define CONFIG_CHARGING CHARGING_MONITOR
#define HAVE_POWEROFF_WHILE_CHARGING

/* Trimpod: idle power-off hands off to the launcher's safe PMIC shutdown
 * (poweroff_next) via the /tmp/poweroff flag, instead of just exiting. */
#define HAVE_TRIMUI_SAFE_POWEROFF

/* Trimpod: sleep timer does a NextUI-style deep sleep (suspend-to-RAM via the
 * launcher's `suspend` script), waking on the power button. */
#define HAVE_TRIMUI_DEEP_SLEEP

/* Define this for LCD backlight available */
#define HAVE_BACKLIGHT_BRIGHTNESS
#define CONFIG_BACKLIGHT_FADING BACKLIGHT_FADING_SW_SETTING

/* Main LCD backlight brightness range and defaults, steps of 15... revisit this for other chipsets */
#define MIN_BRIGHTNESS_SETTING      1
#define MAX_BRIGHTNESS_SETTING      17
#define DEFAULT_BRIGHTNESS_SETTING  6

#define CONFIG_KEYPAD RETRO_HANDHELD_PAD

/* Use SDL audio/pcm in a SDL app build */
#define HAVE_SDL
#define HAVE_SDL_AUDIO

#define HAVE_SW_TONE_CONTROLS

/* Define this to the CPU frequency */
/*
#define CPU_FREQ 48000000
*/

#define CONFIG_LCD LCD_COWOND2

/* Hold/lock switch (Brick side toggle): blocks all input when engaged. */
#define HAS_BUTTON_HOLD

#define BOOTDIR "/tmp/trimpod"

/* No special storage */
#define CONFIG_STORAGE STORAGE_HOSTFS
#define HAVE_STORAGE_FLUSH
