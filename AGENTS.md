# Project maintenance rules

## Versioning (`A.B.C`)

- `1.0.0` is the public launch. Until then the major number stays `0`.
- Bump **B** (`0.B.0`) for a big step forward, such as finishing a phase in [PLAN.md](PLAN.md) or a major new capability. Reset C to 0.
- Bump **C** (`0.B.C`) for **every** other update: fixes, diagnostics, tweaks, refactors. Every rebuild that changes behavior gets a new C.
- Keep these in sync on every bump: `VERSION`, `set_version` in `xmake.lua`, the version heading in `README.md`, and a new `CHANGELOG.md` entry.
- Always rebuild after a bump so the DLL's embedded version matches, then copy it to `C:\GTS\mods\Starfield RT (Dev)\SFSE\Plugins\`.

## Changelog

- Add a dated entry at the top of `CHANGELOG.md` for every version, newest first.
- Say what changed and why, in plain language. Mention crashes fixed and anything the tester should check in-game.

## README

- Keep `README.md` current: what works, what is being tested, how to install, how to build, and where the logs are.

## Build

- Build from PowerShell (Git Bash makes xmake pick MinGW):
  `xmake f -p windows -a x64 -m release -y`, then `xmake -y`.
- Output: `build/windows/x64/release/StarfieldRT.dll`.

## Notes

- Reverse-engineering notes go in `RT_FINDINGS.md` and the roadmap in `PLAN.md`.
- Target runtime is Starfield 1.16.244 only. The plugin refuses to load on any other version.
