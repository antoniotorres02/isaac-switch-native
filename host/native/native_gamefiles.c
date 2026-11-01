#include "native_gamefiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BIG_FILE (1024 * 1024)

/* ship.py's rows: {"p":"<path>","s":<size>}, as native_main.c's seed_list reads
 * them. */
int isaac_gamefiles_check(const char *index_path, const char *guest_dir, isaac_gamefiles *out) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(index_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        fclose(f);
        return -1;
    }
    fclose(f);
    text[n] = 0;
    int named = 0;
    for (char *p = text; (p = strstr(p, "\"p\":\"")); ) {
        p += 5;
        char *end = strchr(p, '"');
        if (!end) break;
        *end = 0;
        char *size_at = strstr(end + 1, "\"s\":");
        long long want = size_at ? strtoll(size_at + 4, NULL, 10) : 0;
        char path[512];
        struct stat st;
        int state = 0;      /* 0 fine, 1 missing, 2 truncated */
        if (snprintf(path, sizeof path, "%s/%s", guest_dir, p) >= (int)sizeof path ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            state = 1;
            ++out->missing;
        } else if (st.st_size == 0 ||
                   (want >= BIG_FILE && (long long)st.st_size < want - want / 100)) {
            state = 2;
            ++out->truncated;
        } else if ((long long)st.st_size != want) {
            ++out->different;
        }
        ++out->total;
        if (state && named < ISAAC_GAMEFILES_NAMES)
            snprintf(out->bad[named++], sizeof out->bad[0], "%s", p);
        p = end + 1;
    }
    free(text);
    return out->total ? 0 : -1;
}

int isaac_gamefiles_blocking(const isaac_gamefiles *files) {
    return files->missing > 0 || files->truncated > 0;
}
