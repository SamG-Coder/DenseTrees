#include "tree.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

bool finite(dense::Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

int main() {
    dense::TreeGenerator generator;
    dense::TreeParameters p;
    p.attractionPoints = 360;
    p.growthIterations = 55;
    p.fullBiologicalInventory = false;
    auto a = generator.grow(p);
    auto b = generator.grow(p);
    require(a.size() > 40, "generic generator produced no useful branch hierarchy");
    require(a.size() == b.size(), "same seed did not reproduce the same node count");
    for (size_t i = 0; i < a.size(); ++i) {
        require(a[i].position.x == b[i].position.x && a[i].position.y == b[i].position.y &&
                    a[i].position.z == b[i].position.z,
                "same seed did not reproduce identical node positions");
        require(finite(a[i].position), "generic generator emitted a non-finite position");
        if (i) {
            require(a[i].parent >= 0 && static_cast<size_t>(a[i].parent) < i,
                    "generic branch parent must precede its child");
        }
    }

    auto mesh = generator.buildMesh(a, p);
    require(!mesh.branchVertices.empty() && mesh.branchIndices.size() % 3 == 0,
            "generic branch mesh is empty or has incomplete triangles");
    require(!mesh.leafVertices.empty() && mesh.leafIndices.size() % 3 == 0,
            "generic leaf mesh is empty or has incomplete triangles");

    for (auto species : {dense::TreeSpecies::EnglishOak, dense::TreeSpecies::NorwaySpruce,
                         dense::TreeSpecies::SilverBirch, dense::TreeSpecies::WeepingWillow,
                         dense::TreeSpecies::UmbrellaAcacia}) {
        auto preset = dense::TreeGenerator::parametersFor(species, 42);
        preset.attractionPoints = 240;
        preset.growthIterations = 45;
        preset.fullBiologicalInventory = false;
        auto nodes = generator.grow(preset);
        require(nodes.size() > 20, "species preset produced no useful branch hierarchy");
        auto speciesMesh = generator.buildMesh(nodes, preset);
        require(!speciesMesh.branchIndices.empty(), "species preset produced no branch triangles");
    }

    // Canonical mature, open-grown Q. robur. These checks constrain architecture,
    // not a frozen geometry budget: node and leaf totals may grow as detail improves.
    auto oak = dense::TreeGenerator::parametersFor(dense::TreeSpecies::EnglishOak, 5080);
    auto oakNodes = generator.grow(oak);
    require(!oakNodes.empty(), "mature oak generator returned no nodes");
    // A catastrophe guard, not a visual quota. It catches accidental exponential
    // runaway early while leaving well over an order of magnitude of headroom.
    require(oakNodes.size() < 2'000'000, "mature oak node population ran away catastrophically");

    auto oakMesh = generator.buildMesh(oakNodes, oak);
    require(!oakMesh.branchVertices.empty() && oakMesh.branchIndices.size() % 3 == 0,
            "mature oak branch mesh is empty or malformed");
    require(!oakMesh.leafVertices.empty() && oakMesh.leafIndices.size() % 3 == 0,
            "mature oak leaf mesh is empty or malformed");
    require(oakMesh.branchVertices.size() <= std::numeric_limits<uint32_t>::max(),
            "mature oak branch vertices exceed the 32-bit mesh index range");
    require(oakMesh.leafVertices.size() <= std::numeric_limits<uint32_t>::max(),
            "mature oak leaf vertices exceed the 32-bit mesh index range");

    dense::Vec3 minimum = oakNodes.front().position;
    dense::Vec3 maximum = minimum;
    std::array<size_t, 5> orders{};
    std::array<size_t, 5> terminals{};
    std::array<size_t, 16> outerShootSectors{};
    std::vector<float> primaryAzimuths;
    std::vector<float> primaryAttachmentHeights;
    size_t currentYearShoots = 0;
    size_t leafBearingShoots = 0;

    for (size_t i = 0; i < oakNodes.size(); ++i) {
        const auto& node = oakNodes[i];
        require(finite(node.position) && finite(node.direction) && std::isfinite(node.radius),
                "mature oak contains non-finite node data");
        require(node.axisOrder >= 0 && node.axisOrder <= 4,
                "mature oak contains an invalid botanical axis order");
        if (i) {
            require(node.parent >= 0 && static_cast<size_t>(node.parent) < i,
                    "mature oak branch parent must precede its child");
        }

        minimum.x = std::min(minimum.x, node.position.x);
        minimum.y = std::min(minimum.y, node.position.y);
        minimum.z = std::min(minimum.z, node.position.z);
        maximum.x = std::max(maximum.x, node.position.x);
        maximum.y = std::max(maximum.y, node.position.y);
        maximum.z = std::max(maximum.z, node.position.z);
        orders[static_cast<size_t>(node.axisOrder)]++;
        if (node.children == 0) terminals[static_cast<size_t>(node.axisOrder)]++;

        if (i && node.axisOrder == 1 && oakNodes[static_cast<size_t>(node.parent)].axisOrder == 0) {
            primaryAzimuths.push_back(std::atan2(node.direction.z, node.direction.x));
            primaryAttachmentHeights.push_back(oakNodes[static_cast<size_t>(node.parent)].position.y);
        }

        if (node.currentYear) {
            ++currentYearShoots;
            require(node.alive, "current-year oak shoot was marked dead before leaf attachment");
            require(node.axisOrder == 4 && node.children == 0,
                    "current-year oak foliage must terminate a fourth-order shoot");
            require(node.growthUnitStart == node.parent && node.parent >= 0,
                    "current-year oak shoot has no real parent growth-unit segment");
            require(node.birthSeason > 0, "current-year oak shoot has no birth season");
            const auto& start = oakNodes[static_cast<size_t>(node.growthUnitStart)];
            require(start.axisOrder == 3,
                    "current-year oak shoot must arise from a surviving third-order twig");
            const float shootLength = dense::length(node.position - start.position);
            require(shootLength >= .020f && shootLength <= .085f,
                    "current-year fourth-order shoot falls outside the measured compact length envelope");

            const float horizontalRadius = std::sqrt(node.position.x * node.position.x +
                                                     node.position.z * node.position.z);
            if (horizontalRadius > oak.crownRadius * .45f) {
                float angle = std::atan2(node.position.z, node.position.x);
                if (angle < 0) angle += 2 * dense::pi;
                size_t sector = static_cast<size_t>(angle / (2 * dense::pi) * outerShootSectors.size());
                sector = std::min(sector, outerShootSectors.size() - 1);
                outerShootSectors[sector]++;
            }
        }

        // This exactly mirrors the foliage eligibility contract in buildMesh.
        if (node.alive && node.axisOrder == 4 && node.children == 0 && node.radius <= .0048f) {
            ++leafBearingShoots;
            require(node.currentYear && node.growthUnitStart == node.parent,
                    "leaf-bearing oak terminal is not a real current-year growth unit");
        }
    }

    const float spanX = maximum.x - minimum.x;
    const float treeHeight = maximum.y - minimum.y;
    const float spanZ = maximum.z - minimum.z;
    const float crownMajor = std::max(spanX, spanZ);
    const float crownMinor = std::min(spanX, spanZ);
    require(treeHeight >= (oak.trunkHeight + oak.crownHeight) * .84f &&
                treeHeight <= (oak.trunkHeight + oak.crownHeight) * 1.30f,
            "mature oak height escaped its biological envelope");
    require(crownMajor / treeHeight >= 1.40f && crownMajor / treeHeight <= 2.30f,
            "open-grown oak crown must be broad rather than columnar or implausibly flat");
    require(crownMinor / treeHeight >= 1.15f,
            "open-grown oak must spread substantially in both horizontal dimensions");
    require(crownMajor / crownMinor <= 1.55f,
            "open-grown oak crown became an implausibly narrow one-axis fan");

    require(primaryAttachmentHeights.size() >= 5 && primaryAttachmentHeights.size() <= 16,
            "mature oak should have a small population of major reiterating limbs");
    const float firstPrimaryHeight = *std::min_element(primaryAttachmentHeights.begin(),
                                                       primaryAttachmentHeights.end());
    require(firstPrimaryHeight >= 1.60f && firstPrimaryHeight <= 2.50f,
            "mature open-grown oak must fork low without becoming a ground-level starburst");
    std::sort(primaryAttachmentHeights.begin(), primaryAttachmentHeights.end());
    size_t distinctAttachmentLevels = 0;
    float lastAttachment = -std::numeric_limits<float>::infinity();
    for (float height : primaryAttachmentHeights) {
        if (height - lastAttachment > .15f) {
            ++distinctAttachmentLevels;
            lastAttachment = height;
        }
    }
    require(distinctAttachmentLevels >= 3,
            "major oak limbs must emerge sequentially, not as radial arms from one junction");

    require(primaryAzimuths.size() == primaryAttachmentHeights.size(),
            "major-limb direction and attachment accounting diverged");
    std::sort(primaryAzimuths.begin(), primaryAzimuths.end());
    float largestPrimaryGap = 0;
    float smallestPrimaryGap = 2 * dense::pi;
    for (size_t i = 0; i < primaryAzimuths.size(); ++i) {
        const float next = i + 1 < primaryAzimuths.size()
                               ? primaryAzimuths[i + 1]
                               : primaryAzimuths.front() + 2 * dense::pi;
        const float gap = next - primaryAzimuths[i];
        largestPrimaryGap = std::max(largestPrimaryGap, gap);
        smallestPrimaryGap = std::min(smallestPrimaryGap, gap);
    }
    const float meanPrimaryGap = 2 * dense::pi / static_cast<float>(primaryAzimuths.size());
    require(largestPrimaryGap > meanPrimaryGap * 1.35f && largestPrimaryGap < 2.45f,
            "major oak limbs reverted to an even radial whorl or a one-sided fan");
    require(smallestPrimaryGap > .10f,
            "multiple major oak limbs collapsed onto the same radial direction");

    require(currentYearShoots > 0 && currentYearShoots == leafBearingShoots,
            "oak foliage set and current-year growth-unit set must match exactly");
    const uint64_t minimumLeaves = static_cast<uint64_t>(leafBearingShoots) * 8;
    const uint64_t maximumLeaves = static_cast<uint64_t>(leafBearingShoots) * 14;
    require(oakMesh.leafCount >= minimumLeaves && oakMesh.leafCount <= maximumLeaves,
            "oak foliage must contain 8-14 leaves per surviving current-year shoot");
    const float meanLeafAreaCm2 = oakMesh.totalLeafAreaM2 * 10'000.0f /
                                  static_cast<float>(oakMesh.leafCount);
    require(std::isfinite(meanLeafAreaCm2) && meanLeafAreaCm2 >= 8.0f && meanLeafAreaCm2 <= 23.0f,
            "oak mean leaf area escaped the Q. robur lamina envelope");

    size_t occupiedSectors = 0;
    size_t sectorMaximum = 0;
    double sectorSum = 0;
    for (size_t count : outerShootSectors) {
        occupiedSectors += count != 0;
        sectorMaximum = std::max(sectorMaximum, count);
        sectorSum += static_cast<double>(count);
    }
    require(occupiedSectors >= 12,
            "mature oak foliage no longer occupies a broadly spreading crown");
    const double sectorMean = sectorSum / outerShootSectors.size();
    double sectorVariance = 0;
    for (size_t count : outerShootSectors) {
        const double delta = static_cast<double>(count) - sectorMean;
        sectorVariance += delta * delta;
    }
    sectorVariance /= outerShootSectors.size();
    const double sectorCv = std::sqrt(sectorVariance) / sectorMean;
    require(sectorCv >= .10 && sectorCv <= 1.20,
            "outer oak foliage must be irregularly occupied without collapsing into one sector");
    require(static_cast<double>(sectorMaximum) < sectorMean * 3.0,
            "one foliage sector monopolized the mature oak crown");

    for (size_t order = 0; order < orders.size(); ++order) {
        require(orders[order] > 0, "mature oak is missing a required axis order");
    }
    require(terminals[4] == currentYearShoots,
            "fourth-order oak terminals must correspond to explicit current-year shoots");

    std::cout << "oak nodes=" << oakNodes.size() << " leaves=" << oakMesh.leafCount
              << " leafArea=" << oakMesh.totalLeafAreaM2 << "m2 extent=" << spanX << 'x'
              << treeHeight << 'x' << spanZ << " width/height=" << crownMajor / treeHeight
              << " fork=" << firstPrimaryHeight << "m attachmentLevels=" << distinctAttachmentLevels
              << " sectorCV=" << sectorCv << " leaves/shoot="
              << static_cast<double>(oakMesh.leafCount) / leafBearingShoots << " orders="
              << orders[0] << ',' << orders[1] << ',' << orders[2] << ',' << orders[3] << ','
              << orders[4] << '\n';
    std::cout << "nodes=" << a.size() << " branchTriangles=" << mesh.branchIndices.size() / 3
              << " leafTriangles=" << mesh.leafIndices.size() / 3 << '\n';
}
