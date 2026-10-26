# SkyCounter UI API Header

This package exposes a small, versioned C ABI for native SKSE plugins that want to read SkyCounter UI's recent pickup records and location discovery/capture records as JSON snapshots. It does not expose renderer internals or grant control over the PrismaUI overlay.

## Header and runtime

- `include/SkyCounterUIAPI.h` — dynamic loader and API function table. Resolve `SkyCounterUI.dll` with `GetModuleHandleW` and call `SkyCounterUI_LoadAPI`; no import library or C++ STL types cross the ABI.
- `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUIAPIHeader/SkyDocuments/SkyCounter UI API Guide.html` — integration examples, message format, threading, and project ideas.
- Runtime dependency: SkyOS Core and SkyCounter UI installed and loaded in the same profile.

This standalone SkyCounter UI project packages the header and guide alongside the app runtime. The header remains an optional integration surface: native plugins can use it when SkyCounter UI is installed, and should continue to work when it is absent.

The API is read-only. It copies each recent item/location record as UTF-8 JSON into caller-owned memory. Callers should tolerate additional JSON fields and records changing between calls. For event-driven notifications, mod authors can also implement their own game-side listener; this API does not install event sinks on their behalf.
