# Porting Kotor-Patch-Manager findings into K2SE — design

**Date:** 2026-08-28
**Source studied:** `Kotor-Patch-Manager` (LaneDibello, MIT), local copy at `~/Desktop/Patch-Manager`
**Status:** approved, implementation in progress

---

## 0. Executive summary

Patch-Manager's KOTOR 2 address database targets **the exact binary K2SE targets** and
every address K2SE derived by hand agrees with it. That mutual confirmation, plus six
genuinely new `CVirtualMachine` entry points, is the real prize — not the volume of data,
which is small.

Five tracks, in order: **A** addresses-as-data, **B** strings and engine structures,
**C** object API and first real routines, **D** Ghidra pipeline, **E** runtime fog.

**Licensing posture:** no Patch-Manager source is copied. Addresses and struct offsets are
facts about a binary, imported with provenance recorded per row. All C++ and all tooling in
this project is written fresh. The local copy of the upstream repo has no root `LICENSE`
file and is not a git clone, so nothing is taken on the strength of its README's MIT claim.

---

## 1. What was verified before designing anything

K2SE's discipline is that no address is trusted until it is read back out of the binary.
Every claim below was re-derived locally against
`swkotor2.exe.pre-laa-backup`, not accepted from the other project.

| Claim | Method | Result |
|---|---|---|
| The DB describes our binary | SHA-256 of the pristine backup vs `game_version.sha256_hash` | exact match, `6A522E71…BFFEF` |
| The 48 function addresses are real | read each entry point, require the MSVC prologue `55 8B EC` | **48/48** in `.text` |
| Steam rows are not a copy of GOG | diff all 48 against `kotor2_gog_aspyr.db` | all differ, deltas non-constant → genuinely re-located |
| The new stack accessors share the proven ABI | disassemble all 13 `CVirtualMachine` thunks, read each `ret imm16` | identical thunk shape |
| `CExoString` layout | disassemble the 4 ctors/dtor | `{char* @0x00, uint32 @0x04}`, 8 bytes |
| The fog IAT slots in DESIGN.md §5 | resolve the PE import directory by name | **4/4 correct** |

Scripts live in `tools/`; the prologue sweep is permanent, not one-off (see Track A).

### Provenance caveat, recorded deliberately

`kotor2_steam_aspyr.db`'s own description says it was *"generated from GOG Aspyr
mappings"* — auto-ported, not independently reversed. The 48/48 prologue sweep is what
promotes it from *claimed* to *verified*, and it is the reason that sweep becomes a
permanent part of the toolchain rather than a one-time import check. Rows whose semantics
have not been confirmed by reading a consuming handler are marked as such in the CSV.

---

## 2. Correction to DESIGN.md §1

DESIGN.md assumes the address database represents a year of Ghidra work that K2SE would
otherwise duplicate. That is **wrong**, and the wrong version of this belief could justify
bad decisions later.

| Database | functions | globals | offsets | classes |
|---|---|---|---|---|
| `kotor2_steam_aspyr.db` | **48** | 14 | 21 | **0** |
| `kotor1_0_3.db` | 9711 | 21 | 4727 | 977 |

The K2 tables are small. Their value is **confirmation**, and six new VM entry points.
The KOTOR 1 database is the genuinely large asset, and it is useful to K2SE as a
**structure oracle**: the addresses are K1, but Odyssey struct layouts and method
inventories carry over. It independently confirms `CSWVirtualMachineCommands::commands =
+0x0C` — the offset K2SE derived from instruction bytes — and it supplies
`CVirtualMachineInternal` / `CVirtualMachineStack` layouts, `C2DA` accessors, `CExoDebug`
logging, and 577 `CSWVirtualMachineCommands` methods named `ExecuteCommand<RoutineName>`.

---

## 3. Track A — addresses become data

**Problem.** `src/offsets.h` is hand-written and hand-audited. The imported set is roughly
5× larger; hand-maintaining it would break the property that makes it trustworthy.

**Design.**

```
data/k2se_addresses.csv      single source of truth, human-reviewable, committed
tools/import_kpm_db.py       one-shot import; prologue-verifies before admitting a row
tools/gen_offsets.py         CSV -> src/offsets_generated.h
src/offsets_generated.h      generated, committed so a build needs no Python
src/offsets.h                keeps hand-derived constants; includes the generated header
tools/verify_offsets.py      extended to probe every CSV row
src/fingerprint.cpp          runtime probes for addresses K2SE actually calls
```

CSV columns: `kind, class, name, value, provenance, verified_by`.

- `kind` ∈ `function | global | offset | constant`
- `provenance` — where the value came from (`k2se-ghidra`, `kpm-db`, …)
- `verified_by` — how it was checked *on this binary* (`prologue`, `callsite-rel32`,
  `disasm`, `runtime`, `unverified`)

**Why SQLite stays build-time only.** The shipped DLL imports only KERNEL32 and works
without the VC++ redistributable. A runtime database would add a DLL dependency, a file to
locate, and a failure mode, to solve a problem K2SE does not have: it targets one build.

**Invariant.** `verify_offsets.py` fails if the CSV and the generated header disagree, so
the two cannot drift.

---

## 4. Track B — strings and engine structures (closes Q11)

Six new entry points, all confirmed as the same thunk family K2SE already drives:

| Accessor | VA | `ret` | Note |
|---|---|---|---|
| `StackPopString` | `0x006FDA70` | 4 | byte-identical shape to `StackPopInteger` |
| `StackPushString` | `0x006FDA90` | 4 | |
| `StackPopEngineStructure` | `0x006FDAB0` | 8 | two args |
| `StackPushEngineStructure` | `0x006FDAD0` | 8 | two args |
| `StackPopCommand` | `0x006FDB30` | 4 | |
| `RunScript` | `0x006FD8D0` | — | not wrapped yet |

`StackPopVector` / `StackPushVector`, which `offsets.h` currently marks *"shape-verified
only, do NOT wrap"*, come back `ret 4` and `ret 0xC` — matching the predicted shapes, so
the warning can be lifted.

**Design.**

- `src/vmstack.h/.cpp` — the stack accessors move out of `routines.cpp`'s anonymous
  namespace into a real module. Track B roughly doubles their number; they need a home.
- `src/exostring.h/.cpp` — an RAII `CExoString` holder calling the **engine's own**
  constructors and destructor, so the engine's allocator owns the inner buffer. Size 8 is
  a compile-time constant checked against the CSV.
- Routine **880** `K2SE_EchoString`, a string round-trip self-test in the same shape as
  the existing 878/879 battery.

**Not yet fact.** The engine-structure type tags (`0=effect, 1=event, 2=location,
3=talent`) come from the other project and have **not** been confirmed against this
binary. They are a hypothesis to be tested by reading a consuming handler before any
location/effect routine ships. `StackPopEngineStructure` is wrapped but no routine exposes
it until that is done.

---

## 5. Track C — object API and first real routines

The lookup chain, all four addresses prologue-verified:

```
*(void**)0x00A1B4A4              CAppManager
  +0x08                          CServerExoApp
  CServerExoApp::GetObjectArray            0x0051C080
  CGameObjectArray::GetGameObject          0x0053DFB0   (thiscall, (id, void** out) -> int)
  vtable slot 12                           AsSWSCreature, null if not a creature
```

`CServerExoApp::GetCreatureByGameObjectID` (`0x0051C100`) does id→creature in one call and
is the simpler path where a creature is what's wanted.

**Correctness note.** The reference implementation in the other project leaves the output
pointer uninitialized and discards the `int` success flag, so a failed lookup returns
garbage stack memory rather than null. K2SE's version initializes the out-pointer and
honours the return value.

**First real routines** are drawn from the 15 verified `CSWSCreatureStats` entries
(attributes, skills, feats, spells) — the first K2SE functions intended for actual mods
rather than self-tests.

---

## 6. Track D — Ghidra pipeline and upstream contribution

Written fresh, in Python, to match K2SE's existing toolchain:

- `tools/ghidra/ExportK2SE.java` — exports named functions to CSV. RFC-4180 correct on
  both sides, because the reference implementation's reader splits on physical lines while
  its writer quotes embedded newlines — a latent corruption bug.
- `tools/import_ghidra_csv.py` — merges an export into `data/k2se_addresses.csv`.
- `tools/export_to_kpm.py` — emits CSVs in the upstream importer's format.

The contribution matters because the K2 schema currently has **no home** for what K2SE
knows: the `0x009940D0` vtable, `ExecuteCommand`, the 877-entry routine table, or
`CSWVirtualMachineCommands` itself. None of it appears in any K2 database.

---

## 7. Track E — runtime fog

**Confirmed:** all four IAT slots named in DESIGN.md §5 are correct, resolved by name from
the import directory — `glFogf 0x00986394`, `glFogfv 0x00986398`, `glFogi 0x009863B0`,
`gluPerspective 0x00986028`.

**Confirmed:** the premise. The Aspyr ARB fragment programs contain no `state.fog`
reference, so GL fog state is inert until the shaders honour it. Writing fog state alone
would produce no visible change — exactly as DESIGN.md M5 warns.

**Design.**

1. Hook `glProgramStringARB` and inject `OPTION ARB_fog_linear;` immediately after the
   `!!ARBfp1.0` header. One token, no per-shader knowledge, and it makes the existing
   fixed-function fog state live. Preferred over rewriting each shader body.
2. IAT-patch `glFogf`/`glFogfv`/`glFogi` **by import name**, using the verified slot
   addresses as assertions rather than as the lookup. Name resolution survives a rebuild;
   a hardcoded slot does not.
3. Resolve `glProgramStringARB` through `wglGetProcAddress` — it is an extension entry
   point, not an `opengl32.dll` export, so there is no IAT slot for it. This must happen
   off the loader lock, after a GL context exists; not in `DllMain`.
4. Investigate `0x0046A97D` — a `__thiscall` reachable from the grass-fog code path and
   adjacent to the fog-disable site at `0x0046AADD`. A strong candidate for the engine's
   own apply-area-fog method, and preferable to writing GL state ourselves if it is.
5. Detect an already-installed shader fix and degrade with a clear message rather than
   fighting it, per DESIGN.md M5.

**Also settled:** DESIGN.md §8 item 8 says `0x0085CE5D` is not a valid hook site for
Steam-Aspyr because it is an unported GOG copy. That is confirmed — it is the GOG
counterpart of Steam's `0x0046AADD`.

---

## 8. Plan D: table expansion, documented as an alternative

The KOTOR 1 script extender in Patch-Manager uses a strategy DESIGN.md §3.3 does not list.
Rather than replacing the dispatcher, it moves the ceiling with three coordinated byte
patches — the allocation size (`count × 4`), the dispatcher's bound check, and a detour on
a function tail that fills the enlarged table.

For K2SE this would mean patching `0x00665F5A` (`0x0DB4`), `0x00668FDC` (`0x36D`), and
`0x00665F87` (`0x36D`) — all three already known and fingerprinted.

**Not adopted.** K2SE's vtable-slot swap is strictly more general: it can intercept vanilla
routines, which is what makes the `abs()` presence sentinel possible, and it needs no
writes to `.text` at all. Recorded in DESIGN.md as plan D so the trade-off is explicit
rather than rediscovered.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Imported semantics wrong even though the address is real (auto-ported DB) | `verified_by` per row; nothing marked `unverified` is called from a shipping routine |
| Engine-structure type tags unconfirmed | wrapped but unexposed until a consuming handler is read |
| `CExoString` ownership subtleties → heap corruption | use the engine's own ctors/dtor; never free the inner buffer; in-game test before shipping |
| Fog work destabilises a working DLL | Track E is self-contained; a failure to resolve GL entry points disables fog and leaves the VM hook untouched |
| Static file patches (4GB/LAA, DeSteamify) break an on-disk hash | already correct: K2SE fingerprints in memory and masks `Characteristics` |

**In-game verification cannot be performed by the implementer.** Every track that changes
runtime behaviour ships with a self-test routine in the existing 878/879 battery style, and
must be confirmed by a real session before it is considered done. Static verification
(prologue probes, build, tool output) is necessary but not sufficient, and is not reported
as if it were.
