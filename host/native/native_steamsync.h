/* Save sync between the console's save folder and the account's Steam Cloud.
 *
 * Steam keeps Isaac's slots as rep+persistentgamedataN.dat / rep+gamestateN.dat
 * (the "rep+" is the local-name mapping the game's own PC build uses); the
 * native build reads and writes persistentgamedataN.dat / gamestateN.dat under
 * saves/documents/my games/binding of isaac repentance+/. The bytes are the
 * same, only the name differs.
 *
 * Pull runs before the game opens its saves, push after it closes them. Both
 * compare the SHA-1 of what is on each side with what the last sync left there
 * (saves/steam-sync-state.txt), so:
 *
 *   only Steam changed   -> download it (the copy it replaces is backed up)
 *   only the console     -> nothing to do now, push uploads it at exit
 *   both changed         -> conflict: ask on screen when the caller installed a
 *                           prompt, otherwise the newest timestamp wins
 *   neither              -> nothing
 *
 * Nothing here ever deletes a save: the losing copy of a conflict or of an
 * overwrite is kept under saves/save_backups/steam-<date>/.
 */
#ifndef ISAAC_NATIVE_STEAMSYNC_H
#define ISAAC_NATIVE_STEAMSYNC_H

#include <stddef.h>
#include <stdint.h>

#include "native_steamcm.h"

#define ISAAC_STEAMSYNC_SLOTS 6

/* isaac_steamsync_pull: the account does not own the game, nothing was synced. */
#define ISAAC_STEAMSYNC_NOT_OWNED (-2)

/* What Steam says the account owns. It decides whether the saves sync, never
 * whether the game runs. */
enum {
    ISAAC_OWN_UNKNOWN,          /* no answer: the sync goes on */
    ISAAC_OWN_ALL,              /* the game and every expansion */
    ISAAC_OWN_PART,             /* the game, but `missing` names expansions it lacks */
    ISAAC_OWN_NONE,             /* not the game: no sync */
};

typedef struct {
    const char *save_dir;       /* .../saves */
    uint64_t steamid;           /* account id, from the token file */
    const char *token;          /* refresh token (iss=steam) */
    int prompt;                 /* 1: ask on a conflict */
    int timeout_ms;             /* network timeout */
} isaac_steamsync_cfg;

/* The token file written by the PC provisioning step:
 *     steamid=7656119...
 *     token=eyJhbGciOi...
 * Returns 0 and fills `steamid`/`token` when the file is usable. */
int isaac_steamsync_read_token(const char *path, uint64_t *steamid, char *token, size_t cap,
                               char *err, size_t errcap);

/* Writes that same file, replacing what is there. 0 when it is on the card. */
int isaac_steamsync_write_token(const char *path, uint64_t steamid, const char *token,
                                char *err, size_t errcap);

/* Forgets what the last sync left on each side: the account changes at a
 * sign-in or sign-out, and those notes describe the previous one's cloud. The
 * next pull compares the saves as a first sync does. No save is touched. */
void isaac_steamsync_forget(const char *save_dir);

/* 1 = use the cloud copy, 0 = keep the console's. */
typedef int (*isaac_steamsync_ask_fn)(void *user, const char *slot,
                                      const char *local_desc, const char *cloud_desc);
void isaac_steamsync_set_ask(isaac_steamsync_ask_fn fn, void *user);
void isaac_steamsync_set_log(void (*fn)(void *user, const char *line), void *user);

/* Told once per pull, before the saves are compared. `missing` is "" or the
 * names of what the account lacks, comma separated. */
typedef void (*isaac_steamsync_own_fn)(void *user, int status, const char *missing);
void isaac_steamsync_set_own_fn(isaac_steamsync_own_fn fn, void *user);

/* What the pull decided for each slot, for a screen that lists them. A
 * conflict ends as DOWNLOADED (Steam's copy kept) or UPLOAD (the console's). */
enum {
    ISAAC_SYNC_SLOT_EMPTY,          /* on neither side */
    ISAAC_SYNC_SLOT_SAME,           /* both sides hold the same bytes */
    ISAAC_SYNC_SLOT_DOWNLOADED,     /* Steam's copy replaced the console's */
    ISAAC_SYNC_SLOT_UPLOAD,         /* the console's copy goes up at exit */
    ISAAC_SYNC_SLOT_LOCAL_ONLY,     /* on the console only, already synced once */
    ISAAC_SYNC_SLOT_FAILED,         /* the download failed: the console copy stays */
};
void isaac_steamsync_set_slot_fn(void (*fn)(void *user, int slot, int status), void *user);

/* 0 when the sync completed (including "nothing to do"), -1 when it could not
 * run. Never fatal for the game: the caller logs and carries on. */
int isaac_steamsync_pull(const isaac_steamsync_cfg *cfg);
int isaac_steamsync_push(const isaac_steamsync_cfg *cfg);

/* Same, with an injected transport and clock (the host tests use these). */
int isaac_steamsync_pull_with(const isaac_steamsync_cfg *cfg, const isaac_steam_transport *transport,
                              int64_t now);
int isaac_steamsync_push_with(const isaac_steamsync_cfg *cfg, const isaac_steam_transport *transport,
                              int64_t now);

/* Cloud name for a slot (for logs and tests). */
const char *isaac_steamsync_cloud_name(int slot);
const char *isaac_steamsync_local_name(int slot);

#endif
