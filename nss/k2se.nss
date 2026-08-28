// k2se.nss -- the K2SE modder-facing include.
//
// Put this in your override folder (or next to nwnnsscomp.exe) and
// #include "k2se" in your script.
//
// -----------------------------------------------------------------------------
// TWO DIFFERENT THINGS LIVE IN THIS FILE
//
// 1. K2SE_Version() works with the STOCK nwscript.nss. It rides on abs(),
//    vanilla routine 77, so it costs no routine ID and is safe to call on a
//    machine with no K2SE installed. ALWAYS gate on it.
//
// 2. Everything below the marked line needs the EXTENDED nwscript.nss shipped
//    with K2SE, because those functions are appended past routine 876.
//    A script that calls them on a machine without K2SE will NOT degrade
//    gracefully -- the VM's out-of-range path returns -2002 and what it does
//    with that is still being characterised. Gate every single call.
// -----------------------------------------------------------------------------

const int K2SE_PROBE_MAGIC = -1234567890;

// Returns major*10000 + minor*100 + patch, or 0 when K2SE is not installed.
// Safe on a vanilla install; needs no extended header.
int K2SE_Version()
{
    int nResult = abs(K2SE_PROBE_MAGIC);
    if (nResult == 1234567890) return 0;   // plain abs() -> K2SE absent
    return nResult;
}

int K2SE_IsPresent()
{
    return K2SE_Version() != 0;
}

// =============================================================================
// EVERYTHING BELOW REQUIRES THE EXTENDED nwscript.nss
// =============================================================================

// Routine 877. Same value as K2SE_Version(), reached the direct way.
// Present so the round trip through a genuinely new routine ID can be tested.
int K2SE_GetVersion();

// Routines 878/879 -- DEVELOPMENT ONLY self-test plumbing. These exercise the
// multi-argument stack ABI end to end and may be renumbered before the first
// public release; do not build mods against them.
//
// K2SE_SelfTest returns an order-sensitive checksum:
//   nFirst*1000000 + FloatToInt(fSecond*10.0)*10000 + nThird
// K2SE_ReportTest compares and writes one PASS/FAIL line per nTestId into
// k2se.log; returns TRUE on match.
int K2SE_SelfTest(int nFirst, float fSecond, int nThird);
int K2SE_ReportTest(int nTestId, int nExpected, int nActual);

// Routine 880 -- DEVELOPMENT ONLY. Returns sIn unchanged.
//
// An identity function on purpose: it exercises the whole string path (pop,
// CExoString construction, push, and the engine's copy semantics in both
// directions) while keeping a failure unambiguous. If what comes back differs
// from what went in, the string ABI is wrong -- there is nothing else it could
// be. Strings are the first K2SE type whose lifetime the engine shares, so this
// gets its own test rather than riding along in K2SE_SelfTest.
string K2SE_EchoString(string sIn);

// =============================================================================
// Creature queries -- the first K2SE functions meant for real mods.
//
// All read-only, and all return -1 rather than failing the script when the
// creature cannot be resolved. ALWAYS check for -1: it means "K2SE could not
// answer", which is not the same as 0.
//
//     if (!K2SE_IsPresent()) return;
//     int nStr = K2SE_GetAbilityScoreBase(oPC, ABILITY_STRENGTH);
//     if (nStr < 0) return;    // could not resolve -- do not treat as a score
// =============================================================================

// The attribute the creature actually has, before items and effects.
// nAbility is ABILITY_STRENGTH .. ABILITY_CHARISMA (0..5).
int K2SE_GetAbilityScoreBase(object oCreature, int nAbility);

// The skill rank the creature bought, without item or effect modifiers --
// which vanilla GetSkillRank cannot report separately.
int K2SE_GetSkillRankBase(object oCreature, int nSkill);

// TRUE if the creature HAS the feat, as opposed to whether it can use it right
// now. Vanilla GetHasFeat folds in daily uses and so answers a different
// question.
int K2SE_GetFeatAcquired(object oCreature, int nFeat);

// TRUE if the creature knows the Force power, regardless of current Force points.
int K2SE_GetSpellAcquired(object oCreature, int nSpell);

// =============================================================================
// Runtime fog (M5).
//
// OFF BY DEFAULT. Fog support only installs when a file named `k2se_fog.txt`
// sits next to swkotor2.exe. Without it every call here is a silent no-op, so
// ALWAYS check K2SE_GetFogStatus() and tell the player if it is 0 -- otherwise
// your mod looks broken when it is merely switched off.
//
// Why the marker exists: this is the one part of K2SE that runs on the render
// thread, and it works by rewriting fragment programs on their way to the
// driver. It has not yet been confirmed in a live session, and it is not allowed
// to destabilise a DLL that otherwise works.
//
// Why it is global rather than per-area: the fix works by owning the fog values
// the engine hands to OpenGL, which is a per-frame, global thing. There is no
// per-area or per-room fog state to address, and fog density is never read by
// this engine -- only the linear start/end range is.
//
//     if (K2SE_GetFogStatus() == 0) return;   // fog support not installed
//     K2SE_SetFogRange(20.0, 120.0);
//     K2SE_SetFogColor(0.25, 0.13, 0.06);
//     K2SE_SetFogEnabled(TRUE);
// =============================================================================

// Status bit flags, also returned by the three setters:
//   0  fog support is off (no marker file)
//   1  hooks installed
//   2  at least one fragment program was made fog-aware
//   4  a script override is active
//   8  enabled but refused -- the GL imports did not look as expected
int K2SE_GetFogStatus();

// Turn the override on or off. With it off the engine's own fog values apply.
int K2SE_SetFogEnabled(int bEnable);

// Linear fog distances in world units. fStart is where fog begins, fEnd where
// it reaches full opacity.
int K2SE_SetFogRange(float fStart, float fEnd);

// Colour components, each 0.0 to 1.0.
int K2SE_SetFogColor(float fRed, float fGreen, float fBlue);

// -----------------------------------------------------------------------------
// Usage pattern -- copy this shape:
//
//   #include "k2se"
//
//   void main()
//   {
//       if (!K2SE_IsPresent()) return;    // degrade silently, never crash
//       // ... K2SE-only calls here ...
//   }
// -----------------------------------------------------------------------------
