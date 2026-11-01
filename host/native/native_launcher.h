/* The launch screen: Steam Cloud status and a Play button, drawn in software
 * into a 1280x720 RGBA8888 buffer (bytes R, G, B, A) before the game creates
 * its EGL context.
 *
 * The drawing is plain C over FreeType, so the same code renders on a PC
 * (tools/preview_switch_launcher.py writes PNGs of every state); the Switch
 * glue in native_switch.c owns the framebuffer, the pad and the sync thread,
 * and fills an isaac_launcher_view each frame. */
#ifndef ISAAC_NATIVE_LAUNCHER_H
#define ISAAC_NATIVE_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#include "native_qr.h"
#include "native_steamsync.h"

#define ISAAC_LAUNCHER_W 1280
#define ISAAC_LAUNCHER_H 720

/* Where the sync stands. */
enum {
    ISAAC_LAUNCHER_SYNCING,     /* the pull is running: Play waits */
    ISAAC_LAUNCHER_DONE,        /* pulled: Play */
    ISAAC_LAUNCHER_FAILED,      /* could not sync: Play anyway, Retry */
    ISAAC_LAUNCHER_OFF,         /* steam_sync=0 */
    ISAAC_LAUNCHER_LAUNCHING,   /* Play was pressed: the last frame */
    ISAAC_LAUNCHER_SIGNED_OUT,  /* no Steam sign-in: Play, saves stay here */
};

/* The sign-in dialog. */
enum {
    ISAAC_SIGNIN_NONE,
    ISAAC_SIGNIN_WAIT,          /* asking Steam for a code */
    ISAAC_SIGNIN_SCAN,          /* the QR code is up */
    ISAAC_SIGNIN_FAILED,        /* signin_error says why: Y tries again */
};

/* The sign-out dialog. */
enum { ISAAC_SIGNOUT_NONE, ISAAC_SIGNOUT_ASK, ISAAC_SIGNOUT_BUSY };

/* What X does on the page. */
enum { ISAAC_ACCOUNT_NONE, ISAAC_ACCOUNT_SIGN_IN, ISAAC_ACCOUNT_SIGN_OUT };

/* The steps of a pull, in order. */
enum {
    ISAAC_LAUNCHER_STEP_TOKEN,      /* steam-token.txt */
    ISAAC_LAUNCHER_STEP_CLOCK,      /* the clock TLS checks certificates against */
    ISAAC_LAUNCHER_STEP_CONNECT,    /* CM list, TLS, websocket */
    ISAAC_LAUNCHER_STEP_LOGON,
    ISAAC_LAUNCHER_STEP_OWNED,      /* the game and its expansions on the account */
    ISAAC_LAUNCHER_STEP_COMPARE,    /* list the cloud, compare, download */
    ISAAC_LAUNCHER_STEPS
};
enum { ISAAC_STEP_WAIT, ISAAC_STEP_RUN, ISAAC_STEP_OK, ISAAC_STEP_FAIL, ISAAC_STEP_SKIP, ISAAC_STEP_WARN };

/* The game's files on the card (native_gamefiles.c). */
enum {
    ISAAC_FILES_UNCHECKED,      /* the build's list could not be read, or the check is off */
    ISAAC_FILES_OK,
    ISAAC_FILES_BAD,            /* missing or cut short: no Play */
};

/* A slot the pull has not reported yet; the rest are ISAAC_SYNC_SLOT_*. */
#define ISAAC_LAUNCHER_SLOT_PENDING (-1)

typedef struct {
    int phase;
    int step[ISAAC_LAUNCHER_STEPS];
    char step_detail[ISAAC_LAUNCHER_STEPS][64];
    int slot[ISAAC_STEAMSYNC_SLOTS];    /* persistentgamedata1-3, gamestate1-3 */
    char account[32];                   /* "" when there is no token */
    char error[192];                    /* why the sync failed or is off */
    char activity[160];                 /* the latest log line, in the footer */
    int conflict;                       /* 1: the dialog is up */
    char conflict_slot[64];
    char conflict_local[40];
    char conflict_cloud[40];
    int conflict_choice;                /* 0 this console, 1 Steam Cloud */
    int account_action;                 /* ISAAC_ACCOUNT_* */
    int signin;                         /* ISAAC_SIGNIN_* */
    char signin_error[160];
    int qr_size;                        /* modules per side */
    uint8_t qr[ISAAC_QR_MAX_SIZE * ISAAC_QR_MAX_SIZE];
    int signout;                        /* ISAAC_SIGNOUT_* */
    int files;                          /* ISAAC_FILES_* */
    char files_summary[32];             /* "22 of 22 found", "4 missing" */
    char files_detail[192];             /* which ones, and what to do */
    float t;                            /* seconds, for the animations */
} isaac_launcher_view;

/* The font (a TTF/OTF in memory, kept alive by the caller). 0 when usable. */
int isaac_launcher_set_font(const void *data, size_t size);

/* Draws the whole screen. `stride` is in pixels. */
void isaac_launcher_draw(uint32_t *pixels, int stride, const isaac_launcher_view *view);

/* Frees the cached background, the glyphs and the face: the game gets the
 * memory back, and the font memory may go away after this. */
void isaac_launcher_release(void);

#endif
