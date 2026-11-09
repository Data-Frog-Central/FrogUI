/*
 * FrogUI - MinUI-style File Browser for Multicore
 * A libretro core that provides a file browser interface
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

#include "font.h"
#include "render.h"
#include "theme.h"
#include "recent_games.h"
#include "favorites.h"
#include "settings.h"
#include "frogos.h"
#include "multicore_includes.h"

struct retro_private_accel_functions accel_funcs = {0};

// Console to core name mapping
typedef struct {
    char folder_name[64];
    char core_name[32];      // core_name is the internal core name
    char console_file[32];   // core file name
} ConsoleMapping;

#define MAX_MAPPINGS 256
static ConsoleMapping console_mappings[MAX_MAPPINGS];
size_t console_mapping_count;

static char *trim(char *s) {
    char *end;

    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';

    return s;
}

int load_console_mappings(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1; // failed to open file

    char line[256];
    console_mapping_count = 0;

    while (fgets(line, sizeof(line), fp))
    {
        char *p1, *p2;
        char *key, *val1, *val2;

        // Skip comments and empty lines
        char *trimmed = trim(line);
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        // Find first '='
        p1 = strchr(trimmed, '=');
        if (!p1) continue;
        *p1 = '\0';
        key = trim(trimmed);

        // Find second '='
        p2 = strchr(p1 + 1, '=');
        if (!p2) continue;
        *p2 = '\0';
        val1 = trim(p1 + 1);
        val2 = trim(p2 + 1);

        if (console_mapping_count >= MAX_MAPPINGS)
            break;

        // Copy into struct safely
        strncpy(console_mappings[console_mapping_count].folder_name, key, sizeof(console_mappings[0].folder_name) - 1);
        console_mappings[console_mapping_count].folder_name[sizeof(console_mappings[0].folder_name) - 1] = '\0';

        strncpy(console_mappings[console_mapping_count].core_name, val1, sizeof(console_mappings[0].core_name) - 1);
        console_mappings[console_mapping_count].core_name[sizeof(console_mappings[0].core_name) - 1] = '\0';

        strncpy(console_mappings[console_mapping_count].console_file, val2, sizeof(console_mappings[0].console_file) - 1);
        console_mappings[console_mapping_count].console_file[sizeof(console_mappings[0].console_file) - 1] = '\0';

        console_mapping_count++;
    }

    fclose(fp);
    return 0; // success
}

// Get core name for a console folder
static const char* get_core_name_for_console(const char* folder_name) {
    for (size_t i = 0; i < console_mapping_count; i++) {
        if (strcmp(console_mappings[i].folder_name, folder_name) == 0) {
            return console_mappings[i].core_name;
        }
    }
    return NULL; // Unknown console
}

// Show core-specific settings menu
static void show_core_settings(const char* core_name) {
    if (settings_load_core(core_name)) {
        settings_show_menu();
    } else {
        // No settings file found for this core - could show a message or create default
        // For now, just ignore if no settings file exists
    }
}

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define MAX_PATH_LEN 512
#define INITIAL_ENTRIES_CAPACITY 64

char assets_dir[128];
char roms_path[128];
char configs_dir[128];
char os_settings_title[64];
char empty_dirs_cache_file[256];

// Empty folders cache - avoid rescanning on every navigation
#define MAX_EMPTY_DIRS 256
static char empty_dirs[MAX_EMPTY_DIRS][64];  // Store folder names (not full paths)
static int empty_dirs_count = 0;
static int empty_dirs_loaded = 0;

// Forward declarations
static void rebuild_empty_dirs_cache(void);
static void show_cache_rebuild_screen(void);

// Load empty directories cache from file (or rebuild if missing)
static void load_empty_dirs_cache(void) {
    if (empty_dirs_loaded) return;
    empty_dirs_loaded = 1;
    empty_dirs_count = 0;

    FILE *fp = fopen(empty_dirs_cache_file, "r");
    if (!fp) {
        // Cache file doesn't exist - rebuild it
        xlog("Empty dirs cache: file not found, rebuilding...\n");
        rebuild_empty_dirs_cache();
        return;
    }

    char line[64];
    while (fgets(line, sizeof(line), fp) && empty_dirs_count < MAX_EMPTY_DIRS) {
        // Remove newline
        int len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (line[0] != '\0') {
            strncpy(empty_dirs[empty_dirs_count], line, sizeof(empty_dirs[0]) - 1);
            empty_dirs[empty_dirs_count][sizeof(empty_dirs[0]) - 1] = '\0';
            empty_dirs_count++;
        }
    }
    fclose(fp);
    xlog("Empty dirs cache: loaded %d entries\n", empty_dirs_count);
}

// Check if a folder name is in the empty dirs cache
static int is_in_empty_cache(const char *folder_name) {
    for (int i = 0; i < empty_dirs_count; i++) {
        if (strcasecmp(empty_dirs[i], folder_name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Rebuild and save empty directories cache by scanning ROMS folder
static void rebuild_empty_dirs_cache(void) {
    show_cache_rebuild_screen();
    empty_dirs_count = 0;

    DIR *dir = opendir(roms_path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && empty_dirs_count < MAX_EMPTY_DIRS) {
        if (ent->d_name[0] == '.') continue;
        if (strcasecmp(ent->d_name, "frogui") == 0 ||
            strcasecmp(ent->d_name, "saves") == 0 ||
            strcasecmp(ent->d_name, "save") == 0) continue;

        // Skip non-directories using d_type (avoids stat() syscall)
        if (ent->d_type != DT_DIR) continue;

        // Save entry name BEFORE inner readdir (readdir uses static buffer!)
        char entry_name[64];
        strncpy(entry_name, ent->d_name, sizeof(entry_name) - 1);
        entry_name[sizeof(entry_name) - 1] = '\0';

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", roms_path, entry_name);

        // Check if directory is empty via opendir/readdir
        DIR *check = opendir(full_path);
        if (check) {
            int has_content = 0;
            struct dirent *sub;
            while ((sub = readdir(check)) != NULL) {
                if (sub->d_name[0] != '.') {
                    has_content = 1;
                    break;
                }
            }
            closedir(check);

            if (!has_content) {
                strncpy(empty_dirs[empty_dirs_count], entry_name, sizeof(empty_dirs[0]) - 1);
                empty_dirs[empty_dirs_count][sizeof(empty_dirs[0]) - 1] = '\0';
                empty_dirs_count++;
            }
        }
    }
    closedir(dir);

    // Save to file
    FILE *fp = fopen(empty_dirs_cache_file, "w");
    if (fp) {
        for (int i = 0; i < empty_dirs_count; i++) {
            fprintf(fp, "%s\n", empty_dirs[i]);
        }
        fclose(fp);
    }
    xlog("Empty dirs cache: rebuilt with %d entries\n", empty_dirs_count);
}

// Layout constants are now in render.h

// Thumbnail cache
static Thumbnail current_thumbnail;
static char cached_thumbnail_path[MAX_PATH_LEN];
static int thumbnail_cache_valid = 0;
static int last_selected_index = -1;

// Text scrolling state
static int text_scroll_frame_counter = 0;
static int text_scroll_offset = 0;
static int text_scroll_direction = 1;

// Menu state
typedef struct {
    char path[MAX_PATH_LEN];
    char name[256];
    int is_dir;
} MenuEntry;

static MenuEntry *entries = NULL;
static int entry_count = 0;
static int entries_capacity = 0;
static int selected_index = 0;
static int scroll_offset = 0;
static char current_path[MAX_PATH_LEN];
static uint16_t *framebuffer = NULL;

// Boundary scroll delay (frames to wait before wrapping)
#define BOUNDARY_DELAY_FRAMES 30
static int boundary_delay_timer = 0;
static int at_boundary = 0; // 1 = at top, 2 = at bottom

// A-Z picker state
static int az_picker_active = 0;
static int az_selected_index = 0; // 0-25 for A-Z, 26 for 0-9, 27 for #

// Ensure entries array has enough capacity
static void ensure_entries_capacity(int required_capacity) {
    if (entries_capacity >= required_capacity) {
        return; // Already have enough capacity
    }

    // Double capacity each time, or use initial capacity
    int new_capacity = entries_capacity == 0 ? INITIAL_ENTRIES_CAPACITY : entries_capacity;
    while (new_capacity < required_capacity) {
        new_capacity *= 2;
    }

    MenuEntry *new_entries = (MenuEntry*)realloc(entries, new_capacity * sizeof(MenuEntry));
    if (!new_entries) {
        // Memory allocation failed - keep old array
        return;
    }

    entries = new_entries;
    entries_capacity = new_capacity;
}

// Reset navigation state when entering new folder
static void reset_navigation_state(void) {
    selected_index = 0;
    scroll_offset = 0;
    boundary_delay_timer = 0;
    at_boundary = 0;
}

// Libretro callbacks
static retro_video_refresh_t video_cb = NULL;
static retro_audio_sample_t audio_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t environ_cb = NULL;
static retro_input_poll_t input_poll_cb = NULL;
static retro_input_state_t input_state_cb = NULL;

// Input state
static int prev_input[16] = {0};
static bool game_queued = false;  // Flag to indicate game is queued
bool show_multicore_opt = false;  // Flag to indicate showing multicore.opt
bool resume_on_boot = false;
bool hide_empty_folders = true;
bool prevent_scrolling = false;
bool bgm_enabled = true;
bool sfx_enabled = true;


void init_direct_loader(const char* core_name, const char* directory, const char* filename) {
    // Don't set ptr_gs_run_folder - currently inherit from menu core for savestates to work
    // TODO: Find a way to force ptr_gs_run_folder to be /sdcard/ROMS or /sdcard/ARCADE (unified save states folder)
    // TODO: Replace second core_name with full directory (besides /sdcard/) and seperate core_name from directory
#if defined(SF2000)
    sprintf((char *)ptr_gs_run_game_file, "%s;%s;%s.gba", core_name, directory, filename); // Emulate a stub directory, stub doesn't have to exist (loader fixes later)
    sprintf((char *)ptr_gs_run_game_name, "%s", filename); // Expects the filename without any extension

    // Remove extension from ptr_gs_run_game_name
    char *dot_position = strrchr(ptr_gs_run_game_name, '.');
    if (dot_position != NULL) {
        *dot_position = '\0';
    }
#else 
    sprintf(game_name_buf, "%s/%s/%s", roms_path, directory, filename);
    sprintf(core_name_buf, "%s", core_name);
#endif // SF2000

    // Add to recent history
    recent_games_add(core_name, filename, directory);

    game_queued = true; // Pass to retro_run, can only run the loader from there

}

// Apply settings changes
void apply_settings(void) {
    struct retro_variable var;
    
    // Load theme
    var.key = "frogui_theme";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        theme_load_from_settings(var.value);
    }

    // Load font
    var.key = "frogui_font";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        font_load_from_settings(var.value);
    }

    // Resume on boot
    var.key = "frogui_resume_on_boot";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "false") == 0) resume_on_boot = false;
        else if (strcmp(var.value, "true") == 0) resume_on_boot = true;
    }

    // Hide empty folders
    var.key = "frogui_hide_empty";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "false") == 0) hide_empty_folders = false;
        else if (strcmp(var.value, "true") == 0) hide_empty_folders = true;
    }

    // Prevent scrolling
    var.key = "frogui_prevent_scrolling";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "false") == 0) prevent_scrolling = false;
        else if (strcmp(var.value, "true") == 0) prevent_scrolling = true;
    }

    // Enable BGM
    var.key = "frogui_bgm_enabled";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "false") == 0) bgm_enabled = false;
        else if (strcmp(var.value, "true") == 0) bgm_enabled = true;
    }

    // Enable SFX
    var.key = "frogui_sfx_enabled";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "false") == 0) sfx_enabled = false;
        else if (strcmp(var.value, "true") == 0) sfx_enabled = true;
    }
}

// Show a loading screen during cache rebuild
static void show_cache_rebuild_screen(void) {
    if (!framebuffer || !video_cb) return;

    // Fill background
    render_fill_rect(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, theme_bg());

    // Draw centered message
    const char* msg = "Rebuilding folder cache...";
    int text_width = font_measure_text(msg);
    int x = (SCREEN_WIDTH - text_width) / 2;
    int y = (SCREEN_HEIGHT - FONT_CHAR_HEIGHT) / 2;
    render_text_pillbox(framebuffer, x, y, msg, theme_header(), theme_bg(), 6);

    // Push frame to display
    video_cb(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
}

// Get the base name from a path
static const char *get_basename(const char *path) {
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

// Get console_file (or folder_name if not found) from a full ROM path
static void get_corename(const char *path, char *core_name, size_t size) {
    const char *start;
    const char *end;
    char folder[64];
    char prefix[64];
    strncpy(prefix, roms_path, sizeof(prefix) - 1);

    // Skip prefix if present
    if (strncmp(path, prefix, strlen(prefix)) == 0)
        start = path + strlen(prefix);
    else
        start = path;

    if (start && start[0] == '/') start++;

    // Find end of the first directory
    end = strchr(start, '/');
    if (!end) {
        // No further '/', take the rest of the string
        strncpy(folder, start, sizeof(folder) - 1);
        folder[sizeof(folder) - 1] = '\0';
    } else {
        size_t len = end - start;
        if (len >= sizeof(folder)) len = sizeof(folder) - 1;
        strncpy(folder, start, len);
        folder[len] = '\0';
    }

    // Lookup console_file in console_mappings
    const char *console_file = folder; // default
    for (size_t i = 0; i < console_mapping_count; i++) {
        if (strcmp(console_mappings[i].folder_name, folder) == 0) {
            console_file = console_mappings[i].console_file;
            break;
        }
    }

    // Copy the result into core_name
    strncpy(core_name, console_file, size - 1);
    core_name[size - 1] = '\0';
}

// Auto-launch most recent game if resume on boot is enabled
static void auto_launch_recent_game(void) {
    // Get the most recent game
    const RecentGame *recent_list = recent_games_get_list();
    int recent_count = recent_games_get_count();

    if (recent_count == 0) {
        return; // No recent games to launch
    }

    // Get the first (most recent) game
    const RecentGame *game = &recent_list[0];

    init_direct_loader(game->core_name, game->full_path, game->game_name);
}

// Get scrolling display text for selected item
static void get_scrolling_text(const char *full_name, int is_selected, char *display_name, size_t display_size) {
    if (!full_name || !display_name) return;

    int name_len = strlen(full_name);

    static char padded_name[512];
    const int leading_gap_spaces = 3;
    const int trailing_gap_spaces = 2;
    snprintf(padded_name, sizeof(padded_name), "%*s%s%*s", leading_gap_spaces, "", full_name, trailing_gap_spaces, "");
    const char *scroll_text = padded_name;
    int scroll_len = strlen(scroll_text);

    // Check if we're in main menu or special views (no thumbnails)
    int in_main_menu = (strcmp(current_path, roms_path) == 0 ||
                        strcmp(current_path, "RECENT_GAMES") == 0 ||
                        strcmp(current_path, "FAVORITES") == 0 ||
                        strcmp(current_path, "TOOLS") == 0 ||
                        strcmp(current_path, "UTILS") == 0 ||
                        strcmp(current_path, "HOTKEYS") == 0 ||
                        strcmp(current_path, "CREDITS") == 0);

    // Use different max lengths: shorter for unselected items only in ROM lists (with thumbnails)
    int max_len = 30;
    if (thumbnail_cache_valid) max_len = is_selected ? MAX_FILENAME_DISPLAY_LEN : (in_main_menu ? MAX_FILENAME_DISPLAY_LEN : MAX_UNSELECTED_DISPLAY_LEN);

    // If short enough or not selected, just copy/truncate normally
    if ((name_len <= max_len || !is_selected) || (prevent_scrolling)) {
        if (name_len <= max_len) {
            strcpy(display_name, full_name);
        } else {
            strncpy(display_name, full_name, max_len);
            display_name[max_len] = '\0';
            strcat(display_name, "...");
        }
        return;
    }
    
    // Handle scrolling for selected long names
    text_scroll_frame_counter++;
    
    // Wait before starting scroll
    if (text_scroll_frame_counter < SCROLL_DELAY_FRAMES) {
        strncpy(display_name, scroll_text, max_len);
        display_name[max_len] = '\0';
        return;
    }
    
    // Update scroll position
    if (text_scroll_frame_counter % SCROLL_SPEED_FRAMES == 0) {
        text_scroll_offset += text_scroll_direction;
        
        // Reverse direction at ends
        int max_scroll = scroll_len - max_len;
        if (text_scroll_offset >= max_scroll) {
            text_scroll_direction = -1;
            text_scroll_offset = max_scroll;
        } else if (text_scroll_offset <= 0) {
            text_scroll_direction = 1;
            text_scroll_offset = 0;
        }
    }
    
    // Extract scrolled portion
    int copy_len = min(max_len, scroll_len - text_scroll_offset);
    strncpy(display_name, scroll_text + text_scroll_offset, copy_len);
    display_name[copy_len] = '\0';
}

// Load thumbnail for currently selected item
static void load_current_thumbnail() {
    if (selected_index < 0 || selected_index >= entry_count || entry_count == 0) {
        thumbnail_cache_valid = 0;
        return;
    }
    
    // Only load thumbnails for files, not directories
    if (entries[selected_index].is_dir) {
        thumbnail_cache_valid = 0;
        return;
    }
    
    char thumb_path[MAX_PATH_LEN];
    char file_path[MAX_PATH_LEN];
    
    // Check if we're in Recent games mode
    if (strcmp(current_path, "RECENT_GAMES") == 0) {
        // For recent games, we need to use the full_path from the RecentGame structure
        const RecentGame* recent_list = recent_games_get_list();
        int recent_count = recent_games_get_count();

        if (selected_index < recent_count) {
            const RecentGame *recent_game = &recent_list[selected_index];

            if (recent_game->full_path[0] != '\0') {
                snprintf(file_path, sizeof(file_path), "%s/%s/%s", roms_path, recent_game->full_path, recent_game->game_name);
            } else {
                // No full path available, skip thumbnail
                thumbnail_cache_valid = 0;
                return;
            }
        } else {
            // This is the ".." entry, no thumbnail
            thumbnail_cache_valid = 0;
            return;
        }
    } else if (strcmp(current_path, "FAVORITES") == 0) {
        // For favorites, we need to use the full_path from the FavoriteGame structure
        const FavoriteGame* favorites_list = favorites_get_list();
        int favorites_count = favorites_get_count();

        if (selected_index < favorites_count) {
            const FavoriteGame *favorite_game = &favorites_list[selected_index];

            if (favorite_game->full_path[0] != '\0') {
                snprintf(file_path, sizeof(file_path), "%s/%s/%s", roms_path, favorite_game->full_path, favorite_game->game_name);
            } else {
                // No full path available, skip thumbnail
                thumbnail_cache_valid = 0;
                return;
            }
        } else {
            // This is the ".." entry, no thumbnail
            thumbnail_cache_valid = 0;
            return;
        }
    } else {
        // Regular file browser mode
        strncpy(file_path, entries[selected_index].path, sizeof(file_path) - 1);
    }
    get_thumbnail_path(file_path, thumb_path, sizeof(thumb_path));

    bool is_wqw = false;
    is_zip_wqw_file(file_path, &is_wqw);

    // Check if we already have this thumbnail cached
    if (thumbnail_cache_valid && strcmp(cached_thumbnail_path, thumb_path) == 0) return; // Already cached 
    if ((is_wqw) && (thumbnail_cache_valid && strcmp(cached_thumbnail_path, file_path) == 0)) return; // Already cached
    
    // Free previous thumbnail
    if (thumbnail_cache_valid) {
        free_thumbnail(&current_thumbnail);
        thumbnail_cache_valid = 0;
    }
    
    // Try to load new thumbnail
    if (load_thumbnail(thumb_path, &current_thumbnail)) {
        strncpy(cached_thumbnail_path, thumb_path, sizeof(cached_thumbnail_path) - 1);
        cached_thumbnail_path[sizeof(cached_thumbnail_path) - 1] = '\0';
        thumbnail_cache_valid = 1;
    }

    if (!thumbnail_cache_valid && is_wqw) {
        uint16_t *wqw_image_buffer = malloc(g_preview_width * g_preview_height * sizeof(uint16_t));
        if (!wqw_image_buffer) return; // malloc failed

        if (load_rgb565_image(file_path, wqw_image_buffer, g_preview_width, g_preview_height) != 0) {
            free(wqw_image_buffer);
            return;
        }

        current_thumbnail.data = wqw_image_buffer;
        current_thumbnail.width = g_preview_width;
        current_thumbnail.height = g_preview_height;

        strncpy(cached_thumbnail_path, file_path, sizeof(cached_thumbnail_path) - 1);
        cached_thumbnail_path[sizeof(cached_thumbnail_path) - 1] = '\0';
        thumbnail_cache_valid = 1;
    }
}

// Check if path is a directory - optimized to use d_type first
static inline int is_directory_fast(const char *path, unsigned char d_type) {
    // Use d_type if available (much faster, no stat call needed)
    if (d_type != DT_UNKNOWN) {
        return (d_type == DT_DIR);
    }

    // Fallback to stat only if needed
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return 0;
}

// Comparison function to sort entries alphabetically by name
int compare_entries(const void *a, const void *b) {
    const MenuEntry *entry_a = (const MenuEntry *)a;
    const MenuEntry *entry_b = (const MenuEntry *)b;
    return strcmp(entry_a->name, entry_b->name);  // Compare by name
}

// Show recent games list
static void show_recent_games(void) {
    entry_count = 0;
    reset_navigation_state();
    
    // Set current_path so thumbnail loading knows we're in recent games mode
    strncpy(current_path, "RECENT_GAMES", sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
    
    // Clear thumbnail cache when switching to recent games mode
    thumbnail_cache_valid = 0;

    const RecentGame* recent_list = recent_games_get_list();
    int recent_count = recent_games_get_count();

    if (recent_count == 0) {
        // Only show back entry if no recent games
        ensure_entries_capacity(entry_count + 1);
        strncpy(entries[entry_count].name, "..", sizeof(entries[entry_count].name) - 1);
        strncpy(entries[entry_count].path, roms_path, sizeof(entries[entry_count].path) - 1);
        entries[entry_count].is_dir = 1;
        entry_count++;
    } else {
        // Add recent games first
        ensure_entries_capacity(entry_count + recent_count + 1);
        for (int i = 0; i < recent_count; i++) {
            strncpy(entries[entry_count].name, recent_list[i].display_name, sizeof(entries[entry_count].name) - 1);
            snprintf(entries[entry_count].path, sizeof(entries[entry_count].path),
                    "%s;%s", recent_list[i].core_name, recent_list[i].game_name);
            entries[entry_count].is_dir = 0;
            entry_count++;
        }

        // Add back entry after recent games
        strncpy(entries[entry_count].name, "..", sizeof(entries[entry_count].name) - 1);
        strncpy(entries[entry_count].path, roms_path, sizeof(entries[entry_count].path) - 1);
        entries[entry_count].is_dir = 1;
        entry_count++;
    }
    
    // Load thumbnail for initially selected item AND reset last_selected_index to prevent duplicate loading
    load_current_thumbnail();
    last_selected_index = selected_index;  // Prevent render loop from detecting this as a "change"
}

// Show favorites
static void show_favorites(void) {
    entry_count = 0;
    reset_navigation_state();

    // Set current_path so thumbnail loading knows we're in favorites mode
    strncpy(current_path, "FAVORITES", sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    // Clear thumbnail cache when switching to favorites mode
    thumbnail_cache_valid = 0;

    const FavoriteGame* favorites_list = favorites_get_list();
    int favorites_count = favorites_get_count();

    if (favorites_count == 0) {
        // Only show back entry if no favorites
        ensure_entries_capacity(entry_count + 1);
        strncpy(entries[entry_count].name, "..", sizeof(entries[entry_count].name) - 1);
        strncpy(entries[entry_count].path, roms_path, sizeof(entries[entry_count].path) - 1);
        entries[entry_count].is_dir = 1;
        entry_count++;
    } else {
        // Add favorites first
        ensure_entries_capacity(entry_count + favorites_count + 1);
        for (int i = 0; i < favorites_count; i++) {
            strncpy(entries[entry_count].name, favorites_list[i].display_name, sizeof(entries[entry_count].name) - 1);
            snprintf(entries[entry_count].path, sizeof(entries[entry_count].path),
                    "%s;%s", favorites_list[i].core_name, favorites_list[i].game_name);
            entries[entry_count].is_dir = 0;
            entry_count++;
        }

        // Add back entry after favorites
        strncpy(entries[entry_count].name, "..", sizeof(entries[entry_count].name) - 1);
        strncpy(entries[entry_count].path, roms_path, sizeof(entries[entry_count].path) - 1);
        entries[entry_count].is_dir = 1;
        entry_count++;
    }

    // Load thumbnail for initially selected item AND reset last_selected_index to prevent duplicate loading
    load_current_thumbnail();
    last_selected_index = selected_index;  // Prevent render loop from detecting this as a "change"
}

// Show tools menu
static void show_tools_menu(void) {
    entry_count = 0;
    reset_navigation_state();

    // Set current_path for tools mode
    strncpy(current_path, "TOOLS", sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    // Clear thumbnail cache when switching to tools mode
    thumbnail_cache_valid = 0;

    // Ensure we have space for 4 entries
    ensure_entries_capacity(4);

    // Add Hotkeys entry
    strncpy(entries[entry_count].name, "Hotkeys", sizeof(entries[entry_count].name) - 1);
    strncpy(entries[entry_count].path, "HOTKEYS", sizeof(entries[entry_count].path) - 1);
    entries[entry_count].is_dir = 1;
    entry_count++;

    // Add Credits entry
    strncpy(entries[entry_count].name, "Credits", sizeof(entries[entry_count].name) - 1);
    strncpy(entries[entry_count].path, "CREDITS", sizeof(entries[entry_count].path) - 1);
    entries[entry_count].is_dir = 1;
    entry_count++;

    // Add Utils entry
    strncpy(entries[entry_count].name, "Utils", sizeof(entries[entry_count].name) - 1);
    strncpy(entries[entry_count].path, "UTILS", sizeof(entries[entry_count].path) - 1);
    entries[entry_count].is_dir = 1;
    entry_count++;

    // Add back entry
    strncpy(entries[entry_count].name, "..", sizeof(entries[entry_count].name) - 1);
    strncpy(entries[entry_count].path, roms_path, sizeof(entries[entry_count].path) - 1);
    entries[entry_count].is_dir = 1;
    entry_count++;

    // Load thumbnail for initially selected item AND reset last_selected_index to prevent duplicate loading
    load_current_thumbnail();
    last_selected_index = selected_index;  // Prevent render loop from detecting this as a "change"
}

// Show utils menu with js2000 files
static void show_utils_menu(void) {
    entry_count = 0;
    reset_navigation_state();
    
    // Set current_path for utils mode
    strncpy(current_path, "UTILS", sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
    
    // Clear thumbnail cache when switching to utils mode
    thumbnail_cache_valid = 0;
    
    // Scan js2000 directory for files
    char js2000_path[MAX_PATH_LEN];
    snprintf(js2000_path, sizeof(js2000_path), "%s/js2000", roms_path);

    DIR *dir = opendir(js2000_path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;  // Skip hidden files

            char full_path[MAX_PATH_LEN];
            snprintf(full_path, sizeof(full_path), "%s/%s", js2000_path, ent->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                ensure_entries_capacity(entry_count + 1);
                strncpy(entries[entry_count].name, ent->d_name, sizeof(entries[entry_count].name) - 1);
                strncpy(entries[entry_count].path, full_path, sizeof(entries[entry_count].path) - 1);
                entries[entry_count].is_dir = S_ISDIR(st.st_mode);
                entry_count++;
            }
        }
        closedir(dir);
    }

    // Add "Rebuild folder cache" option
    ensure_entries_capacity(entry_count + 1);
    strncpy(entries[entry_count].name, "Rebuild folder cache", sizeof(entries[entry_count].name) - 1);
    strncpy(entries[entry_count].path, "REBUILD_CACHE", sizeof(entries[entry_count].path) - 1);
    entries[entry_count].is_dir = 0;
    entry_count++;

    // Add back entry
    ensure_entries_capacity(entry_count + 1);
    strncpy(entries[entry_count].name, "..", sizeof(entries[entry_count].name) - 1);
    strncpy(entries[entry_count].path, "TOOLS", sizeof(entries[entry_count].path) - 1);
    entries[entry_count].is_dir = 1;
    entry_count++;
    
    // Load thumbnail for initially selected item
    load_current_thumbnail();
    last_selected_index = selected_index;
}

// Show hotkeys screen
static void show_hotkeys_screen(void) {
    // Set current_path for hotkeys mode
    strncpy(current_path, "HOTKEYS", sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    // Clear thumbnail cache and entries for hotkeys mode
    thumbnail_cache_valid = 0;
    entry_count = 0;
    reset_navigation_state();
}

// Show credits screen
static void show_credits_screen(void) {
    // Set current_path for credits mode
    strncpy(current_path, "CREDITS", sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
    
    // Clear thumbnail cache and entries for credits mode
    thumbnail_cache_valid = 0;
    entry_count = 0;
    reset_navigation_state();
}

// Scan directory and populate entries
static void scan_directory(const char *path) {
    DIR *dir;
    struct dirent *ent;

    entry_count = 0;
    reset_navigation_state();

    // Store whether we're at root for recent games insertion later
    int is_root = (strcmp(path, roms_path) == 0);

    // Add parent directory entry if not at root
    if (!is_root) {
        ensure_entries_capacity(entry_count + 1);
        strncpy(entries[entry_count].name, "..", sizeof(entries[entry_count].name) - 1);
        strncpy(entries[entry_count].path, path, sizeof(entries[entry_count].path) - 1);
        entries[entry_count].is_dir = 1;
        entry_count++;
    }

    dir = opendir(path);
    if (!dir) {
        return;
    }

    // Collect all entries in a single pass - optimized
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;  // Skip hidden files

        // Skip frogui, and saves folders
        if (strcasecmp(ent->d_name, "frogui") == 0 || strcasecmp(ent->d_name, "saves") == 0 || strcasecmp(ent->d_name, "save") == 0) {
            continue;
        }

        // Save entry name and type BEFORE any nested readdir calls (readdir uses static buffer)
        char entry_name[256];
        strncpy(entry_name, ent->d_name, sizeof(entry_name) - 1);
        entry_name[sizeof(entry_name) - 1] = '\0';
        int entry_type = ent->d_type;

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry_name);

        // Fast path: use d_type if available, avoid stat() calls
        int is_dir = is_directory_fast(full_path, entry_type);

        // Skip files if in root ROMS directory (only show folders there)
        if (is_root && !is_dir) {
            continue;
        }

        // Skip empty directories in root ROMS directory (use cache for speed)
        if (is_root && is_dir) {
            if (hide_empty_folders) {
                // Load cache on first use (default to hiding if setting not found)
                load_empty_dirs_cache();
                if (is_in_empty_cache(entry_name)) {
                    continue; // Skip cached empty directory
                }
            }
        }

        // Ensure we have space for one more entry
        ensure_entries_capacity(entry_count + 1);

        // Add directories first, then files
        if (is_dir) {
            // Add directory entry
            strncpy(entries[entry_count].name, entry_name, sizeof(entries[entry_count].name) - 1);
            strncpy(entries[entry_count].path, full_path, sizeof(entries[entry_count].path) - 1);
            entries[entry_count].is_dir = 1;
            entry_count++;
        } else {
            // Add file entry
            strncpy(entries[entry_count].name, entry_name, sizeof(entries[entry_count].name) - 1);
            strncpy(entries[entry_count].path, full_path, sizeof(entries[entry_count].path) - 1);
            entries[entry_count].is_dir = 0;
            entry_count++;
        }
    }

    // Close the directory after reading
    closedir(dir);

    // Sort all entries alphabetically by name
    qsort(entries, entry_count, sizeof(MenuEntry), compare_entries);

    // Add Recent games at the very top if in root directory
    if (is_root) {
        // Ensure we have space for 4 more entries (Recent games, Favorites, Random game, Tools)
        ensure_entries_capacity(entry_count + 4);

        // Shift all entries down by 1 to make room for Recent games at index 0
        for (int i = entry_count; i > 0; i--) {
            entries[i] = entries[i - 1];
        }

        // Insert Recent games at the top
        strncpy(entries[0].name, "Recent games", sizeof(entries[0].name) - 1);
        strncpy(entries[0].path, "RECENT_GAMES", sizeof(entries[0].path) - 1);
        entries[0].is_dir = 1;
        entry_count++;

        // Shift entries down by 1 more to make room for Favorites
        for (int i = entry_count; i > 1; i--) {
            entries[i] = entries[i - 1];
        }

        // Insert Favorites at position 1 (right after Recent games)
        strncpy(entries[1].name, "Favorites", sizeof(entries[1].name) - 1);
        strncpy(entries[1].path, "FAVORITES", sizeof(entries[1].path) - 1);
        entries[1].is_dir = 1;
        entry_count++;

        // Shift entries down by 1 more to make room for Random Game
        for (int i = entry_count; i > 2; i--) {
            entries[i] = entries[i - 1];
        }

        // Insert Random Game at position 2 (right after Favorites)
        strncpy(entries[2].name, "Random game", sizeof(entries[2].name) - 1);
        strncpy(entries[2].path, "RANDOM_GAME", sizeof(entries[2].path) - 1);
        entries[2].is_dir = 1;
        entry_count++;

        // Add Tools at the bottom
        strncpy(entries[entry_count].name, "Tools", sizeof(entries[entry_count].name) - 1);
        strncpy(entries[entry_count].path, "TOOLS", sizeof(entries[entry_count].path) - 1);
        entries[entry_count].is_dir = 1;
        entry_count++;
    }

    // Defer thumbnail loading to first render for faster boot
    // The render loop will handle loading thumbnails on the first frame
    thumbnail_cache_valid = 0;
    last_selected_index = -1;  // Force load on first render
}

// Render settings menu
static void render_settings_menu() {
    // If saving, show saving overlay
    if (settings_is_saving()) {
        const char* saving_text = "SAVING...";
        int text_width = font_measure_text(saving_text);
        int x = (SCREEN_WIDTH - text_width) / 2;
        int y = (SCREEN_HEIGHT - FONT_CHAR_HEIGHT) / 2;

        render_text_pillbox(framebuffer, x, y, saving_text, theme_header(), theme_bg(), 6);
        return;
    }

    // Draw title
    if (show_multicore_opt) font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 10, os_settings_title, COLOR_HEADER);
    else font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 10, "CORE SETTINGS", COLOR_HEADER);

    // Draw the label in top-right
    char entry_label[20];
    snprintf(entry_label, sizeof(entry_label), "SEL - SWAP");
    int label_width = font_measure_text(entry_label);
    int label_x = SCREEN_WIDTH - label_width - 12;  // Right-aligned, just above the legend
    int label_y = 8;  // Position it slightly below the top edge
    render_text_pillbox(framebuffer, label_x, label_y, entry_label, COLOR_LEGEND_BG, COLOR_LEGEND, 6);

    int settings_count = settings_get_count();
    int start_y = 40;
    int selected_index = settings_get_selected_index();
    int scroll_offset = settings_get_scroll_offset();

    // Show settings options (two lines per option)
    // Reserve space for legend at bottom - ensure no overlap
    int max_visible = 3; // Reduced from 4 to ensure no overlap with legend
    for (int i = 0; i < max_visible && (scroll_offset + i) < settings_count; i++) {
        int option_index = scroll_offset + i;
        const SettingsOption *option = settings_get_option(option_index);
        if (!option) continue;

        int y_name = start_y + (i * ITEM_HEIGHT * 2);
        int y_value = y_name + ITEM_HEIGHT;

        // Check if this option is selected
        int is_selected = (option_index == selected_index);

        // Draw setting name (always white)
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y_name, option->name, COLOR_TEXT);

        // Draw setting value with selection background and arrows
        if (is_selected) {
            // Format value with arrows: "< current_value >"
            char value_text[256];
            snprintf(value_text, sizeof(value_text), "< %s >", option->current_value);

            // Use unified pillbox rendering
            render_text_pillbox(framebuffer, PADDING, y_value, value_text, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 6);
        } else {
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y_value, option->current_value, COLOR_TEXT);
        }
    }

    // Draw legend with pillbox highlighting
    const char *legend = " A - SAVE   B - EXIT   Y - RESET ";
    int legend_y = SCREEN_HEIGHT - 24;

    // Calculate width and position (right-aligned)
    int legend_width = font_measure_text(legend);
    int legend_x = SCREEN_WIDTH - legend_width - 12;

    // Draw legend pill with rounded corners
    render_rounded_rect(framebuffer, legend_x - 4, legend_y - 2, legend_width + 8, 20, 10, COLOR_LEGEND_BG);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, legend_x, legend_y, legend, COLOR_LEGEND);
}

// Render hotkeys screen
static void render_hotkeys_screen() {
    // Draw title
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 10, "HOTKEYS", COLOR_HEADER);

    // Draw hotkey information
    int start_y = 50;
    int line_height = 24;

    // Hotkeys text
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y, "SAVE STATE: L + R + X", COLOR_TEXT);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y + line_height, "LOAD STATE: L + R + Y", COLOR_TEXT);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y + line_height * 2, "NEXT SLOT: L + R + >", COLOR_TEXT);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y + line_height * 3, "PREV SLOT: L + R + <", COLOR_TEXT);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y + line_height * 4, "SCREENSHOT: L + R + START", COLOR_TEXT);
    
    // Draw legend
    const char *legend = " B - BACK ";
    int legend_y = SCREEN_HEIGHT - 24;
    int legend_width = font_measure_text(legend);
    int legend_x = SCREEN_WIDTH - legend_width - 12;
    
    render_rounded_rect(framebuffer, legend_x - 4, legend_y - 2, legend_width + 8, 20, 10, COLOR_LEGEND_BG);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, legend_x, legend_y, legend, COLOR_LEGEND);
}

// Render credits screen
static void render_credits_screen() {
    // Draw title
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 10, "CREDITS", COLOR_HEADER);
    
    // Draw credits information
    int start_y = 50;
    int line_height = 24;
    
    // Credits text with pillboxes for sections
    // FrogUI Dev & Idea section
    const char *section1 = " FrogUI Dev & Idea ";
    int section1_width = font_measure_text(section1);
    render_rounded_rect(framebuffer, PADDING - 4, start_y - 2, section1_width + 8, 20, 10, COLOR_HEADER);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y, section1, COLOR_BG);
    
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y + line_height, "Prosty & Desoxyn", COLOR_TEXT);
    
    // Design section
    const char *section2 = " Design ";
    int section2_width = font_measure_text(section2);
    render_rounded_rect(framebuffer, PADDING - 4, start_y + line_height * 2 - 2, section2_width + 8, 20, 10, COLOR_HEADER);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y + line_height * 2, section2, COLOR_BG);
    
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, start_y + line_height * 3, "Q_ta", COLOR_TEXT);
    
    // Draw legend
    const char *legend = " B - BACK ";
    int legend_y = SCREEN_HEIGHT - 24;
    int legend_width = font_measure_text(legend);
    int legend_x = SCREEN_WIDTH - legend_width - 12;
    
    render_rounded_rect(framebuffer, legend_x - 4, legend_y - 2, legend_width + 8, 20, 10, COLOR_LEGEND_BG);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, legend_x, legend_y, legend, COLOR_LEGEND);
}

void clean_path(char *path) {
    char prefix[64];
    strncpy(prefix, roms_path, sizeof(prefix) - 1);
    size_t len = strlen(prefix);

    // remove prefix
    if (strncmp(path, prefix, len) == 0) {
        memmove(path, path + len, strlen(path + len) + 1);
    }

    // remove filename
    char *last_slash = strrchr(path, '/');
    if (last_slash) {
        *last_slash = '\0';
    }
}

typedef struct {
    char display_name[256]; // currently rendered scrolling text
    int scroll_offset;      // current scroll offset in pixels
    int scroll_direction;   // 1 = forward, -1 = backward
    uint32_t last_scroll_time;
    int valid;              // flag to indicate state is valid
} ScrollingTextState;

ScrollingTextState selected_item_scroll;
#define SCROLL_DELAY_MS 500   // milliseconds per scroll step
#define MAX_TEXT_WIDTH 200    // width in pixels of the menu text area

void update_selected_item_scroll() {
    if (selected_index < 0 || selected_index >= entry_count) return;

    const char *text = entries[selected_index].name;
    int text_width = font_measure_text(text);

    // If text fits, no scroll needed
    if (text_width <= MAX_TEXT_WIDTH) {
        strcpy(selected_item_scroll.display_name, text);
        selected_item_scroll.scroll_offset = 0;
        selected_item_scroll.scroll_direction = 1;
        selected_item_scroll.last_scroll_time = os_get_tick_count();
        selected_item_scroll.valid = 1;
        return;
    }

    // Get current time in ms
    uint32_t now = os_get_tick_count();

    // Update offset if enough time has passed
    if (now - selected_item_scroll.last_scroll_time >= SCROLL_DELAY_MS) {
        selected_item_scroll.last_scroll_time = now;
        selected_item_scroll.scroll_offset += selected_item_scroll.scroll_direction;

        // Reverse direction when text reaches bounds
        if (selected_item_scroll.scroll_offset + MAX_TEXT_WIDTH >= text_width) {
            selected_item_scroll.scroll_direction = -1;
        } else if (selected_item_scroll.scroll_offset <= 0) {
            selected_item_scroll.scroll_direction = 1;
        }
    }

    // Update the displayed scrolling text
    get_scrolling_text(text, 1, selected_item_scroll.display_name, sizeof(selected_item_scroll.display_name));
    selected_item_scroll.valid = 1;
}

void render_selected_item_text() {
    if (!selected_item_scroll.valid) return;
    if (selected_index < scroll_offset || selected_index >= scroll_offset + VISIBLE_ENTRIES) return;

    int is_favorited = 0;
    char filename[256];
    char directory[256];
    strcpy(directory, entries[selected_index].path);
    clean_path(directory);
    char *filename_path = strrchr(entries[selected_index].path, '/');
    snprintf(filename, sizeof(filename), "%s", filename_path ? filename_path + 1 : entries[selected_index].name);
    is_favorited = favorites_is_favorited(directory, filename);
    render_menu_item(framebuffer, selected_index, selected_item_scroll.display_name, entries[selected_index].is_dir, 1, scroll_offset, is_favorited);
}

static bool block_scrolling = false;
static uint32_t scrolling_delay_time = 0;

// Render the menu using modular render system
static void render_menu() {
    block_scrolling = true;
    render_clear_screen(framebuffer);

    // If game is queued, just show loading screen
    if (game_queued) {
        // Show centered loading pillbox
        const char* loading_text = "LOADING...";
        int text_width = font_measure_text(loading_text);
        int x = (SCREEN_WIDTH - text_width) / 2;
        int y = (SCREEN_HEIGHT - FONT_CHAR_HEIGHT) / 2;
        
        // Use unified pillbox rendering
        render_text_pillbox(framebuffer, x, y, loading_text, theme_header(), theme_bg(), 6);
        return;
    }

    // If settings are active, render settings menu
    if (settings_is_active()) {
        render_settings_menu();
        return;
    }
    
    // If in hotkeys mode, render hotkeys screen
    if (strcmp(current_path, "HOTKEYS") == 0) {
        render_hotkeys_screen();
        return;
    }
    
    // If in credits mode, render credits screen
    if (strcmp(current_path, "CREDITS") == 0) {
        render_credits_screen();
        return;
    }

    // Draw header with current folder name
    const char *display_path = current_path;
    if (strcmp(current_path, roms_path) == 0) {
        display_path = "FROGUI: SYSTEMS";  // Marketing branding
    } else {
        // Show just the folder name, not full path
        display_path = get_basename(current_path);
    }
    render_header(framebuffer, display_path);

    // Adjust the scroll_offset if necessary to keep the selected item visible
    if (selected_index < scroll_offset) {
        scroll_offset = selected_index;  // Scroll up to make the item visible
    } else if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
        scroll_offset = selected_index - VISIBLE_ENTRIES + 1;  // Scroll down to make the item visible
    }

    // Load and display thumbnail for selected item FIRST (background layer)
    // Only reload if selection changed
    if (last_selected_index != selected_index) {
        load_current_thumbnail();
        last_selected_index = selected_index;
        // Reset scrolling state for new selection
        text_scroll_frame_counter = 0;
        text_scroll_offset = 0;
        text_scroll_direction = 1;
    }
    
    if (thumbnail_cache_valid) {
        render_thumbnail(framebuffer, &current_thumbnail);
    }

    // Draw menu entries ON TOP of thumbnail
    for (int i = scroll_offset; i < entry_count && i < scroll_offset + VISIBLE_ENTRIES; i++) {
        // Get display name (with scrolling for selected item)
        char display_name[256];
        get_scrolling_text(entries[i].name, (i == selected_index), display_name, sizeof(display_name));

        // Check if this item is favorited
        int is_favorited = 0;
        if (!entries[i].is_dir &&
            strcmp(current_path, roms_path) != 0 &&
            strcmp(current_path, "RECENT_GAMES") != 0 &&
            strcmp(current_path, "FAVORITES") != 0 &&
            strcmp(current_path, "TOOLS") != 0 &&
            strcmp(current_path, "UTILS") != 0 &&
            strcmp(current_path, "HOTKEYS") != 0 &&
            strcmp(current_path, "CREDITS") != 0) {
            
            char filename[256];
            char directory[256];
            strcpy(directory, entries[i].path);
            clean_path(directory);
            char *filename_path = strrchr(entries[i].path, '/');
            if (filename_path) snprintf(filename, sizeof(filename), "%s", filename_path + 1);
            else snprintf(filename, sizeof(filename), "%s", entries[i].name);

            is_favorited = favorites_is_favorited(directory, filename);
        }

        render_menu_item(framebuffer, i, display_name, entries[i].is_dir, (i == selected_index), scroll_offset, is_favorited);
    }

    // Draw legend - determine X button mode based on current view
    int x_button_mode = LEGEND_X_NONE;
    if (strcmp(current_path, "FAVORITES") == 0) {
        // In favorites menu, show "X - REMOVE"
        x_button_mode = LEGEND_X_REMOVE;
    } else if (strcmp(current_path, roms_path) != 0 &&
               strcmp(current_path, "RECENT_GAMES") != 0 &&
               strcmp(current_path, "TOOLS") != 0 &&
               strcmp(current_path, "UTILS") != 0 &&
               strcmp(current_path, "HOTKEYS") != 0 &&
               strcmp(current_path, "CREDITS") != 0) {
        // In ROM directories, show "X - FAVOURITE"
        x_button_mode = LEGEND_X_FAVOURITE;
    }
    render_legend(framebuffer, x_button_mode);

    // Draw the "current entry/total entries" label in top-right, above the legend
    char entry_label[20];
    snprintf(entry_label, sizeof(entry_label), "%d/%d", selected_index + 1, entry_count); // 1-based indexing for display
    int label_width = font_measure_text(entry_label);
    int label_x = SCREEN_WIDTH - label_width - 12;  // Right-aligned, just above the legend
    int label_y = 8;  // Position it slightly below the top edge
    render_text_pillbox(framebuffer, label_x, label_y, entry_label, COLOR_LEGEND_BG, COLOR_LEGEND, 6);

    // Draw A-Z picker overlay if active
    if (az_picker_active) {
        // Draw centered background box using theme background color
        int box_width = 280;
        int box_height = 180;
        int box_x = (SCREEN_WIDTH - box_width) / 2;
        int box_y = (SCREEN_HEIGHT - box_height) / 2;
        render_fill_rect(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, box_x, box_y, box_width, box_height, COLOR_BG);

        // Draw title using theme colors
        const char *title = "QUICK JUMP";
        int title_width = font_measure_text(title);
        int title_x = (SCREEN_WIDTH - title_width) / 2;
        render_text_pillbox(framebuffer, title_x, 30, title, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 6);

        // Draw A-Z grid (7 columns x 4 rows = 28 slots)
        const char *labels[] = {
            "A", "B", "C", "D", "E", "F", "G",
            "H", "I", "J", "K", "L", "M", "N",
            "O", "P", "Q", "R", "S", "T", "U",
            "V", "W", "X", "Y", "Z", "0-9", "#"
        };

        int grid_start_x = 40;
        int grid_start_y = 70;
        int col_width = 38;
        int row_height = 30;

        for (int i = 0; i < 28; i++) {
            int col = i % 7;
            int row = i / 7;
            int x = grid_start_x + col * col_width;
            int y = grid_start_y + row * row_height;

            if (i == az_selected_index) {
                render_text_pillbox(framebuffer, x, y, labels[i], COLOR_SELECT_BG, COLOR_SELECT_TEXT, 6);
            } else {
                font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, x, y, labels[i], COLOR_TEXT);
            }
        }
    } else {
        scrolling_delay_time = os_get_tick_count();
        block_scrolling = false;
    }
}

// Pick and launch a random game by randomly navigating the menu
static void pick_random_game(void) {
    int max_attempts = 100;
    int attempts = 0;

    while (attempts < max_attempts) {
        attempts++;

        // Pick a random console directory from root
        strncpy(current_path, roms_path, sizeof(current_path) - 1);
        scan_directory(current_path);

        // Filter out non-console entries
        int valid_console_count = 0;
        for (int i = 0; i < entry_count; i++) {
            if (entries[i].is_dir &&
                strcmp(entries[i].path, "RECENT_GAMES") != 0 &&
                strcmp(entries[i].path, "FAVORITES") != 0 &&
                strcmp(entries[i].path, "RANDOM_GAME") != 0 &&
                strcmp(entries[i].path, "TOOLS") != 0) {
                valid_console_count++;
            }
        }

        if (valid_console_count == 0) {
            strncpy(current_path, roms_path, sizeof(current_path) - 1);
            scan_directory(current_path);
            return;
        }

        // Pick a random console directory
        int random_console = rand() % valid_console_count;
        int console_idx = 0;
        for (int i = 0; i < entry_count; i++) {
            if (entries[i].is_dir &&
                strcmp(entries[i].path, "RECENT_GAMES") != 0 &&
                strcmp(entries[i].path, "FAVORITES") != 0 &&
                strcmp(entries[i].path, "RANDOM_GAME") != 0 &&
                strcmp(entries[i].path, "TOOLS") != 0) {
                if (console_idx == random_console) {
                    strncpy(current_path, entries[i].path, sizeof(current_path) - 1);
                    break;
                }
                console_idx++;
            }
        }

        // Scan the console directory
        scan_directory(current_path);

        // Count files (not directories, not ..)
        int file_count = 0;
        for (int i = 0; i < entry_count; i++) {
            if (!entries[i].is_dir && strcmp(entries[i].name, "..") != 0) {
                file_count++;
            }
        }

        if (file_count == 0) {
            continue;
        }

        // Pick a random file
        int random_file = rand() % file_count;
        int file_idx = 0;
        for (int i = 0; i < entry_count; i++) {
            if (!entries[i].is_dir && strcmp(entries[i].name, "..") != 0) {
                if (file_idx == random_file) {
                    char core_name[256];
                    char filename[256];
                    char directory[256];
                    get_corename(entries[i].path, core_name, sizeof(core_name));
                    strcpy(directory, entries[i].path);
                    clean_path(directory);
                    char *filename_path = strrchr(entries[i].path, '/');
                    if (filename_path) snprintf(filename, sizeof(filename), "%s", filename_path + 1);
                    else snprintf(filename, sizeof(filename), "%s", entries[i].name);

                    init_direct_loader(core_name, directory, filename);
                    return;
                }
                file_idx++;
            }
        }
    }

    strncpy(current_path, roms_path, sizeof(current_path) - 1);
    scan_directory(current_path);
}

/* =========================
   WAV STRUCT + HELPERS
   ========================= */

typedef struct {
    int sample_rate;
    int channels;
    int bits_per_sample;
    int num_samples;
    uint8_t *data;
} Wav;

static int read_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static int read_le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

bool wav_load(const uint8_t *buf, size_t size, Wav *out)
{
    if (size < 44) return false;
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
        return false;

    int pos = 12;

    while (pos + 8 <= (int)size)
    {
        int chunk_size = read_le32(buf + pos + 4);

        if (!memcmp(buf + pos, "fmt ", 4))
        {
            int format = read_le16(buf + pos + 8);
            out->channels = read_le16(buf + pos + 10);
            out->sample_rate = read_le32(buf + pos + 12);
            out->bits_per_sample = read_le16(buf + pos + 22);
            if (format != 1) return false; /* PCM only */
        }
        else if (!memcmp(buf + pos, "data", 4))
        {
            out->data = (uint8_t *)(buf + pos + 8);
            out->num_samples = chunk_size /
                (out->channels * (out->bits_per_sample / 8));
            return true;
        }

        pos += 8 + chunk_size;
    }

    return false;
}

/* =========================
   FILE LOADER
   ========================= */

bool load_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    *out_data = data;
    *out_size = size;
    return true;
}

/* =========================
   AUDIO ENGINE
   ========================= */

#define AUDIO_FRAMES 1024
#define MAX_SFX 8

typedef struct {
    const Wav *wav;
    int pos;
    int volume;   /* 0–256 */
    bool active;
} SfxVoice;

/* --- BGM --- */
static const Wav *bgm_wav = NULL;
static int bgm_pos = 0;
static int bgm_volume = 128;
static bool bgm_playing = false;

/* --- SFX --- */
static SfxVoice sfx[MAX_SFX];

/* =========================
   CONTROL FUNCTIONS
   ========================= */

void bgm_play(const Wav *wav, int volume)
{
    bgm_wav = wav;
    bgm_pos = 0;
    bgm_volume = volume;
    bgm_playing = true;
}

void bgm_stop(void)
{
    bgm_playing = false;
}

void sfx_play(const Wav *wav, int volume)
{
    for (int i = 0; i < MAX_SFX; i++)
    {
        if (!sfx[i].active)
        {
            sfx[i].wav = wav;
            sfx[i].pos = 0;
            sfx[i].volume = volume;
            sfx[i].active = true;
            return;
        }
    }
}

/* =========================
   MIXER
   ========================= */

static inline int16_t clamp16(int v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

void output_wav_audio(void)
{
    if (!audio_batch_cb)
        return;

    int16_t buffer[AUDIO_FRAMES * 2] = {0};

    for (int i = 0; i < AUDIO_FRAMES; i++)
    {
        int mix_l = 0;
        int mix_r = 0;

        /* --- BGM (looping) --- */
        if (bgm_playing && bgm_wav)
        {
            if (bgm_pos >= bgm_wav->num_samples)
                bgm_pos = 0;

            int16_t l = 0, r = 0;

            if (bgm_wav->bits_per_sample == 16)
            {
                int16_t *pcm = (int16_t *)bgm_wav->data;
                if (bgm_wav->channels == 1)
                    l = r = pcm[bgm_pos];
                else {
                    l = pcm[bgm_pos * 2 + 0];
                    r = pcm[bgm_pos * 2 + 1];
                }
            }
            else
            {
                uint8_t *pcm = bgm_wav->data;
                if (bgm_wav->channels == 1)
                    l = r = ((int)pcm[bgm_pos] - 128) << 8;
                else {
                    l = ((int)pcm[bgm_pos * 2 + 0] - 128) << 8;
                    r = ((int)pcm[bgm_pos * 2 + 1] - 128) << 8;
                }
            }

            mix_l += (l * bgm_volume) >> 8;
            mix_r += (r * bgm_volume) >> 8;
            bgm_pos++;
        }

        /* --- SFX (one-shot) --- */
        for (int v = 0; v < MAX_SFX; v++)
        {
            SfxVoice *vc = &sfx[v];
            if (!vc->active) continue;

            if (vc->pos >= vc->wav->num_samples)
            {
                vc->active = false;
                continue;
            }

            int16_t l = 0, r = 0;

            if (vc->wav->bits_per_sample == 16)
            {
                int16_t *pcm = (int16_t *)vc->wav->data;
                if (vc->wav->channels == 1)
                    l = r = pcm[vc->pos];
                else {
                    l = pcm[vc->pos * 2 + 0];
                    r = pcm[vc->pos * 2 + 1];
                }
            }
            else
            {
                uint8_t *pcm = vc->wav->data;
                if (vc->wav->channels == 1)
                    l = r = ((int)pcm[vc->pos] - 128) << 8;
                else {
                    l = ((int)pcm[vc->pos * 2 + 0] - 128) << 8;
                    r = ((int)pcm[vc->pos * 2 + 1] - 128) << 8;
                }
            }

            mix_l += (l * vc->volume) >> 8;
            mix_r += (r * vc->volume) >> 8;
            vc->pos++;
        }

        buffer[i * 2 + 0] = clamp16(mix_l);
        buffer[i * 2 + 1] = clamp16(mix_r);
    }

    audio_batch_cb(buffer, AUDIO_FRAMES);
}

static Wav bgm;
static uint8_t *bgm_file;
static size_t bgm_file_size;

void audio_init(void) {
    char music_path[128];
    snprintf(music_path, sizeof(music_path), "%s/menu_music.wav", assets_dir);
    if (!load_file(music_path, &bgm_file, &bgm_file_size)) {
        bgm_enabled = false;
        return;
    }

    if (!wav_load(bgm_file, bgm_file_size, &bgm)) {
        bgm_enabled = false;
        return;
    }

    /* Start playing immediately */
    bgm_play(&bgm, 128);  // volume: 0–256
}

static Wav nav;
static uint8_t *nav_file;
static size_t nav_file_size;
bool nav_init_once = false;

void navigation_sfx(void) {
    if (!nav_init_once) {
        char nav_sound_path[128];
        snprintf(nav_sound_path, sizeof(nav_sound_path), "%s/navigation.wav", assets_dir);
        if (!load_file(nav_sound_path, &nav_file, &nav_file_size)) {
            sfx_enabled = false;
            return;
        }

        if (!wav_load(nav_file, nav_file_size, &nav)) {
            sfx_enabled = false;
            return;
        }
    
        nav_init_once = true;
    }
    sfx_play(&nav, 128);  // volume: 0–256
}

// Handle input
static void handle_input() {
    if (!input_poll_cb || !input_state_cb) return;

    input_poll_cb();

    // If game is queued, just show loading screen
    if (game_queued) {
        // Don't process any input
        return;
    }

    // Get current input state
    int up = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    int down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    int a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    int b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    int x = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
    int y = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
    int l = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
    int r = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
    int select = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);

    int left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    int right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

    if ((prev_input[0] && !up) || (prev_input[1] && !down) || (prev_input[7] && !left) || (prev_input[8] && !right)) { // Play audio for up down left and right
        if (sfx_enabled) navigation_sfx();
    }

    // Flag to determine if the menu needs to be redrawn
    int input_changed = (prev_input[0] != up) || (prev_input[1] != down) || 
                        (prev_input[2] != a) || (prev_input[3] != b) ||
                        (prev_input[4] != l) || (prev_input[5] != r) || 
                        (prev_input[6] != select) || (prev_input[7] != left) || 
                        (prev_input[8] != right) || (prev_input[9] != x) || 
                        (prev_input[10] != y);

    // Handle SELECT button to open settings (on button release)
    if (prev_input[6] && !select) {
        if (settings_is_active()) show_multicore_opt = !show_multicore_opt;
        if (show_multicore_opt) {
            // Main menu settings - reload and show multicore.opt
            settings_load();
            settings_show_menu();
        } else if (strcmp(current_path, roms_path) == 0) { 
            show_core_settings("FrogUI");
        } else {
            // Check if we're in a console folder that has core-specific settings
            char console_folder[256];
            const char *slash = strrchr(current_path, '/');
            if (slash && slash != current_path) {
                // Extract folder name from path like "/sdcard/ROMS/gb"
                strcpy(console_folder, slash + 1);
                const char *core_name = get_core_name_for_console(console_folder);
                if (core_name) {
                    // Show core-specific settings
                    show_core_settings(core_name);
                }
            }
        }
        prev_input[6] = select;
        render_menu();
        return;
    }
    
    // Check if settings menu should handle input
    if (settings_handle_input(prev_input[0] && !up, prev_input[1] && !down,
                            prev_input[7] && !left, prev_input[8] && !right,
                            prev_input[2] && !a, prev_input[3] && !b, prev_input[10] && !y)) {
        // Settings consumed the input, update prev_input and return
        prev_input[0] = up;
        prev_input[1] = down;
        prev_input[2] = a;
        prev_input[3] = b;
        prev_input[4] = l;
        prev_input[5] = r;
        prev_input[6] = select;
        prev_input[7] = left;
        prev_input[8] = right;
        prev_input[9] = x;
        prev_input[10] = y;
        if (input_changed) render_menu();
        return;
    }

    // Handle A-Z picker input
    if (az_picker_active) {
        // Navigate the A-Z grid
        if (prev_input[0] && !up) { // UP
            if (az_selected_index >= 7) az_selected_index -= 7;
        }
        if (prev_input[1] && !down) { // DOWN
            if (az_selected_index < 21) az_selected_index += 7;
        }
        if (prev_input[7] && !left) { // LEFT
            if (az_selected_index > 0) az_selected_index--;
        }
        if (prev_input[8] && !right) { // RIGHT
            if (az_selected_index < 27) az_selected_index++;
        }

        // A button - select letter and jump
        if (prev_input[2] && !a) {
            const char *search_chars[] = {
                "A", "B", "C", "D", "E", "F", "G",
                "H", "I", "J", "K", "L", "M", "N",
                "O", "P", "Q", "R", "S", "T", "U",
                "V", "W", "X", "Y", "Z", "0", ""
            };

            char first_char = search_chars[az_selected_index][0];

            // Find first entry starting with this letter (case insensitive)
            for (int i = 0; i < entry_count; i++) {
                char entry_first = entries[i].name[0];
                if (entry_first >= 'a' && entry_first <= 'z') {
                    entry_first = entry_first - 'a' + 'A'; // Convert to uppercase
                }

                // Handle 0-9 category
                if (az_selected_index == 26 && entry_first >= '0' && entry_first <= '9') {
                    selected_index = i;
                    break;
                }
                // Handle # category (special characters)
                else if (az_selected_index == 27 &&
                         !((entry_first >= 'A' && entry_first <= 'Z') ||
                           (entry_first >= '0' && entry_first <= '9'))) {
                    selected_index = i;
                    break;
                }
                // Handle A-Z
                else if (az_selected_index < 26 && entry_first == first_char) {
                    selected_index = i;
                    break;
                }
            }

            az_picker_active = 0;
        }

        // B button - cancel
        if (prev_input[3] && !b) {
            az_picker_active = 0;
        }

        // Update prev_input and return (picker consumed input)
        prev_input[0] = up;
        prev_input[1] = down;
        prev_input[2] = a;
        prev_input[3] = b;
        prev_input[7] = left;
        prev_input[8] = right;
        if (input_changed) render_menu();
        return;
    }

    // Handle RIGHT button to open A-Z picker (on button release)
    if (prev_input[8] && !right) {
        // Don't activate in special menus
        if (strcmp(current_path, "RECENT_GAMES") != 0 &&
            strcmp(current_path, "FAVORITES") != 0 &&
            strcmp(current_path, "TOOLS") != 0 &&
            strcmp(current_path, "UTILS") != 0 &&
            strcmp(current_path, "HOTKEYS") != 0 &&
            strcmp(current_path, "CREDITS") != 0 &&
            entry_count > 0) {
            az_picker_active = 1;
            az_selected_index = 0;
        }
    }

    // Handle up (on button release)
    if (prev_input[0] && !up) {
        if (selected_index > 0) {
            selected_index--;
        } else {
            // Loop to the last entry when at the top
            selected_index = entry_count - 1;
        }
        // Adjust scroll_offset if necessary
        if (selected_index < scroll_offset) {
            scroll_offset = selected_index;
        }
    }

    // Handle down (on button release)
    if (prev_input[1] && !down) {
        if (selected_index < entry_count - 1) {
            selected_index++;
        } else {
            // Loop to the first entry when at the bottom
            selected_index = 0;
        }
        // Adjust scroll_offset if necessary
        if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
            scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
        }
    }

    // Handle L button (move up by 7 entries)
    if (prev_input[4] && !l) {
        if (selected_index >= 7) {
            selected_index -= 7;
        } else {
            // Loop to the bottom when reaching the top
            selected_index = entry_count - (7 - selected_index);
        }
        // Adjust scroll_offset if necessary
        if (selected_index < scroll_offset) {
            scroll_offset = selected_index;
        }
    }

    // Handle R button (move down by 7 entries)
    if (prev_input[5] && !r) {
        if (selected_index < entry_count - 7) {
            selected_index += 7;
        } else {
            // Loop to the top when reaching the bottom
            selected_index = (selected_index + 7) % entry_count;  // Wrap around to the top
        }
        // Adjust scroll_offset if necessary
        if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
            scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
        }
    }

    // Handle X button (toggle favorite / remove from favorites) - on button release
    if (prev_input[9] && !x && entry_count > 0) {
        MenuEntry *entry = &entries[selected_index];

        // Handle removing from favorites when in FAVORITES view
        if (strcmp(current_path, "FAVORITES") == 0) {
            // Don't allow removing the ".." back entry
            if (!entry->is_dir && strcmp(entry->name, "..") != 0) {
                // Remove this favorite by index
                favorites_remove_by_index(selected_index);

                // Refresh the favorites list
                show_favorites();

                // Adjust selection if needed
                int new_count = favorites_get_count();
                if (new_count == 0) {
                    selected_index = 0; // Select the ".." entry
                } else if (selected_index >= new_count) {
                    selected_index = new_count - 1;
                }

                // Reset scroll offset if needed
                if (selected_index < scroll_offset) {
                    scroll_offset = selected_index;
                }
            }
        }
        // Only allow favoriting in ROM directories (not in special menus)
        else if (!entry->is_dir &&
            strcmp(current_path, "RECENT_GAMES") != 0 &&
            strcmp(current_path, "TOOLS") != 0 &&
            strcmp(current_path, "UTILS") != 0 &&
            strcmp(current_path, "HOTKEYS") != 0 &&
            strcmp(current_path, "CREDITS") != 0 &&
            strcmp(current_path, roms_path) != 0) {

            // Get core name and filename
            char core_name[256];
            char filename[256];
            char directory[256];
            get_corename(entry->path, core_name, sizeof(core_name));
            strcpy(directory, entry->path);
            clean_path(directory);
            char *filename_path = strrchr(entry->path, '/');
            if (filename_path) snprintf(filename, sizeof(filename), "%s", filename_path + 1);
            else snprintf(filename, sizeof(filename), "%s", entry->name);

            // Toggle favorite
            favorites_toggle(core_name, filename, directory);
        }
    }

    // Handle A button (select) - on button release
    if (prev_input[2] && !a && entry_count > 0) {
        MenuEntry *entry = &entries[selected_index];

        if (strcmp(entry->name, "..") == 0) {
            // Go to parent directory
            char *last_slash = strrchr(current_path, '/');
            if (last_slash && last_slash != current_path) {
                // Remember which directory we're leaving so we can restore position
                char prev_dir[256];
                strncpy(prev_dir, last_slash + 1, sizeof(prev_dir) - 1);
                prev_dir[sizeof(prev_dir) - 1] = '\0';

                *last_slash = '\0';
                scan_directory(current_path);

                // Find the directory we just left and restore selection to it
                for (int i = 0; i < entry_count; i++) {
                    if (strcmp(entries[i].name, prev_dir) == 0) {
                        selected_index = i;
                        // Update scroll offset to keep selection visible
                        if (selected_index < scroll_offset) {
                            scroll_offset = selected_index;
                        } else if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
                            scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
                        }
                        break;
                    }
                }
            }
        } else if (entry->is_dir) {
            // Enter directory
            if (strcmp(entry->path, "RECENT_GAMES") == 0) {
                // Show recent games list
                show_recent_games();
                strncpy(current_path, "RECENT_GAMES", sizeof(current_path) - 1);
            } else if (strcmp(entry->path, "FAVORITES") == 0) {
                // Show favorites list
                show_favorites();
                strncpy(current_path, "FAVORITES", sizeof(current_path) - 1);
            } else if (strcmp(entry->path, "RANDOM_GAME") == 0) {
                // Pick and launch a random game
                pick_random_game();
                render_menu();
                return;
            } else if (strcmp(entry->path, "TOOLS") == 0) {
                // Show tools menu
                show_tools_menu();
                strncpy(current_path, "TOOLS", sizeof(current_path) - 1);
            } else if (strcmp(entry->path, "HOTKEYS") == 0) {
                // Show hotkeys screen
                show_hotkeys_screen();
                strncpy(current_path, "HOTKEYS", sizeof(current_path) - 1);
            } else if (strcmp(entry->path, "CREDITS") == 0) {
                // Show credits screen
                show_credits_screen();
                strncpy(current_path, "CREDITS", sizeof(current_path) - 1);
            } else if (strcmp(entry->path, "UTILS") == 0) {
                // Show utils menu
                show_utils_menu();
                strncpy(current_path, "UTILS", sizeof(current_path) - 1);
            } else {
                strncpy(current_path, entry->path, sizeof(current_path) - 1);
                scan_directory(current_path);
            }
        } else {
            // File selected - try to launch it
            char core_name[256];
            char filename[256];
            char directory[256];

            // Check if we're in Utils
            if (strcmp(current_path, "UTILS") == 0) {
                // Handle "Rebuild folder cache" action
                if (strcmp(entry->path, "REBUILD_CACHE") == 0) {
                    rebuild_empty_dirs_cache();
                    // Go back to ROMS root after rebuild
                    strncpy(current_path, roms_path, sizeof(current_path) - 1);
                    scan_directory(current_path);
                    return;
                }

                // Launch selected file with js2000 core using format: corename;full_path
                init_direct_loader("js2000", "js2000", entry->name);
                render_menu();
                return;
            }
            
            // Check if we're in Recent games
            if (strcmp(current_path, "RECENT_GAMES") == 0) {
                // Parse core_name;game_name from entry->path
                char *separator = strchr(entry->path, ';');
                if (!separator) {
                    return; // Invalid format
                }

                // Copy core_name
                size_t core_len = (size_t)(separator - entry->path);
                if (core_len >= sizeof(core_name))
                    core_len = sizeof(core_name) - 1;

                memcpy(core_name, entry->path, core_len);
                core_name[core_len] = '\0';

                // Copy filename
                snprintf(filename, sizeof(filename), "%s", separator + 1);

                // For recent games, get the full_path from the RecentGame structure
                const RecentGame* recent_list = recent_games_get_list();
                int recent_count = recent_games_get_count();

                for (int i = 0; i < recent_count; i++) {
                    if (strcmp(recent_list[i].core_name, core_name) == 0 &&
                        strcmp(recent_list[i].game_name, filename) == 0) {
                        strcpy(directory, recent_list[i].full_path);
                        break;
                    }
                }
            } else if (strcmp(current_path, "FAVORITES") == 0) {
                // Parse core_name;game_name from entry->path
                char *separator = strchr(entry->path, ';');
                if (!separator) {
                    return; // Invalid format
                }

                // Copy core_name
                size_t core_len = (size_t)(separator - entry->path);
                if (core_len >= sizeof(core_name))
                    core_len = sizeof(core_name) - 1;

                memcpy(core_name, entry->path, core_len);
                core_name[core_len] = '\0';

                // Copy filename
                snprintf(filename, sizeof(filename), "%s", separator + 1);

                // For favorites, get the full_path from the FavoriteGame structure
                const FavoriteGame* favorites_list = favorites_get_list();
                int favorites_count = favorites_get_count();

                for (int i = 0; i < favorites_count; i++) {
                    if (strcmp(favorites_list[i].core_name, core_name) == 0 &&
                        strcmp(favorites_list[i].game_name, filename) == 0) {
                        strcpy(directory, favorites_list[i].full_path);
                        break;
                    }
                }
            } else {
                // Extract core name from parent directory
                get_corename(entry->path, core_name, sizeof(core_name));
                strcpy(directory, entry->path);
                clean_path(directory);
                char *filename_path = strrchr(entry->path, '/');
                if (filename_path) snprintf(filename, sizeof(filename), "%s", filename_path + 1);
                else snprintf(filename, sizeof(filename), "%s", entry->name);
            }
            init_direct_loader(core_name, directory, filename);
        }
    }

    // Handle B button (back) - on button release
    if (prev_input[3] && !b) {
        if (strcmp(current_path, "RECENT_GAMES") == 0) {
            // Go back from Recent games to main ROMS directory
            strncpy(current_path, roms_path, sizeof(current_path) - 1);
            scan_directory(current_path);
            // Restore selection to "Recent games" entry
            for (int i = 0; i < entry_count; i++) {
                if (strcmp(entries[i].path, "RECENT_GAMES") == 0) {
                    selected_index = i;
                    if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
                        scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
                    }
                    break;
                }
            }
        } else if (strcmp(current_path, "FAVORITES") == 0) {
            // Go back from Favorites to main ROMS directory
            strncpy(current_path, roms_path, sizeof(current_path) - 1);
            scan_directory(current_path);
            // Restore selection to "Favorites" entry
            for (int i = 0; i < entry_count; i++) {
                if (strcmp(entries[i].path, "FAVORITES") == 0) {
                    selected_index = i;
                    if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
                        scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
                    }
                    break;
                }
            }
        } else if (strcmp(current_path, "TOOLS") == 0) {
            // Go back from Tools to main ROMS directory
            strncpy(current_path, roms_path, sizeof(current_path) - 1);
            scan_directory(current_path);
            // Restore selection to "Tools" entry
            for (int i = 0; i < entry_count; i++) {
                if (strcmp(entries[i].path, "TOOLS") == 0) {
                    selected_index = i;
                    if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
                        scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
                    }
                    break;
                }
            }
        } else if (strcmp(current_path, "HOTKEYS") == 0) {
            // Go back from Hotkeys to Tools
            show_tools_menu();
            strncpy(current_path, "TOOLS", sizeof(current_path) - 1);
        } else if (strcmp(current_path, "CREDITS") == 0) {
            // Go back from Credits to Tools
            show_tools_menu();
            strncpy(current_path, "TOOLS", sizeof(current_path) - 1);
        } else if (strcmp(current_path, "UTILS") == 0) {
            // Go back from Utils to Tools
            show_tools_menu();
            strncpy(current_path, "TOOLS", sizeof(current_path) - 1);
        } else if (strcmp(current_path, roms_path) != 0) {
            // Remember which directory we're leaving so we can restore position
            char prev_dir[256];
            char *last_slash = strrchr(current_path, '/');
            if (last_slash && last_slash != current_path) {
                strncpy(prev_dir, last_slash + 1, sizeof(prev_dir) - 1);
                prev_dir[sizeof(prev_dir) - 1] = '\0';

                *last_slash = '\0';
                scan_directory(current_path);

                // Find the directory we just left and restore selection to it
                for (int i = 0; i < entry_count; i++) {
                    if (strcmp(entries[i].name, prev_dir) == 0) {
                        selected_index = i;
                        // Update scroll offset to keep selection visible
                        if (selected_index < scroll_offset) {
                            scroll_offset = selected_index;
                        } else if (selected_index >= scroll_offset + VISIBLE_ENTRIES) {
                            scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
                        }
                        break;
                    }
                }
            }
        }
    }
    
    if (input_changed) render_menu();

    // Store current state for next frame
    prev_input[0] = up;
    prev_input[1] = down;
    prev_input[2] = a;
    prev_input[3] = b;
    prev_input[4] = l;
    prev_input[5] = r;
    prev_input[6] = select;
    prev_input[7] = left;
    prev_input[8] = right;
    prev_input[9] = x;
}

const char *parent_dir(const char *path) {
    char *buf;
    char *last_slash;

    if (!path) return NULL;

    buf = strdup(path);
    if (!buf) return NULL;

    // remove trailing slash
    size_t len = strlen(buf);
    if (len > 1 && buf[len - 1] == '/')
        buf[len - 1] = '\0';

    last_slash = strrchr(buf, '/');
    if (last_slash)
    {
        if (last_slash == buf)
            last_slash[1] = '\0';
        else
            *last_slash = '\0';
    }

    return buf;
}

void fallback_get_roms_dir(char* roms_dir, size_t max_len) {
    const char* home_dir = NULL;
    char* allocated_dir = NULL;
#if defined(NOCTURNAL) || defined(SF2000)
    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &home_dir)) {
        allocated_dir = (char*)parent_dir(home_dir);
        home_dir = allocated_dir;
    } else home_dir = "/mnt/sda1"; // No system directory available
#else
    home_dir = getenv("HOME");
    if (home_dir == NULL) home_dir = getenv("USERPROFILE");
#endif

#ifdef _WIN32
    snprintf(roms_dir, max_len, "%s\\ROMS", home_dir);
#else
    snprintf(roms_dir, max_len, "%s/ROMS", home_dir);
#endif
    if (allocated_dir) free(allocated_dir);
}

// Libretro API implementation
void retro_init(void) {
    // Set the roms directory
    const char* dir = NULL;
#ifdef RETRO_ENVIRONMENT_GET_ROMS_DIRECTORY
    if (environ_cb(RETRO_ENVIRONMENT_GET_ROMS_DIRECTORY, &dir) && dir) {
        strncpy(roms_path, dir, sizeof(roms_path) - 1);
        roms_path[sizeof(roms_path) - 1] = '\0';
    } else fallback_get_roms_dir(roms_path, sizeof(roms_path));
#else 
    fallback_get_roms_dir(roms_path, sizeof(roms_path));
#endif

    size_t len = strlen(roms_path);
    while (len > 1 && (roms_path[len - 1] == '/' || roms_path[len - 1] == '\\')) {
        roms_path[len - 1] = '\0';
        len--;
    }

    // Set the configs directory
#ifdef RETRO_ENVIRONMENT_GET_CONFIG_DIRECTORY
    dir = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CONFIG_DIRECTORY, &dir) && dir) {
        strncpy(configs_dir, dir, sizeof(configs_dir) - 1);
        configs_dir[sizeof(configs_dir) - 1] = '\0';
    } else {
        char* allocated_configs = parent_dir(roms_path);
        snprintf(configs_dir, sizeof(configs_dir), "%s/configs", allocated_configs);
        free(allocated_configs); 
    }
#else
    char* allocated_configs = parent_dir(roms_path);
    snprintf(configs_dir, sizeof(configs_dir), "%s/configs", allocated_configs);
    free(allocated_configs);
#endif

    // Set the assets directory
    dir = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY, &dir) && dir) {
        strncpy(assets_dir, dir, sizeof(assets_dir) - 1);
        assets_dir[sizeof(assets_dir) - 1] = '\0';
    } else {
        char* allocated_assets = parent_dir(roms_path);
        snprintf(assets_dir, sizeof(assets_dir), "%s/assets/FrogUI", allocated_assets);
        free(allocated_assets);
    }

    snprintf(empty_dirs_cache_file, sizeof(empty_dirs_cache_file), "%s/frogui_empty_dirs.cache", assets_dir);

    strcpy(os_settings_title, OS_NAME_LOWER);
    for (char *p = os_settings_title; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
    strcat(os_settings_title, " SETTINGS");

    strcpy(ptr_gs_run_game_name, "FrogUI"); // Sets the name for the pause menu
    framebuffer = (uint16_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    environ_cb(RETRO_ENVIRONMENT_GET_HCGE_ACCEL_FUNCTIONS, &accel_funcs);

    // Seed random number generator for random game picker
    srand(time(NULL));

    // Initialize modular systems
    render_init(framebuffer);
    font_init();
    theme_init();
    recent_games_init();
    favorites_init();
    settings_init();

    char console_mappings_path[128];
    snprintf(console_mappings_path, sizeof(console_mappings_path), "%s/console_mappings.opt", assets_dir);
    load_console_mappings(console_mappings_path);

    recent_games_load();
    favorites_load();
    settings_load();

    apply_settings();
}

void retro_deinit(void) {
    // Free thumbnail cache
    if (thumbnail_cache_valid) {
        free_thumbnail(&current_thumbnail);
        thumbnail_cache_valid = 0;
    }

    // Free entries array
    if (entries) {
        free(entries);
        entries = NULL;
        entries_capacity = 0;
        entry_count = 0;
    }

    if (framebuffer) {
        free(framebuffer);
        framebuffer = NULL;
    }
}

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port;
    (void)device;
}

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "FrogUI";
    info->library_version  = "0.1";
    info->need_fullpath    = true;
    info->valid_extensions = "frogui";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;

    info->geometry.base_width   = SCREEN_WIDTH;
    info->geometry.base_height  = SCREEN_HEIGHT;
    info->geometry.max_width    = SCREEN_WIDTH;
    info->geometry.max_height   = SCREEN_HEIGHT;
    info->geometry.aspect_ratio = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    bool no_content = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
    audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
    input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
}

void retro_reset(void) {
    strncpy(current_path, roms_path, sizeof(current_path) - 1);
    scan_directory(current_path);
}

void retro_run(void) {
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      apply_settings();
    }
    handle_input();
    if ((os_get_tick_count() - scrolling_delay_time > 500) && (!block_scrolling) && (!prevent_scrolling)) {
        update_selected_item_scroll();
        render_selected_item_text();
    }
    if (bgm_enabled || sfx_enabled) output_wav_audio();
    if (video_cb) {
        video_cb(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
    }
    if (game_queued) {
        // Because the loader checks ptr_gs_run_game_file directly it doesn't matter what we pass to run_game (aslong as it's a gba file)
        // TODO: Add support for stock cores, tbh only ARCADE matters, we should list games from /ARCADE and pass those directly to run_game
        // Pass to core_api as a workaround so we can unload game and deinit
        wrap_run_game("/mnt/sda1/ROMS/menu;m.gba", 0);
        return;
    }
}

bool retro_load_game(const struct retro_game_info *info) {
    if (info && info->path) {
        // If label is not available, fallback to using the file name from the path
        const char *game_name = strrchr(info->path, '/');
        if (game_name) {
            game_name++; // Move past the '/' character
            // Auto-launch most recent game if resume on boot is enabled and we aren't loading from the pause menu
            if ((strcmp(game_name, "p") != 0) && (resume_on_boot)) auto_launch_recent_game();

        }
    }

    // Skip directory scan if we're auto-launching a game (faster boot)
    if (!game_queued) {
        strncpy(current_path, roms_path, sizeof(current_path) - 1);
        scan_directory(current_path);
    }
    
    render_menu();
    if (bgm_enabled) audio_init();
    return true;
}

void retro_unload_game(void) {
}

unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    (void)type;
    (void)info;
    (void)num;
    return false;
}

size_t retro_serialize_size(void) {
    return 0;
}

bool retro_serialize(void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

bool retro_unserialize(const void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

void *retro_get_memory_data(unsigned id) {
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned id) {
    (void)id;
    return 0;
}

void retro_cheat_reset(void) {
}

void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index;
    (void)enabled;
    (void)code;
}
