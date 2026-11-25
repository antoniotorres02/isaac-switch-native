"""Every text the builder shows, in Spanish and English.

The language follows the system's; --lang es|en (or ISAAC_BUILDER_LANG)
chooses it by hand. t(key, **values) gives the text in the current language."""
from __future__ import annotations

import locale
import os
import sys

T: dict[str, tuple[str, str]] = {
    # ---- the DRM notice: shown before anything else, on every path ----------------
    "drm.title": ("Antes de empezar: el ejecutable sin DRM", "Before you start: the executable without DRM"),
    "drm.body": (
        "Este programa necesita el ejecutable del juego (isaac-ng.exe) con el DRM de Steam quitado. "
        "Ese paso NO lo hace este programa, ni explica cómo hacerlo. Si lo haces y cómo es decisión "
        "y responsabilidad tuya, según la ley que te aplique: compruébala primero.\n\n"
        "El fichero tiene que ser exactamente este (SHA-256):\n{sha}",
        "This program needs the game's executable (isaac-ng.exe) with Steam's DRM removed. "
        "This program does NOT do that step, nor explain how to do it. Whether and how you do it is your "
        "decision and your responsibility under the law that applies to you: check it first.\n\n"
        "The file has to be exactly this one (SHA-256):\n{sha}"),
    "drm.short": ("Hace falta el ejecutable SIN el DRM de Steam. Este programa no lo quita.",
                  "You need the executable WITHOUT Steam's DRM. This program does not remove it."),

    # ---- the game folder ------------------------------------------------------------
    "game.not_found": ("No se ha encontrado el juego en Steam. Elige su carpeta a mano (la que contiene "
                       "resources/ y savedatapath.txt).",
                       "The game was not found in Steam. Choose its folder by hand (the one containing "
                       "resources/ and savedatapath.txt)."),
    "game.no_folder": ("La carpeta {path} no existe.", "The folder {path} does not exist."),
    "game.not_game": ("{path} no parece la carpeta del juego: elige la que contiene resources/ y savedatapath.txt.",
                      "{path} does not look like the game's folder: choose the one containing resources/ and "
                      "savedatapath.txt."),
    "game.found": ("Juego encontrado en {path}", "Game found in {path}"),
    "game.incomplete": ("Faltan ficheros del juego: {files}. En Steam: clic derecho en el juego → Propiedades → "
                        "Archivos instalados → Verificar integridad.",
                        "Game files are missing: {files}. In Steam: right-click the game → Properties → "
                        "Installed files → Verify integrity."),
    "game.dlc_missing": ("Faltan expansiones: {dlcs}. Hacen falta todas, hasta Repentance+.",
                         "Expansions missing: {dlcs}. All of them are needed, up to Repentance+."),
    "game.dlc_ok": ("Expansiones instaladas: Afterbirth, Afterbirth+, Repentance y Repentance+",
                    "Expansions installed: Afterbirth, Afterbirth+, Repentance and Repentance+"),
    "game.build_differs": ("Steam dice que tienes la build {got}; este programa sabe construir la {want} "
                           "(v{version}). Si Steam ha actualizado el juego, hace falta una versión nueva del "
                           "programa.",
                           "Steam says you have build {got}; this program can build {want} (v{version}). "
                           "If Steam has updated the game, a newer version of this program is needed."),
    "game.version_ok": ("Versión correcta: v{version}", "Right version: v{version}"),
    "game.version_differs": ("El isaac-ng.exe de tu carpeta no es el de la v{version}: los ficheros del juego "
                             "pueden no coincidir.",
                             "The isaac-ng.exe in your folder is not v{version}'s: the game's files may not match."),

    # ---- the executable --------------------------------------------------------------
    "exe.ok": ("Ejecutable correcto: {path}", "Right executable: {path}"),
    "exe.original": ("Este es el isaac-ng.exe original de Steam, todavía con DRM. Es la versión correcta, "
                     "pero el build necesita el ejecutable sin DRM (lee el aviso de arriba).",
                     "This is Steam's original isaac-ng.exe, still with DRM. It is the right version, "
                     "but the build needs the executable without DRM (read the notice above)."),
    "exe.retired": ("Este ejecutable es de la v{got}, que ya no se puede construir. Actualiza el juego en Steam "
                    "(v{version}) y usa el ejecutable nuevo.",
                    "This executable is from v{got}, which can no longer be built. Update the game in Steam "
                    "(v{version}) and use the new executable."),
    "exe.other": ("Este fichero no es el que hace falta.\nTiene SHA-256 {got}\ny hace falta {want} (v{version}).\n"
                  "Otra versión del juego, o un fichero que difiera en un solo byte, no sirve.",
                  "This is not the file the build needs.\nIt has SHA-256 {got}\nand it must be {want} (v{version}).\n"
                  "Another game version, or a file that differs in a single byte, cannot work."),
    "exe.folder": ("{path} es una carpeta; elige el fichero .exe.", "{path} is a folder; choose the .exe file."),
    "exe.missing": ("Elige el ejecutable sin DRM.", "Choose the executable without DRM."),
    "exe.searching": ("Buscando en Descargas, Escritorio, Documentos y la carpeta del juego…",
                      "Looking in Downloads, Desktop, Documents and the game's folder…"),
    "exe.not_found": ("No lo he encontrado. Elígelo con «Elegir…».", "Not found. Choose it with “Choose…”."),
    "exe.suggested": ("Posibles ficheros encontrados: {n}. Despliega la lista de arriba para elegir uno.",
                      "Possible files found: {n}. Open the list above to pick one."),
    "game.others": ("Hay {n} instalaciones del juego: puedes elegir otra en la lista.",
                    "There are {n} installs of the game: you can pick another from the list."),
    "exe.hashing": ("Comprobando el fichero…", "Checking the file…"),

    # ---- Docker --------------------------------------------------------------------------
    "docker.missing": ("Docker no está instalado. Es lo que trae todas las herramientas del build. Instálalo "
                       "(gratis, «Cómo instalar Docker» abre la guía de tu distribución) y esta página lo "
                       "detectará sola.",
                       "Docker is not installed. It brings every tool the build uses. Install it (free; “How to "
                       "install Docker” opens the guide for your distribution) and this page will notice."),
    "docker.missing_win": ("Docker Desktop no está instalado. Es gratis para uso personal y trae todas las "
                           "herramientas del build. Pulsa «Instalar Docker Desktop»: Windows pedirá permiso de "
                           "administrador y puede pedir reiniciar al terminar. Si prefieres hacerlo a mano, "
                           "«Página de descarga».",
                           "Docker Desktop is not installed. It is free for personal use and brings every tool the "
                           "build uses. Press “Install Docker Desktop”: Windows will ask for administrator "
                           "permission and may ask to restart afterwards. To do it by hand, “Download page”."),
    "docker.no_permission": ("Docker está instalado pero tu usuario no puede usarlo. En una terminal:\n"
                             "    sudo usermod -aG docker $USER\ny cierra sesión y vuelve a entrar.",
                             "Docker is installed but your user cannot use it. In a terminal:\n"
                             "    sudo usermod -aG docker $USER\nthen log out and back in."),
    "docker.stopped": ("Docker no está en marcha. Arráncalo en una terminal con\n"
                       "    sudo systemctl start docker\n(o abre Docker Desktop). Esta página lo detecta sola.",
                       "Docker is not running. Start it in a terminal with\n"
                       "    sudo systemctl start docker\n(or open Docker Desktop). This page notices by itself."),
    "docker.stopped_win": ("Docker Desktop no está abierto. Pulsa «Abrir Docker Desktop» y espera a que diga "
                           "«Engine running» (la primera vez te pedirá aceptar sus condiciones; no hace falta "
                           "crear cuenta). Esta página lo detecta sola.",
                           "Docker Desktop is not open. Press “Open Docker Desktop” and wait until it says "
                           "“Engine running” (the first time it asks you to accept its terms; no account is "
                           "needed). This page notices by itself."),
    "docker.waiting": ("Esperando a Docker: se comprueba solo cada 5 segundos.",
                       "Waiting for Docker: checked again every 5 seconds."),
    "docker.installing": ("Instalando Docker Desktop (unos minutos; acepta el aviso de administrador de Windows)…",
                          "Installing Docker Desktop (a few minutes; accept Windows' administrator prompt)…"),
    "docker.installed": ("Docker Desktop está instalado. Si Windows pide reiniciar, reinicia y vuelve a abrir este "
                         "programa; si no, pulsa «Abrir Docker Desktop».",
                         "Docker Desktop is installed. If Windows asks to restart, restart and open this program "
                         "again; otherwise press “Open Docker Desktop”."),
    "docker.install_failed": ("La instalación no terminó (código {rc}). Prueba con «Página de descarga».",
                              "The installation did not finish (code {rc}). Try “Download page”."),
    "docker.windows_containers": ("Docker Desktop está en modo «contenedores Windows». Clic derecho en su icono "
                                  "→ «Switch to Linux containers…».",
                                  "Docker Desktop is in “Windows containers” mode. Right-click its icon → "
                                  "“Switch to Linux containers…”."),
    "docker.ok": ("{engine} funciona", "{engine} works"),
    "docker.memory_ok": ("Memoria para Docker: {gib} GiB", "Memory for Docker: {gib} GiB"),
    "docker.memory_low": ("Docker sólo tiene {gib} GiB de memoria y el build necesita {need} GiB.",
                          "Docker only has {gib} GiB of memory and the build needs {need} GiB."),
    "docker.memory_low_win": ("Para dar más memoria a Docker: pulsa «Abrir .wslconfig», deja la línea "
                              "memory=10GB (o más, sin pasar de tu RAM), guarda, y en una terminal ejecuta "
                              "«wsl --shutdown». Después reinicia Docker Desktop.",
                              "To give Docker more memory: press “Open .wslconfig”, keep the line memory=10GB "
                              "(or more, below your RAM), save, and run “wsl --shutdown” in a terminal. Then "
                              "restart Docker Desktop."),
    "docker.podman": ("Usando podman: funciona igual en teoría, pero sólo se ha probado con Docker.",
                      "Using podman: it should work the same, but only Docker has been tested."),
    "image.present": ("Imagen de herramientas lista ({ref})", "Tool image ready ({ref})"),
    "image.absent": ("Falta la imagen de herramientas (~7,4 GB, una sola vez).",
                     "The tool image is missing (~7.4 GB, once)."),
    "image.unpublished": ("La imagen todavía no está publicada. Se puede construir aquí mismo desde el "
                          "repositorio (unos minutos y varios GB de descarga).",
                          "The image is not published yet. It can be built right here from the repository "
                          "(a few minutes and several GB of downloads)."),
    "image.unavailable": ("La imagen no está publicada y este programa no está dentro del repositorio para "
                          "construirla.",
                          "The image is not published and this program is not inside the repository to "
                          "build it."),
    "image.pulling": ("Descargando la imagen…", "Downloading the image…"),
    "image.building": ("Construyendo la imagen…", "Building the image…"),
    "image.failed": ("No se pudo preparar la imagen. Revisa tu conexión y vuelve a intentarlo.",
                     "The image could not be prepared. Check your connection and try again."),

    # ---- disk -------------------------------------------------------------------------
    "disk.ok": ("Espacio libre en {path}: {free} GiB", "Free space in {path}: {free} GiB"),
    "disk.low": ("Poco espacio en {path}: {free} GiB libres y hacen falta unos {need} GiB.",
                 "Not enough space in {path}: {free} GiB free and about {need} GiB needed."),

    # ---- stages (build_all.py's names) ---------------------------------------------------
    "stage.prepare": ("Preparar las fuentes", "Prepare the sources"),
    "stage.functions": ("Listar las funciones del juego", "List the game's functions"),
    "stage.tables": ("Tablas e imagen de memoria", "Tables and memory image"),
    "stage.starts": ("Buscar las funciones", "Find the functions"),
    "stage.lift": ("Traducir el juego a C (la larga)", "Translate the game to C (the long one)"),
    "stage.assets": ("Indexar los ficheros del juego", "Index the game's files"),
    "stage.nro": ("Compilar el NRO", "Compile the NRO"),
    "stage.nsp": ("Preparar la instalación (NSP)", "Prepare the install (NSP)"),
    "stage.sdcard": ("Preparar la carpeta de la SD", "Prepare the SD card folder"),
    "stage.export": ("Copiar el resultado a tu carpeta", "Copy the result to your folder"),

    # ---- the window --------------------------------------------------------------------
    "app.title": ("Isaac Switch Builder", "Isaac Switch Builder"),
    "nav.back": ("← Atrás", "← Back"),
    "nav.next": ("Siguiente →", "Next →"),
    "nav.report": ("Copiar informe", "Copy report"),
    "nav.report_done": ("Informe copiado: pégalo donde pidas ayuda.", "Report copied: paste it where you ask for help."),
    "nav.quit_running": ("El build sigue en marcha. ¿Pararlo y salir? Podrás continuar luego desde donde iba.",
                         "The build is still running. Stop it and quit? You can carry on later from where it was."),
    "btn.choose": ("Elegir…", "Choose…"),
    "btn.search": ("Buscar automáticamente", "Search automatically"),
    "btn.check_again": ("Comprobar de nuevo", "Check again"),
    "btn.install_docker": ("Instalar Docker Desktop", "Install Docker Desktop"),
    "btn.docker_page": ("Página de descarga" if sys.platform == "win32" else "Cómo instalar Docker",
                        "Download page" if sys.platform == "win32" else "How to install Docker"),
    "btn.start_docker": ("Abrir Docker Desktop", "Open Docker Desktop"),
    "btn.copy_cmd": ("Copiar el comando", "Copy the command"),
    "copied": ("Copiado. Pégalo en una terminal.", "Copied. Paste it in a terminal."),
    "btn.wslconfig": ("Abrir .wslconfig", "Open .wslconfig"),
    "btn.verify_steam": ("Abrir Steam", "Open Steam"),
    "btn.get_image": ("Descargar la imagen", "Download the image"),
    "btn.build_image": ("Construir la imagen", "Build the image"),
    "btn.check_all": ("Comprobar todo", "Check everything"),
    "btn.start": ("Empezar el build", "Start the build"),
    "btn.cancel": ("Parar", "Stop"),
    "btn.resume": ("Continuar donde iba", "Carry on from where it was"),
    "btn.open_folder": ("Abrir la carpeta", "Open the folder"),
    "btn.copy_sd": ("Copiar a la SD", "Copy to the SD card"),
    "btn.refresh": ("Actualizar", "Refresh"),
    "btn.clean": ("Borrar los ficheros de trabajo", "Delete the work files"),

    "page.game": ("1. El juego", "1. The game"),
    "page.game.intro": ("Busco tu copia de The Binding of Isaac: Rebirth (con todas las expansiones hasta "
                        "Repentance+) instalada con Steam.",
                        "Looking for your copy of The Binding of Isaac: Rebirth (with every expansion up to "
                        "Repentance+) installed through Steam."),
    "page.game.folder": ("Carpeta del juego:", "Game folder:"),
    "page.exe": ("2. El ejecutable sin DRM", "2. The executable without DRM"),
    "page.exe.intro": ("Elige el fichero .exe sin DRM. Puede llamarse como quieras; se comprueba por su "
                       "contenido.",
                       "Choose the .exe without DRM. Its name does not matter; it is checked by its content."),
    "page.docker": ("3. Docker", "3. Docker"),
    "page.docker.intro": ("Docker trae todas las herramientas del build (Ghidra, compiladores…) a la versión "
                          "exacta con la que se probó. No se instala nada más en tu ordenador, y el build "
                          "corre sin conexión: nada sale de tu equipo.",
                          "Docker brings every tool the build uses (Ghidra, compilers…) at the exact version it "
                          "was tested with. Nothing else is installed on your computer, and the build runs "
                          "offline: nothing leaves your machine."),
    "page.options": ("4. Preparado", "4. Ready"),
    "page.options.intro": ("Todo listo. El build tarda unos 45 minutos (25 de ellos en Ghidra) y usa unos 8 GiB "
                           "de memoria. Puedes dejarlo trabajando: el ordenador no se suspenderá mientras tanto.",
                           "All set. The build takes about 45 minutes (25 of them in Ghidra) and uses about "
                           "8 GiB of memory. You can leave it working: the computer will not sleep meanwhile."),
    "page.options.folder": ("Carpeta del resultado:", "Result folder:"),
    "page.options.advanced": ("Opciones avanzadas", "Advanced options"),
    "page.options.steam": ("Incluir Steam Cloud (sincronizar partidas desde la consola)",
                           "Include Steam Cloud (sync saves from the console)"),
    "page.options.steam_warning": ("La consola se conecta a Steam como un cliente NO oficial, con un token que "
                                   "queda en texto plano en la SD. Lee STEAM-CLOUD.md antes de activarlo.",
                                   "The console signs in to Steam as an UNOFFICIAL client, with a token kept in "
                                   "plain text on the SD card. Read STEAM-CLOUD.md before turning it on."),
    "page.options.threads": ("Hilos de Ghidra (vacío = automático):", "Ghidra threads (empty = automatic):"),
    "page.options.jobs": ("Trabajos de compilación (vacío = automático):", "Compile jobs (empty = automatic):"),
    "page.options.volume": ("Los ficheros de trabajo (~4 GB) se guardan en un volumen de Docker; el resultado se "
                            "copia a esta carpeta.",
                            "The work files (~4 GB) live in a Docker volume; the result is copied to this "
                            "folder."),
    "page.build": ("5. Construyendo", "5. Building"),
    "page.build.eta": ("Tiempo: {elapsed} · quedan unos {left}", "Time: {elapsed} · about {left} left"),
    "page.build.elapsed": ("Tiempo: {elapsed}", "Time: {elapsed}"),
    "page.build.starting": ("Arrancando el contenedor y comprobando tus ficheros…",
                            "Starting the container and checking your files…"),
    "page.build.stopped": ("Parado. Cuando quieras, «Continuar donde iba» sigue desde la fase en que estaba.",
                           "Stopped. Whenever you want, “Carry on from where it was” resumes at the stage it was on."),
    "page.build.failed": ("El build se ha parado: {message}\nPuedes reintentar: sigue desde esa fase. Si vuelve a "
                          "fallar, «Copiar informe» y pide ayuda con él.",
                          "The build stopped: {message}\nYou can retry: it carries on from that stage. If it fails "
                          "again, “Copy report” and ask for help with it."),
    "page.build.problems": ("El contenedor no puede empezar:", "The container cannot start:"),
    "page.result": ("6. ¡Hecho!", "6. Done!"),
    "page.result.intro": ("El instalador y el juego están en:", "The installer and the game are in:"),
    "page.result.hash": ("SHA-256 del NRO del juego: {sha}", "Game NRO SHA-256: {sha}"),
    "page.result.how": (
        "Copia el CONTENIDO de esa carpeta a la raíz de la SD (con un lector de tarjetas, o con el botón de abajo). "
        "Tus partidas no se tocan.\n\nEn la consola, abre el menú homebrew, lanza isaac-recomp-installer y pulsa A: "
        "instala ISAAC REPENTANCE+ NS1 RECOMP en el menú HOME (hace falta Atmosphère con sigpatches). Al terminar "
        "puedes borrar los ficheros de instalación con X. Los mods van en sdmc:/switch/isaac-recomp/mods/. "
        "PLAYING.md explica los controles y cómo pasar tus partidas.",
        "Copy the CONTENTS of that folder to the root of the SD card (with a card reader, or with the button below). "
        "Your saves are not touched.\n\nOn the console, open the homebrew menu, start isaac-recomp-installer and "
        "press A: it installs ISAAC REPENTANCE+ NS1 RECOMP on the HOME menu (Atmosphère with sigpatches is needed). "
        "When it is done, X deletes the install files. Mods go in sdmc:/switch/isaac-recomp/mods/. PLAYING.md covers "
        "the controls and moving your saves."),
    "page.result.share": ("No compartas el juego instalado ni esa carpeta: llevan la imagen del ejecutable y los ficheros del "
                          "juego. Cada persona tiene que construir el suyo.",
                          "Do not share the installed game or that folder: they hold the executable's image and the game's "
                          "files. Everyone has to build their own."),
    "page.result.sd": ("Tarjeta SD:", "SD card:"),
    "page.result.no_sd": ("No veo ninguna tarjeta. Métela y pulsa «Actualizar», o elige la carpeta a mano.",
                          "No card found. Insert it and press “Refresh”, or choose the folder by hand."),
    "page.result.not_switch": ("{path} no parece la SD de una Switch con homebrew (no tiene atmosphere/ ni "
                               "switch/). ¿Copiar igualmente?",
                               "{path} does not look like the SD card of a Switch with homebrew (no atmosphere/ "
                               "or switch/). Copy anyway?"),
    "page.result.copying": ("Copiando {name}…", "Copying {name}…"),
    "page.result.copied": ("Copiado: {copied} ficheros nuevos, {checked} comprobados. Ya puedes sacar la "
                           "tarjeta (expúlsala antes).",
                           "Copied: {copied} new files, {checked} checked. You can take the card out now "
                           "(eject it first)."),
    "page.result.copy_failed": ("No se pudo copiar: {error}", "Could not copy: {error}"),

    "check.title": ("Comprobación", "Check"),
    "check.all_ok": ("Todo correcto: puedes empezar.", "Everything is fine: you can start."),
    "check.some_fail": ("Hay que arreglar lo marcado con ✘ antes de empezar.",
                        "Fix what is marked ✘ before starting."),

    # ---- command line -------------------------------------------------------------------
    "cli.checking": ("Comprobando…", "Checking…"),
    "cli.cannot_start": ("No se puede empezar. Arregla lo marcado con ✘.", "Cannot start. Fix what is marked ✘."),
    "cli.need_exe": ("Indica el ejecutable sin DRM con --exe RUTA.", "Give the executable without DRM with --exe PATH."),
    "cli.done": ("Hecho en {time}. Copia el contenido de {path} a la raíz de la SD.",
                 "Done in {time}. Copy the contents of {path} to the root of the SD card."),
    "cli.failed": ("El build se ha parado: {message}. Ejecuta el mismo comando para seguir desde esa fase.",
                   "The build stopped: {message}. Run the same command again to carry on from that stage."),
    "clean.confirm": ("Esto borra los ficheros de trabajo ({what}); el próximo build empezará desde cero. "
                      "¿Seguir?",
                      "This deletes the work files ({what}); the next build starts from scratch. Continue?"),
    "yes_no": (" [s/N] ", " [y/N] "),
    "cli.cleaned": ("Ficheros de trabajo borrados.", "Work files deleted."),
    "cli.stopped": ("Parado. Ejecuta el mismo comando para seguir desde la fase en que estaba.",
                    "Stopped. Run the same command again to carry on from the stage it was on."),
    "cli.clean_refused": ("No se ha borrado: {path} contiene otras cosas.", "Not deleted: {path} holds other things."),
    "cli.no_sd_source": ("Todavía no hay resultado en {path}: ejecuta antes «build».",
                         "There is no result in {path} yet: run “build” first."),
    "time.min": ("{m} min", "{m} min"),
    "time.sec": ("{s} s", "{s} s"),
}

_lang = None


def detect() -> str:
    forced = os.environ.get("ISAAC_BUILDER_LANG")
    if forced in ("es", "en"):
        return forced
    candidates = [os.environ.get(v, "") for v in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE")]
    if sys.platform == "win32":
        try:
            import ctypes
            lcid = ctypes.windll.kernel32.GetUserDefaultUILanguage()
            if lcid & 0x3FF == 0x0A:          # LANG_SPANISH
                return "es"
            return "en"
        except Exception:
            pass
    try:
        candidates.append(locale.getlocale()[0] or "")
    except ValueError:
        pass
    for c in candidates:
        if c:
            return "es" if c.lower().startswith(("es", "spanish")) else "en"
    return "en"


def set_lang(lang: str | None) -> None:
    global _lang
    _lang = lang if lang in ("es", "en") else detect()


def lang() -> str:
    if _lang is None:
        set_lang(None)
    return _lang


def t(key: str, **values) -> str:
    pair = T.get(key)
    if pair is None:
        return key
    text = pair[0] if lang() == "es" else pair[1]
    return text.format(**values) if values else text


def duration(seconds: float) -> str:
    seconds = max(0, int(seconds))
    if seconds >= 60:
        return t("time.min", m=(seconds + 30) // 60)
    return t("time.sec", s=seconds)
