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

#ifdef ISAAC_NO_STEAM
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
