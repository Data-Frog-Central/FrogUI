#ifndef MULTICORE_INCLUDES_H
#define MULTICORE_INCLUDES_H

#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <dirent.h>

extern int fallback_load_rgb565_image(const char* filename, uint16_t* framebuffer, int width, int height);
extern bool fallback_is_zip_wqw_file(const char *file_path, bool *is_wqw);
extern unsigned long long fallback_os_get_tick_count();
extern void fallback_wrap_run_game(const char *path, int state);

// TODO: Replace all xlog with proper libretro logging
// TODO: Replace all RGB565 with BMP (Besides WQW)
#if defined(SF2000)
#define OS_NAME_LOWER "multicore"
#include <stockfw.h>
#include <debug.h>
#include <core.h>
#elif defined(DARTOS)
#define OS_NAME_LOWER "dartos"
#include "libretro.h"
#include <sys/stat.h>
#include <core_api.h>
#include <frontend_functions.h>
#define load_rgb565_image fallback_load_rgb565_image
#define is_zip_wqw_file fallback_is_zip_wqw_file
#define os_get_tick_count xTaskGetTickCount
#define wrap_run_game close_emulator
extern uint32_t g_preview_width;
extern uint32_t g_preview_height;
extern char *ptr_gs_run_game_name;
#else 
#define OS_NAME_LOWER "linux"
#define xlog printf // For non-SF2000 builds, use printf as fallback
#define load_rgb565_image fallback_load_rgb565_image
#define is_zip_wqw_file fallback_is_zip_wqw_file
#define os_get_tick_count fallback_os_get_tick_count
#define wrap_run_game fallback_wrap_run_game
extern uint32_t g_preview_width;
extern uint32_t g_preview_height;
extern char *ptr_gs_run_game_name;
extern char *ptr_gs_run_game_file;
#endif

#endif
