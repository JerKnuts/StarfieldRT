# Changelog

Newest first. `1.0.0` is launch, `0.B.0` marks big milestones, and every other update bumps the last number.

## 0.5.136 - 2026-11-26

- **Fixed: no RT sun shadows at all since 0.5.134.** The phantom detector added a binding to the sun-shadow root signature, but the parameter count passed to D3D12 stayed 12, so the sun-shadow pipeline failed to build (log: `sun shadows ...: skipped: pipeline unavailable`). That's a big part of "nothing dark anymore", and why the trash-bin shadow was gone in 0.5.134 (not the phantom fix: as suspected, coincidence). The phantom detector itself never ran either, so it is untested.
- **New: lamp shadows at the points GI rays hit** (`iGILampShadowRays`, default 2). Probe behind a post standing at a wall: the lamps at the pixel were correctly shadowed and 13 of 15 GI rays hit the wall within 8-34 cm, but each hit was lit by the lamps with no shadow test (`iHitLampShadowRays=0`, chosen for reflections because of noise). So the hidden wall behind the post was lit as if it faced the lamps and bounced that light into the gap. Now the 2 strongest lamps at each GI hit get a shadow ray. GI is smoothed over frames, so the extra noise is small. Reflections are unchanged.
- **0.5.135 result:** the gap behind the post was still lit. Tram ride: F8 (all RT off) fixes the darkness, so it is caused by RT; which effect is not known yet.
- **To check:** sun shadows are back (outdoors in daylight, and the trash-bin spot: is the phantom shadow back, or does the phantom detector remove it now?). Gaps behind objects at walls should be darker. GI cost may rise a little (PERF GI line).

## 0.5.135 - 2026-11-26

- **Changed: RT GI respects the material's ambient occlusion** (tester: "there's nothing dark anymore, some things need more shadows", e.g. behind objects).
  - Our GI replaces the game's indirect (bounce) light. The game's version is multiplied by the material's baked ambient occlusion (G-buffer t2.x: creases, gaps, contact areas); ours wasn't. At one ray per 2x2 pixels and with a wide blur, our own traced occlusion smooths out, so those areas were lit like open surfaces.
  - Now our GI light is multiplied by that AO too. The game's screen-space AO is still applied on top of it by the game's composite pass, as before.
- **0.5.134 results:** the trash-bin shadow was gone (could be coincidence). The tram ride still went dark. Reflection strength 1.0 (0.5.133) is good: keep it.
- **To check:** corners, the floor under and behind furniture, gaps between objects: they should be darker than in 0.5.134, while open surfaces look the same. Numpad 1 switches GI off for comparison.

## 0.5.134 - 2026-11-26

- **New: objects the game doesn't draw no longer cast RT shadows or show in RT reflections** (tester: a "random shadow" beside an upright trash bin in Jemison, probed 3 times).
  - Probes: two invisible parts (instances 108656 and 108657, 78 and 134 triangles) stood 0.5 m above the floor at the centre of the shadow. Camera rays hit them well in front of the floor the game shows, and their record changed slightly every frame (a settling physics object), but the game drew nothing there. The MAST floor probe had the same kind of object (instance 87592).
  - The sun pass now checks this all the time: on 1 pixel in 16 per frame (rotating), a camera ray is compared with the game's depth. A hit clearly in front of what the game shows (more than 10% + 0.25 m nearer) is a "phantom" sample for that object; a hit where the game shows it is a "drawn" sample. After 16 phantom samples in a row, the object gets mask 0x10: no shadow, reflection or GI ray traces it any more, only the check itself, so one drawn sample brings it back. Characters and the player are never marked. Only up to 120 m.
  - New ini setting `bPhantomDetect` (default 1).
- **0.5.133 not tested yet** (built while the game was running); this build includes it.
- **To check:** the trash-bin spot in the Jemison station: the shadow should disappear within a second or two of looking at it. Shadows from real objects must stay: furniture, rails, trees and leaves, people. Also watch for shadows of real objects flickering on and off.

## 0.5.133 - 2026-11-26

- **Fixed (likely): the scene going dark during the tram ride.** The game's cubemaps are now hidden only while RT is fully running: not during a load or while RT fades back in afterwards. The tram ride is a load (13-25 s in the log). RT reflections and GI pause during loads, and with the probes blanked as well, interiors had no reflection and lost light. 0.5.131 (GatherIndirect keeps its probes) wasn't enough.
- **Changed: reflections twice as strong by default.** `fReflectionStrength` 0.5 -> 1.0 (1 = physically based; tester: "I would like to see more reflections"). Live: Ctrl+Numpad * stronger, Ctrl+Numpad / weaker (0.1 per press, 0.5 to 3); Ctrl+Numpad + glossier, Ctrl+Numpad - softer. The value shows on screen; put the one you like in the ini.
- **0.5.132 result:** the curved floor shadow in the Jemison station was gone (could be coincidence). The tram still went dark during the ride.
- **To check:** ride the tram (no dark scene), and look at floors and panels with the stronger reflections.

## 0.5.132 - 2026-11-26

- **Fixed (likely): big curved sun-shadow patches on floors in Jemison** (tester: "weird shadow" on the station floor; probably also blocker 10, the MAST floor shadow, and maybe 9).
  - Probe: all 144 grid samples were sun-blocked by the same object, instance 32824, a 512 m solid mesh at round coordinates (-700, 1700, -239), never drawn on screen: one of the city's distant-detail blocks. The sun rays hit it 0.1-0.4 m above the real floor, mostly on its back faces (139 of 142). Up close our copy of that block lies just off the real floor, and its edge is the curved shadow line.
  - Since 0.5.55 a back-face hit is passed only within 0.1 m + 2% of the view distance of the ray start (0.14 m here), so this got through.
  - Now objects over 150 m across are tagged (hit group 3), and sun rays pass their back faces at any distance. Seeing the back of a 500 m block means you are inside or under its low-detail shell, which isn't really there. Front faces still cast shadows, and ordinary buildings and ceilings are unchanged.
- Includes 0.5.131 (built but not installed before this).
- **To check:** the Jemison station floor and the MAST Defense floor (no big dark patches; real shadows from desks, rails and people stay). Also outdoors in daylight: distant buildings should still cast shadows, and no sunlight should leak into large enclosed spaces.

## 0.5.131 - 2026-11-26

- **Fixed (likely): everything very dark after riding the Jemison tram with 0.5.130.** GatherIndirect, the lighting pass whose probe reflection RT already replaces, keeps the game's reflection probes again. Every other pass still sees them blanked.
  - Suspected cause: the 0.5.129 survey area showed GatherIndirect binding only a single 256 px cube, not the probe array, but in other areas it seems to read the probe array, and its ambient (diffuse) light uses it too. With the array blanked, interiors lost their ambient light. The tram ride is a load into another area, which matches.
  - Its reflection output is still replaced by RT, so no cubemap reflection comes back from this.
- **0.5.130 result:** sign panels without cubemaps, confirmed; nothing too dark in that area. After the tram ride, the station was very dark.
- **To check:** ride the tram again. The station and the tram interior should have their normal ambient light. The sign panels should stay free of cubemaps. If it is still dark, toggle RT reflections with Numpad 0 there (off brings the game's probes back) and say whether the light returns.

## 0.5.130 - 2026-11-26

- **Fixed: the game's cubemap reflections are hidden everywhere while RT reflections are on** (tester: glossy sign panels showed a cubemap image, "a mix of cubemaps and RT reflections"; "apply it all around").
  - The 0.5.129 cube survey: the lighting pass we replace (GatherIndirect) no longer binds the game's reflection-probe array (it binds a single 256 px cube). The probe array (198 slices, 7 mips) is bound by about 15 other table sets, graphics and compute, most of them in passes we don't hook, plus the forward and OIT passes. One of those applies the probe reflection on the panels.
  - So instead of chasing passes: whenever the game writes or copies a view of that probe array into the shader-visible heap, the plugin immediately overwrites it with a view of a black cube array. Every pass then sees black probes, so no game cubemap reflection is added anywhere. RT reflections are the only reflections.
  - Only while RT reflections are on (Numpad 0 = on). Switching them off gives the game's cubemaps back from the next frame. New ini setting `bHideGameCubemaps` (default 1).
  - The probe array is the one the glass pass binds (found by the 0.2.39 glass code), so it is registered the first time glass is drawn after loading.
- **0.5.129 result:** the survey found the probe array in about 15 places, mostly unhooked passes (log `[cubes]`).
- **To check:**
  - The Jemison sign panels (Oishii, Mercantile): no dark-hall image; only the RT reflection of what is behind you.
  - Nothing goes too dark: shiny floors, metals, glass, water and characters (the probes may also feed some ambient light). Compare by switching reflections off with Numpad 0.
  - Switch RT reflections off and on again: the game's cubemaps should come back when off.
- **0.5.130 result:** the tester confirmed the cubemaps are gone. The probe array was registered as soon as glass was drawn after loading; no warnings, no GPU errors.

## 0.5.129 - 2026-11-26

- **New (diagnostic): cube survey.** For 3 frames after a Numpad . probe, every descriptor table the game binds is scanned for cubemap views, and each pass, root slot and table entry that holds one is logged once (`[cubes]` lines). No change to the image.
  - Why: the glossy sign panels in Jemison still show a cubemap reflection (a dark ring hall that isn't there) with RT reflections on. Screenshots: the same image with RT reflections on and off; the coverage view all green (our pass replaces the lighting pass's probe reflection there); the RT colour view shows the real bright room behind the player. So another game pass applies reflection probes, most likely the tiled lighting with local probes. The survey finds it so the next build can blank its cubemap, the way 0.2.39 does for glass.
- **0.5.128 result:** at the crosshair the panel's reflection is fully ray traced now (hit at 3.9 m, our shading, no cubemap floor).
- **To check:** stand in front of one of the sign panels and press Numpad . once. Send the log.

## 0.5.128 - 2026-11-26

- **Fixed: the game's cubemap mixed into RT reflections on glossy panels** (tester: the Oishii and Mercantile sign boxes showed "a mix of cubemaps and RT reflections").
  - Probe: the reflection ray hit a real object 4.1 m behind the player, and our shading was a dark red (0.48, 0.06, 0.07 HDR); the value used was the cubemap's grey (0.54, 0.62, 0.52).
  - Cause: since 0.5.32, an off-screen reflection hit is never darker than the game's cubemap in that direction, per colour channel. It was added for metals (copper columns, The Viewport's facade went near-black). On glossy non-metals it pasted the cubemap over the real reflection.
  - Now the floor applies to metals only (none below metalness 0.3, full from 0.7). Non-metal reflections are purely ray traced. Ctrl+Numpad 5 still switches the floor off entirely.
- **To check:** the sign boxes in Jemison (Oishii, Mercantile): the reflection should show what is really behind you, without the cubemap's shapes. Also metal surfaces (copper columns, The Viewport) should not go black.

## 0.5.127 - 2026-11-26

- **Fixed: 0.5.126 could never mark the Mercantile panel as solid** (found before testing). The glass pass's camera ray stops just short of the visible surface, so a glass-tagged object that *is* the visible surface was never sampled, and its score never moved. On the glass pass's statistics pixels (1 in 16 per frame) that no see-through glass covers, a short ray now looks for a glass-tagged object right at the visible surface; one found there counts as a sample drawn solid. Real panes aren't in the depth buffer, so they aren't found this way.
- **To check:** as for 0.5.126.

## 0.5.126 - 2026-11-26

- **Fixed: reflections passing through solid panels the game's material flags mark like windows** (tester: a glossy Jemison Mercantile board showed "a bunch of stuff that is not there" from behind the player).
  - Probes: the panel behind the player (instance 42018, material 563) is a solid surface in the game's image (G-buffer, roughness 0.31, 1.7 m away), and its own reflection ray hits the board. But the board's reflection rays went straight through it: they hit things 15-40 m away, or nothing (then the game's cubemap, a picture taken somewhere else). The reflection-source view showed the board almost all red (traced, off screen) and blue (cubemap).
  - Cause: material 563 has flags 0x12 at +108, the same value as real window panes. Since 0.2.44 that puts an object in the glass group, which reflection rays skip.
  - Fix: the glass pass already scores, per object, how much of it the game draws see-through (real panes about 1). An object it finds drawn solid (score and this frame's samples both under 0.15) now goes into the solid group as well as the glass group, so reflection rays hit it and the glass pass keeps checking it. A new pane is never marked (its samples are see-through from the start). It takes the glass pass about a second of looking at the object to learn it.
- **0.5.125 result:** the railing squares are reflections of the street lamps at about their real brightness; kept as they are.
- **To check:** the Jemison Mercantile board (look at it, and at the wall behind you, for a second or two). Its reflection should show the panel behind you, not the far hall or the cubemap. Also check that real windows still reflect and still let you see through them, and that nothing became an invisible wall for shadows.

## 0.5.125 - 2026-11-26

- **New (diagnostic): the glass probe shows what our shading of a reflection hit is made of:** the hit's albedo, its emission, the light from the game's lamps, and ambient + sun. No change to the image.
  - Why: three probes of the bright squares in the railing glass (0.5.124) all show the glass reflection ray hitting the same kind of object (family 0, material 662, instances 34201 and 34162) off screen. We shade it at HDR 6-7.5, while the glass pixel on screen is 0.01-0.02. The 0.5.122 emission fix was active, so the extra light comes from somewhere else.
  - The street-lamp probe points the same way: the lamp itself really is bright (HDR 18 on screen), but the reflection ray from it hit an off-screen surface next to it (material 485) that we shade at HDR about 9 where the game's probe says 1.5. Suspect: surfaces right next to a bright lamp get its full light in our hit shading (no shadow ray there, and the lamp's falloff is at its maximum).
- **0.5.124 results:** no hang in this session (TLAS allocated once for 262,144 objects at load, never again).
- **To check:** probe one of the railing squares again (Numpad . on a square in the glass) and send the log. The `our shading:` line says which part is too bright.
- **0.5.125 result:** the squares are the street lamps across from the railing, reflected at about their real brightness (our 5-6.6 vs the game's ~4.8 for the same panel at that exposure). Not a bug. The tester chose to keep them as they are.

## 0.5.124 - 2026-11-26

- **Fixed (likely): Frame Generation GPU hang when opening a menu or shortly after.** The ray-tracing scene's top-level buffers (TLAS) are now allocated for 262,144 objects from the start (about 120 MB, 60 MB more than before), so they are no longer swapped for bigger ones in the middle of play.
  - 0.5.123 hung 5 s after pressing Tab (20:49:37). The new labels showed none of our passes running: only Frame Generation's pacer list and a DXGI list were waiting, the usual FG signature.
  - In both 0.5.122/0.5.123 hangs, the log shows the TLAS buffers growing from 131,072 to 196,608 objects (about 90 MB of new buffers) 4-5 s before the hang: once as the menu opened, once 11 s after a menu closed. The scene had about 90,000 objects; with 25% headroom, the menu pushed the request past 131,072. Earlier FG hangs during loads (0.2.22, 0.2.29) also followed TLAS reallocations.
  - If the buffers ever grow anyway, the log line now says what was asked for and how many objects were active.
- **To check:** with Frame Generation on, run around and open menus (Tab, map, inventory) many times, including right after probing. No hang. VRAM use should be about 60 MB higher.

## 0.5.123 - 2026-11-26

- **New: GPU hang reports name the plugin pass the GPU froze in.** A marker goes before each of our 27 dispatches and acceleration-structure builds, and the crash report (DRED 1.2) now prints, for each unfinished command list, the ops by name, the last plugin label before the stop, and every plugin label in that list. No change to the image.
  - Why: 0.5.122 hung on the way to the Jemison tram (20:38:37), 14 s after Ray Reconstruction came back on viewport 2 after a 55 s menu. Unlike the earlier Frame Generation hangs, no FG pacer list was unfinished: one unnamed list had stopped on a dispatch (541 of 838 ops) and everything else waited behind it. Our passes record into the game's own unnamed lists, so the old report couldn't say whether that dispatch was ours.
  - The previous 0.5.122 hang (2026-11-25) also came about 19 s after the main view moved to viewport 2.
- **0.5.122 results:** staying in a menu for 30-60 s with Frame Generation on: no hang. The bright squares in the railing glass at night are still there with the emission fix active (`iEmissiveModel=1`), so they come from something else.
- **To check:** play normally with FG on. If it hangs, send the log: the `DRED ... last label before the stop` line names the pass. Also probe the railing squares (Numpad . on a square in the glass).

## 0.5.122 - 2026-11-25

- **Fixed (likely): glowing materials far too bright in reflections and GI when off screen** (tester: a grid of bright squares in a railing's thin glass at night, from nothing visible).
  - The glass probe: the reflection ray hit instance 33788 (standard material 149) off screen, and our shading made it HDR 13.9 where the image at that spot was 0.13.
  - Emission used the "adaptive" model: 2^(exposure offset) at a fixed on-screen brightness, ignoring the material's luminous emittance. A +4 EV panel came out around HDR 16.
  - Checked against an earlier probe of a glowing screen (material 513: luminous 95.5, +4 EV): the game showed radiance 7.75. Adaptive gives about 16x that; **luminous emittance / 4 pi gives 7.6.**
  - Emission is now luminous emittance / 4 pi in scene units (`iEmissiveModel=1`, the new default), so it also follows exposure like the game's own glow.
- May also help beta blocker 11 (tram very bright at night), if that is a glowing material.
- **To check:** the railing glass (the bright squares should be gone or faint), neon signs in reflections (Neon), and the Jemison tram at night.

## 0.5.121 - 2026-11-25

- **Fixed (likely): a long hard sun shadow on a walkway where nothing stands** (Jemison, tester screenshot; probably also beta blocker 8, the long shadows on the Neon walkways).
  - The probe: the sun ray was blocked by two **invisible 16 m panels** about 0.4 m above the floor. Instance 180 is a standard material with an emission entry (brightness 192, +6 EV) that isn't drawn as a solid surface: a light-shaft or hologram card. Instance 173 is family 7, the screen/hologram family. The game never draws either as a solid surface there, but both were in our scene as shadow casters.
- **New instance masks:**
  - **Glowing cards** (standard material, not drawn as solid, with emission flags): mask 0x10, traced by nothing. They also no longer count as glass for the thin-glass pass or its fallback search.
  - **Family 7 screens:** mask 0x08, traced only by reflection, GI and thin-glass reflection rays, so they cast no sun or lamp shadows but still show in reflections.
- **To check:** the same walkway (the shadow should be gone), the Neon walkways, and that screens still show in reflections. Watch for any shadow that should be there going missing (a glowing sign or panel that used to shade the ground).
- **0.5.121 result: confirmed in game, the walkway shadow is gone.**

## 0.5.120 - 2026-11-25

- **0.5.119: mannequin still missing.** Probes: the RT ray through the mannequin's chest hit the wall behind it (2.20 m), while the mannequin's surface was about 1.2 m away (from the depth buffer). Its skinned parts at 1.19-1.38 m all showed "frozen while others moved 227".
- **Fixed (likely): mannequins were removed from the RT scene for standing still (beta blocker 7).**
  - Since 0.5.51 a skinned body near the camera that stays frozen for 60 frames while other characters move is collapsed. That rule is for the player's unused second body, which the game leaves in a T-pose (it cast a second, T-pose shadow).
  - A mannequin never moves either, so while NPCs walked around, it was collapsed after about a second.
  - Now a body the game skinned this frame (LOD record bit 30, set only for bodies the game draws) is never collapsed. The player's unused body never has that bit, so it is still removed.
- **To check:** the mannequin in the red wall's reflection and its shadow. In third person, make sure the T-pose shadow doesn't come back.
- **0.5.120 result: confirmed in game, the mannequin reflects.**

## 0.5.119 - 2026-11-25

- **Fixed (likely): mannequins and some nearby characters missing from reflections (beta blocker 7).**
  - Mannequins are posed character models, so they enter the RT scene as skinned characters: one slot per character part (body, head, hair, clothes), nearest first, 64 slots on High and 128 on Ultra.
  - The GPU lists the candidate parts in arbitrary order and the list held 256. In Neon it was full ("256 candidates within 60 units"), so parts were cut off at random before the nearest-first sort: a mannequin a few metres away could lose to NPCs much further off.
  - The candidate list now holds 2048, so the sort sees every part in range. The slot counts per quality are unchanged.
- **To check:** the mannequin from the soak test (and characters close to you in crowded Neon) in reflections. If a mannequin is still missing, probe it with Numpad . and send the log.

## 0.5.118 - 2026-11-25

- **0.5.117: still no screen in the window.** Likely cause (not yet confirmed by a probe): glow learning skips pixels the last-frame image marks as covered by glass or smoke, and a family 7 screen draws its picture as a transparent layer over a black panel. Then every screen pixel is "covered" and nothing is ever learned.
- **Changed: family 7 screens learn their glow from covered pixels too**, as long as the distance matches the screen (so it's the screen, not something in front of it). In the main reflections and GI. Other objects still never learn from covered pixels.
- **To check:** look at the AURORA screen for a few seconds, then turn away: does the window keep its colour? If not, one probe of the window (Numpad .) shows the glow value directly.
- **0.5.118 result:** the window still doesn't reflect the family 7 screen. Video screens are parked for future work (notes in PLAN.md, blocker 5). The glow memory stays in: it works for other glowing surfaces, and GI is back to about 1-3 ms.

## 0.5.117 - 2026-11-25

- **0.5.116: the window still didn't reflect the screen.** Four probes on one family 7 screen: the last-frame image can see it bright (HDR 2.06 / 0.89 / 1.94 pink), but each probe hit a **different instance** (2977, 3111, 3072, 3146) with a different material, same mesh (9049) and place. Each video frame is a separate object record, so a memory keyed by object number started over every frame.
- **Fixed: family 7 glow is keyed by the screen's world position** (0.5 m cells, hashed into the same memory) instead of its object number, so every frame of one screen's video feeds and reads the same entry. Other families are unchanged.
- **To check:** look at a video screen for a few seconds, then turn so it's out of view: thin glass, reflections and walls nearby should keep its colour (one averaged colour, not the picture).

## 0.5.116 - 2026-11-25

- **0.5.115 glass probe found it:** the glass reflection ray did hit the screen (instance 3398, family 7) every frame, but the screen's **material changed every frame** (372, 408, 358, 434, ...): family 7 screens play their video as a flipbook of materials.
  - The glow memory tagged each entry with the object's material word (so a reused slot wouldn't inherit another object's glow). For a flipbook screen the tag never matched: learning restarted every frame and lookups found nothing (glow 0.000).
- **Fixed: the glow tag leaves out the material number for family 7.** Other families keep the full tag. A used entry is now marked by bit 31 instead of bit 0.
- This should also fix family 7 screens in the main reflections and GI, not just thin glass.
- **To check:** look at a video screen for a few seconds, then turn so it's out of view: thin glass, reflections and nearby walls should keep its colour.

## 0.5.115 - 2026-11-25

- **0.5.114 glass probe:** last frame's image was usable for thin glass in all but 0-2 frames per 600. At the window probed, the glass reflection ray hit something every frame but never took its colour from the image. The ring didn't record what the reflection ray hit.
- **Glass probe ring extended (32 -> 48 bytes per frame):** each frame now also logs the glass reflection ray's hit (instance, family, material), why it did or didn't take the screen colour (off screen last frame, under smoke/glass/metal, distance mismatch, used), and the glow added from the glow memory.
- **To check:** probe (Numpad .) a window that should reflect the magenta screen again and send the log.

## 0.5.114 - 2026-11-25

- **0.5.113 result:** the magenta screen still doesn't show in thin glass. Next step: a glass probe (Numpad . on the window) to see what the glass reflection rays hit.
- **Thin glass reflections are dimmer: `fThinGlassStrength` 1.5 -> 1.0** (the tester: too strong). 1.0 is real glass's Fresnel. Alt+Numpad - / + still adjust it live (x0.8 / x1.25) and the new value shows on screen.
- **To check:** thin glass brightness; if it needs more, note the value Alt+Numpad - gives you.

## 0.5.113 - 2026-11-25

- **0.5.112 result:** the family 12 screen now shows in reflections (seen on the player character), but not in thin glass.
- **New: thin glass uses the glow memory too.** Glass reflection hits that weren't on screen last frame add the object's learned glow, like the main reflections. The glass pass reads the main reflection pass's memory, only when both run at the same size (otherwise the address would be wrong; then glass works as before).
- **To check:** the same screen reflected in a storefront window, including when the screen itself is out of view.

## 0.5.112 - 2026-11-25

- **0.5.111 probe:** the non-working screen was **family 12** (material 72). In last frame's image it is bright (HDR 2.9 / 1.0 / 3.4), so the glow memory could learn it.
  - Why it didn't: our shading draws families we can't read as a grey placeholder, and the glow rule only counted light beyond 8x our shading. A lit grey surface x8 out-shone the screen.
- **Fixed: glow learning for unknown material families.** For families other than 0 (standard) and 3 (vegetation), the glow is now simply what our placeholder shading misses. Standard and vegetation materials keep the 8x rule (their shading is real, so ordinary lit surfaces don't get a glow).
- **Scene table dump findings** (RT_FINDINGS): slot +22 (368-byte entries) holds an emission block in the standard layout for family 12 material 72 (luminous 31.8, flags 0x3341). +12 (96 B) and +17 (176 B) hold texture-index-like values. The family 7 table is still unknown: the earlier family 7 screen showed almost black in the image, so it needs its own probe with this build.
- **To check:** the family 12 screen you probed (bright magenta): look at it, then away; reflections and walls nearby should keep its colour. And if you find a screen that still doesn't, probe it with Numpad .

## 0.5.111 - 2026-11-25

- **0.5.109 result: some video screens kept their light, some didn't.** Probes of one of each:
  - The working screen is an ordinary material (family 0) with an emission entry, which our shading already lights. The glow memory wasn't needed there.
  - The failing screen is **family 7**. In the image we learn from it is almost black (HDR 0.016 green, about 40x darker than the working one), so the glow memory has nothing to learn. The video is most likely drawn after the point where we capture last frame's image, or in a way that capture doesn't see.
  - So family 7 screens need their own texture read at the ray hit, which needs the family 7 material table. It isn't one of the tables we know (+11 standard, +13 vegetation).
- **New: the crosshair probe (Numpad .) dumps the scene table when it hits an unknown family.** For every CPU-readable buffer in the scene table it logs the stride, size and the hit material's entry. That should show which slot is the family 7 table and where its texture index sits.
- The glow memory stays (it covers other glowing surfaces our shading gets too dark). 0.5.110's faster learning is included.
- **To check:** probe a family 7 screen (one that doesn't keep its light) with Numpad . and send the log.

## 0.5.110 - 2026-11-25

- **0.5.109 result:** no errors. GI cost went up from about 3 ms to 3.2-7.5 ms (Neon, High, Frame Generation on, frames 24-34 ms).
- **Fixed: the glow learning made GI slower.** It picked 1 trace cell in 16 inside every 8x8 block, so every GPU thread group waited for its few learners. Learning now happens in whole 8x8 blocks, 1 block in 16 per frame (rotating): the same amount of learning, but 15 of 16 groups skip it entirely.
  - The reflection pass's brightness-correction learning (since 0.5.75) used the same pattern and is changed the same way, so reflections may get a little cheaper too.
- **To check:** GI and reflection times in the log (Neon). And still: do video screens keep their coloured light and reflections after they leave the view?

## 0.5.109 - 2026-11-25

- **New: video screens keep lighting the room and showing in reflections when they are off screen (beta blocker 5).**
  - Glowing screens in Neon are material family 7, which reads black in the G-buffer, and we haven't decoded that family's material table. Reflection and GI rays that hit a screen took its colour from last frame's image while it was visible, but once it left the view our own shading saw a black surface.
  - Reflection and GI rays now also learn each object's **glow** while it is on screen: the part of its colour that our shading can't explain even with the largest brightness correction (x8). Off-screen hits add it back. It updates continuously (10% per sample), so it follows a video's average colour over time.
  - Each entry is tagged with the object's material, so an object slot reused after a load doesn't inherit another object's glow. Objects never seen on screen still have no glow.
  - Any other glowing surface our shading gets too dark (signs, panels) benefits the same way. Ordinary lit surfaces don't get a glow, because the brightness correction already explains them.
  - Memory: 8 MB more for reflections and 8 MB more for GI.
- **To check:** Neon, near big video screens. Look at a screen, then turn so it's off screen: walls, floors and reflections nearby should keep the screen's coloured light. Watch for anything glowing that shouldn't (a wrong object lighting up after a load or after turning around).

## 0.5.108 - 2026-11-25

- **Fixed (likely): NPC hair very shiny (beta blocker 4).**
  - Skin and hair are G-buffer types 1 and 8. The game takes their roughness from a material table (RT_FINDINGS, "Indirect"), so the G-buffer's roughness and metalness channels don't mean roughness and metalness for them. The RT reflections already left them to the game, but the RT lamp pass read them as normal materials. Hair came out with a mirror-sharp, bright highlight from every lamp, partly tinted like metal.
  - Skin and hair now get a soft non-metal surface (roughness 0.5, metalness 0) in the RT lamp pass, the Ray Reconstruction guide images, GI and the last-frame capture. We don't read the game's table yet; if 0.5 is too dull or too shiny, that table is the next step.
- **To check:** NPCs under lamps (Neon, Jemison): hair and skin should no longer shine. Also check faces don't look flat or too matte.
- **0.5.108 result: confirmed in game, hair is fixed.**

## 0.5.107 - 2026-11-25

- **High reflections now trace 1 ray per pixel (beta blocker 3, "High still too noisy").**
  - The tester turned effects off one at a time on High: reflections (Numpad 0) were the noise.
  - High traced 1 reflection ray per 2x2 pixels, moving inside the block every frame: a coarse grain that crawls, which the temporal filter couldn't hide. Ultra had the same noise until 0.5.89 put it back to 1 ray per pixel.
  - Expected cost: about 2-4 ms more on High in busy places like the Neon walkways. High still differs from Ultra in GI rays (2 vs 4), lamp and sun shadow rays (4 vs 8), reflection length and RT scene range.
- **To check:** High, the same noisy spot. Is the reflection noise gone, and what is the frame rate (send the log)?
- **0.5.107 result: confirmed in game, the noise is gone.** Neon walkways on High: reflections 7-9 ms (was 3-5), frame 28-32 ms with Frame Generation on (31-35 fps rendered).

## 0.5.106 - 2026-11-25

- **0.5.105 result:** 7 loads with Frame Generation on, no hang. RR stepped aside for every load and FG switch and came back each time. The fade-in from 0.5.104 was still too noticeable.
- **Fixed: no sun shadows at all while RT waited after a load.** While RT sun shadows are on, the plugin stops the game drawing its own sun shadow maps. That stayed in force while the RT shadows were paused for the load, so outdoors everything was in full sun for several seconds, then RT shadows snapped on.
  - Now the game's sun shadows stay until RT has fully faded in. RT sun shadows fade in on top of them, then take over.
- **New: lamp shadows fade in too.** Before, only reflections, GI and thin glass faded.
- **Fade is longer: 1.5 s** (was 0.75 s).
- **New ini settings** (`[RT]`): `fRtSettleSeconds` (default 2, the wait once the scene is ready) and `fRtFadeSeconds` (default 1.5). They let us try other timings without a rebuild. Going below 1 s of wait risks the old after-load GPU hangs.
- **To check:** loads outdoors in daylight (sun shadows should be there the whole time and change softly), and interiors with lamps. Is the change still noticeable, and which part stands out most?
- **0.5.106 result:** 5 loads, no hang. The tester called the return of RT after a load "good enough for now".

## 0.5.105 - 2026-11-25

- **Crash on 0.5.104 entering an interior (21:01:28, GPU hang).**
  - DRED shows only Frame Generation's pacer list and an internal DXGI list unfinished, none of ours. That's the same signature as the 0.5.81 menu hang and the earlier load hangs.
  - 7 s before, the load started, and the game switched Frame Generation off (21:01:21) and back on (21:01:23). The scene was still loading when it hung, so reflections, GI and glass were paused: the 0.5.103 shorter waits and the 0.5.104 fade weren't active.
  - Ray Reconstruction was still running through the load. The 0.5.84 bisect showed these hangs need Frame Generation and Ray Reconstruction together.
- **Fix (likely): no Ray Reconstruction during loads, or for 2 s after Frame Generation switches.** DLSS Super Resolution runs instead. Every RT effect is paused during loads anyway, so RR added nothing then.
  - The log says `[RR] held back (scene loading)` or `(Frame Generation just switched)` when this happens.
- **To check:** with Frame Generation on, go in and out of interiors, fast travel and load saves several times. No hang. RT and RR should come back after each load (RR resumes with the RT fade-in).

## 0.5.104 - 2026-11-25

- **0.5.103 result:** five loads, no GPU hang. RT came back 5-12 s after each load started. The tester couldn't see a difference, and asked for RT to come back faster or less noticeably.
  - The wait after the scene is ready is now 2.4-3.3 s. The rest is the game still streaming in, sometimes in a second wave 0.3 s after the first (another 6 s).
  - That second wave is exactly where the 0.3.7 GPU hangs happened (records still being rewritten), so it stays paused for now.
- **New: RT fades in after a load instead of popping on.** Reflections, GI and thin glass blend from the game's own result to RT over about 0.75 s.
  - RT lamps don't fade: the game's lamps are switched off while they run, and they have the same brightness, so only their shadows appear. Sun shadows also still switch on directly (they come back earlier, when the scene is ready).
  - The fade is passed to the shaders in spare bits of an existing constant, so nothing else in the passes changes.
- **To check:** load a save, fast travel, go through doors. The return of RT should be a soft change instead of a pop. Also check that reflections, GI and glass look the same as before once it has faded in (nothing stuck dim).

## 0.5.103 - 2026-11-25

- **0.5.102 duplicate check: no duplicates to remove.** Only 1.5% of the objects are exact copies. 62% on the Neon walkways share their place with a different mesh, but those are the parts of one model split by material (real geometry). In short runs the walkways and Jemison hold 25-31 ms, so the soak test's 61-100 ms looks tied to VRAM filling up over a long session (to be confirmed with a long run later). Details in RT_FINDINGS.md.
- **RT appears sooner after a load (beta blocker 2).** After a load, RT waits until the scene is calm, then waits more before reflections, GI and lamps return (the 0.3.7 guard against GPU hangs right after a load). Both waits were counted in frames: at 35 fps rendered that was 2.6 s + 5 s.
  - Now each wait is the shorter of the old frame count and a fixed time: 1 s calm, then 2 s (at least 30 and 45 frames). At 35 fps, RT should come back about 4.5 s sooner. Nothing changes at 90 fps or more.
  - The same waits also ran mid-walk when Jemison streamed in a lot at once (20:44:54 and 20:45:17 in the last log), so RT dropped out there too. Those gaps get shorter as well.
  - New log line: **RT effects back after N s**, so we can measure the gap after each load.
- **To check:** load a save, fast travel and go through a door or two. RT should come back quicker. Most importantly: **no GPU hang or crash after loads** (the waits exist because of those). Send the log.

## 0.5.102 - 2026-11-25

- **0.5.101 breakdown: the Neon walkways' 222k objects are mid-sized pieces at mid range.**
  - 160-175k are 50-200 m from the camera, and 134k have a bounding radius of 2-10 m (building kit). Only about 3k are alpha-tested and 369 are effect meshes, so neither is the cost.
  - Jemison is different: 83-98k of its 143-161k objects are more than 1 km away, mostly large (45k over 10 m), and 18k are alpha-tested.
  - 222k building pieces within 200 m seems like too many, so this build checks whether the game's records hold the same object more than once.
- **New: duplicate count in the TLAS breakdown line.** It counts objects that are the same mesh at exactly the same place and rotation, and objects that share a place with another one. It uses a 4 MB GPU table cleared each frame (a few microseconds).
- **To check:** the Neon walkways and Jemison again for 20 s each, then send the log.

## 0.5.101 - 2026-11-25

- **0.5.100 test: the slowdown is smaller but not fixed.** On Ultra the Neon walkways ran at 26-37 ms (27-39 fps rendered, Frame Generation on) instead of the soak test's 61 ms. Jemison ran at 31-37 ms. VRAM stayed at 55-63% of budget, so the VRAM guard never kicked in.
  - The size cut left out only 3.5k of 269k objects on the walkways (1.4%) and 48k of 272k in Jemison (18%). The walkways' objects are not small far-away clutter, so the cause was guessed wrong.
  - Going from 63k to 265k objects costs: reflections 3.2 to 5-9 ms, GI 3.0 to 3.7-6 ms, TLAS build 1.6 to 2.4 ms, plus 2-3 ms of sun shadows outside.
- **New log line: TLAS breakdown** (every 10 s). It counts the objects that stay in the RT scene by distance, by size, and how many are alpha-tested, effect meshes and characters. It shows what the 265k objects actually are before the next fix.
- **To check:** stand on the Neon walkways and in Jemison for 20 s each (any quality) and send the log.

## 0.5.100 - 2026-11-25

- **Soak test (0.5.99) found a big slowdown.** The beta is on hold; the tester's 12 findings are listed at the top of PLAN.md.
  - Outside on the Neon walkways, the RT scene jumped from 65k to **275k objects** (Ultra's 4 km range picked up all the far-away city clutter). Frames went from 17 to 61 ms.
  - In Jemison: 110k objects, VRAM at **97% of budget**, and 90-100 ms frames by the end.
- **New: objects too small on screen are left out of the RT scene.**
  - The cut is on an object's bounding radius divided by its distance: about 3 px on Ultra (0.0015), 4 on High, 6 on Medium, 8 on Low (at 3440 wide).
  - A cup drops out past about 100 m, a crate past about 500 m. Buildings and large props stay in to the full range. Characters and the player are never cut.
  - The TLAS log line now counts how many objects were left out this way.
- **New: VRAM guard.** While local VRAM is over 90% of budget, the RT scene range shrinks in steps (down to 40%) and the size cut grows. It grows back once usage drops under 82%. Each step is logged (`VRAM ..% of budget: RT scene range x..`). This mainly cuts our scene size and build time; most of the VRAM in Jemison is the game's own.
- `[RT] fTlasMinScreenSize`: -1 = from the quality level, 0 = off, or a value.
- To check in game:
  - The Neon walkways and Jemison at Ultra. Compare frame rate and VRAM with the soak test.
  - Do far-away objects visibly lose their shadows or reflections?
  - Send the log.
## 0.5.99 - 2026-11-25

- **Fix: the storefront flicker behind thin glass, found with 0.5.98's per-frame crosshair log.**
  - At the flickering spot the result cycled every 3 frames: reflected, reflected, rejected.
  - On 2 frames the tagged-glass ray found nothing, so the untagged-window search ran and found the storefront pane 0.37 m away (shop-window family 13, untagged), which reflected.
  - On every 3rd frame the tagged-glass ray found a tagged layer lying on the floor 18.4 m behind the pane (instance 45655, family 0). It was correctly rejected as "lying on a surface". But a tagged hit used to skip the untagged-window search entirely, so the pane in front lost its reflection for that frame.
- Now the untagged-window search also runs in front of a tagged hit, up to its distance, and the nearer acceptable object wins. It costs one short extra ray only on pixels where tagged glass was found.
- 0.5.96 (per-object "stands clear" score) and 0.5.97 (candidate cap 512) weren't the cause but stay. Both remove real ways for the same pass to flip.
- **Confirmed in game:** the storefront flicker is gone.
## 0.5.98 - 2026-11-25

- 0.5.97 didn't fix the storefront flicker either, so this is a diagnostic build.
- **New: the Numpad . probe logs what the thin-glass pass decided at the crosshair on each of the last 64 frames.**
  - Stage: no glass coverage / rejected / reflected.
  - Covered, tagged, hit, fallback ran, reflection ray hit, colour from last frame's image.
  - Object family and instance, distance.
  - Candidates checked by the first ray and the fallback ray.
  - Standoff and final weight, and the reflection's brightness.
  - A flicker shows up as the frame where one of these changes.
- No change to the image.
- To check in game: put the crosshair on a spot that flickers, keep still, and press Numpad . (twice is fine). Send the log.
## 0.5.97 - 2026-11-25

- **Fix (second try): flickering patches behind the Neon storefront glass.**
  - 0.5.96 didn't fix it. The debug-colour video showed magenta patches ("covered, no pane found") on the far floor and the overhead truss, changing shape every frame. The rest of the pane reflected.
  - The crosshair probe hit the pane itself (instance 41399, 0.37-0.39 m, shop-window family 13, untagged, score 1.0). So the pane is in the scene; some rays just don't reach it.
  - Cause: the glass camera ray gave up after 64 see-through candidates. The hardware doesn't hand them over nearest first, so a ray toward the dense smoke and truss layers used up its 64 on far-away layers before reaching the pane 0.4 m in front. Which pixels ran out changed every frame.
  - The cap is now 512. It still stops a runaway loop (the reason for the cap since 0.3.2).
- Not the cause, ruled out with the tester: the first-person gun (holstered and third person: no change), the smoke filter, the screen colours, old inputs.
- 0.5.96's per-object "stands clear" score stays. It removes the same kind of flip at the foot of panes.
- To check in game: the storefront flicker. In the debug colours (Numpad 4 twice) the far floor and truss behind the pane should stay one colour. Also check GPU time in the `PERF thin glass` line (was 0.4-1.1 ms).
## 0.5.96 - 2026-11-25

- **Fix: flickering patches seen through thin glass** (Neon storefronts, around the cart and the floor at the foot of the pane).
  - 0.5.95 test: only Ctrl+Numpad 4 (untagged-window search off) stopped it. Screen colours, old inputs and the smoke filter weren't the cause; the log counted 0-3 bad frames in 600.
  - Cause: the storefront pane has no glass tag, so the untagged-window search finds it. Since 0.5.64 a hit only counted as a pane where it stood clearly in front of the surface behind it, decided per pixel. That rule filters fake layers lying on floors. Where the floor or the cart sits just behind the pane, pixels were right on the limit, and DLSS's sub-pixel depth jitter flipped them between reflecting and not reflecting every frame.
  - Now each object gets a "stands clear" score over many frames (sampled 1 pixel in 16 per frame, about 0.2 s to accept, faster to drop), and that score decides for the whole object. A pane in the open is reflected everywhere, including right in front of the cart. A layer lying on a surface still scores low and stays off.
  - `bThinGlassStandoffScore=0` brings back the per-pixel test.
- The glass statistics buffer grows by 6 MB (per-object standoff counts and score).
- To check in game:
  - At the storefront the patches no longer flicker, and the pane still reflects.
  - Elsewhere in Neon no new fake reflections appear on floors or the truss in the smoke (the 0.5.64 case).
## 0.5.95 - 2026-11-25

- **Diagnostics for the flicker seen through thin glass** (Neon storefront). Numpad 4 (thin glass off) stops it; the smoke filter, RT lamps and the quality level don't.
  - New key **Ctrl+Alt+Numpad 4:** thin-glass reflections stop taking colours from last frame's image and use ray-traced colours only.
  - The `[G3] camera for thin glass` log line now also counts frames where the reflection pass's inputs were older than this frame, and frames where last frame's image couldn't be used.
- To check in game, at the flickering window:
  1. Press Ctrl+Alt+Numpad 4: does the flicker stop?
  2. Press Ctrl+Numpad 4 (untagged-window search off): does it stop?
  3. Press Numpad 4 twice to get the debug colours: do patches switch between colours?
  Then send the log.
## 0.5.94 - 2026-11-25

- **Gameplay Options has a quality selector again:** Starfield RT > **Ray Tracing Quality**, Low / Medium / High / Ultra (default High), next to **Ray Tracing** On/Off. Both apply live and are saved with the game.
  - The menu quality sets both the reflection and shadow levels unless the ini has `-1` (custom) for them.
  - The game's options records can't grey out one option based on another (checked against every options record in the official plugins). So the quality selector stays visible while Ray Tracing is Off. It has no effect then, and ray tracing comes back at that quality when switched On.
- 0.5.93 test: the tester saw flickering on the floor seen through thin glass in Neon (Medium, DLSS Quality, FG on). Not fixed yet; under investigation.
- To check in game:
  - The menu shows two options.
  - Changing the quality writes a `quality (Gameplay Options)` line in the log.
  - Ray Tracing Off then On comes back at the chosen quality.
## 0.5.93 - 2026-11-25

- **Medium GI is back to 1 ray per 2x2 pixels.** In 0.5.92's test the 4x4 GI looked blotchy and crawled slightly, and it saved only about 1 ms (GI 0.5-0.7 ms instead of 1.5-2.3).
  - 0.5.92 Medium in Neon (DLSS Quality, FG off): 14.5-18 ms (56-69 fps). Reflections 1.5-2.5 ms, RT lamps 1.0-1.7.
  - Medium keeps its other cuts: no sun shadows inside reflections, 2 lamp shadows, shorter rays and scene range.
- Low keeps 4x4 GI.
- Gameplay Options "Ray Tracing On" worked (log: `Gameplay Options: Ray Tracing On`).
## 0.5.92 - 2026-11-25

- **Gameplay Options is now one setting:** Settings > Gameplay Options > Starfield RT > **Ray Tracing**, On or Off (default On). Off is the same as F8: every RT effect off and the game's own look. On switches them back on. The old RT Reflections and RT Shadows menus (Off to Ultra) are gone. Quality now comes from `iReflectionQuality` / `iShadowQuality` in StarfieldRT.ini (default Ultra). The option has a new id, so older saves start at On.
- `bStartAllOff` now defaults to 0. With the plugin active, the menu decides once a save is loaded.
- **Medium is a real step down from High** (it was under 1 ms cheaper):
  - GI traces 1 ray per 4x4 pixels (was 2x2). New in the shaders: 4x4 trace cells, with the traced pixel moving through the cell in a 16-frame Bayer order.
  - No sun shadow rays inside reflections.
  - RT lamps shadow-test their strongest 2 per pixel (was 4).
  - Reflection rays 1500 (was 2000) and RT scene range 1200 (was 1500).
- **Low:** GI at 4x4 and 1 lamp shadow.
- To check in game:
  - The Gameplay Options menu shows one "Ray Tracing" option and On/Off works live.
  - Medium (`iReflectionQuality=2` and `iShadowQuality=2` in the ini) is faster than High and its GI doesn't crawl or flicker.
## 0.5.91 - 2026-11-25

- **Timings in Neon** (3440x1440, DLSS Quality; ms per rendered frame):

  | Preset | FG off | FG on |
  |---|---|---|
  | RT effects off | 9-10 ms (~100 fps) | |
  | Medium | 15-18.5 ms (54-66 fps) | |
  | High | 16-19 ms (53-63 fps) | 18-21 ms (47-56 rendered, ~95-110 shown) |
  | Ultra | 19-24 ms (41-53 fps) | 24-27 ms (37-41 rendered, ~75-80 shown) |

- Medium is barely cheaper than High. The difference between the presets needs another look before the beta.
- The PERF lines said "FG unknown" until Frame Generation was first switched on, because the game doesn't set FG options before then. They now say "FG off" in that case. Logging only.
## 0.5.90 - 2026-11-24

- **0.5.89 confirmed in game:** the noise looks better. Ultra in Neon (3440x1440, DLSS Quality, **Frame Generation on**): 24-27 ms per rendered frame (37-41 fps rendered, about 75-80 on screen). Reflections 4.7-6.2 ms, GI 3.7-4.5, RT lamps 1.5, TLAS 1.6.
- The log now records **Frame Generation**. A new line `slDLSSGSetOptions ... Frame Generation ON/OFF` appears whenever the game changes it. Each `PERF RT mode` line now says `FG on/off` and reports "fps rendered" (with FG on, the screen shows about twice that). Before, the log couldn't tell whether FG was on, so the timings were easy to misread.
- Logging only. Nothing about rendering changes.
- To check in game: switch FG off and on once in Display settings, then send the log.
## 0.5.89 - 2026-11-24

- **0.5.88 timings** (Neon, 3440x1440, **DLSS Quality** (renders 2292x960), Ultra, everything on, Frame Generation off): frame 16-22 ms (45-64 fps), down from 66 ms. Reflections 1.2-2.4 ms, GI 1.6-3.0, RT lamps 1.1-1.9, thin glass 0.4-0.9, RR guides 0.6, TLAS 1.6.
- The tester said the image looked **noisier**. That is the 0.5.88 cuts: reflections dropped to 1 ray per 2x2 pixels and Ultra GI to 2 rays per 2x2 cell.
- With the time saved, **Ultra** now spends some back on quality:
  - reflections trace **1 ray per pixel** again (expected about +5 ms);
  - GI traces **4 rays** per 2x2 cell (was 2; expected about +2.5 ms).
- **High** GI traces 2 rays per cell (was 1). High reflections stay at 1 ray per 2x2.
- To check in game: Ultra, DLSS Quality, Neon. Is the noise back to how it was before 0.5.88, and what is the frame rate? Send the log.
## 0.5.88 - 2026-11-24

- **0.5.87 timings** (Neon, 3440x1440, DLSS in **DLAA** mode, everything on, Frame Generation off, GPU ms per frame):

  | | Ultra | High |
  |---|---|---|
  | Frame | 66 ms (15 fps) | 34-41 ms (24-29 fps) |
  | GI | 23.3 | 2.2-3.0 |
  | Reflections | 17.7 | 9.3-14.5 |
  | RT lamps | 4.0 | 2.8-3.5 |
  | TLAS build | 1.6 | 1.0-1.1 |
  | Thin glass | 1.3 | 0.9-1.3 |
  | RR guides | 1.3 | 1.2 |

- **Presets retuned:**
  - **Reflections trace 1 ray per 2x2 pixels on every level** (Ultra and High traced one per pixel). Ray Reconstruction fills in; Cyberpunk traces reflections at reduced resolution too.
  - **Ultra GI 1 ray cell per 2x2 pixels** (was per pixel), still 2 rays: about a quarter of its 23 ms.
  - **0.5.71's long Ultra ranges pulled back:** RT scene range 10 km -> 4 km (High 3 -> 2.5 km), reflection rays 10 -> 5 km, sun rays 10 -> 4 km, characters within 60 m (was 100). The 10 km scene more than doubled the TLAS build for objects you can barely see.
  - Lamp shadows on Ultra: 8 per pixel (16 was set, but the lamp pass caps at 8).
- **Big free win: DLSS Quality instead of DLAA.** DLAA renders at full resolution, so every RT pass works on all 5 million pixels. DLSS Quality renders 2293x960, about 44% of the pixels, which roughly halves the cost of every RT pass. Ray Reconstruction is designed for upscaling.
- **To check in game:** Ultra, everything on, Neon, DLSS Quality (Display settings). One minute, then send the log. Also say whether reflections look noticeably softer or noisier than before.

## 0.5.87 - 2026-11-24

- **0.5.86 result (run D): no hang with Frame Generation + Ray Reconstruction + every effect on**, menus opened about 20 times. RR came back after every menu (12 "running" lines). **The Frame Generation menu hang is fixed.**
- **But: very low frame rate on Ultra with everything on.** PERF: frame 45.7 ms (22 fps); RT reflections alone 11.5 ms GPU; TLAS build 1.43 ms (0.06 ms with effects off); VRAM 81% of budget. Part of this is 0.5.71's longer Ultra ranges (10 km scene range and rays, characters within 100 m, more lamp shadows).
- **New: GPU timings for every RT pass.** Every 600 frames the log now prints `PERF GI`, `PERF RT lamps`, `PERF thin glass`, `PERF last-frame image capture` and `PERF RR guide images`, next to the existing `PERF reflections` and the sun-shadow/BLAS/TLAS line. That shows where the frame time goes, for tuning the presets and for the Nexus page (checklist step 5).
- **To check in game:** Ultra, everything on (F8), FG off, in Neon for about a minute (stand still for part of it). Then switch Gameplay Options to High for a minute. Send the log.

## 0.5.86 - 2026-11-24

- **0.5.85 result (run D):** no crash, but **Ray Reconstruction stopped running after a few menus**, so the run doesn't prove the fix yet. The log shows why: the game takes a **new viewport number for the main view at every menu toggle** (0, 2, 4 ... 100 in one session). 0.5.85's per-viewport table had 8 slots, so from viewport 8 on there was no entry and the game got plain DLSS.
- The same log confirms the bug 0.5.85 targets. Each toggle switches the old main viewport off ("viewport 0: mode 0, 4294967295x4294967295") a quarter of a second before the new one is set up ("viewport 2: mode 6, 3440x1440"). In that gap the old single global copy held the invalid "off" options.
- **Fix:** the per-viewport options are now a lookup keyed by viewport number, with no limit. A viewport switched off is dropped.
- **To check in game (run D again):** FG on, every effect on (F8), Ray Reconstruction on. Open and close menus about 20 times, play a few minutes. The log should keep saying `DLSS Ray Reconstruction is running` after each menu.

## 0.5.85 - 2026-11-24

- **0.5.84 result:** the thin-glass reflections no longer jump while turning (confirmed). The log showed the old camera was from another frame 596 of 600 frames.
- **Frame Generation test (FG on, menus opened ~20 times):** A, plugin disabled: no hang. B, plugin on with every effect off: no hang. C, every effect on with Ray Reconstruction paused: no hang. So the hang needs **Ray Reconstruction + Frame Generation**.
- **Fix (likely): GPU hang with Frame Generation on when opening a menu.**
  - **Cause:** the game sets DLSS options per viewport. Opening a menu sets viewport 2 to DLSS off with an output size of 4294967295 x 4294967295 (the log line right before the 21:19 hang). Our hook kept only the last options it saw, for any viewport, and used them to set up Ray Reconstruction on the main viewport. So the menu's "off, invalid size" options configured RR on the main view. With Frame Generation pacing those frames, the GPU hung.
  - **Fix:** DLSS options are kept per viewport. RR is set up and run only on a viewport whose own options are valid (DLSS on, a sane output size); others get the game's DLSS untouched. The log says when a viewport is skipped (`[RR] viewport N: DLSS off or invalid there`).
- **To check in game (run D):** FG on, every effect on (F8), Ray Reconstruction on. Open and close menus about 20 times, play a few minutes. Then send the log.

## 0.5.84 - 2026-11-24

- **0.5.83 result:** "worse". The log shows the fix never engaged: this frame's camera was **not found** in the transparency pass's tables, so the glass used the old camera exactly as before. Any difference was chance.
- **Second attempt:** the camera now also comes from the copy of PerSceneConstants in the game's global scene table (root table 1, entry 34), where the RT build already reads it, and from the composite pass's own tables after its draw.
- **Log:** `[G3] camera for thin glass: <source>`, with every constant buffer bound for the composite and the table 1 +34 entry. It's printed 3 times in game and then once a minute. The every-600-frames count of how often the old camera was from another frame stays.
- **Crash 21:38 (not ours):** the call stack was in `LSCNativeBridge.dll` (Transit Report's snapshot sampler) on the main thread, right after cancelling a load (`LoadPanel_CancelLoad`), reading a null pointer in game code. StarfieldRT.dll appears only in the module list.
- **To check in game:** turn the mouse at the same glass, then send the log.

## 0.5.83 - 2026-11-24

- **0.5.82 result:** the poster circle and the glass disc are fixed (confirmed).
- **Fix (likely): thin-glass reflections jumping and sliding against the mouse while turning.**
  - **Cause:** the thin-glass pass runs right after the game's transparency pass, but it took its camera (position and view matrices) from the main reflection pass. The game records its passes on several threads, so the glass pass was often recorded before that frame's reflection pass and used **last frame's camera** with this frame's depth. While you turn, the reflections were worked out for where you were looking a frame ago, so they lagged and jumped against the turn. The image capture that reflections reuse (0.4.4) had the same stale camera.
  - **Fix:** the glass pass and the capture take the camera bound by the transparency draws recorded just before them on the same command list, which is always this frame's.
  - **Log:** `[G3] this frame's camera for thin glass: found` at start, and every 600 frames a count of how often the old camera was from another frame (that's how often it jumped before).
- **To check in game:** turn the mouse while looking at thin glass (the same spot as the video): the reflection should stay put on the glass, with no jumping. Then send the log.

## 0.5.82 - 2026-11-24

- **Poster circle (RT lamps), second fix.**
  - **Test results:** it goes away with RT lamps off (Numpad 2), and GI (Numpad 1) makes no difference. Lamp probe at 13 m: both lamps blocked **0.20 m away by the poster itself** (the crosshair object, instance 43334). At 7 m they reach it.
  - **Cause:** 0.5.80's same-object rule was right, but a lamp shadow ray could step past its own surface only twice. A poster is several layers of one object (cover, frame, backing), so the third hit still cast the shadow.
  - **Fix:** up to 7 steps past the surface's own layers (8 tries).
- **Thin glass: a blank disc with no reflection when standing close to a pane.**
  - **Cause:** the glass pass ignored glass within 0.5 m of the camera, a leftover from before the player was tagged (it kept your own head from counting as glass). A flat pane cut by a 0.5 m sphere around the camera is a circle.
  - **Fix:** 5 cm. The camera ray skips the player anyway since 0.5.69.
- **Crash on 0.5.81 (21:19, GPU hang):** DRED shows only DLSS Frame Generation's pacer list and an internal DXGI list unfinished, none of ours. That's the same signature as every earlier hang with Frame Generation on (see 0.2.33). Six seconds before it, the game switched DLSS off on a second viewport (`slDLSSSetOptions viewport 2: mode 0`), most likely a menu or photo-mode transition.
- **To check in game:** the same poster from far and near with RT lamps on, and a pane of glass up close.

## 0.5.81 - 2026-11-24

- **Thin-glass reflections dimmer:** default `fThinGlassStrength` 3.0 -> 1.5 (1 = the reflectance of real glass).
- **New keys:** **Alt+Numpad +** / **Alt+Numpad -** make thin-glass reflections brighter or dimmer live (x1.25 / x0.8 per press). The HUD shows the value to put in the ini. The ini line in the plugin folder is replaced on each install, so tell me the value to make it the default.
- **0.5.80 result (poster circle):** not fixed yet. The test ran with every effect on (F8), and no probe was taken, so it isn't known yet whether lamps, GI or reflections darken it now.

## 0.5.80 - 2026-11-24

- **RT lamps: a poster in the Ryujin lobby still had the circle** (dark from far away, lit up close).
  - **Lamp probes:** the crosshair object is the poster (instance 43170) in both shots. At 10.2 m (dark) three lamps are blocked **0.13-0.14 m away by that same poster**. At 7.7 m they reach it.
  - **Why 0.5.78 missed it:** that fix only passes hits that lie flat against the surface. Here the poster's own frame or light-box sides were hit, and they face another way.
  - **Fix:** as the GI pass does since 0.5.46, a lamp shadow ray now also passes a hit within the self-gap (10 cm + 2% of the view distance) on **the same object** as the lit surface, whatever way it faces. To find that object, a camera ray is traced only when a shadow ray is blocked inside the gap, so the extra cost is small. The lamp probe uses the same test.
- **To check in game:** the same poster from far and near with RT lamps on: lit at every distance.

## 0.5.79 - 2026-11-24

- **0.5.78 result:** the RT lamp circle in the Ryujin lobby is gone (confirmed).
- **New: soft lamp shadows** (`fRTLampSoftness=0.25`, metres).
  - **Before:** every lamp shadow ray aimed at the lamp's exact centre, a point light, so lamp shadows had razor-sharp edges.
  - **Now:** each shadow ray aims at a random point on a disc of that radius around the lamp, different every frame and per lamp. Ray Reconstruction blends them into soft edges that are sharp where an object touches the surface and widen with distance, like real lamps.
  - 0 = hard shadows as before. Bigger = softer (0.1 for small bulbs, 0.5 for large panels). Read at start-up.
  - **Needs Ray Reconstruction on** (Ctrl+Numpad 0). With plain DLSS the edges look grainy instead of soft.
- **To check in game:** shadows of people, counters and chairs under the lobby lights: soft edges and no crawling grain. Compare with `fRTLampSoftness=0` if needed.

## 0.5.78 - 2026-11-24

- **RT lamps: the same circle as the reflections (Ryujin lobby video).** The red walls went dark beyond a circle around the camera.
  - **Lamp probes:** wall 14.8 m away (dark): two lamps blocked **0.14-0.16 m** from the wall point. Wall 9.3 m away (lit): the same kind of lamp reaches it.
  - **Cause:** the level-of-detail mismatch again. Past a distance the game draws a simpler wall, and our full-detail copy sits a little in front of it, so the lamp's shadow ray hit the copy's front face right away. Lamp shadows skipped such hits only from behind (0.5.45) or within 10 cm.
  - **Fix:** as for reflections in 0.5.76, lamp shadow rays now also skip a front-face hit within the self-gap (10 cm + 2% of the view distance) that lies parallel to the lit surface. Real blockers face another way and still cast shadows. The lamp probe (Numpad .) uses the same test.
- **To check in game:** the same walk in the Ryujin lobby with RT lamps on: the walls should stay lit at every distance, with no circle.

## 0.5.77 - 2026-11-24

- **0.5.76 result:** the reflection circle in the Ryujin lobby is gone (confirmed).
- **New: F8 switches every RT effect off and back on** (`iMasterToggleKey=119`).
  - **Off:** remembers what was on, then applies the baseline: RT reflections, GI, RT lamps, RT sun shadows, thin glass and the glass tweaks off, and Ray Reconstruction paused (plain DLSS). That's the game's own look.
  - **On:** restores exactly what was on before. If nothing was on when you pressed it, F8 turns everything on.
  - While off, it stays off through Gameplay Options changes and save loads.
  - Individual keys still work as before.
- **To check in game:** F8 back and forth in a few places (Neon, the Ryujin lobby, outdoors). Each press should switch the whole look at once, with a HUD message.

## 0.5.76 - 2026-11-24

- **0.5.75 result:** still a circle. The reflection-ray probe found the real cause:
  - wall spot 3.8 m away: the reflection ray hits the room behind, 14 m away, bright (HDR 9.9, 6.3, 4.7);
  - wall spot 12 m away: the reflection ray hits **the wall itself, 0.01 m from its start** (the same object), so it took the wall's own red colour and showed no reflection.
- **Cause: the level-of-detail mismatch, as with the earlier rings.** Past a distance the game draws a simpler wall model, while our RT copy is the full one. The reflection ray starts on the game's surface and runs straight into our copy. Since 0.5.33 reflections skipped such hits only when they hit the copy from behind; here they hit its front.
- **Fix:** reflection rays also skip a front-face hit within the self-gap (10 cm + 2% of the view distance) when it lies parallel to the surface they left (its own copy). Objects standing on the surface face another way and still reflect. Up to 3 such skips per ray.
- **Source view fix:** "hit right at its start" (magenta) is now checked before "colour from the screen" (green). A self-hit lands on its own on-screen pixel, so in 0.5.73 it showed green and hid this.
- 0.5.75's per-object colour matching stays (Alt+Numpad 5).
- **To check in game (Ryujin lobby):** walk toward the back wall and away. The reflections should cover the whole wall at every distance. With Alt+Numpad 0 there should be no magenta on the walls.

## 0.5.75 - 2026-11-24

- **Fix (likely): reflections growing in a circle as you move (Ryujin lobby).**
  - **Not the level-of-detail problem** behind the earlier rings: the source view (0.5.73) showed no magenta or yellow (no hits on our own copy, no distance mismatches). Outside the circle, every ray reached the room behind you, but it was coloured by **our shading**, which is duller than the game's lit image used inside the circle. The circle is the border between the two.
  - **Change:** where a reflected point is on screen, the reflection pass also shades it our way (1 pixel in 16 per frame) and learns, per object, the ratio between the game's colour and ours. Where an object is off screen, our shading is multiplied by its learned ratio. Both sides now match, so the bright reflections no longer stop at a moving edge. Objects never seen on screen keep plain RT shading.
  - Smoke-covered pixels don't teach (their colour is the smoke's).
  - `bReflectionMatchScreen=1`; **Alt+Numpad 5** toggles it to compare.
- 0.5.74's reflection-ray probe (Numpad .) is included.
- **To check in game (Ryujin lobby, source view off):** walk toward the back wall and away. The reflections should stay bright at every distance, with no moving edge. Compare with Alt+Numpad 5 off. Objects need a second on screen to learn.

## 0.5.74 - 2026-11-24

- **0.5.73 result:** outside the arch, the source view is solid **red**: the reflected point was off screen last frame. A wall you face reflects what's behind you, so from far away its rays hit ceiling behind you, which has no on-screen colour to take. Walking closer, the upper wall reflects ceiling that's on screen. So the circle is where reflections switch from the game's image to our own shading, and our shading is too dull there.
- **Probes on the ceiling:** a bright light panel is a standard material with its glow filled in: screen radiance 154, glow level +3.5 EV, warm tint (1.0, 0.82, 0.62), glow texture = its albedo. Our shading gives it about 70% of the screen value with the right tint. So the panels aren't the whole story.
- **New probe:** Numpad . now also follows the **reflection ray from the crosshair** (`[R] reflection ray at the crosshair`). It logs what the ray hits (object, distance, material), whether its colour came from the screen or our shading and why, and both values side by side: the colour used, our own shading of the same hit, and the game's probe value. On a green pixel that's a direct measurement of how far off our shading is.
- **To check in game (Ryujin lobby, source view off):**
  1. Crosshair on the back wall from close up (where it reflected well), Numpad .
  2. Back away until the same spot loses its reflection, Numpad . again.
  3. Send the log.

## 0.5.73 - 2026-11-24

- **0.5.72 result (source view video, Ryujin lobby):** on the back wall the boundary between green (colour from the screen) and red (ray traced by us) is an arch centred on the player. Inside it, closer, reflections come from the screen; outside it, farther, they're ray traced, and the arch moves as you walk. That's the "growing circle".
- **Likely cause, the same family as the earlier rings** (copper columns 0.5.33, the RT lamp ring 0.5.44-45, the GI ceiling ring 0.5.46-47, the sun-shadow building flips 0.5.58-60): past a set distance the game draws a simpler model of each object (its level of detail), while our RT copy is always the full model.
- **The source view now says why a reflection is ray traced:**
  - **red:** the hit point was off screen last frame;
  - **yellow:** on screen, but the distance there didn't match (another surface in front, or the full model vs the game's simpler distant one);
  - **white:** on screen, but marked unusable there (smoke, glass, metal);
  - **magenta:** the ray hit something within 50 cm + 3% of the view distance of where it started (likely the wall's own full-detail copy);
  - **green** (from the screen) and **blue** (no hit) as before.
- **To check in game:** the same walk in the Ryujin lobby with Alt+Numpad 0 on, as a video or two screenshots (near and far). The colour outside the arch says which fix it needs.

## 0.5.72 - 2026-11-24

- **0.5.71 result (Ryujin lobby video):** on the same red back wall, reflections cover more of the wall as you walk in and shrink as you back away, with an edge that moves with you ("grows in a circle"). Reflection strength (Ctrl+Numpad *) doesn't change that. Probes on one wall panel at 13 m and 10 m: RT applied both times, roughness 0.06-0.11, head-on view, weight 0.020. So it isn't a range limit, roughness or viewing angle.
- **Likely cause:** a reflected point that was on screen last frame takes its colour from last frame's image, which shows the ceiling light panels bright, as the game draws them. A point that wasn't on screen is shaded by our own lighting, which gets those panels' glow wrong, so they come out dull. From far back, the wall reflects ceiling behind you (off screen); walking in, it reflects ceiling that's on screen, so the bright area spreads.
- **New debug view, Alt+Numpad 0 (reflection source view):** colours every reflection by where its colour came from. **Green** = last frame's image (on screen), **red** = ray traced and shaded by us (off screen), **blue** = no hit (the game's probe). Shown at full strength, so it's visible on any surface. Press again to turn it off.
- **To check in game (Ryujin lobby):**
  1. Alt+Numpad 0, then walk toward and away from the wall: does the edge of the bright reflections follow the green/red border?
  2. Turn it off, put the crosshair on one of the ceiling light panels, press Numpad . once, then send the log (it shows that panel's material, to fix its glow in our shading).

## 0.5.71 - 2026-11-24

- **Requests after 0.5.70:** stronger glass blur, smoke visible in reflections, and longer ranges on Ultra so nothing pops in.
- **Found in the log:** the ini says Ultra, but the save's **Gameplay Options set Starfield RT to High** after loading (the menu overrides the ini). The last tests ran on High: 64 characters within 40 m, 3 km reflection rays, 4 lamp shadows, GI with a quarter of the rays. Set **Settings > Gameplay > Starfield RT** to Ultra (both options).
- **Glass blur:** default `fThinGlassBlurPx` 4 -> 8.
- **Smoke in reflections** (`bSmokeInReflections=1`, Alt+Numpad 7 toggles):
  - **Why it was missing:** reflections take the colour of on-screen points from last frame's image, but every pixel covered by smoke or glass was marked unusable there (since 0.3.9, against GI flicker). Those points were ray traced instead, and ray tracing doesn't see smoke.
  - **Change:** smoke-covered pixels keep their distance in that image, in a form GI still rejects. Reflections (floors and thin glass) now use them, so a reflected point that was on screen behind smoke takes its smoky colour. Points that weren't on screen are still ray traced without smoke. GI is unchanged.
  - Metal pixels stay unusable (0.5.29, mirror tunnels).
- **Ultra ranges extended:**
  - **RT scene range:** objects farther than 1.5 km were left out of the RT scene on every level, so distant buildings popped into reflections and shadows. Now 10 km on Ultra (3 km High, 1.5 km Medium, 1 km Low).
  - **Reflection rays** 6 km -> 10 km; **sun shadow rays** 3 km -> 10 km.
  - **Characters** within 100 m (was 60). The cap stays at 128 skinned pieces (about 25 characters), nearest first.
  - **Lamp shadows:** the 16 strongest lamps per pixel get a shadow (was 8), so fewer lamps switch their shadow on and off as you move.
  - The log's quality line now also prints the RT scene range.
- **Cost:** more GPU time on Ultra (bigger scene, more lamp shadow rays). Check the `PERF` lines if it gets slow.
- **Not ours:** lamps themselves can still pop in at the game's own light fade distance. RT only lights the lamps the game hands over.
- **To check in game:** set Ultra in Gameplay Options first. Then the Neon smoke reflected in floors and windows (compare with Alt+Numpad 7), the glass blur, and distant buildings in reflections and shadows.

## 0.5.70 - 2026-11-24

- **0.5.69 result:** the gun and arms are gone from reflections (confirmed). Next request: thin-glass reflections need some blur and the right colours; people in them look undetailed.
- **Why they looked wrong:** the thin-glass pass shaded everything it reflected with our own simple lighting (texture colour, sun, the game's lamps and a flat ambient). It never used the game's own image, as the main reflections have since 0.4.4. People came out flat and plain, and rays that hit nothing got a bluish "sky" colour, which tinted windows indoors.
- **Changes:**
  - **Real colours from the screen:** a reflected point that was visible on screen last frame now takes its colour from last frame's image, so people, signs and lit walls look as the game draws them, with full detail. Points that weren't on screen, or that were behind smoke, glass or metal, are ray traced as before.
  - **Blur:** a new pass softens the reflection across each pane (`fThinGlassBlurPx=4.0`, radius in pixels at 1440p; 0 = mirror sharp). It only mixes glass pixels at a similar distance, so it never smears across a pane's edge.
  - **Misses:** a softer, less blue colour.
- **Known limit:** in thick smoke (Neon), most of the screen is marked unusable in last frame's image, so reflections there are still mostly ray traced.
- **To check in game:** shop windows with people behind you (in view and out of view), windows in a brightly lit area, and the blur. If the blur is too strong or too weak, change `fThinGlassBlurPx` (it's read at start-up).

## 0.5.69 - 2026-11-24

- **0.5.68 result:** arms and gun still show in window reflections.
- **Real cause:** the thin-glass pass first traces a ray from the camera to find the window. That ray treated everything as solid, so the player tag (0.5.65, 0.5.68) had no effect on it. Parts of the first-person gun (lens, sights) and arms gear count as glass. Our copy of them sits at their real world position, not where the game draws the first-person view, which uses its own field of view. The camera ray found "glass" on that copy and painted a reflection there: a pale, arms-and-gun-shaped copy next to the real ones. It went away with Numpad 4 because that's the pass that drew it. It was never a reflection of the gun in the window.
- **Fix:** the glass pass's camera ray (and its untagged-window search) now skips the player's body, as reflection, GI and shadow rays already do.
- The 0.5.68 weapon tagging stays: it keeps the gun out of floor reflections and shadows.
- **To check in game:** the same window in first person: no pale arms or gun copy. Real windows still reflect.

## 0.5.68 - 2026-11-24

- **0.5.67 result:** the ghost gun in the window goes away with thin glass off (Numpad 4), and Numpad 9 doesn't change it. So it's our RT reflection, not the game's glass effect. The near-camera probe shows the weapon is about 50 separate parts (records 63136-63185 in Neon), listed right next to your own body's parts (63159-63165). Most were already tagged as the player. A few weren't: their size bound reads 2 m or 10 m (it's a loose upper bound, not the real size), and 0.5.65 only tagged objects under 1.5 m.
- **Fix:** a rigid object within 1.5 m of the camera now counts as the player if one of your body's skinned parts is within 64 places of it in the game's list (and stands where the player does). There's no size limit any more. Walls and props are thousands of places away in that list, so they aren't caught. In third person the camera is metres behind your character, so your weapon still reflects and casts shadows.
- **To check in game:** the same window in first person: no ghost gun. Third person and photo mode: your character and gun still reflect.

## 0.5.67 - 2026-11-24

- **0.5.66 result:** 295 objects within 3 m of the camera. The first 48 listed were all wall and building pieces (the nearest 1.18 m away), nothing gun-sized and nothing tagged as the player. So the gun was either cut off the list, or it isn't in the RT scene at all.
- **Probe narrowed to 1.5 m,** so the list can't fill up with walls. If it lists nothing small near the camera, the gun isn't in our scene, and the ghost in the window is the game's own see-through-glass effect (it draws what's behind glass from a copy of the screen that includes your gun), not an RT reflection.
- **To check in game,** in front of the window with the ghost gun:
  1. Numpad 4 (thin-glass reflections) off and on: does the ghost go away?
  2. Numpad 9 (view through glass sharp/blurred): does the ghost change or go away?
  3. Numpad . once, then send the log.

## 0.5.66 - 2026-11-24

- **0.5.65 result:** the first-person gun still shows in reflections (a pale, see-through copy in a Neon window). The arms are gone from that screenshot.
- **New probe:** Numpad . now also lists every object within 3 m of the camera (`[near]` lines): record and mesh, bones, whether it's in the RT scene, opaque or not, whether it's tagged as the player, whether the game skinned it this frame, its size, distance, offset from the camera and rotation. This shows exactly how the gun is stored, instead of guessing a third rule. No behaviour change otherwise.
- **To check in game:** first person, gun out, standing still in front of the same window. Press Numpad . once, then send the log.

## 0.5.65 - 2026-11-24

- **0.5.64 result: the Neon smoke flicker on thin glass is gone** (confirmed in game). The thin-glass issue parked since 0.3.10 is closed.
- **Fix: your first-person arms and gun no longer show in reflections.**
  - **Cause:** the RT scene recognised the player's body (to leave it out of reflections, GI, glass and shadows) only as a skinned character standing right under the camera. The first-person arms are a separate, tilted rig anchored elsewhere, so that test missed them. The weapon isn't skinned at all, so it was never recognised.
  - **Change:** two more things now count as the player:
    - a tilted character rig that the game draws, within 2.5 m of the camera (in first person, that's the arms; NPCs always stand upright);
    - a small rigid object (under 1.5 m half-size) whose centre is within 1.5 m of the camera (the held weapon).
  - Like the rest of the first-person body, they're skipped by reflection, GI and glass rays and cast no sun or lamp shadow.
  - In third person the camera is metres behind your character, so your character and weapon still show in reflections and cast shadows.
  - **Possible side effect:** a small prop within 1.5 m of the camera (on a shelf you're pressed against) drops out of reflections and shadows while you're that close.
- **To check in game:** first person over a reflective floor or window: no arms or gun in the reflection. Third person: your character and weapon still reflect and cast shadows. Photo mode too.

## 0.5.64 - 2026-11-24

- **0.5.63 result:** still flickering, and the smoke filter (Alt+Numpad 4) made no difference. The debug-view video shows why. Dense smoke covers almost every pixel (magenta), so every object scores as "drawn see-through" and the filter can't tell anything apart. The flicker is **yellow patches popping on and off from frame to frame**, on the overhead truss and on the floor far down the walkway. They're see-through layers (trim, decals and the like) that lie **on** solid surfaces. Our RT copy sits a few centimetres off the drawn surface, so with the game's jittered depth they landed just in front of the cut-off in some frames and not in others.
- **Fix:** a glass hit must now stand clearly off the surface behind it: at least 15 cm + 3% of the distance in front of it, and 20 cm off its plane. The untagged-window search has had this test since 0.3.10; tagged glass never had it. Real windows (shop fronts, canopies) stand well off what's behind them.
- The smoke filter stays in. It still removes things only smoke covers where the smoke is thin.
- **Trade-off:** glass mounted flat against a wall (a glass cover on a poster) gets no RT reflection.
- **To check in game (Neon walkway, Numpad 0 on, Numpad 4 on):** the flicker should be gone. In the Numpad 4 debug view, no yellow or white patches should pop on and off on the truss or the floor. Shop windows should still be yellow or white.

## 0.5.63 - 2026-11-24

- **0.5.62 result:** the pass ran this time. Numpad . (the probe) in the middle of the Neon smoke hit **the smoke plume itself**: instance 18536, LOD dword 0x3a307, **material family 7** (the family of effects and glowing panels), not tagged as glass, found by the untagged-window search 3.5 m away. Its glass score was 0.59, then 0.37: the plume covers itself with its own see-through layer, so it scored just around the 0.5 threshold and still got a partial reflection that faded in and out.
- **Fix:**
  - The untagged-window search now skips family 7. The smoke plume is one, and glowing panels are solid surfaces that already get the main reflection. Real shop windows are another family (13).
  - The score threshold is stricter: no reflection below 0.7, full from 0.9 (was 0.5 / 0.85). Smoke scored up to 0.6, and real panes score close to 1.
- **To check in game (Neon walkway, Numpad 0 on, Numpad 4 on):** the smoke should show no reflection and the floor under it should be steady. Numpad 4's debug view over the smoke should be magenta or blue, not yellow, white or green. Shop windows should still reflect.

## 0.5.62 - 2026-11-24

- **0.5.61 result: "no difference".** The log shows why. The thin-glass pass ran for only about 2 seconds of the test, because it needs RT reflections (Numpad 0). The baseline had them off, and Numpad 0 was cycled on, through the debug views, and back off. For the rest of the test, the log said `thin-glass reflections: skipped: RT reflections off`. The HUD still said "on", so neither the fix nor the old flicker was showing.
- **Change:** Numpad 4 now says "on, but NOT RUNNING: turn RT reflections on (Numpad 0)" when reflections are off.
- **The same log ended in a GPU reset** (device removed, reason "reset", no page fault, no breadcrumbs), about 30 s after the last key press, with reflections, GI, lamps and thin glass all off. If the game didn't crash for you there (for example you quit), it was most likely the shutdown. If it did crash, say so.
- **To check in game (Neon walkway):** Numpad 0 until the HUD says reflections are on (RT), then Numpad 4 to "on". Compare Alt+Numpad 4 (smoke filter) on and off. Then use the Numpad 4 debug colours on the smoke.

## 0.5.61 - 2026-11-24

- **Fix (likely): thin-glass reflections flickering with the smoke in Neon** (the issue parked since 0.3.10).
  - **Cause:** the thin-glass pass decided per pixel, from the game's transparency coverage, whether glass was there and how strongly to reflect. Smoke adds to that coverage and changes every frame. So:
    - objects tagged as see-through that the game doesn't actually draw as glass (light beams, glow cards and similar effect meshes) got a mirror reflection wherever smoke drifted over them, and lost it when the smoke moved on;
    - real panes with a faint coverage of their own got a strength that rose and fell with the smoke density (the 0.4.10 fade made this worse, not better).
  - **Change, a smoke filter:** each object now earns a **glass score**: the share of its pixels where the game draws something see-through. It is sampled on a rotating 1-in-16 pixel pattern whether or not anything is drawn there, and averaged over time (rises in about half a second, drops faster). A real pane is always covered, so it scores about 1 and reflects at a steady strength, however much smoke is around. A mesh that only smoke drifts over scores low and gets no reflection. The reflection no longer fades with the coverage of each pixel.
  - Panes fade in over about half a second the first time they're seen after a load.
  - **The untagged-window search is on again by default** (`bThinGlassUntaggedFallback=1`). It was turned off in 0.5.13 because it flickered under smoke; the score now filters it too, so Neon's shop windows whose material isn't tagged as glass reflect again. Ctrl+Numpad 4 still toggles it.
  - **New key:** Alt+Numpad 4 switches the smoke filter on and off live (`bThinGlassSmokeFilter=1`), for comparison.
  - **Debug view** (Numpad 4 to "debug colours"): new **orange** = an object that only smoke covers, filtered out. **Green** = a reflecting object still fading in. Yellow and white are reflected glass, as before.
  - **F8** now logs the glass score of the object at the crosshair (`[G3] F8 glass score`).
- **To check in game, in the Neon walkway with the smoke:**
  1. Thin glass on (Numpad 4): the floor and walls under the smoke should be steady.
  2. Alt+Numpad 4 (filter off) should bring the flicker back. That shows the filter is what fixed it.
  3. Shop windows, including the ones that needed the untagged search, should still reflect after half a second.
  4. If something still flickers: Numpad 4 to the debug view on that spot, F8 with the crosshair on it, and send the log and a screenshot.

## 0.5.60 - 2026-11-23

- **0.5.59 result (Jemison walkway):** the big dark area looked the same with RT shadows, with soft shadows off, and in the hit-distance view. It disappeared only in the alignment view, which doesn't trace toward the sun. So real sun rays were blocked there, though the probe said they weren't.
- **Cause:** the sun shadow pass traced against **every** object type (mask 0xFF since 0.2.50), including the invisible **effect volumes**: haze and smoke boxes and other effect meshes (LOD material 0, tagged mask 0x04 since 0.2.64), such as the 16 m boxes over the plaza. They cast solid shadows where nothing visible stood. The 0.5.58 probe traced solid geometry only, so it missed them. They also confused the shadow pass's camera ray, which picks where each sun ray starts.
- **Fix:** sun shadows (and the probe) skip effect volumes. They still hit solid geometry and glass, as in 0.2.50.
- **To check in game:**
  - The same walkway, Numpad 3 on: the big shadow should be gone.
  - Glass canopies and windows still cast their usual shadows.
  - Look for any place where a real object lost its shadow (that would mean something solid was tagged as an effect).
## 0.5.59 - 2026-11-23

- **0.5.58 result:** a large dark area on the Jemison walkway is there only with RT sun shadows on, yet the probe at its centre says the sun ray reaches the sun, and no blocker shows up anywhere on screen. So the real shadow pass and the probe disagree there.
- **New: Ctrl+Numpad 3 cycles the sun shadow debug views** (Numpad 3 alone still switches RT shadows on/off):
  - **Hit distance** (mode 4): the real shadow ray toward the sun's centre, at full resolution and unfiltered. It now uses the same alpha test and self-hit skips as the shadow pass (it used a simpler trace before). Black = blocked right at the surface, grey = blocked far away, normal = unblocked.
  - **Alignment** (mode 5): rays from the camera must hit our scene where the game's depth says. Sunlit surfaces go dark where they don't.
  - Then back to normal RT shadows.
- **To check in game:** at the dark walkway, take a screenshot of each of these:
  1. Normal RT shadows.
  2. Numpad / (soft shadows off: shows whether the soft-shadow filter spreads the dark).
  3. Numpad / again, then Ctrl+Numpad 3 (hit distance).
  4. Ctrl+Numpad 3 again (alignment).
## 0.5.58 - 2026-11-23

- **0.5.57 result:** the square shadow on the wall is gone.
- **New report:** standing in the open plaza with the sun ahead, the player stands in a large shadow with nothing visible to cast it.
- **Sun probe now matches the shadow pass:** it traces solid geometry only. It used to include the invisible effect volumes (big boxes over the plaza) and glass, which the real shadow pass ignores, so recent probes reported false blockers. The centre line now also says whether the blocker was hit on its front or back face, its size, and where it's placed.
- **To check in game:** aim at the shadowed ground in front of you (where nothing should shadow it), press Numpad ., quit, say done.
## 0.5.57 - 2026-11-23

- **0.5.56 result:** the plant shadows are steadier, but a big **square shadow** on a Jemison wall appeared from about 10 m away and vanished up close.
- **Probes (lit at 7.7 m, shadowed at 13.5 m):** both sun rays were blocked from behind by the same object, a **coarse 476-triangle, 64 m shell of the building** (an earlier session showed it at the same spot under another instance number). The game keeps it for distance views and doesn't draw it up close, but it's in our RT scene, 0-70 cm in front of the real wall.
  - Close up, the shadow pass's camera ray finds that shell within tolerance and starts the sun ray outside it, so the wall is lit.
  - Farther away, the shell is more than 5% of the distance in front of the wall, so the start point falls back to the wall's depth, inside the shell. The sun ray hits the shell's inner face (0.42 m at 13.5 m, past the 0.37 m skip gap), so the wall is shadowed. The shell's flat panels make it a square.
- **Fix:** when the start point is that depth fallback (no RT surface found near it), back-face hits within 6% of the view distance are skipped (still 2% when the start is on an RT surface).
- **To check in game:** the same wall, walking from close to 15-20 m away with Numpad 3 on. No square should appear. Shadows under overhangs should look the same.
## 0.5.56 - 2026-11-23

- **RT sun shadows crawled as the camera moved** (tree and vine shadows on the Jemison plaza walls swelled and shrank every few frames). Before tracing toward the sun, the shadow pass shoots a ray from the camera to find exactly where to start. That ray treated leaf and vine cards as solid rectangles, so where a hanging vine card was in front of the wall (within 5% of the view distance), the sun ray started from the card instead of the wall. Which pixels hit a card changes with every camera move, so the shadows crawled.
- **Fix:** the camera ray now runs the same leaf cut-out (alpha) test as the sun rays and passes through the see-through parts of cards. It also skips the first-person player body.
- **Probe note:** the sun probe (Numpad .) traces with every instance type, including invisible effect volumes (big 16 m boxes over the plaza, material 0) that the real shadow pass ignores. Its "primary ray IN FRONT" and SUSPECT lists are skewed by them.
- **To check in game:** the same wall with the hanging vines, Numpad 3 on. Walk and turn; the shadows should hold still. Also check other walls behind trees.
## 0.5.55 - 2026-11-23

- **RT sun shadows: distant buildings flipped between fully shadowed and fully lit as you moved.** It's the level-of-detail mismatch again (copper columns 0.5.33, lamps 0.5.45, GI 0.5.46). Far buildings are drawn with a simpler mesh than our RT copy, metres apart at a few hundred metres. The sun ray could start inside our copy and hit its inner face, so the whole building went dark, and it switched back whenever the game changed detail levels.
- **Fix:** sun shadow rays now pass through a back-face hit within 10 cm + 2% of the view distance (up to twice). Real blockers are always entered through a front face first, so they still cast shadows.
- **To check in game:** Jemison plaza in daylight with Numpad 3 on. Walk and turn while watching the far buildings. They should stay steadily lit (or shadowed where something really blocks the sun). Also check that close shadows (people, trees, railings) are unchanged.
## 0.5.54 - 2026-11-23

- **0.5.53 probe result:** the first-person suit (four parts, no head) differs from everything else in two ways.
  - **The game doesn't GPU-skin it in third person** (LOD position handle bit 30 clear: `1b1580d0` and similar). In first person it does (`c1bfc003`). The game only skins what it draws. Our fallback skinning of its half-animated skeleton gave the T-pose arms. This alone isn't enough, because some NPC head parts (eyes, hair) never have that bit set even on screen.
  - **Its root is tilted** (rotation quaternion y about -0.17, the same in both views). Every character, and the third-person body, stands upright (yaw only).
- **Fix:** a character copy that's tilted, not skinned by the game this frame, and within 6 m of the camera is left out of the RT scene. In first person the arms are drawn, so they stay (they already cast no shadow, 0.5.49, and reflections skip them). The 0.5.51 frozen-pose rule stays as a fallback.
- **To check in game:**
  - Third person: walk, run and stand still. Only your real shadow should show, with no T-pose arms.
  - Photo mode: shadows intact.
  - Dead or lying NPCs (tilted) off screen nearby may drop out of shadows; tell me if you notice.
## 0.5.53 - 2026-11-23

- **0.5.52 probe result (third vs. first person):** the player shows up as two groups of character copies at the same spot.
  - Group A has a face (15,370 vertices, like an NPC face), hair and eyes. That's the third-person body.
  - Group B (a 29,572-vertex suit body, gloves and a pack) has **no head**, the way first-person models are built. That's the first-person body, the T-pose arms in third person.
  - The game's records couldn't be read from the CPU (GPU-only memory), so there's nothing yet to tell the groups apart in code. Instance numbers change from session to session.
- **Probe now copies the records on the GPU:** Numpad . logs each character copy's instance record and LOD record as `[skin]` lines, plus its frozen counter, sorted by distance.
- **To check in game:** same as before. In third person walk so the T-pose arms show and press Numpad .; in first person press Numpad . again; quit, say done.
## 0.5.52 - 2026-11-23

- **0.5.51 result:** the T-pose arms reappear in third person whenever you move and go away when you stop. So the unused first-person body isn't frozen: the game animates part of its skeleton while you move (the movement cycle) without drawing it, and the "frozen pose" test can't catch it. The 0.5.51 rule stays for now (it still hides it while you stand still, and photo mode is unaffected).
- **New probe:** Numpad . now also logs every character copy in the RT scene as `[skin]` lines: instance, mesh, vertex and triangle counts, distance, and the game's raw instance record (96 B) and LOD record (80 B). Comparing a probe in first person with one in third person should show what tells the two player bodies apart (a different mesh, a flag, or a position offset).
- **To check in game:** in third person, walk so the T-pose arms show, press Numpad .; switch to first person, press Numpad . again; quit, say done.
## 0.5.51 - 2026-11-23

- **Photo mode keeps everyone's RT shadows.** 0.5.50 dropped any character near the camera whose pose hadn't changed for a second, and in photo mode or while paused that's everyone. Now a character copy is dropped only once it has stayed frozen through 60 frames in which *other* characters were moving. The unused player body meets that during normal play, because your real body keeps animating. When the whole game stops, nothing is moving, so nothing new gets dropped, and the unused body (already dropped before photo mode) stays out. A dropped body comes back as soon as its own pose changes (for example when you switch views).
- **To check in game:**
  - Third person: one shadow only.
  - Photo mode with NPCs nearby: your character and the NPCs keep their RT shadows, and no T-pose shadow appears. Change poses a few times.
  - First/third person switching still works.
## 0.5.50 - 2026-11-23

- **Third person: a second, T-pose shadow next to your real one.** The player has two bodies, one for first person and one for third. The game animates only the one in use; the other stays in its bind pose (T-pose), and our RT copy of it cast a shadow. 0.5.49 only caught this in first person, where the camera is right on top of the body.
- **Fix:** the skinning pass now tracks each character's pose. A character copy within 6 m of the camera whose pose hasn't changed for 60 frames (about a second) is left out of the RT scene. Animated characters (including idle NPCs, who always breathe and sway) are unaffected.
- **Side effect to watch:** while the game is paused or in photo mode, characters within 6 m stop animating too, so after a second they lose their RT shadows and reflections until animation resumes.
- **To check in game:** third person with Numpad 2 on. Only your real shadow should remain, in both first and third person. Switch views a few times. Also check that NPCs standing close to you still cast shadows.
## 0.5.49 - 2026-11-23

- **No more player shadow in first person.** Our RT copy of the player's body isn't animated in first person (the game doesn't animate it), so with RT lamps it cast a T-pose shadow, for example on the counters in the UC Security office. Reflections and GI already ignored this body (0.4.4). Now RT lamp shadows, RT sun shadows, and the shadow tests at reflection and GI hits ignore it too.
- In third person the player isn't tagged this way (the camera isn't over the body), so your character still casts RT shadows there.
- **To check in game:** stand near a counter or wall with Numpad 2 on. Your T-pose shadow should be gone. NPCs should still cast shadows.
## 0.5.48 - 2026-11-23

- **0.5.47 GI probe result (lobby chair back, 5.3 m):** the normal was fine (facing the camera, so the 0.5.47 flip wasn't the cause). But **all 15 GI rays hit the chair itself (instance 694), 1-6 cm out, on a front face**. The camera ray reaches our RT chair at 5.27 m, while the game draws it at 5.34 m. At that distance the game draws a simpler chair, and our RT copy sits about 7 cm in front of it. So GI rays started inside our copy, and the chair shaded itself dark. Up close both are the same mesh, which is why the spot vanished up close and grew with distance.
- **Fix:** GI rays now also skip hits on the pixel's own object within 10 cm + 2% of the view distance, front face or back. The object is the one the existing camera ray (used for the character check) finds. The probe uses the same rule.
- **To check in game:** the lobby chairs with Numpad 1 on, walking from close to far. The spots should be gone at every distance.
## 0.5.47 - 2026-11-23

- **RT GI: dark, frame-shaped patterns on the lobby chair backs**, worse from a distance. They look like the chair's own frame behind the backrest. Most likely the screen's normal for these thin two-sided backrests points away from the camera, so GI rays went out of the back of the chair into its frame. GI now turns any normal that clearly faces away from the camera back toward it.
- **Probe (Numpad .) now also traces 15 GI rays from the centre surface** and logs them as `[gi]` lines: whether the normal faced away, and for each ray whether it hit, how far, front or back face, and how many self-hits it skipped. If the chair patterns remain, this shows why.
- **To check in game:** the lobby chairs with Numpad 1 on, near and far. If the patterns are still there, aim at one, press Numpad ., quit, say done.
## 0.5.46 - 2026-11-23

- **RT GI: stepped dark shadow on the lobby ceiling that grew in a ring as you moved.** It's the level-of-detail problem again (copper columns in 0.5.33, lamps in 0.5.45). Past the game's detail switch, GI rays started inside our RT copy of the surface and hit its inner face, so the surface counted as enclosed and went dark. GI rays now pass through a back-face hit within 10 cm + 2% of the view distance.
- **RT GI made light surfaces (the lobby chairs) glow nearly white.** The default `fGIStrength` was 2, twice the physically based value, left over from early testing. It's now 1. If an old ini still has `fGIStrength=2.0`, the new ini replaces it.
- **Not fixed yet:** the dark oval spots on the chair backs. The likely candidates are an object our RT scene has that the game doesn't draw there (for example a leftover character copy), or more self-hits. If they're still there in 0.5.46, a probe will tell.
- **To check in game:** the lobby ceiling and the chairs with Numpad 1 on vs. off, walking back and forth.
## 0.5.45 - 2026-11-23

- **RT lamps lit the posters only inside a circle around the player.** It's the same level-of-detail problem as the copper columns in 0.5.33. Past a certain distance the game draws a simpler wall mesh than our RT copy. The lamp's shadow ray then started inside our copy and hit its inner face, so every lamp counted as blocked. Moving forward and back made the light switch on and off at that ring.
- **Fix:** lamp shadow rays now pass through a back-face hit within 10 cm + 2% of the view distance, the same rule reflections use. Up to two such layers are skipped (the 0.5.44 cover-layer skip counts as one).
- **To check in game:** walk toward and away from the Jemison posters with Numpad 2 on. The light should stay steady with no ring. Check some curved or rounded walls from a distance too.
## 0.5.44 - 2026-11-23

- **0.5.43 probe result (Jemison poster):** every lamp's shadow ray was **blocked 0-1 cm from the poster** by the same object (instance 1445, material 260). That's a layer lying right on the poster face (a cover sheet or frame layer the game counts as solid). It hid all six lamps aimed at the poster, so posters went dark with RT lamps on. The 0.5.42 change couldn't help because it only trims the lamp end of the ray.
- **Fix:** an RT lamp shadow ray now passes through the first thing it hits if that's within 10 cm of the lit surface, and keeps going. Anything further along still blocks, so people, furniture and pillars still cast RT shadows. The probe (Numpad .) uses the same rule.
- **To check in game:** the Jemison posters with Numpad 2 on vs. off. They should now be about as bright as with the game's lamps. Also check that small objects sitting right on walls or floors still look grounded (very close contact shadows from lamps are what this gives up).
## 0.5.43 - 2026-11-22

- **Numpad 0 switching RT reflections on now also turns on DLSS Ray Reconstruction**, since RR is what settles their noise. The HUD says "RT reflections ON + DLSS Ray Reconstruction on". Switching reflections off leaves RR as it is. Ctrl+Numpad 0 still toggles RR on its own.

## 0.5.42 - 2026-11-22

- **0.5.41 probe result (UC Security lobby poster):**
  - The two nearby spots (#29, #40) reach the poster.
  - The poster's main light is **#21** (brightness 8778, 7.1 m away, aimed at it), more than all the others put together. Its shadow ray was **blocked by instance 298 (material 23) 1.8 m from the lamp**, a quarter of the way. The game shadows #21 too, yet lights the poster, so its shadow map doesn't contain that trim.
- **Cause:** shadow maps lose thin occluders close to the light (near plane, bias, resolution). Our exact shadow rays keep them, so rooms come out darker than the game designed them.
- **Fix:** lamp shadow rays ignore anything in the last 30% of the way to the lamp, and always within the lamp's near offset + 10 cm (at least 30 cm). Occluders nearer the lit surface (people, furniture, pillars) still cast RT shadows.
- **To check in game:** the same posters and lobby with Numpad 2. If shadows near lamps now look too weak, say so; the 30% is easy to tune.

## 0.5.41 - 2026-11-22

- **0.5.40 probe result (poster centre in the UC Security lobby):** 13 spot lamps within 8 m.
  - We re-extracted and re-read the game's tiled-lighting shader (PSO 11809). Its spot formula matches ours exactly: near term, falloff, inverse square and cone. The projector lamps (flag bit 21) are also cut by the cone before the projector is applied. So for every lamp the game's attenuation equals ours.
  - The strip light right above the poster gives zero at its centre in the game too.
  - Two lamps clearly light the poster: #32 (2.45 m) and #42 (4.05 m, almost head-on). Both are game-shadowed, so they get our shadow ray.
- **Probe now includes each lamp's shadow ray:** whether it reaches the lamp, or what blocked it, at what distance (instance and material). If #32/#42 show BLOCKED, the blocker is what darkens the posters: a light cover, a glow mesh, or other geometry the game's shadow maps don't include.
- **To check in game:** the same probe at the same poster: RT reflections and RT lamps on, Numpad . at the poster centre, quit, done.

## 0.5.40 - 2026-11-22

- **0.5.39 result:** the posters are still unlit with RT lamps on. The light pool from each strip light above them is missing entirely, so shadows aren't the cause: the light itself doesn't reach them in our lamp formula.
- **New: lamp probe.** With RT reflections on (Numpad 0), press **Numpad .** while looking at a surface. For every lamp within 8 m of it (box lights at any distance), the log lists:
  - the lamp's type and flags;
  - its light channels, compared with the surface's;
  - distance, attenuation and N·L, using the same formula as our RT lamp pass;
  - brightness, and the angle to its axis;
  - its falloff, near offset, cone and box sizes;
  - whether the game shadows it.
  This shows which term zeroes the poster lights. Lines start with `[lamps]`.
- **To check in game:** look straight at the middle of a poster with Numpad 0 (reflections) on, press Numpad ., wait a second, quit and say done.

## 0.5.39 - 2026-11-22

- **0.5.38 result:** the lobby is still dark, so unshadowed fill lamps weren't the main cause.
- **Tester clue:** the posters have strip lights right above them that should be lighting them.
- **Cause (likely):** those lights are spotlights the game does shadow, so they get an RT shadow ray. The ray went from the poster toward the light and stopped 10 cm short of it. The light point sits inside its fixture, behind the cover, so the ray hit the lamp's own housing, and the poster counted as shadowed from its own light. Ceiling lamps in recessed fixtures have the same problem, which would darken the whole room.
- **Fix:** the shadow ray now stops short of the lamp by the lamp's near offset, which is where the game's own shadows start too, and always by at least 30 cm.
- **To check in game:** the same lobby with Numpad 2. Posters should be lit from above, and ceiling lights should light the room as the game does.

## 0.5.38 - 2026-11-22

- **0.5.37 result:** the RT lamp flicker outdoors is gone.
- **Next RT lamps issue (Jemison UC Security lobby):** with RT lamps on, the whole room was darker, and the light-box posters most of all.
- **Cause:** each pixel's 8 strongest lamps got a shadow ray, including lamps the game never shadows. Those are fill lamps: the light inside a poster box, or soft room fill that level designers place inside or behind geometry. A shadow ray from such a lamp is blocked by the very geometry it sits in.
- **Fix:** only lamps with the game's own shadow flag get an RT shadow ray (light record row 0, flag bit 20 = has shadow data). Every other lamp is added unshadowed, as the game does.
- **To check in game:** the same lobby with Numpad 2. Posters and walls should be close to the game's brightness, and lamp shadows should still show where the game has them.

## 0.5.37 - 2026-11-22

- **0.5.36 log result:** one lamp-list capture per frame, and the game's lamps were switched off every frame, so neither of the earlier guesses was right. The game rotates its lamp data between two buffers in the pattern A, B, B, A, B, B…
- **Cause (likely):** we only copied the lamp list when its address changed, so on the second B frame of each run there was no copy. The RT lamps then used last frame's copy, and the lamp positions in it are relative to last frame's camera. That 2-on, 1-off rhythm matches the bright/dark runs in the flicker recording.
- **Fix:** the lamp list is copied every frame. The copy ring grows from 4 to 6 buffers, so the GPU never reads one that is being rewritten.
- **To check in game:** outside Jemison with Numpad 2, then indoors and in Neon.

## 0.5.36 - 2026-11-22

- **0.5.34 result:** the RT lamps (Numpad 2) still flicker outside Jemison. So the lamp-list countdown wasn't the cause, or not the only one.
- **Diagnostics:** after RT lamps are switched on, the log records the first 120 lamp-list captures as `[lamps] capture frame N #k: ...`. Each line has the frame, the capture number within that frame, the source lamp records, the table and command list, and whether the game's lamps were switched off for that table. The suspicion is that outdoors the game runs its tiled lighting more than once per frame with different lamp lists, and our RT lamps alternate between them.
- **To check in game:** outside Jemison, press Numpad 2 and let it flicker for 2–3 seconds. Quit and send the log.

## 0.5.35 - 2026-11-22

- **Reflection look tuned in game:**
  - `fReflectionGloss` = **0.65** (was 0.7): a bit sharper on non-metal surfaces.
  - `fReflectionStrength` = **0.5** (was 1.3): non-metal reflections at half their physically based strength.
  - Metals are unaffected by both (0.5.27).
- **The baseline (`bStartAllOff`) no longer resets gloss and strength to 1.** It keeps the ini values, so the tuned look is what you see when RT reflections are switched on.

## 0.5.34 - 2026-11-22

- **Baseline test results so far:**
  - RT reflections (Numpad 0): fixed by 0.5.33. The copper columns look right and the facades are no longer dark.
  - Ray Reconstruction (Ctrl+Numpad 0) settles the remaining reflection noise.
- **RT lamps (Numpad 2), outside Jemison:** the whole picture flickered between two brightness levels (about 158 and 107 on a 0–255 scale) every 1–3 frames.
- **Cause:** the game's lamps were kept off only for the next 3 lamp-list captures after our RT lamp pass ran. Indoors the game reads its lamp list about once per frame, but outdoors it reads it several times per frame. The countdown ran out partway through the frame, so the game's lamps came back on for part of it, on top of our RT lamps.
- **Fix:** the game's lamps stay off as long as our RT lamp pass ran within the last 250 ms. If our pass stops for any reason, the game's lamps come back within a quarter second.
- **To check in game:** outside Jemison with Numpad 2, then indoors and in Neon.

## 0.5.33 - 2026-11-22

- **Tester clue:** close to the copper columns the red goes away. Moving back, it shows up again, in a circle around the camera.
- **Cause (likely): reflection rays hitting their own surface beyond the game's level-of-detail switch.** Past a certain distance the game draws a simpler version of the column than our RT copy, and on curved shapes the two are centimetres apart. The reflection ray then started inside our copy and hit its inner face, so the copper reflected copper (deep red). Up close both use the same detailed mesh, hence the circle.
- **Fix:** if a reflection ray hits the back face of a triangle within 10 cm plus 2% of the view distance of its start, it passes through and keeps tracing, up to twice.
- **Also in this build:** 0.5.32's probe floor for off-screen hits (Ctrl+Numpad 5). 0.5.32 was built but never installed.
- **To check in game:** the copper columns from near and far with Numpad 0. Then The Viewport's facade, also with Ctrl+Numpad 5 on and off.

## 0.5.32 - 2026-11-22

- **The 0.5.31 start-up crash wasn't the plugin:** the `Streamline 2.14.16 (RT)` mod had a mixed set of files. `sl.common`, `sl.dlss`, `sl.dlss_g`, `sl.reflex` and `nvngx_dlss` had been replaced with the game's 2.2.0 versions, most likely by MO2 Root Builder (copy mode with redirect) after the crashes. The 2.14.16 production files were restored from the SDK; the mixed files are kept in `dlls\streamline-mixed-backup-2026-11-22`.
- **Baseline test, Numpad 0 (RT reflections), result:** still "smudging endlessly". The copper columns at Jemison stay dark red, and The Viewport's metal facade turns into a large dark smear with light streaks. So 0.5.29's feedback explanation was wrong, or at least not the main cause.
- **Cause: off-screen reflection hits are lit far too dark.** A reflection ray that hits something on screen reuses that pixel's real colour. One that hits something off screen (the canopy above The Viewport, the room behind the camera) is lit only by the sun, the game's lamps and a weak ambient term. The game's own probe saw that direction fully lit, bounce light and emissives included. Metal shows nothing but its reflection, so metal went near-black, and the temporal filter smeared the dark samples.
- **Fix:** an off-screen hit is never darker than the game's probe in that direction. RT keeps whatever is brighter (lamp highlights, sunlit hits), and the probe fills the rest. On-screen hits stay fully ray-traced.
- **New key:** Ctrl+Numpad 5 switches this live, for comparison.
- **To check in game:** The Viewport's facade and the copper columns at Jemison, with Numpad 0 on, then Ctrl+Numpad 5 off and on.

## 0.5.31 - 2026-11-22

- **0.5.30 result:** the same Streamline crash at start, even with no RR unload during start-up. So 0.5.30's explanation was wrong. Error 24 is `eErrorInitNotCalled`: Streamline acting as if its start-up never finished.
- **Change: 0.5.22 is reverted.** The game's own resource callbacks are passed to Streamline again.
  - **Why:** clearing them did not stop the main-menu UI crash it was aimed at (0.5.24 still crashed). It also makes Streamline create its GPU resources on its own, a code path the game never uses. Both Streamline crashes (0.5.29, 0.5.30) came after that change.
- **Kept:** 0.5.30's RR unload on the first `slSetConstants`, and loading it again once in game.
- **To check in game:** start to the main menu a few times. The Reflex lines in the log should say `-> 0`. If the main-menu UI crash comes back, the RR unload on the first frame is the next thing to look at.

## 0.5.30 - 2026-11-22

- **Crash at startup in Streamline (0.5.29).** The stack ends in Streamline's plugin manager, which threw a C++ error (`sl::plugin_manager::PluginManager`, `std::_Throw_Cpp_error`). Just before it, the game's own Reflex lookups failed (`slGetFeatureFunction ... -> 24` instead of 0).
  - **Cause:** 0.5.25 unloaded Ray Reconstruction straight after slInit, while Streamline was still finishing its own start-up. That left Streamline in a bad state. It happened to survive once; this time it crashed.
- **Change:** RR is now unloaded on the game's first `slSetConstants` call instead. That runs on the game's own Streamline thread between frames, after start-up, the same place where RR is loaded again once we are in game.
- **New log line:** `[RR] DLSS-RR unloaded until in game (first slSetConstants)`.
- **To check in game:** start to the main menu a few times. Check that the Reflex lines in the log say `-> 0`, and that RR still loads and runs in game.

## 0.5.29 - 2026-11-22

- **Baseline test, step 2 (Numpad 0, RT reflections):** at Jemison Mercantile, turning RT reflections on made the copper columns go deep, dark red, with reflections that "melt into infinity".
- **Cause: a feedback loop in reusing last frame's image.** A reflection ray that hits something on screen takes its colour from last frame's image (0.4.4: steady room reflections). For a metal surface that colour is mostly its own reflection, so copper reflected copper that was already reflecting copper, frame after frame. Every round multiplied by copper's colour, and it settled at dark red: an endless mirror tunnel.
- **Fix:** when last frame's image is saved, metal pixels are now marked unusable, like smoke already is. Reflection and GI rays that hit metal shade that spot directly instead (one bounce). Non-metal surfaces still reuse the image.
- **To check in game:** the same copper columns at Jemison with Numpad 0. They should keep their copper colour and reflect the room.

## 0.5.28 - 2026-11-22

- **New: baseline mode, to re-check every effect one at a time.** With `bStartAllOff=1` (the default in this build), the game starts with every effect off and the game's own look:
  - the game's own reflections, GI, lamps and sun shadows;
  - no thin-glass pass;
  - none of our glass tweaks (cubemap hidden on glass, sharp view through glass, thick-glass layer);
  - Ray Reconstruction paused (plain DLSS);
  - reflection gloss and strength at 1.
- The RT scene (BLAS/TLAS) is still built, so each effect can be switched on live with its key.
- **Ctrl+Numpad .** applies the baseline at any time.
- Changing Gameplay Options re-applies it, because a quality change re-applies all modes.
- Set `bStartAllOff=0` to go back to starting with effects on.
- **Suggested order:**
  1. Numpad 3: RT sun shadows.
  2. Numpad 0: RT reflections.
  3. Numpad 2: RT lamps.
  4. Numpad 1: GI.
  5. Numpad 4: thin-glass reflections.
  6. Ctrl+Numpad 0: Ray Reconstruction.
  7. Then the glass tweaks (Numpad 8, 9, 6) and the reflection gloss and strength keys.

## 0.5.27 - 2026-11-22

- **0.5.26 result:** the dark areas were not gone. A recording outside The Viewport and Jemison shows the building fronts as smeared, mirror-like chrome with dark blotches: blurry reflections of dark interiors and sky.
- **Cause: the 0.5.24 gloss and strength controls.** They applied to every surface. The facade panels are metal, and a metal's colour is entirely its reflection, so making them glossier and stronger turned brushed metal into dark, smeary mirrors. The 0.5.23 RR guide was not the cause (it stays off by default anyway).
- **Fix:** gloss and strength now apply only to non-metal surfaces, such as floors and painted or plastic surfaces. Partly metallic surfaces get a share. Metals keep the game's own material values.
- **To check in game:** the same spot outside The Viewport and Jemison, then the Ryujin lobby floor (still glossier than before).

## 0.5.26 - 2026-11-22

- **0.5.25 result:** the game started cleanly. RR stayed unloaded at the main menu, loaded in game and ran. But large areas went very dark (outside Jemison Mercantile).
- **Fix (likely): the RR colour-before-transparency guide (0.5.23) is now off by default** (`bRRColorBeforeTransparency=0`).
  - **Why:** the log showed the source texture switching 167 times in a few minutes. The game keeps several copies of its pre-transparency image and alternates between them, so RR was often handed the wrong frame's copy. RR compares that image with the real one to find smoke and glass; where they don't match it treats the pixel as transparent, which can darken large areas.
  - Ctrl+Numpad 2 still turns it on, for comparison. The source-texture log line is now limited to the first 4 changes.
- **To check in game:** the same spot. The dark areas should be gone. If they aren't, press Ctrl+Numpad 0 (RR off) and Numpad 0 (reflections off) to see which one removes them.

## 0.5.25 - 2026-11-22

- **Startup crash, round 3.** Another crash in the main menu's UI code on 0.5.24, about 3 s after rendering started. Like the others, the crash comes through OSF UI's per-frame UI update and reads a corrupted pointer ending in `0000000C`. The 0.5.22 change was active (the game's resource callbacks were cleared) and did not prevent it.
  - **What is certain:** no crash in 5 starts without Starfield RT, and none in 4–5 starts with `bDLSSRayReconstruction=0`. So the trigger is Ray Reconstruction being active at the main menu: either its plugin being loaded during startup, or our per-frame RR settings calls.
- **Change:** RR is still added at Streamline startup, as it must be, but is unloaded again straight away (`slSetFeatureLoaded(RR, false)`). It is loaded only once we are in game and our guide images exist, from the game's own Streamline thread between frames. Until then, our RR code makes no Streamline calls.
- **New log lines:** `[RR] DLSS-RR unloaded until in game` at startup, `[RR] DLSS-RR loaded now that we are in game` after a load.
- **To check in game:** start to the main menu 4–5 times. Load a save and check that `[RR] DLSS Ray Reconstruction is running` still appears.

## 0.5.24 - 2026-11-22

- **Same-scene comparison** with another Starfield RT mod's teaser shots (Ryujin lobby, Jemison Mercantile, a chrome object):
  - Their floors clearly mirror the ceiling lights and walls. Ours show only a faint, blurry trace.
  - Our chrome sphere at GalBank reflects a soft, cloudy smear where theirs would be sharp.
  - **Why:** ours follows the game's materials exactly: roughness for blur, physically based strength. Starfield's floors are fairly rough dielectrics, so physically their reflections are faint and blurry. The look we want (Cyberpunk-like) is glossier.
- **New: two art controls for RT reflections** (replace-all mode, the default):
  - `fReflectionGloss` (default **0.7**): multiplies surface roughness for reflections only. Lower = sharper, glossier. 1 = the old behaviour.
  - `fReflectionStrength` (default **1.3**): multiplies how strong reflections are, capped at a full mirror. 1 = the old behaviour.
  - **Live keys:** Ctrl+Numpad + / − (glossier / softer, steps of 0.05), Ctrl+Numpad * / Ctrl+Numpad / (stronger / weaker, steps of 0.1). The HUD message shows the value to put in the ini.
  - Skin and hair are unchanged, as are GI and lamps.
- **To check in game:** the same scenes as the comparison. Tune the two values until floors and chrome look right, then send the values.

## 0.5.23 - 2026-11-21

- **New: Ray Reconstruction gets a "colour before transparency" guide, for smoke smearing the reflections behind it.**
  - **Why:** Ray Reconstruction denoises the final image, which has smoke, glass and particles already drawn on top. The guide images we give it (albedo, normals, reflection hit distance) describe the floor behind the smoke. So it treated the smoke as part of the floor's reflection, moved it like a reflection and smeared it. NVIDIA's RR guide has an optional input for exactly this: `kBufferTypeColorBeforeTransparency`.
  - **How:** the game already makes a copy of the lit image just before it draws transparencies: the mip-mapped scene texture the glass shows. The plugin picks that texture up while the transparency pass runs and tags it for Ray Reconstruction, only when its size and format match the image RR denoises. The guide warns that a different format makes RR see transparency everywhere.
  - **Switch:** `bRRColorBeforeTransparency=1` (default), with a live toggle on **Ctrl+Numpad 2**.
  - **New log lines:** `[RR] colour-before-transparency source: ...` and `[RR] colour before transparency: tagged / NOT tagged ...`.
- **To check in game:** reflections on floors and windows behind the Neon smoke, with Ctrl+Numpad 2 on and off. If the log says NOT tagged, the size or format differs and nothing changed.
- **Still open from 0.5.22:** startup to the main menu 4–5 times with RR on.

## 0.5.22 - 2026-11-21

- **Startup crash narrowed down to Ray Reconstruction.** On 0.5.21 the game crashed again right as the main menu came up. That's the same moment as the 19:55 and 21:09 startup crashes, and every crash was in the game's UI code. With `bDLSSRayReconstruction=0`, 4–5 starts in a row had no crash.
- **Fix (likely):** when Ray Reconstruction is wanted, the plugin passes the game's Streamline settings on with RR added. Those settings include the game's resource callbacks: Streamline asks the game to create every GPU resource it needs. The game's callbacks were written for Streamline 2.2, and the 2.14 RR plugin creates many internal resources through them. The plugin now clears both callbacks, so Streamline creates its resources itself.
- **New log line:** `[SL] game resource callbacks: allocate ..., release ...` shows what the game passed.
- **`bDLSSRayReconstruction=1` again** (this build's ini restores it).
- **To check in game:** start the game to the main menu several times with RR on. Also check that Ray Reconstruction still runs (`[RR] DLSS Ray Reconstruction is running` in the log).

## 0.5.21 - 2026-11-21

- **0.5.20 result:** got in game without a crash, and the squares in hair are gone (0.5.18). Five starts with Starfield RT disabled had no crash. Some dots remain on the shadowed side of faces in certain spots.
- **Fix: dots on the side of a face turning away from the camera.**
  - **Cause:** the character check casts a ray from the camera to the pixel. Where the face turns away, that ray only grazes it. Our copy of the face has no facial morphs, so its outline differs slightly, and the ray slipped past the copy to the background. Those pixels then got the noisy RT GI.
  - **Change:** if the camera ray doesn't find a character, a second short ray goes from 12 cm above the surface straight back into it along its normal. If it hits a character, the pixel keeps the game's indirect light. This works at any view angle.
- **To check in game:** the same kind of spot: faces in shade, seen half from the side.

## 0.5.20 - 2026-11-21

- **Fix (likely the real cause): crashes at startup and at the end of save loads.** Today's crash logs all pointed into the game's UI code (Scaleform) and its scratch memory. The save loaded fine with Starfield RT disabled.
  - **Cause:** to read the Gameplay Options menu values (StarfieldRT.esm), the plugin looked the two option values up by name with `TESForm::LookupByEditorID` every 120 frames until it found them, from a render thread. That walks the game's global form table while the main thread is still filling it at startup and at the end of every load. Two load crashes came right after the `Gameplay Options found` line, and a third about two seconds after it.
  - **Change:** the lookup now runs on the game's main thread only: once when the game's data has loaded (SFSE message), and if needed again as an SFSE main-thread task. The render thread only reads the two cached values.
  - **The 0.5.14/0.5.17 fixes stay:** they closed real use-after-free bugs too.
- **To check in game:** start the game and load saves several times. The log line is now `Gameplay Options found (StarfieldRT.esm, main thread)`, shortly after startup.

## 0.5.19 - 2026-11-21

- **0.5.18 result:** blotches on some NPC faces go away with GI off (Numpad 1). RT reflections, Ray Reconstruction, lamps and sun shadows don't change them.
- **Fix: GI on faces, second try.**
  - **Why 0.5.15 only worked some of the time:** it decided whether a pixel shows a character by casting a ray from the camera to that pixel. In first person, the RT scene also holds a copy of the player's own body and head around the camera. At some angles that ray hit the player's own head copy first, so the NPC wasn't recognised and got the noisy RT GI again ("depending on where they stood").
  - **Change:** the check ray now starts half a metre in front of the surface.
- **To check in game:** blotchy NPC faces with GI on, from several angles. Also report whether the squares in hair are gone (0.5.18).

## 0.5.18 - 2026-11-21

- **0.5.17 result:** still squares, along hair edges and the profile of a nose against the background. Numpad 2 (game lamps) and Numpad 3 (sun shadows) did not fix the blotchy skin.
- **Fix (second try): squares along silhouettes.**
  - **Why 0.5.16 wasn't enough:** a tile across a silhouette ranked its lamps against one box spanning both the hair and the far street, so it still dropped lamps that only mattered for the hair. A tile fully on the hair kept them.
  - **Change:** every lamp tile now splits its pixels into near and far at the middle of its depth range and keeps a separate list of up to 64 lamps for each. A pixel uses the list for its own depth.
- **Blotchy skin, next test:** in front of a blotchy NPC, try Numpad 0 (RT reflections off) and Ctrl+Numpad 0 (Ray Reconstruction off). Lamps, sun shadows and GI are ruled out.

## 0.5.17 - 2026-11-21

- **Crash at startup (main menu, before any RT ran) on 0.5.16.** The game read a pointer that held the float value FLT_MAX, inside its scratch allocator: memory damaged earlier. Our code never writes that value directly, but 0.5.14's fix left a gap that can cause exactly this kind of damage.
- **Fix: the 0.5.14 gap.**
  - **The gap:** when the game copied a descriptor into the GPU heap, the plugin took a reference to the buffer through the source descriptor's raw pointer. Games copy whole blocks of descriptors, stale ones included, so this could again write into a freed buffer, at startup or on loads.
  - **Change:** every descriptor heap now holds its own references, taken only when the game creates a view (the buffer is alive then). A copy takes its reference from the source slot's own reference, under the same lock.
  - **Watch:** `buffers held through descriptor slots` in the log. It now counts CPU-side heaps too, so it will be higher than 0.5.16's ~23,500. It should level off, not climb all session.
- **To check in game:** start the game and load saves several times. Send the log after a longer session.

## 0.5.16 - 2026-11-21

- **0.5.15 result:** faces look better. Some are still blotchy depending on where the NPC stands. There are squares in hair.
- **Fix: squares in hair (and anywhere a near object stands against a far background).**
  - **Cause:** RT lamps are sorted into 16×16-pixel tiles, and each tile keeps at most 64 lamps. A tile that mixes hair with the distant street spans a huge depth range, so in Neon it catches far more than 64 lamps. It kept whichever 64 its threads happened to add first, so neighbouring tiles lit the same hair with different lamps.
  - **Change:** each tile now scores its lamps by how bright they can be at the tile and keeps the 64 strongest. This is the same kind of squares as the 0.4.8 floors at High.
- **Not addressed yet:** NPCs that are still blotchy in some spots. A screenshot of one with the spot it stands in, and whether Numpad 2 (the game's lamps) or Numpad 3 (sun shadows) changes it, would narrow it down.

## 0.5.15 - 2026-11-21

- **0.5.13 face test result:** the dot grid on NPC faces goes away with GI off (Numpad 1), so it is our GI.
- **Fix: characters keep the game's own indirect light.** Faces and hair animate every frame, so our GI's smoothing across frames never builds up on them, and the raw ray noise showed as a dot grid. The GI pass now sends one ray from the camera through each pixel. If the surface there is a character, it uses the game's indirect light instead of tracing. Everything else keeps RT GI. NPCs still bounce RT GI onto the world around them.
- **Cost:** one extra short ray per GI cell.
- **To check in game:** faces and hair up close with GI on: no dots, no black hair. Also check that walls and floors near NPCs don't show a bright or dark outline around them.

## 0.5.14 - 2026-11-21

- **Fix: crashes on save load (and one at startup) in the game's UI code.** Three crash logs in a row pointed at Scaleform, the game's UI system. The same save loaded fine with Starfield RT disabled, so the cause was ours.
  - **Cause:** to build RT shapes from streamed vertex buffers, the plugin took a reference to a buffer through a raw pointer remembered from the game's descriptor. If the game had already freed that buffer, which happens constantly during a load, taking the reference wrote into freed memory. That damage then crashed whatever used the memory next, here the UI. 0.5.11 made the plugin rebuild changed objects sooner and build more per frame, which made this much more frequent.
  - **Change:** the descriptor tracking now holds its own reference to every buffer it records in the game's shader-visible descriptor heaps, and releases it when the slot is overwritten. The RT build takes its reference from that, never from a pointer that may be stale.
  - **New log line:** `buffers held through descriptor slots: N`, every 600 frames. If N keeps climbing through a long session, buffers are being held too long (VRAM).
- **To check in game:** load saves several times, including the one that crashed. Watch the VRAM line in the log over a longer play session.

## 0.5.13 - 2026-11-21

- **0.5.12 result:** faces still blotchy, but now in a regular dot grid instead of patches. One NPC's hair rendered pure black. With thin-glass reflections off (Numpad 4), almost all the flicker in Neon goes away. The tester thinks much of it is the smoke.
- **Thin-glass flicker under smoke: the untagged-window search is now off by default** (`bThinGlassUntaggedFallback=0`).
  - **Why:** that search only runs where the game reports something see-through on the pixel, and drifting smoke switches that on and off every frame. This is the test suggested in 0.4.10 that never got run.
  - **Trade-off:** a few Neon shop windows whose material isn't tagged as glass lose their RT reflection.
  - **Compare:** **Ctrl+Numpad 4** toggles the search live.
- **New: Ctrl+Numpad 0 switches DLSS Ray Reconstruction off and on live** (plain DLSS-SR while off). This is for the face test: the dot grid looks like our GI's noise pattern, and black hair can come from Ray Reconstruction getting a wrong colour guide for hair.
- **To check in game, in front of an NPC's face:**
  1. Ctrl+Numpad 0 (RR off): do the dots and the black hair go away?
  2. Numpad 1 (GI off): do the dots go away?
  3. Numpad 2 (the game's lamps): do they go away?
  - Also: does the smoke walkway still flicker? If it does, try Ctrl+Numpad 4.

## 0.5.12 - 2026-11-21

- **Fix: splotchy, smudgy NPC faces and hair.**
  - **Cause:** our RT copy of each character is skinned by bones only. It doesn't include facial morphs, so it sits a few centimetres off the face the game draws. GI, lamp and sun shadow rays leaving a face hit that copy and shadowed the face in sharp-edged patches. 0.5.11 put more characters into RT, which made it more visible.
  - **Change:** characters now ignore hits within 20 cm of where a ray starts. They still block light beyond that, so NPCs keep casting shadows on floors, walls and each other. Character hair now also goes through the cutout test where its material has one, instead of acting as a solid sheet.
  - **Cost:** characters are now checked hit by hit (like leaves), which is slightly slower in crowds.
- **Build:** `toolsuild_shaders.ps1` recompiles every shader header. 0.5.11 had compiled BuildInstances for cs_6_0; it is cs_6_6 again.
- **To check in game:** NPC faces and hair up close in Neon. Also check that NPCs still cast RT shadows and show in reflections.

## 0.5.11 - 2026-11-21

- **Less RT pop-in as you walk up to things.**
  - **Objects:** when the game swaps in a more detailed version of a mesh as you get closer, the RT copy had to wait for two mesh scans 60 frames apart before it was rebuilt: 1.5 to 3 s with no RT shadow or reflection for that object. Scans now run every 10 frames (`iBlasScanInterval`), and a changed mesh settles for 10 frames instead of 30 outside loads. Loads keep 30, because they hung on half-loaded meshes. That brings the gap to about a quarter to half a second.
  - **Faster catch-up:** `iBlasBuildsPerFrame` 64 -> 128.
  - **Characters:** NPCs in the RT scene (reflections, RT shadows): Ultra 64 within 40 units -> **128 within 60**, High 48 within 30 -> 64 within 40. Each character is several instances, so crowds filled the old 64 slots fast.
- **Cost:** more skinned BLAS rebuilds per frame in crowds and a little more CPU for the scans. Watch fps in Neon and the `PERF` lines.
- **To check in game:** walk toward buildings, signs and crowds in Neon and watch whether shadows and reflections still pop in. Also check that save loads stay stable.

## 0.5.10 - 2026-11-21

- **Ray Reconstruction raw mode is now off by default** (`bRRRawMode=0`). In game, raw mode flickered. Our own temporal and spatial filters with RR on top are much more stable. Ctrl+Numpad 1 still toggles it for comparison.
- **0.5.9's flicker fix confirmed:** in 3600 frames, 15 frames had a build that wrote nothing, and none of them lost RT lighting (0 dark frames, down from about 1 in 250).

## 0.5.9 - 2026-11-21

- **Flicker, round 2.** The 0.5.8 log confirmed the dark flashes: 17 frames in about 80 s had no RT GI, lamps or reflections. In every one, this frame's RT build had started but never marked a TLAS, and the lighting pass was on a different command list. The 0.5.8 fix covered a different case and saved none of them.
  - **Change:** when this frame's build finishes without writing anything to the GPU (no BLAS rebuilt, no TLAS update), the previous TLAS is complete and unchanged, so the lighting passes now trace it from any command list instead of dropping the frame.
  - **Confirmed by the numbers:** the TLAS build count fell behind the frame count by exactly the number of dark frames (18 by frame 5400), so every dark frame was a frame whose build wrote nothing.
  - **Same fix for sun shadows:** those frames also skipped the RT sun shadow (outdoors that would flash fully lit). The sun shadow now traces the unchanged TLAS too.
  - **Still skipped (for GPU safety):** a lighting pass on another command list while this frame's TLAS update is being written. Tracing a TLAS mid-rebuild can hang the GPU.
- **More detail in the `[flicker]` lines:** each skip now says what this frame's build was doing (not started, still recording, wrote GPU work, wrote nothing) and on which list. `RT build recorded no BLAS/TLAS work` and `ran on the unchanged TLAS` show the fix working.
- **To check in game:** same walk through Neon. Then send the log. If dark flashes remain, the skip lines say which case is left, and that decides the next fix: double-buffering the TLAS.

## 0.5.8 - 2026-11-21

- **Fix (likely): one-frame dark flashes.** In Neon the whole picture sometimes went flat and dark for a single frame: RT GI, RT lamps and RT reflections all dropped out together.
  - **Cause (suspected):** the game records each frame on several threads at once. The RT passes only ran if the TLAS had been built on the same command list in the same frame. When the frame counter moved on between the TLAS build and the lighting pass, that whole frame was skipped. The skip was only logged for the first three times, so it never showed up.
  - **Change:** on the same command list, a TLAS one frame old now counts. The build comes earlier on that list, so the GPU order is still safe. Different command lists are still skipped, as before.
- **New `[flicker]` log lines** (the first 40 of each kind):
  - `RT passes skipped, frame N: ...` for each skip, with the reason.
  - `frame N ended without RT GI/lamps/reflections` for any frame that gets no RT lighting for any reason.
  - `ran on the TLAS from frame N` when the fix saved a frame.
  - A total every 600 frames.
- **To check in game:** walk around Neon as in the last recording (the shop sign, the wall with the face). Watch for the dark flash. Then send the log. If flashes remain, the `[flicker]` lines show which case is left.

## 0.5.7 - 2026-11-20

- **New: Ray Reconstruction raw mode** (`bRRRawMode=1`, on by default; **Ctrl+Numpad 1** toggles it in game with a HUD message).
  - **What changes:** while RR is actually running, RT reflections and RT GI skip our own denoisers (temporal accumulation and the spatial blur) and trace one ray per pixel instead of per 2×2 cell. RR is trained on exactly this kind of raw path-traced noise and does the accumulation and filtering itself, as in Cyberpunk.
  - **Expected result:** sharper reflections and GI detail, less smearing and lag when lights or objects move, no blotchy blur.
  - **Kept:** reflections still use last frame's image for on-screen hits (the camera history is still written).
  - **Automatic fallback:** when DLSS is off or RR isn't running, everything goes back to our own filters.
  - **Log:** a `[RR] raw mode on/off` line whenever it switches.
- **Check in-game** (DLSS on, Ultra):
  - Compare Ctrl+Numpad 1 on and off in Neon: reflections on floors and windows, GI in rooms, under the RT lamps.
  - Look for "boiling" (crawling noise RR can't settle), ghosting behind people, and the cost in fps.
## 0.5.6 - 2026-11-20

- **0.5.5 result: no crashes, and DLSS Ray Reconstruction runs.** The load crash (scene buffers swapped mid-load) is fixed.
  - The game's upscaler had to be switched back to DLSS in Display settings; it had been off since the Streamline swap.
  - With DLSS on (DLAA, 3440×1440), RR took over 0.3 s after DLSS started and ran with no errors until the game closed.
- **Log tidy-up:** the game tags a new depth buffer most frames, which filled the log with ~400 identical lines. Streamline tag lines are now capped at 40.
- **Check in-game:** with DLSS on, compare noise in reflections, GI and RT lamps against earlier versions. Look for smearing or ghosting on moving things, and on glass and smoke.
## 0.5.5 - 2026-11-20

- **0.5.4 result:** crash on save load again, but this time the GPU crash report was clear:
  - The GPU stopped in an acceleration-structure build on a page fault.
  - The faulting address belonged to one of our own buffers ("StarfieldRT buffer") that had just been freed.
  - It happened ~0.2 s after the log line "TLAS buffers for 98304 instances", the moment the scene buffers grow and the old ones are retired.
  - The 0.5.3 crash had the same line just before it. Streamline and Ray Reconstruction were not involved.
- **Fixed:**
  - **No mid-load resize:** the scene (TLAS) buffers are now made at 131072 instances (~60 MB) from the start, more than a Neon load needs, so they're no longer swapped in the middle of a load.
  - **Longer hold:** retired GPU buffers are kept 120 frames instead of 30 before release, so the GPU can't still be using them.
- **Also from the 0.5.4 log (DLSS):**
  - Streamline reports DLSS as supported on the RTX 5080. The two "not supported" answers were for the other adapters (Intel iGPU, virtual display).
  - The game still never asked for DLSS. That points at the game's upscaler setting, not the Streamline files.
- **Check in-game:**
  - Save loads.
  - In Display settings, check which upscaler is selected. Pick DLSS if it isn't. With DLSS on, the log should show the `[RR]` lines.
## 0.5.4 - 2026-11-20

- **0.5.3 result:** GPU hang ("device removed, hung") about 5 seconds after a save finished loading. Streamline 2.14.16 itself loaded fine, including Ray Reconstruction (`slInit` → 0). But the game then **never used DLSS at all**: it didn't look up any DLSS function, tag any image or run DLSS once. It only set up Reflex. So the game decided DLSS wasn't available with the new Streamline set, and RR never got a chance to run.
- **Fixed:** the RR guide pass (one extra mirror ray per pixel at full resolution, plus 4 copies) ran every frame even though DLSS wasn't active. It now runs only while the game is actually running DLSS (within the last 2 seconds). This takes our extra GPU work out of the picture whenever RR can't be used.
- **Diagnostic:** the plugin now logs:
  - every function Starfield imports from `sl.interposer.dll`;
  - each Streamline support check the game makes (`slIsFeatureSupported`, `slIsFeatureLoaded`, `slSetFeatureLoaded`, `slGetFeatureRequirements`, `slGetFeatureVersion`) with its result.

  This shows which check makes the game skip DLSS.
- **Also seen:** the RenoDX DLSS ReShade add-on (`renodx-dlss.addon64` in overwrite) hooks Streamline's versions, tags and swapchain. It is a likely suspect for the game turning DLSS down.
- **Check in-game:** open the display settings and check whether DLSS is still selected and available. Load a save, then send the log (and say whether it crashed).
## 0.5.3 - 2026-11-20

- **Clean Streamline set for Ray Reconstruction.** The mixed Streamline files (2.14.1 / 2.2.0, no RR plugin, DLSS 3.5) that sat in MO2's `overwrite\Root` were moved into a new, disabled mod `Streamline backup (from overwrite)`. A new mod `Streamline 2.14.16 (RT)` holds one matching production set from the Streamline SDK: `sl.interposer`, `sl.common`, `sl.dlss`, `sl.dlss_d` (RR), `sl.dlss_g`, `sl.reflex`, `sl.pcl`, `sl.nis` 2.14.16 and `nvngx_dlss` / `nvngx_dlssd` / `nvngx_dlssg` 310.9.16. It is enabled at the top of the `okok` profile.
- **Ray Reconstruction is on again** (`bDLSSRayReconstruction=1`). If anything is missing it falls back to normal DLSS.
- **Check in-game:** DLSS on (any quality), load a save. The log should show the `[RR]` lines (loaded, options set, running). Look at reflections and GI in Neon: noise should be much calmer. Frame generation and Reflex now also come from 2.14.16, so check they still work (or stay off for testing). If the game crashes at start, disable `Streamline 2.14.16 (RT)` and enable the backup mod to go back.
## 0.5.2 - 2026-11-20

- **0.5.1 result:** crash on save load. Streamline's own log, forwarded by 0.5.1, explained everything:
  - **Plugin folder:** Streamline looks for plugins in `C:\GTS\Stock Game\` (MO2 stock-game setup), not the Steam folder, so the RR files copied there in 0.5.0 were never seen. They've been removed again.
  - **Mismatched Streamline set:** in the active profile both DLSS5 mods are disabled. The files the game gets come from MO2's `overwrite\Root`, which mixes `sl.interposer` 2.14.1 with `sl.common` / `sl.dlss` / `sl.dlss_g` / `sl.reflex` 2.2.0, has no `sl.dlss_d.dll`, and an old DLSS 3.5 `nvngx_dlss.dll`.
  - **Crash cause:** with 0.5.1's verbose Streamline logging, Streamline threw an exception while loading those plugins, so `slInit` failed (24). A broken Streamline followed: "Reflex context missing", then a GPU crash.
- **Fixed:** the verbose-logging change is gone, back to the `slInit` path that worked in 0.5.0.
- **Ray Reconstruction now defaults to off** (`bDLSSRayReconstruction=0`) until the Streamline set is consistent (needs the tester's OK to rearrange the MO2 files).
- **Check in-game:** save loads work again.

## 0.5.1 - 2026-11-20

- **0.5.0 result:** no crash, and the guide images were made and tagged. But Streamline answered the RR function lookup with result 31 (`eErrorFeatureMissing`): it accepted the request to load DLSS-RR but didn't load the plugin, even though sl.dlss_d.dll and nvngx_dlssd.dll are next to Starfield.exe. The game kept running normal DLSS (the fallback worked).
- **Diagnostic:** the game gives Streamline no log file, so its reason is invisible. The plugin now raises Streamline's log level to verbose and forwards its messages into our log (`[SL log â€¦]`): warnings, errors and anything about plugins, features or NGX. The game's own log callback, if any, still gets every message.
- **Check in-game:** start the game with DLSS on, load a save, quit, and send the log.

## 0.5.0 - 2026-11-20

- **New: DLSS Ray Reconstruction (north star item 4, first version).** NVIDIA's AI denoiser from Cyberpunk's path tracing now runs in place of DLSS Super Resolution. It denoises and upscales the ray-traced image in one step, trained on exactly this kind of noise.
- **How it's wired** (from the 0.4.11 `[SL]` log):
  - `slInit` now also loads DLSS-RR.
  - A new guide pass (right after GI and lamps) writes the four extra images RR needs at the DLSS input size (3440Ã—1440):
    - diffuse albedo;
    - specular albedo (from the game's own split-sum lookup table);
    - world normal with roughness (packed);
    - specular hit distance (one mirror ray per pixel).
  - Those are tagged together with the game's own DLSS inputs (colour, depth, motion vectors, exposure).
  - Each frame the RR options mirror the game's DLSS options (Quality, 3440Ã—1440, HDR, pre-exposure) plus the camera matrices from the game's Streamline constants.
  - The game's DLSS-SR evaluate then runs as DLSS-RR, but only once RR has loaded and its options and guides are in place. Otherwise, or if an RR evaluate fails, normal DLSS runs that frame.
- **Files:** `sl.dlss_d.dll` (Streamline 2.14.16, production) and `nvngx_dlssd.dll` (310.9.16) were copied next to Starfield.exe. They are new files; nothing was replaced. `bDLSSRayReconstruction=0` in the ini (read at game start) goes back to normal DLSS.
- **Not done yet:** our own denoisers still run before RR (RR is trained on raw noise, so a "raw" mode for our passes comes next if RR looks good). How it combines with the tester's DLSS-NR proxy and RenoDX is unknown.
- **Check in-game (DLSS on):**
  - The log should show `[RR] DLSS-RR plugin loaded â€¦ ready`, `[RR] guide images tagged â€¦` and `[RR] DLSS Ray Reconstruction is running in place of DLSS Super Resolution`.
  - Look for a cleaner, sharper image, or for problems: smearing, wrong colours, a black screen, crashes.
  - Compare with `bDLSSRayReconstruction=0` (needs a game restart).

## 0.4.11 - 2026-11-20

- **DLSS Ray Reconstruction, step 1: Streamline logging (nothing changes in the image).** The plugin now patches Starfield.exe's import table entries for NVIDIA Streamline (`sl.interposer.dll`: `slInit`, `slSetTag`, `slEvaluateFeature`, `slSetConstants`, `slGetFeatureFunction`, and through it `slDLSSSetOptions`). It logs what the game asks of Streamline:
  - which features it loads;
  - which buffers it hands DLSS, with their size, format, state and lifecycle;
  - its DLSS options;
  - its camera and motion-vector constants.
- Every call is passed through unchanged. Log lines start with `[SL]`.
- Builds against NVIDIA's Streamline SDK 2.14.16 headers (copied to `external/streamline/include`; nothing is linked). The RR files the tester downloaded (production `sl.dlss_d.dll` 2.14.16, `nvngx_dlssd.dll` 310.9.16) are in `dlls/` and not installed yet.
- **Check in-game:** start the game, load a save, play ~10 seconds with DLSS on, quit, and send the log. The `[SL]` lines are what's needed to wire up Ray Reconstruction.

## 0.4.10 - 2026-11-20

- **0.4.9 result:** squares gone, wavy noise gone (Ultra set in Gameplay Options).
- **Glass flicker under smoke (parked since 0.3.10), two changes:**
  - The thin-glass reflection now fades in with the game's transparency coverage, reaching full strength about 0.02 above the threshold. Before, it switched on at a hard threshold, so thin, drifting smoke could flip a full-strength reflection on and off.
  - New `bThinGlassUntaggedFallback` (default 1). 0 turns off the "any object in front of the surface behind" search for untagged windows, so one switch shows whether that search causes the flicker.
- **Check in-game:**
  - Is the walkway still flickering?
  - If yes, set `bThinGlassUntaggedFallback=0` and check again.
  - Also record a few seconds with the Numpad 4 debug colours on (press it until the message says "debug colours"): yellow = tagged glass, white = found by the fallback, magenta = covered but nothing found.

## 0.4.9 - 2026-11-20

- **0.4.8 result:** blocky squares on floors with RT lamps on at High; they go away with the game's lamps (Numpad 2) or at Ultra. The log also showed the in-game Gameplay Options were still on High, which overrides the ini's Ultra.
- **Fixed: squares with RT lamps at High.**
  - 0.4.8 kept a list of the 8 strongest lamps per pixel but shadow-tested only the first 4 slots at High. The list isn't sorted, so which lamps got a shadow depended on the order of the tile's lamp list.
  - That order differs per 16Ã—16 tile, because the GPU builds the lists in parallel, so a bright lamp was shadowed in one tile and not in the next.
  - The list now only holds as many lamps as get shadow-tested (4 at High, 8 at Ultra): always the strongest.
- **Check in-game:** at High and at Ultra, no squares with RT lamps on (Numpad 2).

## 0.4.8 - 2026-11-20

- **Maximum quality: Ultra now maxes every effect, and Ultra is the ini default** (`iReflectionQuality=4`, `iShadowQuality=4`).
  - **GI:** one cell per pixel (was one per 2Ã—2) with 2 rays each, so 8Ã— the GI rays of High. The noise should drop a lot.
  - **RT lamps (Numpad 2):** the 8 strongest lamps per pixel get a shadow ray (was 4).
  - **Reflections (already at Ultra):** one ray per pixel, 6 km range, 64 animated characters within 40 m.
  - **Sun shadows (already at Ultra):** 8 soft rays per pixel at full resolution, 3 km range.
  - High stays as before, as the fallback if the frame rate drops too far.
- **The in-game Gameplay Options menu overrides the ini.** If it's on High there, set **both RT options to Ultra** in the game's Gameplay Options menu.
- The log's `quality (...)` line now also lists the GI and lamp settings in use.
- **Check in-game:** set Ultra in Gameplay Options, then compare the noise and the frame rate with High.

## 0.4.7 - 2026-11-20

- **0.4.6 result:** you're gone from reflections. Walls and ceilings still wave and are noisy, and it stops with GI off (Numpad 1), so the GI is the cause.
- **Fixed (likely): GI waving while moving.**
  - The GI blends each spot's result over up to 48 frames, as long as last frame's sample is the same surface.
  - That check compared distances from the camera (5% tolerance). On walls and ceilings seen at a slant, neighbouring pixels differ by more than that in camera distance, and the pixel each GI cell samples changes every frame.
  - So while moving, slanted surfaces kept throwing their history away and fell back to raw one-ray noise with an extra-wide blur: the drifting blobs.
  - Now the check asks whether last frame's sample lies on the same plane (distance along the surface normal, plus a sanity distance), which doesn't depend on the viewing angle. Slanted surfaces keep their history while you move.
- **Same fix for the reflections' frame blending**, which used the same kind of test.
- **Under the hood:** last frame's camera is now kept in full (rows 0â€“16 of the scene constants, was 0â€“8), to rebuild where last frame's sample was.
- **Check in-game:** walk and turn in the same room. Walls and ceilings should stay calm with GI on, and moving lamps or people shouldn't leave ghost trails.

## 0.4.6 - 2026-11-20

- **0.4.5 result (tester video and screenshot):** walls and ceilings "wave": soft blotches that shift every frame (frames 30â€“33 of the video). A big copy of your character still shows in a window: suit, backpack, a head looking up.
  - The log shows on-screen reflection hits now use last frame's image ("yes").
  - The first-person tag catches 2â€“6 parts of you, not all.
- **Fixed (likely): GI boiling.**
  - A GI ray that hits a small, very bright lamp panel brings back a huge value while its neighbours miss it. The blur and frame blending turned those rare spikes into slowly moving blobs.
  - Each GI ray is now capped at 4Ã— the screen's white level (was 32Ã—), so lamps light a room evenly instead of in spikes.
  - GI also blends 48 frames (was 32) and blurs a little wider (radius 6, was 4).
- **Fixed (likely): the rest of your body in windows.** Parts anchored near head height (helmet, head, backpack) sit level with the camera. 0.4.5 only tagged parts at least 0.5 m below it, so they were never hidden. Now: from 0.6 m above the camera to 2.5 m below, within 1.2 m sideways.
- **Check in-game:**
  - Walls and ceilings near lamps should be steady.
  - Numpad 1 off/on tells whether any waving left comes from GI; Numpad 0 cycles reflections.
  - No copy of you in windows.

## 0.4.5 - 2026-11-20

- **0.4.4 result:**
  - Reflections are still very noisy, even with Numpad 2 off (so it isn't the RT lamp pass).
  - Your character still shows in window reflections.
  - Shadows are fine.
- **Cause of the noise:** 0.4.2 brought the game's lamps back into reflections and GI (they'd been missing since 0.2.60, which is why things looked calmer before). Each point seen in a reflection did a hard shadow test toward its brightest lamps, and neighbouring glossy rays land on different points, so the lamp light speckled.
- **Fixed (likely):** lamps at reflection and GI hits are no longer shadow-tested by default (`iHitLampShadowRays=0`). Reflected surfaces are still lit by every lamp, just smoothly. The trade-off: a lamp behind a wall can shine through it in a reflection. The on-screen RT lamps (Numpad 2) keep their 4 shadow rays per pixel.
- **New log line:** `[R] on-screen reflection hits use last frame's image: yes/no (reason)`. 0.4.4's noise fix for on-screen hits only works when this says yes.
- **Fixed (likely): your character in window reflections.** You're recognised as the first-person player when your body is right under the camera. Running, the body lags or leans and dropped out of the old 0.75 m range, so it wasn't hidden. The range is now 1.5 m sideways and 0.5â€“2.5 m below the camera.
- **Check in-game:**
  - Room reflections: much calmer?
  - Windows while standing and running: no copy of you?
  - Send the log if either is still off; the new `[R] on-screenâ€¦` line matters for the noise.

## 0.4.4 - 2026-11-20

- **0.4.3 result:** Numpad 2 works (the GPU copy and descriptor swap do their job). Problems: room reflections were still very noisy, there were black squares around people's heads with the game's lamps off, and in first person a big T-posed copy of your character showed in windows and smoke.
- **Fixed: black squares around heads.** Dialogue face lights are spotlights with a cone exponent of 0. Outside the cone my formula computed `pow(0, 0)`, which is NaN on the GPU, and NaN turns a pixel black. Every 16Ã—16 lamp tile that included a face light went black outside its cone.
  - A safe power function now returns 0 there, never NaN, and a final guard drops non-finite results.
  - The RT lamp pass also honours the game's **light channels** (lamp mask âˆ§ the pixel's G-buffer channel bits), as the game does. Face lights now light only the characters, not the walls behind them.
- **Much less noise in reflections:** a reflection ray that hits something that was on screen last frame takes that spot's real lit colour from last frame's image (the capture GI already uses), like screen-space reflections, instead of re-lighting it with per-ray shadow tests.
  - In rooms most reflection rays hit visible walls, so most of the crawling noise goes away.
  - Off-screen hits, and hits behind glass or smoke, are still ray-traced.
  - Reflections now also keep that image when GI is off.
- **Your first-person body is gone from reflections, GI and glass.** The game doesn't animate the third-person body in first person, so our copy stood in a T-pose and showed in windows and smoke.
  - Reflection, GI and glass rays now skip it entirely, and the glass pass never takes it for a pane.
  - Shadow rays still see it (minus the head), so you keep a shadow.
  - In third person your character is unaffected and reflects normally (the first-person tag only applies when the camera sits right above the character).
- **Check in-game:**
  - Room reflections should be much calmer.
  - No black squares near people with Numpad 2 on.
  - No T-pose copy of you in windows or smoke in first person.
  - Tell me if your first-person shadow looks T-posed too.

## 0.4.3 - 2026-11-20

- **0.4.2 result:** reflections now pick up the game's lamps (a big visible change), but they're very noisy in rooms, and Numpad 2 still did nothing. The log shows the lamp buffers still weren't in any CPU-writable buffer the plugin tracks, even with the newer creation calls hooked. They're GPU memory, or allocated out of the registry's sight.
- **New approach, all on the GPU (no CPU access needed):**
  - **Copying the lamp list:** when the game binds its lighting table, the plugin records a tiny compute shader on the game's own command list. It reads the game's lamp list straight from its GPU address and copies it into the plugin's buffer: lamp records, index ranges, and a second copy of the ranges with point, box and spot lamps emptied. The copy runs on the GPU right before the game's lighting, so reflections, GI and RT lamps always read a complete copy from the same frame.
  - **Switching the game's lamps off:** while the RT lamp pass is running, the plugin repoints one entry of the game's lighting table (its lamp ranges) at that emptied copy. The game's tiled lighting then skips its lamps. The plugin already uses this kind of descriptor swap to hide the game's glass reflections.
- **Safety:** compute root descriptors (buffers bound by address) are now tracked and restored after injected work too, not just tables and constants.
- **Less noise in room reflections:**
  - Reflection hits now shadow-test only lamps bright enough to matter. Before, the first lamps found got the shadow test, often faint ones, leaving the bright lamp next to the surface unshadowed.
  - The firefly clamp on reflection rays is tighter (16Ã— the exposure's white instead of 64Ã—).
- **Check in-game:**
  - Numpad 2: lamp light should visibly change between ray traced (with shadows) and the game's. The log should show `[lamps] RT lamps: applied`.
  - Room reflections should be calmer.
  - If anything goes black or the game crashes when the lighting table is touched, send the log.

## 0.4.2 - 2026-11-20

- **0.4.0 result:** Numpad 2 changed nothing. The log said `[L] light buffers not in a known upload buffer (lights false, ranges false)`, so the RT lamp pass never ran ("no lamp list captured yet").
  - Since 0.2.60, the lamp lighting in reflections and GI depended on the same lookup. They were probably missing the game's lamps too, which may also explain why the 0.2.58 flicker "went away" then.
- **Why:** the plugin finds the buffer behind a GPU address with a registry of CPU-writable buffers it records as the game creates them. That registry only covered plain "upload" heaps created through the three oldest creation calls, requested as the oldest resource interface. The game's lamp lists live somewhere else.
- **Fixed (likely):** the registry now covers:
  - every CPU-writable heap: upload, custom heaps with CPU access, and GPU-upload (ReBAR);
  - the newer resource interfaces;
  - the newer creation calls (`CreateCommittedResource2/3`, `CreatePlacedResource1/2`, hooked when the device has them; slots checked against the Windows SDK).
- **Fallback:** if the lamp lists still can't be found, reflections and GI read the game's lamps in place (the 0.2.58 way), so lamps aren't lost. The RT lamp pass then stays off, because without write access it can't switch the game's lamps off and would light them twice. The log says so.
- **Check in-game:** press Numpad 2 in Neon. The log should show `[lamps] RT lamps: applied`, and the lamps should change. A new log line, `CPU-writable buffer registry hooks: Device8 â€¦, Device10 â€¦`, says which creation calls were hooked.

## 0.4.1 - 2026-11-20

- **All hotkeys moved to the numpad** (NumLock must be on). New keys:

  | Function | Old key | New key |
  |---|---|---|
  | Reflections off / RT / debug views | F2 | Numpad 0 |
  | RT sun shadows | F11 | Numpad 3 |
  | Probe (logs what's under the crosshair) | F8 | Numpad . |
  | Reflection brightness | PageUp / PageDown | Numpad + / Numpad âˆ’ |
  | Soft shadows | F6 | Numpad / |
  | Leaf and fence cutouts | F7 | Numpad * |

  The numpad 1, 2, 4â€“9 keys are unchanged.
- The debug-only keys (lit test F4, skip test F3, PIX capture F10) have no key by default now; they can still be set in the ini.
- **StarfieldRT.ini starts with a key map** listing every key. Each key still shows what it switched on screen.
- **Check in-game:** with NumLock on, each numpad key shows its on-screen message.

## 0.4.0 - 2026-11-20

- **New: every lamp is ray traced (north star item 3, first version).** The game's own lamp lighting (point lights, spotlights, box-shaped area lights) is switched off, and the plugin draws those lamps itself:
  - **Shadows:** at every pixel, the 4 lamps that light it most get a shadow ray, so people, railings, signs and furniture cast real shadows under every lamp. Weaker lamps are added without a shadow test.
  - **Same lamps as the game:** brightness, colour, falloff and spotlight cones follow the game's own formulas, decoded from its tiled lighting shader. The surface response matches the game's too: diffuse with 1/Ï€, GGX specular, metals tinted.
  - **Speed:** lamps are first sorted into 16Ã—16-pixel tiles, so each pixel only considers the lamps that can reach it (up to 64).
  - **Combines with the game:** the result goes into the game's own indirect-light targets, so its AO, fog, transparency and tone mapping apply as before.
  - **The sun is unchanged:** RT sun shadows as before.
- **How the game's lamps are switched off:** each frame the game writes which lamps to draw into a small buffer. The plugin reads those lists first (for its own use), then empties them for the game's lighting. That only happens while the plugin's lamp pass is actually running. If it stops (menus, loads), the game's lamps come back within 3 frames.
- **Key and settings:** **Numpad 2** switches between ray-traced lamps and the game's own lamps (on-screen message says which). New in StarfieldRT.ini: `bRTLamps`, `fRTLampScale` (brightness, 1 = the game's), `iRTLampKey`. The shadow ray count is the existing `iReflectionLocalShadowRays` (max 4).
- **Check in-game (Neon):**
  - Compare with Numpad 2. The brightness and colour of lamp light should roughly match the game's, now with shadows.
  - If lamp light is much darker or brighter than the game's, tell me which (fRTLampScale is the quick fix).
  - Look for people casting shadows under lamps.
  - Watch the frame rate and the `[lamps] RT lamps: applied` log line.
  - **Known side effect to watch for:** transparent things (glass, smoke) and fog may lose their lamp lighting, if the game feeds them from the same lamp list.

## 0.3.10 - 2026-11-20

- **0.3.9 result:** still flickering, and Numpad 4 (thin-glass reflections off) stops it. So the thin-glass pass is the cause, not GI. The F8 glass dumps on the smoke said "camera ray hit nothing", so it isn't reflecting the smoke itself. The window that reflects properly is material family 13, found by the fallback ray (not tagged as glass).
- **Fixed (likely): reflections flickering under smoke.**
  - The fallback that finds untagged windows (any object in front of the surface behind) also accepted the RT copy of the walkway floor, which is a non-standard material family like the window. 0.3.8 only excluded solid family-0 materials.
  - Its distance test ran along the ray, and at grazing angles a centimetre between the RT floor and the drawn floor becomes tens of centimetres along it.
  - Smoke switches the pass on and off over the floor every frame, hence the flicker.
  - Now a fallback hit must also be at least 20 cm off the plane of the surface behind it (measured with that surface's normal, so the viewing angle doesn't matter). Real panes stand well off the wall behind them.
- **Check in-game:** the walkway with thin glass on (Numpad 4 "on"): the floor under the smoke should be steady. Shop windows should still reflect.

## 0.3.9 - 2026-11-20

- **0.3.8 result:** still flickering (second tester video). Frames 300â€“303 show dark, smoke-shaped patches on the walkway floor that change every frame. 0.3.8 fixed a real but different problem in the glass pass.
- **Fixed (likely): GI flickering under smoke.** The "last frame's image" that GI rays read since 0.3.3 is captured after the game blends in transparent things, so the smoke was baked into it. A GI ray from the floor that hit a nearby surface behind the smoke read the smoke's dark, animated colour instead of the surface's light. The GI then flickered with the smoke.
- **The fix:** pixels covered by transparency (smoke, glass, effects) are now marked unusable in that image, using the game's own transparency coverage. GI rays that land there use normal hit shading instead. Neon signs are opaque, so they still light things.
- **Check in-game:** the same walkway. The floor under the smoke should be steady. Numpad 1 (GI off) shows whether any flicker that remains comes from the GI.

## 0.3.8 - 2026-11-20

- **0.3.7 result:** save loads work.
- **Fixed (likely): reflections flickering through smoke** (tester video, Neon walkway).
  - The thin-glass pass works only where the game says a see-through surface covers the pixel. Drifting smoke switches that on and off over the floor every frame.
  - Where it was on, the pass's "any object in front of the solid surface" fallback (for untagged windows) could take the RT copy of the floor itself for a glass pane. At grazing angles that copy sits slightly in front of the drawn floor.
  - So patches of floor under the smoke got an extra reflection that flickered with the smoke.
  - The fallback now ignores ordinary solid materials, and its hit has to be clearly in front of the surface behind it (15 cm + 3% of the distance). Real windows still qualify.
- **Check in-game:** the same walkway: the floor under the smoke should stay steady. Windows should still reflect (Numpad 4 debug: yellow/white).

## 0.3.7 - 2026-11-20

- **Fixed (likely): GPU hang right after a save load (0.3.6, and the same pattern in 0.2.58, 0.2.62 and 0.3.1).** Every one of these hangs happened within about a second after the plugin decided the scene was "ready", in a reflection, glass or GI pass. Right then the game is still streaming in and rewriting the scene's records; this time the TLAS was reallocated in the same moment. Two defences:
  - **A settling pause:** reflections, thin glass and GI now wait about 3 seconds after the scene reports ready. A TLAS reallocation in that window extends the wait. During the pause you see the game's own reflections and lighting, as during the load itself. RT sun shadows are unchanged.
  - **Bounds checks on texture lookups:** colour, glow and leaf-cutout textures are looked up by an index read from the material records. A half-written record can hold an index past the end of the game's descriptor heap, which is undefined on the GPU and can hang it. Such indices are now treated as "no texture".
- **Check in-game:** load saves several times, in Neon and elsewhere. After each load the RT effects should switch on a few seconds after the scene appears.

## 0.3.6 - 2026-11-20

- **New: glowing materials light the world even off screen.** The 0.3.5 F8 dumps showed where the game keeps a material's glow:
  - a 32-byte block at the start of its material record: strength, exposure offset, flags and tint colour;
  - a glow texture in slot +24 of its texture set. A plain wall has all of that empty.
- Rays (GI, reflections, thin glass) that hit a glowing material now get its light: tint Ã— glow texture Ã— strength. Neon signs behind you or around a corner light the floor and walls and show in reflections.
- **Strength model:** the game keeps glowing signs at a constant on-screen brightness whatever the camera's exposure ("adaptive emittance"), so by default the plugin does the same: 2^(exposure offset) on screen.
  - `iEmissiveModel=1` uses the absolute value instead (luminous emittance).
  - `fEmissiveScale` sets the brightness; 0 = off.
- **Not covered yet:** glowing screens and panels use a different material family (7) whose table isn't decoded. They still light things while on screen (0.3.3), not when off screen.
- **F8 also logs the sign's real on-screen colour and exposure at the crosshair** (`[G3] F8 screen colour at the crosshair`, needs thin-glass reflections on). Together with the material dump (`[R]   material entry`), that lets me match the strength exactly.
- **Check in-game:**
  - In Neon, turn so a big sign is just behind you. The floor and walls in front should keep a bit of its colour (compare `fEmissiveScale=0`).
  - Look at a shiny floor that reflects a sign from off screen.
  - If signs light things far too much or too little, press F8 on a sign and send the log.

## 0.3.5 - 2026-11-20

- **Diagnostic for off-screen glow (emissive materials).** F8 now also traces a ray through the crosshair. It logs the object it hits:
  - its material family and number;
  - the full 288-byte material record;
  - its texture-set record (the list of textures the material uses).
- Comparing a glowing sign with a plain wall shows where the game keeps a material's glow colour, strength and glow texture. That's what rays need, so signs off screen can light things too.
- **Check in-game:** in Neon, aim at each of these and press F8, waiting a second between presses:
  1. a bright neon sign;
  2. a glowing screen or light panel;
  3. a plain wall next to it.
- Then send the log: the `[R]   crosshair ray` lines.

## 0.3.4 - 2026-11-20

- **Less noise in RT reflections and GI.**
- **Reflections now blend over several frames.** Until now each frame's reflection rays were filtered and thrown away, so every frame had fresh noise. A new pass lines up last frame's result with the camera's movement and blends it in:
  - Rough surfaces keep much more of the past (their noise needed it most). Mirror-like surfaces react quickly, so they don't smear.
  - Last frame's value is clamped to what this frame's neighbouring rays allow, so moving lights and objects don't leave ghost trails.
  - `bReflectionTemporal=0` restores the old per-frame behaviour.
- **GI is calmer:**
  - Blends 32 frames instead of 16.
  - Ray directions follow an even pattern over time instead of random clumps, so it settles faster.
  - Last frame's light is clamped to this frame's neighbourhood (no lingering light from moved or switched-off lamps).
  - Freshly revealed areas, which have no history yet, get a wider blur instead of raw noise.
- **Check in-game:**
  - Stand still in Neon: reflections and GI should be much calmer.
  - Turn quickly: look for smearing or ghost trails on reflections, especially on shiny floors.
  - Toggle Numpad 1 and F2 to compare.

## 0.3.3 - 2026-11-20

- **New: glowing things light the world (first step toward emissive lighting).** At the end of every frame the plugin now keeps the final lit image (glass included) and each pixel's distance. Next frame, when a GI ray hits something that was on screen, it takes that surface's real lit colour instead of re-shading it.
  - Neon signs, screens, light strips and brightly lit walls now cast their colour onto floors, walls and people nearby.
  - Light now bounces more than once (multi-bounce GI), because last frame's image already contains last frame's GI.
- **Limitation:** glowing things that are off screen (behind you, around a corner) don't light anything yet. Rays that hit them fall back to the old shading. Fixing that needs the material's own glow values, which aren't decoded yet; that's next.
- **Setting:** `fGIScreenLight` (1 = physically based, 0 = off). Above 1 the light can feed on itself and slowly run away to full brightness.
- **Check in-game:** in Neon, stand near a big sign and press Numpad 1 to compare GI on and off. The floor and walls near the sign should pick up its colour. The debug view (Numpad 1 twice) shows the GI light alone. The log should show `[GI] last-frame image capture: applied`.

## 0.3.2 - 2026-11-20

- **Fixed (likely): GPU hang a few seconds after a save load (0.3.1).** The GPU stopped inside the reflection or GI pass right after the load finished. Probable cause, introduced in 0.2.67:
  - To hide your first-person head, the player's body became a shape rays inspect triangle by triangle.
  - Unlike leaves and fences, those player triangles weren't counted toward the per-ray safety cap (32).
  - Right after a load, a character's bone data can briefly be garbage, and the skinned body then spans the scene in huge triangles. Every ray then waded through thousands of them.
- **Two fixes:**
  - Player triangles now count toward the cap, so a ray can never stall on them.
  - The skinning step collapses any vertex that lands more than 50 m from its actor (was 10 km), so a garbage pose disappears instead of filling the scene.
- **Check in-game:** load saves a few times, especially in Neon. GI and reflections should continue normally after each load.

## 0.3.1 - 2026-11-20

- **New: hotkeys tell you what they did, on screen.** Every RT hotkey now shows a message top-left, using the game's own HUD notification, e.g. "Starfield RT - Global illumination: debug: RT GI light alone (grey surfaces)". It's sent from the game's main thread. `bOnScreenMessages=0` turns it off (the log keeps the lines either way).
- Covers: F11 sun shadows, F2 reflections, PageUp/PageDown brightness, F6 soft shadows, F7 cutouts, F8 probe, Numpad 1 GI, Numpad 4 thin-glass reflections, Numpad 5 game lamps, Numpad 6 thick-glass layer, Numpad 7 see-through surfaces, Numpad 8 game glass reflections, Numpad 9 view through glass.
- **Check in-game:** press Numpad 1 a few times; a message should appear each time. If the game crashes on a key press, set `bOnScreenMessages=0` and tell me (it would mean the HUD-message address is wrong for this game version).

## 0.3.0 - 2026-11-20

- **New: ray-traced global illumination (Phase 3 start).** The game's indirect ("bounce") light comes from baked probes. It's now replaced by ray-traced light:
  - Every 2Ã—2 pixel block sends one ray into the hemisphere above the surface.
  - Where the ray lands is lit the same way reflection hits are: the sun (with shadow), the game's own lamps (with shadows), plus half of the game's own ambient as a cheap second bounce.
  - Rays that reach the open sky see the game's ambient.
  - The result is accumulated over 16 frames (reprojected with last frame's camera, reset where the surface changed) and smoothed with an edge-aware filter.
  - It goes into the game's own indirect-diffuse target, so it combines with the rest of the game's lighting (AO, materials) as before.
- **Intense by default, as asked:** `fGIStrength=2.0` (1 = physically based). Lamps and sunlit floors should visibly bleed colour onto nearby walls and ceilings, and corners and spaces away from lights should get darker than the game's flat probe light.
- **Keys and settings:**
  - **Numpad 1** cycles GI on â†’ debug (GI light alone on grey surfaces) â†’ off (the game's own GI), for A/B.
  - New in StarfieldRT.ini: `iGI`, `fGIStrength`, `fGIBounce`, `iGIRayLength`, `iGIHistoryFrames`, `fGIFilterRadius`, `iGITraceScale`, `iGIKey`.
- **Check in-game:**
  - Compare Numpad 1 on and off in Neon and outdoors.
  - Watch for noise or crawling when standing still, smearing when turning fast, and the GPU cost in the `PERF` lines.
  - The log line `[GI] RT GI on: applied` confirms it runs. If it says `no history`, the smoothing over time isn't working.

## 0.2.67 - 2026-11-20

- **0.2.66 result:** NPCs and the player now show in reflections at Neon. But your own head appeared in reflections everywhere.
- **Fixed (likely): your own head in reflections (first person).** The game keeps your full third-person body at the camera and only hides the head in first person; the RT copy had the head. The plugin now spots the player's body (a character standing right under the camera) and ignores its hits within 0.45 m of the camera in reflection rays, light and sun shadow rays, and the glass pass. The rest of your body still reflects and casts shadows. In third person nothing changes (your head reflects normally).
- **Check in-game:** in first person the head should be gone from reflections; your body should still show in floor reflections when you look down near a shiny floor. The TLAS log line now shows `N first-person player parts`. If that stays 0, the detection missed and I'll adjust it.

## 0.2.66 - 2026-11-20

- **Fixed: NPCs and the player missing from RT reflections (and RT shadows) in Neon.** Skinned characters go into the RT scene only when the plugin knows the camera position, and at this location the build step can't find the camera constants (the same reason the F8 probe failed). So no character was ever added there. The build now uses the copy of those constants in the scene table, which also turns on the normal distance culling there.
- **0.2.65 result:** the metal "window" gets RT (metal 0.98, roughness 0.17, RT applied). The "mapping" look you see is part of the material itself, most likely a fake-interior or screen effect drawn into its colour, not a reflection. Leaving that as is.
- **Check in-game:** NPCs and your character (in third person) should now show in floor and glass reflections at Neon. The log should show `skinned characters: N candidates â€¦ M in the RT scene` every ~10 seconds.

## 0.2.65 - 2026-11-20

- **0.2.64 result (F8 dumps):** on the smoke, the glass pass's ray now hits nothing, so the plugin no longer draws a reflection there. The reflective look left on the smoke comes from elsewhere, most likely the floor's RT reflection showing through the haze. On the metal "window" that still shows the game's old reflection, the glass pass never ran: it's a solid G-buffer surface, handled by the main reflection pass, which keeps the game's reflection only for some material types.
- **New: F8 also dumps the main reflection pass at the screen centre** (`[R] F8 dump`): G-buffer material type, roughness, metalness, whether RT replaced the reflection, the game's reflection value and the albedo. That shows why a surface keeps its old reflection.
- **Check in-game:** F8 on the metal window again, then send the log.

## 0.2.64 - 2026-11-20

- **0.2.63 result:** save loads worked, and the F8 dumps answered the smoke question. The reflective smoke sheet uses **material 0**, the game's blank default material (LOD dword 0, all-zero entry, flags 0). Flags 0 read as "see-through", so the plugin tagged it as glass. The real window uses a normal material (family 13) and was found through the fallback that hits any object in front of the wall.
- **Fixed: no more reflections on smoke and haze.** Anything on material 0 now gets its own ray mask (effects, 0x04). Reflection and glass rays skip it. The "any object in front of the wall" fallback skips it too, so windows still reflect and smoke doesn't.
- Sun shadows are unchanged: they still include these meshes, as before.
- **Check in-game:** the yellow haze and the walkway smoke sheet should no longer reflect, and windows should. With Numpad 4 â†’ debug view, smoke shows magenta (no reflection) and windows yellow or white.

## 0.2.63 - 2026-11-20

- **Fixed: GPU crash on save load (0.2.58 and 0.2.62).** The thin-glass pass reuses the main reflection pass's inputs (scene records, material tables, G-buffer table) for a few passes. During a load it is skipped before its "inputs are getting old" counter ticks, so on the first frame after the load it reused inputs from before the load. The load frees and reallocates those buffers, so the GPU read freed memory and hung. Now the remembered inputs and the lights copy are dropped whenever the scene starts or stops loading, and whenever the glass pass is skipped for a load. The glass pass waits for the main pass's first post-load inputs.
- **Check in-game:** load saves a few times (that's where it crashed). Then do the F8 test from 0.2.62: smoke, then a window.

## 0.2.62 - 2026-11-20

- **0.2.61 result:** F8 still logged nothing in Neon; the shadow-pass probe doesn't run there. The screenshots show the problem clearly: a yellow haze volume and a see-through smoke sheet in the walkway both pick up mirror reflections, because to the thin-glass pass they look like glass.
- **New: F8 also dumps what the thin-glass pass hits at the screen centre.** It goes straight into the pass that draws those reflections, so it works wherever they appear. Logged as `[G3] F8 dump:` with the instance, whether it's tagged as glass, its material flags and the full material record. Comparing smoke with a real window gives the rule to filter smoke out.
- **Check in-game:** F8 with the crosshair on the reflective smoke, then on a reflective window, then send the log.

## 0.2.61 - 2026-11-20

- **Fixed: F8 probe did nothing in interiors like Neon.** The probe runs inside the sun-shadow step, and there that step can't find the camera constants (`sun shadows: skipped: PerSceneConstants not found`), so the probe was never recorded. It now falls back to the copy of the camera constants in the scene table. RT sun shadows are unchanged there (still off).
- **Why now:** to filter out the odd see-through shape in the Neon walkway that picks up a reflection, we need its material, and F8 logs the material of whatever is at the screen centre.
- **Check in-game:** press F8 on the shape and on a normal window. Each press should log `PROBE primary ray HIT â€¦` and `PROBE   primary hit material: â€¦`.

## 0.2.60 - 2026-11-20

- **Fixed (likely): flickering game-light reflections (0.2.58).** Two causes:
  - The game rewrites its light list into a rotating memory ring every frame. The plugin kept the list's address and read it later from another thread, sometimes after the ring had reused that memory for other data. Now the game's own lighting command list copies the lights into the plugin's own buffers the moment it binds them. The GPU runs lighting â†’ copy â†’ reflections in order, so reflections always read a complete, current copy.
  - When a thread reused the same descriptor table on a later frame, the plugin skipped it as "already seen". The lights then counted as too old and switched off for a frame. Now every bind is checked, with a quick three-lookup test.
- **Check in-game:** the same spots should no longer flicker. If they still do, press Numpad 5 (game lights off). If the flicker stops, it's still the lights; if not, it's something else (send a short video).

## 0.2.59 - 2026-11-20

- **Fixed: GPU hang on save load (0.2.58).** The log shows the GPU stopped inside the thin-glass reflection pass right as the save started loading. The main reflection pass already pauses while the scene loads, because geometry is being freed and rebuilt then, but the thin-glass pass didn't, so it traced into freed geometry. It now pauses during loads too, and it only uses the current frame's ray-tracing scene, never an older one.
- The game-lights code from 0.2.58 found the light list correctly on the first try (`[L] light table seen â€¦ lights at +3`); this crash wasn't caused by it.
- **Check in-game:** load the same save. It should load without crashing. Then check the 0.2.58 lamp lighting in reflections (Numpad 5 toggles it).

## 0.2.58 - 2026-11-20

- **Reflections are lit by the game's own lamps.** Until now a reflected object was lit only by the sun (with shadow) and a flat ambient, so in interiors with no sun (like Neon) reflections came out dim and flat. The plugin now reads the game's own light list, the one its tiled lighting uses: up to 512 point, spot and box-shaped area lights per frame. Each reflected point gets light from every lamp that reaches it, with the game's own falloff and spotlight cones. The first 4 lights that reach a point also get a shadow ray, so lamps behind walls don't light it.
- Applies to all RT reflections (floors, glass doors, thin glass).
- **New settings:** `bReflectionLocalLights`, `fReflectionLocalLightScale` (brightness, default 1.0), `iReflectionLocalShadowRays` (default 4). **Numpad 5** toggles game lights in reflections live, for A/B.
- **Check in-game:** in the same interior, reflections should now show lit lamps, glowing ceiling panels and coloured light pools instead of flat grey. Press Numpad 5 to compare. If everything goes black, blows out, or the lights are in the wrong places, send the log: the line `[L] light table seen` shows how the light buffer was found. Also watch the frame rate: this adds a light loop to every reflection ray.

## 0.2.57 - 2026-11-20

- **0.2.56 result:** thin-glass reflections work. In the debug view the shop windows show yellow (reflected), but in-game they're faint, visible mainly where it's dark behind the glass. That's because glass reflects only ~4% head-on, and in interiors (no sun) what it reflects is lit only by a flat ambient light; the game's own lamps aren't used yet.
- **Changed:** `fThinGlassStrength` now defaults to 3.0 (was 1.0), so window reflections read clearly. Tune it in StarfieldRT.ini.
- **Known issues:** some smoke/steam columns also count as glass (yellow in the debug view) and get a reflection. Reflected objects in interiors are flat and dim until reflections use the game's lights.

## 0.2.56 - 2026-11-20

- **0.2.55 result (debug view):** the window pixels came out magenta. The game's glass coverage works, but our camera ray never found the pane, so no reflection was drawn. Either the pane isn't tagged as see-through in our scene (it gets the solid mask), or the depth limit cut the ray short.
- **Fixed (likely):** the coverage check is back on (it works). Inside covered pixels the ray first looks for glass-tagged objects, then for any object in front of the solid surface, which must be the glass.
- **Debug view (Numpad 4, second press) colours:** blue = no glass there; magenta = glass there but our ray found nothing; red = the depth buffer leaves no room; yellow = reflected, glass-tagged; white = reflected, untagged.
- **Check in-game:** the same window should show a reflection. In the debug view it should be yellow or white, not magenta.

## 0.2.55 - 2026-11-20

- **RT reflections on all thin glass.** The thin-glass pass no longer waits for the game's transparency data (OIT moments) to say glass covers the pixel. That check is the main suspect for "runs but nothing shows". Now any see-through surface the camera ray finds in front of the solid scene gets an RT reflection. This includes some effect panels (holograms, light cards), so expect a few wrong reflections. We'll trim those afterwards. `fThinGlassMinCoverage` (new, default -1 = off) brings the gate back.
- **Numpad 4 now cycles: on -> stage debug view -> off.** The debug view tints every pixel by how far the glass pass got: **blue** = nothing found, **red** = the game says glass covers it, **green** = our ray hit a see-through surface (**yellow** = red + green), **white** = its reflection ray hit something too. If the whole screen doesn't turn bluish, the pass's output isn't reaching the screen at all.
- **Check in-game:** look through a window. You should see a faint reflection on it. If not, press Numpad 4 once and screenshot the debug colours.

## 0.2.54 - 2026-11-19

- **0.2.53 result:** still no RT at the new location (`no TLAS this frame`). The log answered the question: the RT build pass's compute root table 1 there is a handle in **no CBV/SRV/UAV heap** (0xc5678aâ€¦, the sampler-heap range), so that pass doesn't bind the scene table at this location. The fallback did kick in, but the scene table **moves to a new heap slot every frame** (indices 64517, 68613, 72197, 74536 â€¦). The remembered one was a frame old, its slots already reused, so the build still failed.
- **Fixed (likely):** the plugin keeps a ring of every distinct root-table-1 handle the game binds (compute and graphics). When the build pass's own table 1 isn't the scene table, it uses the newest recently bound one that holds the scene table right now (instances at +2, 96-byte records), so it's from the same frame.
- **Check in-game:** at the same location, RT shadows and reflections should work, and with them thin-glass reflections (Numpad 4) and F8. The log line `using the scene table recently bound by another pass` confirms the fallback.

## 0.2.53 - 2026-11-19

- **0.2.52 result (new location):** still no thin-glass reflections, Numpad 4 did nothing visible, and F8 returned no probe. The log shows why: about 15 seconds after loading, **the RT scene stopped being built at all** (`no TLAS this frame`, plugin GPU time 0.00 ms), so every RT effect was off there. The scene table the build reads (the contact-shadow blur pass's compute root table 1) was not in any tracked descriptor heap (`global scene table at heap index 4294967295`), and none of the scene buffers could be found.
- **Fixed (likely):** the scene table is now validated (instances at +2 with 96-byte records). Every valid sighting is remembered, from the RT build pass or from GatherIndirect's graphics table 1. When the build pass's table 1 is something else, the build uses the last valid table, and so do the shadow and reflection inputs. The first fallback logs the bound handle and every tracked heap, to show whether the table moved or the plugin lost track of a heap.
- **Check in-game:** at the same location, RT shadows and reflections should work again, and with them thin-glass reflections (Numpad 4) and F8. Send the log either way: look for `root table 1 ... is not the scene table`.

## 0.2.52 - 2026-11-19

- **0.2.51 result:** no reflections on thin glass, and Numpad 4 did nothing visible. The log shows the glass pass almost always said `skipped: no reflection pass this frame`. The game records render passes on several threads, so the OIT composite is often recorded before that frame's reflection pass, and the glass pass insisted on same-frame inputs.
- **Fixed:** the glass pass uses the reflection inputs from up to 8 passes ago (the scene records are persistent buffers) and always this frame's TLAS.
- **Crash while running around** (crash log 2026-11-19-21-15-05): an access violation in a memory copy inside the game's own render scheduler (Starfield.exe+2A0477E), with no plugin code in the call stack. The cause is not known yet; watch whether it happens again with this build.
- **Check in-game:** thin glass should now show reflections; Numpad 4 toggles them.

## 0.2.51 - 2026-11-19

- **New (glass step 3): RT reflections on thin glass.** Thin glass (windows, panes) is not in the G-buffer, so the main reflection pass never sees it. A new pass runs right after CompositeMomentBasedOIT has blended the see-through layer into the HDR scene target:
  1. Only pixels where the game's OIT moments show a blended surface (b0 > `fThinGlassMinCoverage`, half-res moment target from composite table 3 +2) are considered. Additive effects (light beams, glows) don't write moments, so they are left alone.
  2. A camera ray against the see-through instances only (mask 0x02) finds the glass surface in front of the solid scene. Thick glass at the solid depth is excluded, since it already gets RT reflections.
  3. A mirror ray from the glass, shaded like the main pass (sun, sun shadow, alpha-tested foliage), is added with the Fresnel of real glass (F0 0.04). On a miss, a sky-ish ambient is used.
- Settings: `[RT] bThinGlassReflections=1`, `fThinGlassStrength=1.0`, **Numpad 4** toggles live. The log line `[G3] thin-glass reflections:` says whether the pass runs, or why not.
- **Check in-game:** windows and glass panes should now reflect the scene, more strongly at grazing angles. Compare with Numpad 4. Watch for see-through effects (holograms, smoke, force fields) that suddenly reflect like glass, and check the log line.

## 0.2.50 - 2026-11-19

- **0.2.49 result:** there are no plants near the doors, so the leaf theory was wrong. Pressing **Numpad 7** (skip the see-through materials tagged in 0.2.44) removed the big blobs from the door reflection, a half-dome shape among them. The reflected NPC behind it became visible. The same key also changed the sun shadows, because it applied to shadow rays too.
- **Cause:** see-through materials that the game draws as overlays (glass, light-effect panels, and similar) are in the ray-tracing scene, and reflection rays treated them as solid walls.
- **Changed: reflections skip see-through materials by default** (`bGlassSeparate=1`, Numpad 7 still toggles it). **Sun shadows hit everything again**, as before 0.2.44, because the tag covers ~11% of instances and may include real shadow casters. Letting sunlight through windows needs a finer glass test (later).
- The 0.2.49 alpha test on reflection rays stays: it is correct for real plants elsewhere (reflected leaves are cut out like their shadows).
- Faint diagonal light streaks on the door glass appear with Numpad 7 on and off, so they come from something else (to investigate).
- **Check in-game:** the doors should look like the Numpad 7 screenshot without pressing anything, and sun shadows like before 0.2.44.

## 0.2.49 - 2026-11-19

- **0.2.48 result:** the thick-glass see-through layer was skipped (thousands of batches in the log), yet the yellow-green streaks on the doors stayed. Up close, the doors now show a clear, sharp RT mirror of the player, the plaza and the trees, with large smeared slabs and diagonal streaks mixed in. From farther away the slabs dominate.
- **Cause:** the streaks are the RT reflection itself. Palm fronds and other plants are flat cards with a cut-out (alpha-tested) leaf texture. Sun-shadow rays have run the alpha test since 0.1.23, but reflection rays treated every card as solid, so reflected palms became big smeared slabs.
- **Fixed:** reflection rays (the main reflection ray, the sun-shadow ray at each reflection hit, and the debug views) now run the same alpha test as the shadow pass (`AlphaTest.hlsli`, vegetation materials and alpha params from scene table +13 / +28). At most 32 alpha candidates per ray, so dense foliage can't hang the GPU.
- **Check in-game:** the doors from far and close should show plants as leaves, not slabs. Compare reflection cost (`PERF reflections`) with earlier: foliage-heavy views may cost a little more.

## 0.2.48 - 2026-11-19

- 0.2.47 was built but never installed (the game was running). This build includes it.
- **Tester report:** up close the doors show the RT reflection, but an old, warped reflection sits on top of it ("making a mess").
- **Cause (most likely):** thick glass is drawn twice. The solid G-buffer layer gets the RT reflection. Then the see-through layer in the transparency pass refracts a copy of the finished scene, and in that copy the door already shows its reflection. The result is a warped, blurred second reflection on top. The earlier "interior" shapes were this duplicate (palm plants and the plaza ring, reflected).
- **New: the thick-glass see-through layer is hidden** (`[RT] bHideThickGlassLayer=1`). In the transparency pass, ExecuteIndirect batches whose table 4 +7 is a single TextureCube (the material's own cube, which only thick glass binds; thin glass binds the probe array) are skipped. **Numpad 6** toggles it live.
- **Check in-game:** the doors should show only the clean RT reflection. Compare with Numpad 6. If the door turns too dark or loses its tint, say so.

## 0.2.47 - 2026-11-19

- **0.2.46 result:** in the material type view, the door glass is **type 2** (green). The NPC is type 4 (yellow), and the frame, floor and walls are type 0.
- **Changed: glass (type 2) reflects mirror-sharp by default** (`iSmoothGlassTypes=4`, i.e. 2^2; `fGlassRoughnessScale=0.0`). The doors' RT reflection is no longer blurred by the game's smudge roughness. For a little softness, raise `fGlassRoughnessScale` (for example 0.3). Set `iSmoothGlassTypes=0` to keep the game's roughness.
- **Check in-game:** the doors should show a clean reflection of the plaza. Look for other type-2 surfaces that suddenly turn mirror-like; if they shouldn't, tell me.

## 0.2.46 - 2026-11-19

- **0.2.45 result:** sunlight now passes through the small glass panes. At the doors, the **coverage** view is solid green (RT replaces the reflection), the **roughness** view shows the glass as fairly smooth but not mirror-smooth, with smudgy patches, and the **mirror-hit** view shows a crisp, correct reflection of the plaza and the characters. So the blurry reflection on the doors is the RT reflection itself: the game gives that glass some roughness, so the rays scatter and the result is blurred, like a smudged mirror. The coverage view also showed the door glass has a non-zero material type, unlike the frame around it.
- **New: F2 debug view 7, "material type".** Each G-buffer material type gets its own color: 0 black, 1 red, 2 green, 3 blue, 4 yellow, 5 cyan, 6 magenta, 7 white, 8 orange, 9 purple, 10 teal, 11 pink, 12 grey, 13 brown, 14 lime, 15 navy.
- **New: smooth glass by material type.** `[RT] iSmoothGlassTypes` is a bitmask of material types to treat as glass (type n = 2^n), and their roughness is multiplied by `fGlassRoughnessScale` (0 = mirror-sharp). Default 0 (off) until the doors' type is known.
- **Check in-game:** at the doors, press F2 until the log says `material type` and note the doors' color.

## 0.2.45 - 2026-11-19

- **0.2.44 result:** sunlight still doesn't come through the window that was tested, and toggling Numpad 7 changed nothing there. The doors still show the old reflection. The log shows the glass flag (material +108 bit 0 clear) tagged **~22,600 of ~200,000 instances (11%)**: far more than glass, and the blocking window was evidently not among them.
- **Changed: glass tagging is off by default** (`bGlassSeparate=0`), until glass can be identified reliably. Numpad 7 still toggles it.
- **Correction:** the 0.2.40 door test was flawed. Turning RT reflections Off restores the game's own reflection on every G-buffer surface, the thick door glass included, so "the blur stays" did not prove it was the interior.
- **New: F2 debug view 6, "coverage".** For each pixel it shows what the RT reflection pass does: **green** = RT replaces the reflection (brighter = stronger), **red** = left to the game because the material type is 1 or 8 (treated as skin/hair), **yellow** = left to the game for another reason, **grey** = no surface. Blue adds the material type.
- **Check in-game:**
  1. At the doors, press F2 until the log says `coverage` and take a screenshot.
  2. Stand on the floor where sun should come through a window, put the screen center on that floor and press F8. The probe log names what blocks the sun ray, with its material flags.

## 0.2.44 - 2026-11-19

- **0.2.43 result:** F8 no longer crashes. Thin glass has no RT reflection yet (expected). The F8 probe's grid found 2-triangle, 4 m panels (mesh 3553, material 281) that are **in the ray-tracing scene but invisible on screen**, and they **block RT sunlight** from 0.2â€“7.5 m away. Those are window panes. Their material flags (+108) are **0x12**, while solid geometry next to them has **0x11**. So glass is in the RT scene, and it has been treated as a solid wall by every shadow and reflection ray.
- **New (glass step 3, part 1): glass is separated in the RT scene.** When the RT scene is built, standard materials with flags bit 0 clear are tagged as glass (instance mask 0x02; everything else 0x01). Sun-shadow and reflection rays only hit mask 0x01, so **sunlight now passes through windows** and reflections no longer hit invisible panes. The glass stays in the scene for the upcoming glass-reflection pass.
- The TLAS log line now counts the instances tagged as glass. **Numpad 7** toggles the separation live (`[RT] bGlassSeparate=1`, `iGlassSeparateKey=103`), to compare shadows through windows.
- **Check in-game:** stand where sunlight comes through a window. The patch of light on the floor should now appear, and pressing Numpad 7 should make it disappear again. Watch for anything solid that suddenly stops casting an RT shadow: that would mean the glass flag is wrong for it (press Numpad 7 to confirm).

## 0.2.43 - 2026-11-19

- **0.2.42 result:** pressing **F8** at the doors **crashed the game** (access violation in StarfieldRT.dll, crash log 2026-11-19-20-15-44).
- **Fixed: the F8 crash.** F8 also runs the glass survey, which lists every entry of each glass draw's descriptor tables. Unused slots in those tables can still hold old entries for textures the game has already freed. The survey called `GetDesc` on such a texture and crashed. The plugin now records each texture's size, mips, dimension and format when its view is written, while the texture is alive. The survey and the glass swaps (G2, sharp glass) read those saved values and never call into a texture from a table.
- **Check in-game:** press F8 on the doors and on thin glass again (the probe test from 0.2.42). It should log and not crash.

## 0.2.42 - 2026-11-19

- **New: sharp view through glass.** The game blurs whatever is behind glass. The glass draws read the scene from a 6-mip copy of the scene color (transparency table 4, slot +15) and sample blurrier mips for rougher glass. While the glass pass records, that slot is rewritten as a view of the sharp top mip only, so every glass draw sees the scene sharply. Frosted glass turns clear too.
- **Numpad 9** toggles it live (Num Lock on); `[RT] bSharpGlass=1` (default on), `iSharpGlassKey=105`.
- **Next glass step (G3, RT reflections on thin glass):** first find out whether glass is in the ray-tracing scene. Aim the screen center at thin glass with open space behind it and press **F8**. If the log's `PROBE primary ray` line says `IN FRONT of the visible surface`, the glass is in the scene, and its instance and material records follow.
- **Check in-game:** the doors and other glass should show the interior sharply (compare with Numpad 9). Thin-glass reflections are unchanged (none yet).

## 0.2.41 - 2026-11-19

- **0.2.40 result:** the game's reflection is gone from all glass. The blur left on the thick doors stays with every reflection off: it is the interior seen through the glass (the game's refraction blur), not a reflection.
- **Changed: the glass-reflection toggle is now Numpad 8** (was Home), at the tester's request. It needs Num Lock on; with Num Lock off the key sends Up Arrow instead. `[RT] iGlassReflectionKey=104`.

## 0.2.40 - 2026-11-19

- **0.2.39 result:** thin glass is now clear (the game's reflection is gone). The thick doors still had the blurry reflection. The log showed why: some glass draws bind a **single static cube map** (TextureCube, the material's own environment map) at the same slot +7, instead of the probe array. 0.2.39 left those alone.
- **Fixed:** single cube maps at +7 in the glass pass are now swapped for a black cube of the same format too. The log line says `probe array` or `material cube`.
- **Check in-game:** the doors should show only the RT reflection. Home still toggles the game's glass reflections.

## 0.2.39 - 2026-11-19

- **0.2.38 result (G1 survey):** glass is drawn in two places.
  1. **DeferredGBufferTranslucent** writes "thick" glass into the main G-buffer: the same albedo, normal and material textures the RT reflections read. So that glass gets an RT reflection.
  2. **MomentBasedOITRenderTransparency** then draws the see-through layer (ExecuteIndirect, into a full-res RGBA16 target that CompositeMomentBasedOIT blends into the scene). It samples the game's reflection-probe cube array at root table 4, slot +7: the same 256Ã—256, 198-face, 7-mip array the opaque scene uses.

  That is why the doors showed both reflections, and why thin glass shows only the game's.
- **New (G2): the game's probe reflection is hidden on glass.** While MomentBasedOITRenderTransparency records, each table-4 block it binds gets slot +7 rewritten to point at a black cube array of the same shape. The pass has its own table blocks (two, alternating per frame), so only glass and other see-through draws are affected. Water, particles and solid surfaces keep the probes.
- **Home** toggles it live; `[RT] bHideGameGlassReflections` (default 1) sets the start state.
- Expect: the doors from the screenshots show only the RT reflection. **Thin glass shows no reflection at all for now.** Giving it an RT reflection is step G3, which needs the glass surface itself (it isn't in the G-buffer).
- **Check in-game:** stand at the doors and press Home a few times to compare. Look for anything else see-through that turns too dark (holograms, visors, bottles); those use the same pass.

## 0.2.38 - 2026-11-19

- **New: G1 glass survey (diagnostics only, nothing changes on screen).** The plugin now also hooks 16 transparent and forward render passes: Forward, the ForwardBlended* family, the moment-based OIT passes (RenderMoments, RenderTransparency, RenderAdditiveTransparency, CompositeMomentBasedOIT), DeferredGBufferTranslucent and RefractionMask. Their addresses come from CommonLibSF's vtable IDs and the 1.16.244 address library, with Execute read from the exe (`tools/pass_vtables.py`). The method was checked against GatherIndirect, which gives the same addresses the plugin already used.
- Each pass logs `[G1] <pass> executes` the first time it runs. About 10 seconds into the first loaded scene, and again on every **F8** press, a 3-frame survey logs each pass's first two executions: up to 6 draws each (ExecuteIndirect included), with the pipeline, render targets, viewport and every root binding.
- Why: glass is drawn after the solid scene, with its own probe reflection. To remove that reflection and add RT reflections to glass, the plugin first needs to know which pass draws it, which targets it writes, and which bindings hold the reflection cubes.
- **Check in-game:** stand in front of glass (for example the doors from the screenshots) and press F8 once. No visual change is expected.

## 0.2.37 - 2026-11-19

- **0.2.36 result:** the Gameplay Options group works, and toggling RT Reflections and RT Shadows applies live. No crash in about 6 minutes in the crowded area (Frame Generation off). Reflections cost about 2.9 ms on High. There's still a reflection **ring close to the player** on floors. Glass shows the game's blurry reflection on top of the RT one, and some glass has no RT reflection at all.
- **Fixed: the ring near the player.** In replace mode, pixels with a very faint reflection still kept the game's own reflection. On a floor that happens near your feet, where you look down at the surface (little Fresnel), so the game's reflection showed inside a ring and RT outside it. Replace mode now traces every reflective pixel (`replaceMinWeight` 0), so the game's reflection never shows. This costs a little more on rough, dull floors.
- **Not fixed yet: glass.** Glass is drawn later, in the game's transparent pass (LayeredEffect, composited by CompositeMomentBasedOIT), with its own probe reflection. RT reflections only replace the reflections of solid surfaces. Thick glass that is also drawn as a solid surface gets both. Thin glass gets only the game's. This needs its own investigation (next).
- **Check in-game:** the floor near the player should have no ring. Compare the `PERF reflections` cost with 0.2.36 (~2.9 ms on High).

## 0.2.36 - 2026-11-19

- **New: RT quality in Gameplay Options.** A new plugin, `StarfieldRT.esm` (a light master: it doesn't use a load-order slot), adds a **Starfield RT** group to Settings > Gameplay Options with **RT Reflections** and **RT Shadows**, each Off / Low / Medium / High / Ultra (default High). Changes apply live, with no restart. The choice is saved with the game, like other gameplay options.
- How it works: a small quest script (`SFRT_OptionsQuest`) copies the two options into the globals `SFRT_ReflectionQuality` and `SFRT_ShadowQuality` whenever they change and on every save load. The DLL reads them every 30 frames and reapplies the quality levels. The levels are the same as the ini's (see 0.2.34).
- The menu wins over the ini's `iReflectionQuality` / `iShadowQuality`. Setting either to `-1` (custom) in the ini makes the plugin ignore the menu for that effect. Without the esm, the ini works as before.
- A menu change also cancels an F11 shadow toggle and resets the F2 debug view (to RT on or off). PageUp/PageDown brightness is kept.
- Install: the mod folder now also has `StarfieldRT.esm` and `Scripts\SFRT_OptionsQuest.pex`. **Enable `StarfieldRT.esm` in MO2's plugin list** (right pane).
- **Check in-game:** the group shows in Gameplay Options. Changing RT Reflections to Off removes the RT reflections within a second. The log shows `Gameplay Options found` and a `quality (Gameplay Options)` line for every change.

## 0.2.35 - 2026-11-19

- **0.2.34 result:** GPU hang after about 1.5 minutes in the crowded area (Frame Generation off). Unlike the earlier Frame Generation hangs, this time DRED showed the GPU stopped inside the plugin's own command list, at the first **skinned-character BLAS build** (right after the skinning dispatches and their barrier). Reflections cost 0.8â€“1.1 ms at medium (was 2.2â€“3.0 ms at full resolution), so the half-resolution tracing works.
- **Fixed: hang building character BLASes.** A character's triangle indices were read straight from the game's shared index pool, using mesh information from three frames earlier. If that character's mesh streamed or changed in between, the indices could point past the vertices the plugin had written. D3D12 leaves that undefined, and on NVIDIA it can hang the GPU. The skinning pass now copies each character's indices into the plugin's own buffer, clamped to the vertex count, and the BLAS is built from that copy. A garbage mesh scale now also collapses the mesh to a point instead of producing NaN positions.
- **Changed: reflections default to High** (one ray per pixel, characters within 30 units). Shadows were already High. Existing ini files keep their value: set `iReflectionQuality=3` (the new ini in the mod folder already has it).
- **Check in-game:** the same crowded area for several minutes, with Frame Generation still off.

## 0.2.34 - 2026-11-19

- **0.2.33 result (Frame Generation off):** no hang in the crowded area. VRAM peaked at 10.6 of 15.2 GB (69%), so memory pressure is ruled out, and the hangs point at a Frame Generation interaction (to be revisited). Frames were 19â€“20 ms: plugin ~5 ms (reflections 2.2â€“3.0, TLAS 1.2, shadow rays 1.0â€“1.4, filter 0.2). Glass windows show no RT reflections, because glass is drawn in a later transparent pass that the plugin doesn't touch yet. On a flat wall the RT reflection ended in a hard **circle**.
- **Fixed: the circle.** Very faint reflections were cut off at a hard threshold. The reflection weight follows the viewing angle, so on a flat wall the threshold traced a circle: RT inside, nothing outside. Faint reflections now fade smoothly to the game's value, and the threshold in replace mode is halved (0.01), so flat walls reflect all the way out.
- **Faster: half-resolution reflection tracing.** Medium and low quality trace one ray per 2Ã—2 pixels (the traced pixel rotates every frame) and upsample. The upsample weights each sample against every full-resolution pixel's own normal, distance and roughness, so object edges stay sharp. Expect roughly half the reflection cost or less.
- **New: quality levels** (`[RT] iReflectionQuality`, `iShadowQuality`: 0 off, 1 low, 2 medium, 3 high, 4 ultra, âˆ’1 custom):

  | | Low | Medium | High | Ultra |
  |---|---|---|---|---|
  | Reflections | 1 ray / 2Ã—2 px, 1000 units, no sun shadows in reflections, 16 character parts within 10 | 1 / 2Ã—2, 2000, shadows, 32 within 20 | 1 / px, 3000, 48 within 30 | 1 / px, 6000, 64 within 40 |
  | Shadows | 1 ray, 1000 units | 2 rays, 1500 | 4 rays, 2000 (the previous default) | 8 rays, 3000, full resolution |

  The defaults are reflections **Medium** and shadows **High**. With âˆ’1 the individual settings apply as before (new for reflections: `iReflectionTraceScale`, `bReflectionHitShadows`).
- The log prints the active levels at startup (`quality: reflections â€¦ shadows â€¦`).
- **Test:** the same wall: the circle should be gone. Compare frame time and the `PERF reflections` line with 0.2.33 (Medium should be clearly cheaper), and check that edges in reflections don't look jagged at half resolution. If you like, try `iReflectionQuality=3` for comparison.

## 0.2.33 - 2026-11-19

- **0.2.32 result:** characters joined the RT scene (up to 176 skinned instances within 30 units, 64 in the scene, about 200,000 skinned BLAS builds in 3 minutes). After running around a crowded area: a GPU hang. It had the same signature as the load hangs: only Frame Generation's lists unfinished, none of the plugin's. The frame took about 23 ms, and reflections cost 3.0â€“3.7 ms GPU.
- **Every hang so far** (0.2.6, 0.2.9â€“0.2.11, 0.2.22, 0.2.29, 0.2.32) happened under heavy GPU load (loads, crowds) and showed only Frame Generation waiting. The tester will now test with **Frame Generation off**, to tell an FG interaction apart from plugin bugs; FG is to be revisited later.
- **New diagnostic: VRAM.** Every 600 frames the log prints local video memory used against the OS budget (and the peak), plus once more when the device is removed: `VRAM (â€¦): X of Y MB budget used (Z%), peak â€¦`. If the use approaches the budget before a hang, memory pressure is the cause.
- **Lighter character defaults:** `iSkinnedRadius=20` (was 30) and `iSkinnedMax=32` (was 64), roughly half the per-frame skinning and BLAS work.
- **Test:** turn Frame Generation off in the game's settings, then run around the same crowded area and look at NPCs in reflections. Send the log either way: it shows whether it hangs without FG and how close VRAM gets to the budget.

## 0.2.32 - 2026-11-19

- **Request:** sharper reflections, and NPCs in them.
- **New: characters in the ray-traced scene.** Skinned meshes (NPCs, creatures, the player) used to be left out entirely, so they never showed up in RT reflections or cast RT shadows. Now:
  - The TLAS builder lists skinned instances within `[RT] iSkinnedRadius=30` units of the camera.
  - The nearest `iSkinnedMax=64` get a slot (a character is several instances: body, head, hair, outfit).
  - Each frame a compute pass poses each slot's vertices exactly the way the game's vertex shader does: up to 8 bones per vertex from the bone-weight stream (mesh +28) and the instance's bone matrices (global table +9). Characters the game has already posed (LOD position handle bit 30) are copied as they are.
  - A fast BLAS is rebuilt from the posed vertices every frame, so animation is followed exactly.
  - Characters appear in reflections and now also **cast RT sun shadows**. The game's own character shadows are still drawn, so edges may look slightly darker where both overlap.
  - The log prints `skinned characters: N candidates â€¦, M in the RT scene` about every 10 s.
- **Sharper reflected textures: ray cones.** The texture detail level at a ray hit used to be a guess from distance alone, far too blurry up close. It now comes from the ray's real footprint (screen-pixel angle Ã— distance travelled, widened by roughness) and the hit triangle's own texel density, as in "Improved Shader and Texture Level of Detail Using Ray Cones" (2021). Near reflections now use full-resolution textures.
- **Less glossy blur:** `fReflectionBlurPx` now defaults to 10 (was 16), so near-mirror surfaces stay crisper.
- Known limit: a reflected character is lit using its resting-pose surface direction, so lighting on moving limbs may be slightly off. Their shape is exact.
- **Test:** stand near NPCs on a shiny floor or by a glossy wall (the Trident pillar area works): they should now appear in the reflection and move with their animation. Watch for frame-time changes (the `PERF reflections` and `PERF RT` lines, and the new `skinned characters` line), flicker, or characters' RT shadows looking wrong. Also check reflected textures look sharper (posters, floor patterns).

## 0.2.31 - 2026-11-19

- **0.2.30 result:** no crash on load. The TLAS buffers were resized only 3 times during the load (was dozens), which confirms the 0.2.29 hang theory. Glossy reflections were "better", and cost **1.6â€“2.2 ms GPU** indoors. The game's own cubemap reflection still showed through on the Trident poster.
- **New: RT replaces the game's reflections everywhere** (`[RT] bReplaceGameReflections=1`, the default). The game's cubemap reflection used to show in three places; now:
  - **Rough surfaces:** they get glossy RT reflections, with no more cross-fade to the cubemap above roughness 0.45.
  - **Very faint reflections:** pixels below a 0.02 weight get no reflection instead of the game's.
  - **Rays that hit nothing:** these still use the game's sky/probe colour, since that is what is really out there.
  - Skin and hair still keep the game's reflection (the game takes their roughness from a separate table).
- The glossy filter now uses 20 taps on rough surfaces (12 elsewhere) for the extra noise. `bReplaceGameReflections=0` restores the 0.2.29 behaviour.
- **Test:** the poster and other spots with F2 off/on: no cubemap image should remain on top of the RT reflection. Watch for grain on rough surfaces (walls, floors) and for anything that now looks too shiny or too dull. Send screenshots and the `PERF reflections` lines (this traces more pixels, so the cost will go up).

## 0.2.30 - 2026-11-19

- **0.2.29 result:** a GPU hang about 15 s into a save load. RT shadows and reflections were both paused for the load the whole time, so the new glossy reflections weren't running. DRED again showed none of the plugin's command lists unfinished, only Frame Generation's waiting.
- **Likely cause, found:** right before this hang, and before the 0.2.22 load hang, the TLAS buffers were reallocated several times a second (`TLAS buffers for 241664 â€¦ 248832 instances`). Their size followed the instance-record count, which rises 1,024 at a time while a scene streams in. Each step allocated about 110 MB of new GPU memory, and every replaced set is kept for 30 frames in case the GPU still uses it, so close to 1 GB was in flight at once during the load.
- **Fixed:** the TLAS buffers now grow by 1.5Ã— at a time, rounded up to 16,384 instances, so a whole load causes only a handful of reallocations.
- Glossy reflections (0.2.29) are unchanged and still need their first in-game test.
- **Test:** load a save a few times, including moving straight after loading. In the log, the `TLAS buffers for â€¦` lines should now be few and far apart. Then test the reflections as described under 0.2.29.

## 0.2.29 - 2026-11-19

- **0.2.28 feedback:** at the Trident pillar, the game's own reflection showed up on top of the RT one ("in the way"), and the smooth-ish floor panel outside became far too mirror-like. Starfield has no screen-space reflections, so the "game reflection" there is its cubemap. Both problems come from every RT ray being a perfect mirror:
  - On medium-rough surfaces, the 0.35â€“0.6 fade cross-faded a sharp RT mirror image with the game's blurry cubemap image, so both showed at once (ghosting).
  - A somewhat rough floor got a sharp, bright mirror where it should get a soft, dim reflection. Ray hits also skipped the game's specular occlusion.
- **New: glossy RT reflections (step R5's core).** Each pixel now traces one ray scattered the way that surface's roughness scatters light (GGX visible-normal sampling, a new pattern every frame). A second pass filters the samples with an edge-aware blur that respects depth, normals and roughness, with radius âˆ âˆšroughness (`[RT] fReflectionBlurPx=16`), then composites. Smooth surfaces stay sharp, rougher ones blur naturally, and the cross-fade to the game's reflection moves up to roughness **0.45â€“0.7** (was 0.35â€“0.6).
- **Ray hits now get specular occlusion** (the standard AO-based term) instead of plain AO, so creases and floors near walls reflect less.
- Expect some fine grain on rough surfaces, settling as DLSS accumulates. Temporal accumulation is the next quality step.
- **Test:** the same two spots (pillar and outdoor floor panel), F2 off/on: the double image at the pillar should be gone and the floor should look soft rather than mirror-like. Check for grain or shimmer while moving, and send screenshots plus the `PERF reflections` lines.

## 0.2.28 - 2026-11-19

- **0.2.27 result:** RT reflections work and blend in. Outdoors, a smooth floor panel near the transit entrance now mirrors the buildings and tower, where the game showed only a vague sheen. But indoors (the Trident pillar) they came out **darker than the game's** and hard to see. They also cost a lot: **2.6 ms GPU indoors, 7.8 ms outdoors**. No crash.
- **Fixed: reflected objects were too dark.** Reflected objects were lit by the game's probe light times their texture colour. The probe light is the light leaving the surfaces the probe captured, so it already contains their colour, and the texture colour was counted twice: roughly 2â€“3Ã— too dark. `fReflectionProbeAmbient` now defaults to **2.5** (was 1.0) to make up for it.
- **New: live brightness keys.** **PageUp** / **PageDown** make reflected objects brighter or darker (Ã—1.25 per press, probe light and sun together). The log prints the new `fReflectionProbeAmbient`; put that value in the ini to keep it. Keys: `[Debug] iReflectionBrighterKey=33`, `iReflectionDarkerKey=34`.
- **Faster: no rays where the reflection is too faint to see.** Where the game's own reflection weight is below `fReflectionMinWeight=0.045` (for example matte or non-metal surfaces seen head-on), the game's reflection is kept and no ray is traced. Lower it to give more surfaces RT reflections, at more cost.
- **Test:** at the Trident pillar and outdoors, toggle F2 (off/on) and use PageUp/PageDown until the reflections look as bright as the game's but sharper. Send the final `fReflectionProbeAmbient` from the log, screenshots, and the new `PERF reflections` numbers (indoors and outdoors).

## 0.2.27 - 2026-11-19

- **0.2.26 result (R3 passed):** reflections showed the right textures in the right places (floor tiles and arches in the Trident pillar, sushi adverts in a glossy ceiling panel). No crash. Reflected texture edges look slightly blocky.
- **New, RT reflections step R4: real RT reflections, on by default** (`[RT] iReflections=1`). On smooth surfaces they replace the game's cubemap reflection, weighted exactly the way the game weights its own (decoded from `Indirect#0`):
  - **Same surface response:** the game's split-sum weight is recomputed per pixel: Fresnel from albedo and metalness (`F0 = lerp(0.04, albedo, metal)`, roughness-aware), times the game's BRDF lookup texture, times its exposure (PerScene rows 311â€“312). Matte surfaces get faint reflections, metal and glass strong ones.
  - **Rough surfaces fade back to the game's own reflection** between roughness `fReflectionFadeStart=0.35` and `fReflectionFadeEnd=0.6`. A single mirror ray can't do blurry reflections yet (that's R5).
  - **Lighting in the game's units:** the plugin copies the game's reflection value out first and divides out the weight, which leaves the brightness of the game's probe in that direction. Rays that hit nothing use exactly that, so the sky and distant views match the game. Reflected objects are lit by that probe light (`fReflectionProbeAmbient=1.0`) plus the sun with a shadow ray, scaled to the game's exposure (`[Debug] fReflectionSunIntensity=1.0`).
  - Skin and hair-like materials keep the game's reflection.
- **F2 now cycles:** off (game reflections) â†’ **RT reflections on** â†’ the four debug views â†’ off.
- **PERF:** a new log line, `PERF reflections (â€¦): x ms GPU`, about every 600 frames.
- **Test:** compare off and on with F2 at the Trident pillar, a shiny floor and somewhere outdoors. Check that reflections look plausible: not too bright or dark against the game's, nothing wrong on matte walls or characters, no flicker. Send screenshots of each pair and the log (the PERF reflections line gives the cost). If reflected objects look too dark, raise `fReflectionProbeAmbient`; if sunlit ones are too bright or dull, change `fReflectionSunIntensity`.

## 0.2.26 - 2026-11-19

- **0.2.25 result (R2 passed):** world normals decode smoothly, and the mirror view on a curved pillar in New Atlantis showed the room behind the camera upright and correctly curved, characters included. It ran every frame with no crash. The debug colours also showed at full strength on matte walls, so the final pass must weight reflections itself (split-sum, as the game does).
- **New, RT reflections step R3: a 4th F2 view, "RT reflection colour".** Each mirror ray's hit now gets real shading:
  - **albedo:** the hit object's own base-colour texture, at the hit's UV and times its per-instance tint, read the way the game's G-buffer shader reads it (StandardMaterial PSO 12209: texture set at material +116, entry +0; UV set from +112, UV transform +168/+172; tint from instance +12).
  - **lighting:** the sun on the hit surface (face normal from the hit triangle), with a shadow ray, plus a flat ambient. Rays that hit nothing show a flat sky colour.
  - Objects that aren't standard materials (vegetation and others) show a flat colour for now.
- The lighting is a **placeholder** (`[Debug] fReflectionSunIntensity=1.0`, `fReflectionAmbient=0.15`). It isn't yet matched to the game's brightness, local lights or probe lighting; that's R4. This view checks that the right textures, in the right place, show up in reflections.
- **Test:** press F2 four times to get the new view, at the shiny pillar and somewhere outdoors in sunlight. Look for recognisable textures (posters, signs, floor patterns) mirrored correctly, sunlit vs shadowed areas, and any garbage (noise, stripes, wrong textures). Send screenshots and the log. If the view is too dark or bright, change `fReflectionDebugScale`.

## 0.2.25 - 2026-11-19

- **0.2.24 result:** no crash, and the survey now lists the G-buffer textures. `GatherIndirect`'s table 4 is the shader's `space4` table with offset = register: t1 normal (R10G10B10A2), t2 AO/roughness (RGBA8), t4 material type (R16_UINT), **t6 depth** (an earlier note said t11, which was wrong), t7/t8 reflection-probe cube arrays, t13 the BRDF lookup. Full table in RT_FINDINGS "R1 survey". R1 is done.
- **New, RT reflections step R2: debug views on F2** (`[Debug] iReflectionDebugKey=113`). Right after the game draws its reflections, a new compute pass writes over the reflection (probe specular) target. Each F2 press cycles:
  1. **world normal**: the G-buffer normal decoded the game's way, as colour. Smooth and consistent across the screen means the decode is right.
  2. **mirror hit**: one perfect-mirror ray per pixel against the ray-traced scene. Each object it hits gets a flat colour, darker further away; blue means the ray hit nothing. This is not a real reflection yet. It checks that rays start in the right place and point the right way.
  3. **roughness** (white = rough, black = mirror-smooth).
  4. off.
- The views only show where the game adds reflections, so expect them to look faint on dull surfaces and strong on shiny ones. Sky is left black. `fReflectionDebugScale` brightens or dims them. They need RT on (F11) and pause while a scene loads.
- **Test:** in-game, press F2 once (normals), again (mirror hits), again (roughness), again (off), with a screenshot of each. On the mirror view, check a floor or wall mirrors the right objects (a floor should show the colours of things above it). Send the screenshots and the log (`reflection debug` lines).

## 0.2.24 - 2026-11-19

- **0.2.23 result:** no crash. Two loads went through, each with the stricter post-load pause, and the F8 survey ran in-game. As in 0.2.22, though, the G-buffer textures weren't listed: table 4 came out empty.
- **Fixed (survey only):** the plugin did record texture views, but the descriptor-table lookup returned only buffers and constant buffers, so the survey never saw them. The survey now has its own lookup that also returns textures. The shadow code's lookups are unchanged.
- **Test:** in-game, look at something shiny and press **F8** once, then send the log. The `[R1]` lines under `root param 4` should now list textures (size and format).

## 0.2.23 - 2026-11-19

- **0.2.22 result:** a GPU hang about 0.4 s after a load finished (the tester started running right away). The R1 survey was not involved: it had already run once in the main menu and finished cleanly. The hang came right after RT shadows resumed from the load pause, with 984 BLAS builds still queued and the ray-traced scene still growing every few frames. This is the 0.2.11/0.2.12 load hang: the 1,000-build pause threshold let shadows resume too early.
- **Fixed: RT shadows wait longer after a load.** The pause still starts above 1,000 queued builds or settling records. It now ends only after 90 frames in a row (about 1.5 s) with both under 200 and the ray-traced scene no longer growing. RT shadows will appear a little later after loads.
- **R1 data so far:** the main-menu survey found `GatherIndirect` drawing one full-screen triangle into two 3440Ã—1440 R11G11B10 targets (indirect diffuse and probe specular, as expected). Table 4, where the G-buffer textures should be, showed no entries there, so an in-game F8 survey is still needed.
- **Test:** load a save a few times, including running straight after the load. Then look at something shiny and press **F8** once, and send the log.

## 0.2.22 - 2026-11-18

- **RT reflections, step R1 (survey only, no visual change).** The game's probe reflections are drawn by `Indirect#0`, which the `GatherIndirect` render pass runs. The plugin now hooks that pass. On the first in-game frame, and again on every **F8** press, it logs each draw's render targets (size and format), viewport, and every graphics root binding: tables resolved entry by entry into textures, buffers and constant buffers. This tells the reflection pass where to read the G-buffer and where to write.
- **Plumbing:** the device hooks now also track render-target views and texture views (format and size), and the command-list hooks track the graphics root signature, tables and root constant buffers.
- F3's skip test no longer cycles into the new pass.
- **Test:** load a save, look at something shiny (metal, a floor), press F8 once, then send the log (lines start with `[R1]`). Check that nothing looks different and that loading does not crash.

## 0.2.21 - 2026-11-18

- **The game's sun shadows are now off cleanly while RT is on: no flicker.** Mode 2 (`bReplaceGameSunShadows=2`, the sun cascade pass draws nothing) is the default again. It was dropped in 0.2.10 because it seemed to cause the Frame Generation crashes, but those came from the RT shadow pass itself and were fixed in 0.2.12. Tested with 0.2.20: the game's shadows were gone, no flicker, no crash.
- **Removed:** the SceneShadowData patching from 0.2.11-0.2.20 (zeroing the cascade count in the lighting constants). It never worked every frame, which caused the flicker. Its queue and root-constant-buffer hooks are gone too. An old `bReplaceGameSunShadows=1` in an INI now acts as 2.
- **F3** is back to its 0.2.5 job: cycling which shadow-map pass has its draws dropped (debugging only).
- **Test:** RT on â†’ only RT shadows, no flicker; F11 off â†’ the game's shadows return. Check loads for crashes.

## 0.2.20 - 2026-11-18

- **0.2.19 result:** still flickering. The strict pattern switched off the game's shadows on fewer frames than 0.2.18's looser one (22-40% against 55-70%), so the real data is not always `4 4 15 0`. The 12 logged misses all came from the main menu, which used up the budget, so the in-game misses were never logged.
- **Change:** buffer views go back to the looser check (root bindings keep the strict one). Missed buffers are now logged by sampling (every 20th, up to 40 lines), with rows 63 and 64 both at view creation and 8 frames later, to show what the frames the plugin misses contain.
- **Test:** this build is diagnostic. Load in with RT on, play for about a minute, then send the log.

## 0.2.19 - 2026-11-18

- **0.2.18 result:** the game's shadows still flickered; the RT shadows looked fine. The new stats show the game makes exactly one SceneShadowData buffer per frame. The plugin switched it off when the view was created on only 55-70% of frames, and the submit-time pass caught none of the rest. The likeliest reason is that the data for those frames is written later than the plugin waited: it dropped each buffer after 2 frames. Both submit hooks (Streamline and native D3D12) are confirmed running.
- **Change:** buffers are now kept and re-checked for 8 frames. The match check is now strict (`4 4 15 0` / `1 1 0 0`, or the same with the first value already 0), so a recycled ring slot holding other data is never touched.
- **Diagnostics:** the first 12 buffers that are never switched off get logged with what they contain 8 frames later, and the stats line counts them (`never matched`).
- **Test:** same as before: check for flicker with RT on, play for at least 30 seconds, then send the log.

## 0.2.18 - 2026-11-18

- **0.2.17 result:** the game's shadows still flickered with RT on. The log shows SceneShadowData being found and zeroed (`4 4 15 0 -> 0 4 15 0`), so on some frames the lighting must read its copy through a path the plugin does not cover. Two suspects, both handled now:
  - **The submit hook may never run.** It was installed on Streamline's queue wrapper (`sl.interposer.dll`). If the game submits on the native D3D12 queue, the 0.2.16/0.2.17 fix did nothing, which matches the identical results. The plugin now also hooks `ExecuteCommandLists` on the native D3D12 queue.
  - **Root constant buffers.** A buffer bound straight to a shader slot (`Set*RootConstantBufferView`) never creates a view, so the plugin never saw it. Those bindings are now checked too. Only data with SceneShadowData's exact pattern is changed (`4 4 15 0`, or one already set to 0).
- **Diagnostics:** every 600 frames the log prints a `SceneShadowData last 600 frames` line: views seen, patches at view creation, at submit and on root bindings, and how many times the submit hook ran.
- **Test:** look at the game's sun shadows with RT on (flicker or gone?). Play for at least 30 seconds after loading so a few stats lines are written, then send the log.

## 0.2.17 - 2026-11-18

- **0.2.16 result:** the game's shadows still flickered with RT on, and F3 did nothing. The log showed why: the engine puts SceneShadowData at a **new address every frame** (a ring buffer), and on some frames it creates the view before writing the data. The plugin checked the contents at view creation, saw old data, and skipped that frame's buffer, so those frames kept the game's shadows.
- **Fixed:** every view of SceneShadowData's exact size (1,280 bytes) is now recorded, whatever it holds yet, and the ones from the last two frames are patched right before each GPU submission, after the data has been written. The contents check still decides what gets patched, so other buffers of the same size are left alone. Old entries are dropped after two frames because the ring reuses that memory.
- **Test:** with RT on, the game's sun shadows should be gone with no flicker; F11 off brings them back. Send the log either way.

## 0.2.16 - 2026-11-18

- **0.2.15 result:** by default the game's shadows flickered; F3 setting 2 (focus mask only) stopped the flicker; settings 1 and 3 flickered. So zeroing the cascade count does turn off the game's sun cascades, but only on frames where the engine creates a new constant-buffer view. On other frames it rewrites the same buffer through an existing view, and the shadows came back.
- **Fixed: re-patch right before every GPU submission.** Each SceneShadowData buffer the plugin finds is now remembered (up to 16, holding a reference so a buffer the game frees can never be touched), and a new hook on the command queue's `ExecuteCommandLists` zeroes the cascade count in all of them just before the game submits its frame. The game's sun cascades should now be off every frame, with no flicker.
- **Test:** with RT on, the game's tree and ground shadows should be gone without flicker (only RT shadows remain). F11 off should bring the game's shadows back. Check for crashes on load, and send the log (look for `ExecuteCommandLists hook installed` and `SceneShadowData buffer ... tracked`).

## 0.2.15 - 2026-11-18

- **0.2.14 result:** no F3 setting removed the game's shadows. The log confirmed the SceneShadowData layout (row 64 = `4 4 15 0`: four cascades, focus mask 15) but showed only one or two patches per setting, at a different address each time, instead of one every frame. When the lighting binds its descriptor table, slot 1 still points at the **previous frame's** copy (the engine writes the descriptor afterwards, which D3D12 allows). So the plugin was patching data the GPU had already used.
- **Fixed: patch when the engine creates the constant-buffer view.** A callback from the device's `CreateConstantBufferView` hook sees every CBV right after the engine has written that frame's data. Views of SceneShadowData size (1,072â€“1,536 bytes) whose row 64 looks right (cascade count 1â€“4, second field 1â€“8) get the selected fields zeroed (F3: cascades, focus mask, both, none). The table-bind check from 0.2.13 is removed.
- **Test:** at the tree wall with RT on, press F4 (lit test), then F3 slowly. Note which setting removes the game's tree and ground shadows, and whether character shadows go with it. Send the log.

## 0.2.14 - 2026-11-18

- **0.2.13 result:** the game's shadows stayed with the lit test (F4). The log shows one lookalike buffer correctly left alone (row 64 all zeros) and patches being applied, but the patched values weren't logged. Either the patched buffer isn't the one the lighting reads, or zeroing the cascade count isn't what removes these shadows. Nearby sun shadows may be drawn as **focus shadows** (row 64 `.z`), not cascades.
- **F3 now cycles which SceneShadowData fields are zeroed:** cascade count (`.x`, the default), focus-shadow mask (`.z`), both, nothing. The F3 pass-skip test from 0.2.5 is retired. The next six patches after each change are logged with their before and after values.
- **Test:** at the tree wall with RT on, press F4 (lit test), then press F3 slowly and watch which step removes the game's tree and ground shadows, and whether character shadows also go. Send the log.

## 0.2.13 - 2026-11-18

- **0.2.12 result:** no crashes over several loads with Frame Generation on. The load pause works: RT shadows paused at about 2,000 settling records and resumed about a second later. RT costs about 2.7 ms of GPU time at the tree wall (TLAS 1.27, rays 1.3, filter 0.14). The BlurY table-3 dump explained the 0.2.11 miss: slot 0 is the PerSceneConstants CBV (CBVs have no resource in the descriptor mirror, so they print as untracked), and slot 1 is a 16-byte SRV. BlurY's table 3 is laid out differently from the lighting's.
- **New: the game's sun cascades are switched off where the lighting binds them.** On every `SetComputeRootDescriptorTable(3, â€¦)`, the plugin checks slot 1 for a CBV the size of SceneShadowData (1,072â€“1,536 bytes) whose row 64 looks like { cascade count 1â€“4, ?, focus mask, ? }. CBVs carry only a GPU address, so a new device hook on `CreateCommittedResource`, `CreatePlacedResource` and `CreateCommittedResource1` keeps a map of upload-heap buffers by GPU address range. The newest resource at an address wins; that's safe because the buffer is in use when it's patched. The plugin maps that buffer and sets the cascade count to 0, once per buffer address per frame, only while RT is on and the scene isn't loading. The shadow-map passes and the character focus shadows are untouched. The log shows the first candidate's row-64 values and whether it was patched.
- **Test:** load in with Frame Generation on, go to the tree wall and press F4 (lit test). Only character shadows should remain. F11 (RT off) should bring the game's shadows back. Send the log.

## 0.2.12 - 2026-11-18

- **0.2.11 result, and the cause narrowed down:** 0.2.11 crashed on load too, but its new method never ran (`SceneShadowData not found at table 3 + 1`). The game's shadows were untouched, yet the same hang happened. So the sun-shadow handling (0.2.6â€“0.2.11) was **not** the cause. A bisect with Frame Generation on settled it: with the **plugin disabled**, loads were fine; with the **plugin on but `iSunShadows=0`** (BLAS and TLAS still built, no rays traced), loads were fine too. The hang comes from the **RT shadow pass right after a load**.
- **Likely mechanism:** the shadow rays' leaf-transparency loop runs the alpha test (a texture fetch) on every leaf or cutout triangle a ray passes. Mid-load, the scene can briefly hold huge or misplaced shapes, and a ray can then cross thousands of such candidates. Across millions of rays, one frame takes seconds and Windows resets the GPU (TDR). The Frame Generation list is just what's queued behind.
- **Fixed, three guards:**
  - **Bounded rays:** at most 64 alpha-tested candidates per shadow ray (256 for the F8 probe); past that, the ray counts as blocked.
  - **RT shadows pause while the scene loads:** while more than 1,000 BLAS builds are queued or 1,000 records are settling, the shadow pass is skipped (the game's shadows show meanwhile). The log says "scene loading â€¦ paused" and "ready â€¦ resumed". After a load this usually lasts about a second.
  - **Sanity check on instances:** non-finite or absurd positions (over 1e6 units) and scales (over 1000) are left out of the TLAS.
- **Diagnostic:** the first frame logs what's at BlurY's descriptor table 3, to locate `SceneShadowData` (the 0.2.11 method for removing the game's sun shadows found nothing at table 3 + 1).
- **Test:** with Frame Generation on and the shipped ini (`iSunShadows=2` again), load in several times. Send the log. It should show the pause/resume lines and the table 3 dump.

## 0.2.11 - 2026-11-18

- **0.2.10 result:** it crashed after load again, the same GPU hang with only the Frame Generation list unfinished. The re-clear itself worked (one DSV re-cleared to 0, none unknown). Every version that touched the game's sun-shadow pass (dropping its draws, or re-clearing its maps) has hung at some point. The session that left it alone didn't. The tree shadows also shimmer (DLSS sub-pixel jitter across the fine leaf cut-outs); temporal accumulation is next.
- **New default: the lighting ignores the game's sun cascades** (`bReplaceGameSunShadows=1`). The game's lighting shader reads its sun cascades from the `SceneShadowData` constants (cb1 space3, 1,072 bytes): 4 character focus shadows, then 4 sun cascades, then a row holding the cascade count and the focus-shadow mask. Compute passes share that descriptor table, so at BlurY the plugin finds this frame's copy (table 3 + 1), which lives in an upload heap, and sets the **cascade count to 0** before the GPU runs the lighting. Lighting then skips the sun cascades, while the shadow-map passes run untouched and character focus shadows stay. The game writes a fresh copy every frame, so turning RT off (F11) brings its shadows straight back. The first time, the log prints the row's original values to confirm the layout. Modes 2 (drop draws) and 3 (re-clear maps) remain available for comparison.
- Side effect to watch: anything else that reads the sun cascades this frame (for example light shafts and fog) also loses them.
- **Test:** with Frame Generation on, load in a few times. Then press F4 (lit test) at the tree wall: only character shadows should remain. Send the log. It should contain `SceneShadowData row 64 = â€¦`.

## 0.2.10 - 2026-11-18

- **0.2.9 results:**
  - **It crashed right after loading in:** a GPU hang where only the DLSS Frame Generation list was unfinished, the same as the 0.2.6 crash. Frame Generation is always on. With `bReplaceGameSunShadows=0` (the game's sun-shadow pass left alone), several loads didn't crash. Dropping that pass's draw calls coincides with the hangs.
  - **A whole wall turned shadowed.** The F8 probe found the camera ray hitting an invisible object 18.7 m away in front of a building 280 m away. It hit triangle 10,850 of mesh 21476, whose current record has only 136 triangles. The game reuses mesh slots for different meshes, and 0.2.9 kept the old BLAS until a replacement was built, leaving a wrong, huge, invisible shape in the scene: 83 of 107 grid samples hit something in front of the real surface.
  - **The leaf test works:** varied texture coordinates, 26 of 74 leaf surfaces solid.
- **Fixed: stale shapes from reused mesh slots.** A BLAS is now kept until its replacement is ready only when the record's index and vertex counts are unchanged (the same mesh moved in memory, most of the LOD churn). If the counts change, the slot holds a different mesh and the BLAS is retired at once. The BLAS pool line logs in-place replacements and slot reuses.
- **New: a gentler way to switch off the game's sun shadows** (`bReplaceGameSunShadows=1`, the new default). The sun cascade pass (ShadowMapInstance) draws normally. Afterwards, the plugin clears the depth targets it bound, with the same depth value the game clears them with, learned from a new `ClearDepthStencilView` hook. The cascade maps end the pass empty, which reads as fully lit. If no clear value is known for a target, it's left alone and the log says so. `bReplaceGameSunShadows=2` is the old draw-dropping method, 0 keeps the game's shadows.
- **Test:** load in a few times with Frame Generation on. Check that RT alone is shadowing (F4 lit test: no shadows should remain except characters), and check the log for the "sun cascades: N DSV(s) re-cleared" line. Also confirm the wall isn't fully shadowed any more.

## 0.2.9 - 2026-11-18

- **0.2.8 feedback (screen recording):** the blur is fine for now. Three problems remain: shadows popping in and out, jitter, and background buildings without shadows.
  - **Popping:** in consecutive video frames, a large block-shaped shadow on the wall changes shape. The 0.2.7 log explains it: standing still, about 25,000 BLASes stayed live, but 500â€“700 were rebuilt every 10 seconds. The game keeps rewriting mesh records as it streams level-of-detail data, and since 0.1.34 a changed record retired its BLAS at once. The object then had no ray-traced shape for at least half a second (settle time plus rebuild), so its shadow popped out and back in.
  - **Jitter:** the soft-shadow sampling pattern was rotated every frame.
  - **Background buildings:** checked the game's lighting shader (TiledDeferredLight PSO 11809). It starts from the contact mask, where RT writes, and takes the `min` with its cascade lookups at every distance, with no distance fade. So the missing far shadows need a probe (below).
- **Fixed: no more shadow pop-out on LOD streaming.** A mesh whose record changes keeps its current BLAS in the TLAS until the replacement (built after the record settles) is ready. The two are swapped in the same frame. Every BLAS is built from the plugin's own sanitized copy, so the old one stays valid. A mesh whose record disappears is still removed at once. The BLAS pool line logs how many replacement builds were queued.
- **Fixed: soft-shadow shimmer.** The per-pixel sampling pattern no longer rotates every frame.
- **Test:** walk around the tree wall and watch for popping and shimmer. Then aim the screen centre at a background building that should have shadows (in RT on, compare with F11) and press F8. Send the log and a screenshot.

## 0.2.8 - 2026-11-18

- **0.2.7 results:** RT shadows now fill in about 1â€“2 seconds after a load (was 30+ s). The plugin's CPU time while streaming dropped from 6â€“7 ms to 0.3â€“0.8 ms per frame, and BLAS GPU time while streaming from about 1.4 ms to about 0.1 ms. All ~24,600 BLASes (384 MB in 7 pool pages) were built within one log window, with no failures and no crash. At the tree wall RT adds about 2.1 ms of GPU time: TLAS 0.85, shadow rays 1.1, filter 0.15.
- **Sharper soft shadows by default:** `fSunAngularRadiusDeg=0.15` (was 0.27, the real sun's size) and `iSoftShadowMaxBlur=4` (was 6). Pure-RT shadows looked too blurry. These narrow every penumbra at no performance cost, a little crisper than real life. For finer detail inside the shadows (leaf gaps), `iSoftShadowScale=1` traces every pixel, for about 3 ms more.
- **Test:** check the tree shadows on the wall again. If you want them even crisper, try `fSunAngularRadiusDeg=0.1`; for softer, 0.27.

## 0.2.7 - 2026-11-18

- **0.2.6 session:** one launch crashed inside another mod, Shield-Count-Mod.dll, while it patched game code at startup. The next session hit a GPU hang (`DEVICE_HUNG`) after about 2Â½ minutes. DRED showed **none of the plugin's command lists unfinished**: only the DLSS frame generation list (`dlss_g`, not started) and the frame-pacing and present lists behind it. RT shadows were off at that moment, and about 3,000 BLAS builds were still queued. Not conclusively the plugin; testing with frame generation off would tell.
- **Faster shadow fill-in after loads (BLAS memory pool).** Every BLAS used to get its own committed GPU buffer, a driver allocation per build. That cost 6â€“7 ms of CPU per frame while streaming and was why only 16 builds per frame were allowed, so the ~25,000 BLASes of New Atlantis took 30+ seconds to appear. BLASes now live in 64 MB pages carved by a best-fit free-list allocator (256-byte aligned, neighbouring free ranges merged). A retired BLAS's range goes back to its page only after 30 frames.
- **Builds overlap on the GPU:** each build in a batch gets its own slice of a 32 MB scratch buffer, with one barrier at the end (or when the scratch wraps) instead of one after every build.
- `[RT] iBlasBuildsPerFrame` now defaults to **64** (was 16). The log's BLAS section adds a pool line (pages, MB reserved, MB in use).
- **Test:** load a save and watch how quickly RT shadows fill in. Send the log; the PERF lines show the CPU and BLAS cost while streaming. If you can, also try a session with DLSS frame generation off to see whether the hangs stop.

## 0.2.6 - 2026-11-18

- **0.2.5 result:** the first F3 press, **ShadowMapInstance**, removed the game's sun shadows. The survey shows it drawing into a 4096Ã—4096 depth target: the sun cascades at the tester's shadow quality. GroupedDynamicShadowMap draws 2048Ã—2048 maps, likely local lights.
- **Fixed for real: RT now replaces the game's sun shadows.** While RT sun shadows are on (and `bReplaceGameSunShadows=1`), ShadowMapInstance's draw calls are dropped. The pass still runs, so its culling dispatches and the engine's state are untouched. The cascade maps stay empty (fully lit), and the RT shadow alone decides what's in sunlight. F11 still compares RT with the game's shadows. The `fMaxShadowDistance` change from 0.1.24 has been removed (it never worked) along with its `fGameSunShadowDistanceWithRT` setting.
- **Tuned for pure RT:** with the game's shadows really gone, the soft shadows turned out too blurry, with tree shadows becoming soft blobs. The defaults had been tuned while the game's crisp shadows were still underneath. The sun size is now the real sun's angular radius, `fSunAngularRadiusDeg=0.27` (was 0.4), and the filter's largest radius is `iSoftShadowMaxBlur=6` (was 12). `iSoftShadowScale=1` traces at full resolution for the most detail.
- **Test:** at the tree wall with RT on, tree shadows should have leaf detail again, softening only toward their far ends. Compare F11 (game) and F6 (hard RT). Watch for any shadow that's missing in RT but present in the game: things not in the ray-traced scene, like characters, are covered by the game's separate character shadows. Lamp and other local-light shadows should be unaffected.

## 0.2.5 - 2026-11-18

- **0.2.4 result: the game's sun shadows were never switched off.** With the F4 lit test (RT forced to fully lit), the game's tree, lamp-post and ground shadows all stayed, even though `fMaxShadowDistance:Shadows` sat at 0.1 the whole session. Toggling F11 off and on didn't help either. So changing that setting at runtime doesn't switch off the sun cascades. The 0.1.24 conclusion was wrong: the game's tree shadows had been hidden inside RT's big extra wall shadow. Until now, RT shadows have been drawn *on top of* the game's shadows.
- **New: shadow-pass survey and skip test (F3, `[Debug] iSkipPassKey=114`).** Six of the game's shadow-map render passes are now hooked: ShadowMapInstance, GroupedDynamicShadowMap, ClearShadowMaps, ClearShadowMapGroup, TerrainShadows and SetupDynamicShadow. Their vtables are found from the game's RTTI. The log records what each one draws (draw counts, viewports, render targets). While RT is on, each F3 press drops the draw calls of the next pass in that list, then none. It does this by suppressing `DrawInstanced`, `DrawIndexedInstanced` and the newly hooked `ExecuteIndirect` on the pass's command list, so the engine's state and the pass's return value stay untouched. With the lit test on, the press that makes the tree shadows disappear names the pass that draws the sun cascades.
- Fixed: the lit-test log printed the mode from before the toggle.
- **Test:** at the tree wall with RT on, press F4 (lit test). Then press F3 slowly, a few seconds apart, and note when the game's tree and ground shadows disappear and whether other shadows (lamps, characters) also go. Send the log. It names the pass after each press.

## 0.2.4 - 2026-11-18

- **0.2.3 results:** distance culling cut the TLAS's active instances only from about 233,000 to 206,000, because New Atlantis is dense within 1.5 km. At the spot tested, RT with soft shadows ran at 7.1 ms per frame (140 fps): shadow rays 1.25 ms, TLAS 0.86 ms, filter 0.17 ms. The tester believes the game's own tree shadows stay on even with RT on, although the log shows the game's sun shadow maps switching off (`fMaxShadowDistance -> 0.1`).
- **New: lit test (F4, `[Debug] iLitTestKey=115`).** It forces the RT result to "fully lit" everywhere (sun shadow mode 6) while the game's sun shadow maps stay off. Any shadow still on screen then comes from another game system, such as character shadows, terrain shadows or vegetation/static shadow maps, not from RT. Press F4 again to go back.
- **Test:** at the tree wall, press F4. If the tree shadows stay, the game draws them through another system, and I'll find and switch that off too. If they disappear, the tree shadows with RT on are the RT ones.

## 0.2.3 - 2026-11-18

- **0.2.2 results** (tree wall): soft-shadow rays dropped from 5.5 ms to 1.4 ms (filter 0.16 ms). Soft RT now costs about 2.7 ms over the game's shadows (20.1 vs 17.4 ms per frame) instead of 5.7 ms, and is slightly cheaper than hard shadows (19.3 ms). The soft shadows still look good at half resolution. TLAS compaction saved nothing there: about 233,000 instances are active in New Atlantis, close to the old 262,144-slot scan cap, so the TLAS still cost 1.06 ms. The profiler's RT-off line now correctly shows zero shadow time.
- **New: TLAS distance culling** (`[RT] iTlasCullDistance=1500`). Instances whose nearest point is more than 1,500 units (about metres) from the camera are left out of the TLAS. Shadow rays start at visible surfaces, so kilometre-distant objects rarely shade anything you can see, and they only add build time. The camera position comes from the scene constants (root CBV). Set it to 0 to keep everything.
- **Wider instance scan:** the TLAS now scans 524,288 instance slots (was 262,144), so instances past the old cap are no longer silently dropped. The TLAS buffers are sized from the compacted active count and grow when it does, so the wider scan costs no extra memory.
- **Test:** at the tree wall with RT on, check the log's TLAS line: how many active instances are left after culling. Also watch for missing shadows from tall distant buildings, especially at low sun angles. If anything goes missing, raise `iTlasCullDistance`.

## 0.2.2 - 2026-11-18

- **0.2.1 profiler results** (tree wall, 3440Ã—1440, DLSS): game shadows 17.8 ms/frame (56 fps). RT with hard shadows 19.5 ms (51 fps): shadow rays 2.1 ms, TLAS 0.9 ms. RT with soft shadows 23.5 ms (43 fps): shadow rays 5.5 ms, TLAS 0.9 ms, filter 0.2 ms. The leaf test costs about 0.25 ms. The plugin's CPU time is about 1.2 ms at rest but 6.5â€“7.8 ms per frame while geometry streams in.
- **Faster: soft shadows trace at half resolution** (`[RT] iSoftShadowScale=2`). Pass 1 traces one sample (4 rays toward the sun's disc) per 2Ã—2 pixels, which is a quarter of the rays. Pass 2 runs at full resolution: each pixel reconstructs its own depth and upsamples the nearby samples, weighting them by bilinear distance and by how close their depth is. Shadow outlines therefore follow object edges at full resolution. The penumbra filter then runs in sample space. Set it to 1 for full-resolution tracing.
- **Faster: TLAS compaction.** The instance builder used to write and build all 262,144 instance slots, including inactive ones. It now packs only active instances to the front with an atomic counter and nulls the rest of the range. The TLAS is built over only the active count, read back 3 frames later, plus 25% headroom. The log's TLAS line shows the active count and how many descriptors were built. Shaders read the game's records through `InstanceID`, which still holds the record index.
- **Fixed: profiler numbers after a toggle.** Frames recorded before an F11/F6/F7 toggle could still land in the next line's per-stage averages. That's why "RT off" showed shadow-ray time. Each frame is now tagged with its measurement window.
- **Test:** same as 0.2.1. Stand at the tree wall for about 15 seconds each with soft shadows on, F6 (hard), and F11 (game shadows), then send the log. Also check whether the soft shadows look as good as in 0.2.0, and watch for blocky or flickering penumbra edges.

## 0.2.1 - 2026-11-18

- **New: built-in profiler** (`[Debug] bProfiler=1`). The plugin writes GPU timestamps around each stage of its per-frame work: BLAS (geometry copies and builds), TLAS (instance descriptors and build), shadow rays (pass 1) and the soft filter (pass 2). It reads them back a few frames later without stalling, and also times its CPU work in the game's render thread and the whole frame. About every 10 seconds the log gets a line like `PERF RT mode 2 soft on alpha on (600 frames): frame 8.3 ms (120 fps) | plugin GPU: BLAS â€¦ + TLAS â€¦ + shadow rays â€¦ + soft filter â€¦ = â€¦ ms | plugin CPU â€¦ ms`. A new line starts whenever F11, F6 or F7 changes the state, so one toggle gives a direct before/after.
- Note: with RT off (F11), the BLAS and TLAS are still built every frame, so that state's frame time still includes them (the PERF line shows how much).
- **Test:** stand somewhere typical (the tree wall is fine) and don't move for about 15 seconds in each of these: RT on with soft on (default), F6 off (hard shadows), F6 back on then F7 off (no leaf test), F11 off (game shadows). Then send the log.

## 0.2.0 - 2026-11-18

**Milestone: Phase 1 (ray-traced sun shadows) complete.**

- **0.1.34 test result:** stable. Save loads and about 6 minutes of play built 78,664 BLASes, including 10,082 meshes from separately streamed buffers, with no failures and no GPU faults.
- **What 0.2.0 delivers:** RT sun shadows that line up with the game, replace its sun shadow maps (F11 compares the two), include trees and props from streamed buffers, alpha-test leaves, grass and fences (F7), and have soft, distance-dependent penumbras (F6). They're stable under streaming and save loads. Crash diagnostics (DRED) are built in.
- README and PLAN updated for the milestone. No code changes from 0.1.34.
- **Next:** measure and tune performance, then Phase 2 (ray-traced global illumination).

## 0.1.34 - 2026-11-18

- **0.1.33 test result:** the GPU crash watchdog and DRED worked. The game crashed on save load, and the log recorded the cause: the GPU **hung** (`DXGI_ERROR_DEVICE_HUNG`) inside one of the plugin's own BLAS builds (DRED breadcrumb op 31, `BuildRaytracingAccelerationStructure`). Every other command list was waiting behind it.
- **Cause:** BLAS builds read the game's vertex and index data directly. On a save load (and during heavy streaming) a mesh record can exist before its geometry has been uploaded. The build then reads garbage or unmapped memory, and out-of-range indices are undefined behaviour in DXR, which hung the GPU. The plugin's CPU-side checks can't see GPU data.
- **Fixed, two layers:**
  - **Settle time:** a new or changed mesh record has to stay the same for 30 frames before its BLAS is built. A mesh whose record changes loses its old BLAS right away, so it never shows the wrong shape.
  - **Sanitized copies:** a small compute pass (`shaders/CopyGeometry.hlsl`) copies each mesh's positions and indices into the plugin's own staging buffer (3 Ã— 32 MB ring), clamping every index into range. The BLAS is built from that copy, so the builder never touches the game's memory.
- After a save load, ray-traced shadows now take a second or two longer to appear while meshes settle. The log's mesh line shows how many are still "settling".
- **Test:** load a save (a few times if you can), then play normally with travel and fast movement. If it crashes, send the log. The DRED section will show where.

## 0.1.33 - 2026-11-18

- **0.1.32 test result:** loading a save works. The game then crashed with a GPU fault (nvlddmkm event 153) after about 4Â½ minutes, during heavy streaming: 7,299 meshes were queued for BLAS builds and the game's mesh table had grown from 32,768 to 41,984 slots. Holding streamed vertex buffers (0.1.31) wasn't enough. The log had no DRED report, because the once-per-frame check runs on the render thread, which stops once the GPU faults.
- **New: GPU crash watchdog.** A background thread checks the device every 200 ms and writes the DRED report (reason, faulting address, the allocations there, and where each command list stopped) as soon as the device dies, without waiting for the next frame.
- Remaining suspects for the fault, since it started with 0.1.27â€“0.1.29: BLAS builds reading streamed geometry the game is unloading, and the leaf test reading texture coordinates, vertex colours or opacity textures of trees that were just unloaded. The leaf test only reads real per-vertex data since 0.1.29.
- **Test:** play normally with RT on, including travel and fast movement. If it crashes, send the log (it should end with a `GPU DEVICE REMOVED` section).

## 0.1.32 - 2026-11-18

- **Fixed: crash on save load in 0.1.31.** The new once-per-frame GPU crash check called `GetDeviceRemovedReason` on the first D3D12 device the game creates. That one is short-lived (the game creates it and destroys it again), so the check called into a destroyed object: an access violation in StarfieldRT.dll, calling slot 37 of a null function table. It now checks the renderer's real device, the one the rest of the plugin already uses.
- **Test:** load a save. Then play normally with RT and soft shadows on, including travel and fast movement. If the GPU crashes, the log should end with a `GPU DEVICE REMOVED` section.

## 0.1.31 - 2026-11-18

- **0.1.30 test result:** soft shadows look right. Tree tops on the wall get soft edges, the leaf detail stays visible, and contact areas stay sharp. The game crashed once as F6 was pressed: Windows logged three nvlddmkm event-153 GPU errors in the same second. Repeating it didn't reproduce the crash, either standing still and pressing F6 ten times or walking around with F6 untouched. So it's intermittent, and probably a race with geometry streaming: about 11,500 meshes were queued for BLAS builds just before.
- **Likely fix: hold streamed vertex buffers during BLAS builds.** Since 0.1.27, meshes whose vertices live in separately streamed buffers are built too. The builder checks the mesh is still valid and records a build that runs on the GPU a little later. If the game releases that buffer in between, because it stopped using the mesh a few frames earlier, the build reads freed memory and the GPU faults. The game only keeps buffers alive for its own GPU work. The plugin now takes a reference on each streamed vertex buffer it builds from and releases it 30 frames later.
- **New: DRED crash diagnostics** (`[Debug] bDred=1`). The plugin turns on Direct3D's Device Removed Extended Data before the game creates its device. If the GPU device dies, the log records the reason, the faulting GPU address, the allocations at that address (including recently freed ones), and where each unfinished command list stopped. The plugin's own buffers are now named ("StarfieldRT acceleration structure" or "StarfieldRT buffer"), so they show up in that report. DRED costs a little GPU time; set `bDred=0` to turn it off.
- **Test:** play normally with RT and soft shadows on, including travel and fast movement that streams lots of geometry. If it crashes, send the log. It should now end with a `GPU DEVICE REMOVED` section.

## 0.1.30 - 2026-11-18

- **0.1.29 test result:** leaf shadows work. Once the trees stream in, their RT shadows look "real sharp, real good". The probe shows leaf hits with varying texture coordinates (28 of 118 leaf surfaces counted as solid), and there were no warnings or crashes.
- **New: soft shadows.** Each pixel now traces several rays (default 4) toward random points on the sun's disc instead of one ray at its centre. The pattern rotates per pixel and per frame. A second pass then filters the result into the game's shadow mask, PCSS-style. It estimates the penumbra width from the distance to the blockers, so shadows stay sharp where an object touches the ground and soften the farther they fall. It blurs only across similar depths, so edges between objects don't bleed.
- New settings: `[RT] bSoftShadows=1`, `iSunShadowRays=4`, `fSunAngularRadiusDeg=0.4` (sun size; bigger is softer, the real sun is about 0.27), `iSoftShadowMaxBlur=12` (px), `iSoftToggleKey=117` (**F6** toggles soft shadows live).
- **Test:** at the tree wall, toggle F6 and compare. Soft shadows should have gentle edges on long shadows (tree tops on the wall) and stay crisp near contact (the base of the lamp post). Look for speckled noise or flicker in the penumbras, and for a frame-rate drop compared with F6 off. The pass traces 5 rays per pixel instead of 2. If it's too soft or too hard, try `fSunAngularRadiusDeg` between 0.27 and 0.6.

## 0.1.29 - 2026-11-18

- **0.1.28 test result:** the UV fetch debug found the leaf bug. The triangle's indices sit 425 MB into the geometry pool. The shader read them through bindless slot 39, assuming slots 14 onward are consecutive 16 MB windows of the pool. Slot 39 returned zeros, so all three vertex indices came back 0, every hit used vertex 0's texture coordinates (0.258, 0.010), and the alpha there was 0. Only slot 14 had ever been confirmed as the pool. The BLAS builds read indices from the pool's GPU address directly, which is why the tree shapes were right.
- **Fixed:** the shadow pass and the probe now read triangle indices from the pool's GPU address too (bound as a root buffer), so leaf hits sample the right spot of the leaf texture. RT_FINDINGS.md is corrected.
- **Test:** RT on (F11), leaf test on (F7). Tree shadows should now be leafy, with light coming through the gaps, instead of disappearing (F7 on) or being solid (F7 off). Toggle F7 to compare and send a screenshot of each.

## 0.1.28 - 2026-11-18

- **0.1.27 test result:** trees now cast RT shadows on the wall. The log shows 1,711 meshes from separate buffers built, 24,600 BLASes (385 MB), no failures and no skipped meshes. Where the camera ray and the depth buffer disagree, it's down to 3 of 142 grid points (from 15â€“33). There's a new problem: with F7 on, the tree shadows vanish completely, so RT looks switched off.
- **The cause, narrowed down with the F8 leaf log:** 179 leaf surfaces met and none counted as solid. Every hit, across different trees and triangles, reads the *same* texture coordinates (0.258, 0.010) and gets alpha 0. The vegetation material's UV scale is (1, 1) and its offset is (0, 0), so this is a raw per-vertex UV, and the triangle's three vertex indices apparently come back the same every time.
- **New diagnostic:** F8 now logs the raw UV fetch for the first leaf the centre sun ray meets: instance, triangle, mesh, first-index field, the index-buffer window and dword it read, the three vertex indices, the UV stream handle, and the raw UV words.
- **Test:** RT on (F11), leaf test on (F7, tree shadows gone), screen centre where a tree shadow should be, press F8, send the log.

## 0.1.27 - 2026-11-18

- **0.1.26 test result:** no change to the tree shadows. F8 aimed at a tree shadow on the wall found the sun ray reaching the sun, and the grid met no leaf surfaces at all. So these trees aren't in the ray-traced scene at all, and the leaf test never runs on them.
- **Cause: the pool-only rule.** Since 0.1.12 the BLAS builder skipped every mesh whose vertices live in a separately streamed buffer instead of the game's big geometry pool (`bBlasPoolOnly=1`). That was a cautious guard after the 0.1.11 GPU crash, but the real cause of that crash (stale queue entries) was fixed in 0.1.12 by re-checking each mesh against the live record and the buffer bounds before building. Many trees and props live in those separate buffers, so they never made it into the scene. Skipped meshes were also re-queued and re-skipped on every scan, which is why the "skipped" counter kept climbing.
- **Fixed:** `bBlasPoolOnly` now defaults to 0, so those meshes are built too, with the same live-record and bounds checks. A mesh skipped by the pool rule (when it's set to 1) is remembered instead of re-queued every scan.
- The BLAS summary now logs mesh statistics: meshes in use, in the pool, in separate buffers, skinned, and over 65,536 vertices.
- **Test:** at the tree wall with RT on, tree shadows should appear, with leaf gaps when F7 is on. The ray-traced scene now holds more meshes, so watch for a crash, a black screen or a big frame-time hit, and send the log either way. If it crashes, set `bBlasPoolOnly=1` to go back.

## 0.1.26 - 2026-11-18

- **0.1.25 test result:** the leaf test is far too lenient. Across four F8 probes, sun rays met 24, 25, 0 and 5 leaf or cutout surfaces, and the test called only 2, 2, 0 and 0 of them solid. The probe aimed at a tree shadow on the wall found the sun ray reaching the sun. The big extra wall shadow showed up for a second and then disappeared. It comes from the same distant objects as before (about 55 m toward the sun, 18 m up), and it looks like they only pop in briefly, probably during a level-of-detail switch.
- **Fixed (probably): the alpha "mode 3" remap.** The game's cutout and vegetation shadow shaders apply a remap to the texture alpha when the material's alpha-params entry has mode 3 (table +28, 32-byte entries, indexed by standard material +280 or vegetation material +276). It uses the vertex alpha as a fade control and two parameters at +24/+28. The plugin skipped it and used plain `texture Ã— vertex alpha`, which can come out far too low. Both the shadow pass and the probe now apply it exactly as the game's shader does.
- F8 now logs leaf-test details for the first 8 grid samples that met a leaf or cutout surface (not just the screen centre and middle), including each surface's alpha mode.
- **Test:** at the tree wall with RT on, do tree shadows show up now? Toggle F7 to compare. Press F8 on a spot where a tree shadow should be and send the log.

## 0.1.25 - 2026-11-18

- **0.1.24 test result:** replacing the game's sun shadow maps works live. The log shows `fMaxShadowDistance:Shadows` going from 120000 to 0.1 and back on every F11 press. With the game's shadows gone, two RT problems show that they had been hiding:
  1. A large extra RT shadow covers most of the New Atlantis wall. The F8 grid says it comes from a type-0 mesh (21348, material 1406) about 52â€“57 m toward the sun and 12â€“18 m up, which is never visible to the camera. It could be glass, or something the game excludes from shadows.
  2. Tree shadows are mostly missing, on the wall and on the ground. The trees *are* in the ray-traced scene (vegetation pieces show up as blockers in the probe), and F7 changes nothing, so the leaf test is letting almost all light through them.
- **F8 is now a leaf-test debugger.** The probe's sun ray runs the exact same alpha test as the shadow pass, shared through the new `shaders/AlphaTest.hlsli`. For the screen centre and the grid middle it logs up to four leaf or cutout surfaces the ray met: material, opacity texture index, threshold, texture coordinates, the alpha it read, and whether it counted as solid. The grid summary counts how many such surfaces were met and how many were solid. It also dumps each hit object's raw material entry (288 or 368 bytes), so the extra-shadow material can be compared with ordinary walls.
- **Test:** at the wall with RT on, put the screen centre on (a) the big extra shadow and press F8, then (b) where a tree shadow should be on the wall and press F8. Send the log.

## 0.1.24 - 2026-11-18

- **0.1.23 test result:** alpha-tested shadows work. Leaf shadows on the ground break up into leaf shapes, F7 switches them live, and there were no crashes over a long session. The material tables were found where predicted: 8,192 standard entries at +11 and 1,024 vegetation entries at +13, both with the expected sizes. The probe shows plants as type 3 and buildings as type 0. Tree shadows on the wall looked the same with F7 and F11, because the game's own sun shadow maps still drew underneath and RT can only add shadow.
- **New: RT replaces the game's sun shadow maps.** While RT sun shadows are on, the plugin shrinks the game's directional shadow distance (`fMaxShadowDistance:Shadows`) to 0.1 through the game's own setting collection. The sun cascades then cover nothing, and the RT shadow alone decides what's in sunlight. Toggling RT off with **F11** restores the original value, so F11 is now a true RT-versus-game comparison. The log records the original value and every change.
- New settings: `[RT] bReplaceGameSunShadows=1`, `fGameSunShadowDistanceWithRT=0.1`.
- **Test:** check the log for `fMaxShadowDistance:Shadows = ... (original)` and `-> 0.1`. Then toggle F11 at the tree wall. RT on should show only the RT shadows; RT off should show the game's. If nothing changes, the game only reads the setting at startup, and I'll need another way in. Things that may also change: light shafts and fog may lose their shadowing, and terrain far away keeps its own terrain shadows.

## 0.1.23 - 2026-11-18

- **0.1.22 test result:** RT sun shadows now match the game's on the New Atlantis wall (same shapes, directions and positions), so the self-shadowing fix worked. Trees do cast RT shadows, but as solid blobs: every leaf card blocked sunlight as a full rectangle.
- **New: alpha-tested shadows.** Materials with an opacity texture (leaves, grass, fences, railings) now only block sunlight where the texture is solid, using the same test as the game's own shadow shaders. That's the opacity texture's red channel, times the vertex colour when the material uses it, against the material's threshold. Two material families are handled: standard materials (type 0) and vegetation (type 3, trees and plants). Other families still cast solid shadows. These instances are marked non-opaque in the TLAS, so opaque geometry keeps the fast path. Found by reverse-engineering the game's cutout shadow shaders (PSO 971) and culling shaders. Details are in RT_FINDINGS.md under "Alpha-tested materials".
- The shadow pass now uses Shader Model 6.6 bindless access (the same descriptor heap the game's shaders read), with root signature 1.1.
- New settings: `[RT] bAlphaTest=1` and `iAlphaToggleKey=118` (**F7** toggles the alpha test live). The F8 probe still treats everything as solid, but now logs each instance's material type and index.
- **Test:** stand where trees cast shadows on a wall or the ground. Toggle F7 and compare: with it on, leaf shadows should break up into dappled shapes like the game's. Also watch for a crash or a black screen. This is the first version reading engine textures directly. If the game dies, send the log and check Windows' System event log for nvlddmkm errors.

## 0.1.22 - 2026-11-18

- **0.1.21 test result:** RT shadows no longer move with the camera, so the sun direction fix works. The wall still had a large extra RT shadow. The F8 probe showed why: the sun ray from the wall was blocked by that same wall 1 cm away. The game's depth buffer puts surfaces about 1% of the view distance behind our ray-traced copy (13 cm at 13 m), more than the 10 cm + 0.2%-per-metre bias. So shadow rays started just inside the wall and hit its back face (67 of 96 grid samples were shadowed).
- **Fixed: self-shadowing.** Each pixel now first traces a ray from the camera to find the exact ray-traced surface. The shadow ray starts just in front of that hit (1 cm plus 0.05% of the distance back toward the camera, then 2 cm toward the sun). If the camera ray finds no matching surface (geometry not in the TLAS yet), it falls back to the old depth-buffer start. The F8 probe uses the same rule. This costs one extra ray per pixel.
- **Test:** at the wall, toggle F11 and compare with the game. The big shadow on the sunlit wall should be gone, leaving shadows from real objects. Trees still cast no RT shadow.

## 0.1.21 - 2026-11-18

- **Fixed: the RT sun direction was in camera space, so RT shadows turned with the camera.** The 0.1.20 test confirmed it. At one spot, two F8 presses with the camera turned 90Â° gave "to sun" vectors 45Â° up and 27Â° below the horizon. The game's light vector (contact-shadow constants, row 5) is camera-relative (x right, y up, z forward) and points toward the sun. Rotated into world space with the camera's own axes, both readings give the same sun: east-northeast, about 12Â° up (an evening sun). The RT shadow pass and the F8 probe now do this rotation every frame.
- This was the cause of the large extra shadows on the New Atlantis wall: rays were sent toward a "sun" in the wrong place, so they hit the building itself and its neighbours.
- **Test:** at the wall, toggle F11 and compare with the game's shadows. They should point the same way now and stay put when you turn the camera. Expected gaps: trees cast no RT shadow yet, and right after loading some objects are missing until their BLASes finish building (a few seconds).

## 0.1.20 - 2026-11-18

- **0.1.19 grid probe result:** no single class of hidden object stands out. 29 "never visible" blockers are ordinary meshes of all sizes, and the visible wall itself blocks 27 of the 76 shadowed samples, mostly through back faces. A third of the samples (42 of 128) also have the camera ray disagree with the depth buffer. In the RT screenshot, the slanted beam seems to cast its shadow sideways across the wall, while the game's tree shadows show a low sun from behind the camera. So the **sun direction** is the new prime suspect. It comes from the contact-shadow constants (row 5), was only checked once at a high sun, and may be in camera space rather than world space.
- **F8 now also logs the sun direction** the RT pass uses (with elevation and azimuth) and the camera's forward vector, plus whether each depth mismatch has the TLAS surface in front of or behind the real one.
- **Test:** at the same spot, press F8. Then turn the camera about 90 degrees without moving, and press F8 again. Send the log. If the sun direction changes when only the camera turns, it's in camera space and that's the bug.

## 0.1.19 - 2026-11-18

- **0.1.18 test result:** RT shadows now cover the whole screen. They still add a large extra shadow on the New Atlantis wall (horizontal bars and slats), and the tree shadows the game shows on that wall are missing in RT.
  - The F8 probe on the extra shadow: the camera ray matches the depth buffer (scene alignment is right), and the sun ray is blocked 15 m away by instance 40967. That's a very simple mesh (about 50 triangles over a ~64 m extent), typical of an invisible proxy (occluder, LOD shell or collision-style mesh) rather than real visible geometry.
  - Trees: wind-animated (skinned) meshes are still skipped by the BLAS builder, so they cast nothing in RT yet.
- **F8 now probes a 16Ã—9 grid of the screen as well as the centre.** For every sample it traces a camera ray and a sun ray and records the instances hit. The log groups them by instance: **visible** (a camera ray hit it where the depth buffer says), and **SUSPECT** (blocks sun rays but is never visible). Each comes with its triangle count, size, position, front/back face hits, and its raw instance, LOD and mesh records. Comparing the two groups should reveal the flag that marks the proxies, so they can be left out of the TLAS.
- **Test:** stand where the extra shadow is visible with RT on (F11), press F8 once, and send the log.

## 0.1.18 - 2026-11-18

- **Fixed: RT sun shadows only covered the top-left quarter of the screen.** The contact-shadow blur runs 8Ã—8 threads per group (confirmed from its shader, PSO 11800), so its mask is full output resolution (3440Ã—1440), not the 1720Ã—720 DLSS size the plugin assumed. The RT pass now covers the whole mask and reads the real texture sizes on the GPU, so a wrong guess can't shrink it again.
- This most likely explains the "extra shadows" from 0.1.16/0.1.17: the boxy shape's straight edges were the quarter boundary, and inside it the shadows were drawn from the wrong screen positions. The 0.1.17 probe agreed: the screen-centre probe read the depth at the centre of the top-left quarter (sky), not the real centre.
- The F8 probe and the mask test now use the corrected size too.
- **Test:** with `iSunShadows=2`, RT shadows should now cover the whole screen with no hard rectangular edge. Toggle F11 and compare against the game's shadows again. If extra shadows remain, aim the screen centre at one and press F8.

## 0.1.17 - 2026-11-18

- **Debug probe (F8).** Traces a ray from the camera through the screen centre and a sun ray from the surface there, reads the result back, and logs what each one hit: instance index, mesh index, triangle, distance (compared with the depth buffer: matches / in front of the visible surface / behind it), plus the raw instance record, LOD entries and mesh record. Works with RT shadows on or off. `[Debug] iProbeKey` (119 = F8).
- Purpose: the 0.1.16 on/off comparison shows RT adding large hard-edged shadows that the game doesn't have (a blocky shape with a window frame and diagonal slats, and a diagonal band on a wall). Likely casters are transparent surfaces (glass) or hidden instances. The 0.1.14 alignment test also flagged something invisible in front of that wall. The probe identifies the instance so the right filter can be found.
- **Test:** with RT shadows on (`iSunShadows=2`), put the screen centre on one of the extra RT shadows and press F8. Then put it on a spot RT shadows correctly and press F8 again. Send the log. A screenshot of what the centre was aimed at helps too.

## 0.1.16 - 2026-11-18

- The RT sun shadow toggle key is now **F11** by default (F9 is quick load in Starfield). `[RT] iSunToggleKey=122`.
- **Test result:** the on/off comparison works. RT matches the game's ground, pillar and arch shadows, but adds large hard-edged shadows the game doesn't have (a blocky shape with a window frame and diagonal slats on the wall, swallowing a tree shadow, and a diagonal band on the lower wall). Investigated in 0.1.17.

## 0.1.15 - 2026-11-18

- **F9 toggles the ray-traced sun shadows live** (between off and the ini mode), so the same view can be compared with and without RT. `[RT] iSunToggleKey` (120 = F9, 0 = no key). Each toggle is logged.
- The shipped ini now uses `iSunShadows=2` (RT replaces the contact shadows on the whole screen) for the comparison.
- **Test:** stand somewhere with clear shadows and press F9 a few times. Take one screenshot with RT on and one with it off from the same spot. Expect solid shadows from foliage for now (no alpha testing yet).

## 0.1.14 - 2026-11-18

- **Debugging the 0.1.13 shadows.** In the 0.1.13 test every shadow ray reported a hit: in the A/B view the whole ray-traced half of every sunlit surface came out shadowed (the game's shadow-map shadows still showed through). Candidates: a huge enclosing object in the TLAS (sky/planet/terrain shell), rays hitting their own surface, or the scene not lining up with the reconstructed positions.
- New debug views in `[RT] iSunShadows`:
  - **5 = alignment test (shipped default for this test):** a ray from the camera through each pixel must hit our TLAS at the distance the game's depth buffer reports. Sunlit surfaces stay normal where they match and go dark where they don't. This checks geometry, transforms and camera reconstruction, independent of the sun.
  - **4 = hit distance:** shadow rays toward the sun, black = hit right at the start (self-intersection), grey = hit far away, normal = no hit.
- New `[RT] iTlasMaxInstanceSize` (1000): instances larger than this are left out of the TLAS (filters giant sky/planet/terrain shells).
- Shadow ray bias is now configurable: `iSunBiasCm` (10, was 5) and `iSunBiasPerKm` (2).
- **Test:** load the same sunny spot with `iSunShadows=5` and send a screenshot. Then, if you can, quit, set `iSunShadows=4`, and send a second screenshot from the same spot.

## 0.1.13 - 2026-11-18

- **First ray-traced sun shadows.** A new inline-ray-tracing compute shader (`shaders/RtSunShadow.hlsl`, SM 6.5 `RayQuery`) runs right after the TLAS build on the contact-shadow blur's command list. For every pixel it rebuilds the world position from depth with Starfield's own inverse view-projection (PerSceneConstants rows 13â€“16 plus the camera position in row 0), casts one ray toward the sun (direction from the contact-shadow constants, row 5) through our TLAS, and writes the visibility into the shadow mask that `TiledDeferredLight` reads.
- The descriptor tracker now records constant buffer views too, so the plugin finds PerSceneConstants (blur root table 3 + 0) and the contact-shadow constants (ContactShadows root table 4 + 0) at runtime.
- New `[RT] iSunShadows`: 0 off, 1 combined with the contact shadows, 2 debug replace, **3 debug A/B (left half ray-traced, right half vanilla; the shipped default for this test)**. Also `iSunRayLength` (2000).
- Known limits for now: the game's shadow maps still apply on top, every surface is treated as opaque (foliage casts solid shadows), characters (skinned) don't cast RT shadows, the shadows are hard (one ray, no softening/denoise), and meshes outside the geometry pool are missing.
- **Test:** load a sunny outdoor save. Compare the left and right halves of the screen: RT shadows on the left should line up with the objects casting them. Send a screenshot plus the log ("sun shadows (frame â€¦): applied").
- **Test result:** the pass runs every frame, but every ray reported a hit, so the ray-traced half of every sunlit surface was fully shadowed. Debugged in 0.1.14.

## 0.1.12 - 2026-11-18

- **Fixed a GPU fault a few minutes into play** (0.1.11 test: the game closed with no crash log; Windows logged NVIDIA `nvlddmkm` event 153 twelve seconds after the plugin's last report, during the initial BLAS fill). Likely cause: building BLASes from queue entries up to ~30 s old, whose geometry had since streamed out or been freed.
  - Each queued mesh is now re-checked against the **live** mesh record right before its build. Changed or removed meshes are skipped (logged as "stale").
  - Vertex and index ranges are bounds-checked against the tracked buffer sizes, including the 2-byte over-read of the last vertex that `R16G16B16A16` at a 6-byte stride does (logged as "out of bounds").
  - New `[RT] bBlasPoolOnly` (default 1): only builds meshes whose positions live in the persistent 4 GB geometry pool, not in separately streamed buffers that can be freed.
  - Retired BLAS/TLAS buffers are now kept 30 frames (was 8) before release.
- **Test:** play for **5+ minutes**, moving through several areas (fast travel or a loading door helps), then send the log. Check whether the "stale" / "out of bounds" counters are non-zero. If it still faults, set `bEnableTLAS=0` next to narrow it down.
- **Test result: stable.** No new GPU errors in the Windows event log. **383 stale meshes** (changed or removed between queueing and building) were caught and skipped, so that was the likely fault. 0 out of bounds. 23,615 BLASes live (318 MB), and the TLAS was rebuilt every frame (4,600+ builds). Known inefficiency: meshes outside the pool are re-queued and re-skipped every scan ("skipped" climbs to 116k). Harmless; to be cleaned up.

## 0.1.11 - 2026-11-18

- **Test result (later):** after a few minutes the game closed without a crash log. Windows logged an NVIDIA driver GPU error (nvlddmkm 153) at 10:38:07, during the BLAS fill. Fixed in 0.1.12.

- **TLAS every frame.** A new compute shader (`shaders/BuildInstances.hlsl`) turns Starfield's instance records into DXR instance descriptors on the GPU. It reads the rotation quaternion, translation and uniform scale, takes the LOD 0 mesh, and builds the transform `R(q) Ã— scale Ã— meshScale Ã— 32767` (undoing the SNORM normalization) plus translation. Instances without a BLAS, with an invalid quaternion, or skinned are written as inactive. The TLAS is then built over those descriptors on the same command list, right after the BLAS builds.
- The CPU only uploads a small table of BLAS addresses per mesh each frame (3-buffer ring). All inputs are bound as root SRVs/UAVs by GPU address, so the engine's descriptor heap is never touched.
- New `[RT]` settings: `bEnableTLAS` (default 1), `iTlasMaxInstances` (262144).
- Still no visual change. The ray-traced debug view / first RT sun shadow comes next.
- **Test:** same as 0.1.10. Play for a minute and send the log. Look for "TLAS buffers for â€¦ instances" once, then "TLAS: N builds â€¦" every ~10 s. A crash or device removal is possible. `bEnableTLAS=0` isolates it.
- **Test result: works.** TLAS buffers for 262,144 instances (74 MB result, 27 MB scratch, 16 MB descriptors). The TLAS was rebuilt every frame (1,358 builds in the test) alongside the BLAS builds, with no crash or device removal. Side note: one of the several contact-shadow blurs per frame binds a different global table (probably a secondary view), so the per-frame update skips a frame when it lands on that one.

## 0.1.10 - 2026-11-18

- **First ray tracing work on the GPU: BLAS builds.** Once per frame, on the contact-shadow blur's command list, the plugin re-resolves the scene buffers (they're replaced as the world streams in), rescans the mesh records every 60 frames or whenever their buffer changes, and builds bottom-level acceleration structures for new or changed rigid meshes straight from Starfield's own GPU buffers: `R16G16B16A16_SNORM` vertices at a 6-byte stride from the bindless slot, `R16_UINT` indices from the 4 GB pool. No copies or conversion. New file `src/Blas.cpp`.
- Each BLAS gets its own buffer, and builds share a scratch buffer with a UAV barrier between them. Total memory is capped. Replaced buffers are released 8 frames later. The engine's compute bindings are restored afterward.
- Skinned meshes (characters) are skipped for now. Every geometry is treated as opaque for now (foliage alpha comes later).
- New `[RT]` settings: `bEnableBLAS` (default 1), `iBlasBuildsPerFrame` (16), `iBlasBudgetMB` (2048).
- No visual change yet: nothing traces rays until the TLAS and debug view land.
- **Test:** load a save, walk around for a minute, then send the log. Look for the "BLAS: N live (X MB), â€¦ built total, â€¦ failed" lines every ~10 s. **This is the first build that runs real DXR work, so a crash or "device removed" is possible.** Send the crash log if that happens. Set `bEnableBLAS=0` to rule it out.
- **Test result: works.** No crash, no device removal. 26,616 BLASes live (423 MB, about 16 KB each before compaction), 0 failed, 0 skipped. The mesh table grew from 1,024 to 29,696 slots while walking around, and the builder followed it (the queue peaked around 14,700 and drained in roughly 20â€“30 s at 16 builds per frame). Replaced and unloaded meshes were retired.

## 0.1.9 - 2026-11-18

- **Runtime groundwork for BLAS/TLAS (read-only, no visual change).**
- Hooks Starfield's import of `sl.interposer!D3D12CreateDevice` at plugin load, so the plugin sees the device from the moment it's created. On the device it hooks `CreateDescriptorHeap`, `CreateShaderResourceView`, `CreateUnorderedAccessView`, `CreateConstantBufferView`, `CopyDescriptors` and `CopyDescriptorsSimple`, and mirrors every buffer view written into a CBV/SRV/UAV heap (resource, GPU address, size, stride). New file `src/DeviceHooks.cpp`.
- A few seconds into gameplay it resolves the scene buffers through the global root table the lighting binds (instances, LOD entries, mesh records, the 4 GB geometry pool, transforms, meshlets), counts the bindless geometry slots, checks that slot 14 is the pool, and reads the mesh records on the CPU when their buffer is mappable. Everything is logged.
- The checkerboard test is now off by default (`iMaskTest=0`). The setting is still available.
- **Test:** load a save and play for about 10 seconds, then send the log. Look for "descriptor hooks installed", the scene buffer lines (strides 96/20/52/48/32), "slot 14 â€¦ matches the pool" and the mesh record totals. Watch for any new stutter.
- **Test result: works.** The device hooks installed at creation (main shader-visible heap: 98,304 descriptors). All six scene buffers were found with the strides seen in the PIX capture. Slot 14 is exactly the pool base. Mesh records are in an UPLOAD heap, so the CPU can read them (79 meshes, 96k vertices, 158k triangles at that moment). The mesh/transform buffers were smaller than in the capture, so they grow as the world streams in and must be re-resolved continuously.

## 0.1.8 - 2026-11-18

- **First write into Starfield's lighting.** Right after `TiledDeferredContactShadowsBlurY` finishes the contact shadow mask, the plugin dispatches its own compute shader into that same mask, before `TiledDeferredLight` reads it. The test shader draws a checker pattern. It reuses the blur's own output descriptor (root table 4, slot +2, found in the PIX capture) and restores the engine's compute bindings afterward.
- New settings: `[Debug] iMaskTest` (0 off, 1 checker squares forced into shadow, 2 forced lit) and `iMaskTestCell` (square size, default 64 px). The shipped ini sets `iMaskTest=1`.
- The plugin now tracks each command list's compute root signature, pipeline, root tables, root constants and last dispatch.
- First shader in the project: `shaders/MaskTestPattern.hlsl`, compiled with the Windows SDK's dxc into `src/generated/MaskTestPatternCS.h`.
- **Test:** load an outdoor daylight save. Sunlit ground should show a checkerboard of dark squares where the lighting thinks it's in shadow. The log should say "mask test after BlurY â€¦ applied".
- **Test result: works.** The checkerboard is visible in-game, so the lighting uses our values. The log shows the pipeline was created and the injection was applied after every BlurY (dispatch 430Ã—180, several worker threads). No crash. The plugin can now feed its own shadow term into Starfield's lighting.

## 0.1.7 - 2026-11-17

- Fixed a freeze on launch in 0.1.6. Setting PIX's target window called `GetWindowTextW` from a render worker thread. That sends a message to the game's main thread, which was waiting on the render thread, so both waited forever. The window lookup now uses only calls that don't send messages, and it runs on its own background thread so it can't block rendering.
- **Test:** same as 0.1.6. Launch, load the save, press F10 once, and wait about 10 seconds.
- **Test result:** works. No freeze on launch, and PIX's target window was set (class 'Starfield'). F10 produced a complete 5.6 GB capture (`Starfield_2026-11-17_23-41-01.wpix`). The game hangs for about 50 seconds while PIX writes the capture, which is expected.
- **Frame mapped from the capture (no code change):** every pipeline was named with Bethesda's shader technique names (399/399) by matching shader hashes against the game's shader archive. The sun's contact shadows go into an R8 screen mask (`ContactShadows`), which `TiledDeferredLight` reads at t4/space4. Indirect lighting (`Indirect`) and the final `Composite` were identified too. Details are in RT_FINDINGS.md. New analysis tools are in `tools/`. Temporary capture-session changes were restored (ReShade files, frame generation/Reflex, `bLoadPixGpuCapturer=0`).

## 0.1.6 - 2026-11-17

- **Test result:** froze on launch ("Starfield is not responding") right after the command list hooks were installed. Fixed in 0.1.7.

- Fixed PIX's "Take Capture" hanging forever on "stopping captureâ€¦". PIX wasn't seeing the game's presents, because Streamline creates and wraps the swapchain. The plugin now points PIX at Starfield's main window (`SetGlobalTargetWindow`).
- New in-game capture key (`[Debug] iPixCaptureKey`, default **F10**). It takes a programmatic one-frame GPU capture through PIX's `BeginProgrammaticGpuCapture`/`EndProgrammaticGpuCapture`, starting at one DeferredComposite pass and ending two passes later. It saves to `Documents\StarfieldRT Captures\Starfield_<date>_<time>.wpix` and doesn't need the PIX UI.
- **Test:** with PIX attached or not, press F10 in-game. The log should say "PIX capture begin" and then "PIX capture ended", and a .wpix file should appear.

## 0.1.5 - 2026-11-17

- New `[Debug] bLoadPixGpuCapturer` setting. When it's 1, the plugin loads Microsoft PIX's GPU capturer before the game creates its D3D12 device, so PIX can **attach** to Starfield launched normally through MO2/SFSE and take a full GPU frame capture. It picks the newest PIX under `%ProgramFiles%\Microsoft PIX`.
- The shipped ini enables it for the capture session. Set it back to 0 afterward.
- The pass survey from 0.1.4 is unchanged.
- **Test:** install PIX, launch the game, check that the log says "loaded PIX GPU capturer", then in PIX use Attach â†’ Starfield.exe â†’ GPU Capture.
- **First test:** crashed on game load in the game's present thread (a Present call returned E_ABORT), with DLSS Frame Generation and Reflex still on. Frame generation and Reflex replace the swapchain present path, and PIX's capture layer wraps that same path. Both are now off in the profile prefs for the capture session (backup: `StarfieldPrefs.ini.bak-before-pix`). The PIX capturer itself loaded fine.
- **Second test:** same crash with frame generation and Reflex off. The game's Present call (swapchain vtable +0x40) returned E_ABORT, and Starfield deliberately crashes on unexpected Present errors (0x142a0ef86). The cause is **ReShade**: the enabled MO2 mod "Starfield DLSS5" installs a ReShade `dxgi.dll` proxy plus a RenoDX addon, and PIX GPU capture doesn't work with DXGI-wrapping injectors. Disable that mod for the capture session.

## 0.1.4 - 2026-11-17

- **Pass survey.** Hooks the Execute of 15 candidate render passes (shadows, contact shadows, tiled lighting, Lighting, SAO, G-buffer, composite, sun disk, geometry gathering). For the first 3 runs of each it logs draws/dispatches, bound targets, and every resource barrier with the resource's size, format and state change. Goal: find where sun shadowing and lighting actually land on screen.
- Removed the ShadowMask debug strip and its ini settings. `StarfieldRT.ini` has no settings in this version.
- Handles passes nested inside other passes (subgraphs).
- **Test:** load any save, outdoors if possible, wait a few seconds, quit, and send the log. Nothing should look different.
- **Test result:** all 15 hooks installed, no crash. Internal resolution is 1720Ã—720 (DLSS). ScreenSpaceShadows and TiledDeferredContactShadowsBlurX build a full-res screen-space shadow term. Compute outputs are bound through descriptor tables, so finding the exact buffers needs descriptor tracking or a GPU frame capture.

## 0.1.3 - 2026-11-17

- 0.1.3 test result: no visible change, and none was possible. ShadowMask draws 1,024 instanced cubes (36 indices each) into a **64Ã—64 depth-only** target. It is a coarse shadow-caster mask, not the screen-space sun shadow mask, so there is no color target to write.

- Fixed the 0.1.2 shadow-mask test never running. The render graph binds the pass's render target and viewport **before** calling its Execute, and 0.1.2 only watched calls made inside Execute. The plugin now tracks the bound render target, depth target and viewport of every command list at all times.
- The log now shows each ShadowMask execute's draw call (indexed or not, index/vertex count, instance count) and gives a reason whenever the test clear is skipped.
- **Test:** same as 0.1.2. Load a save outdoors in daylight and look at the left third of the screen. Send the log.

## 0.1.2 - 2026-11-17

- 0.1.2 test result: no visible change. The log showed the clear was skipped on every execute ("RT 0x0", "viewport 0x0"), because the render target is bound outside Execute.

- First write into the game's frame: after the ShadowMask pass draws, the plugin can clear a strip of its render target (the sun shadow mask) to prove we can replace it with RT shadows later.
- Off by default in code. The shipped `StarfieldRT.ini` turns it on for this test build: `[Debug] iShadowMaskTest=1`, `iTestStripPercent=33`.
- Also captures the pass's render target (through OMSetRenderTargets or BeginRenderPass) and viewport, and logs them for the first 16 executes, so we can tell the per-frame ShadowMask runs apart.
- New `StarfieldRT.ini` next to the DLL.
- **Test:** load a save outdoors in daylight. Look for a strip on the left third of the screen where the sun shadows look wrong (all shadowed or all lit). Then try `iShadowMaskTest=2` and `0`. Send the log either way.

## 0.1.1 - 2026-11-17

- Fixed a crash on save load: 0.1.0 called QueryInterface on a non-COM NVIDIA driver object while scanning memory. Memory scanning is removed.
- Command-list discovery now wraps a few methods of the command-list vtable from the engine's device and tags calls made during ShadowMask Execute.
- The raytracing tier is logged as major.minor.
- Added README.md and AGENTS.md (versioning rules). No code change.
- **Test result:** no crash. The engine records ShadowMask straight into D3D12 DIRECT command lists (through Streamline), with 3 calls per execute on several worker threads. We can insert our own GPU work into the frame there.

## 0.1.0 - 2026-11-17

- First Phase 0 diagnostics build: hooks ShadowMaskRenderPass::Execute and logs the D3D12 device, graphics queue and DXR tier.
- Known issue: crashes on save load (fixed in 0.1.1).
