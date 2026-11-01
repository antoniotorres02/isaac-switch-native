# Steam Cloud saves, straight from the console

The native build keeps its saves on the SD card. This is how they also live in
the account's Steam Cloud: the console logs on to Steam as a client and moves
the files itself, so **no PC is involved at sync time** — no Steam client
running, no folder bridge, no local network.

It is the route the Steam client itself uses. There is no supported Web API for
it: `ICloudService` answers HTTP 401 to an account token (it needs a publisher
key, i.e. the game's own studio), which is why the console speaks the CM
protocol instead.

| Piece | Where |
| --- | --- |
| Reference implementation, provisioning tool | `host/steam_cloud_cm.py` |
| Protocol, framing, ZIP and SHA-1 | `native/native_steamcm.c`, `native_steamproto.c`, `native/native_zip.c`, `native/native_sha1.c` |
| QR sign-in, QR codes | `native/native_steamauth.c`, `native/native_qr.c` |
| Sync rules, state file, backups | `native/native_steamsync.c` |
| Switch wiring, sync thread, settings | `native/native_switch.c` |
| Launch screen (Steam-style, Play button, conflict dialog) | `native/native_launcher.c` |
| Check of the game's files, in every build | `native/native_gamefiles.c` |
| Host tests | `tools/test_switch_steamsync.py` |
| Launch screen previews (PNG of every state, drawn on the PC) | `tools/preview_switch_launcher.py` |

## Signing in and out

The console needs the account's **refresh token** (`iss=steam`).

**On the console**, as on a Steam Deck: with no `steam-token.txt` the launch
screen opens a dialog with a QR code. Scan it from the Steam app on a phone and
approve; the token is written to the SD card and the pull starts. B closes the
dialog and the game can be played without sync; X on the page opens it again.
The code renews itself while it waits. If Steam later refuses the token (removed
from the account, or expired) the screen says so and X signs in again.

**X on the page signs out** once signed in, after a confirmation. It logs on and
calls `Authentication.RevokeToken` (`revoke_action` permanent), so the session
leaves the account's device list, then deletes `steam-token.txt`. Without a
network the token is still deleted and the screen says Steam was not told; remove
the device by hand then.

Both sign-in and sign-out also delete `saves/steam-sync-state.txt` and the pull
marker, since those notes describe the previous account's cloud: the next pull
compares as a first sync. Saves are never touched by this, so a different account
sees a conflict dialog for each slot that differs, and a slot only this console
has **is uploaded to the account now signed in**, which the slot list shows as
"Uploads on exit" before Play is pressed.

**From a PC** still works, and is the only way with `launcher=0`:

```sh
python3 host/steam_cloud_cm.py login        # scan the QR with the Steam app
python3 host/steam_cloud_cm.py token-file   # build/steam-cloud/steam-token.txt
```

Copy `build/steam-cloud/steam-token.txt` to `sdmc:/switch/isaac-recomp/steam-token.txt`.
It is two lines:

```
steamid=7656119…
token=eyJhbGciOi…
```

Either way the session is named "Isaac on Switch (NRO)" and can be revoked at any
time in Steam → Settings → Security → Manage devices. The token is plaintext on
the SD card: anyone who reads the card can use it.

## Does the account have the game?

Every pull first asks Steam about the game and its expansions: 250900 (the game),
401920 (Afterbirth), 570660 (Afterbirth+), 1426300 (Repentance) and 3353470
(Repentance+), the ids a Steam install's `appmanifest_250900.acf` names. It is only
ever a warning about the sync, never a lock on the game:

| Steam says | The screen | The sync |
| --- | --- | --- |
| the game and all four expansions | "game and all expansions" | runs |
| the game, not every expansion | names what is missing | runs |
| not the game | "not on this account" | **stops**, on both sides: nothing is compared or uploaded |
| no answer | "not verified" | runs |

Play stays available in all four, and nothing is asked without a sign-in. This is
a courtesy, not copy protection: the source is public and the check can be
compiled out. What stands between a person and the game is that every NRO has to
be built from that person's own copy ([BUILDING.md](BUILDING.md)).

## Building without Steam

```sh
python3 host/build_native.py --profile switch --no-steam
```

writes `build/browsing-native/switch-nosteam/isaac-browsing.nro` with none of this
in it: no sign-in, sync, ownership check or TLS. The launch screen keeps Play and
the check of the game's files (see [PLAYING.md](PLAYING.md#settingsini)), and the
`steam_sync` and `sync_prompt` settings do not exist. Measured on the ELF: no
`isaac_steam*`, QR, ZIP or `ssl*` symbols, and none of the strings the client sends
to Steam (the normal build has 28 such symbols and 8 such strings).

## What happens at run time

* **Pull**, before the game opens its saves: the cloud is listed and each of the
  six slots is compared with what the last sync left on both sides
  (`saves/steam-sync-state.txt`).
  * only Steam changed → download it, after backing up the console copy;
  * only the console changed → nothing now, push uploads it at exit;
  * both changed → a dialog on the launch screen asks which copy to keep
    (left/right to choose, A to keep it); with `sync_prompt=0`, or with
    `launcher=0`, the newest timestamp wins;
  * neither changed → nothing.
* **Push**, after the game closes (it has not yet worked on a console, see
  [Verified, and not](#verified-and-not)). Close it with **HOME → Close Software**: the
  NRO holds the exit (`appletLockExit`), so the game quits through its own path
  and the push runs before the process ends. The system allows 15 s after the
  request; measured on the console, the game was down 469 ms after it and the
  whole teardown took 636 ms, which leaves the rest for the upload. An upload
  the system cuts short is never committed, and the next push sends it again.
  Before this, HOME → Close terminated the process at once and nothing was
  uploaded. Then, every slot whose bytes differ from what was
  last synced is uploaded. A push only runs when this session pulled first: the
  NRO uploads only after a pull of the same run ran to the end (so "Play anyway"
  after a failed pull uploads nothing), and the sync engine also wants the
  recent `saves/steam-sync-pull.txt` that a pull leaves (it must never push
  saves it could not compare).

Nothing is ever deleted. A save that is replaced by a download, and any copy
involved in a conflict, is kept under `saves/save_backups/steam-<date>/`.

`settings.ini` (next to the NRO's data on the SD):

```
steam_sync=1     # 0 turns the sync off completely
sync_prompt=1    # 1 asks on screen; 0 = the newest copy wins silently
launcher=1       # 0 = no launch screen: sync and start the game at once
```

### The launch screen

Before the game the NRO draws its own screen, laid out like a game page of the
Steam library: the title banner, a play bar with the green **Play** button and
the cloud status, the steps of the pull (sign-in, clock, connection, logon,
comparison) and what happened to each of the six save files. The pull runs on
its own thread while the screen animates; the game starts only when **A** is
pressed on Play, which is disabled while the pull runs. If the sync could not
finish the screen says why, Play becomes "Play anyway" (nothing is uploaded at
exit then) and **Y** retries; **+** leaves without starting the game.

It is drawn in software on a libnx framebuffer with the console's shared
system font through FreeType, and closed before the game creates its EGL
context. The push after the game has no screen (the game's context still owns
the window) and only writes the stage log. `--autotest` runs skip all of it.

If the clock is not set (TLS verifies certificates against it) or Steam cannot
be reached, the screen shows it, the sync logs why in `browsing-last-run.txt`,
and the game can still be started. The same goes for not being signed in.

## Protocol, as measured

Everything below was checked against the live service with the account's own
files; the field numbers come from SteamDatabase/Protobufs (MIT).

* **Transport**: websocket to a CM from
  `ISteamDirectory/GetCMListForConnect?cmtype=websockets`, over TLS. A payload is
  `[emsg|0x80000000][header_len][header][body]` — no body length. Replies often
  arrive bundled in a `CMsgMulti`, gzipped when it carries `size_unzipped`.
* **Logon**: `CMsgClientLogon` (5514) with the *refresh* token in
  `access_token` (108). The access tokens derived from it are rejected with
  `AccessDenied` (15) — the CM only takes `iss=steam` tokens. Measured reply:
  `eresult=1`, cell 212, heartbeat 9 s.
* **QR sign-in**: Web API, no account needed. `IAuthenticationService/
  BeginAuthSessionViaQR` with `platform_type=1` gives `client_id`, `challenge_url`
  (`https://s.team/q/1/<id>`, 39 bytes, a 29-module code), `request_id` (16 bytes)
  and `interval` (5 s); `PollAuthSessionStatus` answers with `refresh_token`
  once approved, a `new_client_id` and `new_challenge_url` meanwhile, and
  `X-eresult: 27` when the session is over. Replies carry a `Content-Length`.
* **Service calls**: `EMsg 151` with `target_job_name` = `Cloud.<Method>#1`; the
  reply is `EMsg 147` with the same name and `eresult` in the header.
* **Files**: `Cloud.EnumerateUserFiles` returned 7 entries for 250900 — the six
  slots plus a legacy `rep_persistentgamedata1.dat` — all with `platforms=All`.
  `sha_file` is the SHA-1 of the **raw** bytes, which is what uploads declare.
* **Ownership**: `CMsgClientGetAppOwnershipTicket` (857, `app_id` in field 1),
  answered by 858 with `eresult` in field 1. Measured with the account's own token
  (`steam_cloud_cm.py owned`): 1 and a 178-202 byte ticket for what it owns, 15
  (AccessDenied) and no ticket for what it does not. The ticket is not kept.
* **Stored compressed or as-is**: Steam wraps a file in a **ZIP with a single
  entry `z`** only when it compressed it. Measured: `rep+persistentgamedata1.dat`
  comes back as a 6 482-byte ZIP for 16 756 raw bytes, while
  `rep+persistentgamedata2.dat` comes back as its own 5 980 bytes
  (`ISAACNGSAVE09R…`, `file_size == raw_file_size`). Both sides therefore look at
  the leading `PK\x03\x04` before unwrapping (`isaac_zip_looks_like_zip`); a save
  that is not a ZIP is its own content.
* **Download**: `Cloud.ClientFileDownload` → presigned S3 URL + headers → plain
  HTTPS GET → unzip. The SHA-1 was verified on 2/2 files.
* **Upload**: `Cloud.BeginHTTPUpload` (`file_size`, `file_sha` hex of the raw
  bytes) → HTTPS PUT of the **save as-is** to the returned URL with its headers
  → `Cloud.CommitHTTPUpload` (`transfer_succeeded`). The request has no
  raw-size field: what the PUT carries is the file for every client. Until
  2026-10-29 the console PUT a ZIP there; Steam kept the ZIP as the file
  (`file_size == raw_file_size`), our own pull unwrapped it, but the PC game
  got the ZIP as its save and called it corrupt. Measured: `PUT 201`, `file_committed=1`; overwriting
  with different bytes kept the same `ugcid` and returned `PUT 200`.
* **Deduplication**: uploading bytes Steam already has answers `eresult 29`
  (DuplicateRequest) on both upload paths. The sync treats that as success, and
  it is why a no-op session never rewrites a cloud file.
* The client-style upload (`BeginAppUploadBatch` + `ClientBeginFileUpload`)
  answered `eresult 108` (TooManyPending) in this environment, so the console
  uses the HTTP pair above, which does not need an upload batch.

## Building it

The sync is part of the normal NRO build (`native/*.c` is picked up by glob); it
adds `-lz` for the ZIP. The lift output has to exist first, as for any other
build of this port:

```sh
python3 host/build_native.py --profile switch      # NRO
python3 tools/test_switch_steamsync.py                               # host tests
```

The build used here ran inside the pinned devkitA64 image with the upstream
checkout mounted read-only and `--upstream /upstream`; it produced
`build/browsing-native/switch/isaac-browsing.nro`, sha256
`74e266848a956d0624e738822d25b9c79102b36529fb87c74c18b499b78776aa`.

## Verified, and not

Verified on the host, against the live service: logon, listing, download with
SHA-1 check, upload (new file, then overwrite), delete of a self-test file, and
the dedup reply. Verified on the host with `tools/test_switch_steamsync.py`:
SHA-1 against FIPS vectors, ZIP against Python's `zipfile` in both directions,
framing including gzipped multis, and the whole sync engine against a fake Steam
(first pull, no-op, local change, cloud change, conflict both ways, prompt both
answers, failed download, console-only save, push without pull).

**QR sign-in on the console, 2026-11-01 (NRO `612eee56…`)**: with no token on the
card the dialog came up by itself, the code was scanned with the Steam app and
approved, the token was written, and the pull ran at once: 7 cloud files, logon on
`cmp2-hkg1`, four conflicts answered "Steam Cloud" with the console's copies backed
up, then Play and a session of 70 082 frames.

**The upload at exit failed in that same session**, the first time a real push
ran on a console:

```
game closed 370 ms after HOME -> Close
steam sync: push
steam sync: cannot reach Steam: cannot resolve api.steampowered.com (7)
push over 400 ms after HOME -> Close
```

`getaddrinfo` failed at once, not after a timeout. The same token and network had
resolved that name minutes before, and did again at the next start. The cause is
not known, nor whether a wait, or quitting from the game's menu, would avoid it.
Nothing is lost: the saves stay on the card, marked "Uploads on exit" at the next
start. **Until this is solved, treat Steam Cloud on the console as download-only.**

**Ownership and game files on the console, 2026-11-01** (NRO `1189e704…`): the
launch screen showed "game and all expansions" from the live service and "22 of 22
found". With `fonts.a` renamed on the card, Play went grey naming it, A did nothing,
and + left without uploading. The `--no-steam` build (NRO `b0ef27a7…`) showed its
own screen, said "22 of 22 found" and started the game; that run was closed during
the intro, so it did not reach a room. The answers for "not the game" and "not every
expansion" have been seen from Steam only through the PC tool, and on screen only in
the previews and against a fake Steam.

**Sign-out has not been run on a console, so `RevokeToken` has not been seen
working.** On the host the QR encoder matches `qrencode` module by module for
lengths 1-213, OpenCV's decoder reads the code from the rendered screen, the C
parser read a real `BeginAuthSessionViaQR` reply and its URL encoded and decoded
back, and the whole poll sequence ran against a fake Steam. To check the revoke
without touching the console's session, from a PC:

```sh
python3 host/steam_cloud_cm.py login --session build/steam-cloud/revoke-test.json
python3 host/steam_cloud_cm.py revoke --session build/steam-cloud/revoke-test.json
```

The second command logs on again afterwards and says whether Steam now refuses the
token.

**On the console (2026-10-27, NRO `a0ec1760…`)**, pull only, nothing uploaded:
the launch screen drew on the framebuffer with the shared font, the sync thread
ran beside it, TLS to the CM and to Steam's storage worked, logon `eresult 1`,
7 cloud files listed, three conflicts answered "Steam Cloud" in the dialog, and
4 files downloaded and written (`persistentgamedata1-3`, `gamestate1`) while 2
were already equal; the state file then held local == cloud for all six, with
nothing pending. Leaving with + ended the NRO cleanly. With `steam_sync=0`, Play
handed the display to the game (its intro was on screen) and no save changed.

The first console runs found four faults the host could not show, now fixed and
covered where a host test can reach them:

* the socket went to `sslConnectionSetSocketDescriptor` as a newlib descriptor;
  libnx wants `socketSslConnectionSetSocketDescriptor`, which passes the BSD one;
* the logon's `eresult` was read from the header (field 13, absent there, so it
  read as the default 2); a logon answers in its body (field 1);
* Horizon's file system does not replace a file on rename, so every download
  over an existing save failed to write (the host tests now build the engine
  with a rename that refuses an existing target);
* two download URLs come with a `host` header of their own, and a second Host
  line made Steam's storage answer `HTTP 400`.

A download that fails now fails the pull (the screen says so and offers a
retry), leaves the console's copy out of the next push, and is retried by the
next pull. **Still not verified on the console**: an upload; see the failed push
above.

Not synced: `options.ini` (it differs on purpose: `SteamCloud=0`, resolution),
mod save data under `saves/data/`, and the legacy `rep_persistentgamedata1.dat`.
Only the first 100 cloud files are listed, which is far above the seven Isaac
keeps.

## Risk

This logs in as an unofficial Steam client with the account's own token. That is
how the Steam client talks, and the data is the account's own, but Valve has not
authorised third-party clients: an account can in principle be flagged or
locked for it, and the token on the SD card is a real credential. The supported
alternatives (Steam's own client syncing a folder on a PC) avoid both risks and
need the PC to be on.
