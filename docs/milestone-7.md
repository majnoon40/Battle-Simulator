# Milestone 7 — GUI viewer (raylib) and headless renders

## Scope

A window where the user watches battles; no new simulation behavior (the
sim and AI are unchanged — the only sim addition is the read-only
`squad_slot_position` accessor). Architecture: `render/` builds a pure
DrawList (circles, triangles, lines, rects, text) from a sim snapshot, a
camera and view options — no raylib; two backends draw the same list
(raylib in `game/`, a software rasterizer writing PNGs via stb_image_write
in the CLI); `game/core` keeps the viewer's logic in a raylib-free library
(camera math, fixed-timestep accumulator, picking, HUD text,
colors/shapes, ascii map). `-DBUILD_GAME=OFF` stays the default; raylib is
pinned to release tag `5.5` (`c1ab645ca298a2801097931d1079b10ff7eb9df8`);
stb is pinned to commit `9600522de53973336fa033a289858a4354cbb04c`; the
CLI builds without raylib. Full guide: `docs/viewer.md`.

## CLI (headless, verifiable)

- `--render-frame <tick> --out <file.png>`: camera fit to the world,
  1280x720, the same DrawList code as the window.
- `--render-frames a,b,c --out-prefix <dir/name>`: one
  `<prefix>-<tick:7>.png` per tick.
- `--ascii <tick>`: compact terminal map (R/B active, r/b routed, border,
  header, legend).
- In render/ascii modes the simulation stops right after the last target
  tick; the battle report reflects that state.

## Verification results (2026-11-14)

- **244/244 GoogleTest tests pass** (229 M1–M6 + 15 new M7): camera fit /
  world↔screen round-trip (exact) / zoom clamps / zoom-at-cursor invariance
  / pan; accumulator tick counts at every speed (1..16x), the frame cap
  with the "limited" flag, carry preservation, reset, speed cycling;
  picking nearest-alive-within-radius, dead-unpicked, none-when-empty,
  tie-to-lower-index; DrawList unit counts, routed desaturation, dead
  units not drawn, selection ring, 1-pixel points below the zoom
  threshold, squad links/slots/badges counts, byte-deterministic output;
  rasterizer + PNG byte-identity (same list -> same image -> same PNG
  bytes, file round-trip), PNG magic; HUD/inspector/squad-panel text,
  color mapping, shape mapping, ascii map cells; CLI flag parsing and
  validation.
- PNGs verified programmatically (PIL): correct dimensions, faction and
  routed pixel counts present per frame, HUD text pixels, frames differ
  tick-to-tick, and the two post-collapse frames (500/1000) are
  byte-identical — the battle ended at tick 318 and both renders show the
  deterministic final state.

## Acceptance

- All M1–M6 criteria still pass (244/244 includes every earlier pin).
- **BUILD_GAME=ON does not build here** — the pre-step dependency check
  found X11 core headers but no GL/Xcursor/Xrandr/Xinerama headers.
  raylib 5.5 fetches and configures its bundled GLFW, which fails with
  exactly: "RandR headers not found; install libxrandr development
  package". Per the milestone instructions this was NOT worked around;
  the user must install (Debian/Ubuntu names):
  `libx11-dev libxcursor-dev libxrandr-dev libxinerama-dev libxi-dev
  libgl-dev`, then `cmake -S . -B build -DBUILD_GAME=ON && cmake --build
  build`. Full per-OS list in `docs/viewer.md`.
- **PNGs of viewer-demo** (seed 42, morale on, ticks 0/100/250/500/1000):
  `download/m7-frames/viewer-demo-0000*.png` (reviewed by the user). The
  seed-42 battle ends by mutual rout at tick 318, so the 500/1000 frames
  show the deterministic post-collapse state (all survivors routed,
  desaturated). A supplementary morale-off set
  (`viewer-demo-moraleoff-0000250/0500/1000.png`) shows live formations at
  those ticks (at tick 1000: 745 active survivors mid-attrition).
- **20,000-unit single-frame PNG**: 46.4 ms (19,968 units, points mode at
  fit zoom 0.288 on the 2500x2500 perf battlefield).
- **UNVERIFIED by the agent** (no display): everything about the raylib
  window — see the report's hand-check list.

## Not finished / notes

- `game/src/main.cpp` (the raylib app) was never compiled or run here
  (missing system deps); all of its logic that could be pure lives in the
  tested `game_core`/`render` libraries, and the app is a thin adapter.
  The user should build it with BUILD_GAME=ON on a machine with the dev
  libraries and check the controls list in docs/viewer.md.
- M8 was not started, per instructions.
