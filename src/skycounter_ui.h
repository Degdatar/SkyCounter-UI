#pragma once

#include "PrismaUI_API.h"
#include <cstdint>
#include <string>
#include <vector>

#if defined(SKYOS_SKYCOUNTERUI_EXPORTS)
#define SKYOS_SKYCOUNTERUI_API __declspec(dllexport)
#else
#define SKYOS_SKYCOUNTERUI_API __declspec(dllimport)
#endif

namespace SkyOS {
    using SkyCounterUIPickupCallback = void(*)(const std::string& payload);
    using SkyCounterUILocationCallback = void(*)(const std::string& payload, const std::string& imagePath, bool discoveredNow);

    [[nodiscard]] SKYOS_SKYCOUNTERUI_API bool InitializeSkyCounterUI(
        PRISMA_UI_API::IVPrismaUI1* prisma, PrismaView mainView);
    SKYOS_SKYCOUNTERUI_API void SetSkyCounterUISettings(const char* json);
    [[nodiscard]] SKYOS_SKYCOUNTERUI_API bool IsSkyCounterUILocationScreenshotCaptureEnabled();
    SKYOS_SKYCOUNTERUI_API void InstallSkyCounterUIPickupEventSink(SkyCounterUIPickupCallback callback);
    SKYOS_SKYCOUNTERUI_API void InstallSkyCounterUILocationEventSink(SkyCounterUILocationCallback callback);
    SKYOS_SKYCOUNTERUI_API void SetSkyCounterUILocationDiscoverySource(bool skyRewriteLoaded);
    SKYOS_SKYCOUNTERUI_API void RequestSkyCounterUILocationScreenshot(
        const std::string& plugin, std::uint32_t formID, std::uint32_t localFormID, const std::string& name);
    SKYOS_SKYCOUNTERUI_API void ShowSkyCounterUINotice(const char* json);
    SKYOS_SKYCOUNTERUI_API void UpdateSkyCounterUIIcon(const char* json);
    SKYOS_SKYCOUNTERUI_API void UpdateSkyCounterUILocation(const char* json);
    SKYOS_SKYCOUNTERUI_API void ClearSkyCounterUIRecentStream();
    [[nodiscard]] SKYOS_SKYCOUNTERUI_API std::vector<std::string> SnapshotSkyCounterUIPickupHistory();
    SKYOS_SKYCOUNTERUI_API void RestoreSkyCounterUIPickupHistory(const std::vector<std::string>& history);
    SKYOS_SKYCOUNTERUI_API std::vector<std::string> SnapshotSkyCounterUILocationHistory();
    [[nodiscard]] SKYOS_SKYCOUNTERUI_API std::vector<std::string> SnapshotSkyCounterUILocationKeys();
    SKYOS_SKYCOUNTERUI_API void RestoreSkyCounterUILocationHistory(
        const std::vector<std::string>& history, const std::vector<std::string>& discoveredKeys);
    SKYOS_SKYCOUNTERUI_API void ResetSkyCounterUILocationSaveState();
}
