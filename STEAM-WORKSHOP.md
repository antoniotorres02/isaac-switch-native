# Steam Workshop on Switch

Open **Workshop** with **R** on the launcher. Browse, installs, updates and
subscription review use the same QR sign-in and Steam account as Steam Cloud.
Installed mods and their enable settings are available offline. See the
[playing guide](PLAYING.md#steam-workshop).

Press **+** in Browse to start a local install or add a mod to the
download queue. The browser downloads one item at a time and shows progress
on Browse. The item details page still offers a confirmation dialog.
Subscription changes still require opening the Sync tab and reviewing the plan.

## Building for testing

Build the tools image from this checkout so it includes the Workshop CDN
and image libraries. The currently published upstream image predates them.

```sh
docker build -t isaac-switch-builder:workshop -f docker/Dockerfile .
python3 host/builder/isaac_switch_builder.py build \
  --folder ./build/switch-workshop --game "/path/to/The Binding of Isaac Rebirth" \
  --exe "/path/to/isaac-ng.unpacked.exe" --image isaac-switch-builder:workshop --steam
```

## Steam protocol

The client uses App ID and Workshop depot ID `250900`. A Workshop job reads the
existing `steam-token.txt` and opens the existing authenticated CM transport.
Cloud closes its connection after each operation. Workshop also closes its
connection when the job finishes. The launcher gives the CM, TLS buffers and
libnx sockets to one worker at a time. A preview image uses HTTPS without
account credentials.

| Operation | Steam messages |
| --- | --- |
| Browse and search | `PublishedFile.QueryFiles#1` requests up to 24 results with tag filters, search text and Steam's opaque cursor. Full descriptions and tag lists are fetched for the selected item |
| Details and required items | `PublishedFile.GetDetails#1` with `includechildren`. `hcontent_file` supplies the depot manifest ID |
| Author display name | `Player.GetPlayerLinkDetails#1`, using the public persona name |
| Subscription list | `PublishedFile.GetUserFiles#1` with the authenticated SteamID, app, `mysubscriptions` selector and `subscriptiondate` order |
| Subscription confirmation | `PublishedFile.AreFilesInSubscriptionList#1` validates relevant changes before execution and checks the result of an approved Steam write |
| Steam subscription changes | `PublishedFile.Subscribe#1` or `PublishedFile.Unsubscribe#1`, after the user selects the action and confirms the plan |
| Ownership and download authorization | Existing app ownership ticket, `ContentServerDirectory.GetManifestRequestCode#1`, and `ClientGetDepotDecryptionKey` |
| CDN selection and credentials | `ContentServerDirectory.GetServersForSteamPipe#1`. On HTTP 401 or 403 the client requests a host-specific `GetCDNAuthToken#1` |
| Updates | Compare the installed and current manifest IDs. Re-read details for each approved download so a changed manifest requires a new review |

The published-file schemas and Steamworks query enum supply the request fields
and browse sort values. [Published-file protobufs](https://github.com/SteamTracking/Protobufs/blob/master/steam/steammessages_publishedfile.steamclient.proto)
[Steamworks query enum](https://partner.steamgames.com/doc/webapi/IPublishedFileService#EPublishedFileQueryType).

The subscription selector follows a native client implementation that uses
the account's CM connection. [Sts2MobileLauncher Workshop](https://github.com/ModinMobileSTS/Sts2MobileLauncher/blob/main/doc/modding/mod-and-compat-notes.md).

DepotDownloader shows how to resolve the Workshop depot. SteamKit and
node-steam-user show how to authorize and fetch content. [DepotDownloader](https://github.com/SteamRE/DepotDownloader/blob/master/DepotDownloader/ContentDownloader.cs)
[SteamKit CDN client](https://github.com/SteamRE/SteamKit/blob/master/SteamKit2/SteamKit2/Steam/CDN/Client.cs)
[node-steam-user CDN implementation](https://github.com/DoctorMcKay/node-steam-user/blob/master/components/cdn.js)
[Content directory protobufs](https://github.com/SteamTracking/Protobufs/blob/master/steam/steammessages_contentsystem.steamclient.proto)
[Isaac app depots](https://steamdb.info/app/250900/depots/#other).

## Download and installation

The worker verifies the ZIP-wrapped v5 manifest's CRC and checks that the
embedded depot and manifest IDs match the request. It parses the protobuf
mappings, rejects symlinks and unsafe paths, verifies the chunk layout and
checks SD space before opening staging. The manifest's optional signature
section is parsed as framing. Trust in the manifest comes from the authenticated
request and the TLS-validated Steam CDN.

The worker fetches encrypted chunks over HTTPS, decrypts them with the
authorized depot key and supports Steam's VZip (LZMA), VSZa (Zstandard)
and ZIP frames. It checks the chunk length, SHA-1 and Steam Adler-32, then
verifies each file against its manifest SHA-1. Files are written one chunk at
a time and flushed before installation. The UI reports verified bytes and total
uncompressed file bytes. Cancellation stops the active transfer and clears
staging. Network retries are bounded across Steam content servers.

The worker uses one download at a time. Manifest buffers are capped at
8 MiB compressed and 16 MiB unpacked. The wire, decrypted and raw chunk
buffers are each capped at 2 MiB. The LZMA dictionary is capped at 32 MiB.
Browse pages contain up to 24 items and keep a rolling history of recent
cursors. The two-column grid lazily loads visible thumbnails into a cache of
16 images at up to 256 × 144 pixels each. The detail view shows inline PNG,
JPEG and GIF images, fetched over HTTPS without Steam credentials from Steam
image hosts and `i.imgur.com`. PNG and JPEG are limited to 2048 × 2048 input
pixels. GIFs are limited to 1024 × 1024 input pixels and loop their first
16 frames. The browser caches up to three inline images at a time.

## Local registry and recovery

An NRO keeps each managed mod in `sdmc:/switch/isaac-recomp/guest/mods/workshop_<id>/`. An installed HOME Menu build uses `sdmc:/switch/isaac-recomp/mods/workshop_<id>/`
and must contain `metadata.xml` at its root. A hidden
`.isaac-workshop-v1` protobuf record inside the folder is the registry entry.
It records App ID, Published File ID, title, installation folder, manifest ID,
Workshop update time, install time, installed bytes, dependencies and the
verified manifest's SHA-1. The normal game's mod loader skips hidden files.

The launcher owns `sdmc:/switch/isaac-recomp/workshop/` (or `workshop-title/` with the HOME Menu build) for staging, backups
and removal cleanup. A marker identifies the launcher-owned workspace, and
a pre-existing populated folder without that marker is left alone. An
installed folder and its record become visible
together when the verified stage is renamed into place. Updates rename the
previous folder to a backup before committing the replacement. If the process
stops between the renames, recovery restores the backup. After the replacement
is visible, recovery can clean up the old backup. Locally removing a mod
renames it out of the game's directory before cleaning up its files. Recovery
is local and runs before Play or when preparing another transaction. FAT and
exFAT media still depend on the SD card and filesystem to persist a completed
rename through power loss.

A folder's numeric suffix alone gives it no Workshop registry entry.
Unmanaged folders appear in Installed and can be enabled or disabled with
the game's existing `saves/mod-state` markers. Workshop updates, removal
and synchronization act only on managed installations. Mod save data under
`saves/data/` stays outside these transactions.

## Explicit synchronization

The Sync tab reads subscriptions only when the user selects **Sync Steam
Mods**. It compares those subscriptions with the local registry and presents
the changes for review. The user can toggle each proposed change and must
confirm the resulting selection before execution. A preview tied to a
different Steam account is rejected. Local-only subscription and removal
choices, and the two-way unsubscribe alternative, start deselected. Selecting
opposing actions for the same item clears the previous choice. A change in
membership or manifest between review and execution is reported as a conflict.
Local removal also checks known dependencies of other installed mods.

Launching the application, signing in, opening Browse, starting or quitting
the game, and Steam Cloud activity never start mod synchronization. Installing
or removing one mod locally leaves its Steam membership as it was.

## Testing and current limits

Run the host fixtures and launcher previews with these commands.

```sh
python3 tools/test_switch_workshop.py
python3 tools/test_switch_steamsync.py
python3 tools/preview_switch_launcher.py
```

`host/steam_workshop_probe.py` exercises the native request, manifest and
installer code with the existing Python Cloud CM client on a developer PC.
The probe reads an existing QR session JSON from `host/steam_cloud_cm.py login`
or the Switch's `steam-token.txt` directly. It never changes Steam subscriptions.
A download requires an explicit item ID and a new output directory.

```sh
python3 host/steam_workshop_probe.py browse --session build/steam-cloud/session.json --search items
python3 host/steam_workshop_probe.py details --session build/steam-cloud/session.json --id 836319872
```

The build uses libnx TLS and AES alongside devkitPro's zlib, liblzma,
libzstd, libpng and libjpeg-turbo portlibs. Host fixtures use the desktop
versions of those libraries and OpenSSL for AES. There is no Python or PC
component in the Switch runtime. `--no-steam` excludes the Workshop browser,
networking and auth while keeping normal local game-mod loading.

- Legacy direct-file Workshop uploads, collections and items without a valid
  Isaac mod layout are unavailable for install. The browser reports these
  items as unavailable once it has details.
- The plan supports up to 4,096 Steam subscriptions and 4,096 local folders.
  Each item records up to 64 direct dependencies. A truncated list is shown
  as partial, and the download action for dependencies requires a complete
  list. Manually copied dependencies are outside the Workshop registry.
- QR credentials are stored in the original plaintext token file on the SD
  card. Workshop adds no credential file. Keep the card and any backup
  containing `steam-token.txt` private. You can revoke the console session
  from Steam's device settings. [Steam Cloud sign-in and storage](STEAM-CLOUD.md#signing-in-and-out).
- Host fixtures cover native request and response handling, CDN authorization,
  encrypted manifest paths, all three chunk codecs, failed and interrupted
  updates, cancellation, recovery, registry classification and controller
  confirmation. Both desktop build variants reached the starting room and
  loaded a synthetic Lua mod. The preview renderer also passed with and
  without Steam. Authenticated Steam operations and Switch hardware remain
  unverified in this workspace.
