#ifndef ISAAC_NATIVE_OPTIONS_H
#define ISAAC_NATIVE_OPTIONS_H

#define ISAAC_OPTIONS_REL "documents/my games/binding of isaac repentance+/options.ini"
#define ISAAC_EFFECTS_REL "resources/config.ini"
#define ISAAC_OPTION_FRAME_CADENCE 9
#define ISAAC_OPTION_SKIP_REFLECTIONS 10
#define ISAAC_OPTION_COUNT 11

typedef struct {
    const char *key, *label, *detail;
    int effect, min, max, initial;
} isaac_option_spec;

typedef struct {
    int values[ISAAC_OPTION_COUNT];
    unsigned changed;
    int selected;
    char status[96];
} isaac_options_menu;

extern const isaac_option_spec isaac_option_specs[ISAAC_OPTION_COUNT];
extern const char isaac_default_options[];
void isaac_options_defaults(isaac_options_menu *menu);
int isaac_options_read(isaac_options_menu *menu, const char *saves, const char *guest);
int isaac_options_write(isaac_options_menu *menu, const char *saves, const char *guest);
int isaac_port_setting(const char *root, const char *key, int value);
void isaac_options_recover(const char *saves);
unsigned isaac_options_default_effects(char *out, unsigned cap);

#endif
