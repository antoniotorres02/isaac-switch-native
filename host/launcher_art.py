"""Prepare launcher button graphics and private artwork for local builds."""
from __future__ import annotations

from io import BytesIO
import os
import runpy
import shutil
import sys
from pathlib import Path


def find_art(project: Path) -> dict[str, Path]:
    user = Path(os.environ.get("ISAAC_LAUNCHER_ART") or project / "launcher-art").expanduser()
    if sys.platform == "win32":
        steam_dirs = [Path(os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)")) / "Steam"]
    else:
        steam_dirs = [Path.home() / p for p in (
            ".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam",
            "Library/Application Support/Steam")]
    cache = [root / "appcache" / "librarycache" / "250900" for root in steam_dirs]
    locations = {"launcher-art": user}
    cached_art = next((location for location in cache if location.is_dir()), None)
    if cached_art:
        locations["launcher-steam-art"] = cached_art
    return {mount: path.resolve() for mount, path in locations.items() if path.is_dir()}


def build_art_sources(project: Path) -> dict[str, Path]:
    mounted = {name: Path("/") / name for name in ("launcher-art", "launcher-steam-art")
               if (Path("/") / name).is_dir()}
    return mounted or find_art(project)


def art_file(sources: dict[str, Path], name: str) -> Path | None:
    for source in sources.values():
        direct = source / name
        if direct.is_file():
            return direct
        nested = next((path for path in sorted(source.glob("*/" + name)) if path.is_file()), None)
        if nested:
            return nested
    return None


def game_logo(upstream: Path | None) -> bytes | None:
    if upstream is None:
        return None
    extractor = upstream / "scripts/recomp/assets/archive.py"
    packed = Path("/game/resources/packed/afterbirthp.a")
    if not packed.is_file():
        packed = upstream / ".scratch/game-bundle/resources/packed/afterbirthp.a"
    if not extractor.is_file() or not packed.is_file():
        return None
    archive_class = runpy.run_path(str(extractor))["Archive"]
    with archive_class(str(packed)) as archive:
        entry = archive.find("resources/gfx/ui/main menu/logo.png")
        return archive.decode(entry) if entry else None


def prepare_button_icons(out: Path) -> Path:
    icons = Path(__file__).with_name("launcher") / "icons"
    keys = (
        ("A", "a"), ("B", "b"), ("X", "x"), ("Y", "y"), ("L", "l"), ("R", "r"),
        ("ZL", "zl"), ("ZR", "zr"), ("+", "plus"), ("−", "minus"), ("D-pad", "dpad"),
        ("↑", "up"), ("↓", "down"), ("<", "left"), (">", "right"),
    )
    lines = ["/* Generated from the controller PNGs by launcher_art.py. */"]
    for _, name in keys:
        data = (icons / f"{name}.png").read_bytes()
        lines.append(f"static const unsigned char icon_{name}[] = {{")
        for pos in range(0, len(data), 12):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[pos:pos + 12]) + ",")
        lines.append("};")
    lines.append("static launcher_button g_buttons[] = {")
    for label, name in keys:
        lines.append(f'    {{"{label}", icon_{name}, sizeof icon_{name}, {{0}}}},')
    lines.append("};")
    header = out / "native_launcher_icons.inc"
    contents = "\n".join(lines) + "\n"
    out.mkdir(parents=True, exist_ok=True)
    if not header.is_file() or header.read_text(encoding="utf-8") != contents:
        header.write_text(contents, encoding="utf-8")
    return header


def prepare_art(out: Path, sources: dict[str, Path], upstream: Path | None = None) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for suffix in (".ttf", ".woff"):
        font = art_file(sources, "MotivaSans-Regular" + suffix)
        try:
            if font and font.stat().st_size <= 2 * 1024 * 1024:
                shutil.copy2(font, out / font.name)
        except OSError as error:
            print(f"Optional launcher font could not be loaded ({error})", file=sys.stderr)
    try:
        from PIL import Image, ImageOps
        hero = art_file(sources, "library_hero.jpg")
        if hero:
            with Image.open(hero) as image:
                ImageOps.fit(image.convert("RGB"), (1280, 328), Image.Resampling.LANCZOS).save(out / "hero.png")
        local_logo = art_file(sources, "logo.png")
        extracted = game_logo(upstream)
        logo = BytesIO(extracted) if extracted else local_logo
        if logo:
            with Image.open(logo) as image:
                rgba = image.convert("RGBA")
                # The title animation uses the first 480×160 frame of the logo sheet.
                if extracted:
                    rgba = rgba.crop((0, 0, 480, 160))
                rgba = rgba.crop(rgba.getbbox())
                scale = min(680 / rgba.width, 244 / rgba.height)
                resample = Image.Resampling.NEAREST if extracted else Image.Resampling.LANCZOS
                rgba = rgba.resize((max(1, round(rgba.width * scale)), max(1, round(rgba.height * scale))),
                                   resample)
                rgba.save(out / "logo.png")
    except (ImportError, OSError, ValueError) as error:
        print(f"Optional launcher art could not be loaded ({error})", file=sys.stderr)
