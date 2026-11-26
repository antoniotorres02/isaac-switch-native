#include "native_modstate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int marker(char *out, size_t cap, const char *saves, const char *folder, int on) {
    if (!saves || !*saves || !*folder || strchr(folder, '/') || strchr(folder, '\\') ||
        !strcmp(folder, ".") || !strcmp(folder, "..")) return -1;
    int n = snprintf(out, cap, "%s/mod-state/%s.%s", saves, folder, on ? "on" : "off");
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

int isaac_modstate_get(const char *saves, const char *folder) {
    char path[1024];
    struct stat st;
    if (marker(path, sizeof path, saves, folder, 0)) return -1;
    if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) return 0;
    if (marker(path, sizeof path, saves, folder, 1)) return -1;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) ? 1 : -1;
}

int isaac_modstate_set(const char *saves, const char *folder, int on) {
    char dir[768], old[1024], replacement[1024];
    if (marker(old, sizeof old, saves, folder, !on) ||
        marker(replacement, sizeof replacement, saves, folder, on)) return -1;
    int n = snprintf(dir, sizeof dir, "%s/mod-state", saves);
    if (n < 0 || (size_t)n >= sizeof dir) return -1;
    struct stat st;
    if (mkdir(saves, 0755) && errno != EEXIST) return -1;
    if (lstat(saves, &st) || !S_ISDIR(st.st_mode)) return -1;
    if (mkdir(dir, 0755) && errno != EEXIST) return -1;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode)) return -1;
    if (lstat(replacement, &st) != 0) {
        if (errno != ENOENT) return -1;
        int fd = open(replacement, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 || close(fd)) return -1;
    } else if (!S_ISREG(st.st_mode)) return -1;
    if (lstat(old, &st) != 0) return errno == ENOENT ? 0 : -1;
    return S_ISREG(st.st_mode) ? unlink(old) : -1;
}
