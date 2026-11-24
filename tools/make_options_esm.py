"""Builds dist/StarfieldRT.esm (0.2.36): the Starfield RT group in Gameplay Options.

Light master (ESM + ESL flags, like Creation Kit output) with:
  GLOB SFRT_ReflectionQuality / SFRT_ShadowQuality  - read by the SFSE plugin every 30 frames
  GPOF SFRT_GPO_Reflections / SFRT_GPO_Shadows      - Off / Low / Medium / High / Ultra (default High)
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

LEVELS = ["Off", "Low", "Medium", "High", "Ultra"]
DEFAULT_LEVEL = 3

IDS = {
    "SFRT_ReflectionQuality": 0x01000800,
    "SFRT_ShadowQuality": 0x01000801,
    "SFRT_GPO_Reflections": 0x01000802,
    "SFRT_GPO_Shadows": 0x01000803,
    "SFRT_Options": 0x01000804,
    "SFRT_OptionsQuest": 0x01000805,
}


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


def gpof(edid, name, desc):
    subs = [
        sub("EDID", zstr(edid)),
        sub("NNAM", zstr(name)),
        sub("DNAM", zstr(desc)),
        sub("TNAM", b"\x01"),  # value list
        sub("VNAM", struct.pack("<f", DEFAULT_LEVEL)),  # default
        sub("WNAM", struct.pack("<f", DEFAULT_LEVEL)),
        sub("GPOD", struct.pack("<fff", 0.0, len(LEVELS) - 1, 1.0)),  # min, max, step
    ]
    for i, label in enumerate(LEVELS):
        subs += [sub("VOVS", b"\0"), sub("VORV", struct.pack("<f", i)), sub("RESN", zstr(label)), sub("VORN", struct.pack("<I", 0))]
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
    reflect = gpof("SFRT_GPO_Reflections", "RT Reflections",
                   "Ray-traced reflections on floors, metal and other shiny surfaces. Low and Medium trace at half "
                   "resolution; High and Ultra at full resolution, with longer rays and more characters in reflections. "
                   "Also sets how many nearby characters are ray traced (for reflections and shadows).")
    shadows = gpof("SFRT_GPO_Shadows", "RT Shadows",
                   "Ray-traced sun shadows. Higher levels use more rays per pixel for softer, cleaner edges and trace "
                   "shadows from farther away. Off returns to the game's own shadow maps.")
    gpog = record("GPOG", IDS["SFRT_Options"], [
        sub("EDID", zstr("SFRT_Options")),
        sub("NNAM", zstr("Starfield RT")),
        sub("BNAM", b"\0"),
        sub("GOGL", struct.pack("<II", IDS["SFRT_GPO_Reflections"], IDS["SFRT_GPO_Shadows"])),
    ])
    quest = record("QUST", IDS["SFRT_OptionsQuest"], [
        sub("EDID", zstr("SFRT_OptionsQuest")),
        sub("VMAD", vmad_quest("SFRT_OptionsQuest", ["SFRT_GPO_Reflections", "SFRT_GPO_Shadows",
                                                      "SFRT_ReflectionQuality", "SFRT_ShadowQuality"])),
        sub("FULL", zstr("Starfield RT Options")),
        sub("DNAM", bytes.fromhex("110100000000000000000000")),  # start game enabled, run once (as the CK writes)
        sub("NEXT", b""),
        sub("ANAM", struct.pack("<I", 0)),
    ])
    groups = [
        group("GLOB", [glob("SFRT_ReflectionQuality", DEFAULT_LEVEL), glob("SFRT_ShadowQuality", DEFAULT_LEVEL)]),
        group("QUST", [quest]),
        group("GPOF", [reflect, shadows]),
        group("GPOG", [gpog]),
    ]
    count = len(IDS)
    header = record("TES4", 0, [
        sub("HEDR", struct.pack("<fII", 0.96, count, 0x01000810)),
        sub("CNAM", zstr("JerKnuts")),
        sub("SNAM", zstr("Starfield RT: ray tracing quality in Gameplay Options.")),
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
