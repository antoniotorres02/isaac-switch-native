/* Are the game's files on the card? The check the launch screen runs before it
 * offers Play: every file of the build's instance_index.json has to be under
 * guest/ and look like a whole copy of itself.
 *
 * Exact sizes prove nothing. On a card that plays fine, measured 2026-11-01,
 * afterbirthp.a was 467 bytes larger than the list (Steam had patched it) and
 * savedatapath.txt was shorter (it is a small text file). So only what cannot be
 * a working file counts: not there, empty, or an archive of 1 MiB or more with
 * over 1 % gone, which is what an interrupted copy leaves. A copy cut within its
 * last 1 % is not noticed. Sizes only, so it costs a stat per file.
 *
 * No Steam and no libnx here: it runs the same on the PC and in every build. */
#ifndef ISAAC_NATIVE_GAMEFILES_H
#define ISAAC_NATIVE_GAMEFILES_H

#define ISAAC_GAMEFILES_NAMES 3

typedef struct {
    int total;                  /* files the build lists */
    int missing;                /* not there, or not a file */
    int truncated;              /* empty, or clearly cut short */
    int different;              /* another size, but whole: logged, nothing more */
    /* the first files that are missing or truncated */
    char bad[ISAAC_GAMEFILES_NAMES][64];
} isaac_gamefiles;

/* 0 with `out` filled; -1 when the list cannot be read (nothing was checked). */
int isaac_gamefiles_check(const char *index_path, const char *guest_dir, isaac_gamefiles *out);

/* 1 when the game cannot be expected to start from these files. */
int isaac_gamefiles_blocking(const isaac_gamefiles *files);

#endif
