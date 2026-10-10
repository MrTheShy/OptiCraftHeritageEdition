#!/usr/bin/env python3
"""Build the Vita TROPHY.TRP for OptiCraft from the 1.2.5 assets.

    make_trophies.py <assets dir> <out TROPHY.TRP> [lang]

<assets dir> is the extracted client data (achievement/, gui/, terrain.png,
title/, lang/). Icons are the ones the achievement screen draws: its frame
with the achievement's item or block texture, scaled up. Trophy N+1 is
achievement N; trophy 0 is the platinum. Needs Pillow.

The TRP is the plaintext format homebrew uses with NoTrpDrm: a 0x40 header
(magic, version 2, size, entry count, SHA-1 of the file), 0x40-byte entries,
16-byte aligned data.
"""
import hashlib
import io
import os
import re
import struct
import sys

from PIL import Image

COMM_ID = "OPTC00001_00"

# (lang key, grade, icon) in AchievementList order. Icons are
# ("items", col, row) from Item::setIconCoord or ("terrain", index).
ACHIEVEMENTS = [
    ("openInventory", "B", ("items", 11, 3)),
    ("mineWood", "B", ("terrain", 20)),
    ("buildWorkBench", "B", ("terrain", 59)),
    ("buildPickaxe", "B", ("items", 0, 6)),
    ("buildFurnace", "B", ("terrain", 61)),
    ("acquireIron", "B", ("items", 7, 1)),
    ("buildHoe", "B", ("items", 0, 8)),
    ("makeBread", "B", ("items", 9, 2)),
    ("bakeCake", "S", ("items", 13, 1)),
    ("buildBetterPickaxe", "B", ("items", 1, 6)),
    ("cookFish", "B", ("items", 10, 5)),
    ("onARail", "G", ("terrain", 128)),
    ("buildSword", "B", ("items", 0, 4)),
    ("killEnemy", "B", ("items", 12, 1)),
    ("killCow", "B", ("items", 7, 6)),
    ("flyPig", "G", ("items", 8, 6)),
    ("snipeSkeleton", "G", ("items", 5, 1)),
    ("diamonds", "S", ("items", 7, 3)),
    ("portal", "S", ("terrain", 37)),
    ("ghast", "G", ("items", 11, 7)),
    ("blazeRod", "S", ("items", 12, 6)),
    ("potion", "S", ("items", 13, 8)),
    ("theEnd", "G", ("items", 11, 9)),
    ("theEnd2", "G", ("terrain", 167)),
    ("enchantments", "S", ("terrain", 166)),
    ("overkill", "G", ("items", 3, 4)),
    ("bookcase", "B", ("terrain", 35)),
]
# Achievements drawn with the special frame (setSpecial() in AchievementList).
SPECIAL = {"onARail", "flyPig", "snipeSkeleton", "ghast", "theEnd", "theEnd2", "overkill"}
PLATINUM_ICON = ("terrain", 3)  # grass block side


def load_lang(assets, lang):
    entries = {}
    for name in ("en_US", lang):
        path = os.path.join(assets, "lang", name + ".lang")
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            for line in f:
                key, sep, value = line.rstrip("\n").partition("=")
                if sep:
                    entries[key] = value.strip()
    return entries


def tile(sheets, icon):
    if icon[0] == "items":
        x, y = icon[1] * 16, icon[2] * 16
    else:
        x, y = (icon[1] % 16) * 16, (icon[1] // 16) * 16
    return sheets[icon[0]].crop((x, y, x + 16, y + 16))


def trophy_icon(sheets, frame_bg, icon, special):
    # Same layout as GuiAchievements: 26x26 frame, icon at +5,+5, scale 9.
    frame = frame_bg.crop((26, 202, 52, 228) if special else (0, 202, 26, 228))
    frame.alpha_composite(tile(sheets, icon), (5, 5))
    out = Image.new("RGBA", (240, 240), (32, 32, 32, 255))
    out.alpha_composite(frame.resize((234, 234), Image.NEAREST), (3, 3))
    return out


def title_icon(assets):
    out = Image.new("RGBA", (320, 176), (0, 0, 0, 255))
    pano = os.path.join(assets, "title", "bg", "panorama0.png")
    if os.path.exists(pano):
        bg = Image.open(pano).convert("RGBA")
        scale = max(320 / bg.width, 176 / bg.height)
        bg = bg.resize((round(bg.width * scale), round(bg.height * scale)), Image.BILINEAR)
        out.alpha_composite(bg.crop((0, 0, 320, 176)))
    # GuiMainMenu draws the logo as two 155x44 halves of mclogo.png.
    logo = Image.open(os.path.join(assets, "title", "mclogo.png")).convert("RGBA")
    out.alpha_composite(logo.crop((0, 0, 155, 44)), (5, 66))
    out.alpha_composite(logo.crop((0, 45, 155, 89)), (160, 66))
    return out


def png_bytes(image):
    buf = io.BytesIO()
    image.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def xml_escape(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def sfm(trophies, title=None):
    head = "<!--Sce-Np-Trophy-Signature: " + "x" * 320 + "-->\n"
    lines = [head + '<trophyconf version="1.1" platform="psp2" policy="large">',
             " <npcommid>%s</npcommid>" % COMM_ID,
             " <trophyset-version>01.00</trophyset-version>",
             ' <parental-level license-area="default">0</parental-level>']
    if title:
        lines += [" <title-name>%s</title-name>" % xml_escape(title[0]),
                  " <title-detail>%s</title-detail>" % xml_escape(title[1])]
    for tid, grade, name, detail in trophies:
        pid = "-1" if grade == "P" else "000"
        attrs = 'id="%03d" hidden="no" ttype="%s" pid="%s"' % (tid, grade, pid)
        if title is None:
            lines.append(" <trophy %s/>" % attrs)
        else:
            lines += [" <trophy %s>" % attrs,
                      "  <name>%s</name>" % xml_escape(name),
                      "  <detail>%s</detail>" % xml_escape(detail),
                      " </trophy>"]
    lines.append("</trophyconf>\n")
    return "\n".join(lines).encode("utf-8")


def build_trp(files):
    header_size = 0x40 + 0x40 * len(files)
    offset = (header_size + 15) & ~15
    entries, blobs = b"", b""
    for name, data in files:
        pad = (16 - (len(blobs) % 16)) % 16
        blobs += b"\0" * pad
        entries += struct.pack(">32sQQI12x", name.encode(), offset + len(blobs), len(data), 0)
        blobs += data
    body = entries + b"\0" * (offset - header_size) + blobs
    total = 0x40 + len(body)
    header = struct.pack(">IIQIII20s16x", 0xDCA24D00, 2, total, len(files), 0x40, 0, b"\0" * 20)
    trp = bytearray(header + body)
    trp[0x1C:0x30] = hashlib.sha1(trp).digest()
    return bytes(trp)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    assets, out = sys.argv[1], sys.argv[2]
    lang = load_lang(assets, sys.argv[3] if len(sys.argv) > 3 else "en_US")
    sheets = {"items": Image.open(os.path.join(assets, "gui", "items.png")).convert("RGBA"),
              "terrain": Image.open(os.path.join(assets, "terrain.png")).convert("RGBA")}
    frame_bg = Image.open(os.path.join(assets, "achievement", "bg.png")).convert("RGBA")

    trophies = [(0, "P", "OptiCraft", "Unlocks all other trophies.")]
    icons = [trophy_icon(sheets, frame_bg, PLATINUM_ICON, True)]
    for index, (key, grade, icon) in enumerate(ACHIEVEMENTS):
        name = lang.get("achievement." + key, key)
        # The inventory one has a %1$s key placeholder; the Vita opens it with Triangle.
        detail = re.sub(r"'?%1\$s?'?", "Triangle", lang.get("achievement." + key + ".desc", ""))
        trophies.append((index + 1, grade, name, detail))
        icons.append(trophy_icon(sheets, frame_bg, icon, key in SPECIAL))

    files = [("TROPCONF.SFM", sfm(trophies)),
             ("TROP.SFM", sfm(trophies, ("OptiCraft", "Minecraft 1.2.5 on PS Vita"))),
             ("ICON0.PNG", png_bytes(title_icon(assets)))]
    files += [("TROP%03d.PNG" % i, png_bytes(img)) for i, img in enumerate(icons)]
    with open(out, "wb") as f:
        f.write(build_trp(files))
    print("%s: %d trophies, %d bytes" % (out, len(trophies), os.path.getsize(out)))


if __name__ == "__main__":
    main()
