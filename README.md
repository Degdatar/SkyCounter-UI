# SkyCounter UI

SkyCounter UI is the native SkyOS module for pickup notices, discovered-location notices, item-icon rendering, location screenshot capture, and recent pickup/location history. It also exposes a small read-only C ABI so other native plugins can query recent records.

## Features

- Detect and display player inventory pickups, with configurable notice layout, theme, position, size, duration, and detail fields.
- Detect map-marker discoveries and capture a screenshot on first discovery.
- Render and cache item icons for SkyCounter UI and SkyCollection.
- Keep recent pickup and location-capture streams available to the in-game dashboard and optional native integrations.
- Provide a versioned, read-only C ABI that copies pickup and location snapshots as UTF-8 JSON.
- Continue pickup and location notices without SkyCollection. SkyCollection is an optional consumer of the shared icon and screenshot caches.

## Requirements

- Windows x64 and Visual Studio 2022 with the C++ desktop workload.
- CMake 3.24 or newer.
- Git, for fetching CommonLibSSE-NG.
- vcpkg with `VCPKG_ROOT` set and the `x64-windows-static-md` triplet.
- Skyrim Anniversary Edition, SKSE, PrismaUI, and the matching SkyOS core at runtime.
- SkyRewrite.

## Build and package

Configure and build the Release preset from this directory:

```powershell
cmake --preset release
cmake --build --preset release --parallel
```

Create a 7-Zip archive from CPack's install manifest:

```powershell
cpack --config build/CPackConfig.cmake -G 7Z -C Release
```

The package installs the plugin and its runtime resources below Skyrim's `Data` directory. The CPack command above requires a CMake build with the 7Z generator available.

## SkyOS integration

`SkyOS.dll` owns the shared shell and loads `SkyCounterUI.dll` as an app module. The module registers its SKSE serialization and messaging callbacks, provides native pickup/location event sinks and cache APIs to the core, and hosts its dashboard in the SkyOS PrismaUI view tree. The included viewer module settings configure notices and capture/rendering behavior.

SkyCounter UI does not replace SkyOS Core, SKSE, or PrismaUI. Install those runtime dependencies in the same Skyrim profile. SkyCollection can be installed separately when save-backed catalog browsing is desired.

## Public API

The public header is at `data/SKSE/Plugins/SkyCounterUIAPIHeader/include/SkyCounterUIAPI.h`. It resolves exported functions from an already-loaded `SkyCounterUI.dll` with `GetProcAddress`; there is no import library. The API is read-only and provides current recent pickup and location JSON snapshots, not a durable lifetime archive or renderer controls.

The detailed, branded API guide is under `data/SKSE/Plugins/SkyOS/Mods/SkyCounterUIAPIHeader/SkyDocuments/`. The shipped user guide is under `data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/SkyDocuments/`.

## Repository layout

```text
src/                                                    Native SKSE plugin, renderer, capture, and API implementation
data/PrismaUI/views/SkyOS/SkyCounter/                   Dashboard hosted by the SkyOS shell
data/PrismaUI/views/SkyCounterUI/                       App-owned logo and banner
data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/              App manifest and SkyDocuments guide
data/SKSE/Plugins/SkyOS/Mods/SkyViewer/SkyCounterUI/    Viewer settings module and its page assets
data/SKSE/Plugins/SkyCounterUIAPIHeader/                Public C ABI header and README
data/SKSE/Plugins/SkyOS/Mods/SkyCounterUIAPIHeader/     Branded API SkyDocuments guide
```

The module owns item-icon and location-screenshot caches at `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Icons/` and `.../Locations/`. These are runtime-generated and are not included as source assets in this repository.
