#---------------------------------------------------------------------------------
# Steven Universe: Attack the Light -- Nintendo Switch wrapper (32-bit / AArch32, Unity 5.3 Mono)
#
# Ships NO game code and NO game assets: the game's own APK and expansion
# file (the player's copy) are read at run time, and its libraries are
# unpacked from the APK on the first launch.
#
# Built on aks796's dcr_sea_nx (Disney Crossy Road, MIT): the same Unity/Mono
# layer, with the Crossy Road mod taken out.
#
# The build is the android32 runtime's (runtime/runtime.mk: devkitARM +
# libnx32 + mesa32 from portlibs32/); ./build.sh runs it in the toolchain
# container. Output: stevenrpg_nx.nsp, which the launcher NRO carries (launcher/).
#---------------------------------------------------------------------------------
TARGET               := stevenrpg_nx
PORT_NPDM_PROGRAM_ID := 0x0100A7E35B2C4D10
PORT_NPDM_VERSION    := 0.1.0
# The game's movies (Handheld.PlayFullScreenMovie: logos, intro, outro):
# FFmpeg's MP4 demuxer and H.264 / AAC decoders, built by ffmpeg32's build.sh
# (LGPL; FFMPEG_COMPONENTS with h264, see README) and copied into portlibs32/.
# Without them su_video.c builds without pictures (the movies are skipped).
DCR_VIDEO := $(if $(wildcard portlibs32/lib/libavcodec.a),1,0)
ifeq ($(DCR_VIDEO),1)
PORT_LIBS := -L$(CURDIR)/portlibs32/lib -lavformat -lavcodec -lavutil
endif
include runtime/runtime.mk

# FFmpeg is built with int-sized enums (-fno-short-enums), and so is its one
# user here.
$(BUILD)/su_video.o: $(SOURCES)/su_video.c $(RENDERER_STAMP) | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -fno-short-enums -DDCR_VIDEO=$(DCR_VIDEO) -c $< -o $@

.PHONY: check
check:
	@echo "run on the host: python3 tools/offsets/derive53.py <lib/armeabi-v7a> -o <out.h>"
	@echo "                 python3 runtime/tools/gen_imports.py --check"
