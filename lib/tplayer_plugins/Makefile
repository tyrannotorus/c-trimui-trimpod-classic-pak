# Plugins for TrimUI's stock video player (tplayerslave); see the sources.
OUT ?= .
LIBA52 := ../rbcodec/codecs/liba52
CODECLIB := ../rbcodec/codecs/lib
CFLAGS := -shared -fPIC -O2 -Wall -s -Wl,--as-needed

PASSTHRUA_SRC := passthrua.c $(LIBA52)/bit_allocate.c $(LIBA52)/bitstream.c \
	$(LIBA52)/downmix.c $(LIBA52)/imdct.c $(LIBA52)/parse.c \
	$(CODECLIB)/fft-ffmpeg.c $(CODECLIB)/mdct_lookup.c

all: $(OUT)/libpassthrua.so $(OUT)/libdivx_shim.so $(OUT)/libasound_module_pcm_trimpod.so

$(OUT)/libpassthrua.so: $(PASSTHRUA_SRC) codeclib.h
	$(CC) $(CFLAGS) -include codeclib.h -I. -I$(LIBA52) -I$(CODECLIB) \
		$(PASSTHRUA_SRC) -lm -o $@

$(OUT)/libdivx_shim.so: divx_shim.c
	$(CC) $(CFLAGS) -Wextra $< -ldl -o $@

# Its ring is shared with Trimpod's side (video-audio.c), which owns the header.
# -DPIC: alsa-lib's plugin macros otherwise take their static-library branch.
TPVA_DIR := ../../firmware/target/hosted/sdl/retro-handheld

$(OUT)/libasound_module_pcm_trimpod.so: alsa_trimpod.c $(TPVA_DIR)/tpva.h
	$(CC) $(CFLAGS) -Wextra -DPIC -I$(TPVA_DIR) $< -lasound -ldl -o $@

.PHONY: all
