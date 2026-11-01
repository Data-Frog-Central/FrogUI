#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "multicore_includes.h"
#include "libretro.h"

extern retro_environment_t environ_cb;

// WQW stuff
uint32_t g_preview_width = 0;
uint32_t g_preview_height = 0;

// Loader stuff
char game_name_buf[256];
char core_name_buf[256];
char *ptr_gs_run_game_name = game_name_buf;

int fallback_load_rgb565_image(const char* filename, uint16_t* framebuffer, int width, int height) {
    // Open the raw image file
    FILE* file = fopen(filename, "rb");
    if (file == NULL)
    {
        printf("Error opening file: %s\n", filename);
        return -1;
    }

    // Calculate the number of bytes to read (width * height * 2 bytes per pixel)
    size_t image_size = width * height * sizeof(uint16_t);

    // Read the raw RGB565 image data into the framebuffer
    size_t bytes_read = fread(framebuffer, 1, image_size, file);
    if (bytes_read != image_size)
    {
        printf("Error reading the image file\n");
        fclose(file);
        return -1;
    }

    // Close the file after reading the image
    fclose(file);

    return 0;
}

bool fallback_is_zip_wqw_file(const char *file_path, bool *is_wqw) {
    // Always return false, very unlikely this platform supports WQW
    (void)file_path;

    if (is_wqw) *is_wqw = false;

    return false;
}

#ifndef NOCTURNAL
unsigned long long fallback_os_get_tick_count() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void boot_retroarch(const char *core, const char *rom) {
    char *args[] = {
        "retroarch", 
        "-L", 
        (char *)core, 
        (char *)rom, 
        NULL
    };

    execvp(args[0], args);
}
extern const char *parent_dir(const char *path);
#endif

void fallback_wrap_run_game(const char *path, int state) {
    (void)path;
    (void)state;
#ifdef UNIX
    char core_name_buf2[256];
    for (char *p = core_name_buf; *p; ++p)
        *p = (char)tolower((unsigned char)*p);
    const char* home_dir = getenv("HOME");
    sprintf(core_name_buf2, "%s/.config/retroarch/cores/%s_libretro.so", home_dir, core_name_buf);
    boot_retroarch(core_name_buf2, game_name_buf);
#endif
#ifdef RETRO_ENVIRONMENT_RUN_EMULATOR
    struct retro_private_emulator_paths paths;
    paths.core_path = core_name_buf;
    paths.rom_path = game_name_buf;
    environ_cb(RETRO_ENVIRONMENT_RUN_EMULATOR, &paths);
    return;
#endif
    environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}
