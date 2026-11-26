#include "native_options.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char isaac_default_options[] =
    "[Options]\r\nLanguage=0\r\nMusicVolume=0.7000\r\nMusicEnabled=1\r\n"
    "SFXVolume=0.7000\r\nMapOpacity=0.3000\r\nFullscreen=0\r\nFilter=0\r\nExposure=1.0000\r\n"
    "Gamma=1.0000\r\nControllerHotplug=1\r\nPopUps=1\r\nCameraStyle=1\r\nShowRecentItems=0\r\n"
    "HudOffset=1.0000\r\nTryImportSave=0\r\nFoundHUD=0\r\nEnableMods=1\r\nRumbleEnabled=1\r\n"
    "ChargeBars=0\r\nBulletVisibility=0\r\nTouchMode=1\r\nAimLock=1\r\nJacobEsauControls=0\r\n"
    "AscentVoiceOver=1\r\nOnlineHud=0\r\nStreamerMode=0\r\nOnlinePlayerVolume=6\r\n"
    "OnlinePlayerOpacity=10\r\nOnlineChatEnabled=1\r\nOnlineChatFilterEnabled=1\r\n"
    "MultiplayerColorSet=0\r\nOnlineInputDelay=3\r\nItemInfoDisplayEnabled=0\r\n"
    "AcceptedPublicBeta_v1.9.7.17=1\r\nAcceptedPublicBeta_v1.9.7.19=1\r\nAcceptedDataCollectionDisclaimer=1\r\n"
    "EnableDebugConsole=0\r\nMaxScale=99\r\n"
#ifdef __SWITCH__
    "MaxRenderScale=1\r\n"
#else
    "MaxRenderScale=2\r\n"
#endif
    "VSync=1\r\nPauseOnFocusLost=1\r\n"
    "SteamCloud=0\r\nMouseControl=0\r\nBossHpOnBottom=1\r\nAnnouncerVoiceMode=0\r\nConsoleFont=0\r\n"
    "FadedConsoleDisplay=0\r\nSaveCommandHistory=1\r\nWindowWidth=960\r\nWindowHeight=540\r\n"
    "WindowPosX=8\r\nWindowPosY=32\r\nUseExclusiveFullscreen=0\r\nEnableEpicOverlay=0\r\n"
    "EosCrossplay=0\r\n";

const isaac_option_spec isaac_option_specs[ISAAC_OPTION_COUNT] = {
    {"MaxRenderScale", "Internal render scale", "1 is fastest. Higher values sharpen text and small sprites.", 0, 1, 99, 1},
    {"VSync", "Vertical sync", "Keep the game's presentation synchronized with the display.", 0, 0, 1, 1},
    {"ControllerHotplug", "Controller hotplug", "Detect controllers that connect after the game starts.", 0, 0, 1, 1},
    {"EnableCaustics", "Water light patterns", "Draw moving light patterns on flooded floors.", 1, 0, 1, 0},
    {"EnableBloom", "Bloom", "Add a glow around bright effects.", 1, 0, 1, 0},
    {"EnableShockwave", "Shockwave distortion", "Distort the scene around explosions and other impacts.", 1, 0, 1, 0},
    {"EnableColorCorrection", "Room color correction", "Apply the room's color filter.", 1, 0, 1, 0},
    {"EnableLighting", "Room lighting", "Keep light and darkness effects visible.", 1, 0, 1, 1},
    {"EnableWaterSurface", "Water surface", "Keep water reflections and the mirror room visible.", 1, 0, 1, 1},
    [ISAAC_OPTION_FRAME_CADENCE] =
        {"frame_skip", "Frame skip", "30 FPS gives crowded rooms more time per game update.", 2, 0, 2, 1},
    [ISAAC_OPTION_SKIP_REFLECTIONS] =
        {"skip_reflections", "Skip water reflections", "Experimental. Skip reflection draws in flooded rooms.", 2, 0, 1, 0},
};

static const char *skip_space(const char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

static int option_at(const char *line, int effect, const char **value) {
    const char *start = skip_space(line), *eq = strchr(start, '=');
    if (!eq) return -1;
    const char *end = eq;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
    for (int i = 0; i < ISAAC_OPTION_COUNT; ++i) {
        const isaac_option_spec *spec = &isaac_option_specs[i];
        size_t n = (size_t)(end - start);
        if (spec->effect == effect && n == strlen(spec->key) && !strncasecmp(start, spec->key, n)) {
            *value = eq + 1;
            return i;
        }
    }
    return -1;
}

static int options_section(const char *line) {
    line = skip_space(line);
    return *line != '[' ? -1 : !strncasecmp(line, "[Options]", 9);
}

/* Read from an SD file or from the built-in defaults. Keep complete lines so
   the editor can copy unknown settings and comments byte for byte. */
static int read_line(FILE *file, const char **text, char **line, size_t *cap, size_t *len) {
    *len = 0;
    for (;;) {
        int ch;
        if (file) ch = fgetc(file);
        else if (**text) ch = (unsigned char)*(*text)++;
        else ch = EOF;
        if (ch == EOF) break;
        if (*cap - *len < 2) {
            if (*cap > SIZE_MAX / 2) return -1;
            size_t next = *cap ? *cap * 2 : 128;
            char *buffer = realloc(*line, next);
            if (!buffer) return -1;
            *line = buffer;
            *cap = next;
        }
        (*line)[(*len)++] = (char)ch;
        if (ch == '\n') break;
    }
    if (file && ferror(file)) return -1;
    if (!*len) return 0;
    (*line)[*len] = 0;
    return 1;
}

void isaac_options_defaults(isaac_options_menu *menu) {
    for (int i = 0; i < ISAAC_OPTION_COUNT; ++i) menu->values[i] = isaac_option_specs[i].initial;
    menu->changed = (1u << ISAAC_OPTION_FRAME_CADENCE) - 1;
}

static int path_for(char path[1024], const char *dir, int effect) {
    int n = snprintf(path, 1024, "%s/%s", dir, effect ? ISAAC_EFFECTS_REL : ISAAC_OPTIONS_REL);
    return n >= 0 && n < 1024 ? 0 : -1;
}

void isaac_options_recover(const char *saves) {
    for (int effect = 0; effect <= 1; ++effect) {
        char path[1024], backup[1040];
        struct stat st;
        if (path_for(path, saves, effect) || stat(path, &st) == 0 || errno != ENOENT) continue;
        snprintf(backup, sizeof backup, "%s.tmp-backup", path);
        if (!stat(backup, &st) && S_ISREG(st.st_mode)) rename(backup, path);
    }
}

/* Fall back to the user's original graphics config when the SD override is absent. */
static FILE *existing(const char *path, const char *guest, int effect) {
    FILE *file = fopen(path, "rb");
    if (file || errno != ENOENT) return file;
    char recovery[1040];
    snprintf(recovery, sizeof recovery, "%s.tmp-backup", path);
    file = fopen(recovery, "rb");
    if (file || errno != ENOENT || !effect || !guest) return file;
    char source[1024];
    if (path_for(source, guest, 1)) { errno = ENAMETOOLONG; return NULL; }
    return fopen(source, "rb");
}

int isaac_options_read(isaac_options_menu *menu, const char *saves, const char *guest) {
    memset(menu, 0, sizeof *menu);
    isaac_options_defaults(menu);
    menu->changed = 0;
    for (int effect = 0; effect <= 1; ++effect) {
        char path[1024];
        if (path_for(path, saves, effect)) return -1;
        FILE *file = existing(path, guest, effect);
        if (!file) {
            if (errno != ENOENT) return -1;
            continue;
        }
        if (effect)
            for (int i = 0; i < ISAAC_OPTION_COUNT; ++i)
                if (isaac_option_specs[i].effect == 1) menu->values[i] = 1;
        char *line = NULL;
        size_t cap = 0, len;
        int active = 0, rc;
        while ((rc = read_line(file, NULL, &line, &cap, &len)) > 0) {
            int section = options_section(line);
            if (section >= 0) active = section;
            if (!active) continue;
            const char *value;
            int i = option_at(line, effect, &value);
            if (i < 0) continue;
            char *end;
            long v = strtol(value, &end, 10);
            const isaac_option_spec *spec = &isaac_option_specs[i];
            if (end != value && v >= spec->min && v <= spec->max) menu->values[i] = (int)v;
        }
        free(line);
        fclose(file);
        if (rc < 0) return -1;
    }
    return 0;
}

static void append_changed(FILE *out, const isaac_options_menu *menu, unsigned mask) {
    for (int i = 0; i < ISAAC_OPTION_COUNT; ++i)
        if (mask & (1u << i)) fprintf(out, "%s=%d\r\n", isaac_option_specs[i].key, menu->values[i]);
}

static int edit_file(FILE *in, const char *defaults, FILE *out, const isaac_options_menu *menu,
                     int effect, unsigned mask) {
    char *line = NULL;
    size_t cap = 0, len;
    int active = 0, found = 0, newline = 1, rc;
    while ((rc = read_line(in, &defaults, &line, &cap, &len)) > 0) {
        int section = options_section(line);
        if (section >= 0) {
            if (active && mask) {
                if (!newline) fputc('\n', out);
                append_changed(out, menu, mask);
                mask = 0;
            }
            active = section;
            found |= active;
        }
        const char *value;
        int i = active ? option_at(line, effect, &value) : -1;
        if (i >= 0 && (menu->changed & (1u << i))) {
            fprintf(out, "%s=%d\r\n", isaac_option_specs[i].key, menu->values[i]);
            mask &= ~(1u << i);
            newline = 1;
        } else {
            fwrite(line, 1, len, out);
            newline = line[len - 1] == '\n';
        }
    }
    if (rc == 0 && mask) {
        if (!newline) fputc('\n', out);
        if (!found) fputs("[Options]\r\n", out);
        append_changed(out, menu, mask);
    }
    free(line);
    return rc;
}

static int ensure_parent(char *path) {
    for (char *p = path + 1; (p = strchr(p, '/')); ++p) {
        if (p[-1] == ':') continue;
        *p = 0;
        int rc = mkdir(path, 0755);
        *p = '/';
        if (rc && errno != EEXIST) return -1;
    }
    return 0;
}

static int publish(const char *path, const char *temp) {
    char backup[1040];
    snprintf(backup, sizeof backup, "%s.tmp-backup", path);
    if (rename(temp, path) == 0) {
        unlink(backup);
        return 0;
    }
#ifdef __SWITCH__
    /* SD filesystems may require moving the current file out of the way. */
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return -1;
    if (stat(backup, &st) == 0 || errno != ENOENT || rename(path, backup)) return -1;
    if (rename(temp, path)) {
        rename(backup, path);
        return -1;
    }
    unlink(backup);
    return 0;
#else
    return -1;
#endif
}

int isaac_port_setting(const char *root, const char *key, int value) {
    char path[1024], temp[1040];
    int n = snprintf(path, sizeof path, "%s/settings.ini", root);
    if (n < 0 || (size_t)n >= sizeof path || ensure_parent(path)) return -1;
    FILE *in = existing(path, NULL, 0);
    if (!in && errno != ENOENT) return -1;
    snprintf(temp, sizeof temp, "%s.tmp-options", path);
    FILE *out = fopen(temp, "wb");
    if (!out) { if (in) fclose(in); return -1; }

    const size_t key_length = strlen(key);
    char *line = NULL;
    const char *defaults = "";
    size_t cap = 0, length;
    int updated = 0, newline = 1, rc;
    while ((rc = read_line(in, &defaults, &line, &cap, &length)) > 0) {
        const char *start = skip_space(line);
        if (!strncmp(start, key, key_length) && *skip_space(start + key_length) == '=') {
            if (!updated) fprintf(out, "%s=%d\n", key, value);
            updated = newline = 1;
        } else {
            fwrite(line, 1, length, out);
            newline = line[length - 1] == '\n';
        }
    }
    free(line);
    if (in && fclose(in)) rc = -1;
    if (!updated && rc == 0) fprintf(out, "%s%s=%d\n", newline ? "" : "\n", key, value);
    int failed = rc < 0 || ferror(out) || fflush(out) || fsync(fileno(out));
    if (fclose(out) || failed || publish(path, temp)) { unlink(temp); return -1; }
    return 0;
}

int isaac_options_write(isaac_options_menu *menu, const char *saves, const char *guest) {
    for (int effect = 0; effect <= 1; ++effect) {
        unsigned mask = 0;
        for (int i = 0; i < ISAAC_OPTION_COUNT; ++i)
            if (isaac_option_specs[i].effect == effect) mask |= menu->changed & (1u << i);
        if (!mask) continue;
        char path[1024], temp[1040];
        if (path_for(path, saves, effect) || ensure_parent(path)) return -1;
        FILE *in = existing(path, guest, effect);
        if (!in && errno != ENOENT) return -1;
        char graphics[512];
        const char *defaults = isaac_default_options;
        if (!in && effect) {
            if (!isaac_options_default_effects(graphics, sizeof graphics)) return -1;
            defaults = graphics;
        }
        snprintf(temp, sizeof temp, "%s.tmp-options", path);
        FILE *out = fopen(temp, "wb");
        if (!out) { if (in) fclose(in); return -1; }
        int rc = edit_file(in, defaults, out, menu, effect, mask);
        if (in && fclose(in)) rc = -1;
        int failed = rc < 0 || ferror(out) || fflush(out) || fsync(fileno(out));
        if (fclose(out) || failed || publish(path, temp)) { unlink(temp); return -1; }
        menu->changed &= ~mask;
    }
    return 0;
}

unsigned isaac_options_default_effects(char *out, unsigned cap) {
    int n = snprintf(out, cap, "[Options]\r\n");
    if (n < 0 || (unsigned)n >= cap) return 0;
    unsigned len = (unsigned)n;
    for (int i = 0; i < ISAAC_OPTION_COUNT; ++i) {
        const isaac_option_spec *spec = &isaac_option_specs[i];
        if (spec->effect != 1) continue;
        n = snprintf(out + len, cap - len, "%s=%d\r\n", spec->key, spec->initial);
        if (n < 0 || (unsigned)n >= cap - len) return 0;
        len += (unsigned)n;
    }
    return len;
}
