# Game versions

One folder per version of Repentance+ the builder knows. `versions.py` reads
them. The build (`build_all.py`) and the builder program (`builder/isb/checks.py`)
recognise an executable by its SHA-256 here. When the hash is unknown they say so
and name the version they support. They never fail halfway through a build.

| File | What it is |
| --- | --- |
| `version.json` | Steam build id, Steam's `isaac-ng.exe` (still with DRM), the unpacked executable the build reads (`pe.sha256`, `pe.size`), `supported`, and for a supported version `upstream.overlay` and `functions` |
| `upstream.patch` | changes to upstream's scripts at the pinned commit (`upstream.json`) that this version needs: its addresses, the shims that changed, the patches turned off. `build_all.py` applies it in the *prepare* stage with `patch -p1` |
| `functions.txt` | where each function starts and ends, as Ghidra found it (see `functions_data.py`). Only addresses, about 170 KB. It replaces a 25-minute Ghidra run in every build |

Changes to upstream that every version needs live outside this folder, in
`../upstream-patches/`. They are applied before the overlay and must not touch
the same files. Today there is one: it keeps a lift worker's memory bounded.

A folder with `"supported": false` has only `version.json`. Its job is to
recognise an older executable and say "update the game in Steam".

Only one version can be supported at a time. The addresses in `native/`,
`patches/` and `build_native.py` (`LIFT_HOOKS`) belong to it. To support two
versions at once, those addresses would have to move into the version folder first.

## When Steam updates the game

These steps are for a maintainer. Users only need the new builder.

1. Unpack the new executable (`tools/prepare_current_pe.py --version ...`) and
   record its hashes. Steam's build id is in `appmanifest_250900.acf`.
2. Run Ghidra once on a development checkout of upstream (BUILDING.md, "Ghidra
   export"). That is the only place Ghidra is still needed.
3. Move the addresses from the old version to the new one with the tools in
   `../rebase/` (`map_versions.py`, then `rebase_sources.py` on upstream's
   scripts, on `native/`, `patches/` and `LIFT_HOOKS`). Then check what they
   could not place by hand (`locate.py`, `fn_by_refs.py`).
4. Create `versions/<new>/`:
   ```sh
   git -C <dev checkout> diff 22be498 -- . ':!scripts/recomp/host/generated' > versions/<new>/upstream.patch
   python functions_data.py make <dev checkout>/output/recomp/export versions/<new>
   ```
   Fill in `version.json` (copy the one before). Set the old version to
   `"supported": false` and delete its `upstream.patch` and `functions.txt`.
5. Run `tools/test_build_all.py` and `tools/test_builder.py`, do a full build in
   the image, and play-test on PC and on the console. Then publish the image and
   the program (`builder/README.md`).
