# Everything the build needs, at the versions it was measured with, and nothing
# from the game: the executable and the game folder are mounted when it runs.
#
# Build it from the repository root, so that .dockerignore applies:
#
#     docker build -t isaac-switch-builder -f docker/Dockerfile .
#
# See GUIDE.md.

FROM eclipse-temurin@sha256:ebfbb18d11ff163ee83b21258acf12248af48d22a0fdfdc68fa1a1516c05976a AS jdk

# pypcode 4.0.1 needs Python 3.12; Debian 12, under the devkitA64 image, has 3.11.
FROM python@sha256:e697c81897302a4c4333bade000d98038fbb21eb064f8ee1fd42d02ebcd9d337 AS python

# The image build_native.py pins as SDK_IMAGE. Its DEVKITPRO variable also tells
# build_native.py that it is already inside, so no Docker runs within Docker.
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

COPY --from=jdk /opt/java/openjdk /opt/java/openjdk
COPY --from=python /usr/local /usr/local

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libffi8 libsqlite3-0 libssl3 liblzma5 libbz2-1.0 zlib1g libreadline8 libexpat1 libuuid1 \
 && rm -rf /var/lib/apt/lists/* \
 && ldconfig \
 && /usr/local/bin/python3 -c "import ssl, ctypes, sqlite3, lzma, bz2, zlib, venv, ensurepip"

# Ghidra, checked against the hash in upstream.json.
RUN curl -fsSL -o /tmp/ghidra.zip \
      https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.2_build/ghidra_12.1.2_PUBLIC_20260605.zip \
 && echo "b62e81a0390618466c019c60d8c2f796ced2509c4c1aea4a37644a77272cf99d  /tmp/ghidra.zip" | sha256sum -c - \
 && unzip -q /tmp/ghidra.zip -d /opt \
 && rm /tmp/ghidra.zip

# Emscripten: upstream's pipeline builds its web module, and its outputs are what
# the native build consumes. The node it brings runs upstream's scripts.
RUN git init -q /opt/emsdk \
 && git -C /opt/emsdk fetch -q --depth 1 https://github.com/emscripten-core/emsdk d7b4130688bfa348392b921ab04015aa15c4f154 \
 && git -C /opt/emsdk checkout -q FETCH_HEAD \
 && /opt/emsdk/emsdk install 6.0.5 \
 && /opt/emsdk/emsdk activate 6.0.5 \
 && ln -s "$(ls -d /opt/emsdk/node/*/ | head -1)" /opt/emsdk/node/current \
 && rm -rf /opt/emsdk/downloads /opt/emsdk/.git \
 && chmod -R a+rX /opt/emsdk

RUN /usr/local/bin/python3 -m venv /opt/isaac/venv \
 && /opt/isaac/venv/bin/pip install --no-cache-dir -q \
      pypcode==4.0.1 capstone==5.0.9 pefile==2024.8.26 psutil==7.2.2 brotli==1.2.1 zopfli==0.4.3 pillow==13.0.0

# Upstream at the commit in upstream.json, with its two local changes: the hash
# its PE toolkit expects, and Lua extracted by hand (BUILDING.md explains both).
RUN git init -q /opt/isaac/upstream \
 && cd /opt/isaac/upstream \
 && git fetch -q --depth 1 https://github.com/xreamm1/the-browsing-of-isaac 22be498c60aad2f8abcf010cd408422385fb58a3 \
 && git checkout -q FETCH_HEAD \
 && sed -i 's/^EXPECTED_SHA256 = "1FBA9A37BCEBF9276C9A80372E8C4DCAD59C8F652D2FD41727B396DCDCD6B89C"/EXPECTED_SHA256 = "B4743967F3D31050F7B4222E42AC6BEAE6A0B7850B1F1ADAAC07EF7B94D01EC8"/' \
      scripts/decomp/tools/pe.py \
 && grep -q '^EXPECTED_SHA256 = "B4743967' scripts/decomp/tools/pe.py \
 && mkdir -p tools \
 && curl -fsSL -o tools/lua-5.3.3.tar.gz https://www.lua.org/ftp/lua-5.3.3.tar.gz \
 && echo "5113c06884f7de453ce57702abaac1d618307f33f6789fa870e87a59d772aca2  tools/lua-5.3.3.tar.gz" | sha256sum -c - \
 && tar -xzf tools/lua-5.3.3.tar.gz -C tools \
 && chmod -R a+rX /opt/isaac

COPY host /opt/isaac/host

ENV JAVA_HOME=/opt/java/openjdk \
    GHIDRA_HOME=/opt/ghidra_12.1.2_PUBLIC \
    EMSDK=/opt/emsdk \
    EM_CONFIG=/opt/emsdk/.emscripten \
    PYTHONDONTWRITEBYTECODE=1 \
    PATH=/opt/isaac/venv/bin:/opt/java/openjdk/bin:/opt/emsdk/upstream/emscripten:/opt/emsdk/node/current/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

ENTRYPOINT ["python", "/opt/isaac/host/build_all.py"]
