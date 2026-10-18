#include "../src/components/ClothData.h"
#include "../src/files/HkxCloth.h"
#include "ClothPayload.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>

using namespace nifly;

namespace {
// Bones of the reference skeleton. Like a real Havok payload, every cloth block lists them next to its own bones.
const std::vector<std::string> skeleton{"Root", "C_Spine"};

bool Skeleton(const std::string& bone) {
    return std::find(skeleton.begin(), skeleton.end(), bone) != skeleton.end();
}

struct ClothSpec {
    std::string label;
    // Bones the cloth writes
    std::vector<std::string> bones;
    // Bones it only reads or collides with
    std::vector<std::string> rig = {};
};

// Starfield cloth is a tagfile that records the bones it writes. Fallout 4 cloth is a packfile the reader can't resolve,
// modelled as a label followed by the bones it names, NUL separated like the strings of a Havok file.
std::unique_ptr<BSClothExtraData> Cloth(const ClothSpec& spec, bool fallout = false) {
    std::string payload;
    if (fallout) {
        payload = spec.label;
        for (const auto* bones : {&skeleton, &spec.rig, &spec.bones}) {
            for (const auto& bone : *bones)
                payload += '\0' + bone;
        }
        payload += '\0';
    }
    else
        payload = ClothPayload::Tagfile(skeleton, {{spec.label, spec.bones, spec.rig}});

    auto block = std::make_unique<BSClothExtraData>();
    block->data.resize(static_cast<uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), block->data.begin());
    return block;
}

void AddCloth(NifFile& nif, const ClothSpec& spec, bool fallout = false) {
    nif.GetRootNode()->extraDataRefs.AddBlockRef(nif.GetHeader().AddBlock(Cloth(spec, fallout)));
}

NiShape* AddShape(NifFile& nif, const std::string& name, const std::vector<std::string>& bones = {}) {
    const std::vector<Vector3> vertices{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const std::vector<Triangle> triangles{{0, 1, 2}};
    const std::vector<Vector2> uvs{{0, 0}, {1, 0}, {0, 1}};
    NiShape* shape = nullptr;
    if (nif.GetHeader().GetVersion().IsSF()) {
        // nifly creates no Starfield geometry from data, so build the BSGeometry the way the game files hold it
        auto block = std::make_unique<BSGeometry>();
        auto geometry = block.get();
        shape = geometry;
        geometry->name.get() = name;
        geometry->SetInternalGeomData(true);
        auto meshSlot = geometry->AddMesh();
        meshSlot->numVerts = 3;
        meshSlot->triSize = 3;
        meshSlot->flags = 64;
        auto& mesh = meshSlot->meshData;
        mesh.version = 2;
        mesh.scale = 1.0f;
        mesh.nVertices = 3;
        mesh.Create(nif.GetHeader().GetVersion(), &vertices, nullptr, nullptr, nullptr);
        mesh.nTriIndices = 3;
        mesh.tris = triangles;
        mesh.nUV1 = 3;
        mesh.uvSets = {uvs};
        mesh.nNormals = 3;
        mesh.normals.assign(3, Vector3(0, 0, 1));
        if (!bones.empty()) {
            mesh.nWeightsPerVert = 4;
            mesh.skinWeights.resize(3, std::vector<BSGeometryMeshData::BoneWeight>(4));
            for (uint16_t v = 0; v < 3; ++v)
                mesh.skinWeights[v][0] = {static_cast<uint16_t>(v % bones.size()), 65535};
        }
        nif.GetRootNode()->childRefs.AddBlockRef(nif.GetHeader().AddBlock(std::move(block)));
    }
    else {
        shape = nif.CreateShapeFromData(name, &vertices, &triangles, &uvs);
        // Earlier games bind bones by NiNode block, Starfield by the names in SkinAttach
        for (const auto& bone : bones) {
            if (!nif.FindBlockByName<NiNode>(bone))
                nif.AddNode(bone, MatTransform());
        }
    }

    if (shape && !bones.empty()) {
        nif.CreateSkinning(shape);
        nif.SetShapeBoneList(shape, bones);
    }
    return shape;
}

NifFile Fixture(const std::string& shape, const std::vector<std::string>& bones, const std::vector<ClothSpec>& cloth, bool fallout = false) {
    NifFile nif;
    nif.Create(fallout ? NiVersion::getFO4() : NiVersion::getSF());
    REQUIRE(AddShape(nif, shape, bones));
    for (const auto& spec : cloth)
        AddCloth(nif, spec, fallout);
    return nif;
}

std::string Label(const BSClothExtraData& block) {
    if (auto cloth = HKX::ReadClothBones(block.data.data(), block.data.size()))
        return cloth->simulations.front().name;
    const std::string payload(block.data.data(), block.data.size());
    return payload.substr(0, payload.find('\0'));
}

std::vector<std::string> Payloads(NifFile& nif) {
    std::vector<std::string> labels;
    for (auto* block : nif.GetChildren<BSClothExtraData>(nullptr, true))
        labels.push_back(Label(*block));
    std::sort(labels.begin(), labels.end());
    return labels;
}

std::vector<std::string> Bones(NifFile& nif, const std::string& shape) {
    std::vector<std::string> bones;
    nif.GetShapeBoneList(nif.FindBlockByName<NiShape>(shape), bones);
    return bones;
}

std::vector<std::string> Labels(const ClothData& data, const ClothData::Sources& sources) {
    std::vector<std::string> labels;
    for (const auto& source : sources)
        labels.push_back(Label(*data.GetBlocks().at(source)));
    std::sort(labels.begin(), labels.end());
    return labels;
}

NifFile ReloadClothFixture(NifFile& input) {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(input.Save(stream) == 0);
    NifFile output;
    REQUIRE(output.Load(stream) == 0);
    return output;
}

std::string Bytes(NifFile& nif) {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(nif.Save(stream) == 0);
    return stream.str();
}

// Exports one shape, as Outfit Studio does with the other shapes deleted, and reads the result back from disk
void CheckClothExport(NifFile& nif, const ClothData& data, const std::string& keep, const std::vector<std::string>& expected,
                      const std::string& reference = {}) {
    NifFile output(nif);
    for (auto* shape : output.GetShapes())
        if (shape->name.get() != keep)
            output.DeleteShape(shape);
    data.Apply(output, Skeleton, reference);
    auto disk = ReloadClothFixture(output);
    REQUIRE(disk.GetShapes().size() == 1);
    CHECK(disk.GetShapes().front()->GetNumVertices() == 3);
    CHECK_FALSE(disk.FindBlockByName<NiStringsExtraData>("BSOS_ClothOwnership_v1"));
    CHECK(Payloads(disk) == expected);
}

std::filesystem::path WorkflowDirectory() {
    const auto* directory = std::getenv("BSOS_CLOTH_DATA_DIR");
    REQUIRE(directory);
    return std::filesystem::u8path(directory);
}

const std::vector<std::string> bodyBones{"C_Spine", "Cloth_Body"};
const std::vector<std::string> coatBones{"C_Spine", "Cloth_Bone_001", "Cloth_Bone_Badge"};
const std::vector<ClothSpec> bodyCloth{{"body physics", {"Cloth_Body"}}};
const std::vector<ClothSpec> coatCloth{{"coat physics", {"Cloth_Bone_001"}, {"C_Spine"}}, {"badge physics", {"Cloth_Bone_Badge"}, {"C_Spine"}}};
} // namespace

TEST_CASE("Exported shapes retain their own cloth instead of the reference cloth", "[cloth][export]") {
    for (bool fallout : {false, true}) {
        CAPTURE(fallout);
        ClothData data;
        auto reference = Fixture("Reference", bodyBones, bodyCloth, fallout);
        auto outfit = Fixture("Coat", coatBones, coatCloth, fallout);
        data.Capture(reference, "naked.nif", reference.GetShapes(), Skeleton);
        data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
        REQUIRE(Payloads(outfit).empty());
        REQUIRE(Payloads(reference).empty());
        NifFile work(reference);
        REQUIRE(work.CloneShape(outfit.GetShapes().front(), "Coat", &outfit));

        SECTION("Export only the outfit") {
            work.DeleteShape(work.FindBlockByName<NiShape>("Reference"));
            data.Apply(work, Skeleton, "Reference");
            CHECK(Payloads(work) == std::vector<std::string>{"badge physics", "coat physics"});
        }
        SECTION("Include the reference") {
            data.Apply(work, Skeleton, "Reference");
            CHECK(Payloads(work) == std::vector<std::string>{"badge physics", "body physics", "coat physics"});
        }
        SECTION("Export only the reference") {
            work.DeleteShape(work.FindBlockByName<NiShape>("Coat"));
            data.Apply(work, Skeleton, "Reference");
            CHECK(Payloads(work) == std::vector<std::string>{"body physics"});
        }
        SECTION("Delete the outfit and reuse its name for clothless geometry") {
            data.RemoveShape("Coat");
            work.DeleteShape(work.FindBlockByName<NiShape>("Coat"));
            REQUIRE(AddShape(work, "Coat"));
            work.DeleteShape(work.FindBlockByName<NiShape>("Reference"));
            data.Apply(work, Skeleton, "Reference");
            CHECK(Payloads(work).empty());
        }
    }
}

TEST_CASE("Loading and replacing a reference preserves the outfit's original cloth", "[cloth][export]") {
    const bool fallout = GENERATE(false, true);
    const bool referenceFirst = GENERATE(false, true);
    const bool replacementHasCloth = GENERATE(false, true);
    CAPTURE(fallout, referenceFirst, replacementHasCloth);

    ClothData data;
    auto outfit = Fixture("Coat", coatBones, coatCloth, fallout);
    auto reference = Fixture("Reference", bodyBones, bodyCloth, fallout);
    if (referenceFirst)
        data.Capture(reference, "reference.nif", reference.GetShapes(), Skeleton);
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    if (!referenceFirst)
        data.Capture(reference, "reference.nif", reference.GetShapes(), Skeleton);

    const auto outfitSources = data.GetShapeSources("Coat");
    NifFile work(outfit);
    REQUIRE(work.CloneShape(reference.GetShapes().front(), "Reference", &reference));

    // Loading another reference removes the old reference shape and reuses its name.
    data.RemoveShape("Reference");
    work.DeleteShape(work.FindBlockByName<NiShape>("Reference"));
    const std::vector<ClothSpec> replacementCloth = replacementHasCloth
        ? std::vector<ClothSpec>{{"replacement body physics", {"Cloth_Body"}}}
        : std::vector<ClothSpec>{};
    auto replacement = Fixture("Reference", bodyBones, replacementCloth, fallout);
    data.Capture(replacement, "reference.nif", replacement.GetShapes(), Skeleton);
    REQUIRE(work.CloneShape(replacement.GetShapes().front(), "Reference", &replacement));
    CHECK(data.GetShapeSources("Coat") == outfitSources);

    data.Apply(work, Skeleton, "Reference");
    auto expected = std::vector<std::string>{"badge physics", "coat physics"};
    if (replacementHasCloth)
        expected.push_back("replacement body physics");
    CHECK(Payloads(work) == expected);

    work.DeleteShape(work.FindBlockByName<NiShape>("Reference"));
    data.Apply(work, Skeleton, "Reference");
    CHECK(Payloads(work) == std::vector<std::string>{"badge physics", "coat physics"});
}

TEST_CASE("Cloth goes with the shapes skinned to the bones it writes", "[cloth][export]") {
    NifFile combined;
    combined.Create(NiVersion::getSF());
    REQUIRE(AddShape(combined, "Reference", bodyBones));
    REQUIRE(AddShape(combined, "Coat", coatBones));
    REQUIRE(AddShape(combined, "Clothless", {"C_Spine"}));
    REQUIRE(AddShape(combined, "Unskinned"));
    for (const auto& spec : {bodyCloth.front(), coatCloth.front(), coatCloth.back()})
        AddCloth(combined, spec);

    SECTION("Every shape keeps only the cloth that moves it") {
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes(), Skeleton);
        CheckClothExport(combined, data, "Reference", {"body physics"});
        CheckClothExport(combined, data, "Coat", {"badge physics", "coat physics"});
        CheckClothExport(combined, data, "Clothless", {});
        CheckClothExport(combined, data, "Unskinned", {});
    }
    SECTION("Cloth that moves skeleton bones goes with every shape skinned to them") {
        AddCloth(combined, {"skeleton physics", {"C_Spine"}});
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes(), Skeleton);
        CheckClothExport(combined, data, "Reference", {"body physics", "skeleton physics"});
        CheckClothExport(combined, data, "Coat", {"badge physics", "coat physics", "skeleton physics"});
        CheckClothExport(combined, data, "Clothless", {"skeleton physics"});
        CheckClothExport(combined, data, "Unskinned", {});
    }
    SECTION("Node and HKX spellings of a bone can differ in case") {
        REQUIRE(AddShape(combined, "Lowercase", {"C_Spine", "cloth_bone_badge"}));
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes(), Skeleton);
        CheckClothExport(combined, data, "Lowercase", {"badge physics"});
    }
    SECTION("Bones the cloth only reads or collides with claim nothing") {
        // A physics body under cloth built for that body's skeleton: the rig names the body bone, but doesn't move it
        REQUIRE(AddShape(combined, "Physics Body", {"C_Spine", "L_BreastSkin"}));
        AddCloth(combined, {"physics coat", {"Cloth_Bone_Physics"}, {"L_BreastSkin"}});
        REQUIRE(AddShape(combined, "Physics Coat", {"C_Spine", "L_BreastSkin", "Cloth_Bone_Physics"}));
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes(), Skeleton);
        CheckClothExport(combined, data, "Physics Coat", {"physics coat"});
        CheckClothExport(combined, data, "Physics Body", {});
    }
    SECTION("Written bones need no reference skeleton") {
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes());
        for (const auto& [keep, expected] : {std::pair<std::string, std::vector<std::string>>{"Reference", {"body physics"}},
                 {"Coat", {"badge physics", "coat physics"}}, {"Clothless", {}}}) {
            CAPTURE(keep);
            NifFile output(combined);
            for (auto* shape : output.GetShapes())
                if (shape->name.get() != keep)
                    output.DeleteShape(shape);
            data.Apply(output);
            CHECK(Payloads(output) == expected);
        }
    }
}

TEST_CASE("Unreadable cloth falls back to the bone names it contains", "[cloth][export]") {
    NifFile combined;
    combined.Create(NiVersion::getFO4());
    REQUIRE(AddShape(combined, "Reference", bodyBones));
    REQUIRE(AddShape(combined, "Coat", coatBones));
    REQUIRE(AddShape(combined, "Clothless", {"C_Spine"}));
    REQUIRE(AddShape(combined, "Unskinned"));
    for (const auto& spec : {bodyCloth.front(), coatCloth.front(), coatCloth.back()})
        AddCloth(combined, spec, true);

    SECTION("Every shape keeps the cloth that names its added bones") {
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes(), Skeleton);
        CHECK(data.GetShapeSources("Reference") == ClothData::Sources{"combined.nif"});
        CHECK(data.GetShapeSources("Coat") == ClothData::Sources{"combined.nif (2)", "combined.nif (3)"});
        CHECK(data.GetShapeSources("Clothless").empty());
        CHECK(data.GetShapeSources("Unskinned").empty());
    }
    SECTION("Cloth that names no added bone stays with every shape") {
        AddCloth(combined, {"skeleton physics", {}}, true);
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes(), Skeleton);
        for (const auto& shape : {"Reference", "Coat", "Clothless", "Unskinned"}) {
            CAPTURE(shape);
            const auto sources = data.GetShapeSources(shape);
            CHECK(std::find(sources.begin(), sources.end(), "combined.nif (4)") != sources.end());
        }
    }
    SECTION("A body bone the cloth only names also claims the cloth") {
        // Known limitation of the fallback: names alone can't tell a written bone from one the cloth collides with
        REQUIRE(AddShape(combined, "Physics Body", {"C_Spine", "L_BreastSkin"}));
        AddCloth(combined, {"physics coat", {"Cloth_Bone_Physics"}, {"L_BreastSkin"}}, true);
        REQUIRE(AddShape(combined, "Physics Coat", {"C_Spine", "L_BreastSkin", "Cloth_Bone_Physics"}));
        ClothData data;
        data.Capture(combined, "combined.nif", combined.GetShapes(), Skeleton);
        CHECK(data.GetShapeSources("Physics Coat") == ClothData::Sources{"combined.nif (4)"});
        CHECK(data.GetShapeSources("Physics Body") == ClothData::Sources{"combined.nif (4)"});
    }
}

TEST_CASE("Cloth sources survive rename duplication and delete undo", "[cloth][export]") {
    ClothData data;
    auto outfit = Fixture("Coat", coatBones, {coatCloth.front()});
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    const auto sources = data.GetShapeSources("Coat");
    data.RenameShape("Coat", "Renamed");
    REQUIRE(NifFile::RenameShape(outfit.GetShapes().front(), "Renamed"));
    REQUIRE(outfit.CloneShape(outfit.GetShapes().front(), "Duplicate"));
    data.SetShapeSources("Duplicate", data.GetShapeSources("Renamed"));
    data.Apply(outfit, Skeleton);
    CHECK(Payloads(outfit) == std::vector<std::string>{"coat physics"});
    data.RemoveShape("Renamed");
    outfit.DeleteShape(outfit.FindBlockByName<NiShape>("Renamed"));
    data.Apply(outfit, Skeleton);
    CHECK(Payloads(outfit) == std::vector<std::string>{"coat physics"});
    data.RemoveShape("Duplicate");
    data.Apply(outfit, Skeleton);
    CHECK(Payloads(outfit).empty());
    data.SetShapeSources("Duplicate", sources); // Undo restores the snapshot.
    data.Apply(outfit, Skeleton);
    CHECK(Payloads(outfit) == std::vector<std::string>{"coat physics"});
}

TEST_CASE("Restoring a project snapshot restores the cloth of every shape", "[cloth][export]") {
    ClothData data;
    auto outfit = Fixture("Coat", coatBones, {coatCloth.front()});
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    NifFile before(outfit);
    const auto snapshot = data.GetAllShapeSources();

    // Edits after the snapshot rename the shape and import another file
    data.RenameShape("Coat", "Renamed");
    auto other = Fixture("Other", coatBones, {{"other physics", {"Cloth_Bone_001"}}});
    data.Capture(other, "other.nif", other.GetShapes(), Skeleton);

    data.SetAllShapeSources(snapshot);
    CHECK(data.GetShapeSources("Renamed").empty());
    CHECK(data.GetShapeSources("Other").empty());
    data.Apply(before, Skeleton);
    CHECK(Payloads(before) == std::vector<std::string>{"coat physics"});
}

TEST_CASE("Reimporting the same path never replaces an earlier shape's cloth", "[cloth][export]") {
    ClothData data;
    auto first = Fixture("First", coatBones, {{"first version", {"Cloth_Bone_001"}}});
    auto second = Fixture("Second", coatBones, {{"second version", {"Cloth_Bone_001"}}});
    data.Capture(first, "outfit.nif", first.GetShapes(), Skeleton);
    data.Capture(second, "outfit.nif", second.GetShapes(), Skeleton);
    data.Apply(first, Skeleton);
    data.Apply(second, Skeleton);
    CHECK(Payloads(first) == std::vector<std::string>{"first version"});
    CHECK(Payloads(second) == std::vector<std::string>{"second version"});
}

TEST_CASE("Reloading a file reuses the cloth blocks it stored", "[cloth][export]") {
    ClothData data;
    const auto outfit = Fixture("Coat", coatBones, coatCloth);
    for (int load = 0; load < 3; ++load) {
        NifFile copy(outfit);
        data.Capture(copy, "coat.nif", copy.GetShapes(), Skeleton);
    }
    CHECK(data.GetBlocks().size() == 2);
    CHECK(data.GetShapeSources("Coat") == ClothData::Sources{"coat.nif", "coat.nif (2)"});

    SECTION("The same payload from another file keeps that file's name") {
        auto other = Fixture("Other", coatBones, {coatCloth.front()});
        data.Capture(other, "other.nif", other.GetShapes(), Skeleton);
        CHECK(data.GetShapeSources("Other") == ClothData::Sources{"other.nif"});
        CHECK(data.GetBlocks().size() == 3);
    }
    SECTION("Identical blocks in one file are stored once") {
        auto doubled = Fixture("Doubled", coatBones, {coatCloth.front(), coatCloth.front()});
        data.Capture(doubled, "doubled.nif", doubled.GetShapes(), Skeleton);
        CHECK(data.GetShapeSources("Doubled") == ClothData::Sources{"doubled.nif"});
    }
    SECTION("Importing the same HKX again lists it once") {
        data.AddManual("extra.hkx", Cloth({"extra physics", {"Cloth_Extra"}}));
        data.AddManual("extra.hkx", Cloth({"extra physics", {"Cloth_Extra"}}));
        CHECK(data.GetManualSources() == ClothData::Sources{"extra.hkx"});
        data.AddManual("extra.hkx", Cloth({"edited physics", {"Cloth_Extra"}}));
        CHECK(data.GetManualSources() == ClothData::Sources{"extra.hkx", "extra.hkx (2)"});
    }
}

TEST_CASE("HKX imports are added where they move exported bones", "[cloth][export]") {
    ClothData data;
    auto outfit = Fixture("Coat", coatBones, {coatCloth.front()});
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);

    SECTION("An import that moves the bones of original cloth replaces it") {
        data.AddManual("rebuilt.hkx", Cloth({"rebuilt coat physics", {"Cloth_Bone_001"}}));
        auto plan = data.Plan(outfit, Skeleton);
        CHECK(Labels(data, plan.sources) == std::vector<std::string>{"coat physics"});
        CHECK(plan.imports.empty());
        CHECK(plan.replacements == std::vector<std::pair<std::string, ClothData::Sources>>{{"rebuilt.hkx", {"coat.nif"}}});
    }
    SECTION("An import that moves bones nothing else moves is added") {
        data.AddManual("badge.hkx", Cloth(coatCloth.back()));
        auto plan = data.Plan(outfit, Skeleton);
        CHECK(plan.imports == ClothData::Sources{"badge.hkx"});
        CHECK(plan.replacements.empty());
        CHECK(plan.undrivenBones.empty());
    }
    SECTION("An import identical to original cloth adds nothing") {
        data.AddManual("duplicate.hkx", Cloth(coatCloth.front()));
        auto plan = data.Plan(outfit, Skeleton);
        CHECK(plan.imports.empty());
        CHECK(plan.replacements.empty());
    }
    SECTION("An import that moves none of the exported bones is left out") {
        data.AddManual("cape.hkx", Cloth({"cape physics", {"Cloth_Cape"}}));
        auto plan = data.Plan(outfit, Skeleton);
        CHECK(plan.imports.empty());
        CHECK(plan.replacements.empty());
        CHECK(plan.unknownImports.empty());
    }
    SECTION("An import whose bones can't be read stays an explicit choice") {
        data.AddManual("extra.hkx", Cloth({"extra physics", {}}, true));
        auto plan = data.Plan(outfit, Skeleton);
        CHECK(plan.unknownImports == ClothData::Sources{"extra.hkx"});
        data.Write(outfit, plan.sources);
        CHECK(Payloads(outfit) == std::vector<std::string>{"coat physics"});
    }
}

TEST_CASE("Each replacing HKX import is decided once over all the cloth it replaces", "[cloth][export]") {
    ClothData data;
    auto outfit = Fixture("Coat", coatBones, coatCloth);
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);

    std::vector<std::string> asked;
    auto answer = [&](const std::set<std::string>& accepted) {
        return [&asked, accepted](const std::string& import, const ClothData::Sources&) {
            asked.push_back(import);
            return accepted.count(import) > 0;
        };
    };

    SECTION("An import that moves the bones of several original blocks replaces all or none of them") {
        data.AddManual("rebuilt.hkx", Cloth({"rebuilt physics", {"Cloth_Bone_001", "Cloth_Bone_Badge"}}));
        auto plan = data.Plan(outfit, Skeleton);
        REQUIRE(plan.replacements.size() == 1);
        CHECK(Labels(data, plan.replacements.front().second) == std::vector<std::string>{"badge physics", "coat physics"});

        CHECK(Labels(data, plan.Resolve(answer({"rebuilt.hkx"}))) == std::vector<std::string>{"rebuilt physics"});
        CHECK(Labels(data, plan.Resolve(answer({}))) == std::vector<std::string>{"badge physics", "coat physics"});
        CHECK(asked == std::vector<std::string>{"rebuilt.hkx", "rebuilt.hkx"});
    }
    SECTION("Declining one import doesn't bring back cloth another import replaced") {
        data.AddManual("first.hkx", Cloth({"first physics", {"Cloth_Bone_001"}}));
        data.AddManual("second.hkx", Cloth({"second physics", {"Cloth_Bone_001"}}));
        auto plan = data.Plan(outfit, Skeleton);
        CHECK(Labels(data, plan.Resolve(answer({"first.hkx"}))) == std::vector<std::string>{"badge physics", "first physics"});
        CHECK(Labels(data, plan.Resolve(answer({"second.hkx"}))) == std::vector<std::string>{"badge physics", "second physics"});
        CHECK(asked == std::vector<std::string>{"first.hkx", "second.hkx", "first.hkx", "second.hkx"});
    }
}

TEST_CASE("Shapes whose cloth was lost before loading are reported", "[cloth][export]") {
    SECTION("An outfit that kept only a body's cloth, like the damaged Issue 615 export") {
        auto damaged = Fixture("Coat", coatBones, bodyCloth);
        ClothData data;
        data.Capture(damaged, "damaged.nif", damaged.GetShapes(), Skeleton);
        CHECK(data.UndrivenBones(damaged, damaged.GetShapes().front(), Skeleton) == std::vector<std::string>{"Cloth_Bone_001", "Cloth_Bone_Badge"});

        auto plan = data.Plan(damaged, Skeleton);
        CHECK(plan.sources.empty());
        CHECK(plan.undrivenBones == std::map<std::string, std::vector<std::string>>{{"Coat", {"Cloth_Bone_001", "Cloth_Bone_Badge"}}});

        // Importing the original cloth repairs it without any choice
        data.AddManual("coat.hkx", Cloth(coatCloth.front()));
        data.AddManual("badge.hkx", Cloth(coatCloth.back()));
        plan = data.Plan(damaged, Skeleton);
        CHECK(plan.imports == ClothData::Sources{"coat.hkx", "badge.hkx"});
        CHECK(plan.undrivenBones.empty());
    }
    SECTION("Valid input is not reported") {
        auto outfit = Fixture("Coat", coatBones, coatCloth);
        ClothData data;
        data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
        CHECK(data.UndrivenBones(outfit, outfit.GetShapes().front(), Skeleton).empty());
        CHECK(data.Plan(outfit, Skeleton).undrivenBones.empty());
    }
    SECTION("Nothing can be told without readable cloth, a skeleton or Starfield bone binding") {
        auto unreadable = Fixture("Coat", coatBones, {{"opaque physics", {}}});
        auto fallout = Fixture("Coat", coatBones, {}, true);
        auto clothless = Fixture("Coat", coatBones, {});
        ClothData data;
        data.Capture(unreadable, "unreadable.nif", unreadable.GetShapes(), Skeleton);
        CHECK(data.UndrivenBones(unreadable, unreadable.GetShapes().front(), Skeleton).empty());
        data.Capture(fallout, "fallout.nif", fallout.GetShapes(), Skeleton);
        CHECK(data.UndrivenBones(fallout, fallout.GetShapes().front(), Skeleton).empty());
        CHECK(data.UndrivenBones(clothless, clothless.GetShapes().front()).empty());
    }
}

TEST_CASE("Cloth follows weight edits", "[cloth][export]") {
    auto outfit = Fixture("Coat", coatBones, coatCloth);
    REQUIRE(AddShape(outfit, "Lining", {"C_Spine"}));
    ClothData data;
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    CheckClothExport(outfit, data, "Lining", {});

    SECTION("A shape that gets cloth bones by a weight copy takes their cloth") {
        REQUIRE(AddShape(outfit, "Copied", {"C_Spine", "Cloth_Bone_001"}));
        CheckClothExport(outfit, data, "Copied", {"coat physics"});
    }
    SECTION("A shape that loses its cloth bones drops their cloth") {
        outfit.DeleteShape(outfit.FindBlockByName<NiShape>("Coat"));
        REQUIRE(AddShape(outfit, "Coat", {"C_Spine", "Cloth_Bone_001"}));
        CheckClothExport(outfit, data, "Coat", {"coat physics"});
        outfit.DeleteShape(outfit.FindBlockByName<NiShape>("Coat"));
        REQUIRE(AddShape(outfit, "Coat", {"C_Spine"}));
        CheckClothExport(outfit, data, "Coat", {});
    }
}

TEST_CASE("Physics of reference skeleton bones stays with the reference", "[cloth][export]") {
    // Like the Issue 615 body: its cloth jiggles bones the actor's skeleton already has
    const ClothSpec jiggle{"body jiggle", {"C_Spine"}};
    auto reference = Fixture("Body", {"C_Spine"}, {jiggle});
    auto outfit = Fixture("Coat", coatBones, coatCloth);
    ClothData data;
    data.Capture(reference, "body.nif", reference.GetShapes(), Skeleton);
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    REQUIRE(outfit.CloneShape(reference.GetShapes().front(), "Body", &reference));

    SECTION("Merging reference geometry into the outfit doesn't bring the physics along") {
        auto merged = data.GetShapeSources("Coat");
        for (const auto& source : data.GetShapeSources("Body"))
            merged.push_back(source);
        data.SetShapeSources("Coat", merged);
        CheckClothExport(outfit, data, "Coat", {"badge physics", "coat physics"}, "Body");
        CheckClothExport(outfit, data, "Body", {"body jiggle"}, "Body");
    }
    SECTION("An outfit that carried the same physics itself keeps it") {
        auto own = Fixture("Coat", coatBones, {coatCloth.front(), coatCloth.back(), jiggle});
        data.Capture(own, "coat.nif", own.GetShapes(), Skeleton);
        CheckClothExport(outfit, data, "Coat", {"badge physics", "body jiggle", "coat physics"}, "Body");
    }
    SECTION("Physics that creates bones outside the skeleton goes with an outfit using them") {
        const ClothSpec breasts{"breast physics", {"L_BreastSkin"}};
        auto physicsBody = Fixture("Body", {"C_Spine", "L_BreastSkin"}, {breasts});
        data.RemoveShape("Body");
        data.Capture(physicsBody, "physics.nif", physicsBody.GetShapes(), Skeleton);
        // Copied weights bring the bone, but not the cloth sources
        REQUIRE(AddShape(outfit, "Copied", {"C_Spine", "Cloth_Bone_001", "L_BreastSkin"}));
        data.SetShapeSources("Copied", data.GetShapeSources("Coat"));
        CheckClothExport(outfit, data, "Copied", {"breast physics", "coat physics"}, "Body");
    }
}

TEST_CASE("A combined file can't tell whose skeleton physics it holds, so reopening keeps it", "[cloth][export][roundtrip]") {
    // Two histories: the outfit file either lacked or carried the body's physics. Both export the same bytes.
    const ClothSpec jiggle{"body jiggle", {"C_Spine"}};
    std::vector<std::string> combinedBytes;
    for (bool outfitCarriedPhysics : {false, true}) {
        CAPTURE(outfitCarriedPhysics);
        auto reference = Fixture("Body", {"C_Spine"}, {jiggle});
        auto specs = coatCloth;
        if (outfitCarriedPhysics)
            specs.push_back(jiggle);
        auto outfit = Fixture("Coat", coatBones, specs);
        ClothData data;
        data.Capture(reference, "body.nif", reference.GetShapes(), Skeleton);
        data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
        REQUIRE(reference.CloneShape(outfit.GetShapes().front(), "Coat", &outfit));

        // During the session, the outfit's own source decides
        CheckClothExport(reference, data, "Coat",
            outfitCarriedPhysics ? std::vector<std::string>{"badge physics", "body jiggle", "coat physics"}
                                 : std::vector<std::string>{"badge physics", "coat physics"},
            "Body");

        data.Apply(reference, Skeleton, "Body");
        CHECK(Payloads(reference) == std::vector<std::string>{"badge physics", "body jiggle", "coat physics"});
        combinedBytes.push_back(Bytes(reference));

        // After reopening, every shape came from the combined file, also when the reference is loaded again
        auto reopened = ReloadClothFixture(reference);
        ClothData again;
        again.Capture(reopened, "combined.nif", reopened.GetShapes(), Skeleton);
        CheckClothExport(reopened, again, "Coat", {"badge physics", "body jiggle", "coat physics"}, "Body");
        again.RemoveShape("Body");
        auto body = Fixture("Body", {"C_Spine"}, {jiggle});
        again.Capture(body, "body.nif", body.GetShapes(), Skeleton);
        CheckClothExport(reopened, again, "Coat", {"badge physics", "body jiggle", "coat physics"}, "Body");
    }
    CHECK(combinedBytes[0] == combinedBytes[1]);
}

TEST_CASE("Cloth ownership survives saving and reopening a combined NIF", "[cloth][export][roundtrip]") {
    const bool fallout = GENERATE(false, true);
    CAPTURE(fallout);
    ClothData data;
    auto reference = Fixture("Reference", bodyBones, bodyCloth, fallout);
    auto outfit = Fixture("Coat", coatBones, coatCloth, fallout);
    data.Capture(reference, "naked.nif", reference.GetShapes(), Skeleton);
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    NifFile combined(reference);
    REQUIRE(combined.CloneShape(outfit.GetShapes().front(), "Coat", &outfit));
    REQUIRE(AddShape(combined, "Clothless", {"C_Spine"}));
    data.Apply(combined, Skeleton, "Reference");

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(combined.Save(stream) == 0);
    NifFile reloaded;
    REQUIRE(reloaded.Load(stream) == 0);
    REQUIRE(Bones(reloaded, "Coat") == coatBones);
    // Import can rename colliding shapes; NIF tools can reorder root blocks.
    REQUIRE(NifFile::RenameShape(reloaded.FindBlockByName<NiShape>("Coat"), "Renamed"));
    auto& refs = reloaded.GetRootNode()->extraDataRefs;
    std::reverse(refs.begin(), refs.end());

    std::string keep;
    std::vector<std::string> expected;
    bool referenceOnly = false;
    SECTION("Export only the outfit") {
        keep = "Renamed";
        expected = {"badge physics", "coat physics"};
    }
    SECTION("Export only the body") {
        keep = "Reference";
        expected = {"body physics"};
    }
    SECTION("A shape skinned to the skeleton alone must not inherit any cloth") {
        keep = "Clothless";
    }
    SECTION("Load a single reference shape from the combined file") {
        keep = "Reference";
        expected = {"body physics"};
        referenceOnly = true;
    }

    ClothData reopened;
    reopened.Capture(reloaded, "combined.nif", referenceOnly
        ? std::vector<NiShape*>{reloaded.FindBlockByName<NiShape>(keep)} : reloaded.GetShapes(), Skeleton);
    for (auto* shape : reloaded.GetShapes()) {
        if (shape->name.get() != keep)
            reloaded.DeleteShape(shape);
    }
    reopened.Apply(reloaded, Skeleton);
    CHECK(Payloads(reloaded) == expected);

    // Another save/reopen must retain the narrowed ownership as well.
    std::stringstream secondStream(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(reloaded.Save(secondStream) == 0);
    NifFile second;
    REQUIRE(second.Load(secondStream) == 0);
    ClothData secondImport;
    secondImport.Capture(second, "outfit-only.nif", second.GetShapes(), Skeleton);
    secondImport.Apply(second, Skeleton);
    CHECK(Payloads(second) == expected);
}

TEST_CASE("Shared cloth stays with each owner after reopening", "[cloth][export][roundtrip]") {
    const ClothSpec shared{"shared physics", {"Cloth_Body", "Cloth_Bone_001"}};
    ClothData data;
    auto reference = Fixture("Reference", bodyBones, {bodyCloth.front(), shared});
    auto outfit = Fixture("Coat", coatBones, {coatCloth.front(), shared});
    data.Capture(reference, "naked.nif", reference.GetShapes(), Skeleton);
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    REQUIRE(reference.CloneShape(outfit.GetShapes().front(), "Coat", &outfit));
    data.Apply(reference, Skeleton, "Reference");
    CHECK(Payloads(reference) == std::vector<std::string>{"body physics", "coat physics", "shared physics"});
    auto reloaded = ReloadClothFixture(reference);
    ClothData reopened;
    reopened.Capture(reloaded, "combined.nif", reloaded.GetShapes(), Skeleton);
    reloaded.DeleteShape(reloaded.FindBlockByName<NiShape>("Reference"));
    reopened.Apply(reloaded, Skeleton);
    CHECK(Payloads(reloaded) == std::vector<std::string>{"coat physics", "shared physics"});
}

TEST_CASE("Cloth edited outside Outfit Studio keeps following its bones", "[cloth][export][roundtrip]") {
    ClothData data;
    auto reference = Fixture("Reference", bodyBones, bodyCloth);
    auto outfit = Fixture("Coat", coatBones, coatCloth);
    data.Capture(reference, "naked.nif", reference.GetShapes(), Skeleton);
    data.Capture(outfit, "coat.nif", outfit.GetShapes(), Skeleton);
    NifFile combined(reference);
    REQUIRE(combined.CloneShape(outfit.GetShapes().front(), "Coat", &outfit));
    REQUIRE(AddShape(combined, "Clothless", {"C_Spine"}));
    data.Apply(combined, Skeleton, "Reference");
    auto reloaded = ReloadClothFixture(combined);

    std::vector<std::string> coat;
    std::vector<std::string> body;
    std::vector<std::string> clothless;
    SECTION("A rebuilt payload follows the shape skinned to its bones") {
        for (auto* block : reloaded.GetChildren<BSClothExtraData>(nullptr, true)) {
            if (Label(*block) == "coat physics")
                block->data = Cloth({"coat physics (rebuilt)", {"Cloth_Bone_001"}})->data;
        }
        coat = {"badge physics", "coat physics (rebuilt)"};
        body = {"body physics"};
    }
    SECTION("An unreadable payload that names no outfit bone stays with every shape") {
        AddCloth(reloaded, {"new physics", {}}, true);
        coat = {"badge physics", "coat physics", "new physics"};
        body = {"body physics", "new physics"};
        clothless = {"new physics"};
    }

    for (const auto& [keep, expected] : {std::pair{"Coat", coat}, std::pair{"Reference", body}, std::pair{"Clothless", clothless}}) {
        CAPTURE(keep);
        ClothData reopened;
        NifFile work(reloaded);
        reopened.Capture(work, "combined.nif", work.GetShapes(), Skeleton);
        CheckClothExport(work, reopened, keep, expected);
    }
}

TEST_CASE("A shape can own dedicated and shared skeleton cloth", "[cloth][export][roundtrip]") {
    const bool fallout = GENERATE(false, true);
    const bool captureOnlyOwner = GENERATE(false, true);
    CAPTURE(fallout, captureOnlyOwner);
    const ClothSpec shared{"shared skeleton physics", {"C_Spine"}};
    const std::vector<std::string> allBones{"C_Spine", "Cloth_Bone_001"};
    auto coat = Fixture("Coat", allBones, {coatCloth.front(), shared}, fallout);
    auto body = Fixture("Body", {"C_Spine"}, {shared}, fallout);
    ClothData imported;
    imported.Capture(coat, "coat.nif", coat.GetShapes(), Skeleton);
    imported.Capture(body, "body.nif", body.GetShapes(), Skeleton);
    REQUIRE(coat.CloneShape(body.GetShapes().front(), "Body", &body));
    // Ground truth before reopen: both sources really contained the same payload.
    CheckClothExport(coat, imported, "Coat", {"coat physics", "shared skeleton physics"});
    CheckClothExport(coat, imported, "Body", {"shared skeleton physics"});
    imported.Apply(coat, Skeleton);
    REQUIRE(Payloads(coat) == std::vector<std::string>{"coat physics", "shared skeleton physics"});
    auto reopenedNif = ReloadClothFixture(coat);
    ClothData reopened;
    reopened.Capture(reopenedNif, "combined.nif", captureOnlyOwner
        ? std::vector<NiShape*>{reopenedNif.FindBlockByName<NiShape>("Coat")} : reopenedNif.GetShapes(), Skeleton);
    CheckClothExport(reopenedNif, reopened, "Coat", {"coat physics", "shared skeleton physics"});
}

TEST_CASE("Sharing cloth does not require identical complete skin bone lists", "[cloth][export][roundtrip]") {
    const bool fallout = GENERATE(false, true);
    const bool reverse = GENERATE(false, true);
    CAPTURE(fallout, reverse);
    const ClothSpec shared{"shared skeleton physics", {"C_Spine"}};
    auto first = Fixture("First", {"C_Spine"}, {shared}, fallout);
    auto second = Fixture("Second", {"C_Spine", "ExtraAnimatedBone"}, {shared}, fallout);
    auto accessory = Fixture("Accessory", coatBones, {coatCloth.front()}, fallout);
    ClothData imported;
    imported.Capture(first, "first.nif", first.GetShapes(), Skeleton);
    imported.Capture(second, "second.nif", second.GetShapes(), Skeleton);
    imported.Capture(accessory, "accessory.nif", accessory.GetShapes(), Skeleton);
    REQUIRE(first.CloneShape(second.GetShapes().front(), "Second", &second));
    REQUIRE(first.CloneShape(accessory.GetShapes().front(), "Accessory", &accessory));
    CheckClothExport(first, imported, "First", {"shared skeleton physics"});
    CheckClothExport(first, imported, "Second", {"shared skeleton physics"});
    imported.Apply(first, Skeleton);
    if (reverse) {
        auto& refs = first.GetRootNode()->extraDataRefs;
        std::reverse(refs.begin(), refs.end());
    }
    auto reopenedNif = ReloadClothFixture(first);
    ClothData reopened;
    reopened.Capture(reopenedNif, "combined.nif", reopenedNif.GetShapes(), Skeleton);
    CheckClothExport(reopenedNif, reopened, "First", {"shared skeleton physics"});
    CheckClothExport(reopenedNif, reopened, "Second", {"shared skeleton physics"});
}

TEST_CASE("Original multi-shape imports retain legitimate shared cloth", "[cloth][export][roundtrip]") {
    const bool fallout = GENERATE(false, true);
    CAPTURE(fallout);
    auto original = Fixture("Coat", coatBones, {coatCloth.front(), {"shared skeleton physics", {"C_Spine"}}}, fallout);
    REQUIRE(AddShape(original, "Lining", {"C_Spine"}));
    auto fromDisk = ReloadClothFixture(original);
    ClothData imported;
    imported.Capture(fromDisk, "original.nif", fromDisk.GetShapes(), Skeleton);
    CheckClothExport(fromDisk, imported, "Coat", {"coat physics", "shared skeleton physics"});
    CheckClothExport(fromDisk, imported, "Lining", {"shared skeleton physics"});
}

TEST_CASE("Create cloth preservation workflow fixtures", "[.cloth-data-setup]") {
    auto directory = WorkflowDirectory();
    std::filesystem::create_directories(directory);
    auto reference = Fixture("Reference", bodyBones, bodyCloth);
    auto outfit = Fixture("Coat", coatBones, coatCloth);
    REQUIRE(reference.Save(directory / "reference.nif") == 0);
    REQUIRE(outfit.Save(directory / "outfit.nif") == 0);
    NifFile rig;
    rig.Create(NiVersion::getSF());
    rig.GetRootNode()->name.get() = "Root";
    REQUIRE(rig.AddNode("C_Spine", MatTransform()));
    REQUIRE(rig.Save(directory / "skeleton.nif") == 0);
}

TEST_CASE("Verify actual Outfit Studio cloth preservation exports", "[.cloth-data-verify]") {
    auto directory = WorkflowDirectory();
    for (const auto& name : {"outfit-only.nif", "edited.nif", "reopened.nif", "ShapeData/Cloth/Cloth.nif",
             "combined-reopened-outfit.nif", "project-reopened-outfit.nif"}) {
        INFO(name);
        NifFile nif;
        REQUIRE(nif.Load(directory / name) == 0);
        CHECK(nif.GetShapes().size() == 1);
        CHECK(Payloads(nif) == std::vector<std::string>{"badge physics", "coat physics"});
    }
    NifFile both;
    REQUIRE(both.Load(directory / "with-reference.nif") == 0);
    CHECK(both.GetShapes().size() == 2);
    CHECK(Payloads(both) == std::vector<std::string>{"badge physics", "body physics", "coat physics"});
    NifFile savedProject;
    REQUIRE(savedProject.Load(directory / "ShapeData/Combined/Combined.nif") == 0);
    CHECK(savedProject.GetShapes().size() == 2);
    CHECK(Payloads(savedProject) == std::vector<std::string>{"badge physics", "body physics", "coat physics"});
    NifFile merged;
    REQUIRE(merged.Load(directory / "merged.nif") == 0);
    REQUIRE(merged.GetShapes().size() == 1);
    CHECK(merged.GetShapes().front()->GetNumVertices() == 6);
    CHECK(Payloads(merged) == std::vector<std::string>{"badge physics", "body physics", "coat physics"});
}
