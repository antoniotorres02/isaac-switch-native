#!/usr/bin/env python3
"""Renders the Switch launch screen (native_launcher.c) on the PC.

The screen is software-drawn, so the console's own code runs here unchanged:
each state the console can show is written to build/launcher-preview/*.png.
The console draws with its shared system font; the preview uses a TTF from
the PC (--font), so letter shapes and widths differ slightly.

    python3 tools/preview_switch_launcher.py [--font /path/to/font.ttf]
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "host/native"
OUT = ROOT / "build/launcher-preview"
FONTS = ["/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_launcher.h"

static uint32_t px[ISAAC_LAUNCHER_W * ISAAC_LAUNCHER_H];

#define GOOD_FILES(v) do { (v).files = ISAAC_FILES_OK; \
    snprintf((v).files_summary, sizeof (v).files_summary, "22 of 22 found"); } while (0)
#define BAD_FILES(v) do { (v).files = ISAAC_FILES_BAD; \
    snprintf((v).files_summary, sizeof (v).files_summary, "4 of 22 missing"); \
    snprintf((v).files_detail, sizeof (v).files_detail, "music.a, sfx.a and 2 more. " \
             "Copy them from your PC install to sdmc:/switch/isaac-recomp/guest."); } while (0)

static void base(isaac_launcher_view *v) {
    memset(v, 0, sizeof *v);
    for (int i = 0; i < ISAAC_STEAMSYNC_SLOTS; ++i) v->slot[i] = ISAAC_LAUNCHER_SLOT_PENDING;
    snprintf(v->account, sizeof v->account, "Steam \xc2\xb7 \xe2\x80\xa6" "1234567");
    v->t = 0.35f;
    v->files = ISAAC_FILES_OK;
    snprintf(v->files_summary, sizeof v->files_summary, "22 of 22 found");
#ifndef ISAAC_NO_STEAM
    v->workshop_available = 1;
#endif
}

static void ok_through(isaac_launcher_view *v, int last) {
    static const char *detail[] = { "steamid \xe2\x80\xa6" "1234567", "2026-10-27 22:41",
                                    "cmp1-mad1.steamserver.net:443", "cell 212", "game and all expansions",
                                    "7 files in the cloud" };
    for (int i = 0; i <= last; ++i) {
        v->step[i] = ISAAC_STEP_OK;
        snprintf(v->step_detail[i], sizeof v->step_detail[i], "%s", detail[i]);
    }
}

static int save(const char *dir, const char *name, const isaac_launcher_view *v) {
    isaac_launcher_draw(px, ISAAC_LAUNCHER_W, v);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.rgba", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(px, 4, ISAAC_LAUNCHER_W * ISAAC_LAUNCHER_H, f) != ISAAC_LAUNCHER_W * ISAAC_LAUNCHER_H) return 1;
    fclose(f);
    printf("%s\n", name);
    return 0;
}

#ifndef ISAAC_NO_STEAM
/* The in-game panel, laid over a dark page the size of the screen. */
static uint32_t panel_px[ISAAC_PANEL_W * ISAAC_PANEL_H];
static int save_panel(const char *dir, const char *name, const isaac_panel_view *pv) {
    isaac_panel_draw(panel_px, pv);
    for (int i = 0; i < ISAAC_LAUNCHER_W * ISAAC_LAUNCHER_H; ++i) px[i] = 0xff2a2018u;
    int ox = (ISAAC_LAUNCHER_W - ISAAC_PANEL_W) / 2, oy = (ISAAC_LAUNCHER_H - ISAAC_PANEL_H) / 2;
    for (int y = 0; y < ISAAC_PANEL_H; ++y)
        for (int x = 0; x < ISAAC_PANEL_W; ++x) {
            uint32_t s = panel_px[y * ISAAC_PANEL_W + x], a = s >> 24;
            uint32_t *d = &px[(oy + y) * ISAAC_LAUNCHER_W + ox + x], out = 0xff000000u;
            for (int c = 0; c < 24; c += 8)
                out |= ((((s >> c) & 0xff) * a + ((*d >> c) & 0xff) * (255 - a)) / 255) << c;
            *d = out;
        }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.rgba", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(px, 4, ISAAC_LAUNCHER_W * ISAAC_LAUNCHER_H, f) != ISAAC_LAUNCHER_W * ISAAC_LAUNCHER_H) return 1;
    fclose(f);
    printf("%s\n", name);
    return 0;
}
#endif

#ifdef ISAAC_NO_STEAM
const char *isaac_switch_game_files_label(void) {
    return "sdmc:/switch/isaac-recomp/guest";
}

static int states(const char *dir) {
    isaac_launcher_view v;
    int bad = 0;
    base(&v);
    v.phase = ISAAC_LAUNCHER_OFF;
    bad |= save(dir, "nosteam-1-ready", &v);
    BAD_FILES(v);
    bad |= save(dir, "nosteam-2-files-missing", &v);
    base(&v);
    v.phase = ISAAC_LAUNCHER_LAUNCHING;
    bad |= save(dir, "nosteam-3-launching", &v);
    return bad;
}
#endif

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *font = malloc((size_t)n);
    if (fread(font, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (isaac_launcher_set_font(font, (size_t)n)) return 3;
#ifdef ISAAC_NO_STEAM
    return states(argv[2]);
#else

    isaac_launcher_view v;
    int bad = 0;

    base(&v);
    v.phase = ISAAC_LAUNCHER_SYNCING;
    ok_through(&v, 1);
    v.step[2] = ISAAC_STEP_RUN;
    snprintf(v.step_detail[2], sizeof v.step_detail[2], "8 servers");
    snprintf(v.activity, sizeof v.activity, "CM list: 8 host(s)");
    bad |= save(argv[2], "1-syncing", &v);

    base(&v);
    v.phase = ISAAC_LAUNCHER_SYNCING;
    ok_through(&v, 4);
    v.step[5] = ISAAC_STEP_RUN;
    v.slot[0] = ISAAC_SYNC_SLOT_SAME;
    v.slot[1] = ISAAC_SYNC_SLOT_DOWNLOADED;
    v.conflict = 1;
    snprintf(v.conflict_slot, sizeof v.conflict_slot, "Slot 3 (progress)");
    snprintf(v.conflict_local, sizeof v.conflict_local, "2026-10-27 21:05");
    snprintf(v.conflict_cloud, sizeof v.conflict_cloud, "2026-10-26 23:48");
    v.conflict_choice = 0;
    bad |= save(argv[2], "2-conflict", &v);

    base(&v);
    v.phase = ISAAC_LAUNCHER_DONE;
    ok_through(&v, 5);
    snprintf(v.step_detail[5], sizeof v.step_detail[5], "1 downloaded");
    int slots[] = { ISAAC_SYNC_SLOT_SAME, ISAAC_SYNC_SLOT_DOWNLOADED, ISAAC_SYNC_SLOT_UPLOAD,
                    ISAAC_SYNC_SLOT_SAME, ISAAC_SYNC_SLOT_EMPTY, ISAAC_SYNC_SLOT_EMPTY };
    memcpy(v.slot, slots, sizeof slots);
    snprintf(v.activity, sizeof v.activity, "pull done, 1 file(s) replaced");
    v.t = 1.1f;
    bad |= save(argv[2], "3-ready", &v);

    base(&v);
    v.phase = ISAAC_LAUNCHER_FAILED;
    ok_through(&v, 1);
    v.step[2] = ISAAC_STEP_FAIL;
    snprintf(v.step_detail[2], sizeof v.step_detail[2], "no answer");
    snprintf(v.error, sizeof v.error, "cannot reach Steam: api.steampowered.com: TLS handshake failed (0x%x). "
             "Your saves stay on this console and nothing is uploaded at exit.", 0x1234);
    snprintf(v.activity, sizeof v.activity, "steam sync: pull skipped");
    bad |= save(argv[2], "4-failed", &v);

    base(&v);
    v.phase = ISAAC_LAUNCHER_FAILED;
    v.account[0] = 0;
    v.step[0] = ISAAC_STEP_FAIL;
    snprintf(v.step_detail[0], sizeof v.step_detail[0], "no token");
    snprintf(v.error, sizeof v.error, "No Steam sign-in on this console: copy steam-token.txt to "
             "sdmc:/switch/isaac-recomp/ (see STEAM-CLOUD.md).");
    bad |= save(argv[2], "5-no-token", &v);

    base(&v);
    v.phase = ISAAC_LAUNCHER_LAUNCHING;
    ok_through(&v, 5);
    memcpy(v.slot, slots, sizeof slots);
    bad |= save(argv[2], "6-launching", &v);

    /* the sign-in dialog over a page with no account, and the page behind it */
    static const char url[] = "https://s.team/q/1/1234567890123456789";
    base(&v);
    v.account[0] = 0;
    v.phase = ISAAC_LAUNCHER_SIGNED_OUT;
    v.step[0] = ISAAC_STEP_SKIP;
    snprintf(v.error, sizeof v.error, "Not signed in to Steam: your saves stay on this console and nothing is uploaded.");
    v.account_action = ISAAC_ACCOUNT_SIGN_IN;
    bad |= save(argv[2], "7-signed-out", &v);
    v.account_action = ISAAC_ACCOUNT_NONE;
    v.signin = ISAAC_SIGNIN_WAIT;
    bad |= save(argv[2], "8-signin-wait", &v);
    v.signin = ISAAC_SIGNIN_SCAN;
    v.qr_size = isaac_qr_encode(url, strlen(url), -1, v.qr);
    bad |= save(argv[2], "9-signin-qr", &v);
    v.signin = ISAAC_SIGNIN_FAILED;
    snprintf(v.signin_error, sizeof v.signin_error, "cannot connect api.steampowered.com:443.");
    bad |= save(argv[2], "10-signin-failed", &v);

    /* signing out from a page that synced */
    base(&v);
    v.phase = ISAAC_LAUNCHER_DONE;
    ok_through(&v, 5);
    memcpy(v.slot, slots, sizeof slots);
    v.account_action = ISAAC_ACCOUNT_SIGN_OUT;
    bad |= save(argv[2], "11-ready-signed-in", &v);
    v.account_action = ISAAC_ACCOUNT_NONE;
    v.signout = ISAAC_SIGNOUT_ASK;
    bad |= save(argv[2], "12-signout-ask", &v);
    v.signout = ISAAC_SIGNOUT_BUSY;
    bad |= save(argv[2], "13-signout-busy", &v);

    /* a token Steam no longer takes */
    base(&v);
    v.phase = ISAAC_LAUNCHER_FAILED;
    ok_through(&v, 2);
    v.step[3] = ISAAC_STEP_FAIL;
    snprintf(v.error, sizeof v.error, "Steam no longer accepts this console's sign-in: it was removed or has expired. "
             "X signs in again. Your saves stay on this console.");
    v.account_action = ISAAC_ACCOUNT_SIGN_IN;
    bad |= save(argv[2], "14-token-refused", &v);

    /* the account does not own the game: no sync, Play still works */
    base(&v);
    v.phase = ISAAC_LAUNCHER_FAILED;
    ok_through(&v, 3);
    v.step[4] = ISAAC_STEP_FAIL;
    snprintf(v.step_detail[4], sizeof v.step_detail[4], "not on this account");
    snprintf(v.error, sizeof v.error, "This Steam account does not have The Binding of Isaac: Rebirth, so nothing syncs. "
             "You can still play with the files on the SD card.");
    GOOD_FILES(v);
    v.account_action = ISAAC_ACCOUNT_SIGN_OUT;
    bad |= save(argv[2], "15-not-owned", &v);

    /* the game but not every expansion: a warning, and the sync went on */
    base(&v);
    v.phase = ISAAC_LAUNCHER_DONE;
    ok_through(&v, 5);
    v.step[4] = ISAAC_STEP_WARN;
    snprintf(v.step_detail[4], sizeof v.step_detail[4], "not all expansions");
    memcpy(v.slot, slots, sizeof slots);
    snprintf(v.error, sizeof v.error, "This Steam account does not have Repentance, Repentance+. "
             "Saves sync, but this console runs Repentance+.");
    GOOD_FILES(v);
    bad |= save(argv[2], "16-no-expansions", &v);

    /* the files: no Play while they are incomplete */
    base(&v);
    v.phase = ISAAC_LAUNCHER_DONE;
    ok_through(&v, 5);
    memcpy(v.slot, slots, sizeof slots);
    BAD_FILES(v);
    bad |= save(argv[2], "17-files-missing", &v);

    workshop_view workshop = {
        .tab = WORKSHOP_BROWSE, .grid = 1, .row_count = WORKSHOP_GRID_CARDS, .signed_in = 1, .selected = 1,
        .list_count = WORKSHOP_PAGE,
        .hints = {[WORKSHOP_HINT_A] = "Open", [WORKSHOP_HINT_X] = "Sort",
                  [WORKSHOP_HINT_Y] = "Search", [WORKSHOP_HINT_MINUS] = "Tag",
                  [WORKSHOP_HINT_SCROLL] = "Navigate", [WORKSHOP_HINT_PAGES] = "Pages",
                  [WORKSHOP_HINT_TABS] = "Tabs", [WORKSHOP_HINT_B] = "Go Back"},
    };
    v.workshop = &workshop;
    snprintf(workshop.heading, sizeof workshop.heading, "Browse");
    snprintf(workshop.context, sizeof workshop.context, "Most subscribed  •  Page 1  •  68 results  •  Tag Lua");
    snprintf(workshop.search, sizeof workshop.search, "Search  items");
    workshop.rows[0] = (workshop_row){.title = "Isaac Workshop example", .detail = "Author  •  1.6 MiB  •  2026-10-27  •  Installed"};
    workshop.rows[1] = (workshop_row){.title = "Item catalogue", .detail = "Author  •  3.2 MiB  •  2026-10-22  •  Update available"};
    workshop.rows[2] = (workshop_row){.title = "Boss room layout", .detail = "Author  •  416 KiB  •  2026-10-19"};
    const char *titles[] = {"Character portraits", "Room tracker", "Challenge notes", "Co-op markers",
                            "Boss health display", "Item collection", "Door indicators", "Floor map"};
    for (int i = 3; i < workshop.row_count; ++i) {
        snprintf(workshop.rows[i].title, sizeof workshop.rows[i].title, "%s", titles[i - 3]);
        snprintf(workshop.rows[i].detail, sizeof workshop.rows[i].detail,
                 "Workshop author  •  %d KiB  •  2026-10-12  •  %d subscribed", i * 42, 1000 - i * 13);
    }
    static workshop_thumb tile[WORKSHOP_GRID_CARDS];
    for (int i = 0; i < workshop.row_count; ++i) {
        tile[i].width = 256;
        tile[i].height = 144;
        for (int y = 0; y < 144; ++y)
            for (int x = 0; x < 256; ++x)
                tile[i].pixels[y * 256 + x] = 0xff000000u | (uint32_t)(y + 18 * i) << 16 |
                    (uint32_t)(x / 2 + 25 * ((x / 12 + y / 12) & 1)) << 8 | (uint32_t)(20 * i + x / 4);
        workshop.rows[i].thumb = &tile[i];
    }
    bad |= save(argv[2], "30-workshop-browse", &v);
    workshop.busy = 1;
    workshop.done = 1048576;
    workshop.total = 3145728;
    memset(workshop.hints, 0, sizeof workshop.hints);
    workshop.hints[WORKSHOP_HINT_PLUS] = "Queue mod";
    workshop.hints[WORKSHOP_HINT_SCROLL] = "Navigate";
    workshop.hints[WORKSHOP_HINT_B] = "Cancel Queue";
    snprintf(workshop.notice, sizeof workshop.notice, "Installing Item catalogue • 2 queued");
    bad |= save(argv[2], "36-workshop-queue", &v);


    workshop = (workshop_view){.tab = WORKSHOP_BROWSE, .detail = 1, .signed_in = 1,
        .hints = {[WORKSHOP_HINT_A] = "Update", [WORKSHOP_HINT_X] = "Remove",
                  [WORKSHOP_HINT_Y] = "Review dependencies", [WORKSHOP_HINT_B] = "Back"}};
    snprintf(workshop.heading, sizeof workshop.heading, "Item catalogue");
    snprintf(workshop.context, sizeof workshop.context, "Workshop author  •  3.2 MiB  •  2026-10-22  •  #3181120001");
    snprintf(workshop.search, sizeof workshop.search, "Lua, Items");
    snprintf(workshop.description, sizeof workshop.description,
             "Adds a catalogue of discovered items to the game's menus.\n%cA\nBrowse the catalogue from the menu.",
             WORKSHOP_MEDIA_MARKER);
    workshop_animation animation = {.frames = {&tile[1], &tile[3]}, .delay = {200, 300}, .count = 2};
    workshop.media[0] = &animation;
    workshop.thumb = &tile[1];
    snprintf(workshop.dependencies, sizeof workshop.dependencies,
             "Update available  •  1 required, 1 missing\n3181120002 (missing)");
    bad |= save(argv[2], "31-workshop-details", &v);
    workshop.confirming = 1;
    snprintf(workshop.notice, sizeof workshop.notice, "Install this mod locally?");
    bad |= save(argv[2], "35-workshop-confirm-install", &v);

    workshop = (workshop_view){.tab = WORKSHOP_SYNC, .row_count = 2, .list_count = 2, .signed_in = 1,
        .hints = {[WORKSHOP_HINT_A] = "Select / skip", [WORKSHOP_HINT_X] = "Continue",
                  [WORKSHOP_HINT_B] = "Discard"}};
    snprintf(workshop.heading, sizeof workshop.heading, "Review changes");
    snprintf(workshop.context, sizeof workshop.context, "2 changes  •  1 selected");
    workshop.rows[0] = (workshop_row){.title = "Item catalogue", .detail = "Selected  •  Update on Switch"};
    workshop.rows[1] = (workshop_row){.title = "Boss room layout", .detail = "Skipped  •  Remove from Switch"};
    bad |= save(argv[2], "32-workshop-sync-review", &v);

    workshop = (workshop_view){.tab = WORKSHOP_BROWSE, .detail = 1, .signed_in = 1, .busy = 1,
                               .done = 1048576, .total = 3145728,
                               .hints = {[WORKSHOP_HINT_B] = "Cancel"}};
    snprintf(workshop.heading, sizeof workshop.heading, "Item catalogue");
    snprintf(workshop.description, sizeof workshop.description, "Adds a catalogue of discovered items to the game's menus.");
    snprintf(workshop.notice, sizeof workshop.notice, "Downloading and verifying mod...");
    bad |= save(argv[2], "33-workshop-downloading", &v);

    workshop = (workshop_view){.tab = WORKSHOP_BROWSE,
        .hints = {[WORKSHOP_HINT_Y] = "Steam QR sign-in", [WORKSHOP_HINT_TABS] = "Tabs",
                  [WORKSHOP_HINT_B] = "Go Back"}};
    snprintf(workshop.heading, sizeof workshop.heading, "Browse");
    snprintf(workshop.description, sizeof workshop.description, "Sign in to browse the Steam Workshop.");
    bad |= save(argv[2], "34-workshop-signed-out", &v);
    v.workshop = NULL;

    isaac_panel_view pv;
    memset(&pv, 0, sizeof pv);
    snprintf(pv.account, sizeof pv.account, "Steam \xc2\xb7 \xe2\x80\xa6" "1234567");
    pv.auto_upload = 1;
    pv.t = 0.35f;
    pv.state = ISAAC_PANEL_READY;
    snprintf(pv.message, sizeof pv.message, "2 saves changed since the last upload");
    snprintf(pv.detail, sizeof pv.detail, "persistentgamedata1, gamestate1. A sends them to Steam Cloud now.");
    bad |= save_panel(argv[2], "20-panel-ready", &pv);
    pv.state = ISAAC_PANEL_BUSY;
    snprintf(pv.message, sizeof pv.message, "Uploading\xe2\x80\xa6");
    snprintf(pv.detail, sizeof pv.detail, "logged on (cell 212)");
    bad |= save_panel(argv[2], "21-panel-busy", &pv);
    pv.state = ISAAC_PANEL_DONE;
    snprintf(pv.message, sizeof pv.message, "Uploaded 2 saves");
    snprintf(pv.detail, sizeof pv.detail, "Steam Cloud has what this console has. 22:41");
    bad |= save_panel(argv[2], "22-panel-done", &pv);
    pv.state = ISAAC_PANEL_FAILED;
    pv.auto_upload = 0;
    snprintf(pv.message, sizeof pv.message, "Upload failed");
    snprintf(pv.detail, sizeof pv.detail, "cannot reach Steam: cannot resolve api.steampowered.com (7). "
             "Check the console's internet connection; your saves stay on the card.");
    bad |= save_panel(argv[2], "23-panel-failed", &pv);
    pv.state = ISAAC_PANEL_UNAVAILABLE;
    snprintf(pv.message, sizeof pv.message, "Nothing can be uploaded this session");
    snprintf(pv.detail, sizeof pv.detail, "The sync before the game did not finish, so this console cannot "
             "compare its saves with Steam Cloud. Restart the game to sync again.");
    bad |= save_panel(argv[2], "24-panel-unavailable", &pv);

    return bad;
#endif
}
'''


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=next((f for f in FONTS if Path(f).exists()), None))
    args = ap.parse_args()
    if not args.font:
        sys.exit("no font found: pass --font")
    from PIL import Image

    OUT.mkdir(parents=True, exist_ok=True)
    src = OUT / "preview.c"
    src.write_text(HARNESS)
    exe = OUT / "preview"
    ft = subprocess.run(["pkg-config", "--cflags", "--libs", "freetype2"], capture_output=True, text=True,
                        check=True).stdout.split()
    flags = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-DISAAC_LAUNCHER_PREVIEW", "-I", str(NATIVE)]
    # The screen of a --no-steam build, with what it links: no native_qr.c.
    variants = ((exe, [str(src), str(NATIVE / "native_launcher.c"), str(NATIVE / "native_qr.c")], []),
                (OUT / "preview-nosteam", [str(src), str(NATIVE / "native_launcher.c")], ["-DISAAC_NO_STEAM=1"]))
    for binary, sources, defines in variants:
        subprocess.run(flags + defines + sources + ["-o", str(binary)] + ft + ["-lm"], check=True)
        r = subprocess.run([str(binary), args.font, str(OUT)], capture_output=True, text=True)
        if r.returncode:
            sys.exit("preview failed (%d)\n%s" % (r.returncode, r.stderr))
        for name in r.stdout.split():
            raw = OUT / (name + ".rgba")
            Image.frombytes("RGBA", (1280, 720), raw.read_bytes()).save(OUT / (name + ".png"))
            raw.unlink()
            print(OUT / (name + ".png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
