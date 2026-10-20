#include "Engine/Source/VolFog/VolFogLights.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#include <d3d9.h>
#endif

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/Types.h"
#include "Engine/Source/VolFog/VolFog.h"
#include "Engine/Source/VolFog/VolFogModel.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B8.
// Licensed under GPL-3.0-only. Derived from src/engine_lights.cpp (PointLightCutoff, UploadedPointLightColor, LocalLightPriority, the ranking and
// the view test) and src/local_light_stabiliser.cpp (8 slots, pool of 24 candidates, 1.5x hysteresis held for 3 captures, 1 second fades) of
// coa-vfog 205ade5, and from the light upload of Renderer::Draw in src/renderer.cpp (copyright of the authors of CoAVolFog, jealous-sound and the
// contributors of the Raz0r1337 fork; see Shaders/VolFog/NOTICE-modificaciones.txt).
// Modified 2026-10-20 (brief B8): in the namespace VolFog; the point lights come from CM2Scene::m_lightGrid of the world scene (the 64 x 64 buckets
// of 20 yards that CM2Light::Link fills for M2LIGHT_POINT; coa-vfog reads the same table at scene + 0x24), not from fixed addresses, so there is
// no client signature check and no guarded read; the interior capture of coa-vfog is not needed (VolFogWorld.cpp fills cameraInterior); the
// activity log line is gone (the counters are printed by wolVolFogDump); the texture is made with the pure D3D9 device that CreateDepthTexture
// is given, as the INTZ depth is, instead of going through the Gx upload path (which has no blit for A32B32G32R32F, and no scratch texture on D3D9Ex).
// Modified 2026-10-20 (brief B10): the cut-off is 1/32, not 1/256, and lights whose faded colour is under it are not uploaded.

namespace {
    constexpr uint32_t kBucketCount = 64 * 64;
    constexpr uint32_t kMaxBucketNodes = 512;
    constexpr uint32_t kMaxNodes = 8192;
    constexpr float kMaxWorldCoordinate = 1000000.0f;
    constexpr float kMaxLightComponent = 10000.0f;
    constexpr float kMaxUploadedLightComponent = 1000000.0f;
    constexpr float kMaxAttenuationCoefficient = 1000000.0f;

    // local_light_stabiliser.h
    constexpr float kFadeSeconds = 1.0f;
    constexpr float kPrioritySmoothingSeconds = 0.3f;
    constexpr double kSwapMargin = 1.5;
    constexpr uint32_t kSwapCaptures = 3;
    constexpr uint32_t kHoldCaptures = 8;
    constexpr float kIdentityYards = 2.0f;
    constexpr float kViewExitYards = 5.0f;
    constexpr double kResetSeconds = 1.0;
    constexpr uint32_t kEntries = VolFog::kLocalLightCandidatePool + VolFog::kMaxLocalPointLights;

    using VolFog::LightCamera;
    using VolFog::LightCapture;
    using VolFog::LightSet;
    using VolFog::PointLight;

    bool ValidVector(const float values[3], float minimum, float maximum) {
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(values[i]) || values[i] < minimum || values[i] > maximum) {
                return false;
            }
        }

        return true;
    }

    double SquaredDistance(const float a[3], const float b[3]) {
        double result = 0.0;

        for (int i = 0; i < 3; ++i) {
            const double delta = static_cast<double>(a[i]) - b[i];
            result += delta * delta;
        }

        return result;
    }

    float PeakColor(const float color[3]) {
        return std::max(color[0], std::max(color[1], color[2]));
    }

    float LinearChannel(float channel, float peak) {
        const float gammaChannel = std::max(channel, 0.0f);

        if (peak <= 1.0f) {
            return std::pow(gammaChannel, VolFog::kLocalLightGamma);
        }

        return std::pow(gammaChannel / peak, VolFog::kLocalLightGamma) * peak;
    }

    // The fog scatters in linear light (colorSpace 1, the composite's default), so the colour is linearised: engine::UploadedPointLightColor.
    void UploadedPointLightColor(const float captured[3], float uploaded[3]) {
        const float peak = PeakColor(captured);

        for (int channel = 0; channel < 3; ++channel) {
            uploaded[channel] = LinearChannel(captured[channel], peak) * VolFog::kLocalLightIntensity;
        }
    }

    float PointLightCutoff(const float color[3], const float attenuation[3]) {
        if (!ValidVector(color, 0.0f, kMaxUploadedLightComponent) || !ValidVector(attenuation, 0.0f, kMaxAttenuationCoefficient)) {
            return 0.0f;
        }

        const double peak = PeakColor(color);
        const double target = peak / VolFog::kLocalPointLightContributionCutoff;

        if (peak <= 0.0 || target <= std::max(static_cast<double>(attenuation[0]), 1.0)) {
            return 0.0f;
        }

        const double remaining = target - attenuation[0];
        double radius = VolFog::kMaxLocalPointLightRadius;

        if (attenuation[2] > 0.0f) {
            const double linear = attenuation[1];
            radius = 2.0 * remaining / (linear + std::sqrt(linear * linear + 4.0 * attenuation[2] * remaining));
        } else if (attenuation[1] > 0.0f) {
            radius = remaining / attenuation[1];
        }

        return static_cast<float>(std::min(radius, static_cast<double>(VolFog::kMaxLocalPointLightRadius)));
    }

    double LocalLightPriority(const PointLight& light, const float cameraPosition[3]) {
        const double distanceSquared = SquaredDistance(light.position, cameraPosition);
        const double denominator = light.attenuation[0] + light.attenuation[1] * std::sqrt(distanceSquared) + light.attenuation[2] * distanceSquared;

        return PeakColor(light.uploadedColor) / std::max(denominator, 1.0);
    }

    bool Precedes(const PointLight& a, const PointLight& b, const float cameraPosition[3]) {
        const double aPriority = LocalLightPriority(a, cameraPosition);
        const double bPriority = LocalLightPriority(b, cameraPosition);

        if (aPriority != bPriority) {
            return aPriority > bPriority;
        }

        const double aDistance = SquaredDistance(a.position, cameraPosition);
        const double bDistance = SquaredDistance(b.position, cameraPosition);

        if (aDistance != bDistance) {
            return aDistance < bDistance;
        }

        for (int i = 0; i < 3; ++i) {
            if (a.position[i] != b.position[i]) {
                return a.position[i] < b.position[i];
            }
        }

        for (int i = 0; i < 3; ++i) {
            if (a.color[i] != b.color[i]) {
                return a.color[i] > b.color[i];
            }
        }

        for (int i = 0; i < 3; ++i) {
            if (a.attenuation[i] != b.attenuation[i]) {
                return a.attenuation[i] < b.attenuation[i];
            }
        }

        return false;
    }

    bool InsertRankedLight(PointLight* list, uint32_t& count, uint32_t capacity, const PointLight& candidate, const float cameraPosition[3]) {
        uint32_t index = 0;

        while (index < count && !Precedes(candidate, list[index], cameraPosition)) {
            ++index;
        }

        if (index == capacity) {
            return false;
        }

        const uint32_t newCount = std::min(count + 1, capacity);

        for (uint32_t i = newCount - 1; i > index; --i) {
            list[i] = list[i - 1];
        }

        list[index] = candidate;
        count = newCount;

        return true;
    }

    struct ViewPlane {
        float normal[3];
        float offset;
    };

    // View space: x right, y up, z forward (the projection is ndc z = c2 + d2 / z); the camera-relative view is row-major as in D3D.
    void ViewSpacePosition(const PointLight& light, const LightCamera& camera, float out[3]) {
        float relative[3];

        for (int i = 0; i < 3; ++i) {
            relative[i] = light.position[i] - camera.position[i];
        }

        const float* m = camera.cameraRelativeView;

        for (int j = 0; j < 3; ++j) {
            out[j] = relative[0] * m[0 * 4 + j] + relative[1] * m[1 * 4 + j] + relative[2] * m[2 * 4 + j];
        }
    }

    float ProjectionNear(const float* projection) {
        const float divisor = 1.0f + projection[10];
        const float nearPlane = divisor != 0.0f ? -projection[14] / divisor : 0.0f;

        return std::isfinite(nearPlane) ? std::max(nearPlane, 0.0f) : 0.0f;
    }

    bool ProjectsView(const LightCamera& camera) {
        const float* p = camera.glProjection;

        return std::isfinite(p[0]) && std::isfinite(p[5]) && p[0] != 0.0f && p[5] != 0.0f;
    }

    bool SphereBeyondPlane(const float centre[3], float radius, const ViewPlane& plane) {
        const float* n = plane.normal;
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

        if (!(length > 0.0f)) {
            return false;
        }

        const float signedDistance = (n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] + plane.offset) / length;

        return signedDistance > radius;
    }

    // engine_lights.cpp PointLightMeetsView: the sphere of the light (centre + cutoff + margin) against the five planes of the view.
    bool PointLightMeetsView(const PointLight& light, const LightCamera& camera, float margin = 0.0f) {
        if (!ProjectsView(camera)) {
            return true;
        }

        float centre[3];
        ViewSpacePosition(light, camera, centre);
        const float* p = camera.glProjection;
        const ViewPlane planes[] = {
            { { 0.0f, 0.0f, -1.0f }, ProjectionNear(p) },
            { { p[0], 0.0f, p[8] - 1.0f }, 0.0f },
            { { -p[0], 0.0f, -p[8] - 1.0f }, 0.0f },
            { { 0.0f, p[5], p[9] - 1.0f }, 0.0f },
            { { 0.0f, -p[5], -p[9] - 1.0f }, 0.0f },
        };

        for (const ViewPlane& plane : planes) {
            if (SphereBeyondPlane(centre, light.cutoff + margin, plane)) {
                return false;
            }
        }

        return true;
    }

    // engine::SelectLocalPointLightInView: validate, bound (uploaded colour and cutoff), keep if its sphere meets the view, rank.
    bool SelectLocalPointLightInView(LightSet& out, const PointLight& light, const LightCamera& camera) {
        if (!(out.pointLightCount <= VolFog::kMaxLocalPointLights && out.candidateCount <= VolFog::kLocalLightCandidatePool
              && ValidVector(camera.position, -kMaxWorldCoordinate, kMaxWorldCoordinate) && ValidVector(light.position, -kMaxWorldCoordinate, kMaxWorldCoordinate)
              && ValidVector(light.color, 0.0f, kMaxLightComponent))) {
            return false;
        }

        PointLight candidate = light;
        UploadedPointLightColor(candidate.color, candidate.uploadedColor);
        candidate.cutoff = PointLightCutoff(candidate.uploadedColor, candidate.attenuation);

        if (!(candidate.cutoff > 0.0f) || !PointLightMeetsView(candidate, camera)) {
            return false;
        }

        InsertRankedLight(out.candidates, out.candidateCount, VolFog::kLocalLightCandidatePool, candidate, camera.position);

        return InsertRankedLight(out.pointLights, out.pointLightCount, VolFog::kMaxLocalPointLights, candidate, camera.position);
    }

    // Walk of the 64 x 64 buckets (CM2Light::Link puts every visible M2LIGHT_POINT in the bucket of its 20-yard cell); the same checks as
    // CapturePointLightsUnsafe: at most 512 nodes per bucket and 8192 in all, only enabled point lights.
    void CapturePointLights(const CM2Scene* scene, const LightCamera& camera, LightSet& out) {
        CM2Light* const* grid = scene->m_lightGrid;

        if (!grid) {
            return;
        }

        for (uint32_t bucket = 0; bucket < kBucketCount; ++bucket) {
            const CM2Light* light = grid[bucket];
            uint32_t bucketNodes = 0;

            while (light) {
                if (++bucketNodes > kMaxBucketNodes) {
                    out.capture = LightCapture::DamagedTable;

                    return;
                }

                if (++out.nodesWalked > kMaxNodes) {
                    out.capture = LightCapture::TableTruncated;

                    return;
                }

                if (light->m_type != M2LIGHT_POINT) {
                    ++out.skippedNonPoint;
                } else if (!light->m_visible) {
                    ++out.skippedDisabled;
                } else {
                    PointLight point;
                    point.position[0] = light->m_pos.x;
                    point.position[1] = light->m_pos.y;
                    point.position[2] = light->m_pos.z;
                    point.color[0] = light->m_dirColor.x;
                    point.color[1] = light->m_dirColor.y;
                    point.color[2] = light->m_dirColor.z;
                    point.attenuation[0] = light->m_constantAttenuation;
                    point.attenuation[1] = light->m_linearAttenuation;
                    point.attenuation[2] = light->m_quadraticAttenuation;
                    point.enabled = light->m_visible;
                    point.nativeId = reinterpret_cast<uintptr_t>(light);
                    SelectLocalPointLightInView(out, point, camera);
                }

                light = light->m_lightNext;
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // The stabiliser (local_light_stabiliser.cpp): which 8 of the lights in view are uploaded, and with what weight.
    // ---------------------------------------------------------------------------------------------------------------------------------

    bool SameLight(const PointLight& a, const PointLight& b) {
        double distanceSquared = 0.0;

        for (int i = 0; i < 3; ++i) {
            const double delta = static_cast<double>(a.position[i]) - b.position[i];
            distanceSquared += delta * delta;
        }

        return a.nativeId == b.nativeId && distanceSquared <= static_cast<double>(kIdentityYards) * kIdentityYards;
    }

    double Smoothed(double previous, double current, float dt) {
        const double alpha = 1.0 - std::exp(-static_cast<double>(dt) / kPrioritySmoothingSeconds);

        return previous + (current - previous) * alpha;
    }

    bool CentreInView(const PointLight& light, const LightCamera& camera) {
        return PointLightMeetsView(light, camera, -light.cutoff);
    }

    float EasedFade(float weight) {
        return weight * weight * (3.0f - 2.0f * weight);
    }

    bool StructuralFailure(const LightSet& lights) {
        return lights.candidateCount == 0 && lights.capture == LightCapture::DamagedTable;
    }

    class Stabiliser {
        public:
        void Step(const LightCamera& camera, int mapId, uint32_t frame, double seconds, LightSet& lights);
        void Reset();

        uint32_t m_admissions = 0;
        uint32_t m_evictions = 0;
        uint32_t m_fadeOuts = 0;
        uint32_t m_heldCaptures = 0;

        private:
        struct Entry {
            PointLight light;
            double priority = 0.0;
            double smoothedPriority = 0.0;
            float weight = 0.0f;
            uint32_t challengeCaptures = 0;
            bool slotted = false;
            bool evicted = false;
            bool heirToEvictedSlot = false;
            bool enteredAtViewEdge = false;
            bool present = false;
            bool outOfView = false;
        };

        void Replay(LightSet& lights) const;
        bool HoldOutput(LightSet& lights);
        void MatchCandidates(const LightCamera& camera, const LightSet& lights, float dt);
        Entry* FindEntry(const PointLight& light);
        Entry* AddEntry(const PointLight& light, double priority);
        void AdvanceWeights(float dt);
        static bool TakesFreeSlotBefore(const Entry& entry, const Entry& other);
        void FillFreeSlots();
        void ChallengeWeakestSlot();
        void RemoveSpentEntries();
        void WriteOutput(LightSet& lights);

        Entry m_entries[kEntries] = {};
        uint32_t m_count = 0;
        PointLight m_held[VolFog::kMaxLocalPointLights] = {};
        uint32_t m_heldCount = 0;
        uint32_t m_rejectedCaptures = 0;
        bool m_sceneStarting = true;
        int m_mapId = 0;
        uint32_t m_lastFrame = 0;
        double m_lastSeconds = 0.0;
        bool m_started = false;
    };

    void Stabiliser::Reset() {
        m_count = 0;
        m_heldCount = 0;
        m_rejectedCaptures = 0;
        m_sceneStarting = true;
    }

    void Stabiliser::Step(const LightCamera& camera, int mapId, uint32_t frame, double seconds, LightSet& lights) {
        if (m_started && frame == m_lastFrame) {
            Replay(lights);

            return;
        }

        m_lastFrame = frame;

        if (!m_started || mapId != m_mapId || seconds < m_lastSeconds || seconds - m_lastSeconds > kResetSeconds) {
            Reset();
        }

        const double elapsed = m_started ? std::clamp(seconds - m_lastSeconds, 0.0, static_cast<double>(kFadeSeconds)) : 0.0;
        const float dt = static_cast<float>(elapsed);
        m_started = true;
        m_mapId = mapId;
        m_lastSeconds = seconds;

        if (HoldOutput(lights)) {
            return;
        }

        MatchCandidates(camera, lights, dt);
        AdvanceWeights(dt);
        RemoveSpentEntries();
        FillFreeSlots();
        m_sceneStarting = false;
        ChallengeWeakestSlot();
        WriteOutput(lights);
    }

    void Stabiliser::Replay(LightSet& lights) const {
        std::memcpy(lights.pointLights, m_held, sizeof(m_held));
        lights.pointLightCount = m_heldCount;
    }

    bool Stabiliser::HoldOutput(LightSet& lights) {
        if (!StructuralFailure(lights)) {
            m_rejectedCaptures = 0;

            return false;
        }

        if (m_rejectedCaptures >= kHoldCaptures) {
            return false;
        }

        ++m_rejectedCaptures;
        ++m_heldCaptures;
        Replay(lights);

        return true;
    }

    bool Stabiliser::TakesFreeSlotBefore(const Entry& entry, const Entry& other) {
        if (entry.heirToEvictedSlot != other.heirToEvictedSlot) {
            return entry.heirToEvictedSlot;
        }

        return entry.smoothedPriority > other.smoothedPriority;
    }

    Stabiliser::Entry* Stabiliser::FindEntry(const PointLight& light) {
        for (uint32_t i = 0; i < m_count; ++i) {
            if (SameLight(m_entries[i].light, light)) {
                return &m_entries[i];
            }
        }

        return nullptr;
    }

    Stabiliser::Entry* Stabiliser::AddEntry(const PointLight& light, double priority) {
        if (m_count == kEntries) {
            return nullptr;
        }

        Entry& entry = m_entries[m_count++];
        entry = Entry{};
        entry.light = light;
        entry.priority = priority;
        entry.smoothedPriority = priority;

        return &entry;
    }

    void Stabiliser::MatchCandidates(const LightCamera& camera, const LightSet& lights, float dt) {
        for (uint32_t i = 0; i < m_count; ++i) {
            m_entries[i].present = false;
            m_entries[i].outOfView = false;
            m_entries[i].enteredAtViewEdge = false;
        }

        const uint32_t count = std::min(lights.candidateCount, VolFog::kLocalLightCandidatePool);

        for (uint32_t i = 0; i < count; ++i) {
            const PointLight& candidate = lights.candidates[i];
            const double priority = LocalLightPriority(candidate, camera.position);
            const bool inView = PointLightMeetsView(candidate, camera);
            Entry* entry = FindEntry(candidate);

            if (!entry && !inView) {
                continue;
            }

            if (!entry) {
                entry = AddEntry(candidate, priority);

                if (!entry) {
                    continue;
                }

                entry->enteredAtViewEdge = !CentreInView(candidate, camera);
            }

            entry->light = candidate;
            entry->priority = priority;
            entry->smoothedPriority = Smoothed(entry->smoothedPriority, priority, dt);
            entry->present = inView;
            entry->outOfView = !inView;
        }

        for (uint32_t i = 0; i < m_count; ++i) {
            Entry& entry = m_entries[i];

            if (!entry.present && !PointLightMeetsView(entry.light, camera, kViewExitYards)) {
                entry.outOfView = true;
            }
        }
    }

    void Stabiliser::AdvanceWeights(float dt) {
        const float step = dt / kFadeSeconds;

        for (uint32_t i = 0; i < m_count; ++i) {
            Entry& entry = m_entries[i];

            if (!entry.slotted) {
                continue;
            }

            if (entry.outOfView) {
                entry.weight = 0.0f;

                continue;
            }

            const bool shining = entry.present && !entry.evicted;
            entry.weight = shining ? std::min(entry.weight + step, 1.0f) : std::max(entry.weight - step, 0.0f);
        }
    }

    void Stabiliser::FillFreeSlots() {
        uint32_t slotted = 0;

        for (uint32_t i = 0; i < m_count; ++i) {
            slotted += m_entries[i].slotted ? 1 : 0;
        }

        while (slotted < VolFog::kMaxLocalPointLights) {
            Entry* best = nullptr;

            for (uint32_t i = 0; i < m_count; ++i) {
                Entry& entry = m_entries[i];

                if (!entry.slotted && entry.present && (!best || TakesFreeSlotBefore(entry, *best))) {
                    best = &entry;
                }
            }

            if (!best) {
                return;
            }

            best->slotted = true;
            best->evicted = false;
            best->heirToEvictedSlot = false;
            best->weight = m_sceneStarting || best->enteredAtViewEdge ? 1.0f : 0.0f;
            best->challengeCaptures = 0;
            ++slotted;
            ++m_admissions;
        }
    }

    void Stabiliser::ChallengeWeakestSlot() {
        uint32_t vacating = 0;
        uint32_t waitingHeirs = 0;
        Entry* weakest = nullptr;

        for (uint32_t i = 0; i < m_count; ++i) {
            Entry& entry = m_entries[i];

            if (entry.slotted && (entry.evicted || !entry.present)) {
                ++vacating;
            } else if (entry.slotted && (!weakest || entry.smoothedPriority < weakest->smoothedPriority)) {
                weakest = &entry;
            }

            if (!entry.slotted && entry.present && entry.heirToEvictedSlot) {
                ++waitingHeirs;
            }
        }

        if (!weakest || vacating > waitingHeirs) {
            return;
        }

        Entry* challenger = nullptr;

        for (uint32_t i = 0; i < m_count; ++i) {
            Entry& entry = m_entries[i];

            if (entry.slotted || !entry.present || entry.heirToEvictedSlot) {
                continue;
            }

            if (entry.smoothedPriority > kSwapMargin * weakest->smoothedPriority) {
                ++entry.challengeCaptures;
            } else {
                entry.challengeCaptures = 0;
            }

            if (entry.challengeCaptures >= kSwapCaptures && (!challenger || entry.smoothedPriority > challenger->smoothedPriority)) {
                challenger = &entry;
            }
        }

        if (!challenger) {
            return;
        }

        weakest->evicted = true;
        challenger->challengeCaptures = 0;
        challenger->heirToEvictedSlot = true;
        ++m_evictions;
    }

    void Stabiliser::RemoveSpentEntries() {
        uint32_t kept = 0;

        for (uint32_t i = 0; i < m_count; ++i) {
            Entry entry = m_entries[i];
            const bool shining = entry.present && !entry.evicted && !entry.outOfView;

            if (entry.slotted && entry.weight <= 0.0f && !shining) {
                if (!entry.outOfView) {
                    ++m_fadeOuts;
                }

                entry.slotted = false;
                entry.evicted = false;
                entry.challengeCaptures = 0;
            }

            if (entry.slotted || entry.present) {
                m_entries[kept++] = entry;
            }
        }

        m_count = kept;
    }

    void Stabiliser::WriteOutput(LightSet& lights) {
        uint32_t order[kEntries];
        uint32_t count = 0;

        for (uint32_t i = 0; i < m_count && count < VolFog::kMaxLocalPointLights; ++i) {
            if (!m_entries[i].slotted) {
                continue;
            }

            uint32_t index = count;

            while (index > 0 && m_entries[i].smoothedPriority > m_entries[order[index - 1]].smoothedPriority) {
                --index;
            }

            for (uint32_t j = count; j > index; --j) {
                order[j] = order[j - 1];
            }

            order[index] = i;
            ++count;
        }

        for (uint32_t i = 0; i < count; ++i) {
            const Entry& entry = m_entries[order[i]];
            PointLight light = entry.light;
            const float fade = EasedFade(entry.weight);

            for (int channel = 0; channel < 3; ++channel) {
                light.uploadedColor[channel] *= fade;
            }

            lights.pointLights[i] = light;
        }

        lights.pointLightCount = count;
        std::memcpy(m_held, lights.pointLights, sizeof(m_held));
        m_heldCount = count;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // State of the last PrepareLights and the 32 x 1 texture (s8 of the lit shaders).
    // ---------------------------------------------------------------------------------------------------------------------------------

    Stabiliser s_stabiliser;
    LightSet s_lights;              // what the last frame uploaded (after the stabiliser)
    uint32_t s_uploaded;
    float s_reach;
    bool s_enabled;
    bool s_hadScene;
    uint32_t s_candidatesInView;

    CGxTex* s_wrapper;
    uint32_t s_textureGeneration;   // brief B10: creations of the texture
#if defined(_WIN32)
    IDirect3DDevice9* s_device;
    IDirect3DTexture9* s_texture;
    bool s_dynamic;
    bool s_makeFailed;

    // A32B32G32R32F, 32 x 1: three texels per light (position + cutoff, uploaded colour, attenuation), as renderer.cpp lays it out. MANAGED like
    // coa-vfog; a device that refuses MANAGED (D3D9Ex) gets a DEFAULT + DYNAMIC texture, locked with DISCARD each frame.
    bool UploadTexture(const float* data) {
        if (!s_device) {
            return false;
        }

        if (!s_texture) {
            if (s_makeFailed) {
                return false;
            }

            IDirect3DTexture9* texture = nullptr;
            HRESULT result = s_device->CreateTexture(32, 1, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &texture, nullptr);
            bool dynamic = false;

            if (FAILED(result) || !texture) {
                texture = nullptr;
                result = s_device->CreateTexture(32, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &texture, nullptr);
                dynamic = true;
            }

            if (FAILED(result) || !texture) {
                s_makeFailed = true;
                CGxDevice::Log("wolVolFog: CreateTexture(32x1, A32B32G32R32F) failed (0x%08X), no local lights in the fog", static_cast<unsigned>(result));

                return false;
            }

            s_texture = texture;
            s_dynamic = dynamic;
            ++s_textureGeneration;
            CGxDevice::Log("wolVolFog: local light texture 32x1 A32B32G32R32F made (pool 0 managed, 1 default + dynamic: %d)", dynamic ? 1 : 0);
        }

        D3DLOCKED_RECT locked = {};

        if (FAILED(s_texture->LockRect(0, &locked, nullptr, s_dynamic ? D3DLOCK_DISCARD : 0)) || !locked.pBits) {
            return false;
        }

        std::memcpy(locked.pBits, data, 32 * 4 * sizeof(float));
        s_texture->UnlockRect(0);

        return true;
    }
#endif
}

#if defined(_WIN32)
void VolFog::SetLightDevice(IDirect3DDevice9* device) {
    if (device != s_device) {
        VolFog::ReleaseLightTexture();
        s_device = device;
    }
}

void VolFog::ReleaseLightTexture() {
    if (s_texture) {
        s_texture->Release();
        s_texture = nullptr;
    }

    if (s_wrapper) {
        s_wrapper->m_apiSpecificData = nullptr;
    }

    s_device = nullptr;
    s_makeFailed = false;
    s_uploaded = 0;
}
#else
void VolFog::SetLightDevice(IDirect3DDevice9*) {
}

void VolFog::ReleaseLightTexture() {
    s_uploaded = 0;
}
#endif

uint32_t VolFog::PrepareLights(const FrameInputs& in, uint32_t frame, double seconds, bool enabled) {
    s_uploaded = 0;
    s_reach = 0.0f;
    s_enabled = enabled;
    s_hadScene = in.worldM2Scene != nullptr;
    s_candidatesInView = 0;
    s_lights = LightSet{};

    if (!enabled || !in.worldM2Scene) {
        return 0;
    }

    LightCamera camera;
    std::memcpy(camera.position, in.camPos, sizeof(camera.position));
    std::memcpy(camera.cameraRelativeView, in.cameraRelativeView, sizeof(camera.cameraRelativeView));
    std::memcpy(camera.glProjection, in.glProjection, sizeof(camera.glProjection));

    LightSet set;
    CapturePointLights(in.worldM2Scene, camera, set);
    s_candidatesInView = set.candidateCount;
    s_stabiliser.Step(camera, in.mapId, frame, seconds, set);

    // Brief B10 (REVISION-B7-B8, finding 1, "Detalle"): a light that is fading in or out below the cut-off colour reaches nothing (its radius was worked
    // out for its full colour), so it is not uploaded, and the lit shader variants run only while some light is left. Compacted in place, so the dump
    // lists exactly what the shader gets.
    {
        uint32_t kept = 0;

        for (uint32_t i = 0; i < set.pointLightCount && i < kMaxLocalPointLights; ++i) {
            if (PeakColor(set.pointLights[i].uploadedColor) >= VolFog::kLocalPointLightContributionCutoff) {
                if (kept != i) {
                    set.pointLights[kept] = set.pointLights[i];
                }

                ++kept;
            }
        }

        set.pointLightCount = kept;
    }

    s_lights = set;

#if defined(_WIN32)
    const uint32_t count = std::min(set.pointLightCount, kMaxLocalPointLights);

    if (count == 0) {
        return 0;
    }

    float texels[32][4] = {};
    float reach = 0.0f;

    for (uint32_t i = 0; i < count; ++i) {
        const PointLight& light = set.pointLights[i];
        float relative[3] = { light.position[0] - in.camPos[0], light.position[1] - in.camPos[1], light.position[2] - in.camPos[2] };
        float position[3];
        VolFog::TransformDirection(relative, in.cameraRelativeView, position);
        texels[i * 3][0] = position[0];
        texels[i * 3][1] = position[1];
        texels[i * 3][2] = position[2];
        texels[i * 3][3] = light.cutoff;
        texels[i * 3 + 1][0] = light.uploadedColor[0];
        texels[i * 3 + 1][1] = light.uploadedColor[1];
        texels[i * 3 + 1][2] = light.uploadedColor[2];
        texels[i * 3 + 2][0] = light.attenuation[0];
        texels[i * 3 + 2][1] = light.attenuation[1];
        texels[i * 3 + 2][2] = light.attenuation[2];

        // c53.y: the farthest distance along a ray where the light can still matter (renderer.cpp marchLightLimit).
        const double x = position[0];
        const double y = position[1];
        const double z = position[2];
        const float limit = static_cast<float>(std::sqrt(x * x + y * y + z * z) + light.cutoff + 0.001);
        reach = std::max(reach, std::nextafter(limit, std::numeric_limits<float>::infinity()));
    }

    if (!UploadTexture(&texels[0][0])) {
        return 0;
    }

    s_reach = reach;
    s_uploaded = count;

    return count;
#else
    return 0;
#endif
}

CGxTex* VolFog::LightTex() {
#if defined(_WIN32)
    if (!s_uploaded || !s_texture) {
        return nullptr;
    }

    if (!s_wrapper) {
        // Same trick as VolFog::DepthTex(): not made by GxTexCreate, so the device neither lists it nor recreates it, and m_renderTarget = 1 keeps
        // the D3D9Ex texture cache from evicting (and Release()ing) a texture this file owns. Point filter and clamp, which A32B32G32R32F needs.
        s_wrapper = new CGxTex(GxTex_2d, 32, 1, 1, GxTex_Abgr32F, GxTex_Abgr32F, CGxTexFlags(GxTex_Nearest, 0, 0, 0, 0, 1), nullptr, nullptr, "wolVolFogLights");
    }

    s_wrapper->m_width = 32;
    s_wrapper->m_height = 1;
    s_wrapper->m_apiSpecificData = s_texture;
    s_wrapper->m_needsCreation = 0;
    s_wrapper->m_needsUpdate = 0;
    s_wrapper->m_needsFlagUpdate = 0;

    return s_wrapper;
#else
    return nullptr;
#endif
}

float VolFog::LightReachLimit() {
    return s_reach;
}

uint32_t VolFog::LightCount() {
    return s_uploaded;
}

uint32_t VolFog::LightTextureGeneration() {
    return s_textureGeneration;
}

void VolFog::DescribeLights(const FrameInputs& in, LightLineSink sink, void* user) {
    char line[640];
    std::snprintf(line, sizeof(line), "  lights: wolVolFogLocalLights=%d worldScene=%d grid nodes=%u point lights in view=%u (pool %u) disabled=%u non-point=%u capture=%s; chosen=%u uploaded=%u shader=%s reach=%.1f yd; stabiliser: %u admitted, %u evicted, %u faded out, %u held",
        s_enabled ? 1 : 0, s_hadScene ? 1 : 0, s_lights.nodesWalked, s_candidatesInView, VolFog::kLocalLightCandidatePool, s_lights.skippedDisabled, s_lights.skippedNonPoint,
        s_lights.capture == LightCapture::Captured ? "ok" : (s_lights.capture == LightCapture::TableTruncated ? "truncated" : "damaged"), s_lights.pointLightCount, s_uploaded,
        s_uploaded ? "lit" : "unlit", s_reach, s_stabiliser.m_admissions, s_stabiliser.m_evictions, s_stabiliser.m_fadeOuts, s_stabiliser.m_heldCaptures);
    sink(user, line);

    const float* p = in.glProjection;

    for (uint32_t i = 0; i < s_uploaded && i < kMaxLocalPointLights; ++i) {
        const PointLight& light = s_lights.pointLights[i];
        float relative[3] = { light.position[0] - in.camPos[0], light.position[1] - in.camPos[1], light.position[2] - in.camPos[2] };
        float view[3];
        VolFog::TransformDirection(relative, in.cameraRelativeView, view);
        const float distance = std::sqrt(relative[0] * relative[0] + relative[1] * relative[1] + relative[2] * relative[2]);
        float pixelX = -1.0f;
        float pixelY = -1.0f;

        if (view[2] > 0.01f) {
            // View space is x right, y up, z forward; ndc = (p0 x / z + p8, p5 y / z + p9); pixels from the top-left corner of the back buffer.
            const float ndcX = p[0] * view[0] / view[2] + p[8];
            const float ndcY = p[5] * view[1] / view[2] + p[9];
            pixelX = static_cast<float>(in.viewport.x) + (ndcX * 0.5f + 0.5f) * static_cast<float>(in.viewport.width);
            pixelY = static_cast<float>(in.viewport.y) + (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(in.viewport.height);
        }

        std::snprintf(line, sizeof(line), "  light %u: world=(%.2f, %.2f, %.2f) dist=%.1f yd view=(%.2f, %.2f, %.2f) screen=(%.0f, %.0f) colour=(%.3f, %.3f, %.3f) uploaded=(%.4f, %.4f, %.4f) attenuation=(%.3f, %.4f, %.5f) cutoff=%.1f yd id=%p",
            i, light.position[0], light.position[1], light.position[2], distance, view[0], view[1], view[2], pixelX, pixelY, light.color[0], light.color[1], light.color[2],
            light.uploadedColor[0], light.uploadedColor[1], light.uploadedColor[2], light.attenuation[0], light.attenuation[1], light.attenuation[2], light.cutoff, reinterpret_cast<void*>(light.nativeId));
        sink(user, line);
    }
}
