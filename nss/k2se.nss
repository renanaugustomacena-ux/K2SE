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
