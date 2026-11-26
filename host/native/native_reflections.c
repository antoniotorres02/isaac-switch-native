#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int enabled = -1;
static uint64_t skipped_passes;

static void report_reflections(void) {
    fprintf(stderr, "[native][gl] skipped %" PRIu64 " water reflection passes\n", skipped_passes);
}

int isaac_native_skip_water_reflection(void) {
    if (enabled < 0) {
        const char *setting = getenv("ISAAC_NATIVE_SKIP_REFLECTIONS");
        enabled = setting && *setting == '1';
        if (enabled) atexit(report_reflections);
    }
    if (enabled) ++skipped_passes;
    return enabled;
}
