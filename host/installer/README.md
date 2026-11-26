# Installer: the game as a title on the HOME menu

`build_all.py --format nsp` prepares the installer NRO and its payload. Run the
installer on the console to add the game to the HOME Menu. Use `--format both`
to also include a direct-launch NRO and its loose game files on the same card.

```
sd-card/switch/isaac-recomp-installer.nro
sd-card/switch/isaac-recomp/install/program.nca   the game: NSO + RomFS with the game's files
sd-card/switch/isaac-recomp/install/control.nca   name, icon, NACP
sd-card/switch/isaac-recomp/install/install.txt
```

## Why the console finishes it

An installable NCA has its 0xC00-byte header encrypted with the console's
*header key*, which cannot ship with the Builder. Nothing else needs a key:
the sections are stored in plaintext (encryption type 1, which FS reads like
any other), so `payload.py` writes both NCAs complete — PFS0/IVFC hash trees,
superblocks, every header field — and leaves only that encryption.

On the console, `source/switch_crypto.c` derives the header key the way FS does
at boot (Atmosphère's `SetUpKekAccessKeys`/`GenerateNcaKey`: `spl` seals a kek
from a public source and unwraps the encrypted key with it).
`source/nca_install.c` encrypts the headers, streams both NCAs into ncm
placeholders while hashing them, then builds the meta NCA (a PFS0 holding the
cnmt with each NCA's SHA-256). `source/main.c` registers the three, writes the
content meta record and pushes the application record to ns.

On Horizon 3.0 and later, libnx opens an NS getter service at initialization.
The installer opens its application-manager session before writing content.
The older installer used an uninitialized service handle on those versions,
which caused `ns record 0xe401`. [libnx NS source](https://github.com/switchbrew/libnx/blob/master/nx/source/services/ns.c)

Content ids are the first half of each NCA's *plaintext* SHA-256, computed by
`payload.py`: ncm needs the id before the console has hashed the encrypted NCA,
and a different build always gives a different id. The cnmt carries the real
hashes.

## Layout once installed

| What | Where |
|---|---|
| The game's files | the title's RomFS, `romfs:/guest/` |
| Mods | `sdmc:/switch/isaac-recomp/mods/<mod>/` (with its `metadata.xml`) |
| Saves | `sdmc:/switch/isaac-recomp/saves/`, as with the NRO |
| settings.ini, logs | `sdmc:/switch/isaac-recomp/` |

`native_switch.c` tells the two apart with `envIsNso()`. The NRO keeps reading
`guest/` and `guest/mods/` from the SD card.

Title id `0500B15AAC000000`, version 0. Installing again replaces the earlier
install, including its contents. To uninstall, use System Settings → Data
Management, as with any title.

## Requirements and limits

- **Sigpatches.** The NCAs carry no Nintendo signatures (hactool reports
  `Fixed-Key Signature (FAIL)` and `NPDM Signature (FAIL)`, by design). Atmosphère
  runs them only with sigpatches, as it does any homebrew NSP.
- The NPDM is nx-hbloader's `hbl.json` (the process the NRO was tested in) with
  this title's id and `application_type` 1.
- No logo section in the program NCA (hacBrewPack's `--nologo` case).
- Free space: the payload and the install both live on the SD card (about
  2 × 1.1 GB during the install). **Both** also includes the direct-launch
  files, about 1.1 GB more. X deletes the installer payload afterwards.

## Checks

`tools/test_build_all.py` checks the contents of the SD card folder for all
three output formats. `build.py` builds the installer NRO with
`-Wall -Wextra -Werror`. The PC driver under `test/` can finish the NCAs with
OpenSSL and a synthetic key so their contents can be inspected on a host.

The updated installer and both output formats build on the host. Successful
HOME Menu registration and launch still need to be confirmed on a Switch.
