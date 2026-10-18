#include "../src/files/HkxCloth.h"
#include "ClothPayload.h"

#include "NifFile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {
const std::vector<std::string> rig{"Root", "C_Spine", "L_Thigh"};

std::optional<HKX::ClothBones> Read(const std::string& payload) {
	return HKX::ReadClothBones(payload.data(), payload.size());
}

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}
} // namespace

TEST_CASE("Cloth payloads tell the bones they write from the bones they read", "[cloth][hkx]") {
	const auto payload = ClothPayload::Tagfile(rig,
											   {{"Doctor_Cloth", {"Cloth_Bone_001", "Cloth_Bone_002"}, {"C_Spine", "L_Thigh"}},
												{"Doctor_Badge", {"Cloth_Bone_Badge"}, {"C_Spine"}}});
	const auto cloth = Read(payload);
	REQUIRE(cloth);
	CHECK(cloth->skeleton == std::vector<std::string>{"Root", "C_Spine", "L_Thigh", "Cloth_Bone_001", "Cloth_Bone_002", "Cloth_Bone_Badge"});
	REQUIRE(cloth->simulations.size() == 2);
	CHECK(cloth->simulations[0].name == "Doctor_Cloth");
	CHECK(cloth->simulations[0].writtenBones == std::vector<std::string>{"Cloth_Bone_001", "Cloth_Bone_002"});
	CHECK(cloth->simulations[1].name == "Doctor_Badge");
	CHECK(cloth->simulations[1].writtenBones == std::vector<std::string>{"Cloth_Bone_Badge"});
}

TEST_CASE("Cloth payloads that can't be resolved completely are unreadable", "[cloth][hkx]") {
	SECTION("Data that is not a tagfile") {
		const std::string legacy = std::string("coat physics\0Root\0Cloth_Bone_001\0", 33);
		CHECK_FALSE(Read(legacy));
		CHECK_FALSE(Read(""));
		CHECK_FALSE(HKX::ReadClothBones(nullptr, 16));
	}
	SECTION("A transform set that doesn't cover the skeleton") {
		ClothPayload::Options options;
		options.transformSetSizeDelta = -1;
		CHECK_FALSE(Read(ClothPayload::Tagfile(rig, {{"Cloth", {"Cloth_Bone_001"}, {}}}, options)));
	}
	SECTION("Cloth that writes nothing") {
		CHECK_FALSE(Read(ClothPayload::Tagfile(rig, {{"Cloth", {}, {"C_Spine"}}})));
	}
	SECTION("A payload without cloth") {
		CHECK_FALSE(Read(ClothPayload::Tagfile(rig, {})));
	}
}

TEST_CASE("Damaged cloth payloads never read out of bounds", "[cloth][hkx]") {
	const auto payload = ClothPayload::Tagfile(rig, {{"Cloth", {"Cloth_Bone_001"}, {"C_Spine"}}});
	REQUIRE(Read(payload));

	for (size_t size = 0; size < payload.size(); ++size) {
		CAPTURE(size);
		const std::string truncated = payload.substr(0, size);
		CHECK_FALSE(Read(truncated));
	}

	// A corrupted byte may still leave a readable file, but it must not crash or yield bones outside the skeleton
	for (size_t i = 0; i < payload.size(); ++i) {
		for (char value : {'\x00', '\x7F', '\xFF'}) {
			std::string corrupted = payload;
			corrupted[i] = value;
			if (auto cloth = Read(corrupted)) {
				for (const auto& simulation : cloth->simulations) {
					for (const auto& bone : simulation.writtenBones)
						CHECK(std::find(cloth->skeleton.begin(), cloth->skeleton.end(), bone) != cloth->skeleton.end());
				}
			}
		}
	}
}

// Set BSOS_SF_CLOTH_DIR to a folder of Starfield meshes, for example extracted game archives or a mod folder
TEST_CASE("Every Starfield cloth payload moves bones of its shapes", "[.cloth-real-data]") {
	const char* directory = std::getenv("BSOS_SF_CLOTH_DIR");
	REQUIRE(directory);

	size_t files = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(directory))) {
		if (!entry.is_regular_file() || Lower(entry.path().extension().u8string()) != ".nif")
			continue;

		nifly::NifFile nif;
		if (nif.Load(entry.path()) != 0)
			continue;
		auto blocks = nif.GetChildren<nifly::BSClothExtraData>(nullptr, true);
		if (blocks.empty())
			continue;

		INFO(entry.path().u8string());
		++files;
		std::set<std::string> written;
		for (auto* block : blocks) {
			const auto cloth = HKX::ReadClothBones(block->data.data(), block->data.size());
			REQUIRE(cloth);
			for (const auto& simulation : cloth->simulations) {
				for (const auto& bone : simulation.writtenBones)
					written.insert(Lower(bone));
			}
		}

		size_t driven = 0;
		size_t skinned = 0;
		for (auto* shape : nif.GetShapes()) {
			std::vector<std::string> bones;
			nif.GetShapeBoneList(shape, bones);
			if (bones.empty())
				continue;
			++skinned;
			if (std::any_of(bones.begin(), bones.end(), [&](const std::string& bone) { return written.count(Lower(bone)) > 0; }))
				++driven;
		}

		// Facegen heads hold several shapes, of which only the hair has cloth
		if (Lower(entry.path().u8string()).find("facegendata") != std::string::npos)
			CHECK(driven == 1);
		else
			CHECK(driven == skinned);
	}
	CHECK(files > 0);
}
