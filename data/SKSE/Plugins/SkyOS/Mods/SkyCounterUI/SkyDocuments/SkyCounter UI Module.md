# SkyCounter UI

## Features

- Listens for player inventory pickups and displays configurable notices even when SkyCollection is not installed.
- Listens for map-marker discoveries independently of SkyCollection, displays location notices, and captures first-visit screenshots.
- Supports multiple screen positions, themes, sizes, detail toggles, and display durations.
- Notice display time can be set to 500, 1000, 1500, 2000, 2500, or 3000 ms.
- Owns the Skyrim item-icon renderer and cache used by both pickup notices and SkyCollection.
- Owns all item-icon rendering and capture controls. SkyCollection only reads the Counter UI icon cache and displays those saved images; it cannot start rendering.
- Maintains a recent pickup stream and location-capture list in the SkyOS app page; SkyCollection can read the shared location screenshot cache for its save-backed catalog.

## Changelog

- **Standalone app module:** counter overlay creation, options, and notice rendering ship in `SkyCounterUI.dll`, with its own settings module, view files, and guide.
- **Rendering ownership:** item icon rendering and first-visit location screenshot capture are controlled through the SkyCounter UI module.
- **Standalone pickup events:** pickup detection and notices work from SkyCounter UI alone. SkyCollection remains an optional source of save-specific catalog and location integration.
- **Standalone location events:** location notices and first-visit screenshot capture now work with SkyCounter UI alone; screenshots are cached under `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Locations/`.
- **Branding:** added the supplied SkyCounter UI logo and banner to the SkyViewer page, SkyDocuments, and SkyOS app screens.

## Requirements

- Skyrim Special Edition / Anniversary Edition Latest Version (supported runtime).
- SKSE Latest Version.
- PrismaUI Latest Version.
- SkyOS Latest Version.

SkyCollection Latest Version is optional. When installed, it records location and item discoveries in the active save and displays the shared caches.

## Files and saved data

- Overlay page: `Data/PrismaUI/views/SkyOS/SkyCounter/`.
- Module page and settings schema: `Data/SKSE/Plugins/SkyOS/Mods/SkyViewer/SkyCounterUI/`.
- SkyDocuments guide: `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/SkyDocuments/`.
- Per-user module settings: `%LOCALAPPDATA%/SkyOS/SkyViewer/SkyCounterUI/settings.json`.
- Item-icon renderer and cache: `SkyCounterUI.dll` and `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Icons/`.
- Location capture and cache: `SkyCounterUI.dll` and `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Locations/`.
- Notices are temporary. Collection discoveries remain in the active save's SKSE co-save.

