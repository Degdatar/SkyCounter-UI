#include "pch.h"
#include "skyitemicons.h"

#include <RE/A/AlchemyItem.h>
#include <RE/B/BSModelDB.h>
#include <RE/B/BGSTextureSet.h>
#include <RE/B/BSResourceNiBinaryStream.h>
#include <RE/B/BSLightingShaderProperty.h>
#include <RE/B/BSLightingShaderMaterialBase.h>
#include <RE/B/BSLightingShaderMaterialGlowmap.h>
#include <RE/I/IngredientItem.h>
#include <RE/I/IMenu.h>
#include <RE/I/Inventory3DManager.h>
#include <RE/B/BSTriShape.h>
#include <RE/S/ScrollItem.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/N/NiSourceTexture.h>
#include <RE/N/NiMatrix3.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiPoint3.h>
#include <RE/R/Renderer.h>
#include <RE/T/TESAmmo.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESObjectARMO.h>
#include <RE/T/TESObjectBOOK.h>
#include <RE/T/TESObjectLIGH.h>
#include <RE/T/TESObjectMISC.h>
#include <RE/T/TESObjectWEAP.h>
#include <RE/T/TESModelTextureSwap.h>
#include <RE/U/UI.h>
#include <RE/U/UI3DSceneManager.h>
#include <RE/U/UIMessageQueue.h>
#include <REX/W32/D3D11.h>
#include <REX/W32/D3D11_1.h>
#include <REX/W32/DXGI.h>

#include <Windows.h>
#include <DDSTextureLoader.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SkyOS {
namespace {
    using Texture2D = REX::W32::ID3D11Texture2D;
    using Device = REX::W32::ID3D11Device;
    using Context = REX::W32::ID3D11DeviceContext;
    using Rtv = REX::W32::ID3D11RenderTargetView;
    using Dsv = REX::W32::ID3D11DepthStencilView;
    using Buffer = REX::W32::ID3D11Buffer;
    using ShaderResourceView = REX::W32::ID3D11ShaderResourceView;
    using VertexShader = REX::W32::ID3D11VertexShader;
    using PixelShader = REX::W32::ID3D11PixelShader;
    using InputLayout = REX::W32::ID3D11InputLayout;
    using BlendState = REX::W32::ID3D11BlendState;
    using DepthStencilState = REX::W32::ID3D11DepthStencilState;
    using RasterizerState = REX::W32::ID3D11RasterizerState;
    using SamplerState = REX::W32::ID3D11SamplerState;
    using Query = REX::W32::ID3D11Query;
    using CommandList = REX::W32::ID3D11CommandList;

    // Preserve output-merger bindings while the location screenshot path
    // copies the game's frame into a staging texture. This guard lives at
    // namespace scope because location capture is outside the inventory
    // renderer class that owns its separate ContextOutputState helper.
    class LocationOutputState final {
    public:
        explicit LocationOutputState(Context* context) : context_(context)
        {
            if (!context_) return;
            context_->OMGetRenderTargets(static_cast<std::uint32_t>(renderTargets_.size()),
                renderTargets_.data(), &depthView_);
            for (std::size_t i = 0; i < renderTargets_.size(); ++i) {
                if (renderTargets_[i]) renderTargetCount_ = static_cast<std::uint32_t>(i + 1);
            }
            viewportCount_ = static_cast<std::uint32_t>(viewports_.size());
            context_->RSGetViewports(&viewportCount_, viewports_.data());
        }

        LocationOutputState(const LocationOutputState&) = delete;
        LocationOutputState& operator=(const LocationOutputState&) = delete;

        ~LocationOutputState()
        {
            if (context_) {
                if (targetStateChanged_) {
                    context_->OMSetRenderTargets(renderTargetCount_, renderTargets_.data(), depthView_);
                    context_->RSSetViewports(viewportCount_, viewports_.data());
                }
                for (auto*& target : renderTargets_) Release(target);
                Release(depthView_);
            }
        }

        void UnbindTargets()
        {
            if (!context_) return;
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            targetStateChanged_ = true;
        }

    private:
        template <class T>
        static void Release(T*& value)
        {
            if (value) value->Release();
            value = nullptr;
        }

        Context* context_{};
        std::array<Rtv*, 8> renderTargets_{};
        Dsv* depthView_{};
        std::array<REX::W32::D3D11_VIEWPORT, 16> viewports_{};
        std::uint32_t renderTargetCount_{};
        std::uint32_t viewportCount_{};
        bool targetStateChanged_{};
    };

    void CaptureMenuFrame();
    void CaptureInventoryPreviewFrame();
    void ProcessLocationFrameCapture();
    bool HasLocationFrameCaptureWork();
    void ProcessPendingReadback();
    void ScheduleGamePump(const char* reason = "unspecified");
    bool IsInventoryPreviewMenuOpen(RE::UI* ui);
    void DeferActiveCaptureForInventoryMenu();
    std::string TextureCacheKey(std::string path);
    std::string NormalizeTextureResourceName(std::string path);

    constexpr char kRenderMenuName[] = "SkyCounterUIRenderMenu";
    constexpr std::uint32_t kOutputSize = 128;
    constexpr std::uint32_t kOffscreenSize = kOutputSize * 2;
    constexpr std::uint32_t kIconRendererVersion = 15;
    constexpr std::size_t kQueueLimit = 10000;
    constexpr std::uint32_t kManagerPumpIntervalFrames = 8;
    constexpr std::uint32_t kModelLoadWarningFrames = 60;
    constexpr std::uint32_t kIdleManagerRecoveryTimeoutFrames = 120;
    constexpr std::uint32_t kModelLoadTimeoutFrames = 120;
    constexpr std::uint32_t kMenuPreviewWaitFrames = 24;
    constexpr std::uint64_t kCaptureMenuShowRetryMilliseconds = 1000;

    enum class CaptureMenuKind : std::uint8_t { None, Render };

    std::atomic<CaptureMenuKind> g_captureMenuShowRequested{ CaptureMenuKind::None };
    std::atomic<bool> g_captureMenuHideRequested{ false };
    std::atomic<bool> g_captureMenuPauseOwned{ false };
    std::atomic<std::uint64_t> g_captureMenuShowRequestedAtMs{ 0 };

    class SkyCounterUIRenderMenu final : public RE::IMenu {
    public:
        void PostDisplay() override { CaptureMenuFrame(); }
        ~SkyCounterUIRenderMenu() override { ScheduleGamePump("capture render menu destroyed"); }
    };

    RE::IMenu* CreateSkyCounterUIRenderMenu()
    {
        auto* menu = new SkyCounterUIRenderMenu();
        menu->depthPriority = 11;
        // This invisible service menu provides the inventory-scene render
        // callback without pausing the world or showing a menu movie.
        menu->menuFlags.set(RE::UI_MENU_FLAGS::kCustomRendering,
            RE::UI_MENU_FLAGS::kAllowSaving,
            RE::UI_MENU_FLAGS::kInventoryItemMenu,
            RE::UI_MENU_FLAGS::kRendersOffscreenTargets);
        return menu;
    }

    enum class CapturePhase : std::uint8_t { Idle, Loading, Ready, Captured, Failed };

    struct TextureSwapSnapshot {
        std::uint32_t index3D{};
        std::string shapeName;
        std::vector<std::string> texturePaths;
    };

    struct CaptureRequest {
        RE::FormID formID{};
        std::filesystem::path path;
        RE::TESBoundObject* item{};
        RE::TESBoundObject* restoreItem{};
        std::string modelPath;
        bool ownsInventoryScene{};
        bool usesMenuPreview{};
    };

    struct NativeModelJob {
        RE::FormID formID{};
        std::string modelPath;
        std::vector<TextureSwapSnapshot> textureSwaps;
    };

    struct NativeModelResult {
        RE::FormID formID{};
        RE::NiPointer<RE::NiNode> model;
        std::unordered_map<std::string, std::vector<std::uint8_t>> textures;
        std::string error;
    };

    struct PixelJob {
        RE::FormID formID{};
        std::filesystem::path path;
        std::array<std::uint8_t, kOutputSize * kOutputSize * 4> bgra{};
    };

    struct LocationFrameRequest {
        std::filesystem::path path;
        SkyCounterUIFrameCaptureCallback completion;
    };

    struct LocationFramePixelJob {
        std::filesystem::path path;
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t sourceFormat{};
        std::vector<std::uint8_t> bgra;
        SkyCounterUIFrameCaptureCallback completion;
    };

    enum class OffscreenPoll : std::uint8_t { Idle, Pending, Ready, Failed };

    struct OffscreenVertex {
        float position[3];
        float uv[2];
        std::uint32_t color;
        float emissive[4];
        float normal[3];
        float tangent[4];
        float specular[4];
        float material[4];
        float materialFlags[4];
        float environment[4];
    };

    struct OffscreenSection {
        std::vector<OffscreenVertex> vertices;
        std::vector<std::uint32_t> indices;
        ShaderResourceView* texture{};
        ShaderResourceView* normalTexture{};
        ShaderResourceView* specularTexture{};
        ShaderResourceView* glowTexture{};
        ShaderResourceView* effectMaskTexture{};
        ShaderResourceView* effectGradientTexture{};
        ShaderResourceView* effectLightingTexture{};
        ShaderResourceView* environmentMaskTexture{};
        ShaderResourceView* environmentTexture{};
        std::array<float, 3> emissiveColor{};
        float emissiveMultiple{};
        std::array<float, 3> specularColor{ 1.0f, 1.0f, 1.0f };
        float specularStrength{ 1.0f };
        float glossiness{ 24.0f };
        float materialAlpha{ 1.0f };
        float environmentMapScale{ 1.0f };
        float alphaThreshold{ 0.04f };
        bool useSpecular{ true };
        bool useGlowMap{ false };
        bool isEffectShader{ false };
        bool useAlphaTest{ false };
        bool useEnvironmentMapping{ false };
        bool normalAlphaSpecular{ false };
        bool hasTangents{ false };
        bool hasEnvironmentCube{ false };
    };

    struct OffscreenTextureSwap {
        RE::NiAVObject* target{};
        std::array<ShaderResourceView*, RE::BSTextureSet::Textures::kUsedTotal> textures{};
    };

    struct OffscreenDraw {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        ShaderResourceView* texture{};
        ShaderResourceView* normalTexture{};
        ShaderResourceView* specularTexture{};
        ShaderResourceView* glowTexture{};
        ShaderResourceView* effectMaskTexture{};
        ShaderResourceView* effectGradientTexture{};
        ShaderResourceView* effectLightingTexture{};
        ShaderResourceView* environmentMaskTexture{};
        ShaderResourceView* environmentTexture{};
    };

    void BuildTangentFrame(OffscreenSection& section)
    {
        using Vec3 = std::array<float, 3>;
        std::vector<Vec3> normalSums(section.vertices.size());
        std::vector<Vec3> tangentSums(section.vertices.size());
        std::vector<Vec3> bitangentSums(section.vertices.size());
        for (std::size_t i = 0; i + 2 < section.indices.size(); i += 3) {
            const auto i0 = section.indices[i];
            const auto i1 = section.indices[i + 1];
            const auto i2 = section.indices[i + 2];
            if (i0 >= section.vertices.size() || i1 >= section.vertices.size() || i2 >= section.vertices.size()) continue;
            const auto& v0 = section.vertices[i0];
            const auto& v1 = section.vertices[i1];
            const auto& v2 = section.vertices[i2];
            const Vec3 e1{ v1.position[0] - v0.position[0], v1.position[1] - v0.position[1], v1.position[2] - v0.position[2] };
            const Vec3 e2{ v2.position[0] - v0.position[0], v2.position[1] - v0.position[1], v2.position[2] - v0.position[2] };
            const Vec3 face{ e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            for (const auto index : { i0, i1, i2 })
                for (std::size_t axis = 0; axis < 3; ++axis) normalSums[index][axis] += face[axis];

            const float du1 = v1.uv[0] - v0.uv[0];
            const float dv1 = v1.uv[1] - v0.uv[1];
            const float du2 = v2.uv[0] - v0.uv[0];
            const float dv2 = v2.uv[1] - v0.uv[1];
            const float determinant = du1 * dv2 - du2 * dv1;
            if (std::abs(determinant) < 1.0e-8f) continue;
            const float inverse = 1.0f / determinant;
            const Vec3 tangent{
                (e1[0] * dv2 - e2[0] * dv1) * inverse,
                (e1[1] * dv2 - e2[1] * dv1) * inverse,
                (e1[2] * dv2 - e2[2] * dv1) * inverse };
            const Vec3 bitangent{
                (e2[0] * du1 - e1[0] * du2) * inverse,
                (e2[1] * du1 - e1[1] * du2) * inverse,
                (e2[2] * du1 - e1[2] * du2) * inverse };
            for (const auto index : { i0, i1, i2 }) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    tangentSums[index][axis] += tangent[axis];
                    bitangentSums[index][axis] += bitangent[axis];
                }
            }
        }

        const auto normalize = [](Vec3& value, const Vec3& fallback) {
            float lengthSquared = value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
            if (lengthSquared < 1.0e-12f) {
                value = fallback;
                lengthSquared = value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
            }
            if (lengthSquared < 1.0e-12f) { value = { 0.0f, 0.0f, 1.0f }; return; }
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            for (auto& component : value) component *= inverseLength;
        };
        for (std::size_t i = 0; i < section.vertices.size(); ++i) {
            auto& vertex = section.vertices[i];
            if (section.hasTangents) {
                Vec3 normal{ vertex.normal[0], vertex.normal[1], vertex.normal[2] };
                Vec3 tangent{ vertex.tangent[0], vertex.tangent[1], vertex.tangent[2] };
                normalize(normal, normalSums[i]);
                const float projection = normal[0] * tangent[0] + normal[1] * tangent[1] + normal[2] * tangent[2];
                for (std::size_t axis = 0; axis < 3; ++axis) tangent[axis] -= normal[axis] * projection;
                normalize(tangent, tangentSums[i]);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    vertex.normal[axis] = normal[axis];
                    vertex.tangent[axis] = tangent[axis];
                }
                continue;
            }
            Vec3 normal{ vertex.normal[0], vertex.normal[1], vertex.normal[2] };
            if (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] < 1.0e-8f)
                normal = normalSums[i];
            normalize(normal, { 0.0f, 0.0f, 1.0f });

            Vec3 tangent = tangentSums[i];
            const float projection = normal[0] * tangent[0] + normal[1] * tangent[1] + normal[2] * tangent[2];
            for (std::size_t axis = 0; axis < 3; ++axis) tangent[axis] -= normal[axis] * projection;
            normalize(tangent, std::abs(normal[2]) < 0.9f ? Vec3{ -normal[1], normal[0], 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f });

            const Vec3 cross{ normal[1] * tangent[2] - normal[2] * tangent[1],
                normal[2] * tangent[0] - normal[0] * tangent[2],
                normal[0] * tangent[1] - normal[1] * tangent[0] };
            const float handedness = cross[0] * bitangentSums[i][0] +
                cross[1] * bitangentSums[i][1] + cross[2] * bitangentSums[i][2] < 0.0f ? -1.0f : 1.0f;
            vertex.normal[0] = normal[0]; vertex.normal[1] = normal[1]; vertex.normal[2] = normal[2];
            vertex.tangent[0] = tangent[0]; vertex.tangent[1] = tangent[1]; vertex.tangent[2] = tangent[2];
            vertex.tangent[3] = handedness;
        }
    }

    class SkyCounterUIOffscreenRenderer final {
    public:
        ~SkyCounterUIOffscreenRenderer() { Reset(); }

        bool Submit(RE::NiAVObject* root, RE::FormID formID, const std::filesystem::path& path,
            const std::vector<TextureSwapSnapshot>& textureSwaps,
            const std::unordered_map<std::string, std::vector<std::uint8_t>>& textureBytes,
            Device* device, Context* immediate)
        {
            if (!root || !formID || path.empty() || !device || !immediate) return false;

            std::vector<OffscreenTextureSwap> resolvedSwaps;
            resolvedSwaps.reserve(textureSwaps.size());
            for (const auto& swap : textureSwaps) {
                if (swap.shapeName.empty()) continue;
                auto* target = root->GetObjectByName(RE::BSFixedString(swap.shapeName));
                if (!target || !target->AsTriShape()) continue;

                OffscreenTextureSwap resolved;
                resolved.target = target;
                for (std::size_t slot = 0; slot < swap.texturePaths.size() && slot < resolved.textures.size(); ++slot) {
                    if (swap.texturePaths[slot].empty()) continue;
                    const auto key = TextureCacheKey(NormalizeTextureResourceName(swap.texturePaths[slot]));
                    const auto bytes = textureBytes.find(key);
                    if (bytes != textureBytes.end()) {
                        const bool colorTexture = slot == RE::BSTextureSet::Textures::kDiffuse ||
                            slot == RE::BSTextureSet::Textures::kGlowMap ||
                            slot == RE::BSTextureSet::Textures::kEnvironment;
                        resolved.textures[slot] = LoadTexture(key, bytes->second, device, colorTexture);
                    }
                }
                resolvedSwaps.push_back(resolved);
            }

            std::vector<OffscreenSection> sections;
            RE::NiTransform identity;
            Gather(root, identity, true, sections, resolvedSwaps);
            return SubmitSections(std::move(sections), formID, path, device, immediate);
        }

        OffscreenPoll Poll(PixelJob& job)
        {
            if (!pending_) return OffscreenPoll::Idle;
            job.formID = formID_;
            job.path = path_;
            const auto queryResult = immediate_->GetData(query_, nullptr, 0,
                REX::W32::D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (queryResult == S_FALSE) return ++waitedFrames_ < 180 ? OffscreenPoll::Pending : Finish(OffscreenPoll::Failed);
            if (FAILED(queryResult)) return Finish(OffscreenPoll::Failed);

            REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(immediate_->Map(staging_, 0, REX::W32::D3D11_MAP_READ,
                    REX::W32::D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
                return ++waitedFrames_ < 180 ? OffscreenPoll::Pending : Finish(OffscreenPoll::Failed);
            }
            const auto* pixels = static_cast<const std::uint8_t*>(mapped.data);
            for (std::uint32_t y = 0; y < kOutputSize; ++y) {
                for (std::uint32_t x = 0; x < kOutputSize; ++x) {
                    const auto dst = (static_cast<std::size_t>(y) * kOutputSize + x) * 4;
                    std::uint32_t premul[3]{};
                    std::uint32_t alpha{};
                    for (std::uint32_t sy = 0; sy < 2; ++sy) {
                        const auto* sampleRow = pixels + static_cast<std::size_t>(y * 2 + sy) * mapped.rowPitch;
                        for (std::uint32_t sx = 0; sx < 2; ++sx) {
                            const auto* sample = sampleRow + (x * 2 + sx) * 4;
                            premul[0] += sample[0]; premul[1] += sample[1]; premul[2] += sample[2]; alpha += sample[3];
                        }
                    }
                    const auto a = alpha / 4;
                    job.bgra[dst + 0] = a ? static_cast<std::uint8_t>((premul[2] / 4) * 255 / a) : 0;
                    job.bgra[dst + 1] = a ? static_cast<std::uint8_t>((premul[1] / 4) * 255 / a) : 0;
                    job.bgra[dst + 2] = a ? static_cast<std::uint8_t>((premul[0] / 4) * 255 / a) : 0;
                    job.bgra[dst + 3] = static_cast<std::uint8_t>(a);
                }
            }
            immediate_->Unmap(staging_, 0);
            return Finish(OffscreenPoll::Ready);
        }

        bool IsPending() const noexcept { return pending_; }

        bool Prepare(Device* device, Context* immediate)
        {
            return device && immediate && EnsureResources(device, immediate);
        }

        bool HasRenderableGeometry(RE::NiAVObject* root) const
        {
            return HasRenderableGeometryInTree(root);
        }

    private:
        bool SubmitSections(std::vector<OffscreenSection> sections, RE::FormID formID,
            const std::filesystem::path& path, Device* device, Context* immediate)
        {
            if (pending_ || !formID || path.empty() || !device || !immediate || sections.empty() ||
                !EnsureResources(device, immediate)) return false;

            for (auto& section : sections) BuildTangentFrame(section);

            std::vector<OffscreenVertex> vertices;
            std::vector<std::uint32_t> indices;
            std::vector<OffscreenDraw> draws;
            std::vector<RE::NiPoint3> points;
            std::vector<std::size_t> sectionOffsets;
            points.reserve(4096);
            for (const auto& section : sections) {
                sectionOffsets.push_back(points.size());
                for (const auto& vertex : section.vertices) {
                    points.emplace_back(vertex.position[0], vertex.position[1], vertex.position[2]);
                }
            }
            if (points.empty()) return false;

            float minX = (std::numeric_limits<float>::max)();
            float minY = minX;
            float minZ = minX;
            float maxX = (std::numeric_limits<float>::lowest)();
            float maxY = maxX;
            float maxZ = maxX;
            for (auto& point : points) {
                // Skyrim's item models are authored upright with +Z up and
                // their inventory-facing surface along the Y axis. Project
                // them as a front-facing orthographic preview; the previous
                // three-axis isometric projection made blades and plants look
                // unlike their in-game inventory models.
                const float x = point.x;
                const float y = point.z;
                const float z = point.y;
                point = { x, y, z };
                minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
                minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
                minZ = (std::min)(minZ, z); maxZ = (std::max)(maxZ, z);
            }
            const float width = (std::max)(maxX - minX, 0.001f);
            const float height = (std::max)(maxY - minY, 0.001f);
            const float scale = 1.72f / (std::max)(width, height);
            const float centerX = (minX + maxX) * 0.5f;
            const float centerY = (minY + maxY) * 0.5f;
            const float depth = (std::max)(maxZ - minZ, 0.001f);

            for (std::size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
                const auto& section = sections[sectionIndex];
                const auto vertexStart = static_cast<std::uint32_t>(vertices.size());
                const auto firstIndex = static_cast<std::uint32_t>(indices.size());
                for (std::size_t i = 0; i < section.vertices.size(); ++i) {
                    auto vertex = section.vertices[i];
                    const auto mapToIconView = [](float* vector) {
                        const float y = vector[1];
                        vector[1] = vector[2];
                        vector[2] = y;
                    };
                    mapToIconView(vertex.normal);
                    mapToIconView(vertex.tangent);
                    // Swapping Y/Z reverses the tangent-space handedness.
                    vertex.tangent[3] = -vertex.tangent[3];
                    vertex.emissive[0] = section.emissiveColor[0];
                    vertex.emissive[1] = section.emissiveColor[1];
                    vertex.emissive[2] = section.emissiveColor[2];
                    vertex.emissive[3] = section.emissiveMultiple;
                    vertex.specular[0] = section.specularColor[0];
                    vertex.specular[1] = section.specularColor[1];
                    vertex.specular[2] = section.specularColor[2];
                    vertex.specular[3] = section.specularStrength;
                    vertex.material[0] = std::clamp(section.glossiness, 1.0f, 256.0f);
                    vertex.material[1] = std::clamp(section.materialAlpha, 0.0f, 1.0f);
                    vertex.material[2] = std::clamp(section.alphaThreshold, 0.0f, 1.0f);
                    vertex.material[3] = section.useSpecular ? 1.0f : 0.0f;
                    vertex.materialFlags[0] = section.isEffectShader ? 1.0f : 0.0f;
                    vertex.materialFlags[1] = section.useGlowMap ? 1.0f : 0.0f;
                    vertex.materialFlags[2] = section.useAlphaTest ? 1.0f : 0.0f;
                    vertex.materialFlags[3] = section.normalAlphaSpecular ? 1.0f : 0.0f;
                    vertex.environment[0] = std::clamp(section.environmentMapScale, 0.0f, 8.0f);
                    vertex.environment[1] = section.useEnvironmentMapping ? 1.0f : 0.0f;
                    vertex.environment[2] = section.hasEnvironmentCube ? 1.0f : 0.0f;
                    const auto& point = points[sectionOffsets[sectionIndex] + i];
                    vertex.position[0] = (point.x - centerX) * scale;
                    vertex.position[1] = (point.y - centerY) * scale;
                    vertex.position[2] = 0.05f + ((point.z - minZ) / depth) * 0.9f;
                    vertices.push_back(vertex);
                }
                for (const auto index : section.indices) indices.push_back(vertexStart + index);
                draws.push_back({ firstIndex, static_cast<std::uint32_t>(section.indices.size()), section.texture,
                    section.normalTexture, section.specularTexture, section.glowTexture,
                    section.effectMaskTexture, section.effectGradientTexture, section.effectLightingTexture,
                    section.environmentMaskTexture, section.hasEnvironmentCube ? section.environmentTexture : nullptr });
            }
            if (vertices.empty() || indices.empty() || vertices.size() > 4'000'000 || indices.size() > 12'000'000) return false;

            const auto makeBuffer = [this](const void* data, std::size_t size, std::uint32_t bind, Buffer** out) {
                if (!data || !size || size > (std::numeric_limits<std::uint32_t>::max)()) return false;
                REX::W32::D3D11_BUFFER_DESC desc{};
                desc.byteWidth = static_cast<std::uint32_t>(size);
                desc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
                desc.bindFlags = bind;
                const REX::W32::D3D11_SUBRESOURCE_DATA initial{ data, 0, 0 };
                return SUCCEEDED(device_->CreateBuffer(&desc, &initial, out)) && *out;
            };
            if (!makeBuffer(vertices.data(), vertices.size() * sizeof(OffscreenVertex),
                    REX::W32::D3D11_BIND_VERTEX_BUFFER, &vertexBuffer_) ||
                !makeBuffer(indices.data(), indices.size() * sizeof(std::uint32_t),
                    REX::W32::D3D11_BIND_INDEX_BUFFER, &indexBuffer_)) {
                Release(vertexBuffer_); Release(indexBuffer_);
                return false;
            }

            REX::W32::D3D11_VIEWPORT viewport{};
            viewport.width = static_cast<float>(kOffscreenSize);
            viewport.height = static_cast<float>(kOffscreenSize);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            deferred_->OMSetRenderTargets(1, &rtv_, dsv_);
            deferred_->RSSetViewports(1, &viewport);
            const float clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
            deferred_->ClearRenderTargetView(rtv_, clear);
            deferred_->ClearDepthStencilView(dsv_, REX::W32::D3D11_CLEAR_DEPTH, 1.0f, 0);
            deferred_->RSSetState(rasterizer_);
            deferred_->OMSetBlendState(blend_, nullptr, 0xFFFFFFFFu);
            deferred_->OMSetDepthStencilState(depthState_, 0);
            deferred_->IASetInputLayout(inputLayout_);
            deferred_->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const std::uint32_t stride = sizeof(OffscreenVertex);
            const std::uint32_t offset = 0;
            deferred_->IASetVertexBuffers(0, 1, &vertexBuffer_, &stride, &offset);
            deferred_->IASetIndexBuffer(indexBuffer_, REX::W32::DXGI_FORMAT_R32_UINT, 0);
            deferred_->VSSetShader(vertexShader_, nullptr, 0);
            deferred_->PSSetShader(pixelShader_, nullptr, 0);
            deferred_->PSSetSamplers(0, 1, &sampler_);
            for (const auto& draw : draws) {
                auto* view = draw.texture ? draw.texture : whiteTexture_;
                auto* normalView = draw.normalTexture ? draw.normalTexture : flatNormalTexture_;
                auto* specularView = draw.specularTexture ? draw.specularTexture : blackTexture_;
                auto* glowView = draw.glowTexture ? draw.glowTexture : blackTexture_;
                auto* effectMaskView = draw.effectMaskTexture ? draw.effectMaskTexture : whiteTexture_;
                auto* effectGradientView = draw.effectGradientTexture ? draw.effectGradientTexture : whiteTexture_;
                auto* effectLightingView = draw.effectLightingTexture ? draw.effectLightingTexture : whiteTexture_;
                auto* environmentMaskView = draw.environmentMaskTexture ? draw.environmentMaskTexture :
                    (draw.environmentTexture ? whiteTexture_ : blackTexture_);
                auto* environmentCubeView = draw.environmentTexture;
                ShaderResourceView* views[] = { view, glowView, normalView, specularView,
                    effectMaskView, effectGradientView, environmentMaskView, environmentCubeView, effectLightingView };
                deferred_->PSSetShaderResources(0, 9, views);
                deferred_->DrawIndexed(draw.indexCount, draw.firstIndex, 0);
            }
            ShaderResourceView* noTextures[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
            deferred_->PSSetShaderResources(0, 9, noTextures);

            CommandList* commandList = nullptr;
            if (FAILED(deferred_->FinishCommandList(FALSE, &commandList)) || !commandList) {
                Release(vertexBuffer_); Release(indexBuffer_);
                return false;
            }
            immediate_->ExecuteCommandList(commandList, TRUE);
            Release(commandList);
            immediate_->CopyResource(staging_, target_);
            immediate_->End(query_);

            formID_ = formID;
            path_ = path;
            pending_ = true;
            waitedFrames_ = 0;
            return true;
        }
        ShaderResourceView* LoadTexture(const std::string& key,
            const std::vector<std::uint8_t>& bytes, Device* device, bool colorTexture)
        {
            const auto cacheKey = key + (colorTexture ? "#srgb" : "#linear");
            if (const auto found = textureCache_.find(cacheKey); found != textureCache_.end()) return found->second;
            if (failedTextureCache_.contains(cacheKey)) return nullptr;
            if (bytes.empty() || !device) return nullptr;
            ID3D11Resource* resource = nullptr;
            ID3D11ShaderResourceView* view = nullptr;
            const auto loadFlags = colorTexture ? DirectX::DDS_LOADER_FORCE_SRGB : DirectX::DDS_LOADER_DEFAULT;
            const auto result = DirectX::CreateDDSTextureFromMemoryEx(
                reinterpret_cast<ID3D11Device*>(device), bytes.data(), bytes.size(), 0,
                static_cast<::D3D11_USAGE>(REX::W32::D3D11_USAGE_DEFAULT),
                static_cast<unsigned int>(REX::W32::D3D11_BIND_SHADER_RESOURCE),
                0, 0, loadFlags, &resource, &view);
            if (resource) resource->Release();
            if (FAILED(result) || !view) {
                if (view) view->Release();
                failedTextureCache_.insert(cacheKey);
                SKSE::log::warn("SkyCounterUI could not create D3D texture '{}' from {} DDS bytes (HRESULT 0x{:08X})",
                    key, bytes.size(), static_cast<unsigned long>(result));
                return nullptr;
            }
            if (textureCache_.size() >= 128) {
                auto oldest = textureCache_.begin();
                Release(oldest->second);
                textureCache_.erase(oldest);
            }
            auto* sharedView = reinterpret_cast<ShaderResourceView*>(view);
            textureCache_.emplace(cacheKey, sharedView);
            return sharedView;
        }

        static bool HasRenderableGeometryInTree(RE::NiAVObject* object)
        {
            if (!object) return false;
            if (auto* shape = object->AsTriShape()) {
                const auto& geometry = shape->GetGeometryRuntimeData();
                const auto& tri = shape->GetTrishapeRuntimeData();
                if (geometry.rendererData && geometry.rendererData->rawVertexData &&
                    geometry.rendererData->rawIndexData && tri.vertexCount && tri.triangleCount &&
                    geometry.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) {
                    return true;
                }
            }
            if (auto* node = object->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (HasRenderableGeometryInTree(child.get())) return true;
                }
            }
            return false;
        }

        template <class T>
        static void Release(T*& value)
        {
            if (value) value->Release();
            value = nullptr;
        }

        static float HalfToFloat(std::uint16_t half)
        {
            const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
            int exponent = static_cast<int>((half >> 10) & 0x1Fu);
            std::uint32_t mantissa = half & 0x3FFu;
            std::uint32_t bits{};
            if (exponent == 0) {
                if (mantissa == 0) bits = sign;
                else {
                    exponent = 1;
                    while ((mantissa & 0x400u) == 0) { mantissa <<= 1; --exponent; }
                    mantissa &= 0x3FFu;
                    bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23) | (mantissa << 13);
                }
            } else if (exponent == 0x1Fu) {
                bits = sign | 0x7F800000u | (mantissa << 13);
            } else {
                bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23) | (mantissa << 13);
            }
            return std::bit_cast<float>(bits);
        }

        static void Gather(RE::NiAVObject* object, const RE::NiTransform& parent,
            bool isRoot, std::vector<OffscreenSection>& sections,
            const std::vector<OffscreenTextureSwap>& textureSwaps)
        {
            if (!object) return;
            auto local = object->local;
            if (isRoot) local.translate = RE::NiPoint3{};
            const auto transform = parent * local;
            if (auto* shape = object->AsTriShape(); shape) {
                const auto& runtime = shape->GetGeometryRuntimeData();
                const auto* mesh = runtime.rendererData;
                const auto& tri = shape->GetTrishapeRuntimeData();
                if (mesh && mesh->rawVertexData && mesh->rawIndexData && tri.vertexCount && tri.triangleCount &&
                    runtime.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) {
                    auto vertexDesc = runtime.vertexDesc;
                    const auto stride = vertexDesc.GetSize();
                    const auto uvOffset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
                    const auto colorOffset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
                    const bool hasUV = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV);
                    const bool hasColor = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS);
                    if (stride >= 12 && (!hasUV || uvOffset + 4 <= stride) && (!hasColor || colorOffset + 4 <= stride)) {
                        OffscreenSection section;
                        section.vertices.reserve(tri.vertexCount);
                        section.indices.reserve(static_cast<std::size_t>(tri.triangleCount) * 3);
                        auto* bytes = mesh->rawVertexData;
                        for (std::uint32_t i = 0; i < tri.vertexCount; ++i) {
                            const auto* source = bytes + static_cast<std::size_t>(i) * stride;
                            float x{}, y{}, z{};
                            std::memcpy(&x, source, sizeof(float));
                            std::memcpy(&y, source + 4, sizeof(float));
                            std::memcpy(&z, source + 8, sizeof(float));
                            const auto point = transform * RE::NiPoint3{x, y, z};
                            OffscreenVertex vertex{};
                            vertex.position[0] = point.x; vertex.position[1] = point.y; vertex.position[2] = point.z;
                            vertex.color = 0xFFFFFFFFu;
                            if (hasUV) {
                                std::uint16_t uv[2]{};
                                std::memcpy(uv, source + uvOffset, sizeof(uv));
                                vertex.uv[0] = HalfToFloat(uv[0]);
                                vertex.uv[1] = HalfToFloat(uv[1]);
                            }
                            if (hasColor) std::memcpy(&vertex.color, source + colorOffset, sizeof(vertex.color));
                            section.vertices.push_back(vertex);
                        }
                        const auto indexCount = static_cast<std::size_t>(tri.triangleCount) * 3;
                        bool validIndices = true;
                        for (std::size_t i = 0; i < indexCount; ++i) {
                            const auto index = mesh->rawIndexData[i];
                            if (index >= tri.vertexCount) { validIndices = false; break; }
                            section.indices.push_back(index);
                        }
                        if (validIndices) {
                            if (auto* shader = shape->lightingShaderProp_cast()) {
                                if (auto* texture = shader->GetBaseTexture(); texture && texture->rendererTexture)
                                    section.texture = texture->rendererTexture->resourceView;
                                auto* material = shader->GetBaseMaterial();
                                auto* lightingMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(material);
                                if (lightingMaterial && lightingMaterial->normalTexture && lightingMaterial->normalTexture->rendererTexture)
                                    section.normalTexture = lightingMaterial->normalTexture->rendererTexture->resourceView;
                                if (material && material->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap) {
                                    auto* glowMaterial = static_cast<RE::BSLightingShaderMaterialGlowmap*>(material);
                                    if (glowMaterial->glowTexture && glowMaterial->glowTexture->rendererTexture)
                                        section.glowTexture = glowMaterial->glowTexture->rendererTexture->resourceView;
                                }
                                if (shader->emissiveColor) {
                                    section.emissiveColor = { shader->emissiveColor->red,
                                        shader->emissiveColor->green, shader->emissiveColor->blue };
                                    section.emissiveMultiple = shader->emissiveMult;
                                }
                            }
                            for (const auto& swap : textureSwaps) {
                                if (swap.target != object) continue;
                                if (swap.textures[RE::BSTextureSet::Textures::kDiffuse])
                                    section.texture = swap.textures[RE::BSTextureSet::Textures::kDiffuse];
                                if (swap.textures[RE::BSTextureSet::Textures::kNormal])
                                    section.normalTexture = swap.textures[RE::BSTextureSet::Textures::kNormal];
                                if (swap.textures[RE::BSTextureSet::Textures::kSpecular])
                                    section.specularTexture = swap.textures[RE::BSTextureSet::Textures::kSpecular];
                                if (swap.textures[RE::BSTextureSet::Textures::kGlowMap])
                                    section.glowTexture = swap.textures[RE::BSTextureSet::Textures::kGlowMap];
                                if (swap.textures[RE::BSTextureSet::Textures::kEnvironmentMask])
                                    section.environmentMaskTexture = swap.textures[RE::BSTextureSet::Textures::kEnvironmentMask];
                                if (swap.textures[RE::BSTextureSet::Textures::kEnvironment])
                                    section.environmentTexture = swap.textures[RE::BSTextureSet::Textures::kEnvironment];
                            }
                            sections.push_back(std::move(section));
                        }
                    }
                }
            }
            if (auto* node = object->AsNode()) {
                for (const auto& child : node->GetChildren())
                    Gather(child.get(), transform, false, sections, textureSwaps);
            }
        }

        bool CompileShader(const char* source, const char* entry, const char* target,
            void** shaderBytecode, std::size_t* shaderSize)
        {
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            static auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
            if (!compile) return false;
            ID3DBlob* bytecode = nullptr;
            ID3DBlob* errors = nullptr;
            const auto result = compile(source, std::strlen(source), nullptr, nullptr, nullptr, entry,
                target, 0, 0, &bytecode, &errors);
            if (FAILED(result) || !bytecode) {
                if (errors) {
                    SKSE::log::error("SkyCounterUI off-screen shader compilation failed: {}",
                        static_cast<const char*>(errors->GetBufferPointer()));
                    errors->Release();
                }
                if (bytecode) bytecode->Release();
                return false;
            }
            if (errors) errors->Release();
            *shaderSize = bytecode->GetBufferSize();
            *shaderBytecode = std::malloc(*shaderSize);
            if (*shaderBytecode) std::memcpy(*shaderBytecode, bytecode->GetBufferPointer(), *shaderSize);
            bytecode->Release();
            return *shaderBytecode != nullptr;
        }

        bool EnsureResources(Device* device, Context* immediate)
        {
            if (device_ == device && immediate_ == immediate && target_ && deferred_) return true;
            Reset();
            device->AddRef(); immediate->AddRef();
            device_ = device; immediate_ = immediate;
            if (FAILED(device_->CreateDeferredContext(0, &deferred_)) || !deferred_) return false;

            const char* shader =
                "Texture2D itemTexture : register(t0); Texture2D glowTexture : register(t1); "
                "Texture2D normalTexture : register(t2); Texture2D specularTexture : register(t3); "
                "Texture2D effectMaskTexture : register(t4); Texture2D effectGradientTexture : register(t5); "
                "Texture2D environmentMaskTexture : register(t6); TextureCube environmentTexture : register(t7); "
                "Texture2D effectLightingTexture : register(t8); "
                "SamplerState itemSampler : register(s0);"
                "struct VSIn { float3 p:POSITION; float2 uv:TEXCOORD0; float4 c:COLOR0; float4 e:COLOR1; float3 n:NORMAL; float4 t:TANGENT; float4 s:COLOR2; float4 m:COLOR3; float4 f:COLOR4; float4 env:COLOR5; };"
                "struct VSOut { float4 p:SV_Position; float2 uv:TEXCOORD0; float4 c:COLOR0; float4 e:COLOR1; float3 n:TEXCOORD2; float4 t:TEXCOORD3; float4 s:TEXCOORD4; float4 m:TEXCOORD5; float4 f:TEXCOORD6; float4 env:TEXCOORD7; };"
                "VSOut VSMain(VSIn i){ VSOut o; o.p=float4(i.p,1); o.uv=i.uv; o.c=i.c; o.e=i.e; o.n=i.n; o.t=i.t; o.s=i.s; o.m=i.m; o.f=i.f; o.env=i.env; return o; }"
                "float4 PSMain(VSOut i):SV_Target { float4 c=itemTexture.Sample(itemSampler,i.uv)*i.c; "
                "if(i.f.x>0.5){ float gray=effectMaskTexture.Sample(itemSampler,i.uv).r; float4 ramp=effectGradientTexture.Sample(itemSampler,float2(gray,0.5)); c.rgb*=ramp.rgb*effectLightingTexture.Sample(itemSampler,i.uv).rgb; c.a*=ramp.a; } "
                "float alpha=c.a*i.m.y; if(i.f.z>0.5) clip(alpha-i.m.z); else clip(alpha-0.005); "
                "float3 n=normalize(i.n); float3 t=normalize(i.t.xyz-n*dot(n,i.t.xyz)); "
                "float3 b=normalize(cross(n,t)*i.t.w); float4 normalSample=normalTexture.Sample(itemSampler,i.uv); "
                "float3 mapN=normalSample.xyz*2.0-1.0; "
                "n=normalize(t*mapN.x+b*mapN.y+n*mapN.z); float3 light=normalize(float3(-0.38,0.55,0.74)); "
                // Use a bright neutral studio fill. A low ambient floor made
                // valid game textures appear almost black in the small icons.
                "float diffuse=0.82+0.18*saturate(dot(n,light)); float3 viewDir=float3(0,0,1); "
                "float specMask=(i.f.w>0.5?normalSample.a:specularTexture.Sample(itemSampler,i.uv).r); "
                "float exponent=max(i.m.x,2.0); float3 halfDir=normalize(light+viewDir); "
                "float3 spec=pow(saturate(dot(n,halfDir)),exponent)*specMask*i.s.rgb*i.s.a*i.m.w; "
                "float envMask=environmentMaskTexture.Sample(itemSampler,i.uv).r; "
                "float3 env=float3(0,0,0); if(i.env.z>0.5){ env=environmentTexture.Sample(itemSampler,reflect(-viewDir,n)).rgb*envMask*i.env.x*i.env.y*0.22; } "
                "float3 glow=(i.f.y>0.5?glowTexture.Sample(itemSampler,i.uv).rgb*i.e.rgb*i.e.a:float3(0,0,0)); "
                "float3 linearColor=saturate(c.rgb*diffuse+spec+glow+env); "
                "float3 srgbColor=lerp(linearColor*12.92,1.055*pow(linearColor,1.0/2.4)-0.055,step(0.0031308,linearColor)); "
                "return float4(srgbColor,alpha); }";
            void* vsCode = nullptr; std::size_t vsSize{};
            void* psCode = nullptr; std::size_t psSize{};
            const bool compiled = CompileShader(shader, "VSMain", "vs_4_0", &vsCode, &vsSize) &&
                CompileShader(shader, "PSMain", "ps_4_0", &psCode, &psSize);
            if (!compiled) { std::free(vsCode); std::free(psCode); Reset(); return false; }
            const auto vsResult = device_->CreateVertexShader(vsCode, vsSize, nullptr, &vertexShader_);
            const auto psResult = device_->CreatePixelShader(psCode, psSize, nullptr, &pixelShader_);
            const REX::W32::D3D11_INPUT_ELEMENT_DESC layout[] = {
                { "POSITION", 0, REX::W32::DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, REX::W32::DXGI_FORMAT_R32G32_FLOAT, 0, 12, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 1, REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "NORMAL", 0, REX::W32::DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TANGENT", 0, REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 2, REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 68, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 3, REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 84, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 4, REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 100, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 5, REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 116, REX::W32::D3D11_INPUT_PER_VERTEX_DATA, 0 }
            };
            const auto layoutResult = SUCCEEDED(vsResult) ? device_->CreateInputLayout(layout, 10, vsCode, vsSize, &inputLayout_) : E_FAIL;
            std::free(vsCode); std::free(psCode);
            if (FAILED(vsResult) || FAILED(psResult) || FAILED(layoutResult)) { Reset(); return false; }

            REX::W32::D3D11_TEXTURE2D_DESC colorDesc{};
            colorDesc.width = colorDesc.height = kOffscreenSize;
            colorDesc.mipLevels = colorDesc.arraySize = 1;
            colorDesc.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
            colorDesc.sampleDesc.count = 1;
            colorDesc.usage = REX::W32::D3D11_USAGE_DEFAULT;
            colorDesc.bindFlags = REX::W32::D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&colorDesc, nullptr, &target_)) ||
                FAILED(device_->CreateRenderTargetView(target_, nullptr, &rtv_))) { Reset(); return false; }
            auto depthDesc = colorDesc;
            depthDesc.format = REX::W32::DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDesc.bindFlags = REX::W32::D3D11_BIND_DEPTH_STENCIL;
            if (FAILED(device_->CreateTexture2D(&depthDesc, nullptr, &depthTexture_)) ||
                FAILED(device_->CreateDepthStencilView(depthTexture_, nullptr, &dsv_))) { Reset(); return false; }
            auto stagingDesc = colorDesc;
            stagingDesc.usage = REX::W32::D3D11_USAGE_STAGING;
            stagingDesc.bindFlags = 0;
            stagingDesc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&stagingDesc, nullptr, &staging_))) { Reset(); return false; }

            const REX::W32::D3D11_QUERY_DESC queryDesc{ REX::W32::D3D11_QUERY_EVENT, 0 };
            if (FAILED(device_->CreateQuery(&queryDesc, &query_))) { Reset(); return false; }

            REX::W32::D3D11_RASTERIZER_DESC raster{};
            raster.fillMode = REX::W32::D3D11_FILL_SOLID;
            raster.cullMode = REX::W32::D3D11_CULL_NONE;
            raster.depthClipEnable = TRUE;
            if (FAILED(device_->CreateRasterizerState(&raster, &rasterizer_))) { Reset(); return false; }
            REX::W32::D3D11_DEPTH_STENCIL_DESC depth{};
            depth.depthEnable = TRUE;
            depth.depthWriteMask = REX::W32::D3D11_DEPTH_WRITE_MASK_ALL;
            depth.depthFunc = REX::W32::D3D11_COMPARISON_LESS;
            if (FAILED(device_->CreateDepthStencilState(&depth, &depthState_))) { Reset(); return false; }
            REX::W32::D3D11_BLEND_DESC blend{};
            auto& target = blend.renderTarget[0];
            target.blendEnable = TRUE;
            target.srcBlend = REX::W32::D3D11_BLEND_SRC_ALPHA;
            target.destBlend = REX::W32::D3D11_BLEND_INV_SRC_ALPHA;
            target.blendOp = REX::W32::D3D11_BLEND_OP_ADD;
            target.srcBlendAlpha = REX::W32::D3D11_BLEND_ONE;
            target.destBlendAlpha = REX::W32::D3D11_BLEND_INV_SRC_ALPHA;
            target.blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD;
            target.renderTargetWriteMask = 0x0F;
            if (FAILED(device_->CreateBlendState(&blend, &blend_))) { Reset(); return false; }
            REX::W32::D3D11_SAMPLER_DESC sampler{};
            sampler.filter = REX::W32::D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.addressU = sampler.addressV = sampler.addressW = REX::W32::D3D11_TEXTURE_ADDRESS_WRAP;
            sampler.maxAnisotropy = 1;
            sampler.comparisonFunc = REX::W32::D3D11_COMPARISON_ALWAYS;
            sampler.minLOD = 0.0f;
            sampler.maxLOD = (std::numeric_limits<float>::max)();
            if (FAILED(device_->CreateSamplerState(&sampler, &sampler_))) { Reset(); return false; }

            const std::uint32_t white = 0xFFFFFFFFu;
            const std::uint32_t black = 0xFF000000u;
            const std::uint32_t flatNormal = 0xFFFF8080u;
            REX::W32::D3D11_TEXTURE2D_DESC whiteDesc{};
            whiteDesc.width = whiteDesc.height = whiteDesc.mipLevels = whiteDesc.arraySize = 1;
            whiteDesc.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
            whiteDesc.sampleDesc.count = 1;
            whiteDesc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
            whiteDesc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            const REX::W32::D3D11_SUBRESOURCE_DATA whiteData{ &white, 4, 4 };
            Texture2D* whiteTexture = nullptr;
            if (FAILED(device_->CreateTexture2D(&whiteDesc, &whiteData, &whiteTexture)) ||
                FAILED(device_->CreateShaderResourceView(whiteTexture, nullptr, &whiteTexture_))) {
                Release(whiteTexture); Reset(); return false;
            }
            Release(whiteTexture);
            const REX::W32::D3D11_SUBRESOURCE_DATA blackData{ &black, 4, 4 };
            Texture2D* blackTexture = nullptr;
            if (FAILED(device_->CreateTexture2D(&whiteDesc, &blackData, &blackTexture)) ||
                FAILED(device_->CreateShaderResourceView(blackTexture, nullptr, &blackTexture_))) {
                Release(blackTexture); Reset(); return false;
            }
            Release(blackTexture);
            const REX::W32::D3D11_SUBRESOURCE_DATA flatNormalData{ &flatNormal, 4, 4 };
            Texture2D* flatNormalTexture = nullptr;
            if (FAILED(device_->CreateTexture2D(&whiteDesc, &flatNormalData, &flatNormalTexture)) ||
                FAILED(device_->CreateShaderResourceView(flatNormalTexture, nullptr, &flatNormalTexture_))) {
                Release(flatNormalTexture); Reset(); return false;
            }
            Release(flatNormalTexture);
            return true;
        }

        OffscreenPoll Finish(OffscreenPoll result)
        {
            pending_ = false;
            formID_ = 0;
            path_.clear();
            waitedFrames_ = 0;
            Release(vertexBuffer_); Release(indexBuffer_);
            return result;
        }

        void Reset()
        {
            pending_ = false;
            for (auto& [key, texture] : textureCache_) Release(texture);
            textureCache_.clear();
            failedTextureCache_.clear();
            Release(vertexBuffer_); Release(indexBuffer_);
            Release(whiteTexture_); Release(blackTexture_); Release(flatNormalTexture_); Release(sampler_); Release(blend_); Release(depthState_);
            Release(rasterizer_); Release(query_); Release(staging_); Release(dsv_); Release(depthTexture_);
            Release(rtv_); Release(target_); Release(inputLayout_); Release(vertexShader_); Release(pixelShader_);
            Release(deferred_); Release(immediate_); Release(device_);
        }

        Device* device_{};
        Context* immediate_{};
        Context* deferred_{};
        Texture2D* target_{};
        Texture2D* depthTexture_{};
        Texture2D* staging_{};
        Rtv* rtv_{};
        Dsv* dsv_{};
        Buffer* vertexBuffer_{};
        Buffer* indexBuffer_{};
        VertexShader* vertexShader_{};
        PixelShader* pixelShader_{};
        InputLayout* inputLayout_{};
        BlendState* blend_{};
        DepthStencilState* depthState_{};
        RasterizerState* rasterizer_{};
        SamplerState* sampler_{};
        ShaderResourceView* whiteTexture_{};
        ShaderResourceView* blackTexture_{};
        ShaderResourceView* flatNormalTexture_{};
        std::unordered_map<std::string, ShaderResourceView*> textureCache_;
        std::unordered_set<std::string> failedTextureCache_;
        Query* query_{};
        RE::FormID formID_{};
        std::filesystem::path path_;
        std::uint32_t waitedFrames_{};
        bool pending_{};
    } g_offscreenRenderer;

    // Uses Skyrim's Inventory3DManager draw path on the active game render
    // surface. Each pass saves and restores the full frame before presentation,
    // while staging the preview pixels for the icon writer.
    class Inventory3DPreviewCapture final {
    public:
        ~Inventory3DPreviewCapture() { Reset(); }

        bool Submit(RE::Inventory3DManager* manager, RE::FormID formID,
            const std::filesystem::path& path, Device* device, Context* immediate,
            bool whitePass, std::string& failureReason)
        {
            if (pending_ || !manager || !formID || path.empty() || !device || !immediate) {
                failureReason = "capture is pending or required renderer inputs are unavailable";
                return false;
            }

            if ((!whitePass && passStarted_) ||
                (whitePass && (!passStarted_ || formID_ != formID || path_ != path))) {
                failureReason = "the inventory preview matte passes are out of sequence";
                return false;
            }

            auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
            Rtv* sourceView{};
            Texture2D* source{};
            bool sourceWasBound{};
            if (!AcquireRenderSurface(immediate, renderer, sourceView, source, sourceWasBound)) {
                failureReason = "Skyrim's active render surface is unavailable";
                return false;
            }

            REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};
            source->GetDesc(&sourceDesc);
            if (!EnsureResources(device, sourceDesc, &failureReason)) {
                Release(sourceView);
                Release(source);
                if (failureReason.empty()) {
                    failureReason = "could not create frame-preserving Inventory3DManager capture resources";
                }
                return false;
            }

            const float matte[4] = { whitePass ? 1.0f : 0.0f,
                whitePass ? 1.0f : 0.0f, whitePass ? 1.0f : 0.0f, 0.0f };
            bool copied = false;
            {
                ContextOutputState savedOutput(immediate);
                Texture2D* depthTexture{};
                if (!AcquireDepthTexture(savedOutput.DepthTarget(), depthTexture)) {
                    failureReason = "Skyrim's active depth view is not backed by a readable texture";
                    Release(sourceView);
                    Release(source);
                    return false;
                }
                if (depthTexture && !EnsureDepthBackup(device, depthTexture, &failureReason)) {
                    Release(depthTexture);
                    Release(sourceView);
                    Release(source);
                    if (failureReason.empty()) {
                        failureReason = "could not preserve Skyrim's active depth target";
                    }
                    return false;
                }
                static bool loggedOutputState = false;
                if (!loggedOutputState) {
                    loggedOutputState = true;
                    SKSE::log::info("SkyCounterUI Inventory3DManager capture source: {}; sourceView=0x{:X}, sourceTexture=0x{:X}, descriptor={}x{} format={} samples={} quality={}; D3D state has {} render target(s), depth {}, and {} viewport(s)",
                        sourceWasBound ? "currently bound render target" : "swap-chain fallback",
                        reinterpret_cast<std::uintptr_t>(sourceView),
                        reinterpret_cast<std::uintptr_t>(source),
                        sourceDesc.width, sourceDesc.height,
                        static_cast<std::uint32_t>(sourceDesc.format),
                        sourceDesc.sampleDesc.count, sourceDesc.sampleDesc.quality,
                        savedOutput.RenderTargetCount(), savedOutput.HasDepthTarget() ? "bound" : "no depth target",
                        savedOutput.ViewportCount());
                    for (std::uint32_t outputIndex = 0; outputIndex < savedOutput.RenderTargetCount(); ++outputIndex) {
                        auto* outputView = savedOutput.RenderTarget(outputIndex);
                        REX::W32::ID3D11Resource* outputResource{};
                        if (outputView) outputView->GetResource(&outputResource);
                        const auto outputResourceAddress = reinterpret_cast<std::uintptr_t>(outputResource);
                        const auto outputViewAddress = reinterpret_cast<std::uintptr_t>(outputView);
                        std::string engineTargets;
                        if (renderer) {
                            for (std::uint32_t engineIndex = 0;
                                 engineIndex < static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL);
                                 ++engineIndex) {
                                const auto& engineTarget = renderer->renderTargets[engineIndex];
                                const bool viewMatches = engineTarget.RTV &&
                                    reinterpret_cast<std::uintptr_t>(engineTarget.RTV) == outputViewAddress;
                                const bool textureMatches = engineTarget.texture &&
                                    reinterpret_cast<std::uintptr_t>(engineTarget.texture) == outputResourceAddress;
                                if (viewMatches || textureMatches) {
                                    if (!engineTargets.empty()) engineTargets += ",";
                                    static constexpr const char* targetNames[] = {
                                        "FRAMEBUFFER", "MAIN", "MAIN_COPY", "MAIN_ONLY_ALPHA",
                                        "NORMAL_TAAMASK_SSRMASK", "NORMAL_TAAMASK_SSRMASK_SWAP"
                                    };
                                    engineTargets += std::to_string(engineIndex);
                                    engineTargets += ":";
                                    engineTargets += engineIndex < (sizeof(targetNames) / sizeof(targetNames[0])) ?
                                        targetNames[engineIndex] : "other";
                                }
                            }
                        }
                        REX::W32::D3D11_TEXTURE2D_DESC outputDesc{};
                        bool hasOutputDesc = false;
                        if (outputResource) {
                            Texture2D* outputTexture{};
                            if (SUCCEEDED(outputResource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                    reinterpret_cast<void**>(&outputTexture))) && outputTexture) {
                                outputTexture->GetDesc(&outputDesc);
                                hasOutputDesc = true;
                                outputTexture->Release();
                            }
                            outputResource->Release();
                        }
                        const auto* viewport = outputIndex < savedOutput.ViewportCount() ?
                            savedOutput.Viewport(outputIndex) : nullptr;
                        SKSE::log::info("SkyCounterUI capture output[{}]: RTV=0x{:X}, resource=0x{:X}, engine render-target slots [{}], descriptor={}x{} format={} samples={}, viewport={} ({:.1f},{:.1f} {:.1f}x{:.1f})",
                            outputIndex, outputViewAddress, outputResourceAddress,
                            engineTargets.empty() ? "no pointer match" : engineTargets,
                            hasOutputDesc ? outputDesc.width : 0,
                            hasOutputDesc ? outputDesc.height : 0,
                            hasOutputDesc ? static_cast<std::uint32_t>(outputDesc.format) : 0,
                            hasOutputDesc ? outputDesc.sampleDesc.count : 0,
                            viewport ? "present" : "missing",
                            viewport ? viewport->topLeftX : 0.0f,
                            viewport ? viewport->topLeftY : 0.0f,
                            viewport ? viewport->width : 0.0f,
                            viewport ? viewport->height : 0.0f);
                    }
                }
                // Inventory3DManager draws its UI scene using the currently
                // bound depth target. At this point Skyrim has already filled
                // that target with world depth, which can reject every preview
                // fragment. Snapshot color and depth while unbound, clear the
                // inherited depth, draw the preview, then restore both before
                // the game frame is presented.
                savedOutput.UnbindTargets();
                immediate->CopySubresourceRegion(savedFrame_, 0, 0, 0, 0, source, 0, nullptr);
                if (depthTexture) immediate->CopyResource(savedDepth_, depthTexture);
                if (!sourceWasBound) {
                    savedOutput.BindCaptureTarget(sourceView);
                } else {
                    savedOutput.BindTargets();
                }
                ClearRenderSurface(immediate, sourceView, matte);
                if (savedOutput.DepthTarget()) {
                    immediate->ClearDepthStencilView(savedOutput.DepthTarget(),
                        REX::W32::D3D11_CLEAR_DEPTH, 1.0f, 0);
                }
                manager->Render();
                if (sourceWasBound && !renderStateAfterFirstPassLogged_) {
                    renderStateAfterFirstPassLogged_ = true;
                    ContextOutputState afterRender(immediate);
                    SKSE::log::info("SkyCounterUI Inventory3DManager post-Render output state: {} render target(s), first RTV=0x{:X}, source RTV=0x{:X} (same={}), {} viewport(s)",
                        afterRender.RenderTargetCount(),
                        reinterpret_cast<std::uintptr_t>(afterRender.RenderTarget(0)),
                        reinterpret_cast<std::uintptr_t>(sourceView),
                        afterRender.RenderTarget(0) == sourceView,
                        afterRender.ViewportCount());
                    for (std::uint32_t outputIndex = 0; outputIndex < afterRender.RenderTargetCount(); ++outputIndex) {
                        auto* outputView = afterRender.RenderTarget(outputIndex);
                        std::string engineTargets;
                        if (renderer) {
                            for (std::uint32_t engineIndex = 0;
                                 engineIndex < static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL);
                                 ++engineIndex) {
                                if (renderer->renderTargets[engineIndex].RTV == outputView) {
                                    if (!engineTargets.empty()) engineTargets += ",";
                                    engineTargets += std::to_string(engineIndex);
                                }
                            }
                        }
                        SKSE::log::info("SkyCounterUI post-Render output[{}]: RTV=0x{:X}, same as capture source={}, engine RTV slots [{}]",
                            outputIndex, reinterpret_cast<std::uintptr_t>(outputView),
                            outputView == sourceView,
                            engineTargets.empty() ? "no pointer match" : engineTargets);
                    }
                }
                // The destination of the frame-restore copy is the render
                // surface itself. Unbind all output-merger targets first, then
                // restore the exact RTV/DSV/viewport state through the RAII
                // snapshot. This avoids copying into a texture still bound as
                // an output and prevents a stale/unbound renderer state.
                savedOutput.UnbindTargets();
                copied = CopyRenderSurfaceToStaging(immediate, source,
                    whitePass ? whiteStaging_ : blackStaging_);
                immediate->CopySubresourceRegion(source, 0, 0, 0, 0, savedFrame_, 0, nullptr);
                if (depthTexture) immediate->CopyResource(depthTexture, savedDepth_);
                Release(depthTexture);
            }
            Release(sourceView);
            Release(source);
            if (!copied) {
                passStarted_ = false;
                failureReason = "Skyrim's active render surface could not be copied for readback";
                return false;
            }

            if (!whitePass) {
                formID_ = formID;
                path_ = path;
                passStarted_ = true;
                return true;
            }

            immediate->End(query_);
            immediate_ = immediate;
            waitedFrames_ = 0;
            pending_ = true;
            passStarted_ = false;
            failureReason.clear();
            return true;
        }

        bool Pump(RE::Inventory3DManager* manager, Device* device, Context* immediate)
        {
            if (pending_ || !manager || !device || !immediate) return false;
            auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
            Rtv* sourceView{};
            Texture2D* source{};
            bool sourceWasBound{};
            if (!AcquireRenderSurface(immediate, renderer, sourceView, source, sourceWasBound)) return false;
            REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};
            source->GetDesc(&sourceDesc);
            if (!EnsureResources(device, sourceDesc)) {
                Release(sourceView);
                Release(source);
                return false;
            }
            {
                ContextOutputState savedOutput(immediate);
                Texture2D* depthTexture{};
                if (!AcquireDepthTexture(savedOutput.DepthTarget(), depthTexture) ||
                    (depthTexture && !EnsureDepthBackup(device, depthTexture))) {
                    Release(depthTexture);
                    Release(sourceView);
                    Release(source);
                    return false;
                }
                savedOutput.UnbindTargets();
                immediate->CopySubresourceRegion(savedFrame_, 0, 0, 0, 0, source, 0, nullptr);
                if (depthTexture) immediate->CopyResource(savedDepth_, depthTexture);
                if (!sourceWasBound) {
                    savedOutput.BindCaptureTarget(sourceView);
                } else {
                    savedOutput.BindTargets();
                }
                if (savedOutput.DepthTarget()) {
                    immediate->ClearDepthStencilView(savedOutput.DepthTarget(),
                        REX::W32::D3D11_CLEAR_DEPTH, 1.0f, 0);
                }
                manager->Render();
                // Model loading also advances through Render(). Restore the
                // frame around these unpaused pumps to prevent a preview flash.
                savedOutput.UnbindTargets();
                immediate->CopySubresourceRegion(source, 0, 0, 0, 0, savedFrame_, 0, nullptr);
                if (depthTexture) immediate->CopyResource(depthTexture, savedDepth_);
                Release(depthTexture);
            }
            Release(sourceView);
            Release(source);
            return true;
        }

        // Allocate every capture surface before the short pause starts. If
        // the active swap-chain format is unsupported, fail here while the
        // game is still running instead of briefly pausing and blinking.
        bool Prepare(Device* device, Context* immediate, std::string& failureReason)
        {
            if (!device || !immediate) {
                failureReason = "Skyrim's D3D11 device or immediate context is unavailable";
                return false;
            }
            auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
            Rtv* sourceView{};
            Texture2D* source{};
            bool sourceWasBound{};
            if (!AcquireRenderSurface(immediate, renderer, sourceView, source, sourceWasBound)) {
                failureReason = "Skyrim's active render surface is unavailable";
                return false;
            }
            REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};
            source->GetDesc(&sourceDesc);
            bool ready = EnsureResources(device, sourceDesc, &failureReason);
            if (ready) {
                ContextOutputState savedOutput(immediate);
                Texture2D* depthTexture{};
                if (!AcquireDepthTexture(savedOutput.DepthTarget(), depthTexture)) {
                    failureReason = "Skyrim's active depth view is not backed by a readable texture";
                    ready = false;
                } else if (depthTexture && !EnsureDepthBackup(device, depthTexture, &failureReason)) {
                    if (failureReason.empty()) {
                        failureReason = "could not preserve Skyrim's active depth target";
                    }
                    ready = false;
                }
                Release(depthTexture);
            }
            Release(sourceView);
            Release(source);
            return ready;
        }

        OffscreenPoll Poll(PixelJob& job)
        {
            if (!pending_) return OffscreenPoll::Idle;
            job.formID = formID_;
            job.path = path_;
            const auto queryResult = immediate_->GetData(query_, nullptr, 0,
                REX::W32::D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (queryResult == S_FALSE) {
                if (++waitedFrames_ < 180) return OffscreenPoll::Pending;
                SKSE::log::warn("SkyCounterUI GPU readback event query timed out for {:08X} after {} render frames",
                    formID_, waitedFrames_);
                return Finish(OffscreenPoll::Failed);
            }
            if (FAILED(queryResult)) {
                const auto deviceResult = device_ ? device_->GetDeviceRemovedReason() : E_POINTER;
                SKSE::log::warn("SkyCounterUI GPU readback event query failed for {:08X} (query HRESULT 0x{:08X}, device HRESULT 0x{:08X})",
                    formID_, static_cast<std::uint32_t>(queryResult),
                    static_cast<std::uint32_t>(deviceResult));
                return Finish(OffscreenPoll::Failed);
            }

            REX::W32::D3D11_MAPPED_SUBRESOURCE blackMap{};
            const auto blackMapResult = immediate_->Map(blackStaging_, 0, REX::W32::D3D11_MAP_READ,
                REX::W32::D3D11_MAP_FLAG_DO_NOT_WAIT, &blackMap);
            if (FAILED(blackMapResult)) {
                if (++waitedFrames_ < 180) return OffscreenPoll::Pending;
                SKSE::log::warn("SkyCounterUI black-matte staging Map timed out for {:08X} after {} frames (HRESULT 0x{:08X})",
                    formID_, waitedFrames_, static_cast<std::uint32_t>(blackMapResult));
                return Finish(OffscreenPoll::Failed);
            }
            REX::W32::D3D11_MAPPED_SUBRESOURCE whiteMap{};
            const auto whiteMapResult = immediate_->Map(whiteStaging_, 0, REX::W32::D3D11_MAP_READ,
                REX::W32::D3D11_MAP_FLAG_DO_NOT_WAIT, &whiteMap);
            if (FAILED(whiteMapResult)) {
                immediate_->Unmap(blackStaging_, 0);
                if (++waitedFrames_ < 180) return OffscreenPoll::Pending;
                SKSE::log::warn("SkyCounterUI white-matte staging Map timed out for {:08X} after {} frames (HRESULT 0x{:08X})",
                    formID_, waitedFrames_, static_cast<std::uint32_t>(whiteMapResult));
                return Finish(OffscreenPoll::Failed);
            }

            const auto* blackPixels = static_cast<const std::uint8_t*>(blackMap.data);
            const auto* whitePixels = static_cast<const std::uint8_t*>(whiteMap.data);
            std::uint32_t minX = width_, minY = height_, maxX = 0, maxY = 0;
            std::uint64_t blackNonMattePixels = 0;
            std::uint64_t whiteNonMattePixels = 0;
            std::uint64_t alphaCandidatePixels = 0;
            std::uint8_t maximumMatteAlpha = 0;
            bool foundPixel = false;
            for (std::uint32_t y = 0; y < height_; ++y) {
                const auto* blackRow = blackPixels + static_cast<std::size_t>(y) * blackMap.rowPitch;
                const auto* whiteRow = whitePixels + static_cast<std::size_t>(y) * whiteMap.rowPitch;
                for (std::uint32_t x = 0; x < width_; ++x) {
                    const auto* blackPixel = blackRow + static_cast<std::size_t>(x) * 4;
                    const auto* whitePixel = whiteRow + static_cast<std::size_t>(x) * 4;
                    const auto b = ReadCapturePixel(blackPixel, format_);
                    const auto w = ReadCapturePixel(whitePixel, format_);
                    if (b[0] > 12 || b[1] > 12 || b[2] > 12) ++blackNonMattePixels;
                    if (w[0] < 243 || w[1] < 243 || w[2] < 243) ++whiteNonMattePixels;
                    const auto alpha = MatteAlpha(b, w);
                    if (alpha > maximumMatteAlpha) maximumMatteAlpha = alpha;
                    if (alpha <= 8) continue;
                    ++alphaCandidatePixels;
                    foundPixel = true;
                    minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
                    minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
                }
            }

            if (foundPixel) {
                const std::uint32_t boxSize = (std::max)(maxX - minX + 1, maxY - minY + 1);
                const auto centerX = (minX + maxX) / 2;
                const auto centerY = (minY + maxY) / 2;
                const auto left = static_cast<std::int64_t>(centerX) - boxSize / 2;
                const auto top = static_cast<std::int64_t>(centerY) - boxSize / 2;
                for (std::uint32_t y = 0; y < kOutputSize; ++y) {
                    for (std::uint32_t x = 0; x < kOutputSize; ++x) {
                        const auto sx = left + static_cast<std::int64_t>((static_cast<std::uint64_t>(x) * boxSize) / kOutputSize);
                        const auto sy = top + static_cast<std::int64_t>((static_cast<std::uint64_t>(y) * boxSize) / kOutputSize);
                        const auto dst = (static_cast<std::size_t>(y) * kOutputSize + x) * 4;
                        if (sx < 0 || sy < 0 || sx >= width_ || sy >= height_) continue;
                        const auto* blackPixel = blackPixels + static_cast<std::size_t>(sy) * blackMap.rowPitch + static_cast<std::size_t>(sx) * 4;
                        const auto* whitePixel = whitePixels + static_cast<std::size_t>(sy) * whiteMap.rowPitch + static_cast<std::size_t>(sx) * 4;
                        const auto b = ReadCapturePixel(blackPixel, format_);
                        const auto w = ReadCapturePixel(whitePixel, format_);
                        const auto alpha = MatteAlpha(b, w);
                        if (alpha <= 8) continue;
                        for (std::uint32_t channel = 0; channel < 3; ++channel) {
                            const auto sourceChannel = IsBgraFormat(format_) ? 2u - channel : channel;
                            const auto premultiplied = static_cast<std::uint32_t>(b[sourceChannel]);
                            job.bgra[dst + (2u - channel)] = static_cast<std::uint8_t>(
                                (std::min)(255u, (premultiplied * 255u + alpha / 2u) / alpha));
                        }
                        job.bgra[dst + 3] = alpha;
                    }
                }
            }
            if (!foundPixel) {
                const auto logSample = [&](const char* label, std::uint32_t x, std::uint32_t y) {
                    const auto* blackPixel = blackPixels + static_cast<std::size_t>(y) * blackMap.rowPitch +
                        static_cast<std::size_t>(x) * 4;
                    const auto* whitePixel = whitePixels + static_cast<std::size_t>(y) * whiteMap.rowPitch +
                        static_cast<std::size_t>(x) * 4;
                    const auto b = ReadCapturePixel(blackPixel, format_);
                    const auto w = ReadCapturePixel(whitePixel, format_);
                    SKSE::log::warn("SkyCounterUI capture pixel sample {} at ({},{}): black=({}, {}, {}, {}), white=({}, {}, {}, {}), derived alpha={}",
                        label, x, y, b[0], b[1], b[2], b[3], w[0], w[1], w[2], w[3], MatteAlpha(b, w));
                };
                SKSE::log::warn("SkyCounterUI Inventory3DManager produced no visible pixels for {:08X}: {}x{} format {}, black-pass non-matte pixels={}, white-pass non-matte pixels={}, alpha candidates={}, max derived alpha={}; expected black/white matte clear values are RGB 0/255",
                    formID_, width_, height_, static_cast<std::uint32_t>(format_),
                    blackNonMattePixels, whiteNonMattePixels, alphaCandidatePixels,
                    maximumMatteAlpha);
                logSample("top-left", 0, 0);
                logSample("center", width_ / 2, height_ / 2);
                logSample("bottom-right", width_ - 1, height_ - 1);
                immediate_->Unmap(whiteStaging_, 0);
                immediate_->Unmap(blackStaging_, 0);
                return Finish(OffscreenPoll::Failed);
            }
            immediate_->Unmap(whiteStaging_, 0);
            immediate_->Unmap(blackStaging_, 0);
            return Finish(OffscreenPoll::Ready);
        }

        bool IsPending() const noexcept { return pending_; }

        void CancelPass() noexcept
        {
            if (pending_) return;
            passStarted_ = false;
            formID_ = 0;
            path_.clear();
        }

    private:
        template <class T>
        static void Release(T*& value)
        {
            if (value) value->Release();
            value = nullptr;
        }

        class ContextOutputState final {
        public:
            explicit ContextOutputState(Context* context) : context_(context)
            {
                if (!context_) return;
                context_->OMGetRenderTargets(static_cast<std::uint32_t>(renderTargets_.size()),
                    renderTargets_.data(), &depthView_);
                for (std::size_t i = 0; i < renderTargets_.size(); ++i) {
                    if (renderTargets_[i]) renderTargetCount_ = static_cast<std::uint32_t>(i + 1);
                }
                viewportCount_ = static_cast<std::uint32_t>(viewports_.size());
                context_->RSGetViewports(&viewportCount_, viewports_.data());
            }

            ContextOutputState(const ContextOutputState&) = delete;
            ContextOutputState& operator=(const ContextOutputState&) = delete;
            ~ContextOutputState() { Restore(); }

            void Restore()
            {
                if (!context_) return;
                if (targetStateChanged_) BindTargets();
                for (auto*& target : renderTargets_) Release(target);
                Release(depthView_);
                context_ = nullptr;
            }

            void UnbindTargets()
            {
                if (context_) {
                    context_->OMSetRenderTargets(0, nullptr, nullptr);
                    targetStateChanged_ = true;
                }
            }

            void BindTargets()
            {
                if (!context_) return;
                context_->OMSetRenderTargets(renderTargetCount_, renderTargets_.data(), depthView_);
                context_->RSSetViewports(viewportCount_, viewports_.data());
            }

            [[nodiscard]] std::uint32_t RenderTargetCount() const { return renderTargetCount_; }
            [[nodiscard]] bool HasDepthTarget() const { return depthView_ != nullptr; }
            [[nodiscard]] Dsv* DepthTarget() const { return depthView_; }
            [[nodiscard]] std::uint32_t ViewportCount() const { return viewportCount_; }
            [[nodiscard]] Rtv* RenderTarget(std::uint32_t index) const
            {
                return index < renderTargets_.size() ? renderTargets_[index] : nullptr;
            }
            [[nodiscard]] const REX::W32::D3D11_VIEWPORT* Viewport(std::uint32_t index) const
            {
                return index < viewportCount_ ? &viewports_[index] : nullptr;
            }

            void BindCaptureTarget(Rtv* sourceView)
            {
                if (!context_) return;
                // Bind the texture selected by AcquireRenderSurface. Restoring
                // the previously captured MRT array here could bind an
                // unrelated target and make the capture clear/draw invisible.
                context_->OMSetRenderTargets(1, &sourceView, depthView_);
                context_->RSSetViewports(viewportCount_, viewports_.data());
                targetStateChanged_ = true;
            }

        private:
            Context* context_{};
            std::array<Rtv*, 8> renderTargets_{};
            Dsv* depthView_{};
            std::array<REX::W32::D3D11_VIEWPORT, 16> viewports_{};
            std::uint32_t renderTargetCount_{};
            std::uint32_t viewportCount_{};
            bool targetStateChanged_{};
        };

        static bool AcquireRenderSurface(Context* immediate,
            RE::BSGraphics::RendererData* renderer, Rtv*& sourceView,
            Texture2D*& source, bool& sourceWasBound)
        {
            sourceView = nullptr;
            source = nullptr;
            sourceWasBound = false;
            immediate->OMGetRenderTargets(1, &sourceView, nullptr);
            sourceWasBound = sourceView != nullptr;
            if (!sourceView && renderer && renderer->renderWindows[0].renderView) {
                sourceView = reinterpret_cast<Rtv*>(renderer->renderWindows[0].renderView);
                sourceView->AddRef();
            }
            if (!sourceView) return false;

            REX::W32::ID3D11Resource* resource{};
            sourceView->GetResource(&resource);
            if (!resource) {
                Release(sourceView);
                return false;
            }
            const auto result = resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                reinterpret_cast<void**>(&source));
            resource->Release();
            if (FAILED(result) || !source) {
                Release(sourceView);
                Release(source);
                return false;
            }
            return true;
        }

        static bool AcquireDepthTexture(Dsv* depthView, Texture2D*& depthTexture)
        {
            depthTexture = nullptr;
            if (!depthView) return true;
            REX::W32::ID3D11Resource* resource{};
            depthView->GetResource(&resource);
            if (!resource) return false;
            const auto result = resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                reinterpret_cast<void**>(&depthTexture));
            resource->Release();
            if (FAILED(result) || !depthTexture) {
                Release(depthTexture);
                return false;
            }
            return true;
        }

        static void ClearRenderSurface(Context* immediate, Rtv* sourceView,
            const float color[4])
        {
            REX::W32::ID3D11DeviceContext1* context1{};
            if (SUCCEEDED(immediate->QueryInterface(REX::W32::IID_ID3D11DeviceContext1,
                    reinterpret_cast<void**>(&context1))) && context1) {
                context1->ClearView(sourceView, color, nullptr, 0);
                context1->Release();
            } else {
                immediate->ClearRenderTargetView(sourceView, color);
            }
        }

        bool CopyRenderSurfaceToStaging(Context* immediate, Texture2D* source,
            Texture2D* staging)
        {
            if (sampleCount_ > 1) {
                if (!resolveFrame_) return false;
                immediate->ResolveSubresource(resolveFrame_, 0, source, 0, format_);
                immediate->CopySubresourceRegion(staging, 0, 0, 0, 0, resolveFrame_, 0, nullptr);
            } else {
                immediate->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, nullptr);
            }
            return true;
        }

        static bool IsBgraFormat(REX::W32::DXGI_FORMAT format)
        {
            const auto value = static_cast<std::uint32_t>(format);
            return value == 87u || value == 91u;  // B8G8R8A8_UNORM[_SRGB]
        }

        static std::array<std::uint8_t, 4> ReadCapturePixel(
            const std::uint8_t* pixel, REX::W32::DXGI_FORMAT format)
        {
            if (static_cast<std::uint32_t>(format) == 24u) {
                std::uint32_t packed{};
                std::memcpy(&packed, pixel, sizeof(packed));
                const auto toByte = [](std::uint32_t value, std::uint32_t maxValue) {
                    return static_cast<std::uint8_t>((value * 255u + maxValue / 2u) / maxValue);
                };
                return { toByte(packed & 0x3FFu, 0x3FFu),
                    toByte((packed >> 10) & 0x3FFu, 0x3FFu),
                    toByte((packed >> 20) & 0x3FFu, 0x3FFu),
                    toByte((packed >> 30) & 0x3u, 0x3u) };
            }
            return { pixel[0], pixel[1], pixel[2], pixel[3] };
        }

        static std::uint8_t MatteAlpha(const std::array<std::uint8_t, 4>& black,
            const std::array<std::uint8_t, 4>& white)
        {
            const auto channelDelta = [](std::uint8_t a, std::uint8_t b) {
                return static_cast<std::uint32_t>(a > b ? a - b : b - a);
            };
            const auto delta = (channelDelta(black[0], white[0]) +
                channelDelta(black[1], white[1]) + channelDelta(black[2], white[2]) + 1u) / 3u;
            return static_cast<std::uint8_t>(delta >= 255u ? 0u : 255u - delta);
        }

        bool EnsureResources(Device* device,
            const REX::W32::D3D11_TEXTURE2D_DESC& sourceDesc,
            std::string* failureReason = nullptr)
        {
            const auto formatValue = static_cast<std::uint32_t>(sourceDesc.format);
            const bool supportedFormat = formatValue == 24u || formatValue == 28u || formatValue == 29u ||
                formatValue == 87u || formatValue == 91u;
            const auto fail = [&](const char* stage, HRESULT result) {
                if (failureReason) {
                    *failureReason = stage;
                    SKSE::log::warn("SkyCounterUI frame-preserving capture resource setup failed at {} (HRESULT 0x{:08X}); source {}x{}, mips {}, slices {}, format {}, samples {}, quality {}, usage {}, bind {}, CPU {}, misc {}",
                        stage, static_cast<std::uint32_t>(result), sourceDesc.width,
                        sourceDesc.height, sourceDesc.mipLevels, sourceDesc.arraySize,
                        formatValue, sourceDesc.sampleDesc.count, sourceDesc.sampleDesc.quality,
                        static_cast<std::uint32_t>(sourceDesc.usage), sourceDesc.bindFlags,
                        sourceDesc.cpuAccessFlags, sourceDesc.miscFlags);
                }
                return false;
            };
            if (!supportedFormat || !sourceDesc.width || !sourceDesc.height ||
                !sourceDesc.sampleDesc.count) {
                return fail("invalid or unsupported source target descriptor", E_INVALIDARG);
            }
            if (device_ == device && savedFrame_ && blackStaging_ && whiteStaging_ && query_ &&
                (sourceDesc.sampleDesc.count <= 1 || resolveFrame_) &&
                width_ == sourceDesc.width && height_ == sourceDesc.height &&
                format_ == sourceDesc.format && sampleCount_ == sourceDesc.sampleDesc.count) return true;
            Reset();
            device_ = device;
            device_->AddRef();

            REX::W32::D3D11_TEXTURE2D_DESC scratchDesc{};
            scratchDesc.width = sourceDesc.width;
            scratchDesc.height = sourceDesc.height;
            scratchDesc.mipLevels = 1;
            scratchDesc.arraySize = 1;
            scratchDesc.format = sourceDesc.format;
            scratchDesc.sampleDesc = sourceDesc.sampleDesc;
            if (scratchDesc.sampleDesc.count == 1) scratchDesc.sampleDesc.quality = 0;
            scratchDesc.usage = REX::W32::D3D11_USAGE_DEFAULT;
            scratchDesc.bindFlags = 0;
            scratchDesc.cpuAccessFlags = 0;
            scratchDesc.miscFlags = 0;
            auto result = device_->CreateTexture2D(&scratchDesc, nullptr, &savedFrame_);
            if (FAILED(result)) {
                fail("CreateTexture2D(saved frame)", result);
                Reset();
                return false;
            }

            auto stagingDesc = scratchDesc;
            stagingDesc.sampleDesc.count = 1;
            stagingDesc.sampleDesc.quality = 0;
            stagingDesc.usage = REX::W32::D3D11_USAGE_STAGING;
            stagingDesc.bindFlags = 0;
            stagingDesc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;
            stagingDesc.miscFlags = 0;
            result = device_->CreateTexture2D(&stagingDesc, nullptr, &blackStaging_);
            if (FAILED(result)) {
                fail("CreateTexture2D(black staging)", result);
                Reset();
                return false;
            }
            result = device_->CreateTexture2D(&stagingDesc, nullptr, &whiteStaging_);
            if (FAILED(result)) {
                fail("CreateTexture2D(white staging)", result);
                Reset();
                return false;
            }
            if (sourceDesc.sampleDesc.count > 1) {
                auto resolveDesc = scratchDesc;
                resolveDesc.sampleDesc.count = 1;
                resolveDesc.sampleDesc.quality = 0;
                result = device_->CreateTexture2D(&resolveDesc, nullptr, &resolveFrame_);
                if (FAILED(result)) {
                    fail("CreateTexture2D(resolve target)", result);
                    Reset();
                    return false;
                }
            }
            REX::W32::D3D11_QUERY_DESC queryDesc{};
            queryDesc.query = REX::W32::D3D11_QUERY_EVENT;
            result = device_->CreateQuery(&queryDesc, &query_);
            if (FAILED(result)) {
                fail("CreateQuery(readback event)", result);
                Reset();
                return false;
            }
            width_ = sourceDesc.width;
            height_ = sourceDesc.height;
            format_ = sourceDesc.format;
            sampleCount_ = sourceDesc.sampleDesc.count;
            SKSE::log::info("SkyCounterUI inventory preview target: {}x{} format {} samples {}",
                width_, height_, static_cast<std::uint32_t>(format_), sampleCount_);
            return true;
        }

        bool EnsureDepthBackup(Device* device, Texture2D* depthTexture,
            std::string* failureReason = nullptr)
        {
            if (!device || !depthTexture) return false;
            REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};
            depthTexture->GetDesc(&sourceDesc);
            const bool descriptorMatches = savedDepth_ &&
                depthWidth_ == sourceDesc.width && depthHeight_ == sourceDesc.height &&
                depthMipLevels_ == sourceDesc.mipLevels && depthArraySize_ == sourceDesc.arraySize &&
                depthFormat_ == sourceDesc.format &&
                depthSampleCount_ == sourceDesc.sampleDesc.count &&
                depthSampleQuality_ == sourceDesc.sampleDesc.quality;
            if (device_ == device && descriptorMatches) return true;

            Release(savedDepth_);
            REX::W32::D3D11_TEXTURE2D_DESC backupDesc = sourceDesc;
            backupDesc.usage = REX::W32::D3D11_USAGE_DEFAULT;
            backupDesc.bindFlags = 0;
            backupDesc.cpuAccessFlags = 0;
            backupDesc.miscFlags = 0;
            const auto result = device->CreateTexture2D(&backupDesc, nullptr, &savedDepth_);
            if (FAILED(result) || !savedDepth_) {
                if (failureReason) *failureReason = "could not allocate a backup for Skyrim's active depth target";
                SKSE::log::warn("SkyCounterUI could not create depth-preservation texture (HRESULT 0x{:08X}); depth {}x{}, mips {}, slices {}, format {}, samples {}, quality {}",
                    static_cast<std::uint32_t>(result), sourceDesc.width, sourceDesc.height,
                    sourceDesc.mipLevels, sourceDesc.arraySize,
                    static_cast<std::uint32_t>(sourceDesc.format),
                    sourceDesc.sampleDesc.count, sourceDesc.sampleDesc.quality);
                Release(savedDepth_);
                return false;
            }

            depthWidth_ = sourceDesc.width;
            depthHeight_ = sourceDesc.height;
            depthMipLevels_ = sourceDesc.mipLevels;
            depthArraySize_ = sourceDesc.arraySize;
            depthFormat_ = sourceDesc.format;
            depthSampleCount_ = sourceDesc.sampleDesc.count;
            depthSampleQuality_ = sourceDesc.sampleDesc.quality;
            SKSE::log::info("SkyCounterUI will isolate inventory preview depth against the {}x{} game depth target (format {}, samples {})",
                depthWidth_, depthHeight_, static_cast<std::uint32_t>(depthFormat_), depthSampleCount_);
            return true;
        }

        OffscreenPoll Finish(OffscreenPoll result)
        {
            pending_ = false;
            immediate_ = nullptr;
            formID_ = 0;
            path_.clear();
            waitedFrames_ = 0;
            passStarted_ = false;
            return result;
        }

        void Reset()
        {
            pending_ = false;
            Release(whiteStaging_);
            Release(blackStaging_);
            Release(resolveFrame_);
            Release(savedFrame_);
            Release(savedDepth_);
            Release(query_);
            Release(device_);
            immediate_ = nullptr;
            width_ = height_ = 0;
            depthWidth_ = depthHeight_ = depthMipLevels_ = depthArraySize_ = 0;
            depthSampleCount_ = depthSampleQuality_ = 0;
            sampleCount_ = 0;
            format_ = static_cast<REX::W32::DXGI_FORMAT>(0);
            depthFormat_ = static_cast<REX::W32::DXGI_FORMAT>(0);
            passStarted_ = false;
            formID_ = 0;
            path_.clear();
        }

        Device* device_{};
        Context* immediate_{};
        Texture2D* savedFrame_{};
        Texture2D* savedDepth_{};
        Texture2D* resolveFrame_{};
        Texture2D* blackStaging_{};
        Texture2D* whiteStaging_{};
        Query* query_{};
        RE::FormID formID_{};
        std::filesystem::path path_;
        std::uint32_t width_{};
        std::uint32_t height_{};
        std::uint32_t depthWidth_{};
        std::uint32_t depthHeight_{};
        std::uint32_t depthMipLevels_{};
        std::uint32_t depthArraySize_{};
        std::uint32_t depthSampleCount_{};
        std::uint32_t depthSampleQuality_{};
        std::uint32_t sampleCount_{};
        REX::W32::DXGI_FORMAT format_{};
        REX::W32::DXGI_FORMAT depthFormat_{};
        std::uint32_t waitedFrames_{};
        bool passStarted_{};
        bool pending_{};
        bool renderStateAfterFirstPassLogged_{};
    } g_inventory3DPreviewCapture;

    std::mutex g_queueMutex;
    std::deque<CaptureRequest> g_captureQueue;
    std::deque<CaptureRequest> g_menuDeferredCaptureQueue;
    std::unordered_set<RE::FormID> g_queuedFormIDs;
    std::deque<PixelJob> g_pixelQueue;
    std::condition_variable g_pixelReady;
    std::atomic<bool> g_writerStarted{ false };
    std::mutex g_locationFrameMutex;
    std::deque<LocationFrameRequest> g_locationFrameQueue;
    std::deque<LocationFramePixelJob> g_locationFramePixelQueue;
    std::condition_variable g_locationFramePixelReady;
    std::atomic<bool> g_locationFrameWriterStarted{ false };
    std::atomic<bool> g_locationFrameReadbackPending{ false };
    std::atomic<bool> g_locationFrameCancelRequested{ false };
    std::uint32_t g_locationFrameReadbackWaitFrames{};
    std::uint32_t g_locationFrameMapWaitFrames{};
    LocationFrameRequest g_activeLocationFrameRequest;
    Texture2D* g_locationFrameResolve{};
    Texture2D* g_locationFrameStaging{};
    Query* g_locationFrameQuery{};
    std::uint32_t g_locationFrameWidth{};
    std::uint32_t g_locationFrameHeight{};
    REX::W32::DXGI_FORMAT g_locationFrameFormat{};
    std::mutex g_modelMutex;
    std::deque<NativeModelJob> g_modelQueue;
    std::deque<NativeModelResult> g_modelResults;
    std::condition_variable g_modelReady;
    std::atomic<bool> g_modelWorkerStarted{ false };
    std::unordered_map<std::string, std::vector<std::uint8_t>> g_activeTextureBytes;
    std::atomic<bool> g_rendererInstalled{ false };
    std::mutex g_rendererInstallMutex;
    // Match SkyCounter UI's shipped renderIcons default. Settings may still
    // disable capture explicitly, but pickups must not depend on the PrismaUI
    // settings callback arriving before the first inventory event.
    std::atomic<bool> g_captureEnabled{ true };
    std::atomic<bool> g_missingPumpTaskInterfaceLogged{ false };
    std::atomic<bool> g_gamePumpScheduled{ false };
    std::atomic<std::uint64_t> g_gamePumpTaskSequence{ 0 };
    std::atomic<std::uint64_t> g_gamePumpCoalescedCount{ 0 };
    std::atomic<std::uint64_t> g_gamePumpQueueWaitCount{ 0 };
    std::atomic<std::uint64_t> g_captureMenuFrameWaitCount{ 0 };
    std::atomic<bool> g_captureMenuWaitLogged{ false };
    std::atomic<CapturePhase> g_phase{ CapturePhase::Idle };
    std::atomic<std::uint32_t> g_gameFrames{ 0 };
    std::atomic<std::uint32_t> g_idleManagerPumpFrames{ 0 };
    std::atomic<bool> g_readbackPending{ false };
    bool g_loadTimeoutLogged{};
    bool g_managerPumpWarningLogged{};
    bool g_inventorySceneClaimed{};
    std::atomic<bool> g_pendingOwnedSceneTeardown{ false };
    std::atomic<std::uint32_t> g_menuPreviewHandoffFrames{};
    CaptureRequest g_activeRequest;
    SkyCounterUIIconReadyCallback g_iconReadyCallback{};
    SkyCounterUICaptureStatusCallback g_captureStatusCallback{};
    std::atomic<bool> g_captureMenuRegistered{ false };
    std::uint32_t g_previewCapturePass{};
    std::atomic<bool> g_menuEventSinkRegistered{ false };

    const char* CapturePhaseName(CapturePhase phase)
    {
        switch (phase) {
        case CapturePhase::Idle: return "idle";
        case CapturePhase::Loading: return "loading";
        case CapturePhase::Ready: return "ready";
        case CapturePhase::Captured: return "captured";
        case CapturePhase::Failed: return "failed";
        default: return "unknown";
        }
    }

    std::size_t CaptureQueueSnapshot(RE::FormID* firstFormID = nullptr)
    {
        std::scoped_lock lock(g_queueMutex);
        if (firstFormID) {
            *firstFormID = g_captureQueue.empty() ? 0 : g_captureQueue.front().formID;
        }
        return g_captureQueue.size();
    }

    void RequeueMenuDeferredCaptures()
    {
        std::size_t requeued{};
        std::vector<RE::FormID> resumedFormIDs;
        {
            std::scoped_lock lock(g_queueMutex);
            requeued = g_menuDeferredCaptureQueue.size();
            while (!g_menuDeferredCaptureQueue.empty()) {
                auto deferred = std::move(g_menuDeferredCaptureQueue.back());
                g_menuDeferredCaptureQueue.pop_back();
                resumedFormIDs.push_back(deferred.formID);
                deferred.usesMenuPreview = false;
                deferred.item = nullptr;
                deferred.restoreItem = nullptr;
                deferred.ownsInventoryScene = false;
                g_captureQueue.push_front(std::move(deferred));
            }
        }
        if (requeued) {
            SKSE::log::info("SkyCounterUI resumed {} pickup icon capture(s) deferred during an open player menu", requeued);
            for (const auto formID : resumedFormIDs) {
                SKSE::log::info("SkyCounterUI requeued deferred menu-preview icon capture: formID={:08X}", formID);
            }
            g_menuPreviewHandoffFrames.store(0, std::memory_order_release);
            ScheduleGamePump();
        }
    }

    bool SetCaptureMenuPaused(bool shouldPause)
    {
        auto* ui = RE::UI::GetSingleton();
        auto menu = ui ? ui->GetMenu(kRenderMenuName) : RE::GPtr<RE::IMenu>{};
        if (!menu) {
            if (!shouldPause) {
                // If Skyrim has already removed the menu, its normal close
                // path has already consumed the pause flags. Do not decrement
                // the global counters a second time here.
                g_captureMenuPauseOwned.store(false, std::memory_order_release);
            }
            return false;
        }

        if (shouldPause) {
            if (g_captureMenuPauseOwned.load(std::memory_order_acquire)) {
                // The capture menu stays open while its model is loaded. If
                // another menu transition changed the shared pause counter,
                // reassert our one owned pause before allowing Render().
                if (!ui->GameIsPaused()) {
                    menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame,
                        RE::UI_MENU_FLAGS::kDisablePauseMenu);
                    ++ui->numPausesGame;
                    SKSE::log::warn("SkyCounterUI restored its capture pause because Skyrim's global pause counter had been cleared");
                }
                return ui->GameIsPaused();
            }

            // Inventory3DManager::Render() is gated by Skyrim's paused-menu
            // state. Keep the service menu non-pausing during the asynchronous
            // model load, then enable the pause only for the matte render
            // frames. Changing an already-open menu's flags does not update
            // UI::numPausesGame, so PausesGame() alone is not enough: update
            // the same UI counters Skyrim normally maintains when a menu opens.
            // kDisablePauseMenu prevents the pause screen from opening over
            // the invisible service menu during this short capture window.
            menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame,
                RE::UI_MENU_FLAGS::kDisablePauseMenu);
            ++ui->numPausesGame;
            ++ui->numDisablePauseMenu;
            g_captureMenuPauseOwned.store(true, std::memory_order_release);
            SKSE::log::info("SkyCounterUI enabled capture-only pause immediately before the off-screen matte passes: menu flag={}, UI reports paused={}, pause counter={}, disable-pause counter={}",
                menu->PausesGame(), ui->GameIsPaused(), ui->numPausesGame,
                ui->numDisablePauseMenu);
            return ui->GameIsPaused();
        }

        if (g_captureMenuPauseOwned.exchange(false, std::memory_order_acq_rel)) {
            menu->menuFlags.reset(RE::UI_MENU_FLAGS::kPausesGame,
                RE::UI_MENU_FLAGS::kDisablePauseMenu);
            if (ui->numPausesGame > 0) --ui->numPausesGame;
            if (ui->numDisablePauseMenu > 0) --ui->numDisablePauseMenu;
            SKSE::log::info("SkyCounterUI released capture-only pause after matte passes: UI reports paused={}, pause counter={}, disable-pause counter={}",
                ui->GameIsPaused(), ui->numPausesGame, ui->numDisablePauseMenu);
        }
        return true;
    }

    void AllowIconCaptureRetry(RE::FormID formID)
    {
        if (!formID) return;
        std::scoped_lock lock(g_queueMutex);
        g_queuedFormIDs.erase(formID);
    }

    class CaptureMenuEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            const bool captureServiceMenu = event &&
                event->menuName == RE::BSFixedString(kRenderMenuName);
            const bool inventoryPreviewMenu = event &&
                (event->menuName == RE::BSFixedString("InventoryMenu") ||
                    event->menuName == RE::BSFixedString("ContainerMenu") ||
                    event->menuName == RE::BSFixedString("BarterMenu") ||
                    event->menuName == RE::BSFixedString("GiftMenu") ||
                    event->menuName == RE::BSFixedString("Crafting Menu") ||
                    event->menuName == RE::BSFixedString("Alchemy Menu") ||
                    event->menuName == RE::BSFixedString("Smithing Menu") ||
                    event->menuName == RE::BSFixedString("Enchanting Menu") ||
                    event->menuName == RE::BSFixedString("Cooking Menu"));
            if (captureServiceMenu) {
                if (event->opening) {
                    g_captureMenuWaitLogged.store(false, std::memory_order_release);
                    g_captureMenuShowRequestedAtMs.store(0, std::memory_order_release);
                }
                RE::FormID queuedFormID{};
                const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                SKSE::log::info("SkyCounterUI capture service-menu event: menu='{}', opening={}, phase={}, queueSize={}, firstQueuedFormID={:08X}, showRequest={}, hideRequested={}, pumpScheduled={}, readbackPending={}, captureEnabled={}, thread={}",
                    event->menuName.c_str(), event->opening,
                    CapturePhaseName(g_phase.load(std::memory_order_acquire)), queueSize,
                    queuedFormID,
                    static_cast<std::uint32_t>(g_captureMenuShowRequested.load(std::memory_order_acquire)),
                    g_captureMenuHideRequested.load(std::memory_order_acquire),
                    g_gamePumpScheduled.load(std::memory_order_acquire),
                    g_readbackPending.load(std::memory_order_acquire),
                    g_captureEnabled.load(std::memory_order_acquire), GetCurrentThreadId());
            }
            if (inventoryPreviewMenu && event->opening) {
                SKSE::log::info("SkyCounterUI detected player preview menu opening: {}", event->menuName.c_str());
                // Give Skyrim ownership immediately. Any unfinished standalone
                // capture is requeued, then can read the preview Skyrim loads
                // for this menu without replacing it in Inventory3DManager.
                if (g_inventorySceneClaimed || g_activeRequest.ownsInventoryScene) {
                    g_pendingOwnedSceneTeardown.store(true, std::memory_order_release);
                    g_menuPreviewHandoffFrames.store(0, std::memory_order_release);
                }
                DeferActiveCaptureForInventoryMenu();
                if (g_activeRequest.formID) g_activeRequest.ownsInventoryScene = false;
            } else if (inventoryPreviewMenu && !event->opening &&
                g_activeRequest.usesMenuPreview &&
                (g_phase.load(std::memory_order_acquire) == CapturePhase::Loading ||
                    g_phase.load(std::memory_order_acquire) == CapturePhase::Ready) &&
                !g_readbackPending.load(std::memory_order_acquire)) {
                // The requested form was not among the menu's loaded previews.
                // Fall back only after Skyrim closes its menu, when the normal
                // isolated capture can safely load its own model.
                auto deferred = std::move(g_activeRequest);
                deferred.usesMenuPreview = false;
                deferred.restoreItem = nullptr;
                g_inventory3DPreviewCapture.CancelPass();
                {
                    std::scoped_lock lock(g_queueMutex);
                    g_captureQueue.push_front(std::move(deferred));
                }
                g_activeRequest = {};
                g_gameFrames.store(0, std::memory_order_relaxed);
                g_previewCapturePass = 0;
                g_phase.store(CapturePhase::Idle, std::memory_order_release);
            }
            if (inventoryPreviewMenu && !event->opening) {
                RE::FormID queuedFormID{};
                const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                SKSE::log::info("SkyCounterUI detected player preview menu closing: menu='{}', phase={}, queuedCaptures={}, firstQueuedFormID={:08X}, captureMenuOpen={}, pumpScheduled={}, captureEnabled={}, thread={}",
                    event->menuName.c_str(),
                    CapturePhaseName(g_phase.load(std::memory_order_acquire)), queueSize,
                    queuedFormID,
                    RE::UI::GetSingleton() && RE::UI::GetSingleton()->IsMenuOpen(kRenderMenuName),
                    g_gamePumpScheduled.load(std::memory_order_acquire),
                    g_captureEnabled.load(std::memory_order_acquire), GetCurrentThreadId());
                RequeueMenuDeferredCaptures();
            }
            if (event && (g_captureEnabled.load(std::memory_order_acquire) ||
                g_phase.load(std::memory_order_acquire) != CapturePhase::Idle ||
                g_readbackPending.load(std::memory_order_acquire) ||
                g_captureMenuHideRequested.load(std::memory_order_acquire))) {
                ScheduleGamePump(inventoryPreviewMenu ? "player preview menu event" :
                    (captureServiceMenu ? "capture render menu event" : "other menu event"));
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    CaptureMenuEventSink g_captureMenuEventSink;

    void ReleaseCom(auto*& value)
    {
        if (value) {
            value->Release();
            value = nullptr;
        }
    }

    std::string Utf8(const std::wstring& value)
    {
        if (value.empty()) return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string result(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
        return result;
    }

    std::string FileUrl(const std::filesystem::path& path)
    {
        std::wstring wide = path.wstring();
        std::replace(wide.begin(), wide.end(), L'\\', L'/');
        auto utf8 = Utf8(wide);
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
        // A renderer version plus file timestamp forces PrismaUI's browser to
        // reload the PNG after it is atomically replaced at the same path.
        result += "?v=" + std::to_string(kIconRendererVersion);
        std::error_code stampError;
        const auto writeTime = std::filesystem::last_write_time(path, stampError);
        if (!stampError) result += "-" + std::to_string(writeTime.time_since_epoch().count());
        return result;
    }

    const RE::TESModelTextureSwap* GetTextureSwapComponent(const RE::TESForm* form)
    {
        if (!form) return nullptr;
        switch (form->GetFormType()) {
        case RE::FormType::Weapon:
            return static_cast<const RE::TESObjectWEAP*>(form);
        case RE::FormType::Ammo:
            return static_cast<const RE::TESAmmo*>(form);
        case RE::FormType::Book:
            return static_cast<const RE::TESObjectBOOK*>(form);
        case RE::FormType::AlchemyItem:
            return static_cast<const RE::AlchemyItem*>(form);
        case RE::FormType::Ingredient:
            return static_cast<const RE::IngredientItem*>(form);
        case RE::FormType::Scroll:
            return static_cast<const RE::ScrollItem*>(form);
        case RE::FormType::Light:
            return static_cast<const RE::TESObjectLIGH*>(form);
        case RE::FormType::Misc:
        case RE::FormType::Apparatus:
        case RE::FormType::KeyMaster:
        case RE::FormType::SoulGem:
            return static_cast<const RE::TESObjectMISC*>(form);
        default:
            return nullptr;
        }
    }

    std::vector<TextureSwapSnapshot> SnapshotTextureSwaps(const RE::TESForm* form)
    {
        std::vector<TextureSwapSnapshot> snapshots;
        const auto* model = GetTextureSwapComponent(form);
        if (!model || !model->numAlternateTextures || !model->alternateTextures) return snapshots;
        if (model->numAlternateTextures > 1024) {
            SKSE::log::warn("SkyCounterUI ignored {} unreasonable texture swaps for form {:08X}",
                model->numAlternateTextures, form->GetFormID());
            return snapshots;
        }

        snapshots.reserve(model->numAlternateTextures);
        for (std::uint32_t i = 0; i < model->numAlternateTextures; ++i) {
            const auto& alternate = model->alternateTextures[i];
            if (!alternate.textureSet) continue;

            TextureSwapSnapshot snapshot;
            snapshot.index3D = alternate.index3D;
            if (const auto* name = alternate.name3D.c_str(); name && *name) snapshot.shapeName = name;
            snapshot.texturePaths.resize(RE::BSTextureSet::Textures::kUsedTotal);
            for (std::size_t slot = 0; slot < snapshot.texturePaths.size(); ++slot) {
                const auto* path = alternate.textureSet->GetTexturePath(
                    static_cast<RE::BSTextureSet::Texture>(slot));
                if (path && *path) snapshot.texturePaths[slot] = path;
            }
            snapshots.push_back(std::move(snapshot));
        }
        return snapshots;
    }

    std::string ItemModelPath(const RE::TESForm* form)
    {
        if (!form) return {};
        const char* model = nullptr;
        switch (form->GetFormType()) {
        case RE::FormType::Weapon:
            model = static_cast<const RE::TESObjectWEAP*>(form)->GetModel();
            break;
        case RE::FormType::Armor: {
            const auto* armor = static_cast<const RE::TESObjectARMO*>(form);
            model = armor->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
            if (!model || !*model) model = armor->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel();
            break;
        }
        case RE::FormType::Ammo:
            model = static_cast<const RE::TESAmmo*>(form)->GetModel();
            break;
        case RE::FormType::Book:
            model = static_cast<const RE::TESObjectBOOK*>(form)->GetModel();
            break;
        case RE::FormType::AlchemyItem:
            model = static_cast<const RE::AlchemyItem*>(form)->GetModel();
            break;
        case RE::FormType::Ingredient:
            model = static_cast<const RE::IngredientItem*>(form)->GetModel();
            break;
        case RE::FormType::Scroll:
            model = static_cast<const RE::ScrollItem*>(form)->GetModel();
            break;
        case RE::FormType::Light:
            model = static_cast<const RE::TESObjectLIGH*>(form)->GetModel();
            break;
        case RE::FormType::Misc:
        case RE::FormType::Apparatus:
        case RE::FormType::KeyMaster:
        case RE::FormType::SoulGem:
            model = static_cast<const RE::TESObjectMISC*>(form)->GetModel();
            break;
        default:
            break;
        }
        if (!model || !*model) return {};
        std::string path(model);
        std::replace(path.begin(), path.end(), '/', '\\');
        while (path.starts_with(".\\")) path.erase(0, 2);
        if (path.find("..") != std::string::npos || path.find(':') != std::string::npos) return {};
        return path;
    }

    bool IsRuntimeGeneratedAlchemyItem(const RE::TESForm* form)
    {
        if (!form || form->GetFormType() != RE::FormType::AlchemyItem) return false;

        // Potions created at a crafting station can be temporary runtime forms
        // rather than records owned by a plugin. Keep this classification for
        // diagnostics; a valid model path can still be loaded through the same
        // bounded Inventory3DManager capture and timeout cleanup as other items.
        return !form->GetFile() || (form->GetFormID() & 0xFF000000u) == 0xFF000000u;
    }

    std::string NormalizeResourceName(std::string path)
    {
        std::replace(path.begin(), path.end(), '/', '\\');
        while (!path.empty() && (path.front() == '\\' || path.front() == ' ')) path.erase(path.begin());
        if (path.size() >= 5) {
            std::string prefix = path.substr(0, 5);
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (prefix == "data\\") path.erase(0, 5);
        }
        if (path.find("..") != std::string::npos || path.find(':') != std::string::npos) return {};
        return path;
    }

    std::string NormalizeModelDatabaseName(std::string path)
    {
        path = NormalizeResourceName(std::move(path));
        if (path.empty()) return {};
        std::string prefix = path.substr(0, std::min<std::size_t>(7, path.size()));
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        // TESModel paths and BSModelDB::Demand are relative to Data\meshes;
        // the engine resolves the meshes directory itself.
        if (prefix == "meshes\\") path.erase(0, 7);
        return path;
    }

    std::string NormalizeTextureResourceName(std::string path)
    {
        path = NormalizeResourceName(std::move(path));
        if (path.empty()) return {};
        std::string prefix = path.substr(0, std::min<std::size_t>(9, path.size()));
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (prefix == "textures\\" || prefix == "textures") return path;
        return "textures\\" + path;
    }

    std::string TextureCacheKey(std::string path)
    {
        path = NormalizeResourceName(std::move(path));
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return path;
    }

    std::vector<std::uint8_t> ReadGameResource(const std::string& rawPath, std::size_t sizeLimit,
        std::string* failureReason = nullptr)
    {
        if (failureReason) failureReason->clear();
        const auto resourcePath = NormalizeResourceName(rawPath);
        if (resourcePath.empty()) {
            if (failureReason) *failureReason = "resource path is empty or invalid";
            return {};
        }

        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length && length < executable.size()) {
            const std::filesystem::path loosePath = std::filesystem::path(
                std::wstring(executable.data(), length)).parent_path() / L"Data" / resourcePath;
            std::ifstream looseFile(loosePath, std::ios::binary | std::ios::ate);
            if (looseFile) {
                const auto size = static_cast<std::streamoff>(looseFile.tellg());
                if (size > 0 && static_cast<std::uint64_t>(size) <= sizeLimit) {
                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                    looseFile.seekg(0, std::ios::beg);
                    looseFile.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    if (looseFile) return bytes;
                }
            }
        }

        RE::BSResourceNiBinaryStream stream(resourcePath);
        if (!stream.good()) {
            if (failureReason) *failureReason = "Skyrim could not open the resource in its active files or BSA archives";
            return {};
        }
        const auto totalSize = stream.stream->totalSize;
        if (!totalSize || totalSize > sizeLimit || totalSize > std::numeric_limits<std::uint32_t>::max()) {
            if (failureReason) *failureReason = "Skyrim returned an empty or oversized resource";
            return {};
        }
        std::vector<std::uint8_t> bytes(totalSize);
        // Use NiBinaryStream's buffered read path, matching SkyLab. Reading
        // Stream::DoRead directly bypasses the wrapper Skyrim uses for
        // archive-backed resources and may fail on compressed BSA entries.
        stream.seek(0);
        if (!stream.read(bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
            if (failureReason) *failureReason = "Skyrim opened the resource but its buffered read failed";
            return {};
        }
        return bytes;
    }

    void QueueNativeModelDemand(NativeModelJob job,
        std::unordered_map<std::string, std::vector<std::uint8_t>> textures)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            NativeModelResult result;
            result.formID = job.formID;
            result.textures = std::move(textures);
            result.error = "SKSE's game task interface is unavailable";
            {
                std::scoped_lock lock(g_modelMutex);
                g_modelResults.push_back(std::move(result));
            }
            ScheduleGamePump();
            return;
        }

        // BSModelDB::Demand touches Skyrim's model/resource database and must
        // run through the engine task queue. Calling it from our detached
        // resource worker can race engine teardown/loading and crash while
        // deserializing a NIF. This task may briefly occupy one game frame,
        // but it does not pause gameplay or open an inventory/menu preview.
        tasks->AddTask([job = std::move(job), textures = std::move(textures)]() mutable {
            NativeModelResult result;
            result.formID = job.formID;
            result.textures = std::move(textures);
            try {
                RE::BSModelDB::DBTraits::ArgsType args{};
                const auto error = RE::BSModelDB::Demand(job.modelPath.c_str(), result.model, args);
                if (error != RE::BSResource::ErrorCode::kNone || !result.model) {
                    result.error = "Skyrim's native model database could not load the item model";
                } else {
                    SKSE::log::info("SkyCounterUI loaded pickup {:08X} with Skyrim's native model database: '{}'",
                        job.formID, job.modelPath);
                }
            } catch (const std::exception& exception) {
                result.error = exception.what();
            } catch (...) {
                result.error = "Skyrim's native model loading task failed with an unknown error";
            }

            {
                std::scoped_lock lock(g_modelMutex);
                g_modelResults.push_back(std::move(result));
            }
            ScheduleGamePump();
        });
    }

    void NativeModelLoaderMain()
    {
        // Read any texture-swap resources away from the game thread, then
        // hand the actual Skyrim model demand to its game task queue.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            NativeModelJob job;
            {
                std::unique_lock lock(g_modelMutex);
                g_modelReady.wait(lock, [] { return !g_modelQueue.empty(); });
                job = std::move(g_modelQueue.front());
                g_modelQueue.pop_front();
            }

            std::unordered_map<std::string, std::vector<std::uint8_t>> textures;
            std::size_t totalTextureBytes = 0;
            constexpr std::size_t kPerTextureLimit = 64 * 1024 * 1024;
            constexpr std::size_t kTotalTextureLimit = 128 * 1024 * 1024;
            for (const auto& swap : job.textureSwaps) {
                for (const auto& texturePath : swap.texturePaths) {
                    if (texturePath.empty()) continue;
                    const auto normalized = NormalizeTextureResourceName(texturePath);
                    const auto key = TextureCacheKey(normalized);
                    if (key.empty() || textures.contains(key)) continue;
                    std::string readFailure;
                    auto bytes = ReadGameResource(normalized, kPerTextureLimit, &readFailure);
                    if (bytes.empty()) {
                        SKSE::log::warn("SkyCounterUI could not read swapped texture '{}': {}",
                            normalized, readFailure);
                        continue;
                    }
                    if (bytes.size() > kTotalTextureLimit - totalTextureBytes) {
                        SKSE::log::warn("SkyCounterUI skipped additional swapped textures for {:08X}: capture texture budget reached",
                            job.formID);
                        break;
                    }
                    totalTextureBytes += bytes.size();
                    textures.emplace(key, std::move(bytes));
                }
                if (totalTextureBytes >= kTotalTextureLimit) break;
            }

            QueueNativeModelDemand(std::move(job), std::move(textures));
        }
    }

    void StartNativeModelLoader()
    {
        bool expected = false;
        if (!g_modelWorkerStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        std::thread(NativeModelLoaderMain).detach();
    }

    void NotifyIconReady(RE::FormID formID, const std::filesystem::path& path)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            SKSE::log::warn("SkyCounterUI saved icon for {:08X}, but could not notify the UI because SKSE TaskInterface is unavailable (path '{}')",
                formID, path.string());
            return;
        }
        const auto url = FileUrl(path);
        SKSE::log::info("SkyCounterUI scheduled UI icon update: formID={:08X}, callbackInstalled={}, path='{}'",
            formID, g_iconReadyCallback != nullptr, path.string());
        tasks->AddTask([formID, url] {
            if (g_iconReadyCallback) g_iconReadyCallback(formID, url);
        });
    }

    void NotifyCaptureStatus(RE::FormID formID, SkyCounterUICaptureStatus status)
    {
        // Capture-menu activity is internal. Keep the whole capture lifecycle
        // out of SkyCounterUI's visible status message; details remain in the
        // SKSE log and finished icons still update through NotifyIconReady.
        const char* state = "unknown";
        switch (status) {
        case SkyCounterUICaptureStatus::Rendering: state = "rendering"; break;
        case SkyCounterUICaptureStatus::Ready: state = "ready"; break;
        case SkyCounterUICaptureStatus::Failed: state = "failed"; break;
        }
        SKSE::log::info("SkyCounterUI icon capture state: formID={:08X}, state={}, statusCallbackInstalled={}",
            formID, state, g_captureStatusCallback != nullptr);
    }

    std::filesystem::path RendererVersionPath(const std::filesystem::path& iconPath)
    {
        auto marker = iconPath;
        marker += L".renderer-version";
        return marker;
    }

    bool WriteRendererVersion(const std::filesystem::path& iconPath)
    {
        const auto marker = RendererVersionPath(iconPath);
        const auto contents = std::to_string(kIconRendererVersion) + "\n";
        HANDLE output = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
        DWORD written{};
        const auto success = WriteFile(output, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
            written == static_cast<DWORD>(contents.size()) && FlushFileBuffers(output);
        CloseHandle(output);
        return success;
    }

    bool ReplaceFileFromTemporary(const std::filesystem::path& temporary,
        const std::filesystem::path& destination, RE::FormID formID)
    {
        constexpr std::uintmax_t kMaxIconFileSize = 16 * 1024 * 1024;
        std::error_code error;
        const auto temporarySize = std::filesystem::file_size(temporary, error);
        if (error || temporarySize < 8 || temporarySize > kMaxIconFileSize) {
            SKSE::log::warn("SkyCounterUI temporary PNG for {:08X} has an unreadable size", formID);
            return false;
        }
        // Copy through CreateFile/WriteFile so MO2's virtual Data filesystem
        // sees the write. MoveFileEx bypassed that route and failed to replace
        // icons in the user's overwrite/cache path.
        std::vector<std::uint8_t> png(static_cast<std::size_t>(temporarySize));
        std::ifstream input(temporary, std::ios::binary);
        if (!input || !input.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size()))) {
            SKSE::log::warn("SkyCounterUI could not read completed temporary PNG for {:08X} from '{}'",
                formID, temporary.string());
            return false;
        }

        HANDLE output = INVALID_HANDLE_VALUE;
        DWORD openError = ERROR_SUCCESS;
        constexpr std::uint32_t kWriteOpenAttempts = 8;
        for (std::uint32_t attempt = 0; attempt < kWriteOpenAttempts; ++attempt) {
            output = CreateFileW(destination.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output != INVALID_HANDLE_VALUE) break;

            openError = GetLastError();
            if (openError != ERROR_SHARING_VIOLATION && openError != ERROR_LOCK_VIOLATION) break;
            if (attempt + 1 < kWriteOpenAttempts) Sleep(50 * (attempt + 1));
        }
        if (output == INVALID_HANDLE_VALUE) {
            SKSE::log::warn("SkyCounterUI could not open cached PNG for writing for {:08X} after {} attempts (Win32 error {}, path '{}')",
                formID, kWriteOpenAttempts, openError, destination.string());
            return false;
        }

        bool writeSucceeded = true;
        std::size_t offset = 0;
        DWORD writeError = ERROR_SUCCESS;
        while (offset < png.size()) {
            const auto remaining = png.size() - offset;
            const auto chunkSize = static_cast<DWORD>((std::min)(remaining,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written{};
            if (!WriteFile(output, png.data() + offset, chunkSize, &written, nullptr)) {
                writeError = GetLastError();
                writeSucceeded = false;
                break;
            }
            if (written != chunkSize) {
                writeError = ERROR_WRITE_FAULT;
                writeSucceeded = false;
                break;
            }
            offset += written;
        }
        if (writeSucceeded && !FlushFileBuffers(output)) {
            writeError = GetLastError();
            writeSucceeded = false;
        }
        CloseHandle(output);
        if (!writeSucceeded) {
            SKSE::log::warn("SkyCounterUI could not write cached PNG for {:08X} (Win32 error {}, path '{}')",
                formID, writeError, destination.string());
            return false;
        }

        static std::atomic<bool> loggedCacheWrite{};
        if (!loggedCacheWrite.exchange(true, std::memory_order_acq_rel)) {
            SKSE::log::info("SkyCounterUI saved its first icon to the Skyrim Data SkyCounter UI cache");
        }
        return true;
    }

    bool SavePng(const PixelJob& job)
    {
        const auto parent = job.path.parent_path();
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            SKSE::log::warn("SkyCounterUI could not create icon cache directory '{}': {}",
                parent.string(), error.message());
            return false;
        }

        // Remove temporary PNGs left in the icon directory by older builds
        // that were interrupted before their normal cleanup path ran.
        static std::unordered_set<std::wstring> cleanedCacheDirectories;
        const auto directoryKey = parent.lexically_normal().native();
        if (cleanedCacheDirectories.insert(directoryKey).second) {
            std::error_code scanError;
            std::filesystem::directory_iterator entry(parent, scanError);
            const std::filesystem::directory_iterator end;
            while (!scanError && entry != end) {
                const auto name = entry->path().filename().native();
                constexpr wchar_t legacySuffix[] = L".png.tmp.png";
                constexpr auto legacySuffixLength = (sizeof(legacySuffix) / sizeof(legacySuffix[0])) - 1;
                if (name.size() >= legacySuffixLength &&
                    name.compare(name.size() - legacySuffixLength, legacySuffixLength,
                        legacySuffix, legacySuffixLength) == 0) {
                    std::error_code removeError;
                    std::filesystem::remove(entry->path(), removeError);
                    if (removeError) {
                        SKSE::log::warn("SkyCounterUI could not remove stale cache temporary '{}': {}",
                            entry->path().string(), removeError.message());
                    }
                }
                entry.increment(scanError);
            }
        }

        auto temporaryDirectory = std::filesystem::temp_directory_path(error);
        if (error) {
            SKSE::log::warn("SkyCounterUI could not locate the Windows temporary directory for icon {:08X}: {}",
                job.formID, error.message());
            return false;
        }
        const auto temporary = temporaryDirectory /
            (L"SkyCounterUI-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(job.formID) + L"-" + std::to_wstring(GetTickCount64()) + L".png");
        DeleteFileW(temporary.c_str());

        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(comResult);
        IWICImagingFactory* factory = nullptr;
        IWICBitmap* bitmap = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;
        bool success = false;
        const char* failedStage = "WIC imaging factory";
        HRESULT failedResult = S_OK;

        do {
            failedResult = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (FAILED(failedResult)) break;
            failedStage = "create bitmap from GPU readback";
            failedResult = factory->CreateBitmapFromMemory(kOutputSize, kOutputSize, GUID_WICPixelFormat32bppBGRA,
                kOutputSize * 4, static_cast<UINT>(job.bgra.size()), const_cast<BYTE*>(job.bgra.data()), &bitmap);
            if (FAILED(failedResult)) break;
            failedStage = "create WIC stream";
            failedResult = factory->CreateStream(&stream);
            if (FAILED(failedResult)) break;
            failedStage = "open temporary PNG";
            failedResult = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE);
            if (FAILED(failedResult)) break;
            failedStage = "create PNG encoder";
            failedResult = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            if (FAILED(failedResult)) break;
            failedStage = "initialize PNG encoder";
            failedResult = encoder->Initialize(stream, WICBitmapEncoderNoCache);
            if (FAILED(failedResult)) break;
            failedStage = "create PNG frame";
            failedResult = encoder->CreateNewFrame(&frame, &properties);
            if (FAILED(failedResult)) break;
            failedStage = "initialize PNG frame";
            failedResult = frame->Initialize(properties);
            if (FAILED(failedResult)) break;
            failedStage = "set PNG dimensions";
            failedResult = frame->SetSize(kOutputSize, kOutputSize);
            if (FAILED(failedResult)) break;
            failedStage = "set PNG pixel format";
            WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
            failedResult = frame->SetPixelFormat(&pixelFormat);
            if (FAILED(failedResult)) break;
            failedStage = "write PNG pixels";
            failedResult = frame->WriteSource(bitmap, nullptr);
            if (FAILED(failedResult)) break;
            failedStage = "commit PNG frame";
            failedResult = frame->Commit();
            if (FAILED(failedResult)) break;
            failedStage = "commit PNG encoder";
            failedResult = encoder->Commit();
            if (FAILED(failedResult)) break;
            success = true;
        } while (false);

        ReleaseCom(properties);
        ReleaseCom(frame);
        ReleaseCom(encoder);
        ReleaseCom(stream);
        ReleaseCom(bitmap);
        ReleaseCom(factory);
        if (shouldUninitialize) CoUninitialize();
        if (!success) {
            SKSE::log::warn("SkyCounterUI PNG save failed during {} for {:08X} (HRESULT 0x{:08X}, path '{}')",
                failedStage, job.formID, static_cast<std::uint32_t>(failedResult), job.path.string());
            DeleteFileW(temporary.c_str());
            return false;
        }
        if (!ReplaceFileFromTemporary(temporary, job.path, job.formID)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        DeleteFileW(temporary.c_str());
        if (!WriteRendererVersion(job.path)) {
            SKSE::log::warn("SkyCounterUI saved icon PNG for {:08X}, but could not update its renderer-version marker",
                job.formID);
        }
        return true;
    }

    void WriterMain()
    {
        for (;;) {
            PixelJob job;
            {
                std::unique_lock lock(g_queueMutex);
                g_pixelReady.wait(lock, [] { return !g_pixelQueue.empty(); });
                job = std::move(g_pixelQueue.front());
                g_pixelQueue.pop_front();
            }
            if (SavePng(job)) {
                SKSE::log::info("SkyCounterUI saved pickup icon: formID={:08X}, path='{}', dimensions={}x{}",
                    job.formID, job.path.string(), kOutputSize, kOutputSize);
                {
                    std::scoped_lock lock(g_queueMutex);
                    g_queuedFormIDs.erase(job.formID);
                }
                NotifyIconReady(job.formID, job.path);
                NotifyCaptureStatus(job.formID, SkyCounterUICaptureStatus::Ready);
            } else {
                SKSE::log::warn("SkyCounterUI could not save rendered icon for {:08X}", job.formID);
                NotifyCaptureStatus(job.formID, SkyCounterUICaptureStatus::Failed);
                AllowIconCaptureRetry(job.formID);
            }
        }
    }

    bool SaveLocationFramePng(LocationFramePixelJob& job)
    {
        if (job.path.empty() || !job.width || !job.height || job.bgra.empty() ||
            job.width > (std::numeric_limits<UINT>::max)() / 4u ||
            static_cast<std::uint64_t>(job.width) * job.height * 4u != job.bgra.size()) {
            SKSE::log::warn("SkyCounterUI rejected invalid location frame pixels for '{}'", job.path.string());
            return false;
        }
        if (job.sourceFormat != 87u && job.sourceFormat != 91u) {
            for (std::size_t pixelOffset = 0; pixelOffset + 3 < job.bgra.size(); pixelOffset += 4) {
                if (job.sourceFormat == 24u) {
                    std::uint32_t packed{};
                    std::memcpy(&packed, job.bgra.data() + pixelOffset, sizeof(packed));
                    const auto toByte = [](std::uint32_t value, std::uint32_t maximum) {
                        return static_cast<std::uint8_t>((value * 255u + maximum / 2u) / maximum);
                    };
                    job.bgra[pixelOffset] = toByte((packed >> 20) & 0x3FFu, 0x3FFu);
                    job.bgra[pixelOffset + 1] = toByte((packed >> 10) & 0x3FFu, 0x3FFu);
                    job.bgra[pixelOffset + 2] = toByte(packed & 0x3FFu, 0x3FFu);
                } else {
                    std::swap(job.bgra[pixelOffset], job.bgra[pixelOffset + 2]);
                }
                job.bgra[pixelOffset + 3] = 255;
            }
        } else {
            for (std::size_t pixelOffset = 3; pixelOffset < job.bgra.size(); pixelOffset += 4) {
                job.bgra[pixelOffset] = 255;
            }
        }
        std::error_code error;
        std::filesystem::create_directories(job.path.parent_path(), error);
        if (error) {
            SKSE::log::warn("SkyCounterUI could not create location image directory '{}': {}",
                job.path.parent_path().string(), error.message());
            return false;
        }

        static std::atomic<std::uint64_t> temporarySequence{};
        // The temporary file must live on the same volume as the destination:
        // MoveFileEx cannot atomically publish across volumes (ERROR_NOT_SAME_DEVICE).
        const auto temporary = job.path.parent_path() /
            (L"SkyCounterUI-Location-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(temporarySequence.fetch_add(1, std::memory_order_relaxed)) + L"-" +
                std::to_wstring(GetTickCount64()) + L".png");

        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(comResult);
        IWICImagingFactory* factory = nullptr;
        IWICBitmap* bitmap = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;
        bool success = false;
        HRESULT failedResult = S_OK;
        const char* failedStage = "WIC imaging factory";
        do {
            failedResult = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
            if (FAILED(failedResult)) break;
            failedStage = "create bitmap from frame readback";
            failedResult = factory->CreateBitmapFromMemory(job.width, job.height,
                GUID_WICPixelFormat32bppBGRA, job.width * 4u,
                static_cast<UINT>(job.bgra.size()), const_cast<BYTE*>(job.bgra.data()), &bitmap);
            if (FAILED(failedResult)) break;
            failedStage = "create temporary PNG stream";
            failedResult = factory->CreateStream(&stream);
            if (FAILED(failedResult)) break;
            failedResult = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE);
            if (FAILED(failedResult)) break;
            failedStage = "create PNG encoder";
            failedResult = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            if (FAILED(failedResult)) break;
            failedResult = encoder->Initialize(stream, WICBitmapEncoderNoCache);
            if (FAILED(failedResult)) break;
            failedStage = "create PNG frame";
            failedResult = encoder->CreateNewFrame(&frame, &properties);
            if (FAILED(failedResult)) break;
            failedResult = frame->Initialize(properties);
            if (FAILED(failedResult)) break;
            failedStage = "set PNG dimensions";
            failedResult = frame->SetSize(job.width, job.height);
            if (FAILED(failedResult)) break;
            WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
            failedStage = "set PNG pixel format";
            failedResult = frame->SetPixelFormat(&pixelFormat);
            if (FAILED(failedResult)) break;
            failedStage = "write PNG pixels";
            failedResult = frame->WriteSource(bitmap, nullptr);
            if (FAILED(failedResult)) break;
            failedStage = "commit PNG frame";
            failedResult = frame->Commit();
            if (FAILED(failedResult)) break;
            failedStage = "commit PNG encoder";
            failedResult = encoder->Commit();
            if (FAILED(failedResult)) break;
            success = true;
        } while (false);

        ReleaseCom(properties);
        ReleaseCom(frame);
        ReleaseCom(encoder);
        ReleaseCom(stream);
        ReleaseCom(bitmap);
        ReleaseCom(factory);
        if (shouldUninitialize) CoUninitialize();
        if (!success) {
            SKSE::log::warn("SkyCounterUI location PNG save failed during {} (HRESULT 0x{:08X}, path '{}')",
                failedStage, static_cast<std::uint32_t>(failedResult), job.path.string());
            DeleteFileW(temporary.c_str());
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), job.path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto publishError = GetLastError();
            SKSE::log::warn("SkyCounterUI could not publish location PNG from '{}' to '{}' (Win32 error {})",
                temporary.string(), job.path.string(), publishError);
            DeleteFileW(temporary.c_str());
            return false;
        }
        DeleteFileW(temporary.c_str());
        return true;
    }

    void LocationFrameWriterMain()
    {
        for (;;) {
            LocationFramePixelJob job;
            {
                std::unique_lock lock(g_locationFrameMutex);
                g_locationFramePixelReady.wait(lock, [] { return !g_locationFramePixelQueue.empty(); });
                job = std::move(g_locationFramePixelQueue.front());
                g_locationFramePixelQueue.pop_front();
            }
            const bool saved = SaveLocationFramePng(job);
            if (saved) {
                SKSE::log::info("SkyCounterUI saved {}x{} location game-frame PNG '{}'",
                    job.width, job.height, job.path.string());
            } else {
                SKSE::log::warn("SkyCounterUI read back {}x{} pixels but could not publish the location PNG '{}'",
                    job.width, job.height, job.path.string());
            }
            if (job.completion) job.completion(saved);
        }
    }

    bool IsInventoryPreviewMenuOpen(RE::UI* ui)
    {
        return ui && (ui->IsMenuOpen("InventoryMenu") || ui->IsMenuOpen("ContainerMenu") ||
            ui->IsMenuOpen("BarterMenu") || ui->IsMenuOpen("GiftMenu") ||
            ui->IsMenuOpen("Crafting Menu") || ui->IsMenuOpen("Alchemy Menu") ||
            ui->IsMenuOpen("Smithing Menu") || ui->IsMenuOpen("Enchanting Menu") ||
            ui->IsMenuOpen("Cooking Menu"));
    }

    bool InventoryItemIsReady(RE::Inventory3DManager* manager, RE::TESBoundObject* item,
        const std::string& modelPath)
    {
        if (!manager || !item) return false;
        auto& runtime = manager->GetRuntimeData();
        if (static_cast<bool>(runtime.loadTask)) return false;
        const auto wantedPath = TextureCacheKey(NormalizeModelDatabaseName(modelPath));
        for (const auto& loaded : runtime.loadedModels) {
            if (!loaded.itemBase || !loaded.modelObj || !loaded.spModel ||
                loaded.spModel->worldBound.radius <= 0.0f) continue;
            if (loaded.itemBase == item || loaded.modelObj == item) return true;
            // Skyrim can deduplicate preview models that share the same NIF,
            // leaving the first form in loadedModels while servicing a later
            // request for the same model path.
            if (!wantedPath.empty() &&
                TextureCacheKey(NormalizeModelDatabaseName(ItemModelPath(loaded.modelObj))) == wantedPath) return true;
        }
        return false;
    }

    bool InventoryItemIsLoadedByMenu(RE::Inventory3DManager* manager, RE::TESBoundObject* item,
        const std::string& modelPath)
    {
        if (!manager || !item) return false;
        const auto wantedPath = TextureCacheKey(NormalizeModelDatabaseName(modelPath));
        for (const auto& loaded : manager->GetRuntimeData().loadedModels) {
            if (!loaded.spModel || loaded.spModel->worldBound.radius <= 0.0f) continue;
            if (loaded.itemBase == item || loaded.modelObj == item) return true;
            // Skyrim can reuse one loaded preview object for several forms
            // that share a NIF. Match the requested model as well as the form
            // pointer so menu previews remain usable for those pickups.
            if (!wantedPath.empty() && loaded.modelObj &&
                TextureCacheKey(NormalizeModelDatabaseName(ItemModelPath(loaded.modelObj))) == wantedPath) {
                return true;
            }
        }
        return false;
    }

    RE::NiAVObject* FindLoadedInventoryPreview(RE::Inventory3DManager* manager,
        RE::TESBoundObject* item, const std::string& modelPath)
    {
        if (!manager || !item) return nullptr;
        const auto wantedPath = TextureCacheKey(NormalizeModelDatabaseName(modelPath));
        for (const auto& loaded : manager->GetRuntimeData().loadedModels) {
            if (!loaded.spModel || loaded.spModel->worldBound.radius <= 0.0f) continue;
            if (loaded.itemBase == item || loaded.modelObj == item) return loaded.spModel.get();
            // The manager may deduplicate requests that share the same NIF.
            if (!wantedPath.empty() && loaded.modelObj &&
                TextureCacheKey(NormalizeModelDatabaseName(ItemModelPath(loaded.modelObj))) == wantedPath) {
                return loaded.spModel.get();
            }
        }
        return nullptr;
    }

    bool EnsureInventoryPreviewAttached(RE::Inventory3DManager* manager,
        RE::TESBoundObject* item, const std::string& modelPath, RE::FormID formID)
    {
        auto* model = FindLoadedInventoryPreview(manager, item, modelPath);
        auto* scene = RE::UI3DSceneManager::GetSingleton();
        constexpr auto scheme = RE::INTERFACE_LIGHT_SCHEME::kInventory;
        const auto schemeIndex = static_cast<std::size_t>(scheme);
        auto* sceneRoot = scene && schemeIndex < 8 ?
            scene->menuObjects[schemeIndex].get() : nullptr;
        if (!model || !scene || !sceneRoot) {
            SKSE::log::warn("SkyCounterUI cannot verify inventory preview scene attachment for {:08X}: model=0x{:X}, scene=0x{:X}, inventory root=0x{:X}",
                formID, reinterpret_cast<std::uintptr_t>(model),
                reinterpret_cast<std::uintptr_t>(scene),
                reinterpret_cast<std::uintptr_t>(sceneRoot));
            return false;
        }

        auto* ancestor = model;
        for (std::uint32_t depth = 0; ancestor && depth < 16; ++depth, ancestor = ancestor->parent) {
            if (ancestor == sceneRoot) return true;
        }

        if (model->parent) {
            SKSE::log::warn("SkyCounterUI inventory preview node {:08X} is parented outside the inventory scene root: node=0x{:X}, parent=0x{:X}, expected root=0x{:X}; preserving Skyrim's existing parent",
                formID, reinterpret_cast<std::uintptr_t>(model),
                reinterpret_cast<std::uintptr_t>(model->parent),
                reinterpret_cast<std::uintptr_t>(sceneRoot));
            return false;
        }

        // The manager's async load can populate loadedModels before the scene
        // graph is ready. Render() only traverses UI3DSceneManager roots, so a
        // parentless loaded node produces a successful but completely blank
        // backbuffer capture. Attach only this known-parentless preview to the
        // inventory root; never steal a node from another menu/light scheme.
        SKSE::log::warn("SkyCounterUI inventory preview node {:08X} was loaded but parentless; attaching it to the inventory UI3D scene root",
            formID);
        scene->AttachChild(model, scheme);
        RE::NiUpdateData updateData{};
        model->Update(updateData);
        if (model->parent != sceneRoot) {
            SKSE::log::error("SkyCounterUI inventory scene attach did not take for {:08X}: node parent=0x{:X}, expected root=0x{:X}",
                formID, reinterpret_cast<std::uintptr_t>(model->parent),
                reinterpret_cast<std::uintptr_t>(sceneRoot));
            return false;
        }
        SKSE::log::info("SkyCounterUI attached inventory preview node {:08X} to scene root 0x{:X}",
            formID, reinterpret_cast<std::uintptr_t>(sceneRoot));
        return true;
    }

    bool InventorySceneCanEnd(RE::Inventory3DManager* manager)
    {
        if (!manager) return false;
        const auto& runtime = manager->GetRuntimeData();
        if (static_cast<bool>(runtime.loadTask)) return false;
        for (const auto& loaded : runtime.loadedModels) {
            if (!loaded.itemBase || !loaded.modelObj || !loaded.spModel) return false;
        }
        return true;
    }

    bool PositionInventoryPreviewAtScreenCenter(RE::Inventory3DManager* manager)
    {
        auto* scene = RE::UI3DSceneManager::GetSingleton();
        const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
        if (!manager || !scene || !screen.width || !screen.height) return false;

        // Inventory menus position the preview in the UI3D camera frustum
        // before loading it. The hidden pickup-capture menu must do the same,
            // otherwise the engine can render the model outside the capture area.
        float depth = manager->itemPos.y;
        if (depth > -1.0f) depth = -500.0f;
        const auto& frustum = scene->viewFrustum;
        const float worldMinX = -frustum.fLeft * depth;
        const float worldMinZ = -frustum.fBottom * depth;
        const float worldWidth = -frustum.fRight * depth - worldMinX;
        const float worldHeight = -frustum.fTop * depth - worldMinZ;
        if (worldWidth == 0.0f || worldHeight == 0.0f) return false;

        const float ratioX = worldWidth / static_cast<float>(screen.width);
        const float ratioY = worldHeight / static_cast<float>(screen.height);
        const RE::NiPoint3 position{
            -static_cast<float>(screen.width) * 0.5f * ratioX - worldMinX,
            depth,
            -static_cast<float>(screen.height) * 0.5f * ratioY - worldMinZ
        };
        manager->itemPos = position;
        manager->itemPosCopy = position;
        return true;
    }

    bool PositionLoadedInventoryPreviewAtScreenCenter(RE::Inventory3DManager* manager,
        RE::TESBoundObject* item, const std::string& modelPath)
    {
        auto* model = FindLoadedInventoryPreview(manager, item, modelPath);
        auto* scene = RE::UI3DSceneManager::GetSingleton();
        const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
        if (!model || !scene || !screen.width || !screen.height) return false;

        // Match the in-game inventory preview's placement. Positioning the
        // manager before LoadInventoryItem only affects newly created nodes;
        // the completed async node must be moved and updated after it lands.
        auto& translation = model->local.translate;
        if (translation.y > -1.0f) translation.y = -500.0f;
        RE::NiUpdateData updateData{};
        model->Update(updateData);

        const auto& frustum = scene->viewFrustum;
        const float worldMinX = -frustum.fLeft * translation.y;
        const float worldMinZ = -frustum.fBottom * translation.y;
        const float worldWidth = -frustum.fRight * translation.y - worldMinX;
        const float worldHeight = -frustum.fTop * translation.y - worldMinZ;
        if (worldWidth == 0.0f || worldHeight == 0.0f) return false;

        const float ratioX = worldWidth / static_cast<float>(screen.width);
        const float ratioY = worldHeight / static_cast<float>(screen.height);
        if (ratioX == 0.0f || ratioY == 0.0f) return false;

        const auto& center = model->worldBound.center;
        const float modelScreenX = -(center.x + worldMinX) / ratioX;
        const float modelScreenY = -(center.z + worldMinZ) / ratioY;
        translation.x += -(static_cast<float>(screen.width) * 0.5f - modelScreenX) * ratioX;
        translation.z += -(static_cast<float>(screen.height) * 0.5f - modelScreenY) * ratioY;
        model->Update(updateData);
        return true;
    }

    void LogInventoryPreviewState(RE::Inventory3DManager* manager,
        RE::TESBoundObject* item, const std::string& modelPath, RE::FormID formID)
    {
        if (!manager) {
            SKSE::log::warn("SkyCounterUI preview state {:08X}: Inventory3DManager is null", formID);
            return;
        }
        auto& runtime = manager->GetRuntimeData();
        auto* scene = RE::UI3DSceneManager::GetSingleton();
        auto* model = FindLoadedInventoryPreview(manager, item, modelPath);
        SKSE::log::info("SkyCounterUI preview state {:08X}: item=0x{:X}, requested model='{}', manager=0x{:X}, loadedModels={}, loadTaskActive={}, manager light scheme={}, scene=0x{:X}, scene light scheme={}, itemPos=({:.3f},{:.3f},{:.3f}), itemScale={:.3f}",
            formID, reinterpret_cast<std::uintptr_t>(item), modelPath,
            reinterpret_cast<std::uintptr_t>(manager), runtime.loadedModels.size(),
            static_cast<bool>(runtime.loadTask),
            static_cast<std::uint32_t>(manager->currentLightScheme),
            reinterpret_cast<std::uintptr_t>(scene),
            scene ? static_cast<std::uint32_t>(scene->currentlightScheme) : 0u,
            manager->itemPos.x, manager->itemPos.y, manager->itemPos.z,
            manager->itemScale);
        if (scene) {
            const auto& frustum = scene->viewFrustum;
            SKSE::log::info("SkyCounterUI preview camera {:08X}: cached position=({:.3f},{:.3f},{:.3f}), frustum L/R/T/B/N/F=({:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f}), orthographic={}",
                formID, scene->cachedCameraPos.x, scene->cachedCameraPos.y,
                scene->cachedCameraPos.z, frustum.fLeft, frustum.fRight,
                frustum.fTop, frustum.fBottom,
                frustum.fNear, frustum.fFar, frustum.bOrtho);
        }
        if (!model) {
            SKSE::log::warn("SkyCounterUI preview state {:08X}: no matching NiAVObject found in Inventory3DManager loadedModels", formID);
        } else {
            const auto& local = model->local.translate;
            const auto& world = model->world.translate;
            const auto& bound = model->worldBound;
            const auto* inventoryRoot = scene ? scene->menuObjects[
                static_cast<std::size_t>(RE::INTERFACE_LIGHT_SCHEME::kInventory)].get() : nullptr;
            SKSE::log::info("SkyCounterUI preview node {:08X}: node=0x{:X}, parent=0x{:X}, inventory root=0x{:X}, local=({:.3f},{:.3f},{:.3f}), world=({:.3f},{:.3f},{:.3f}), bound center=({:.3f},{:.3f},{:.3f}) radius={:.3f}, local scale={:.3f}",
                formID, reinterpret_cast<std::uintptr_t>(model),
                reinterpret_cast<std::uintptr_t>(model->parent),
                reinterpret_cast<std::uintptr_t>(inventoryRoot),
                local.x, local.y, local.z, world.x, world.y, world.z,
                bound.center.x, bound.center.y, bound.center.z, bound.radius,
                model->local.scale);
        }
        for (std::size_t index = 0; index < runtime.loadedModels.size(); ++index) {
            const auto& loaded = runtime.loadedModels[index];
            SKSE::log::info("SkyCounterUI loaded inventory model {:08X}[{}]: itemBase=0x{:X}, modelObj=0x{:X}, node=0x{:X}, light scheme={}, stored bound radius={:.3f}",
                formID, index, reinterpret_cast<std::uintptr_t>(loaded.itemBase),
                reinterpret_cast<std::uintptr_t>(loaded.modelObj),
                reinterpret_cast<std::uintptr_t>(loaded.spModel.get()),
                static_cast<std::uint32_t>(loaded.lightScheme), loaded.boundRadius);
        }
    }

    bool FinishCurrentCapture()
    {
        const auto finishedFormID = g_activeRequest.formID;
        const bool captureFailed = g_phase.load(std::memory_order_acquire) == CapturePhase::Failed;
        if (auto* manager = RE::Inventory3DManager::GetSingleton()) {
            auto* ui = RE::UI::GetSingleton();
            if (g_activeRequest.ownsInventoryScene) {
                if (captureFailed) {
                    // UnloadInventoryItem explicitly cancels a pending manager
                    // load. This keeps one missing/slow preview from leaving
                    // the singleton occupied and blocking every later pickup.
                    manager->UnloadInventoryItem();
                }
                if (IsInventoryPreviewMenuOpen(ui)) {
                    g_inventorySceneClaimed = false;
                } else if (InventorySceneCanEnd(manager)) {
                    manager->End3D();
                    g_inventorySceneClaimed = false;
                } else {
                    // Keep an incomplete manager scene alive. End3D walks its
                    // loadedModels array and is unsafe while a model load is
                    // still in flight; the next request can reuse this scene.
                    g_inventorySceneClaimed = true;
                    SKSE::log::warn("SkyCounterUI left Inventory3DManager active because its scene is not safe to tear down");
                }
            } else if (g_activeRequest.restoreItem && IsInventoryPreviewMenuOpen(ui)) {
                manager->LoadInventoryItem(g_activeRequest.restoreItem, nullptr);
            }
        }
        g_inventory3DPreviewCapture.CancelPass();
        g_activeRequest = {};
        g_gameFrames.store(0, std::memory_order_relaxed);
        g_previewCapturePass = 0;
        g_loadTimeoutLogged = false;
        g_managerPumpWarningLogged = false;
        g_phase.store(CapturePhase::Idle, std::memory_order_release);
        if (captureFailed) {
            NotifyCaptureStatus(finishedFormID, SkyCounterUICaptureStatus::Failed);
            AllowIconCaptureRetry(finishedFormID);
        }
        return true;
    }

    void DeferActiveCaptureForInventoryMenu()
    {
        const auto phase = g_phase.load(std::memory_order_acquire);
        if (!g_activeRequest.formID || (phase != CapturePhase::Loading && phase != CapturePhase::Ready) ||
            g_readbackPending.load(std::memory_order_acquire)) return;

        const auto formID = g_activeRequest.formID;
        SetCaptureMenuPaused(false);
        g_inventory3DPreviewCapture.CancelPass();
        if (g_captureEnabled.load(std::memory_order_acquire)) {
            auto deferred = std::move(g_activeRequest);
            deferred.item = nullptr;
            deferred.restoreItem = nullptr;
            deferred.ownsInventoryScene = false;
            std::scoped_lock lock(g_queueMutex);
            g_captureQueue.push_front(std::move(deferred));
            SKSE::log::info("SkyCounterUI deferred pickup preview {:08X} until Skyrim's inventory or crafting menu closes",
                formID);
        } else {
            g_activeRequest = {};
            SKSE::log::info("SkyCounterUI discarded pending pickup preview {:08X} because icon rendering is disabled",
                formID);
            AllowIconCaptureRetry(formID);
        }
        g_activeRequest = {};
        g_gameFrames.store(0, std::memory_order_relaxed);
        g_previewCapturePass = 0;
        g_loadTimeoutLogged = false;
        g_managerPumpWarningLogged = false;
        g_phase.store(CapturePhase::Idle, std::memory_order_release);
    }

    bool SetCaptureMenuOpen(bool shouldOpen)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;

        if (shouldOpen) {
            if (!g_captureMenuRegistered.exchange(true, std::memory_order_acq_rel)) {
                ui->Register(kRenderMenuName, &CreateSkyCounterUIRenderMenu);
                SKSE::log::info("SkyCounterUI registered its non-pausing off-screen render callback");
            }
            if (ui->IsMenuOpen(kRenderMenuName)) {
                g_captureMenuShowRequested.store(CaptureMenuKind::None, std::memory_order_release);
                g_captureMenuShowRequestedAtMs.store(0, std::memory_order_release);
                g_captureMenuHideRequested.store(false, std::memory_order_release);
                g_captureMenuWaitLogged.store(false, std::memory_order_release);
                return true;
            }
            auto* messages = RE::UIMessageQueue::GetSingleton();
            auto pendingKind = g_captureMenuShowRequested.load(std::memory_order_acquire);
            if (pendingKind == CaptureMenuKind::Render) {
                const auto requestedAtMs = g_captureMenuShowRequestedAtMs.load(std::memory_order_acquire);
                const auto nowMs = static_cast<std::uint64_t>(GetTickCount64());
                const auto requestAgeMs = requestedAtMs && nowMs >= requestedAtMs ?
                    nowMs - requestedAtMs : 0;
                const bool playerPreviewMenuOpen = IsInventoryPreviewMenuOpen(ui);
                if (!playerPreviewMenuOpen && requestedAtMs &&
                    requestAgeMs >= kCaptureMenuShowRetryMilliseconds) {
                    auto expectedKind = CaptureMenuKind::Render;
                    if (g_captureMenuShowRequested.compare_exchange_strong(expectedKind,
                            CaptureMenuKind::None, std::memory_order_acq_rel)) {
                        g_captureMenuShowRequestedAtMs.store(0, std::memory_order_release);
                        g_captureMenuWaitLogged.store(false, std::memory_order_release);
                        SKSE::log::warn("SkyCounterUI is retrying a stale render-menu show request: it remained pending for {} ms while no player preview menu is open; queueSize={}, phase={}, thread={}",
                            requestAgeMs, CaptureQueueSnapshot(),
                            CapturePhaseName(g_phase.load(std::memory_order_acquire)), GetCurrentThreadId());
                        pendingKind = CaptureMenuKind::None;
                    } else {
                        return false;
                    }
                } else {
                    if (!g_captureMenuWaitLogged.exchange(true, std::memory_order_acq_rel)) {
                        RE::FormID queuedFormID{};
                        const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                        SKSE::log::warn("SkyCounterUI is waiting for its render menu to open: pending show request exists, requestAgeMs={}, playerPreviewMenuOpen={}, UI reports render menu closed, queueSize={}, firstQueuedFormID={:08X}, messageQueueAvailable={}, phase={}, thread={}",
                            requestAgeMs, playerPreviewMenuOpen, queueSize, queuedFormID,
                            messages != nullptr,
                            CapturePhaseName(g_phase.load(std::memory_order_acquire)), GetCurrentThreadId());
                    }
                    return false;
                }
            }
            g_captureMenuHideRequested.store(false, std::memory_order_release);
            if (messages && g_captureMenuShowRequested.exchange(CaptureMenuKind::Render, std::memory_order_acq_rel) != CaptureMenuKind::Render) {
                g_captureMenuShowRequestedAtMs.store(static_cast<std::uint64_t>(GetTickCount64()),
                    std::memory_order_release);
                messages->AddMessage(RE::BSFixedString(kRenderMenuName), RE::UI_MESSAGE_TYPE::kShow, nullptr);
                RE::FormID queuedFormID{};
                const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                SKSE::log::info("SkyCounterUI requested its render service menu: queueSize={}, firstQueuedFormID={:08X}, phase={}, thread={}",
                    queueSize, queuedFormID,
                    CapturePhaseName(g_phase.load(std::memory_order_acquire)), GetCurrentThreadId());
            } else if (!messages) {
                SKSE::log::error("SkyCounterUI cannot show its render service menu because UIMessageQueue is unavailable; queueSize={}, phase={}, thread={}",
                    CaptureQueueSnapshot(),
                    CapturePhaseName(g_phase.load(std::memory_order_acquire)), GetCurrentThreadId());
            }
            return false;
        }

        // Always resume before hiding the service menu. In particular, a
        // capture failure or disabled queue must never leave gameplay paused.
        SetCaptureMenuPaused(false);

        const auto pendingKind = g_captureMenuShowRequested.exchange(CaptureMenuKind::None, std::memory_order_acq_rel);
        if (pendingKind != CaptureMenuKind::None) {
            g_captureMenuShowRequestedAtMs.store(0, std::memory_order_release);
            auto* messages = RE::UIMessageQueue::GetSingleton();
            if (messages && !g_captureMenuHideRequested.exchange(true, std::memory_order_acq_rel)) {
                messages->AddMessage(RE::BSFixedString(kRenderMenuName), RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
            }
            return false;
        }
        if (ui->IsMenuOpen(kRenderMenuName)) {
            auto* messages = RE::UIMessageQueue::GetSingleton();
            if (messages && !g_captureMenuHideRequested.exchange(true, std::memory_order_acq_rel)) {
                messages->AddMessage(RE::BSFixedString(kRenderMenuName), RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
            }
            return false;
        }
        g_captureMenuHideRequested.store(false, std::memory_order_release);
        return true;
    }

    void GamePump(std::uint64_t taskSequence)
    {
        g_gamePumpScheduled.store(false, std::memory_order_release);
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            SKSE::log::error("SkyCounterUI capture pump task {} ran on thread {} but RE::UI::GetSingleton() returned null",
                taskSequence, GetCurrentThreadId());
            return;
        }

        const auto startingPhase = g_phase.load(std::memory_order_acquire);
        const bool playerPreviewMenuOpen = IsInventoryPreviewMenuOpen(ui);
        bool queued = false;
        if (!playerPreviewMenuOpen) RequeueMenuDeferredCaptures();
        {
            std::scoped_lock lock(g_queueMutex);
            queued = !g_captureQueue.empty();
        }
        const bool readbackPending = g_readbackPending.load(std::memory_order_acquire);
        const bool locationFrameNeeded = HasLocationFrameCaptureWork();
        if (queued || startingPhase != CapturePhase::Idle || readbackPending ||
            g_pendingOwnedSceneTeardown.load(std::memory_order_acquire)) {
            const auto waitSample = queued && startingPhase == CapturePhase::Idle ?
                g_gamePumpQueueWaitCount.fetch_add(1, std::memory_order_acq_rel) + 1 : 0;
            if (!waitSample || waitSample == 1 || waitSample % 60 == 0) {
                RE::FormID queuedFormID{};
                const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                SKSE::log::info("SkyCounterUI capture pump task {} entered: thread={}, phase={}, queueSize={}, firstQueuedFormID={:08X}, playerPreviewMenuOpen={}, captureRenderMenuOpen={}, showRequest={}, hideRequested={}, captureEnabled={}, readbackPending={}, locationFrameNeeded={}, ownedSceneTeardownPending={}, gamePaused={}, pauseCount={}",
                    taskSequence, GetCurrentThreadId(), CapturePhaseName(startingPhase), queueSize,
                    queuedFormID, playerPreviewMenuOpen, ui->IsMenuOpen(kRenderMenuName),
                    static_cast<std::uint32_t>(g_captureMenuShowRequested.load(std::memory_order_acquire)),
                    g_captureMenuHideRequested.load(std::memory_order_acquire),
                    g_captureEnabled.load(std::memory_order_acquire), readbackPending,
                    locationFrameNeeded,
                    g_pendingOwnedSceneTeardown.load(std::memory_order_acquire),
                    ui->GameIsPaused(), ui->numPausesGame);
            }
        } else {
            g_gamePumpQueueWaitCount.store(0, std::memory_order_release);
        }
        const bool needsTeardown = (startingPhase == CapturePhase::Captured ||
            startingPhase == CapturePhase::Failed) && !readbackPending;
        const bool captureMenuNeeded = startingPhase == CapturePhase::Loading ||
                startingPhase == CapturePhase::Ready ||
                readbackPending || needsTeardown ||
                locationFrameNeeded ||
                g_pendingOwnedSceneTeardown.load(std::memory_order_acquire) ||
                (g_captureEnabled.load(std::memory_order_acquire) && queued &&
                    (startingPhase == CapturePhase::Idle || startingPhase == CapturePhase::Captured ||
                        startingPhase == CapturePhase::Failed));
        if (!SetCaptureMenuOpen(captureMenuNeeded)) {
            // MenuOpenCloseEvent and the frame callback resume the pump. Do not
            // poll AddTask here: draining a task queue can starve Skyrim's UI.
            if (captureMenuNeeded && !g_captureMenuWaitLogged.exchange(true, std::memory_order_acq_rel)) {
                RE::FormID queuedFormID{};
                const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                SKSE::log::info("SkyCounterUI capture pump task {} yielded while the render service menu transition is pending: queueSize={}, firstQueuedFormID={:08X}, phase={}, playerPreviewMenuOpen={}, renderMenuOpen={}, showRequest={}, hideRequested={}, UIMessageQueueAvailable={}, thread={}",
                    taskSequence, queueSize, queuedFormID,
                    CapturePhaseName(startingPhase), playerPreviewMenuOpen,
                    ui->IsMenuOpen(kRenderMenuName),
                    static_cast<std::uint32_t>(g_captureMenuShowRequested.load(std::memory_order_acquire)),
                    g_captureMenuHideRequested.load(std::memory_order_acquire),
                    RE::UIMessageQueue::GetSingleton() != nullptr, GetCurrentThreadId());
            }
            return;
        }
        if (captureMenuNeeded && !ui->IsMenuOpen(kRenderMenuName)) {
            if (!g_captureMenuWaitLogged.exchange(true, std::memory_order_acq_rel)) {
                RE::FormID queuedFormID{};
                const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                SKSE::log::warn("SkyCounterUI capture pump task {} returned because its render menu is still closed after the show request: showRequest={}, queueSize={}, firstQueuedFormID={}, phase={}, thread={}",
                    taskSequence,
                    static_cast<std::uint32_t>(g_captureMenuShowRequested.load(std::memory_order_acquire)),
                    queueSize, queuedFormID,
                    CapturePhaseName(startingPhase), GetCurrentThreadId());
            }
            return;
        }

        if (g_pendingOwnedSceneTeardown.load(std::memory_order_acquire) && !playerPreviewMenuOpen) {
            auto* manager = RE::Inventory3DManager::GetSingleton();
            if (manager && InventorySceneCanEnd(manager)) {
                manager->End3D();
            } else if (manager) {
                // The menu may have left a load task or a partial preview in
                // the shared manager. Do not call End3D on that state and do
                // not let it hold the capture queue hostage; the frame pump
                // below will finish or cancel a remaining load task.
                SKSE::log::warn("SkyCounterUI yielded an incomplete Inventory3DManager scene after the player menu closed");
            }
            g_inventorySceneClaimed = false;
            g_pendingOwnedSceneTeardown.store(false, std::memory_order_release);
            SKSE::log::info("SkyCounterUI released ownership of the interrupted preview scene after Skyrim closed the inventory menu");
        }

        auto phase = g_phase.load(std::memory_order_acquire);
        const bool completingCapture = phase == CapturePhase::Captured ||
            phase == CapturePhase::Failed ||
            (phase == CapturePhase::Ready && !g_captureEnabled.load(std::memory_order_acquire));
        if (completingCapture) {
            SetCaptureMenuPaused(false);
        }
        bool finishedCapture = false;
        if (((phase == CapturePhase::Captured || phase == CapturePhase::Failed) &&
                !g_readbackPending.load(std::memory_order_acquire)) ||
            (phase == CapturePhase::Ready &&
                !g_captureEnabled.load(std::memory_order_acquire))) {
            finishedCapture = FinishCurrentCapture();
            if (finishedCapture) phase = CapturePhase::Idle;
            phase = g_phase.load(std::memory_order_acquire);
        }

        if (finishedCapture) {
            bool moreQueued = false;
            {
                std::scoped_lock lock(g_queueMutex);
                moreQueued = !g_captureQueue.empty();
            }
            if ((moreQueued && g_captureEnabled.load(std::memory_order_acquire)) ||
                HasLocationFrameCaptureWork()) {
                // Keep the non-pausing frame callback alive so the next item
                // can begin without waiting for another menu transition.
                ScheduleGamePump();
            } else {
                SetCaptureMenuOpen(false);
            }
            return;
        }

        if (phase == CapturePhase::Idle && playerPreviewMenuOpen &&
            g_pendingOwnedSceneTeardown.load(std::memory_order_acquire)) {
            const auto handoffFrame = g_menuPreviewHandoffFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (handoffFrame < 2) return;
            // Skyrim owns the open menu preview. Relinquish our scene claim
            // without waiting for the shared manager's load task or changing
            // its models while the menu is open.
            g_inventorySceneClaimed = false;
            g_pendingOwnedSceneTeardown.store(false, std::memory_order_release);
            g_menuPreviewHandoffFrames.store(0, std::memory_order_release);
            SKSE::log::info("SkyCounterUI yielded Inventory3DManager to Skyrim's open preview menu");
        }

        if (phase == CapturePhase::Loading) {
            // Inventory3DManager finishes model loading asynchronously; the
            // render callback moves the request to Ready once its model lands.
            return;
        }

        if (phase == CapturePhase::Idle && g_captureEnabled.load(std::memory_order_acquire) &&
            !g_readbackPending.load(std::memory_order_acquire)) {
            if (auto* manager = RE::Inventory3DManager::GetSingleton(); manager &&
                static_cast<bool>(manager->GetRuntimeData().loadTask) && !playerPreviewMenuOpen) {
                // Do not replace the manager's current request while Skyrim is
                // still loading its selected preview. The frame callback will
                // retry once that load task has completed.
                return;
            }
            CaptureRequest request;
            bool haveRequest = false;
            {
                std::scoped_lock lock(g_queueMutex);
                if (!g_captureQueue.empty()) {
                    request = std::move(g_captureQueue.front());
                    g_captureQueue.pop_front();
                    haveRequest = true;
                }
            }
            if (haveRequest) {
                auto* form = RE::TESForm::LookupByID(request.formID);
                request.item = form && form->IsBoundObject() ?
                                   static_cast<RE::TESBoundObject*>(form) :
                                   nullptr;
                auto* manager = RE::Inventory3DManager::GetSingleton();
                if (!request.item || !manager) {
                    SKSE::log::warn("SkyCounterUI could not start pickup capture: formID={:08X}, formFound={}, isBoundObject={}, managerAvailable={}",
                        request.formID, form != nullptr, form && form->IsBoundObject(), manager != nullptr);
                    NotifyCaptureStatus(request.formID, SkyCounterUICaptureStatus::Failed);
                    AllowIconCaptureRetry(request.formID);
                    ScheduleGamePump();
                    return;
                }

                const bool menuOwnsScene = IsInventoryPreviewMenuOpen(ui);
                request.modelPath = ItemModelPath(request.item);
                const bool runtimeGeneratedAlchemy = IsRuntimeGeneratedAlchemyItem(form);
                SKSE::log::info("SkyCounterUI starting queued icon capture: formID={:08X}, name='{}', formType={}, runtimeGeneratedAlchemy={}, modelPath='{}', playerPreviewMenuOpen={}, menuOwnsScene={}, queueRemaining={}, managerAvailable={}, managerLoadTaskActive={}, thread={}",
                    request.formID, form->GetName() ? form->GetName() : "Unknown item",
                    static_cast<std::uint32_t>(form->GetFormType()), runtimeGeneratedAlchemy, request.modelPath,
                    playerPreviewMenuOpen, menuOwnsScene, CaptureQueueSnapshot(),
                    manager != nullptr,
                    manager && static_cast<bool>(manager->GetRuntimeData().loadTask), GetCurrentThreadId());
                if (!menuOwnsScene && request.modelPath.empty()) {
                    SKSE::log::warn("SkyCounterUI skipped standalone preview load because the item has no usable model path: formID={:08X}, name='{}', formType={}, runtimeGeneratedAlchemy={}, modelPath='{}', menuOwnsScene={}, managerAvailable={}; later captures will continue",
                        request.formID, form->GetName() ? form->GetName() : "Unknown item",
                        static_cast<std::uint32_t>(form->GetFormType()), runtimeGeneratedAlchemy,
                        request.modelPath, menuOwnsScene, manager != nullptr);
                    NotifyCaptureStatus(request.formID, SkyCounterUICaptureStatus::Failed);
                    AllowIconCaptureRetry(request.formID);
                    ScheduleGamePump();
                    return;
                }
                if (!menuOwnsScene && runtimeGeneratedAlchemy) {
                    SKSE::log::info("SkyCounterUI attempting standalone preview for runtime-generated alchemy item {:08X}: valid modelPath='{}'; the normal model-load timeout and cancellation path is active",
                        request.formID, request.modelPath);
                }
                request.ownsInventoryScene = !menuOwnsScene;
                if (menuOwnsScene) {
                    request.usesMenuPreview = true;
                } else if (!g_inventorySceneClaimed) {
                    manager->Begin3D(RE::INTERFACE_LIGHT_SCHEME::kInventory);
                    g_inventorySceneClaimed = true;
                }

                if (!menuOwnsScene && !PositionInventoryPreviewAtScreenCenter(manager)) {
                    SKSE::log::warn("SkyCounterUI could not position Inventory3DManager preview for {:08X}; UI3D camera or screen dimensions are unavailable",
                        request.formID);
                }

                g_activeRequest = std::move(request);
                g_gameFrames.store(0, std::memory_order_relaxed);
                g_previewCapturePass = 0;
                g_loadTimeoutLogged = false;
                g_managerPumpWarningLogged = false;
                NotifyCaptureStatus(g_activeRequest.formID, SkyCounterUICaptureStatus::Rendering);
                if (g_activeRequest.usesMenuPreview) {
                    SKSE::log::info("SkyCounterUI will capture Skyrim's already-loaded menu preview for pickup {:08X} without loading or repositioning Inventory3DManager",
                        g_activeRequest.formID);
                } else {
                    manager->LoadInventoryItem(g_activeRequest.item, nullptr);
                    SKSE::log::info("SkyCounterUI requested an isolated Inventory3DManager preview for pickup {:08X}",
                        g_activeRequest.formID);
                }
                g_phase.store(CapturePhase::Loading, std::memory_order_release);
                phase = CapturePhase::Loading;
            }
        }
    }

    void ScheduleGamePump(const char* reason)
    {
        bool expected = false;
        if (!g_gamePumpScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            const auto coalescedCount = g_gamePumpCoalescedCount.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (coalescedCount == 1 || coalescedCount % 60 == 0) {
                RE::FormID queuedFormID{};
                const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
                SKSE::log::warn("SkyCounterUI coalesced capture-pump request #{}: reason='{}', an earlier task is still marked scheduled, phase={}, queueSize={}, firstQueuedFormID={:08X}, thread={}",
                    coalescedCount, reason ? reason : "unspecified",
                    CapturePhaseName(g_phase.load(std::memory_order_acquire)),
                    queueSize, queuedFormID, GetCurrentThreadId());
            }
            return;
        }
        g_gamePumpCoalescedCount.store(0, std::memory_order_release);
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            g_gamePumpScheduled.store(false, std::memory_order_release);
            if (!g_missingPumpTaskInterfaceLogged.exchange(true, std::memory_order_acq_rel)) {
                SKSE::log::error("SkyCounterUI could not schedule its capture pump because SKSE TaskInterface is unavailable: reason='{}', phase={}, queueSize={}, thread={}",
                    reason ? reason : "unspecified",
                    CapturePhaseName(g_phase.load(std::memory_order_acquire)),
                    CaptureQueueSnapshot(), GetCurrentThreadId());
            }
            return;
        }
        const auto taskSequence = g_gamePumpTaskSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        RE::FormID queuedFormID{};
        const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
        SKSE::log::info("SkyCounterUI submitted capture-pump task {}: reason='{}', phase={}, queueSize={}, firstQueuedFormID={:08X}, thread={}",
            taskSequence, reason ? reason : "unspecified",
            CapturePhaseName(g_phase.load(std::memory_order_acquire)),
            queueSize, queuedFormID, GetCurrentThreadId());
        tasks->AddTask([taskSequence] {
            SKSE::log::info("SkyCounterUI capture-pump task {} dispatched by SKSE TaskInterface on thread {}",
                taskSequence, GetCurrentThreadId());
            GamePump(taskSequence);
        });
    }

    void ProcessPendingReadback()
    {
        if (!g_readbackPending.load(std::memory_order_acquire) || !g_inventory3DPreviewCapture.IsPending()) return;
        PixelJob job{};
        const auto result = g_inventory3DPreviewCapture.Poll(job);
        if (result == OffscreenPoll::Pending) return;
        if (result == OffscreenPoll::Ready) {
            SKSE::log::info("SkyCounterUI read visible preview pixels for {:08X}", job.formID);
            {
                std::scoped_lock lock(g_queueMutex);
                g_pixelQueue.push_back(std::move(job));
            }
            g_pixelReady.notify_one();
        } else {
            SKSE::log::warn("SkyCounterUI Inventory3DManager preview capture failed for {:08X}", job.formID);
            NotifyCaptureStatus(job.formID, SkyCounterUICaptureStatus::Failed);
            AllowIconCaptureRetry(job.formID);
        }
        g_readbackPending.store(false, std::memory_order_release);
        ScheduleGamePump();
    }

    void ReleaseLocationFrameGpuResources()
    {
        if (g_locationFrameQuery) { g_locationFrameQuery->Release(); g_locationFrameQuery = nullptr; }
        if (g_locationFrameStaging) { g_locationFrameStaging->Release(); g_locationFrameStaging = nullptr; }
        if (g_locationFrameResolve) { g_locationFrameResolve->Release(); g_locationFrameResolve = nullptr; }
        g_locationFrameWidth = 0;
        g_locationFrameHeight = 0;
        g_locationFrameReadbackWaitFrames = 0;
        g_locationFrameMapWaitFrames = 0;
    }

    void FinishLocationFrameCapture(bool success)
    {
        auto request = std::move(g_activeLocationFrameRequest);
        g_activeLocationFrameRequest = {};
        ReleaseLocationFrameGpuResources();
        g_locationFrameReadbackPending.store(false, std::memory_order_release);
        g_locationFrameCancelRequested.store(false, std::memory_order_release);
        if (request.completion) request.completion(success);
        ScheduleGamePump();
    }

    void FailLocationFrameCapture(const char* reason, HRESULT result = S_OK)
    {
        SKSE::log::warn("SkyCounterUI non-pausing location frame capture failed for '{}' at {} (HRESULT 0x{:08X})",
            g_activeLocationFrameRequest.path.string(), reason,
            static_cast<std::uint32_t>(result));
        FinishLocationFrameCapture(false);
    }

    bool HasLocationFrameCaptureWork()
    {
        if (g_locationFrameReadbackPending.load(std::memory_order_acquire)) return true;
        std::scoped_lock lock(g_locationFrameMutex);
        return !g_locationFrameQueue.empty();
    }

    void ProcessLocationFrameCapture()
    {
        if (g_locationFrameCancelRequested.exchange(false, std::memory_order_acq_rel) &&
            g_locationFrameReadbackPending.load(std::memory_order_acquire)) {
            FinishLocationFrameCapture(false);
            return;
        }

        auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        auto* immediate = renderer ? reinterpret_cast<Context*>(renderer->context) : nullptr;
        auto* device = renderer ? reinterpret_cast<Device*>(renderer->forwarder) : nullptr;
        if (!immediate || !device) {
            if (g_locationFrameReadbackPending.load(std::memory_order_acquire)) {
                FailLocationFrameCapture("Skyrim D3D11 device or immediate context is unavailable", E_POINTER);
                return;
            }
        }

        if (!g_locationFrameReadbackPending.load(std::memory_order_acquire)) {
            LocationFrameRequest request;
            {
                std::scoped_lock lock(g_locationFrameMutex);
                if (g_locationFrameQueue.empty()) return;
                request = std::move(g_locationFrameQueue.front());
                g_locationFrameQueue.pop_front();
            }
            g_activeLocationFrameRequest = std::move(request);
            g_locationFrameReadbackPending.store(true, std::memory_order_release);
            if (!immediate || !device) {
                FailLocationFrameCapture("Skyrim D3D11 device or immediate context is unavailable", E_POINTER);
                return;
            }

            // PostDisplay runs from SkyCounterUI's hidden service menu. Its
            // currently-bound RTV can be a menu/offscreen surface, so location
            // screenshots must prefer the game's swap-chain view. That is the
            // completed scene the player actually sees before PrismaUI draws.
            Rtv* sourceView{};
            bool sourceIsSwapChain = false;
            if (renderer && renderer->renderWindows[0].renderView) {
                sourceView = reinterpret_cast<Rtv*>(renderer->renderWindows[0].renderView);
                sourceView->AddRef();
                sourceIsSwapChain = true;
            } else {
                immediate->OMGetRenderTargets(1, &sourceView, nullptr);
            }
            if (!sourceView) {
                FailLocationFrameCapture("Skyrim did not expose its swap-chain or currently bound render target", E_FAIL);
                return;
            }
            REX::W32::ID3D11Resource* resource{};
            sourceView->GetResource(&resource);
            sourceView->Release();
            if (!resource) {
                FailLocationFrameCapture("the bound render target did not expose a D3D resource", E_FAIL);
                return;
            }
            Texture2D* source{};
            const auto queryTexture = resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                reinterpret_cast<void**>(&source));
            resource->Release();
            if (FAILED(queryTexture) || !source) {
                if (source) source->Release();
                FailLocationFrameCapture("the bound render target is not a 2D texture", queryTexture);
                return;
            }

            REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};
            source->GetDesc(&sourceDesc);
            const auto format = static_cast<std::uint32_t>(sourceDesc.format);
            const bool supportedFormat = format == 28u || format == 29u ||
                format == 87u || format == 91u || format == 24u;
            const auto pixelCount = static_cast<std::uint64_t>(sourceDesc.width) * sourceDesc.height;
            SKSE::log::info("SkyCounterUI location capture source for '{}': {} RTV=0x{:X}, texture=0x{:X}, {}x{}, format {}, samples {}, usage {}, bind {}, mip levels {}, array size {}",
                g_activeLocationFrameRequest.path.string(),
                sourceIsSwapChain ? "game swap-chain" : "currently bound fallback",
                reinterpret_cast<std::uintptr_t>(sourceView),
                reinterpret_cast<std::uintptr_t>(source), sourceDesc.width,
                sourceDesc.height, format, sourceDesc.sampleDesc.count,
                static_cast<std::uint32_t>(sourceDesc.usage), sourceDesc.bindFlags,
                sourceDesc.mipLevels, sourceDesc.arraySize);
            if (!sourceDesc.width || !sourceDesc.height || !sourceDesc.sampleDesc.count ||
                !supportedFormat || pixelCount > 16ull * 1024ull * 1024ull) {
                source->Release();
                FailLocationFrameCapture("unsupported swap-chain/game render target descriptor", E_INVALIDARG);
                return;
            }

            auto stagingDesc = sourceDesc;
            stagingDesc.mipLevels = 1;
            stagingDesc.arraySize = 1;
            stagingDesc.sampleDesc.count = 1;
            stagingDesc.sampleDesc.quality = 0;
            stagingDesc.usage = REX::W32::D3D11_USAGE_STAGING;
            stagingDesc.bindFlags = 0;
            stagingDesc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;
            stagingDesc.miscFlags = 0;
            auto result = device->CreateTexture2D(&stagingDesc, nullptr, &g_locationFrameStaging);
            if (FAILED(result)) {
                source->Release();
                FailLocationFrameCapture("CreateTexture2D(location staging)", result);
                return;
            }

            {
                // D3D11 must not copy from a texture while it is still bound
                // as an output. Preserve and restore the hidden menu's OM
                // state around this one GPU copy; no pause or frame wait is
                // introduced.
                LocationOutputState savedOutput(immediate);
                savedOutput.UnbindTargets();
                if (sourceDesc.sampleDesc.count > 1) {
                    auto resolveDesc = stagingDesc;
                    resolveDesc.usage = REX::W32::D3D11_USAGE_DEFAULT;
                    resolveDesc.cpuAccessFlags = 0;
                    result = device->CreateTexture2D(&resolveDesc, nullptr, &g_locationFrameResolve);
                    if (FAILED(result)) {
                        source->Release();
                        FailLocationFrameCapture("CreateTexture2D(location resolve)", result);
                        return;
                    }
                    immediate->ResolveSubresource(g_locationFrameResolve, 0, source, 0, sourceDesc.format);
                    immediate->CopyResource(g_locationFrameStaging, g_locationFrameResolve);
                } else {
                    immediate->CopySubresourceRegion(g_locationFrameStaging, 0, 0, 0, 0, source, 0, nullptr);
                }
            }
            source->Release();

            REX::W32::D3D11_QUERY_DESC queryDesc{};
            queryDesc.query = REX::W32::D3D11_QUERY_EVENT;
            result = device->CreateQuery(&queryDesc, &g_locationFrameQuery);
            if (FAILED(result)) {
                FailLocationFrameCapture("CreateQuery(location readback)", result);
                return;
            }
            immediate->End(g_locationFrameQuery);
            g_locationFrameWidth = sourceDesc.width;
            g_locationFrameHeight = sourceDesc.height;
            g_locationFrameFormat = sourceDesc.format;
            SKSE::log::info("SkyCounterUI queued non-pausing location frame copy from the {} surface: {}x{}, format {}, samples {}",
                sourceIsSwapChain ? "game swap-chain" : "bound fallback",
                sourceDesc.width, sourceDesc.height, format, sourceDesc.sampleDesc.count);
            return;
        }

        if (!g_locationFrameQuery || !g_locationFrameStaging) {
            FailLocationFrameCapture("location GPU readback resources are incomplete", E_FAIL);
            return;
        }
        const auto queryResult = immediate->GetData(g_locationFrameQuery, nullptr, 0,
            REX::W32::D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (queryResult == S_FALSE) {
            if (++g_locationFrameReadbackWaitFrames < 180) return;
            FailLocationFrameCapture("GPU copy event query timed out", queryResult);
            return;
        }
        if (FAILED(queryResult)) {
            FailLocationFrameCapture("GPU copy event query failed", queryResult);
            return;
        }

        REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto mapResult = immediate->Map(g_locationFrameStaging, 0,
            REX::W32::D3D11_MAP_READ, REX::W32::D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (FAILED(mapResult)) {
            // Some drivers signal the event query before the staging resource
            // is immediately mappable. Treat DXGI_ERROR_WAS_STILL_DRAWING as a
            // normal asynchronous wait, rather than dropping the screenshot.
            if (static_cast<std::uint32_t>(mapResult) == 0x887A000Au &&
                ++g_locationFrameMapWaitFrames < 180) {
                return;
            }
            FailLocationFrameCapture("Map(location staging)", mapResult);
            return;
        }
        g_locationFrameMapWaitFrames = 0;
        if (!mapped.data || mapped.rowPitch < g_locationFrameWidth * 4u) {
            immediate->Unmap(g_locationFrameStaging, 0);
            FailLocationFrameCapture("Map(location staging) returned invalid pixel data or row pitch", E_FAIL);
            return;
        }

        LocationFramePixelJob job{};
        job.path = g_activeLocationFrameRequest.path;
        job.width = g_locationFrameWidth;
        job.height = g_locationFrameHeight;
        job.sourceFormat = static_cast<std::uint32_t>(g_locationFrameFormat);
        try {
            job.bgra.resize(static_cast<std::size_t>(job.width) * job.height * 4u);
        } catch (...) {
            immediate->Unmap(g_locationFrameStaging, 0);
            FailLocationFrameCapture("allocate the CPU frame buffer", E_OUTOFMEMORY);
            return;
        }
        const auto* sourcePixels = static_cast<const std::uint8_t*>(mapped.data);
        for (std::uint32_t y = 0; y < job.height; ++y) {
            const auto* sourceRow = sourcePixels + static_cast<std::size_t>(y) * mapped.rowPitch;
            auto* targetRow = job.bgra.data() + static_cast<std::size_t>(y) * job.width * 4u;
            std::memcpy(targetRow, sourceRow, static_cast<std::size_t>(job.width) * 4u);
        }
        immediate->Unmap(g_locationFrameStaging, 0);
        std::uint64_t nonDarkPixels = 0;
        std::uint8_t minimumChannel = 255;
        std::uint8_t maximumChannel = 0;
        for (std::size_t offset = 0; offset + 3 < job.bgra.size(); offset += 4) {
            const auto b = job.bgra[offset];
            const auto g = job.bgra[offset + 1];
            const auto r = job.bgra[offset + 2];
            minimumChannel = (std::min)(minimumChannel, (std::min)(b, (std::min)(g, r)));
            maximumChannel = (std::max)(maximumChannel, (std::max)(b, (std::max)(g, r)));
            if (b > 8 || g > 8 || r > 8) ++nonDarkPixels;
        }
        SKSE::log::info("SkyCounterUI read back location pixels for '{}': {}x{}, format {}, non-dark pixels {}/{}, RGB byte range {}-{}",
            job.path.string(), job.width, job.height, job.sourceFormat,
            nonDarkPixels, static_cast<std::uint64_t>(job.width) * job.height,
            minimumChannel, maximumChannel);
        job.completion = std::move(g_activeLocationFrameRequest.completion);
        ReleaseLocationFrameGpuResources();
        g_activeLocationFrameRequest = {};
        g_locationFrameReadbackPending.store(false, std::memory_order_release);
        g_locationFrameCancelRequested.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(g_locationFrameMutex);
            g_locationFramePixelQueue.push_back(std::move(job));
        }
        g_locationFramePixelReady.notify_one();
        ScheduleGamePump();
    }

    bool CaptureActiveItem(bool whitePass, std::string& failureReason)
    {
        auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        auto* context = renderer ? reinterpret_cast<Context*>(renderer->context) : nullptr;
        auto* device = renderer ? reinterpret_cast<Device*>(renderer->forwarder) : nullptr;
        if (!context || !device) {
            failureReason = "Skyrim's D3D11 device or immediate context is unavailable";
            return false;
        }

        auto* manager = RE::Inventory3DManager::GetSingleton();
        if (!manager) {
            failureReason = "Inventory3DManager is unavailable";
            return false;
        }
        const bool itemReady = g_activeRequest.usesMenuPreview ?
            InventoryItemIsLoadedByMenu(manager, g_activeRequest.item, g_activeRequest.modelPath) :
            InventoryItemIsReady(manager, g_activeRequest.item, g_activeRequest.modelPath);
        if (!itemReady) {
            failureReason = "the Inventory3DManager model was not ready at capture time";
            return false;
        }
        if (g_inventory3DPreviewCapture.Submit(manager, g_activeRequest.formID,
                g_activeRequest.path, device, context, whitePass, failureReason)) {
            if (g_inventory3DPreviewCapture.IsPending()) {
                g_readbackPending.store(true, std::memory_order_release);
            }
            return true;
        }
        return false;
    }

    void CaptureInventoryPreviewFrame()
    {
        if (g_phase.load(std::memory_order_acquire) != CapturePhase::Ready ||
            !g_captureEnabled.load(std::memory_order_acquire)) {
            ScheduleGamePump();
            return;
        }

        static bool loggedCaptureMode = false;
        if (!loggedCaptureMode) {
            loggedCaptureMode = true;
            SKSE::log::info("SkyCounterUI captures both inventory preview matte passes back-to-back inside one capture-only pause");
        }
        auto* ui = RE::UI::GetSingleton();
        auto captureMenu = ui ? ui->GetMenu(kRenderMenuName) : RE::GPtr<RE::IMenu>{};
        bool pausedInventoryMenu = captureMenu && captureMenu->PausesGame();
        bool globalGamePaused = ui && ui->GameIsPaused();
        const bool playerInventoryOpen = IsInventoryPreviewMenuOpen(ui);
        if (g_activeRequest.usesMenuPreview && !playerInventoryOpen) {
            auto deferred = std::move(g_activeRequest);
            deferred.usesMenuPreview = false;
            deferred.restoreItem = nullptr;
            g_inventory3DPreviewCapture.CancelPass();
            {
                std::scoped_lock lock(g_queueMutex);
                g_captureQueue.push_front(std::move(deferred));
            }
            g_activeRequest = {};
            g_gameFrames.store(0, std::memory_order_relaxed);
            g_previewCapturePass = 0;
            g_phase.store(CapturePhase::Idle, std::memory_order_release);
            ScheduleGamePump();
            return;
        }
        const bool capturePauseOwned = g_captureMenuPauseOwned.load(std::memory_order_acquire);
        bool pauseReady = globalGamePaused &&
            (pausedInventoryMenu || playerInventoryOpen);
        if (!pauseReady && !g_activeRequest.usesMenuPreview) {
            // Do not freeze the player while discovering that the backbuffer
            // format or depth-preservation resources cannot be captured. This
            // preflight is read-only with respect to the game frame.
            if (!capturePauseOwned) {
                auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
                auto* context = renderer ? reinterpret_cast<Context*>(renderer->context) : nullptr;
                auto* device = renderer ? reinterpret_cast<Device*>(renderer->forwarder) : nullptr;
                std::string preflightFailure;
                if (!g_inventory3DPreviewCapture.Prepare(device, context, preflightFailure)) {
                    SKSE::log::warn("SkyCounterUI rejected capture before pausing the game for {:08X}: {}",
                        g_activeRequest.formID,
                        preflightFailure.empty() ? "capture resources are unavailable" : preflightFailure);
                    g_phase.store(CapturePhase::Failed, std::memory_order_release);
                    ScheduleGamePump();
                    return;
                }
                SKSE::log::info("SkyCounterUI capture resources are ready; enabling the short pause only for the two render passes");
            } else {
                SKSE::log::warn("SkyCounterUI deferred capture until the global paused render state is active (menu pauses game: {}, UI GameIsPaused: {}, numPausesGame: {}, owned capture pause: {})",
                    pausedInventoryMenu, globalGamePaused, ui ? ui->numPausesGame : 0,
                    capturePauseOwned);
            }
            if (!SetCaptureMenuPaused(true)) {
                SKSE::log::warn("SkyCounterUI could not establish the global pause required for Inventory3DManager::Render()");
                g_phase.store(CapturePhase::Failed, std::memory_order_release);
                SetCaptureMenuPaused(false);
                ScheduleGamePump();
                return;
            }
            // The pause flag and counters take effect synchronously. Render
            // both matte passes in this same callback, then release the pause
            // before Skyrim presents the restored game frame.
            ui = RE::UI::GetSingleton();
            captureMenu = ui ? ui->GetMenu(kRenderMenuName) : RE::GPtr<RE::IMenu>{};
            pausedInventoryMenu = captureMenu && captureMenu->PausesGame();
            globalGamePaused = ui && ui->GameIsPaused();
            pauseReady = globalGamePaused && (pausedInventoryMenu || playerInventoryOpen);
            if (!pauseReady) {
                SKSE::log::warn("SkyCounterUI could not confirm the capture-only pause synchronously; abandoning this capture without holding the pause");
                g_phase.store(CapturePhase::Failed, std::memory_order_release);
                SetCaptureMenuPaused(false);
                ScheduleGamePump();
                return;
            }
        }
        if (g_previewCapturePass == 0 && !g_activeRequest.usesMenuPreview) {
            SKSE::log::info("SkyCounterUI confirmed paused Inventory3DManager render state for {:08X} (capture menu pauses game: {}, UI GameIsPaused: {}, numPausesGame: {}, player inventory menu open: {})",
                g_activeRequest.formID, pausedInventoryMenu, globalGamePaused,
                ui ? ui->numPausesGame : 0, playerInventoryOpen);
        } else if (g_previewCapturePass == 0) {
            SKSE::log::info("SkyCounterUI is capturing {:08X} from the preview Skyrim already loaded in the open player menu; no capture pause, manager load, or scene reposition is being applied",
                g_activeRequest.formID);
        }
        std::string failureReason;
        auto* manager = RE::Inventory3DManager::GetSingleton();
        if (manager && g_activeRequest.ownsInventoryScene && g_previewCapturePass == 0) {
            EnsureInventoryPreviewAttached(manager, g_activeRequest.item,
                g_activeRequest.modelPath, g_activeRequest.formID);
            if (PositionLoadedInventoryPreviewAtScreenCenter(manager, g_activeRequest.item,
                    g_activeRequest.modelPath)) {
                const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
                SKSE::log::info("SkyCounterUI centered the loaded inventory preview node for {:08X} on the {}x{} capture surface",
                    g_activeRequest.formID, screen.width, screen.height);
            } else {
                SKSE::log::warn("SkyCounterUI could not position the loaded Inventory3DManager node for {:08X}; attempting capture at its current transform",
                    g_activeRequest.formID);
            }
        }
        if (manager && g_previewCapturePass == 0) {
            LogInventoryPreviewState(manager, g_activeRequest.item,
                g_activeRequest.modelPath, g_activeRequest.formID);
        }
        const auto drawPass = [&](bool whitePass) {
            SKSE::log::info("SkyCounterUI drawing the loaded Inventory3DManager preview for {:08X} (matte pass {}/2)",
                g_activeRequest.formID, whitePass ? 2 : 1);
            return CaptureActiveItem(whitePass, failureReason);
        };
        if (!drawPass(false)) {
            SKSE::log::warn("SkyCounterUI could not submit Inventory3DManager capture for {:08X}: {}",
                g_activeRequest.formID, failureReason.empty() ? "unknown renderer error" : failureReason);
            g_previewCapturePass = 0;
            g_phase.store(CapturePhase::Failed, std::memory_order_release);
            SetCaptureMenuPaused(false);
            ScheduleGamePump();
            return;
        }
        g_previewCapturePass = 1;
        failureReason.clear();
        if (!drawPass(true)) {
            SKSE::log::warn("SkyCounterUI could not submit Inventory3DManager capture for {:08X}: {}",
                g_activeRequest.formID, failureReason.empty() ? "unknown renderer error" : failureReason);
            g_previewCapturePass = 0;
            g_phase.store(CapturePhase::Failed, std::memory_order_release);
            SetCaptureMenuPaused(false);
            ScheduleGamePump();
            return;
        }
        g_previewCapturePass = 0;
        g_phase.store(CapturePhase::Captured, std::memory_order_release);
        SetCaptureMenuPaused(false);
        ScheduleGamePump();
    }

    void CaptureMenuFrame()
    {
        ProcessPendingReadback();
        auto* ui = RE::UI::GetSingleton();
        const bool playerPreviewMenuOpen = IsInventoryPreviewMenuOpen(ui);
        if (!playerPreviewMenuOpen) RequeueMenuDeferredCaptures();
        if (!playerPreviewMenuOpen) ProcessLocationFrameCapture();
        const auto phase = g_phase.load(std::memory_order_acquire);
        RE::FormID queuedFormID{};
        const auto queueSize = CaptureQueueSnapshot(&queuedFormID);
        if (phase == CapturePhase::Idle && queueSize && g_captureEnabled.load(std::memory_order_acquire)) {
            const auto waitFrame = g_captureMenuFrameWaitCount.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (waitFrame == 1 || waitFrame % 120 == 0) {
                SKSE::log::info("SkyCounterUI capture render frame observed a queued but unstarted request: frameSamples={}, thread={}, queueSize={}, firstQueuedFormID={:08X}, playerPreviewMenuOpen={}, captureRenderMenuOpen={}, pumpScheduled={}, showRequest={}, captureEnabled={}, phase={}, gamePaused={}, pauseCount={}, managerLoadTaskActive={}",
                    waitFrame, GetCurrentThreadId(), queueSize, queuedFormID,
                    playerPreviewMenuOpen, ui && ui->IsMenuOpen(kRenderMenuName),
                    g_gamePumpScheduled.load(std::memory_order_acquire),
                    static_cast<std::uint32_t>(g_captureMenuShowRequested.load(std::memory_order_acquire)),
                    g_captureEnabled.load(std::memory_order_acquire), CapturePhaseName(phase),
                    ui && ui->GameIsPaused(), ui ? ui->numPausesGame : 0,
                    RE::Inventory3DManager::GetSingleton() && static_cast<bool>(
                        RE::Inventory3DManager::GetSingleton()->GetRuntimeData().loadTask));
            }
        } else {
            g_captureMenuFrameWaitCount.store(0, std::memory_order_release);
        }
        if (phase == CapturePhase::Idle && !playerPreviewMenuOpen) {
            auto* manager = RE::Inventory3DManager::GetSingleton();
            if (manager && static_cast<bool>(manager->GetRuntimeData().loadTask)) {
                const auto frame = g_idleManagerPumpFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (frame == 1 || frame % kManagerPumpIntervalFrames == 0) {
                    auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
                    auto* context = renderer ? reinterpret_cast<Context*>(renderer->context) : nullptr;
                    auto* device = renderer ? reinterpret_cast<Device*>(renderer->forwarder) : nullptr;
                    if (!g_inventory3DPreviewCapture.Pump(manager, device, context)) {
                        SKSE::log::warn("SkyCounterUI could not advance an interrupted Inventory3DManager load after the player menu closed");
                    }
                }
                if (frame >= kIdleManagerRecoveryTimeoutFrames) {
                    // UnloadInventoryItem is the engine-supported cancellation
                    // path for a load that did not settle after the menu closed.
                    manager->UnloadInventoryItem();
                    g_idleManagerPumpFrames.store(0, std::memory_order_release);
                    SKSE::log::warn("SkyCounterUI cancelled a stalled Inventory3DManager load so queued captures can resume");
                }
                return;
            }
            g_idleManagerPumpFrames.store(0, std::memory_order_release);
        }
        if ((phase == CapturePhase::Loading || phase == CapturePhase::Ready) &&
            playerPreviewMenuOpen && !g_activeRequest.usesMenuPreview) {
            // Menu-open events can arrive before or after this render callback
            // depending on the crafting menu. Detect the live UI state as a
            // fallback so SkyCounter never keeps rendering into the shared
            // manager while Alchemy/other player previews are open.
            if (g_inventorySceneClaimed || g_activeRequest.ownsInventoryScene) {
                g_pendingOwnedSceneTeardown.store(true, std::memory_order_release);
                g_menuPreviewHandoffFrames.store(0, std::memory_order_release);
            }
            SKSE::log::info("SkyCounterUI detected an open player preview menu during capture; yielding the Inventory3DManager");
            DeferActiveCaptureForInventoryMenu();
            ScheduleGamePump();
            return;
        }
        if (phase == CapturePhase::Idle &&
            (g_captureEnabled.load(std::memory_order_acquire) ||
                g_pendingOwnedSceneTeardown.load(std::memory_order_acquire))) {
            bool queued = false;
            {
                std::scoped_lock lock(g_queueMutex);
                queued = !g_captureQueue.empty();
            }
            if ((queued && g_captureEnabled.load(std::memory_order_acquire)) ||
                g_pendingOwnedSceneTeardown.load(std::memory_order_acquire)) ScheduleGamePump();
            return;
        }
        if (phase == CapturePhase::Loading) {
            const auto frame = g_gameFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            auto* manager = RE::Inventory3DManager::GetSingleton();
            if (g_activeRequest.usesMenuPreview) {
                if (!playerPreviewMenuOpen) {
                    // The menu close event normally requeues this request;
                    // retain this guard for missed or late UI notifications.
                    auto deferred = std::move(g_activeRequest);
                    deferred.usesMenuPreview = false;
                    g_inventory3DPreviewCapture.CancelPass();
                    std::scoped_lock lock(g_queueMutex);
                    g_captureQueue.push_front(std::move(deferred));
                    g_activeRequest = {};
                    g_previewCapturePass = 0;
                    g_phase.store(CapturePhase::Idle, std::memory_order_release);
                    ScheduleGamePump();
                    return;
                }
                if (InventoryItemIsLoadedByMenu(manager, g_activeRequest.item, g_activeRequest.modelPath)) {
                    g_phase.store(CapturePhase::Ready, std::memory_order_release);
                    SKSE::log::info("SkyCounterUI found pickup {:08X} in Skyrim's existing menu preview; leaving the shared manager scene untouched",
                        g_activeRequest.formID);
                    ScheduleGamePump();
                } else if (frame >= kMenuPreviewWaitFrames) {
                    const auto deferredFormID = g_activeRequest.formID;
                    if (!g_captureEnabled.load(std::memory_order_acquire)) {
                        g_phase.store(CapturePhase::Failed, std::memory_order_release);
                        SKSE::log::info("SkyCounterUI cancelled menu-preview wait for {:08X} because icon rendering was disabled",
                            deferredFormID);
                        ScheduleGamePump();
                        return;
                    }
                    auto deferred = std::move(g_activeRequest);
                    deferred.usesMenuPreview = false;
                    deferred.item = nullptr;
                    deferred.restoreItem = nullptr;
                    deferred.ownsInventoryScene = false;
                    {
                        std::scoped_lock lock(g_queueMutex);
                        g_menuDeferredCaptureQueue.push_back(std::move(deferred));
                    }
                    g_activeRequest = {};
                    g_gameFrames.store(0, std::memory_order_relaxed);
                    g_previewCapturePass = 0;
                    g_phase.store(CapturePhase::Idle, std::memory_order_release);
                    NotifyCaptureStatus(deferredFormID, SkyCounterUICaptureStatus::Failed);
                    SKSE::log::info("SkyCounterUI deferred pickup preview {:08X} until Skyrim closes its menu because that item is not in the current menu preview; continuing with other captures",
                        deferredFormID);
                    ScheduleGamePump();
                }
                return;
            }
            if (frame == 1 || frame % kManagerPumpIntervalFrames == 0) {
                auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
                auto* context = renderer ? reinterpret_cast<Context*>(renderer->context) : nullptr;
                auto* device = renderer ? reinterpret_cast<Device*>(renderer->forwarder) : nullptr;
                if (!manager || !g_inventory3DPreviewCapture.Pump(manager, device, context)) {
                    if (!g_managerPumpWarningLogged) {
                        g_managerPumpWarningLogged = true;
                        SKSE::log::warn("SkyCounterUI could not advance the Inventory3DManager load pump for {:08X}",
                            g_activeRequest.formID);
                    }
                }
            }
            if (InventoryItemIsReady(manager, g_activeRequest.item, g_activeRequest.modelPath)) {
                g_loadTimeoutLogged = false;
                g_phase.store(CapturePhase::Ready, std::memory_order_release);
                const auto loadedCount = manager ? manager->GetRuntimeData().loadedModels.size() : 0;
                const auto loadTaskActive = manager &&
                    static_cast<bool>(manager->GetRuntimeData().loadTask);
                SKSE::log::info("SkyCounterUI preview model ready: formID={:08X}, name='{}', waitedFrames={}, loadedModels={}, loadTaskActive={}, modelPath='{}'",
                    g_activeRequest.formID,
                    g_activeRequest.item && g_activeRequest.item->GetName() ?
                        g_activeRequest.item->GetName() : "Unknown item",
                    frame, loadedCount, loadTaskActive, g_activeRequest.modelPath);
                ScheduleGamePump();
            } else if (frame >= kModelLoadWarningFrames && !g_loadTimeoutLogged) {
                g_loadTimeoutLogged = true;
                const auto loadedCount = manager ? manager->GetRuntimeData().loadedModels.size() : 0;
                const auto loadTaskActive = manager &&
                    static_cast<bool>(manager->GetRuntimeData().loadTask);
                SKSE::log::warn("SkyCounterUI still waiting for preview model: formID={:08X}, name='{}', waitedFrames={}, loadedModels={}, loadTaskActive={}, modelPath='{}'",
                    g_activeRequest.formID,
                    g_activeRequest.item && g_activeRequest.item->GetName() ?
                        g_activeRequest.item->GetName() : "Unknown item",
                    frame, loadedCount, loadTaskActive, g_activeRequest.modelPath);
            } else if (frame >= kModelLoadTimeoutFrames) {
                auto* manager = RE::Inventory3DManager::GetSingleton();
                const auto loadedCount = manager ? manager->GetRuntimeData().loadedModels.size() : 0;
                const auto loadTaskActive = manager &&
                    static_cast<bool>(manager->GetRuntimeData().loadTask);
                SKSE::log::warn("SkyCounterUI preview model timed out: formID={:08X}, name='{}', waitedFrames={}, loadTaskActive={}, loadedModels={}, modelPath='{}'",
                    g_activeRequest.formID,
                    g_activeRequest.item && g_activeRequest.item->GetName() ?
                        g_activeRequest.item->GetName() : "Unknown item",
                    frame, loadTaskActive, loadedCount, g_activeRequest.modelPath);
                g_phase.store(CapturePhase::Failed, std::memory_order_release);
                ScheduleGamePump();
            }
            return;
        }

        if (phase == CapturePhase::Ready && g_captureEnabled.load(std::memory_order_acquire)) {
            CaptureInventoryPreviewFrame();
            return;
        }

        if ((phase == CapturePhase::Captured || phase == CapturePhase::Failed) &&
            !g_readbackPending.load(std::memory_order_acquire)) {
            ScheduleGamePump();
            return;
        }
        if (phase == CapturePhase::Ready && !g_captureEnabled.load(std::memory_order_acquire)) {
            ScheduleGamePump();
            return;
        }
    }

    void StartWriter()
    {
        bool expected = false;
        if (g_writerStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            std::thread(WriterMain).detach();
        }
        expected = false;
        if (g_locationFrameWriterStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            std::thread(LocationFrameWriterMain).detach();
        }
    }

}

void InstallSkyCounterUIIconRenderer(
    SkyCounterUIIconReadyCallback iconReadyCallback,
    SkyCounterUICaptureStatusCallback captureStatusCallback)
{
    g_iconReadyCallback = iconReadyCallback;
    g_captureStatusCallback = captureStatusCallback;
    std::scoped_lock lock(g_rendererInstallMutex);
    if (g_rendererInstalled.load(std::memory_order_relaxed)) return;
    if (auto* ui = RE::UI::GetSingleton(); ui &&
        !g_menuEventSinkRegistered.exchange(true, std::memory_order_acq_rel)) {
        ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_captureMenuEventSink);
    }
    StartWriter();
    // The engine owns model loading, materials, inventory transforms, and
    // lighting through Inventory3DManager; no NIF parser or custom mesh shader
    // is started for icon capture. Keep the shipped renderIcons default active
    // until settings explicitly disable it.
    g_rendererInstalled.store(true, std::memory_order_release);
    SKSE::log::info("SkyCounterUI Inventory3DManager pickup capture installed (icon rendering enabled: {})",
        g_captureEnabled.load(std::memory_order_acquire));
}

void SetSkyCounterUIIconCaptureEnabled(bool enabled)
{
    const bool previous = g_captureEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (!enabled) {
        std::deque<CaptureRequest> cancelled;
        {
            std::scoped_lock lock(g_queueMutex);
            cancelled.swap(g_captureQueue);
            while (!g_menuDeferredCaptureQueue.empty()) {
                cancelled.push_back(std::move(g_menuDeferredCaptureQueue.front()));
                g_menuDeferredCaptureQueue.pop_front();
            }
            for (const auto& request : cancelled) g_queuedFormIDs.erase(request.formID);
        }
    }
    if (previous != enabled) {
        SKSE::log::info("SkyCounterUI pickup icon rendering {} by settings",
            enabled ? "enabled" : "disabled");
        ScheduleGamePump();
    }
}

bool IsSkyCounterUIIconCurrent(const std::filesystem::path& iconPath)
{
    if (!HasSkyCounterUIIconCache(iconPath)) return false;
    std::ifstream marker(RendererVersionPath(iconPath), std::ios::binary);
    std::uint32_t version{};
    return marker && (marker >> version) && version == kIconRendererVersion;
}

bool HasSkyCounterUIIconCache(const std::filesystem::path& iconPath)
{
    if (iconPath.empty()) return false;
    std::error_code error;
    if (!std::filesystem::is_regular_file(iconPath, error) || error) return false;
    const auto size = std::filesystem::file_size(iconPath, error);
    if (error || size < 8) return false;

    std::array<unsigned char, 8> signature{};
    std::ifstream input(iconPath, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(signature.size()))) return false;
    constexpr std::array<unsigned char, 8> kPngSignature{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    return signature == kPngSignature;
}

void QueueSkyCounterUIIconRender(std::uint32_t formID, std::filesystem::path outputPath,
    const RE::TESForm* form, RE::NiPointer<RE::NiAVObject> sourceModel)
{
    const bool rendererInstalled = g_rendererInstalled.load(std::memory_order_acquire);
    const bool captureEnabled = g_captureEnabled.load(std::memory_order_acquire);
    const char* const itemName = form && form->GetName() ? form->GetName() : "Unknown item";
    const std::string outputPathText = outputPath.string();
    std::string pluginName = "Unknown plugin";
    if (form) {
        if (const auto* file = form->GetFile(); file && !file->GetFilename().empty()) {
            pluginName = file->GetFilename();
        }
    }
    if (!formID || outputPath.empty() || !rendererInstalled) {
        SKSE::log::warn("SkyCounterUI rejected icon request: formID={:08X}, emptyPath={}, rendererInstalled={}, captureEnabled={}, formFound={}, liveSourceModel={}",
            formID, outputPath.empty(), rendererInstalled, captureEnabled,
            form != nullptr, static_cast<bool>(sourceModel));
        return;
    }
    if (IsSkyCounterUIIconCurrent(outputPath)) {
        SKSE::log::info("SkyCounterUI icon request already has a current cached PNG: formID={:08X}, name='{}', path='{}'",
            formID, itemName, outputPathText);
        NotifyIconReady(formID, outputPath);
        return;
    }
    if (!captureEnabled) {
        SKSE::log::warn("SkyCounterUI skipped icon request because renderIcons is disabled: formID={:08X}, name='{}', path='{}'",
            formID, itemName, outputPathText);
        return;
    }
    // Avoid reloading a stale PNG immediately before replacing it. PrismaUI
    // can still be reading that file when the background writer opens it.
    // The refreshed file is announced after the write succeeds.
    bool alreadyQueued = false;
    bool queueFull = false;
    std::size_t queuedCount{};
    {
        std::scoped_lock lock(g_queueMutex);
        if (!g_queuedFormIDs.insert(formID).second) {
            alreadyQueued = true;
        } else if (g_captureQueue.size() >= kQueueLimit) {
            g_queuedFormIDs.erase(formID);
            queueFull = true;
        } else {
            g_captureQueue.push_back({ formID, std::move(outputPath) });
            queuedCount = g_captureQueue.size();
        }
    }
    if (alreadyQueued) {
        SKSE::log::info("SkyCounterUI did not add duplicate icon request: formID={:08X}, name='{}' (already queued or in flight)",
            formID, itemName);
        return;
    }
    if (queueFull) {
        SKSE::log::warn("SkyCounterUI could not queue icon request: formID={:08X}, capture queue reached limit {}",
            formID, kQueueLimit);
        return;
    }
    g_gamePumpQueueWaitCount.store(0, std::memory_order_release);
    g_captureMenuFrameWaitCount.store(0, std::memory_order_release);
    g_captureMenuWaitLogged.store(false, std::memory_order_release);
    SKSE::log::info("SkyCounterUI queued icon request: formID={:08X}, name='{}', formType={}, plugin='{}', liveSourceModel={}, path='{}', pendingQueueSize={}",
        formID, itemName,
        form ? static_cast<std::uint32_t>(form->GetFormType()) : 0u,
        pluginName, static_cast<bool>(sourceModel), outputPathText, queuedCount);
    ScheduleGamePump("pickup icon request queued");
}

bool QueueSkyCounterUIFrameCapture(std::filesystem::path outputPath,
    SkyCounterUIFrameCaptureCallback completion)
{
    if (outputPath.empty() || !g_rendererInstalled.load(std::memory_order_acquire) ||
        !g_locationFrameWriterStarted.load(std::memory_order_acquire)) {
        if (completion) completion(false);
        return false;
    }
    bool accepted = false;
    {
        std::scoped_lock lock(g_locationFrameMutex);
        if (g_locationFrameQueue.size() < 128) {
            g_locationFrameQueue.push_back({ std::move(outputPath), std::move(completion) });
            accepted = true;
        }
    }
    if (!accepted) {
        if (completion) completion(false);
        return false;
    }
    ScheduleGamePump();
    return true;
}

void CancelSkyCounterUIFrameCaptures()
{
    std::deque<LocationFrameRequest> cancelled;
    {
        std::scoped_lock lock(g_locationFrameMutex);
        cancelled.swap(g_locationFrameQueue);
    }
    if (g_locationFrameReadbackPending.load(std::memory_order_acquire)) {
        g_locationFrameCancelRequested.store(true, std::memory_order_release);
    }
    for (auto& request : cancelled) {
        if (request.completion) request.completion(false);
    }
    ScheduleGamePump();
}
}
