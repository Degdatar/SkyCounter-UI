#pragma once

#include <RE/N/NiAVObject.h>
#include <RE/T/TESForm.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#if defined(SKYOS_SKYCOUNTERUI_EXPORTS)
#define SKYOS_SKYITEMICONS_API __declspec(dllexport)
#else
#define SKYOS_SKYITEMICONS_API __declspec(dllimport)
#endif

namespace SkyOS {
    using SkyCounterUIIconReadyCallback = void(*)(std::uint32_t formID, const std::string& iconUrl);
    enum class SkyCounterUICaptureStatus : std::uint8_t { Rendering, Ready, Failed };
    using SkyCounterUICaptureStatusCallback = void(*)(std::uint32_t formID, SkyCounterUICaptureStatus status);
    using SkyCounterUIFrameCaptureCallback = std::function<void(bool)>;

    // Installs automatic Inventory3DManager preview capture. Acquisitions
    // queue immediately; Skyrim renders the item with its inventory model,
    // materials, transforms, and lighting. GPU readback and PNG writing are
    // asynchronous and do not open a pausing menu.
    SKYOS_SKYITEMICONS_API void InstallSkyCounterUIIconRenderer(
        SkyCounterUIIconReadyCallback iconReadyCallback,
        SkyCounterUICaptureStatusCallback captureStatusCallback);

    // Enables or disables queued Inventory3DManager icon captures.
    SKYOS_SKYITEMICONS_API void SetSkyCounterUIIconCaptureEnabled(bool enabled);
    // Returns true when an icon PNG already exists and has a valid PNG header.
    SKYOS_SKYITEMICONS_API bool HasSkyCounterUIIconCache(const std::filesystem::path& iconPath);
    // Returns true only for icons written by the current renderer, so cached
    // files from older rendering passes can be refreshed on the next pickup.
    SKYOS_SKYITEMICONS_API bool IsSkyCounterUIIconCurrent(const std::filesystem::path& iconPath);
    SKYOS_SKYITEMICONS_API void QueueSkyCounterUIIconRender(
        std::uint32_t formID, std::filesystem::path outputPath,
        const RE::TESForm* form = nullptr,
        RE::NiPointer<RE::NiAVObject> sourceModel = {});
    SKYOS_SKYITEMICONS_API bool QueueSkyCounterUIFrameCapture(
        std::filesystem::path outputPath, SkyCounterUIFrameCaptureCallback completion);
    SKYOS_SKYITEMICONS_API void CancelSkyCounterUIFrameCaptures();
}
