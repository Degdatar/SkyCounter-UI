#include "skycounter_ui.h"
#include "skyitemicons.h"
#include "SkyCounterUIAPI.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/S/ScriptEventSourceHolder.h>
#include <RE/N/NiAVObject.h>
#include <RE/T/TESContainerChangedEvent.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace {
    PRISMA_UI_API::IVPrismaUI1* g_prisma{};
    PrismaView g_view{};
    bool g_ready{};
    std::atomic<bool> g_enabled{ true };
    std::atomic<bool> g_optionsLoaded{ true };
    std::atomic<bool> g_captureLocationScreenshots{ true };
    std::atomic<bool> g_pickupSinkInstalled{};
    std::atomic<bool> g_firstNoticeDispatchLogged{};
    std::mutex g_pendingMutex;
    std::mutex g_pickupQueueMutex;
    std::mutex g_pickupHistoryMutex;
    std::atomic<std::uint64_t> g_saveGeneration{};
    std::deque<std::string> g_pendingNotices;
    struct PendingPickup {
        RE::FormID formID{};
        std::uint32_t count{};
        std::uint64_t saveGeneration{};
        RE::NiPointer<RE::NiAVObject> sourceModel;
    };
    std::deque<PendingPickup> g_pendingPickups;
    std::deque<std::string> g_recentPickups;
    bool g_pickupDrainScheduled{};
    SkyOS::SkyCounterUIPickupCallback g_pickupCallback{};
    std::string g_settings = "{\"enabled\":true,\"renderIcons\":true,\"captureLocationScreenshots\":true,\"messageDesign\":\"relic-card\",\"theme\":\"dwemer\",\"position\":\"top-center\",\"size\":\"standard\",\"durationMs\":1500,\"showItemCount\":true,\"showPluginName\":true,\"showCategory\":true}";

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

    std::string HexFormID(std::uint32_t value)
    {
        std::array<char, 9> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%08X", value);
        return buffer.data();
    }

    bool IsGoldForm(const RE::TESForm* form)
    {
        return form && (form->IsGold() || form->GetFormID() == 0x0000000F);
    }

    bool IsGoldPickupRecord(std::string_view record)
    {
        const bool currency = record.find("\"category\":\"Currency\"") != std::string_view::npos;
        const bool goldName = record.find("\"name\":\"Gold\"") != std::string_view::npos ||
            record.find("\"name\":\"Septim\"") != std::string_view::npos ||
            record.find("\"name\":\"Septims\"") != std::string_view::npos;
        return (currency && goldName) || record.find("\"formID\":\"0000000F\"") != std::string_view::npos;
    }


    std::filesystem::path IconPathFor(const RE::TESForm* form)
    {
        if (!form) return {};

        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) return {};

        std::string plugin;
        if (const auto* file = form->GetFile(); file && !file->GetFilename().empty()) plugin = file->GetFilename();
        std::string safePlugin;
        safePlugin.reserve(plugin.size());
        for (const unsigned char ch : plugin) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                safePlugin.push_back(static_cast<char>(ch));
            } else if (ch == '.') {
                safePlugin.push_back('_');
            }
        }
        if (safePlugin.empty()) safePlugin = "UnknownPlugin";

        const std::filesystem::path gamePath(std::wstring(executable.data(), length));
        return gamePath.parent_path() / L"Data" / L"SKSE" / L"Plugins" / L"SkyOS" / L"Mods" /
            L"SkyCounterUI" / L"Icons" / safePlugin / (HexFormID(form->GetLocalFormID()) + ".png");
    }

    std::filesystem::path PreviousPerUserIconPathFor(const RE::TESForm* form)
    {
        if (!form) return {};

        std::wstring localAppData(32768, L'\0');
        const auto localAppDataLength = GetEnvironmentVariableW(
            L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
        if (!localAppDataLength || localAppDataLength >= localAppData.size()) return {};
        localAppData.resize(localAppDataLength);

        std::string plugin;
        if (const auto* file = form->GetFile(); file && !file->GetFilename().empty()) plugin = file->GetFilename();
        std::string safePlugin;
        safePlugin.reserve(plugin.size());
        for (const unsigned char ch : plugin) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                safePlugin.push_back(static_cast<char>(ch));
            } else if (ch == '.') {
                safePlugin.push_back('_');
            }
        }
        if (safePlugin.empty()) safePlugin = "UnknownPlugin";

        return std::filesystem::path(localAppData) / L"SkyOS" / L"SkyCounterUI" / L"Icons" /
            safePlugin / (HexFormID(form->GetLocalFormID()) + ".png");
    }

    std::filesystem::path ExistingIconPathFor(const RE::TESForm* form)
    {
        const auto current = IconPathFor(form);
        if (SkyOS::HasSkyCounterUIIconCache(current)) return current;
        const auto legacy = PreviousPerUserIconPathFor(form);
        return SkyOS::HasSkyCounterUIIconCache(legacy) ? legacy : current;
    }

    std::string FileUrl(const std::filesystem::path& path)
    {
        auto wide = path.wstring();
        std::replace(wide.begin(), wide.end(), L'\\', L'/');
        if (wide.empty() || wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
        const auto wideLength = static_cast<int>(wide.size());
        const auto byteLength = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
        if (byteLength <= 0) return {};
        std::string utf8(static_cast<std::size_t>(byteLength), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(), byteLength, nullptr, nullptr) != byteLength) return {};
        std::string result = "file:///";
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char ch : utf8) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '/' || ch == ':' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                result.push_back(static_cast<char>(ch));
            } else {
                result.push_back('%');
                result.push_back(hex[ch >> 4]);
                result.push_back(hex[ch & 0x0F]);
            }
        }
        result += "?v=2";
        std::error_code stampError;
        const auto writeTime = std::filesystem::last_write_time(path, stampError);
        if (!stampError) result += "-" + std::to_string(writeTime.time_since_epoch().count());
        return result;
    }

    std::string RestoreCachedIcon(std::string notice)
    {
        constexpr std::string_view kFormIDPrefix = "\"formID\":\"";
        const auto formIDField = notice.find(kFormIDPrefix);
        if (formIDField == std::string::npos) {
            SKSE::log::warn("SkyCounterUI history icon restore could not parse record: missing formID field");
            return notice;
        }
        const auto formIDStart = formIDField + kFormIDPrefix.size();
        if (formIDStart + 8 > notice.size()) {
            SKSE::log::warn("SkyCounterUI history icon restore could not parse record: truncated formID field");
            return notice;
        }

        std::uint32_t formID = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            const auto ch = static_cast<unsigned char>(notice[formIDStart + i]);
            std::uint32_t digit{};
            if (ch >= '0' && ch <= '9') digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
            else {
                SKSE::log::warn("SkyCounterUI history icon restore could not parse record: invalid formID digit at offset {}",
                    i);
                return notice;
            }
            formID = (formID << 4) | digit;
        }

        const auto* form = RE::TESForm::LookupByID(formID);
        if (!form || form->IsDeleted() || form->IsDestroyed()) {
            SKSE::log::warn("SkyCounterUI history icon restore could not resolve formID={:08X}: formFound={}, deleted={}, destroyed={}",
                formID, form != nullptr, form && form->IsDeleted(), form && form->IsDestroyed());
            return notice;
        }
        std::string pluginName = "Unknown plugin";
        if (const auto* file = form->GetFile(); file && !file->GetFilename().empty()) {
            pluginName = file->GetFilename();
        }
        const auto iconPath = ExistingIconPathFor(form);
        if (!SkyOS::HasSkyCounterUIIconCache(iconPath)) {
            SKSE::log::info("SkyCounterUI history item has no cached PNG: formID={:08X}, name='{}', plugin='{}', expectedPath='{}'",
                formID, form->GetName() ? form->GetName() : "Unknown item",
                pluginName, iconPath.string());
            return notice;
        }
        const auto iconUrl = JsonEscape(FileUrl(iconPath));
        if (iconUrl.empty()) {
            SKSE::log::warn("SkyCounterUI found cached PNG but could not create file URL: formID={:08X}, path='{}'",
                formID, iconPath.string());
            return notice;
        }

        constexpr std::string_view kIconPrefix = "\"icon\":\"";
        const auto iconField = notice.find(kIconPrefix);
        if (iconField == std::string::npos) {
            SKSE::log::warn("SkyCounterUI history record has no icon field: formID={:08X}", formID);
            return notice;
        }
        const auto valueStart = iconField + kIconPrefix.size();
        const auto valueEnd = notice.find('"', valueStart);
        if (valueEnd == std::string::npos) {
            SKSE::log::warn("SkyCounterUI history record has malformed icon field: formID={:08X}", formID);
            return notice;
        }
        notice.replace(valueStart, valueEnd - valueStart, iconUrl);
        SKSE::log::info("SkyCounterUI attached cached icon to history item: formID={:08X}, name='{}', plugin='{}', path='{}'",
            formID, form->GetName() ? form->GetName() : "Unknown item",
            pluginName, iconPath.string());
        return notice;
    }

    void SchedulePickupDrain();

    void DrainOnePickup()
    {
        PendingPickup pending{};
        {
            std::scoped_lock lock(g_pickupQueueMutex);
            if (g_pendingPickups.empty()) {
                g_pickupDrainScheduled = false;
                return;
            }
            pending = g_pendingPickups.front();
            g_pendingPickups.pop_front();
        }

        auto* form = RE::TESForm::LookupByID(pending.formID);
        const bool generationMatches =
            pending.saveGeneration == g_saveGeneration.load(std::memory_order_acquire);
        if (generationMatches && form && IsGoldForm(form)) {
            SKSE::log::info("SkyCounterUI excluded gold pickup: formID={:08X}, name='{}', count={}",
                form->GetFormID(), form->GetName() ? form->GetName() : "Gold", pending.count);
        } else
        if (generationMatches && form && !form->IsDeleted() && !form->IsDestroyed()) {
            const auto* rawName = form->GetName();
            const std::string name = JsonEscape(rawName && *rawName ? rawName : "Unknown item");
            const auto category = IsGoldForm(form) ? "Currency" : "Item";
            std::string plugin;
            if (const auto* file = form->GetFile(); file && !file->GetFilename().empty()) plugin = file->GetFilename();
            if (plugin.empty()) plugin = "Unknown plugin";
            const auto iconPath = IconPathFor(form);
            const auto existingIconPath = ExistingIconPathFor(form);
            // Keep using an existing PNG while a newer capture is attempted.
            // A failed recapture must never turn a previously visible icon into
            // the empty placeholder in the pickup stream.
            const bool iconCached = SkyOS::HasSkyCounterUIIconCache(existingIconPath);
            const bool iconCurrent = iconCached && SkyOS::IsSkyCounterUIIconCurrent(existingIconPath);
            const auto icon = iconCached ? FileUrl(existingIconPath) : std::string{};

            SKSE::log::info("SkyCounterUI pickup resolved: formID={:08X}, name='{}', plugin='{}', formType={}, count={}, sourceModel={}, cachePng={}, cacheCurrent={}, capturePath='{}'",
                form->GetFormID(), rawName && *rawName ? rawName : "Unknown item", plugin,
                static_cast<std::uint32_t>(form->GetFormType()), pending.count,
                static_cast<bool>(pending.sourceModel), iconCached, iconCurrent, iconPath.string());

            const auto notice = std::string("{\"item\":{\"plugin\":\"") + JsonEscape(plugin) +
                "\",\"name\":\"" + name + "\",\"category\":\"" + category + "\",\"formID\":\"" +
                HexFormID(form->GetFormID()) + "\",\"localFormID\":\"" + HexFormID(form->GetLocalFormID()) +
                "\",\"lastPickup\":" + std::to_string(pending.count) +
                ",\"discoveredNow\":false,\"icon\":\"" + JsonEscape(icon) + "\"}}";
            {
                std::scoped_lock lock(g_pickupHistoryMutex);
                g_recentPickups.push_front(notice);
                while (g_recentPickups.size() > 12) g_recentPickups.pop_back();
            }
            SkyOS::ShowSkyCounterUINotice(notice.c_str());
            if (g_pickupCallback) g_pickupCallback(notice);
            SkyOS::QueueSkyCounterUIIconRender(form->GetFormID(), iconPath, form, std::move(pending.sourceModel));
        } else {
            SKSE::log::warn("SkyCounterUI discarded queued pickup: formID={:08X}, count={}, saveGenerationMatches={}, formFound={}, formDeleted={}, formDestroyed={}",
                pending.formID, pending.count, generationMatches, form != nullptr,
                form && form->IsDeleted(), form && form->IsDestroyed());
        }

        bool more = false;
        {
            std::scoped_lock lock(g_pickupQueueMutex);
            more = !g_pendingPickups.empty();
            if (!more) g_pickupDrainScheduled = false;
        }
        if (more) SchedulePickupDrain();
    }

    void SchedulePickupDrain()
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            std::size_t discarded{};
            {
                std::scoped_lock lock(g_pickupQueueMutex);
                g_pickupDrainScheduled = false;
                discarded = g_pendingPickups.size();
                g_pendingPickups.clear();
            }
            SKSE::log::error("SkyCounterUI cannot drain pickup queue: SKSE TaskInterface is unavailable; discarded {} pickup event(s)",
                discarded);
            return;
        }
        tasks->AddTask([] { DrainOnePickup(); });
    }

    class SkyCounterUIPickupEventSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!event || event->itemCount <= 0 || !event->baseObj) return RE::BSEventNotifyControl::kContinue;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || event->newContainer != player->GetFormID()) return RE::BSEventNotifyControl::kContinue;
            if (auto* item = RE::TESForm::LookupByID(event->baseObj); item && IsGoldForm(item)) {
                SKSE::log::info("SkyCounterUI ignored player gold pickup event: formID={:08X}, itemCount={}",
                    event->baseObj, event->itemCount);
                return RE::BSEventNotifyControl::kContinue;
            }

            bool schedule = false;
            RE::NiPointer<RE::NiAVObject> sourceModel;
            RE::FormID sourceReferenceID{};
            if (auto sourceReference = event->reference.get(); sourceReference &&
                sourceReference->GetBaseObject() && sourceReference->GetBaseObject()->GetFormID() == event->baseObj) {
                sourceReferenceID = sourceReference->GetFormID();
                if (auto* model = sourceReference->Get3D()) sourceModel.reset(model);
            }
            const bool sourceModelAvailable = static_cast<bool>(sourceModel);
            bool accepted = false;
            std::size_t pendingCount{};
            {
                std::scoped_lock lock(g_pickupQueueMutex);
                if (g_pendingPickups.size() < 256) {
                    g_pendingPickups.push_back({ event->baseObj, static_cast<std::uint32_t>(event->itemCount),
                        g_saveGeneration.load(std::memory_order_acquire), std::move(sourceModel) });
                    accepted = true;
                }
                pendingCount = g_pendingPickups.size();
                if (!g_pickupDrainScheduled && !g_pendingPickups.empty()) {
                    g_pickupDrainScheduled = true;
                    schedule = true;
                }
            }
            if (accepted) {
                SKSE::log::info("SkyCounterUI player pickup event: formID={:08X}, itemCount={}, sourceReference={:08X}, liveSourceModel={}, pendingQueueSize={}",
                    event->baseObj, event->itemCount, sourceReferenceID,
                    sourceModelAvailable, pendingCount);
            } else {
                // Keep queue overflow visible; otherwise the pickup notice
                // can be mistaken for an icon-renderer failure.
                SKSE::log::warn("SkyCounterUI dropped pickup event {:08X}: pending pickup queue is full (256)",
                    event->baseObj);
            }
            if (schedule) SchedulePickupDrain();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    SkyCounterUIPickupEventSink g_pickupEventSink;


    bool JsonBooleanOption(std::string_view json, std::string_view key, bool fallback)
    {
        const auto keyToken = "\"" + std::string(key) + "\"";
        const auto keyPosition = json.find(keyToken);
        if (keyPosition == std::string_view::npos) return fallback;
        const auto colon = json.find(':', keyPosition + keyToken.size());
        if (colon == std::string_view::npos) return fallback;
        const auto value = json.find_first_not_of(" \t\r\n", colon + 1);
        if (value == std::string_view::npos) return fallback;
        if (json.compare(value, 4, "true") == 0) return true;
        if (json.compare(value, 5, "false") == 0) return false;
        return fallback;
    }

    void OnDomReady(PrismaView view)
    {
        g_view = view;
        g_ready = true;
        g_prisma->Hide(view);
        std::deque<std::string> pending;
        {
            std::scoped_lock lock(g_pendingMutex);
            if (g_optionsLoaded.load(std::memory_order_acquire)) pending.swap(g_pendingNotices);
        }
        g_prisma->Invoke(view, ("SkyCounter.setSettings(" + g_settings + ")").c_str());
        if (g_optionsLoaded.load(std::memory_order_acquire) && g_enabled.load(std::memory_order_acquire) && !pending.empty()) {
            g_prisma->Show(view);
            for (const auto& notice : pending) {
                g_prisma->Invoke(view, ("SkyCounter.showNotice(" + notice + ")").c_str());
            }
        }
    }

    void OnHide(const char*)
    {
        if (g_prisma && g_view && g_prisma->IsValid(g_view)) g_prisma->Hide(g_view);
    }

    void InvokeNotice(const std::string& json)
    {
        if (!g_optionsLoaded.load(std::memory_order_acquire)) {
            std::scoped_lock lock(g_pendingMutex);
            if (g_pendingNotices.size() == 256) g_pendingNotices.pop_front();
            g_pendingNotices.push_back(json);
            return;
        }
        if (!g_enabled.load(std::memory_order_acquire)) return;
        if (g_prisma && g_ready && g_view && g_prisma->IsValid(g_view)) {
            if (!g_firstNoticeDispatchLogged.exchange(true, std::memory_order_acq_rel)) {
                SKSE::log::info("SkyCounterUI dispatched its first notice to the PrismaUI overlay");
            }
            g_prisma->Show(g_view);
            g_prisma->Invoke(g_view, ("SkyCounter.showNotice(" + json + ")").c_str());
        } else {
            std::scoped_lock lock(g_pendingMutex);
            if (g_pendingNotices.size() == 256) g_pendingNotices.pop_front();
            g_pendingNotices.push_back(json);
        }
    }
}

namespace {
    std::uint32_t CopySkyCounterUIJsonRecord(
        const std::vector<std::string>& records, std::uint32_t index, char* buffer, std::uint32_t capacity)
    {
        if (index >= records.size()) return 0;
        const auto& record = records[index];
        if (record.size() >= (std::numeric_limits<std::uint32_t>::max)()) return 0;
        const auto required = static_cast<std::uint32_t>(record.size() + 1);
        if (buffer && capacity) {
            if (capacity >= required) std::memcpy(buffer, record.c_str(), required);
            else buffer[0] = '\0';
        }
        return required;
    }
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SkyCounterUI_GetAPIVersion()
{
    return SKYCOUNTERUI_API_VERSION;
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SkyCounterUI_GetRecentPickupCount()
{
    try { return static_cast<std::uint32_t>(SkyOS::SnapshotSkyCounterUIPickupHistory().size()); }
    catch (...) { return 0; }
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SkyCounterUI_CopyRecentPickupJsonAt(
    std::uint32_t index, char* buffer, std::uint32_t capacity)
{
    try { return CopySkyCounterUIJsonRecord(SkyOS::SnapshotSkyCounterUIPickupHistory(), index, buffer, capacity); }
    catch (...) { return 0; }
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SkyCounterUI_GetRecentLocationCount()
{
    try { return static_cast<std::uint32_t>(SkyOS::SnapshotSkyCounterUILocationHistory().size()); }
    catch (...) { return 0; }
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SkyCounterUI_CopyRecentLocationJsonAt(
    std::uint32_t index, char* buffer, std::uint32_t capacity)
{
    try { return CopySkyCounterUIJsonRecord(SkyOS::SnapshotSkyCounterUILocationHistory(), index, buffer, capacity); }
    catch (...) { return 0; }
}

namespace SkyOS {
    bool InitializeSkyCounterUI(PRISMA_UI_API::IVPrismaUI1* prisma, PrismaView mainView)
    {
        if (!prisma || !mainView || !prisma->IsValid(mainView)) return false;
        g_prisma = prisma;
        g_view = g_prisma->CreateView("SkyOS/SkyCounter/index.html", OnDomReady);
        if (!g_view) return false;
        g_prisma->Hide(g_view);
        g_prisma->SetOrder(g_view, g_prisma->GetOrder(mainView) + 1);
        g_prisma->RegisterJSListener(g_view, "skyos_skycounter_hide", OnHide);
        return true;
    }

    void SetSkyCounterUISettings(const char* json)
    {
        if (!json) return;
        g_settings = json;
        SetSkyCounterUIIconCaptureEnabled(JsonBooleanOption(g_settings, "renderIcons", true));
        const bool captureLocations = JsonBooleanOption(g_settings, "captureLocationScreenshots", true);
        const bool wasCapturingLocations = g_captureLocationScreenshots.exchange(
            captureLocations, std::memory_order_acq_rel);
        if (wasCapturingLocations && !captureLocations) CancelSkyCounterUIFrameCaptures();
        const auto enabledKey = g_settings.find("\"enabled\"");
        bool enabled = true;
        if (enabledKey != std::string::npos) {
            const auto colon = g_settings.find(':', enabledKey);
            const auto value = colon == std::string::npos ? std::string::npos : g_settings.find_first_not_of(" \t\r\n", colon + 1);
            if (value != std::string::npos && g_settings.compare(value, 5, "false") == 0) enabled = false;
        }
        std::deque<std::string> pending;
        {
            std::scoped_lock lock(g_pendingMutex);
            if (!enabled) g_pendingNotices.clear();
            else if (g_ready) pending.swap(g_pendingNotices);
        }
        g_optionsLoaded.store(true, std::memory_order_release);
        g_enabled.store(enabled, std::memory_order_release);
        if (!g_prisma || !g_ready || !g_view || !g_prisma->IsValid(g_view)) return;
        g_prisma->Invoke(g_view, ("SkyCounter.setSettings(" + g_settings + ")").c_str());
        if (!enabled) g_prisma->Hide(g_view);
        else if (!pending.empty()) {
            g_prisma->Show(g_view);
            for (const auto& notice : pending) {
                g_prisma->Invoke(g_view, ("SkyCounter.showNotice(" + notice + ")").c_str());
            }
        }
    }

    bool IsSkyCounterUILocationScreenshotCaptureEnabled()
    {
        return g_captureLocationScreenshots.load(std::memory_order_acquire);
    }

    void InstallSkyCounterUIPickupEventSink(SkyCounterUIPickupCallback callback)
    {
        g_pickupCallback = callback;
        for (const auto& item : SnapshotSkyCounterUIPickupHistory()) {
            if (g_pickupCallback) g_pickupCallback(item);
        }
        if (g_pickupSinkInstalled.load(std::memory_order_acquire)) return;
        auto* events = RE::ScriptEventSourceHolder::GetSingleton();
        if (!events) {
            SKSE::log::error("SkyCounterUI could not access Skyrim's inventory event source");
            return;
        }
        bool expected = false;
        if (!g_pickupSinkInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        events->AddEventSink<RE::TESContainerChangedEvent>(&g_pickupEventSink);
        SKSE::log::info("SkyCounterUI is listening for player inventory pickups independently of SkyCollection");
    }

    void ShowSkyCounterUINotice(const char* json)
    {
        if (json) InvokeNotice(json);
    }

    void UpdateSkyCounterUIIcon(const char* json)
    {
        if (!json) return;
        const bool viewReady = g_prisma && g_ready && g_view && g_prisma->IsValid(g_view);
        if (!viewReady) {
            SKSE::log::warn("SkyCounterUI received an icon update but its PrismaUI view is unavailable; payload='{}'", json);
            return;
        }
        SKSE::log::info("SkyCounterUI forwarded icon update to its PrismaUI view: payload='{}'", json);
        g_prisma->Invoke(g_view, ("SkyCounter.updateIcon(" + std::string(json) + ")").c_str());
    }

    void UpdateSkyCounterUILocation(const char* json)
    {
        if (json && g_prisma && g_ready && g_view && g_prisma->IsValid(g_view))
            g_prisma->Invoke(g_view, ("SkyCounter.updateLocation(" + std::string(json) + ")").c_str());
    }

    void ClearSkyCounterUIRecentStream()
    {
        ++g_saveGeneration;
        {
            std::scoped_lock lock(g_pickupQueueMutex);
            g_pendingPickups.clear();
            g_pickupDrainScheduled = false;
        }
        {
            std::scoped_lock lock(g_pickupHistoryMutex);
            g_recentPickups.clear();
        }
        {
            std::scoped_lock lock(g_pendingMutex);
            g_pendingNotices.clear();
        }
        ResetSkyCounterUILocationSaveState();
        if (g_pickupCallback) g_pickupCallback("{\"clearStream\":true}");
        if (g_prisma && g_ready && g_view && g_prisma->IsValid(g_view)) {
            g_prisma->Invoke(g_view, "SkyCounter.clearNotice()");
        }
    }

    std::vector<std::string> SnapshotSkyCounterUIPickupHistory()
    {
        std::scoped_lock lock(g_pickupHistoryMutex);
        return std::vector<std::string>(g_recentPickups.rbegin(), g_recentPickups.rend());
    }

    void RestoreSkyCounterUIPickupHistory(const std::vector<std::string>& history)
    {
        std::vector<std::string> restoredHistory;
        restoredHistory.reserve(history.size());
        std::size_t cachedIcons = 0;
        std::size_t excludedGold = 0;
        for (const auto& item : history) {
            if (item.empty() || item.size() > 16384) continue;
            if (IsGoldPickupRecord(item)) { ++excludedGold; continue; }
            auto restored = RestoreCachedIcon(item);
            if (restored != item) ++cachedIcons;
            restoredHistory.push_back(std::move(restored));
        }
        {
            std::scoped_lock lock(g_pickupHistoryMutex);
            g_recentPickups.clear();
            for (auto it = restoredHistory.rbegin(); it != restoredHistory.rend() && g_recentPickups.size() < 12; ++it) {
                g_recentPickups.push_back(*it);
            }
        }
        if (g_pickupCallback) {
            for (const auto& item : restoredHistory) g_pickupCallback(item);
        }
        SKSE::log::info("SkyCounterUI restored {} pickup records, attached {} existing cached icons, and excluded {} legacy gold record(s)",
            restoredHistory.size(), cachedIcons, excludedGold);
    }
}
