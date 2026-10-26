#include "pch.h"
#include "skycounter_ui.h"
#include "skyitemicons.h"

#include <RE/B/BGSLocation.h>
#include <RE/E/ExtraMapMarker.h>
#include <RE/L/LocationDiscovery.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/ScriptEventSourceHolder.h>
#include <RE/T/TESActorLocationChangeEvent.h>
#include <RE/T/TESDataHandler.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
    namespace fs = std::filesystem;
    struct LocationRequest {
        std::string key;
        std::string plugin;
        std::string name;
        std::uint32_t formID{};
        std::uint32_t localFormID{};
        std::uint64_t saveGeneration{};
        fs::path targetPath;
    };

    SkyOS::SkyCounterUILocationCallback g_locationCallback{};
    std::atomic<bool> g_discoverySinkInstalled{};
    std::atomic<bool> g_locationChangeSinkInstalled{};
    std::atomic<bool> g_useSkyRewriteLocationDiscovery{};
    std::atomic<std::uint64_t> g_locationGeneration{};
    std::mutex g_locationMutex;
    std::set<std::string, std::less<>> g_discoveredLocations;
    std::set<std::string, std::less<>> g_pendingCaptures;
    std::deque<std::pair<std::string, std::string>> g_recentLocations;

    std::string JsonEscape(std::string_view input)
    {
        std::string output;
        output.reserve(input.size() + 8);
        for (const unsigned char ch : input) {
            switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: if (ch >= 0x20) output.push_back(static_cast<char>(ch));
            }
        }
        return output;
    }

    std::string Hex(std::uint32_t value)
    {
        std::array<char, 9> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%08X", value);
        return buffer.data();
    }

    std::string Lower(std::string_view value)
    {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        });
        return result;
    }

    std::string SafeSegment(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        bool separator = false;
        for (const unsigned char ch : value) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                result.push_back(static_cast<char>(ch));
                separator = false;
            } else if ((ch == ' ' || ch == '.') && !result.empty() && !separator) {
                result.push_back('_');
                separator = true;
            }
        }
        while (!result.empty() && result.back() == '_') result.pop_back();
        return result;
    }

    std::string SafePlugin(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const unsigned char ch : value) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
                result.push_back(static_cast<char>(ch));
            } else {
                result.push_back('_');
            }
        }
        while (!result.empty() && (result.back() == '.' || result.back() == '_')) result.pop_back();
        return result.empty() ? "UnknownPlugin" : result;
    }

    fs::path LocationRoot()
    {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) return {};
        return fs::path(std::wstring(executable.data(), length)).parent_path() / L"Data" / L"SKSE" /
            L"Plugins" / L"SkyOS" / L"Mods" / L"SkyCounterUI" / L"Locations";
    }

    fs::path CachePath(const std::string& plugin, std::uint32_t localFormID, const std::string& name)
    {
        auto root = LocationRoot();
        if (root.empty()) return {};
        if (Lower(plugin) != "skyrim.esm") root /= SafePlugin(plugin);
        auto safeName = SafeSegment(name);
        if (safeName.empty()) safeName = "Location";
        return root / (safeName + "_" + Hex(localFormID) + ".png");
    }

    fs::path ExistingCachePath(const std::string& plugin, std::uint32_t localFormID, const std::string& name)
    {
        auto candidate = CachePath(plugin, localFormID, name);
        if (candidate.empty()) return {};
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && !error) return candidate;
        for (const auto* extension : { L".bmp", L".jpg", L".jpeg" }) {
            candidate.replace_extension(extension);
            error.clear();
            if (fs::is_regular_file(candidate, error) && !error) return candidate;
        }
        return CachePath(plugin, localFormID, name);
    }

    std::string FileUrl(const fs::path& path)
    {
        auto wide = path.wstring();
        std::replace(wide.begin(), wide.end(), L'\\', L'/');
        if (wide.empty()) return {};
        if (wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
        const auto wideLength = static_cast<int>(wide.size());
        const auto utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) return {};
        std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(), utf8Length, nullptr, nullptr) != utf8Length) return {};
        std::string result = "file:///";
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char ch : utf8) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '/' || ch == ':' || ch == '-' ||
                ch == '_' || ch == '.' || ch == '~') {
                result.push_back(static_cast<char>(ch));
            } else {
                result.push_back('%');
                result.push_back(hex[ch >> 4]);
                result.push_back(hex[ch & 0x0F]);
            }
        }
        return result;
    }

    std::string Payload(const LocationRequest& request, std::string_view image, bool discoveredNow)
    {
        return std::string("{\"key\":\"") + JsonEscape(request.key) + "\",\"plugin\":\"" +
            JsonEscape(request.plugin) + "\",\"name\":\"" + JsonEscape(request.name) +
            "\",\"formID\":\"" + Hex(request.formID) + "\",\"localFormID\":\"" +
            Hex(request.localFormID) + "\",\"discoveredNow\":" + (discoveredNow ? "true" : "false") +
            ",\"image\":\"" + JsonEscape(image) + "\"}";
    }

    RE::BGSLocation* ResolveLocation(std::string_view markerName)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* currentLocation = player ? player->GetCurrentLocation() : nullptr;
        const auto wantedName = Lower(markerName);
        for (auto* candidate = currentLocation; candidate; candidate = candidate->parentLoc) {
            const auto* name = candidate->GetName();
            if (!wantedName.empty() && name && Lower(name) == wantedName) return candidate;
        }
        if (!wantedName.empty()) {
            if (auto* data = RE::TESDataHandler::GetSingleton()) {
                for (auto* candidate : data->GetFormArray<RE::BGSLocation>()) {
                    if (!candidate) continue;
                    const auto* name = candidate->GetName();
                    if (name && Lower(name) == wantedName) return candidate;
                }
            }
        }
        return currentLocation;
    }

    void FinishRequest(const std::string& key, std::uint64_t generation)
    {
        if (generation != g_locationGeneration.load(std::memory_order_acquire)) return;
        std::scoped_lock lock(g_locationMutex);
        g_pendingCaptures.erase(key);
    }

    void QueueCapture(const LocationRequest& request)
    {
        if (request.saveGeneration != g_locationGeneration.load(std::memory_order_acquire)) {
            SKSE::log::info("SkyCounterUI skipped location screenshot for '{}' because the active save changed", request.name);
            return;
        }
        if (!SkyOS::IsSkyCounterUILocationScreenshotCaptureEnabled()) {
            SKSE::log::info("SkyCounterUI skipped location screenshot for '{}' because location capture is disabled in its settings", request.name);
            return;
        }
        if (request.targetPath.empty()) {
            SKSE::log::warn("SkyCounterUI could not create a screenshot path for location '{}' ({})", request.name, request.key);
            return;
        }
        std::error_code error;
        const auto existingCache = ExistingCachePath(request.plugin, request.localFormID, request.name);
        if (fs::is_regular_file(existingCache, error) && !error) {
            SKSE::log::info("SkyCounterUI location screenshot already cached for '{}' at '{}'",
                request.name, existingCache.string());
            return;
        }
        {
            std::scoped_lock lock(g_locationMutex);
            if (!g_pendingCaptures.insert(request.key).second) {
                SKSE::log::info("SkyCounterUI already has a screenshot capture queued for '{}'", request.name);
                return;
            }
        }
        SKSE::log::info("SkyCounterUI queued a non-pausing game-view screenshot for '{}' at '{}'",
            request.name, request.targetPath.string());
        const auto queued = SkyOS::QueueSkyCounterUIFrameCapture(request.targetPath, [request](bool success) {
            if (!success) {
                SKSE::log::warn("SkyCounterUI location screenshot capture failed before publishing '{}'",
                    request.targetPath.string());
                FinishRequest(request.key, request.saveGeneration);
                return;
            }
            if (request.saveGeneration != g_locationGeneration.load(std::memory_order_acquire) ||
                !SkyOS::IsSkyCounterUILocationScreenshotCaptureEnabled()) {
                SKSE::log::info("SkyCounterUI discarded location screenshot for '{}' because save state changed or capture was disabled",
                    request.name);
                FinishRequest(request.key, request.saveGeneration);
                return;
            }
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                SKSE::log::warn("SkyCounterUI saved location screenshot for '{}' but SKSE task interface is unavailable", request.name);
                FinishRequest(request.key, request.saveGeneration);
                return;
            }
            const auto target = request.targetPath;
            const auto payload = Payload(request, FileUrl(target), false);
            tasks->AddTask([request, payload, path = target.string()] {
                if (request.saveGeneration != g_locationGeneration.load(std::memory_order_acquire)) return;
                {
                    std::scoped_lock lock(g_locationMutex);
                    const auto found = std::find_if(g_recentLocations.begin(), g_recentLocations.end(), [&](const auto& entry) {
                        return entry.first == request.key;
                    });
                    if (found != g_recentLocations.end()) found->second = payload;
                }
                SkyOS::UpdateSkyCounterUILocation(payload.c_str());
                if (g_locationCallback) g_locationCallback(payload, path, false);
                SKSE::log::info("SkyCounterUI cached first-visit game-frame capture for '{}'", request.name);
                FinishRequest(request.key, request.saveGeneration);
            });
        });
        if (!queued) {
            SKSE::log::warn("SkyCounterUI could not queue a game-frame capture for location '{}'", request.name);
            FinishRequest(request.key, request.saveGeneration);
        }
    }

    LocationRequest MakeRequest(RE::BGSLocation* location, std::string_view displayName)
    {
        LocationRequest request{};
        if (!location) return request;
        request.formID = location->GetFormID();
        request.localFormID = location->GetLocalFormID();
        request.saveGeneration = g_locationGeneration.load(std::memory_order_acquire);
        const auto* formName = location->GetName();
        request.name = !displayName.empty() ? std::string(displayName) :
            (formName && *formName ? formName : "Unknown location");
        if (const auto* file = location->GetFile(); file && !file->GetFilename().empty()) request.plugin = file->GetFilename();
        if (request.plugin.empty()) request.plugin = "Unknown plugin";
        if (!request.formID || !request.localFormID) return {};
        request.key = request.plugin + "|" + Hex(request.localFormID);
        request.targetPath = CachePath(request.plugin, request.localFormID, request.name);
        return request;
    }

    void PublishDiscovery(LocationRequest request)
    {
        if (request.saveGeneration != g_locationGeneration.load(std::memory_order_acquire)) return;
        auto cached = ExistingCachePath(request.plugin, request.localFormID, request.name);
        std::error_code error;
        const bool hasImage = !cached.empty() && fs::is_regular_file(cached, error) && !error;
        const std::string image = hasImage ? FileUrl(cached) : std::string{};
        const auto payload = Payload(request, image, true);
        {
            std::scoped_lock lock(g_locationMutex);
            g_recentLocations.emplace_front(request.key, payload);
            while (g_recentLocations.size() > 12) g_recentLocations.pop_back();
        }
        if (g_locationCallback) g_locationCallback(payload, hasImage ? cached.string() : std::string{}, true);
        const auto notice = std::string("{\"location\":") + payload + "}";
        SkyOS::ShowSkyCounterUINotice(notice.c_str());
        if (!hasImage) QueueCapture(request);
    }

    class DiscoverySink final : public RE::BSTEventSink<RE::LocationDiscovery::Event> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::LocationDiscovery::Event* event,
            RE::BSTEventSource<RE::LocationDiscovery::Event>*) override
        {
            if (g_useSkyRewriteLocationDiscovery.load(std::memory_order_acquire)) return RE::BSEventNotifyControl::kContinue;
            if (!event || !event->mapMarkerData) return RE::BSEventNotifyControl::kContinue;
            const auto markerType = event->mapMarkerData->type.get();
            if (markerType == RE::MARKER_TYPE::kNone || static_cast<std::uint32_t>(markerType) >=
                static_cast<std::uint32_t>(RE::MARKER_TYPE::kTotalLocationTypes)) return RE::BSEventNotifyControl::kContinue;
            const char* markerName = event->mapMarkerData->locationName.GetFullName();
            if (!markerName || !*markerName) return RE::BSEventNotifyControl::kContinue;
            auto request = MakeRequest(ResolveLocation(markerName), markerName);
            if (request.key.empty()) return RE::BSEventNotifyControl::kContinue;
            bool first = false;
            {
                std::scoped_lock lock(g_locationMutex);
                first = g_discoveredLocations.insert(request.key).second;
            }
            if (first) {
                if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([request] { PublishDiscovery(request); });
                else PublishDiscovery(request);
                SKSE::log::info("SkyCounterUI received map-marker discovery for '{}'", request.name);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class LocationChangeSink final : public RE::BSTEventSink<RE::TESActorLocationChangeEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* event,
            RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!event || !player || event->actor.get() != player || !event->newLoc) return RE::BSEventNotifyControl::kContinue;
            for (auto* location = event->newLoc; location; location = location->parentLoc) {
                auto request = MakeRequest(location, {});
                if (request.key.empty()) continue;
                bool discovered = false;
                {
                    std::scoped_lock lock(g_locationMutex);
                    discovered = g_discoveredLocations.contains(request.key);
                }
                if (discovered) { QueueCapture(request); break; }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    DiscoverySink g_discoverySink;
    LocationChangeSink g_locationChangeSink;
}

namespace SkyOS {
    void SetSkyCounterUILocationDiscoverySource(bool skyRewriteLoaded)
    {
        g_useSkyRewriteLocationDiscovery.store(skyRewriteLoaded, std::memory_order_release);
        SKSE::log::info("SkyCounterUI location discovery source: {}",
            skyRewriteLoaded ? "SkyRewrite discovery messages" : "Skyrim map-marker fallback");
    }

    void InstallSkyCounterUILocationEventSink(SkyCounterUILocationCallback callback)
    {
        g_locationCallback = callback;
        for (const auto& payload : SnapshotSkyCounterUILocationHistory()) {
            if (g_locationCallback) g_locationCallback(payload, {}, false);
        }
        if (!g_discoverySinkInstalled.exchange(true, std::memory_order_acq_rel)) {
            if (auto* source = RE::LocationDiscovery::GetEventSource()) {
                source->AddEventSink(&g_discoverySink);
                SKSE::log::info("SkyCounterUI subscribed to map-marker discoveries without requiring SkyCollection");
            } else {
                g_discoverySinkInstalled.store(false, std::memory_order_release);
                SKSE::log::error("SkyCounterUI could not access Skyrim's map-marker discovery source");
            }
        }
        if (!g_locationChangeSinkInstalled.exchange(true, std::memory_order_acq_rel)) {
            if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
                source->AddEventSink<RE::TESActorLocationChangeEvent>(&g_locationChangeSink);
            } else {
                g_locationChangeSinkInstalled.store(false, std::memory_order_release);
                SKSE::log::error("SkyCounterUI could not subscribe to player location changes for screenshot retries");
            }
        }
    }

    void RequestSkyCounterUILocationScreenshot(
        const std::string& plugin, std::uint32_t formID, std::uint32_t localFormID, const std::string& name)
    {
        if (name.empty()) {
            SKSE::log::warn("SkyCounterUI rejected location screenshot request: plugin='{}', formID={:08X}, localFormID={:08X}, name='{}'",
                plugin, formID, localFormID, name);
            return;
        }
        LocationRequest request{};
        request.plugin = plugin.empty() ? "Unknown plugin" : plugin;
        request.formID = formID;
        request.localFormID = localFormID;
        request.saveGeneration = g_locationGeneration.load(std::memory_order_acquire);
        request.name = name;
        request.key = request.plugin + "|" + Hex(localFormID);
        if (!formID || !localFormID) request.key += "|" + Lower(name);
        request.targetPath = CachePath(request.plugin, localFormID, name);
        SKSE::log::info("SkyCounterUI preparing SkyRewrite discovery screenshot: name='{}', formID={:08X}, localFormID={:08X}, plugin='{}', key='{}', path='{}'",
            request.name, request.formID, request.localFormID, request.plugin, request.key, request.targetPath.string());
        bool firstDiscovery = false;
        {
            std::scoped_lock lock(g_locationMutex);
            firstDiscovery = g_discoveredLocations.insert(request.key).second;
        }
        if (firstDiscovery) {
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([request] { PublishDiscovery(request); });
            else PublishDiscovery(request);
        } else {
            QueueCapture(request);
        }
    }

    std::vector<std::string> SnapshotSkyCounterUILocationHistory()
    {
        std::scoped_lock lock(g_locationMutex);
        std::vector<std::string> result;
        result.reserve(g_recentLocations.size());
        for (auto it = g_recentLocations.rbegin(); it != g_recentLocations.rend(); ++it) result.push_back(it->second);
        return result;
    }

    std::vector<std::string> SnapshotSkyCounterUILocationKeys()
    {
        std::scoped_lock lock(g_locationMutex);
        return std::vector<std::string>(g_discoveredLocations.begin(), g_discoveredLocations.end());
    }

    void RestoreSkyCounterUILocationHistory(
        const std::vector<std::string>& history, const std::vector<std::string>& discoveredKeys)
    {
        {
            std::scoped_lock lock(g_locationMutex);
            g_discoveredLocations.clear();
            g_pendingCaptures.clear();
            g_recentLocations.clear();
            for (const auto& key : discoveredKeys) {
                if (!key.empty() && key.size() <= 512) g_discoveredLocations.insert(key);
            }
            for (const auto& payload : history) {
                if (g_recentLocations.size() >= 12) break;
                if (payload.empty() || payload.size() > 16384) continue;
                const auto marker = payload.find("\"key\":\"");
                if (marker == std::string::npos) continue;
                const auto start = marker + 7;
                const auto end = payload.find('"', start);
                if (end == std::string::npos) continue;
                const auto key = payload.substr(start, end - start);
                g_recentLocations.emplace_front(key, payload);
                g_discoveredLocations.insert(key);
            }
        }
        if (g_locationCallback) {
            for (const auto& payload : history) {
                if (!payload.empty() && payload.size() <= 16384) g_locationCallback(payload, {}, false);
            }
        }
    }

    void ResetSkyCounterUILocationSaveState()
    {
        ++g_locationGeneration;
        {
            std::scoped_lock lock(g_locationMutex);
            g_discoveredLocations.clear();
            g_pendingCaptures.clear();
            g_recentLocations.clear();
        }
        SkyOS::CancelSkyCounterUIFrameCaptures();
        if (g_locationCallback) g_locationCallback("{\"clearStream\":true}", {}, false);
    }
}

extern "C" __declspec(dllexport) void SkyCounterUI_RequestLocationScreenshot(
    const char* plugin, std::uint32_t formID, std::uint32_t localFormID, const char* name)
{
    if (!plugin || !name) return;
    SkyOS::RequestSkyCounterUILocationScreenshot(plugin, formID, localFormID, name);
}
