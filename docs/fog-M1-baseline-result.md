# Fog M1 — the baseline test, and why it failed

**Result: injecting `OPTION ARB_fog_linear` is not safe on its own. Onderon's
western square renders black with no K2SE override active whatsoever.**

## What M1 was supposed to separate

Arming `k2se_fog.txt` does two independent things, and an earlier session
conflated them badly enough to repaint Onderon dark brown:

1. **Shader injection.** `OPTION ARB_fog_linear` is inserted into every ARB
   fragment program, which makes the program honour `state.fog`. On the stock
   Aspyr build the programs ignore fog entirely — that is the Aspyr bug.
2. **The script override.** `K2SE_SetFogEnabled(TRUE)` makes the `glFog*` hooks
   substitute K2SE's own range and colour for whatever the engine passes.

M1 was meant to be (1) alone: arm the marker, set no override, and look at what
the engine's own authored fog does once the shaders finally respect it. The
hypothesis was that (1) is a straight bug-fix and probably looks *better*.

## What actually happened

The hypothesis is wrong. With the override provably off, the western square of
Iziz went black.

The proof that the override was off is in the log, not in an assumption:

```
TEST 21: expected 0, got 0  ->  PASS      <- kOverrideActive (bit 2) is clear
fog range set: 20000..120000 (x1000)      <- values STAGED, once
fog colour set: 63,33,15 (x255)           <- values STAGED, once
```

and, decisively, **no `fog override enabled` line anywhere in the session**.
`SetRange` and `SetColor` only stage values behind `haveRange`/`haveColor`;
only `SetEnabled(true)` sets `g_fog.enabled`, and only `g_fog.enabled` makes the
`glFog*` hooks substitute anything. So every fog value reaching the driver in
that session was **the engine's own**.

```
glhook: first fragment program rewritten (272 -> 295 bytes, OPTION ARB_fog_linear inserted)
```

That line, plus a black area, is the whole finding.

## What it means

The Aspyr build's fog state is not merely *ignored* by its shaders — there is
good reason to think it is *wrong*, and harmlessly so precisely because nothing
reads it. Uninitialised or nonsensical fog parameters (a black fog colour, or a
start/end pair that puts everything past the far plane) cost nothing while the
fragment programs discard them. Teach the programs to obey, and the same values
paint the world black.

This reframes the fog work:

- **The option and the values are a package.** Anything that injects
  `ARB_fog_linear` must also own the fog state completely, for every area, for
  as long as the injection is live. Injecting the option and letting the engine
  supply values is not a lighter-touch version of the feature — it is the
  broken configuration, and it is what shipped in this test.
- **"Use the engine's authored fog" is off the table** until we know what the
  engine is actually passing. That is now the next experiment.
- **Fog stays off by default,** and this is the second time in one day it has
  had to be disarmed after visibly breaking a real playthrough. It has earned
  its marker file twice over.

## The next experiment, and why it is cheap

The `glFogf`/`glFogfv`/`glFogi` hooks already see every value the engine sets;
they just pass them through untouched when no override is active. Logging them
— on change only, the same discipline as everywhere else — answers the question
directly: what start, end, mode and colour does the engine hand the driver in
the western square, and how do those differ from an area that looked fine?

That is a few lines in `glhook.cpp`, no new addresses, no game-state writes, and
it turns "the fog values are probably wrong" into an actual pair of numbers.
Only after that does designing a mist preset make sense, because a preset is a
set of values layered on a baseline, and right now the baseline is unmeasured.

## Two incidents, two different mechanisms

Worth separating, because they looked identical on screen and were not:

| | Cause | Fix |
|---|---|---|
| 2026-08-30, dark brown | The test battery's `SetFogEnabled(nFog != 0)` silently became "on" when the marker was armed, applying a 20–120 brown override every heartbeat | Battery now forces `FALSE`; test 21 guards it |
| 2026-08-30, black | Shader injection alone, with **no** override — the engine's own fog values, finally honoured | Fog disarmed; measure the engine's values before going further |
