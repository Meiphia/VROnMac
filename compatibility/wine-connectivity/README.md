# Wine WinRT network cost compatibility fix

Based on Zagrios/voile-wine commit c73715a9189dcdbf406201ff3a15c00e1d41997b, dlls/windows.networking.connectivity. Sources retain Wine LGPL-2.1-or-later notices.

The original GetConnectionCost returns E_NOTIMPL. VRChat NativeProcess.dll GetNetworkCostType calls it and throws an unhandled C++ exception. This implementation returns IConnectionCost, uses NLM cost flags if available, and reports Unknown when the host has no billing information. It does not change the game or EAC.

Only the cost method is implemented beyond the original Wine behavior. Header compatibility casts adapt Wine COM wrappers to MinGW. Diagnostic macros are no-ops for the standalone build.

Compiled successfully with MinGW. The user ran the standalone runtime probe successfully on 2026-11-05: replacement identity verified, GetConnectionCost and all billing properties returned success (actual NLM cost type 1). For testing, native DLL loading must be forced with WINEDLLOVERRIDES=windows.networking.connectivity=n; otherwise Wine substitutes its original builtin even for an explicit path.

With VRChat closed, run `python3 scripts/install-network-cost-fix.py`. The installer verifies that this exact DLL hash passed a saved runtime probe, backs up the original DLL and user.reg, and sets an override scoped to VRChat.exe. It changes the Wine prefix, not game files or the shared Voile runtime. In-game verification remains pending.
