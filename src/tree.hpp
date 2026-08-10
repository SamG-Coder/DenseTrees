#pragma once

#include "math.hpp"
#include <cstdint>
#include <vector>

namespace dense {

enum class TreeSpecies : uint8_t { EnglishOak, NorwaySpruce, SilverBirch, WeepingWillow, UmbrellaAcacia };

struct SpeciesTraits {
    const wchar_t* name = L"English oak";
    float apicalDominance = 0.35f;
    float phototropism = 0.78f;
    float gravitropism = 0.08f;
    float branchDroop = 0.03f;
    float crownTaper = 0.25f;
    float crownFlatness = 1.0f;
    // Half-extents calibrated to a roughly 15 cm^2 mature Q. robur lamina.
    float leafWidth = 0.025f;
    float leafLength = 0.039f;
    float leafDensity = 1.0f;
    float pipeExponent = 2.15f;
    uint32_t barkColor = 0xff4e5e69u;
    uint32_t leafColor = 0xff277638u;
};

struct TreeParameters {
    TreeSpecies species = TreeSpecies::EnglishOak;
    uint32_t seed = 5080;
    int attractionPoints = 1100;
    int growthIterations = 115;
    float crownHeight = 7.2f;
    float crownRadius = 3.3f;
    float trunkHeight = 2.2f;
    float segmentLength = 0.22f;
    float attractionRadius = 1.15f;
    float killRadius = 0.30f;
    float upwardBias = 0.10f;
    float sunlightAzimuth = 0.55f;
    float waterAvailability = 0.82f;
    bool fullBiologicalInventory = true;
};

struct BranchNode {
    Vec3 position{};
    Vec3 direction{0,1,0};
    int parent = -1;
    float radius = 0.025f;
    int children = 0;
    int age = 0;
    int axisOrder = 0;
    float lightExposure = 1.0f;
    float carbonReserve = 1.0f;
    bool alive = true;
    int growthUnitStart = -1;
    int birthSeason = 0;
    bool currentYear = false;
};

struct MeshVertex { Vec3 position; Vec3 normal; uint32_t color; float material = 0.0f; float u = 0.0f; float v = 0.0f; };
struct TreeMesh {
    std::vector<MeshVertex> branchVertices;
    std::vector<uint32_t> branchIndices;
    std::vector<MeshVertex> leafVertices;
    std::vector<uint32_t> leafIndices;
    uint32_t structuralSegments = 0;
    uint32_t fineShootSegments = 0;
    uint32_t leafCount = 0;
    float totalLeafAreaM2 = 0;
};

class TreeGenerator {
public:
    static SpeciesTraits traits(TreeSpecies species);
    static TreeParameters parametersFor(TreeSpecies species, uint32_t seed = 5080);
    std::vector<BranchNode> grow(const TreeParameters& parameters) const;
    TreeMesh buildMesh(std::vector<BranchNode>& nodes, const TreeParameters& parameters) const;
};

}
