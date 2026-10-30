/* port_config.h -- Steven Universe: Attack the Light's settings for the
 * android32 runtime (taken over from aks796's dcr_sea_nx).
 *
 * Macros only: the runtime's C files, its assembly and the launcher all read
 * this (runtime/source/rt_settings.h). What each setting does is next to its
 * default in the runtime; runtime/docs/ lists them all. MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

/* ------------------------------------------------------------------ the game */
#define PORT_TITLE    "Steven Universe: Attack the Light"
#define PORT_NAME     "stevenrpg_nx"
#define PORT_PACKAGE  "com.turner.stevenrpg"
#define PORT_BANNER   "stevenrpg_nx: Steven Universe: Attack the Light (Unity 5.3.1p2 / Mono, armeabi-v7a)"
/* libmain + libunity + libmono end to end (~20 MB mapped), with room;
 * under-reserving shows up as so_load() -3 */
#define PORT_SO_REGION_BYTES (48u * 1024 * 1024)

/* The APK, by its package: it becomes game.apk. The game's data is in its
 * expansion file, main.<version>.com.turner.stevenrpg.obb, in obb/ of the
 * game folder (dcr_path.c maps Android/obb/<package>/ there). */
#define PORT_APK_DESC "Steven Universe: Attack the Light (armeabi-v7a)"
#define PORT_APK_ROLES                                                                        \
  {.what = "Steven Universe: Attack the Light", .name = "game.apk",                         \
   .package = "com.turner.stevenrpg", .flags = RT_APK_ADOPT}
#define PORT_LAUNCHER_START_NOTE "(the first start unpacks the game's libraries from the APK)"

/* ------------------------------------------------------------------ libc */
#define RT_PROC_COMM        "turner.stevenrp"
#define RT_EXTRA_ENV        "MONO_DEBUG=explicit-null-checks" /* load-bearing: Mono without SIGSEGV */
#define RT_DL_HAS_OPENSLES  0 /* FMOD falls back to its Java output (dcr_audio.c) */
#define RT_ZLIB_HOLDBACK    0

/* ------------------------------------------------------------------ JNI, NDK */
#define RT_JNI_UNHANDLED_INSTANCE_SINGLETON   1 /* Unity's C# plugins */
#define RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF 1
#define RT_LOOPER_MAIN_IMPLICIT               0

/* ------------------------------------------------------------------ frames */
#define RT_GL_SWAP_ENDS_FRAME 0 /* the frame is nativeRender (dcr_boot.c) */
#define RT_CAPTURE_TRIES      300
#define RT_BOOST_LAUNCH_AFTER_PICTURE_MS 25000
#define RT_MAIN_THREAD_NAME   "UnityMain"
#define RT_PAD_MAX_PLAYERS    1

/* ------------------------------------------------------------------ watchdog */
#define RT_WATCHDOG_PRIO 0x1C
#define RT_WATCHDOG_CORE 2
#define RT_WATCHDOG_BOOT 1

/* ------------------------------------------------------------------ files */
#define RT_PATH_CWD_RELATIVE   0
#define RT_DIRCACHE            0 /* Mono probes and writes under data/ */
#define RT_MANIFEST_LOG_PREFIX "unity"
#define RT_EMU_FIXUPS          0 /* its self-fix runs after the module fix (dcr_main.c) */

#endif
