# Session note, 2026-08-29 — the creature reads did not fail the way it looked

## The alarming reading, and why it was wrong

The 19:18 session logged this:

```
TEST  7: expected 1, got 1  ->  PASS
TEST  8: expected 1, got 0  ->  FAIL
TEST  9: expected 1, got 0  ->  FAIL
TEST 10: expected 1, got 0  ->  FAIL
TEST 11: expected 1, got 0  ->  FAIL
TEST 12: expected 1, got 0  ->  FAIL
```

Tests 8–12 are exactly routines 881–884 — the creature reads, the part of K2SE
that walks imported struct offsets and is described in `routines.cpp` as the
least-verified thing in the project. Five consecutive failures there reads, at
first glance, like the offsets are wrong and the mutator gate must stay shut.

It is not what happened. Counting every occurrence in the log tells a different
story:

| Session | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|
| 2026-08-28 20:41 | PASS | PASS | PASS | PASS | PASS | PASS |
| 2026-08-29 01:46 | PASS | PASS | PASS | PASS | PASS | PASS |
| 2026-08-29 16:24 | PASS | PASS | PASS | PASS | PASS | PASS |
| 2026-08-29 19:18 | PASS | **FAIL** | **FAIL** | **FAIL** | **FAIL** | **FAIL** |

Same DLL (`version.dll`, built 2026-08-28 09:41), same offsets, same fingerprint
probes passing, same self-validation. **The creature reads have already produced
correct values in three consecutive live sessions.** Whatever went wrong at
19:18 is a property of that session, not of the pointer chain.

## Why one bad first sample poisons a whole session

`H_ReportTest` (routines.cpp) logs each test id exactly once per session:

```cpp
const bool firstTime =
    nTestId >= 0 && nTestId < 32 && !(g_reportSeenMask & (1u << nTestId));
if (firstTime) {
    g_reportSeenMask |= 1u << nTestId;
    log::Writef("TEST %2d: ...");
} else {
    log::Trace("test %d: ...");
}
```

The battery is wrapped around a creature heartbeat, so it fires every ~6
seconds — but only the **first** result is ever written as a `TEST` line. Every
later result goes to `log::Trace`, which is silent unless the `K2SE_DIAGNOSTIC`
marker file exists. That marker has been absent in every session so far
(`diagnostic marker: absent`).

So the visible result is not "the routines fail". It is "the routines failed on
the first heartbeat after module load", with all subsequent evidence discarded.

## The leading hypothesis

Every failing test needs `oPC`, obtained from `GetFirstPC()`. Test 7 — the only
one in that group that passed — is the string echo, which needs no game object
at all. That is a clean split: everything requiring the player failed, and the
one thing that did not require the player passed.

The likely mechanism is that the first heartbeat fired before `GetFirstPC()`
could resolve to a live creature, so `CreatureFromObjectId` returned null and
all four routines returned their `-1` sentinel. Consistent with this, the log
contains **no** `gameobj: GetCreatureByGameObjectID faulted` line — the SEH
guard never fired, so this was not an access violation. It was one of the silent
early returns.

## The diagnostic gap this exposed (worth fixing regardless)

`gameobj.cpp` logs only on fault. Every plausibility rejection —
`ServerExoApp()` returning null, `LooksLikePointer(creature)` failing,
`objectId == kObjectInvalid` — returns silently. When the chain breaks there is
nothing in the log that says *which link* broke, which is what turned a
ten-second question into an investigation.

That is a real defect in the diagnostics, independent of whether the offsets are
right, and it should be fixed with log-once (or rate-limited) reporting on each
refusal path. The heartbeat cadence means unconditional logging would flood the
file, so log-once per reason is the correct shape.

## SETTLED — the on-screen banner, read live in the failing session

The test script calls `AurPostString` on every heartbeat with a five-second
life, so the banner shows the **current** values while the log had frozen the
**first**. Read off the screen of the very session whose log says FAIL five
times:

```
K2SE v100   str 14/14   dex 12   heal 2/4
```

That is the whole diagnosis in one line:

- `str 14/14` — K2SE's base STR (routine 881) and vanilla `GetAbilityScore`
  agree exactly, which is what must happen when nothing is modifying the stat.
- `dex 12` — a second plausible attribute through the same routine.
- `heal 2/4` — and this is the one that proves the routine is real rather than
  lucky. K2SE reports base rank **2**; vanilla `GetSkillRank` reports **4**.
  They *differ, correctly*: routine 882 returns the rank the character actually
  bought, without item or effect modifiers, which is precisely the number
  vanilla cannot report and the reason the routine exists. A broken chain
  returns −1; a chain that merely echoed vanilla would have said 4.

No −1 anywhere. **The creature reads work.** The five logged FAILs were the
first heartbeat after module load, frozen into the log by the one-shot dedup
and never corrected, while every passing heartbeat since went unrecorded.

Scope of what the banner proves: routines **881 and 882 are confirmed live**.
883 (`HasFeat`) and 884 (`HasSpell`) are not on the banner — they share the
identical `PopCreatureStats` prologue, so the pointer chain they depend on is
confirmed, but their own engine calls still want a session with tracing armed
before the mutator gate is called settled.

## The experiment that settles it — no rebuild required

Both marker files are now armed in the game folder:

- `K2SE_DIAGNOSTIC` — turns on the `log::Trace` lines, so **every** heartbeat's
  result becomes visible, not just the first.
- `k2se_fog.txt` — arms the fog path, which has been `off` in every session
  recorded so far, meaning routines 885–888 have still never actually run.

One restart with a save loaded therefore tests two things at once. Expected
evidence:

- lowercase `test 8: expected 1, got 1 -> pass` lines appearing a few seconds
  after the uppercase `FAIL` ⇒ hypothesis confirmed, harness race, offsets fine;
- `glhook: first fragment program rewritten` ⇒ the fog path is genuinely live
  (without that line, any visual judgement about fog is meaningless).

## Second, unrelated bug found in the same log

```
*** FIRST STRING ROUND TRIP: received "K2SE-880-abc" (length 13) ***
```

`"K2SE-880-abc"` is **12** characters. `ExoString::length()`
(`exostring.cpp:58`) returns the engine's length field raw, so on this build
that field counts the NUL terminator (or `kExoStringOffLength` points at a
size-with-nul/capacity field rather than the character count).

Test 7 still passes because NWScript compares string *content*, which round-trips
correctly — so this is latent, not currently harmful. It stops being latent the
moment the planned string routines (split, trim, replace) use `length()` as a
character count. Reproduced identically in all four sessions, so it is stable
and not a one-off.

## Outcome — the 2026-08-30 00:55 session, all three gates closed

Twenty tests, twenty PASS, on the instrumented build with both markers armed.

**G0a, strings.** `received "K2SE-880-abc" (length 12, buffer 13)`. The two
numbers differ, which is the finding: 12 characters in a 13-byte allocation.
Tests 18–20 pushed 12, 3 and 0 characters through the same shell and all
matched vanilla `GetStringLength` — the reuse case that a `capacity - 1`
implementation would have failed, and the empty string that would have
underflowed it to 4294967295.

**G0b, creature reads.** Tests 8–12 PASS, and test 17 (`oPC != OBJECT_INVALID`)
PASS — so this time the heartbeat found a player, which is exactly the variable
that differed on 2026-08-29. **The mutator gate's condition is met**: the reads
are confirmed in a live session.

**G0c, fog.** The go/no-go line appeared for the first time in the project's
history:

```
glhook: first fragment program rewritten (272 -> 295 bytes, OPTION ARB_fog_linear inserted)
```

Track E is live. The ARB option is being injected into real fragment programs on
the render thread, which is what the whole opt-in marker exists to protect.

**The instrumentation earned its place.** The whole session produced exactly
**one** `gameobj:` line —

```
gameobj: K2SE_GetAbilityScoreBase walk #1 at t+398234 ms -> OK | id=0x7FFFFFF9 app=0x04057168 srv=0x2576F6F8 cre=0x2F9E2150 sts=0x2F884980
```

— because the chain never changed state. That is the design working: a healthy
session costs one line, and any second line would have named the hop that
stopped. The transition logging in `ReportTest` likewise wrote 11 lines where
per-call logging would have written tens of thousands.

One thing the session did expose: with `K2SE_DIAGNOSTIC` armed the per-dispatch
trace wrote **205,808 lines and 9 MB**, burying everything worth reading. That
trace is now thinned to the first eight calls per routine and one in every 1024.

## Status of the Session 0 gates

- **G0a — strings (880):** ✅ CLOSED. Round trip verified, and the length
  semantics corrected and re-proven across three string sizes.
- **G0b — creature reads (881–884):** ✅ CLOSED. Twenty-for-twenty with the
  subject explicitly validated. **The mutator gate may be lifted.**
- **G0c — fog (885–888):** ✅ CLOSED. `first fragment program rewritten` — the
  first time the fog path has ever actually run.
