# Session note, 2026-09-02 — how the driven player actually moves (static RE)

*Everything here was read out of `swkotor2.exe.pre-laa-backup` with the capstone
scripts in `tools/re/`; nothing was observed in game yet. The game was running the
whole time, so no file in the game folder was touched. Hypotheses are marked.*

## The chain, end to end

```
CSWPlayerControlCamRelative::Update(dt)            0x00865830   (ret 4)
  input (+0x10/+0x14) -> camera space -> RK4 toward target velocity
  GetMaxSpeed()                                    0x00867B40   3 call sites: 0x0086603C, 0x00867336, 0x00867AC7
  GetAccel() -> CSWCCreature::GetDriveAccel        0x00867CA0 -> 0x0077F600
  commits ORIENTATION only: CSWSObject::SetOrientation(&vec)   0x00543F10  (writes +0xA0, dirty bit 2)
  CSWCCreature::SetDriveSpeed(speed, 0.0)          0x00776E10  (writes client +0x3C8 / +0x3CC)

CSWSCreature::MovementUpdate                       0x005C3F70   (ret 4; 5 callers)
  when [this+0x11B0] == 3  -> DriveMoverUpdate     0x005C6340   (ret 0x10)
    reads the player's client creature drive speed (+0x3C8) via 0x0073FB90/0x007E5DA0
    runs a movement state machine ([state+4] = case, [state+0x44] = retries <= 50)
      cases call 0x005C7370, 0x005C7AD0, 0x005C87E0, 0x005C8C20, 0x005CA000, 0x005CA6A0,
                 0x005CACD0, 0x005CAE00, 0x005CAF30, 0x005CB060, 0x005CB190   (all ret 0x10, 4 args)
    delta = [state+0x5C] (target) - this->position (+0x94)
    if |delta|^2 > 0.001:
       area = CSWSObject::GetArea(this)                                    0x005453C0
       ok   = area->TestMoveSegment(&target, &pos, &pf.perspace, pf.hitradius, 0, 0)   0x00550980  [H]
       if ok: delta *= [0x00A0F5D8]; delta.z = 0; newpos = pos + delta
              area->GetGroundZ(&newpos, 1, 0, 0)                           0x0054B130  [H]
              CSWSObject::SetPosition(&newpos, 1, 1, 0)                    0x00543F50   <- site 0x005C6E87
       else if area->TestPointWalkable(&target, pf) == 1                   0x0054B650  [H]
              SetPosition(&target, 1, 1, 0)                                            <- site 0x005C6ED8
       else   retries++
    else if TestMoveSegment(...) == 1: SetPosition(&target, 1, 1, 0)                    <- site 0x005C6F9E
    0x0051CAE0(serverApp, this->id, &oldpos, &this->position)   -- area bookkeeping after the move [H]
```

`pf` = `[this+0x380]` = `CPathfindInformation` (`+0x04 perspace`, `+0x08 creperspace`,
`+0x10 cameraspace`, `+0x14 hitradius`), loaded by `CSWSCreature::LoadAppearance`
(`0x0057FD00..0x00580200`) from `appearance.2da`.

So the controller never writes the position: it sets heading and drive speed, and
the **server mover** integrates, tests the walkmesh and commits. That is why:

- **sprint** hooks `GetMaxSpeed` (the controller's speed cap), and
- **jump** hooks the three `SetPosition` calls inside the mover (the only place the
  driven creature's position is committed each frame), and
- **Tier 3 collision** (sliding) lives in the mover's state machine and its
  `0x005CAxxx` helpers, which is where the walkmesh tests are.

## SetPosition semantics (0x00543F50, ret 0x10)

`(Vector* pos, int a, int b, int c)`: returns early if the vector equals `+0x94`
(`0x00517980` = Vector::Equals); sets dirty bit 1 unless `c`; when `a` is non-zero
it notifies through two vtable calls (client update, hypothesis); ends with
`0x00522E00` (area objects array update, hypothesis). The mover always passes
`(vec, 1, 1, 0)`.

## Stealth bit, client side

`word [CSWCCreature+0x2EC] & 1` is written only by the server→client creature
update handler `0x008079B0` (3537 instructions, message readers `0x005E1C70..`),
together with `+0x2E8`, `+0x2EA`, `+0x2F0`, `+0x2F4`. Readers: the animation
choice (`0x0077713F` …), the speed functions (`0x0077F60C`, `0x00867BE0`) and
`0x008B9120`, which turns out to be the **HUD stealth button state** (pushes
0x9403/0x9404 to a GUI control), not a shader. Crouch therefore re-asserts the bit
every frame and accepts that the HUD stealth icon lights up.

## Animation codes

`CSWCAnimBase::vtable[0xE0]` (`0x00863650`) is a switch from 10000-based codes to
**animations.2da rows** (`0x290D → 0x237 = 567 = diveroll`); `vtable[0x44]`
(`0x00860280`) plays a row on the model (`0x008602C0(model, row, flag)`). Any 2DA
row — including a new one — can be played by calling `vtable[0x44](row, 1)` on
`CSWCCreature::GetAnimBase(client)` (`0x007ED830`). `anim.cpp` does exactly that.

## Other facts collected today

- `GetIsInCombat`: `[srv+0x520]` (combat round) non-null, and `byte [srv+0x11E8]` for
  "real" combat.
- `JumpToLocation` (handler `0x0066A230`) pops an engine structure with **tag 2 =
  location**, then queues an action (type `byte [0x994473] = 4`), it does not move
  the creature itself; `CutsceneMove` queues action type 0x3F via `0x0053F7F0`.
- Stealth speed: `GetMaxSpeed` returns `appearance+0x60` when the client bit is set
  and the creature lacks `FEAT_STEALTH_RUN` (197).
- `CSWSCreatureStats`: `+0x1A4` movement rate row, `+0x1A8/+0x1AC` walk/run rate
  floats filled by `SetMovementRate` from `creaturespeed.2da`.
- KPM's "Post-Combat Movement Fix" pattern (`mov reg,[reg+0x2A0] … call [+0x1C]`)
  matches three Steam sites: `0x007AF19E`, `0x007AF306`, `0x007AF3BA` (bonus, unexplored).

## What the code shipped tonight assumes (to confirm in game, session S1/S6)

1. Redirecting the three `GetMaxSpeed` call sites scales the driven speed without a
   server-side clamp (the mover reads the client speed and moves by it).
2. Setting the client stealth bit every frame yields the `stealth`/`pausestl`
   animations without the server's stealth mode.
3. Replacing `vec.z` in the mover's `SetPosition` calls lifts the model and the
   camera follows; the ground Z returns by itself when the parabola ends.
4. `vtable[0x44](567, 1)` on the player's anim base plays `diveroll` as an overlay.

## Live session S1 (evening) — what the game answered

1. **Client and server ids differ.** The controller's `+0x04` is the client object id
   (`0xFFFFFFF6` for the player); `GetCreatureByGameObjectID` on the server returns null
   for it. The engine's own path is client-first: `CClientExoApp::GetClientObject(id)`
   (`0x0073F550`) then `CSWCObject::GetServerObject` (`0x0077D800`). `player::Resolve`
   now does the same (`client=0x2F2FCAB8 server=0x258739B8 appearance=0x2F3DBA78`).
2. **GetMaxSpeed is not per-frame.** The RK4 block in Update runs only while the
   velocity is converging; at steady speed the controller stops calling it. Renan saw
   sprint and roll react only while turning with the mouse. Fixes: the speed factor is
   applied in `CSWCCreature::SetDriveSpeed` (call site `0x00867A33`, once per frame at
   the end of Update), and the frame boundary is now the controller's virtual
   `Update` (vtable `0x009A4818` slot 10, `0x009A4840` → `0x00865830`), swapped like the
   VM hook. GetMaxSpeed stays redirected only as pass-through instrumentation.
3. Roll works: `GetAnimBase` returns a subclass (vtable `0x009A4874`), `vtable[0x44](567, 1)`
   plays `diveroll` while running; cooldown and sprint combination fine.
4. **Keyboard directional movement** (WASD → controller axes `+0x10/+0x14`, written in the
   Update hook before the engine reads them) is built; needs `CameraRotateLeft/Right`
   moved off A/D (`--remap-keys`: `Action284A/B` 51/54 → Numpad 4/6 = 14/16).

## Camera and mouse (static, for the next step)

`0x0078C350` (709 instructions, called per frame) is the camera input handler:
- keymap action `0x11C` (284, CameraRotate) read through `0x0072F9D0(id, ...)` →
  `0x0072C320` (action state as float);
- mouse deltas: `0x0072FDE0(inputMgr [0x00A1B48C], &dx, &dy)` → `0x0072ED70(1, ...)`,
  dx clamped to ±1;
- `[0x00A7FCBC]` global flag copied to a local that the **Mouse Look** option bit
  (`CClientOptions+8` bit 1) inverts; `[0x00A7EBBC]` = reverse-axis sign;
- yaw: `0x007F4220(camCtrl, amount, dt)` (mouse: `-dx*sign`; keyboard: `±1.0`), where
  `camCtrl = 0x0073FEA0(clientApp) → [clientApp+4]+0x274` then `+0x18`; the camera
  behaviour object (`camCtrl+0x40`, vtable `+0x80`) receives the yaw at `+0x2C` or
  `+0x10C` depending on camera mode (`camCtrl+0xC`: 3 = chase, 5 = free look);
- pitch: `0x007F3DE0(camCtrl, amount)`; free-look: `0x007F3EA0 / 0x007F3C70` with the
  mouse sensitivity.

Hypothesis to test in game: with **Mouse Look** enabled the mouse rotates the chase
camera without holding a button (the engine already has the path); otherwise K2SE can
call `0x007F4220` itself with its own mouse delta each frame from the Update hook.

## Live session S1, second half (16:00) — three answers from the game

1. **swkotor2.ini [Keymapping] edits from outside do not stick.** Every backup taken
   by `--remap-keys` (13:58, 14:25) and the file the game rewrote at 15:46 all hold
   the stock values (`Action241=87` Space, `Action281A/B=76/53` Z/C,
   `Action284A/B=51/54` A/D). Renan confirmed in game: Space still pauses, A/D still
   rotate the camera. The codes themselves were right (they are the `language0`
   column of `keymap.2da`: 7/8 Left/Right, 9/10 Up/Down, 12/14/16/18 numpad 2/4/6/8,
   24/25 shifts, 28/29 ctrls, 39..44 F1..F6, 51..76 A..Z, 77..85 1..9, 87 Space,
   88 Enter, 89 CapsLock, 90 PauseKey), so the game either ignores the section at
   start or validates it against its own table and falls back. Either way the
   reliable path is the one the game persists itself: **Options → Keyboard** in game.
   The relevant rows are all `remappable=1`: 41 Pause (Space), 69 ActionLeft (Z),
   70 ActionRight (C), 75 CameraRotateLeft (A), 76 CameraRotateRight (D). Row 24
   Pause (PauseKey) is `remappable=0` and stays, so pausing is never lost.
   `--remap-keys` is therefore retired (it prints the in-game steps); `--restore-keys`
   stays for the backups it made.
2. **A standing jump has nothing to intercept.** `CSWSCreature::MovementUpdate`'s
   drive mover only runs while the player is driving, so with the player standing no
   `SetPosition` is called and the three (then eleven) redirected sites never fire:
   four jumps, "END after 0 frames". The jump is now *active*: while airborne and the
   mover did not commit during the previous frame, `OnFrame` calls
   `CSWSObject::SetPosition(&(p0 + (0,0,h(t))), 1, 1, 0)` itself, exactly the mover's
   call and arguments, SEH-guarded (`CommitPosition`). While running, the mover
   commits every frame and the hook adds the height and re-tracks the ground point
   `p0` from the mover's vector (it already ran `GetGroundZ`). Landing commits `p0`.
   Whether the client model and camera follow a server `SetPosition` with `a=1` when
   the creature is not driving is the thing this build answers.
3. **Diveroll as the jump animation reads as a roll.** `PlaceholderAnim` defaults to
   0 now (template, code default, and the deployed ini); the jump is animation-less
   until J4 (an authored jump animation row).

Also: crouch as a C toggle works and looks right to Renan (stealth posture); C is
still `ActionRight` in the stock keymap, so moving ActionLeft/Right off Z/C in
Options is recommended, not required.

## Live session S1, third run (16:40-16:47) and what SetPosition really does

**The in-game rebinding persisted.** swkotor2.ini after exit: `Action241=47` (Pause on
F9), `Action284A/B=7/8` (camera rotate on the arrows), `Action281A/B=51/54` — Renan put
**ActionLeft/Right (the game's own strafe, stock Z/C) on A/D**. So WASD 8-way is now
the *native* keyboard path; K2SE's axis writer is redundant for him and is switched off
in the deployed ini (`[Directional] Enabled=0`). The writer was made cooperative per
axis anyway (an axis the engine already holds non-zero is left alone), so it can never
fight the native strafe or a gamepad if someone enables it.

**Active jump ran, but the mover never committed.** Five jumps, each `0 mover + 54
active frames, 0.92 s`; the tracked ground point moved between jumps (the server
position *is* written by something), yet none of the eleven 0x005Cxxxx sites fired,
standing or not. Renan: "ancora tutto buggato" — symptoms to be described.

**CSWSObject::SetPosition(vec, a, b, c) read in full (0x00543F50..0x00544128):**
- early return when `vec == this+0x94` (`0x00517980`, Vector::Equals);
- `c == 0` → `0x0054A430(this, 1)` — sets dirty bit 1 (position changed, so the next
  server→client update carries it);
- when the global `[0x00A7FE98]` is set: a debug range check of the new vector against
  constants (never taken in normal play);
- `area = GetArea(this)` (`0x005453C0`); **copy vec into +0x94/+0x98/+0x9C** — the write;
- `a != 0` → `vtable[0x30]` (AsCreature) and, for a creature, a look at
  `[creature+0x1198]->+0x6C` (stored, not used here), then `0x00522E00(area, this)` —
  the area's spatial bookkeeping;
- `b` is not read by this function at all.

So SetPosition **does not notify the client**; the client learns a server position only
through the regular server→client creature update (the handler `0x008079B0` on the
client side), driven by the dirty bit. Corrects the earlier hypothesis ("two vtable
calls = client update"). The model is placed by those updates, so a lifted server
position should show — unless the client treats the local player differently, or
something re-grounds the creature each frame. The 78-site diagnostic (all direct callers
of SetPosition in the exe, `tools/re`-style linear scan) is installed at 16:50; the
first eight distinct sites that commit the player's position get logged with their
arguments. That answers where the player's server position is really written.

Direct callers of SetPosition outside the movement code, by region: 0x0052xxxx (area/
object load), 0x0053xxxx-0x0057xxxx (CSWSObject/creature helpers, 0x0053E9E0,
0x005534A2), 0x0058xxxx (creature spawn/appearance), 0x005Bxxxx-0x005Fxxxx (creature
update paths incl. 0x005D913F/0x005D9178, 0x005F2AE5/0x005F2FD6, 0x005F9EAA/0x005F9FA3),
0x0060xxxx-0x0062xxxx (message handlers / CServerExoApp), 0x0066Fxxx-0x00670xxx (script
commands), 0x0069xxxx, 0x006E45BA, 0x007E8621 (client side), 0x008A04B7.

## Build 16:55 — position scan + client-lift experiment

Renan's answer on the third run: **Space does nothing visible** (server-side lift of 54
frames per jump, invisible), A/D strafe and the arrow-key camera work. So the model is
placed from a client-side copy of the position, not from `CSWSObject+0x94` directly.
Rather than guess offsets, the build finds them: at the first jump `RunPositionScan`
scans the server creature (control: must report `+0x094`), the client creature
(0xA00 bytes, plus everything its first 0x300 bytes point to), the controller and the
anim base (0x400 bytes, plus pointees) for three floats equal to the takeoff position
(x/y within 0.05, z within 0.6), logs every hit (`movement: posscan ...`, capped at 60
lines) and re-reads the hits at airborne frames 12 and 30 to see which copies follow
the server lift and which stay on the ground. With `[Jump] ClientLift=1` (on in the
deployed ini for this experiment) the height is also written into the Z of every hit
inside the client creature each airborne frame, and reset at landing. `anim::AnimBase`
is now public for this. The 78-site SetPosition watch from 16:50 stays in.

## Fourth run (18:22-18:35) — the jump is visible; where the player really lives

Renan: "the jump is weird but it works, only when the player is idle". The log:

- **The player's server position is committed from `0x00569881` (args 1,0,0)** — every
  frame while driving — and once from `0x00538C7B` (args 1,1,0). Neither is in the
  0x005Cxxxx mover: the eleven sites we watched first were the wrong ones. With the
  78-site watch the running jump got `104 mover + 1 active frames` (the hook lifted the
  engine's own commits) and the standing jump `54 mover + 1 active`: the active path is
  now only the first frame, because 0x00569881 fires even when idle (it is per-frame,
  not movement-gated). To redirect for the jump from now on: **only 0x00569881** (plus
  0x00538C7B pass-through); the other 76 can go back to untouched.
- **Client-side copies of the position** (posscan): `CSWCCreature+0x024` and `+0x1E4`
  (both equal to the server vector), `CSWCCreature+0x???` holds the pointer to the
  server creature (the `client->+0x094` hit), and the **model**: the anim base points
  to an object with the position at `+0x0A4` and `+0x0C0` (`0x2D605CB0` here);
  `+0x0C0` lags `+0x0A4` by ~0.05 mid-air, so `+0x0A4` is the model's target and
  `+0x0C0` the smoothed/rendered one. All of them followed the lift at frames 12 and 30
  (10.31 / 10.66), so the server → client sync does carry the position; the lift is
  visible now because the engine's own commit path (0x00569881) carries it, and
  `ClientLift` writes `+0x024/+0x1E4` directly as well.
- "Weird" and "idle only" are the next iteration (J2): no animation, the horizontal
  motion while running probably gets re-grounded by the movement code between commits.
  Parked by Renan's choice; multi-FOV first.

## Evening: k2-multi-fov started (RE complete, module written)

- `gluPerspective` (GLU32, IAT `0x00986028`) is called from exactly one place: `E8`
  at `0x0047F6D7` → thunk `0x0093B436`. The caller is `Camera::ApplyProjection`
  (`0x0047F320`, slot 2 of the Aurora `Camera` vtable `0x0098C45C`, RTTI
  `.?AVCamera@@`, 37 slots). fovy = `[this+0x204]` (vertical degrees, default 45.0 at
  `0x0098C274`), near `+0x210` (0.1), far `+0x214` (100.0). Right after the call the
  same function builds the culling frustum from `tan(fov/2)`: the FOV must change in
  the field, not in the GL call, or edge objects get culled.
- Slot 3 `Camera::Update(float dt)` (`0x00480E40`) advances the FOV animation at
  `+0x208` (dialogue zoom) then updates the transform; slot 17 `SetFOV(float)`
  (`0x0047F190`), slot 16 `SetNearFar`. Full table in `k2-multi-fov/DESIGN.md`.
- `fov.cpp`: one vtable-slot swap (slot 3). After the original Update: adopt any
  value K2SE did not write as the camera's vanilla; in gameplay frames (player
  controller ticked < 100 ms ago, via `movement::UpdateCount()`) write the smoothed
  target (Exploration / Combat / +SprintAdd / hotkey offset, clamped); otherwise put
  the vanilla back. Per-instance tracking (4 cameras). Routines 894 `K2SE_SetFOV`,
  895 `K2SE_GetFOV`. `[FOV]` section in the ini template; deploy `--enable fov`.
- Added to the CSV (11 rows, all probes pass): Camera vtable, ApplyProjection, Update,
  SetFOV, SetNearFar, offsets FOV/FOVAnimation/Near/Far, DefaultFOV, the
  gluPerspective call site. `movement.h` now exposes `UpdateCount()` and
  `PlayerInCombat()` for other modules.
- Texture harvest (sub-agent, Opus): 41,118 loose textures / 11.9 GB from 130 of the
  1957 Skyrim mod folders in `D:\mods` → `k2-texture-pack/raw/<mod>/...` with
  `MANIFEST.csv`, `INVENTORY.md`, `PROVENANCE.md`, `tools/harvest.py` (idempotent).
  Categories: architecture 6.2 GB, landscape 2.9 GB, clutter 2.5 GB, industrial 0.7 GB,
  materials 0.3 GB. Personal use only; every file keeps its Skyrim author.

## 19:50-20:03 — "multi-FOV" means three camera views; first FOV hook was dead

Renan (caps): no FOV change seen; what he wants is a cycle between **near, far and
first person**, and the game's first-person key (free look, CapsLock) freezes movement.
The log explained the first part: `fov: removed after 0 camera updates` — `Camera::Update`
(slot 3) is never called for the scene camera. The hook moved to **slot 2,
`ApplyProjection()`** (`0x0047F320`, ends with a plain `ret` at 0x00480429: no
arguments), runs before the original so both gluPerspective and the frustum see the value.

The chase camera is **`CSWBehaviorCamera`** (RTTI, vtable `0x009A1F94`, ctor
`0x007DD380`). `LoadCameraStyle(int row)` `0x007E1CA0` maps camerastyle.2da:
DISTANCE→+0x16C (copy +0x88), SPEED→+0x170, PITCH→+0x178, HEIGHT→+0x17C,
TILTSPEED→+0x190, row→+0x5C, VIEWANGLE→+0x8C **and** `Camera::SetFOV` through
`[this+0x1C]->vtable[1]()` — so the real vanilla FOV in game is **55**, not 45. Three call
sites (`0x007E1C0B` wrapper from the ctor, `0x007DDC52` SetupFromDefinition,
`0x007DE7D6` ApplyPendingStyle with the pending row in global `0x00A108F8`). Chase
placement reads +0x16C/+0x17C/+0x178 at 0x007E0D25, 0x007E22EF, 0x007E23D2,
0x007E152B, 0x007E2339, 0x007E2492, 0x007DD644, 0x007DDD72, 0x007DE199, 0x007DE83A.

New module `camera.cpp`: the three sites redirected (pass-through + learn the object and
its loaded values), presets near/far/first person written into +0x16C/+0x88/+0x17C/+0x178
every gameplay frame, key **N** (unbound in keymap.2da) cycles, banner; the preset's FOV
feeds `fov.cpp`, and first person raises the Aurora near plane (+0x210) to 0.35 to clip
the player's own body. Routines 896/897. 19 CSV rows added (209 total, all probes pass).
Deployed 20:03 (DLL 184,320 B) with sprint, roll, crouch, jump, fov, camera. Untested.

## 20:10-20:45 — spawner + NPC variety (Renan: "spawner e varietà npc prima")

**NPC variety** (`npcvariety.cpp`): `CSWSCreature::LoadAppearance` is `0x0057FCE0`
(thiscall, one int, ret 4; the 0x0057FD00 dump from the morning began mid-function).
It reads the appearance.2da row from `word [this+0x1184]`; the only 16-bit stores of
that field are in `SetAppearanceType` (`0x00585120`, ret 16) and `SetAppearanceVariant`
(`0x00585310`). Four E8 callers (`0x00576AAF` creation, `0x00585241`, `0x0058536E`,
`0x0065A9DD`) are redirected: before the original runs, a row belonging to one of 7
"families" (same body model, different head — N_CommF 13 rows, N_CommM 12, N_CzerkaOff 6,
N_RepSold 6, N_RepOff 5, N_RepSold_F 4, N_RepOff_F 3, from the stock 2DA) is replaced by
another row of the family, chosen by a hash of the creature's object id (read at +0x4 and
round-tripped through `GetCreatureByGameObjectID` before use, so the KPM-unverified
offset never decides anything on its own). Party/unique rows are outside the families.
The creature GFF loaders (`0x006AFED0`, `0x006B3D10`) read `Appearance_Type` and
`Appearance_Head` but no `TextureVar` (that string is only used by the item loader
`0x00601740`, field +0x2A6), so clothing-texture variety is not a UTC knob in TSL.

**Spawner** (`spawner.cpp` + `nss/k2se_spawn.nss`): no engine object creation from the
DLL. K2SE runs the compiled script through `CVirtualMachine::RunScript` (`0x006FD8D0`,
`(CExoString*, uint32 oid, int)` ret 0xC, VM singleton `[0x00A1B4A8]`) 1 s after every
camera-style load (= area setup, from camera.cpp) and every 4 s of gameplay; OBJECT_SELF
is the PC's server id, self-validated. The script: `K2SE_SpawnBegin(GetModuleName(),
GetTag(area))` → K2SE loads `<game>\k2se_spawns\<MODULE>.ini`; objects the script
created carry LocalBoolean 150 + LocalNumber 25 (saved with the area) so reloads never
duplicate; missing entries go through the engine's `CreateObject`. F10 appends the
player's position/facing as a ready `[spawnN]` block to `k2se_spawns\_captured.txt`.
Routines 898..908; deploy compiles the script with nwnnsscomp against nwscript_k2se.nss
into override/. Installed 20:45 (DLL 195,584 B, 13 CSV rows added, 222 total). Untested.
