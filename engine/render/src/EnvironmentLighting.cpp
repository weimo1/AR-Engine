#include "render/EnvironmentLighting.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/vec3.hpp>

namespace ar::render {
namespace {

constexpr std::uint32_t cubeExtent = 16;
constexpr std::uint32_t sampleCount = 128;

struct SamplePoint {
    float radial;
    float cosPhi;
    float sinPhi;
};

const auto samples = [] {
    std::array<SamplePoint, sampleCount> points{};
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        float radicalInverse = 0.F;
        float bitWeight = 0.5F;
        for (auto bits = i; bits != 0; bits >>= 1) {
            radicalInverse += static_cast<float>(bits & 1u) * bitWeight;
            bitWeight *= 0.5F;
        }
        const float phi = 2.F * std::numbers::pi_v<float> * radicalInverse;
        points[i] = {
            (static_cast<float>(i) + 0.5F) / static_cast<float>(sampleCount),
            std::cos(phi),
            std::sin(phi)};
    }
    return points;
}();

struct Basis {
    glm::vec3 normal;
    glm::vec3 tangent;
    glm::vec3 bitangent;

    explicit Basis(glm::vec3 direction) : normal(direction) {
        const glm::vec3 up =
            std::abs(normal.z) < 0.999F ? glm::vec3{0.F, 0.F, 1.F} : glm::vec3{1.F, 0.F, 0.F};
        tangent = glm::normalize(glm::cross(up, normal));
        bitangent = glm::cross(normal, tangent);
    }

    glm::vec3 direction(const SamplePoint& sample, float cosine) const {
        const float sine = std::sqrt(std::max(0.F, 1.F - cosine * cosine));
        return normal * cosine + (tangent * sample.cosPhi + bitangent * sample.sinPhi) * sine;
    }
};

glm::vec3 cubeDirection(std::uint32_t face, float u, float v) {
    const std::array<glm::vec3, 6> directions{{
        {1.F, -v, -u},
        {-1.F, -v, u},
        {u, 1.F, v},
        {u, -1.F, -v},
        {u, -v, 1.F},
        {-u, -v, -1.F},
    }};
    return glm::normalize(directions[face]);
}

glm::vec3 radiance(EnvironmentPreset preset, glm::vec3 direction) {
    if (preset == EnvironmentPreset::Constant) {
        return {0.25F, 0.5F, 0.75F};
    }
    return direction * 0.5F + glm::vec3{0.5F};
}

float ggxCosine(float radial, float alphaSquared) {
    return std::sqrt((1.F - radial) / (1.F - radial + alphaSquared * radial));
}

float smithG1(float cosine, float alphaSquared) {
    return 2.F * cosine /
           (cosine + std::sqrt(alphaSquared + (1.F - alphaSquared) * cosine * cosine));
}

glm::vec3 bakeDiffuse(EnvironmentPreset preset, glm::vec3 normal) {
    const Basis basis(normal);
    glm::vec3 total{0.F};
    for (const auto& sample : samples) {
        total += radiance(preset, basis.direction(sample, std::sqrt(1.F - sample.radial)));
    }
    return total / static_cast<float>(sampleCount);
}

glm::vec3 bakeSpecular(EnvironmentPreset preset, glm::vec3 normal, float roughness) {
    if (roughness == 0.F) {
        return radiance(preset, normal);
    }
    const float alpha = roughness * roughness;
    const Basis basis(normal);
    glm::vec3 total{0.F};
    float weight = 0.F;
    for (const auto& sample : samples) {
        const auto half = basis.direction(sample, ggxCosine(sample.radial, alpha * alpha));
        const auto light = glm::reflect(-normal, half);
        const float cosine = glm::dot(normal, light);
        if (cosine > 0.F) {
            total += radiance(preset, light) * cosine;
            weight += cosine;
        }
    }
    return total / weight;
}

std::array<float, 2> integrateBrdf(float nv, float roughness) {
    const glm::vec3 view{std::sqrt(1.F - nv * nv), 0.F, nv};
    const float alpha = std::max(roughness * roughness, 0.002F);
    const float alphaSquared = alpha * alpha;
    const float viewMask = smithG1(nv, alphaSquared);
    std::array<float, 2> total{};
    for (const auto& sample : samples) {
        const float nh = ggxCosine(sample.radial, alphaSquared);
        const float sine = std::sqrt(std::max(0.F, 1.F - nh * nh));
        const glm::vec3 half{sine * sample.cosPhi, sine * sample.sinPhi, nh};
        const float vh = glm::dot(view, half);
        const float nl = glm::reflect(-view, half).z;
        if (nl > 0.F && vh > 0.F) {
            const float visible = viewMask * smithG1(nl, alphaSquared) * vh / (nh * nv);
            const float factor = 1.F - std::min(vh, 1.F);
            const float squared = factor * factor;
            const float fresnel = squared * squared * factor;
            total[0] += (1.F - fresnel) * visible;
            total[1] += fresnel * visible;
        }
    }
    // Bound numerical overshoot at unit Fresnel for the finite sample sequence.
    const float normalization = std::max(static_cast<float>(sampleCount), total[0] + total[1]);
    return {total[0] / normalization, total[1] / normalization};
}

template <typename Evaluate> EnvironmentCubeMip bakeCube(std::uint32_t extent, Evaluate evaluate) {
    EnvironmentCubeMip result{extent, {}};
    result.texels.reserve(6u * extent * extent);
    const float scale = 2.F / static_cast<float>(extent);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < extent; ++y) {
            for (std::uint32_t x = 0; x < extent; ++x) {
                const auto normal = cubeDirection(
                    face,
                    (static_cast<float>(x) + 0.5F) * scale - 1.F,
                    (static_cast<float>(y) + 0.5F) * scale - 1.F);
                const auto rgb = evaluate(normal);
                result.texels.push_back({rgb.x, rgb.y, rgb.z, 1.F});
            }
        }
    }
    return result;
}

} // namespace

EnvironmentLightingData bakeEnvironmentLighting(EnvironmentPreset preset) {
    EnvironmentLightingData environment{};
    environment.diffuse =
        bakeCube(cubeExtent, [preset](auto normal) { return bakeDiffuse(preset, normal); });
    const auto lastMip = static_cast<float>(environment.specular.size() - 1);
    for (std::size_t mip = 0; mip < environment.specular.size(); ++mip) {
        const float roughness = static_cast<float>(mip) / lastMip;
        environment.specular[mip] = bakeCube(cubeExtent >> mip, [preset, roughness](auto normal) {
            return bakeSpecular(preset, normal, roughness);
        });
    }
    constexpr auto extent = EnvironmentLightingData::brdfExtent;
    environment.brdf.reserve(extent * extent);
    for (std::uint32_t y = 0; y < extent; ++y) {
        for (std::uint32_t x = 0; x < extent; ++x) {
            environment.brdf.push_back(integrateBrdf(
                (static_cast<float>(x) + 0.5F) / extent, (static_cast<float>(y) + 0.5F) / extent));
        }
    }
    return environment;
}

PackedEnvironmentLighting packEnvironmentLighting(const EnvironmentLightingData& environment) {
    PackedEnvironmentLighting packed;
    const auto append = [](auto& destination, const auto& texels) {
        if (!texels.empty()) {
            destination.reserve(destination.size() + texels.size() * texels.front().size());
        }
        for (const auto& texel : texels) {
            for (float channel : texel) {
                destination.push_back(glm::packHalf1x16(channel));
            }
        }
    };
    append(packed.diffuse, environment.diffuse.texels);
    for (const auto& mip : environment.specular) {
        append(packed.specular, mip.texels);
    }
    append(packed.brdf, environment.brdf);
    return packed;
}

} // namespace ar::render
