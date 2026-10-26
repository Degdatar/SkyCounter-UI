#include "skycounter_ui.h"

#include <SKSE/SKSE.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr std::uint32_t kPluginID = 'SKCU';
    constexpr std::uint32_t kRecordType = 'SCUI';
    constexpr std::uint32_t kRecordVersion = 1;
    constexpr std::uint32_t kMaxRecordBytes = 16 * 1024 * 1024;
    constexpr std::uint32_t kSkyRewriteLocationDiscovered = 'SRLD';

    struct SkyRewriteLocationNotice
    {
        std::uint32_t formID{};
        std::uint32_t localFormID{};
        char plugin[260]{};
        char name[256]{};
    };

    bool Append(std::vector<std::byte>& bytes, const void* data, std::size_t size)
    {
        if (size > kMaxRecordBytes || bytes.size() > kMaxRecordBytes - size) return false;
        const auto* first = static_cast<const std::byte*>(data);
        bytes.insert(bytes.end(), first, first + size);
        return true;
    }

    bool AppendString(std::vector<std::byte>& bytes, const std::string& value, std::size_t limit)
    {
        if (value.size() > limit || value.size() > (std::numeric_limits<std::uint32_t>::max)()) return false;
        const auto length = static_cast<std::uint32_t>(value.size());
        return Append(bytes, &length, sizeof(length)) && Append(bytes, value.data(), value.size());
    }

    template <class T>
    bool Read(const std::byte*& cursor, const std::byte* end, T& value)
    {
        if (static_cast<std::size_t>(end - cursor) < sizeof(T)) return false;
        std::memcpy(&value, cursor, sizeof(T));
        cursor += sizeof(T);
        return true;
    }

    bool ReadString(const std::byte*& cursor, const std::byte* end, std::string& value, std::size_t limit)
    {
        std::uint32_t length{};
        if (!Read(cursor, end, length) || length > limit || static_cast<std::size_t>(end - cursor) < length) return false;
        value.assign(reinterpret_cast<const char*>(cursor), length);
        cursor += length;
        return true;
    }

    void DrainRecord(SKSE::SerializationInterface* serialization, std::uint32_t length)
    {
        std::array<std::byte, 4096> buffer{};
        while (length) {
            const auto chunk = std::min<std::uint32_t>(length, static_cast<std::uint32_t>(buffer.size()));
            if (serialization->ReadRecordData(buffer.data(), chunk) != chunk) break;
            length -= chunk;
        }
    }

    void Save(SKSE::SerializationInterface* serialization)
    {
        if (!serialization) return;
        const auto pickups = SkyOS::SnapshotSkyCounterUIPickupHistory();
        const auto locations = SkyOS::SnapshotSkyCounterUILocationHistory();
        const auto locationKeys = SkyOS::SnapshotSkyCounterUILocationKeys();
        std::vector<std::byte> bytes;
        const auto pickupCount = static_cast<std::uint32_t>(std::min<std::size_t>(pickups.size(), 12));
        const auto locationCount = static_cast<std::uint32_t>(std::min<std::size_t>(locations.size(), 12));
        const auto keyCount = static_cast<std::uint32_t>(std::min<std::size_t>(locationKeys.size(), 10000));
        bool valid = Append(bytes, &pickupCount, sizeof(pickupCount));
        for (std::uint32_t i = 0; valid && i < pickupCount; ++i) valid = AppendString(bytes, pickups[i], 16384);
        valid = valid && Append(bytes, &locationCount, sizeof(locationCount));
        for (std::uint32_t i = 0; valid && i < locationCount; ++i) valid = AppendString(bytes, locations[i], 16384);
        valid = valid && Append(bytes, &keyCount, sizeof(keyCount));
        for (std::uint32_t i = 0; valid && i < keyCount; ++i) valid = AppendString(bytes, locationKeys[i], 512);
        if (!valid || bytes.size() > kMaxRecordBytes ||
            !serialization->OpenRecord(kRecordType, kRecordVersion) ||
            !serialization->WriteRecordData(bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
            SKSE::log::error("SkyCounterUI could not save this save's recent pickup and location history");
        }
    }

    void Load(SKSE::SerializationInterface* serialization)
    {
        if (!serialization) return;
        SkyOS::ClearSkyCounterUIRecentStream();
        std::uint32_t type{}, version{}, length{};
        while (serialization->GetNextRecordInfo(type, version, length)) {
            if (type != kRecordType) {
                DrainRecord(serialization, length);
                continue;
            }
            if (version != kRecordVersion || length > kMaxRecordBytes || length < sizeof(std::uint32_t)) {
                DrainRecord(serialization, length);
                SKSE::log::warn("SkyCounterUI skipped an unsupported save-history record");
                continue;
            }
            std::vector<std::byte> bytes(length);
            if (serialization->ReadRecordData(bytes.data(), length) != length) {
                SKSE::log::error("SkyCounterUI could not read this save's history record");
                continue;
            }
            const auto* cursor = bytes.data();
            const auto* end = cursor + bytes.size();
            std::uint32_t pickupCount{};
            if (!Read(cursor, end, pickupCount) || pickupCount > 12) continue;
            std::vector<std::string> pickups;
            pickups.reserve(pickupCount);
            bool valid = true;
            for (std::uint32_t i = 0; valid && i < pickupCount; ++i) {
                std::string item;
                valid = ReadString(cursor, end, item, 16384);
                if (valid) pickups.push_back(std::move(item));
            }
            std::uint32_t locationCount{};
            valid = valid && Read(cursor, end, locationCount) && locationCount <= 12;
            std::vector<std::string> locations;
            locations.reserve(valid ? locationCount : 0);
            for (std::uint32_t i = 0; valid && i < locationCount; ++i) {
                std::string location;
                valid = ReadString(cursor, end, location, 16384);
                if (valid) locations.push_back(std::move(location));
            }
            std::uint32_t keyCount{};
            valid = valid && Read(cursor, end, keyCount) && keyCount <= 10000;
            std::vector<std::string> keys;
            keys.reserve(valid ? keyCount : 0);
            for (std::uint32_t i = 0; valid && i < keyCount; ++i) {
                std::string key;
                valid = ReadString(cursor, end, key, 512);
                if (valid) keys.push_back(std::move(key));
            }
            if (!valid || cursor != end) {
                SKSE::log::warn("SkyCounterUI rejected a malformed save-history record");
                continue;
            }
            SkyOS::RestoreSkyCounterUIPickupHistory(pickups);
            SkyOS::RestoreSkyCounterUILocationHistory(locations, keys);
            SKSE::log::info("SkyCounterUI restored {} pickups and {} location discoveries for this save",
                pickups.size(), keys.size());
        }
    }

    void Revert(SKSE::SerializationInterface*)
    {
        SkyOS::ClearSkyCounterUIRecentStream();
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message) return;
        const bool skyRewriteSender = message->sender &&
            (std::strcmp(message->sender, "SkyRewrite") == 0 || std::strcmp(message->sender, "SkyRewrite.dll") == 0);
        if (message->type == kSkyRewriteLocationDiscovered && !skyRewriteSender) {
            SKSE::log::warn("SkyCounterUI received SRLD from unexpected sender '{}'", message->sender ? message->sender : "(null)");
            return;
        }
        if (skyRewriteSender && message->type == kSkyRewriteLocationDiscovered) {
            if (!message->data || message->dataLen != sizeof(SkyRewriteLocationNotice)) {
                SKSE::log::warn("SkyCounterUI rejected SkyRewrite location notice: dataPresent={}, dataLen={}, expected={}",
                    message->data != nullptr, message->dataLen, sizeof(SkyRewriteLocationNotice));
                return;
            }
            SkyRewriteLocationNotice notice{};
            std::memcpy(&notice, message->data, sizeof(notice));
            const auto pluginLength = std::find(notice.plugin, notice.plugin + sizeof(notice.plugin), '\0') - notice.plugin;
            const auto nameLength = std::find(notice.name, notice.name + sizeof(notice.name), '\0') - notice.name;
            if (nameLength) {
                const auto plugin = pluginLength
                    ? std::string(notice.plugin, static_cast<std::size_t>(pluginLength))
                    : std::string("Unknown plugin");
                SKSE::log::info("SkyCounterUI received SkyRewrite location notice '{}' ({:08X}, {})",
                    std::string(notice.name, static_cast<std::size_t>(nameLength)), notice.formID, plugin);
                SkyOS::RequestSkyCounterUILocationScreenshot(
                    plugin,
                    notice.formID, notice.localFormID,
                    std::string(notice.name, static_cast<std::size_t>(nameLength)));
            } else {
                SKSE::log::warn("SkyCounterUI rejected invalid SkyRewrite location notice: formID={:08X}, localFormID={:08X}, pluginLength={}, nameLength={}",
                    notice.formID, notice.localFormID, pluginLength, nameLength);
            }
            return;
        }
        if (message->type == SKSE::MessagingInterface::kPreLoadGame ||
            message->type == SKSE::MessagingInterface::kNewGame) {
            SkyOS::ClearSkyCounterUIRecentStream();
            return;
        }
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            const bool skyRewriteLoaded = GetModuleHandleW(L"SkyRewrite.dll") != nullptr;
            SkyOS::SetSkyCounterUILocationDiscoverySource(skyRewriteLoaded);
            if (!GetModuleHandleW(L"SkyOS.dll")) {
                SKSE::log::critical("SkyCounterUI requires SkyOS.dll; the module will remain inactive without the core");
            }
        }
    }
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener("SKSE", OnMessage)) {
        SKSE::log::critical("SkyCounterUI could not register its SKSE lifecycle listener");
        return false;
    }
    if (!messaging->RegisterListener("SkyRewrite", OnMessage)) {
        SKSE::log::warn("SkyCounterUI could not register for SkyRewrite location notices; map-marker discoveries remain available when SkyRewrite is absent");
    }
    const auto* serialization = SKSE::GetSerializationInterface();
    if (!serialization) {
        SKSE::log::error("SkyCounterUI could not access SKSE serialization; recent history will not persist by save");
    } else {
        serialization->SetUniqueID(kPluginID);
        serialization->SetSaveCallback(Save);
        serialization->SetLoadCallback(Load);
        serialization->SetRevertCallback(Revert);
    }
    return true;
}
