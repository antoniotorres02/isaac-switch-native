# Everything the build needs, at the versions it was measured with, and nothing
# from the game: the executable and the game folder are mounted when it runs.
#
# Build it from the repository root, so that .dockerignore applies:
#
#     docker build -t isaac-switch-builder -f docker/Dockerfile .
#
# See GUIDE.md. docker/smoke.sh checks a built image without the game.
#
# The image used to start from the whole devkitA64 image and add a JDK, Ghidra
# and Emscripten: 7.35 GB, almost all of it never touched by the build. Now the
# pinned devkitA64 image is only a quarry: the first stage copies out the
# compiler and the few Switch libraries the NRO links, and the last stage puts
# them on a slim Python, which is the only other thing the build runs.

# The image build_native.py pins as SDK_IMAGE, so the compiler and libraries
# are byte for byte the ones every measured NRO was built with.
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282 AS dkp

# devkitA64 without its debugger, its LTO dumper, the Objective-C front end and
# the manuals. lto1 and cc1plus stay: LTO builds and the C++ in libnx need them.
# libnx whole. Of portlibs, the headers, the licences and only the libraries
# build_native.py links (plus their pkg-config and CMake files, which are tiny).
# tools/bin has nacptool, elf2nro, elf2nso and npdmtool, which make the NRO and
# the installer's NCA; the rest of it is a few hundred KB and comes along.
RUN set -eu; \
    src=/opt/devkitpro; dst=/out/opt/devkitpro; \
    mkdir -p "$dst/portlibs/switch/lib"; \
    cp -a "$src/devkitA64" "$src/libnx" "$src/tools" "$src/licenses" "$src/cmake" "$dst/"; \
    cp -a "$src"/*.sh "$dst/"; \
    rm -f "$dst"/devkitA64/bin/*-lto-dump "$dst"/devkitA64/bin/*-gdb "$dst"/devkitA64/bin/*-gdb-add-index; \
    rm -f "$dst"/devkitA64/libexec/gcc/*/*/cc1obj; \
    rm -rf "$dst/devkitA64/share/info" "$dst/devkitA64/share/man" "$dst/devkitA64/share/gdb"; \
    cp -a "$src/portlibs/switch/include" "$dst/portlibs/switch/"; \
    if [ -d "$src/portlibs/switch/licenses" ]; then cp -a "$src/portlibs/switch/licenses" "$dst/portlibs/switch/"; fi; \
    cp -a "$src/portlibs/switch/lib/pkgconfig" "$src/portlibs/switch/lib/cmake" "$dst/portlibs/switch/lib/"; \
    cd "$src/portlibs/switch/lib"; \
    for lib in openal SDL2 SDL2main EGL GLESv2 glapi drm_nouveau freetype harfbuzz png16 png bz2 z; do \
        found=0; \
        for f in "lib$lib.a" "lib$lib.la"; do \
            if [ -e "$f" ] || [ -L "$f" ]; then cp -a "$f" "$dst/portlibs/switch/lib/"; found=1; fi; \
        done; \
        if [ "$found" = 0 ] && [ "$lib" != SDL2main ]; then echo "missing lib$lib" >&2; exit 1; fi; \
    done; \
    for t in nacptool elf2nro elf2nso npdmtool; do test -x "$dst/tools/bin/$t"; done

# Upstream at the commit in upstream.json, with Lua extracted by hand
# (BUILDING.md explains why). The PE hash upstream's toolkit expects is no
# longer edited here: build_all.py applies the overlay for the game version it
# is given, so one image serves every supported executable.
RUN set -eu; \
    git init -q /out/opt/isaac/upstream; \
    cd /out/opt/isaac/upstream; \
    git fetch -q --depth 1 https://github.com/xreamm1/the-browsing-of-isaac 22be498c60aad2f8abcf010cd408422385fb58a3; \
    git checkout -q FETCH_HEAD; \
    rm -rf .git; \
    mkdir -p tools; \
    curl -fsSL -o tools/lua-5.3.3.tar.gz https://www.lua.org/ftp/lua-5.3.3.tar.gz; \
    echo "5113c06884f7de453ce57702abaac1d618307f33f6789fa870e87a59d772aca2  tools/lua-5.3.3.tar.gz" | sha256sum -c -; \
    tar -xzf tools/lua-5.3.3.tar.gz -C tools; \
    chmod -R a+rX /out/opt/isaac

# Python 3.12, which pypcode 4.0.1 needs, on Debian 12 slim: the devkitA64
# binaries only ask for glibc, and bookworm's is the one they were built against
# by the image above (also Debian 12).
FROM python@sha256:e697c81897302a4c4333bade000d98038fbb21eb064f8ee1fd42d02ebcd9d337

# patch applies upstream's fixes and ours (host/patches).
RUN apt-get update \
 && apt-get install -y --no-install-recommends patch \
 && rm -rf /var/lib/apt/lists/*

# Only what the pipeline imports: pypcode and capstone lift, pefile reads the
# executable, pillow and brotli convert assets.
RUN python3 -m venv /opt/isaac/venv \
 && /opt/isaac/venv/bin/pip install --no-cache-dir -q \
      pypcode==4.0.1 capstone==5.0.9 pefile==2024.8.26 pillow==13.0.0 brotli==1.2.1

COPY --from=dkp /out/opt/devkitpro /opt/devkitpro
COPY --from=dkp /out/opt/isaac/upstream /opt/isaac/upstream

# Last, because it is what changes between builds of the image.
COPY host /opt/isaac/host

# DEVKITPRO also tells build_native.py that it is already inside the SDK image,
# so no Docker runs within Docker.
ENV DEVKITPRO=/opt/devkitpro \
    DEVKITA64=/opt/devkitpro/devkitA64 \
    PYTHONDONTWRITEBYTECODE=1 \
    PATH=/opt/isaac/venv/bin:/opt/devkitpro/tools/bin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

ENTRYPOINT ["python", "/opt/isaac/host/build_all.py"]
