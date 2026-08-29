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

## Status of the Session 0 gates

- **G0a — strings (880):** PASS, four sessions running, content verified. The
  length-semantics bug above is separate and does not invalidate the round trip.
- **G0b — creature reads (881–884):** PASS in three sessions, one contaminated
  sample under investigation. Not the clean sheet needed to lift the mutator
  gate on its own; the diagnostic run above is what should settle it.
- **G0c — fog (885–888):** **never actually exercised.** Every session logged
  `fog support: off (no k2se_fog.txt next to the game exe)`. Tests 13–16 passed
  only because they are written to treat "off" as a valid state.
