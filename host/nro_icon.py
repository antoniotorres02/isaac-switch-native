"""Prepare the user's Windows application icon for the Switch NRO."""
from __future__ import annotations

import io
import struct
from pathlib import Path


def prepare_icon(executable: Path, destination: Path) -> bool:
    if not executable.is_file():
        return False
    import pefile
    from PIL import Image

    def resource(resource_id):
        return next((entry.directory.entries for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries
                     if entry.id == resource_id), [])

    def payload(entry):
        data = entry.directory.entries[0].data.struct
        return pe.get_data(data.OffsetToData, data.Size)

    with pefile.PE(str(executable), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
        if not hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
            return False
        groups, frames = resource(14), resource(3)
        if not groups or not frames:
            return False
        group = payload(groups[0])
        if len(group) < 6:
            return False
        reserved, kind, count = struct.unpack_from("<HHH", group)
        if reserved or kind != 1 or not count or len(group) != 6 + 14 * count:
            return False
        available = {entry.id: entry for entry in frames}
        entries = [struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i) for i in range(count)]
        entries.sort(key=lambda entry: ((entry[0] or 256) * (entry[1] or 256), entry[5]), reverse=True)
        for w, h, colors, reserved, planes, bpp, size, frame_id in entries:
            if frame_id not in available:
                continue
            data = payload(available[frame_id])
            if len(data) != size:
                continue
            icon = struct.pack("<HHHBBBBHHII", 0, 1, 1, w, h, colors, reserved, planes, bpp, size, 22) + data
            try:
                with Image.open(io.BytesIO(icon)) as source:
                    image = source.convert("RGBA")
            except (OSError, ValueError):
                continue
            image.thumbnail((256, 256), Image.Resampling.LANCZOS)
            background = Image.new("RGBA", (256, 256), (30, 26, 34, 255))
            background.alpha_composite(image, ((256 - image.width) // 2, (256 - image.height) // 2))
            destination.parent.mkdir(parents=True, exist_ok=True)
            background.convert("RGB").save(destination, format="JPEG", quality=90, optimize=True)
            return True
    return False
