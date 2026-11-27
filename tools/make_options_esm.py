"""Builds dist/StarfieldRT.esm (0.2.36): the Starfield RT group in Gameplay Options.

Light master (ESM + ESL flags, like Creation Kit output) with:
  GLOB SFRT_RayTracing      - read by the SFSE plugin every 30 frames (0.5.92: replaces the two quality globals)
  GPOF SFRT_GPO_RayTracing  - Ray Tracing: Off / On (default On); quality comes from StarfieldRT.ini
  GPOG SFRT_Options                                 - the "Starfield RT" group in the menu
  QUST SFRT_OptionsQuest (start game enabled)       - script SFRT_OptionsQuest copies the options into the globals

Record layouts were copied from Creation Kit plugins (Configurable Timescale, Accurate Aiming); see RT_FINDINGS.md.
Usage: py -3 tools/make_options_esm.py [out.esm]
"""
import os
import struct
import sys

FORM_VERSION = 0x0240  # what the Creation Kit writes for Starfield 1.16
GROUP_STAMP = 0x3186

LEVELS = ["Off", "On"]
DEFAULT_LEVEL = 1

IDS = {
    "SFRT_ReflectionQuality": 0x01000800,
    "SFRT_ShadowQuality": 0x01000801,
    "SFRT_GPO_Reflections": 0x01000802,
    "SFRT_GPO_Shadows": 0x01000803,
    "SFRT_Options": 0x01000804,
    "SFRT_OptionsQuest": 0x01000805,
    # 0.5.92: new ids so saves holding the old 0-4 option values never feed them to the On/Off option
    "SFRT_GPO_RayTracing": 0x01000806,
    "SFRT_RayTracing": 0x01000807,
    # 0.5.94: quality selector (Low / Medium / High / Ultra = 1..4, the plugin's levels)
    "SFRT_GPO_RTQuality": 0x01000808,
    "SFRT_RTQuality": 0x01000809,
}
QUALITY_LEVELS = ["Low", "Medium", "High", "Ultra"]
QUALITY_DEFAULT = 3  # High


def zstr(s):
    return s.encode("utf-8") + b"\0"


def sub(tag, data):
    assert len(data) < 0x10000
    return tag.encode("ascii") + struct.pack("<H", len(data)) + data


def record(tag, form_id, subs, flags=0):
    body = b"".join(subs)
    return tag.encode("ascii") + struct.pack("<IIIIHH", len(body), flags, form_id, 0, FORM_VERSION, 0) + body


def group(label, records):
    body = b"".join(records)
    return b"GRUP" + struct.pack("<I", 24 + len(body)) + label.encode("ascii") + struct.pack("<iII", 0, GROUP_STAMP, 0) + body


def glob(edid, value):
    return record("GLOB", IDS[edid], [sub("EDID", zstr(edid)), sub("FLTV", struct.pack("<f", value))])


def gpof(edid, name, desc, levels=None, values=None, default=None):
    levels = levels or LEVELS
    values = values or list(range(len(levels)))
    default = DEFAULT_LEVEL if default is None else default
    subs = [
        sub("EDID", zstr(edid)),
        sub("NNAM", zstr(name)),
        sub("DNAM", zstr(desc)),
        sub("TNAM", b"\x01"),  # value list
        sub("VNAM", struct.pack("<f", default)),  # default
        sub("WNAM", struct.pack("<f", default)),
        sub("GPOD", struct.pack("<fff", values[0], values[-1], 1.0)),  # min, max, step
    ]
    for v, label in zip(values, levels):
        subs += [sub("VOVS", b"\0"), sub("VORV", struct.pack("<f", v)), sub("RESN", zstr(label)), sub("VORN", struct.pack("<I", 0))]
    return record("GPOF", IDS[edid], subs)


def wstr(s):
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def vmad_quest(script, props):
    # version 6, object format 2, one script, no fragment section (as Creation Kit writes for fragment-less quests)
    out = struct.pack("<hhH", 6, 2, 1) + wstr(script) + b"\0" + struct.pack("<H", len(props))
    for name in props:
        out += wstr(name) + b"\x01\x01" + struct.pack("<HhI", 0, -1, IDS[name])  # object property, edited
    return out


def build():
    option = gpof("SFRT_GPO_RayTracing", "Ray Tracing",
                  "Ray-traced reflections, bounce lighting (GI), lamp and sun shadows, glass reflections and DLSS Ray "
                  "Reconstruction. Off returns to the game's own lighting. Quality is set in StarfieldRT.ini. F8 also "
                  "switches everything off and on.")
    quality = gpof("SFRT_GPO_RTQuality", "Ray Tracing Quality",
                   "Quality of every ray-traced effect. Low and Medium are faster; Ultra traces reflections at every "
                   "pixel and GI with more rays. Has no effect while Ray Tracing is Off.",
                   QUALITY_LEVELS, [1, 2, 3, 4], QUALITY_DEFAULT)
    gpog = record("GPOG", IDS["SFRT_Options"], [
        sub("EDID", zstr("SFRT_Options")),
        sub("NNAM", zstr("Starfield RT")),
        sub("BNAM", b"\0"),
        sub("GOGL", struct.pack("<II", IDS["SFRT_GPO_RayTracing"], IDS["SFRT_GPO_RTQuality"])),
    ])
    quest = record("QUST", IDS["SFRT_OptionsQuest"], [
        sub("EDID", zstr("SFRT_OptionsQuest")),
        sub("VMAD", vmad_quest("SFRT_OptionsQuest", ["SFRT_GPO_RayTracing", "SFRT_RayTracing", "SFRT_GPO_RTQuality", "SFRT_RTQuality"])),
        sub("FULL", zstr("Starfield RT Options")),
        sub("DNAM", bytes.fromhex("110100000000000000000000")),  # start game enabled, run once (as the CK writes)
        sub("NEXT", b""),
        sub("ANAM", struct.pack("<I", 0)),
    ])
    groups = [
        group("GLOB", [glob("SFRT_RayTracing", DEFAULT_LEVEL), glob("SFRT_RTQuality", QUALITY_DEFAULT)]),
        group("QUST", [quest]),
        group("GPOF", [option, quality]),
        group("GPOG", [gpog]),
    ]
    count = 6  # GLOB x2, QUST, GPOF x2, GPOG records
    header = record("TES4", 0, [
        sub("HEDR", struct.pack("<fII", 0.96, count, 0x01000810)),
        sub("CNAM", zstr("JerKnuts")),
        sub("SNAM", zstr("Starfield RT: Ray Tracing on/off and quality in Gameplay Options.")),
        sub("MAST", zstr("Starfield.esm")),
        sub("BNAM", zstr("Main")),
        sub("INCC", struct.pack("<I", 0)),
    ], flags=0x101)  # master + light
    return header + b"".join(groups)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "dist", "StarfieldRT.esm")
    data = build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {os.path.abspath(out)} ({len(data)} bytes)")
