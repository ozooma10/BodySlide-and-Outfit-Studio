/*
BodySlide and Outfit Studio
See the included LICENSE file
*/

#include "files/SFMorphFile.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>

using nifly::Vector3;

namespace {

constexpr float HalfTolerance = 0.05f; // position deltas round-trip through fp16 at 1/69.969 scale

struct TempMorphFile {
	std::string path;

	TempMorphFile() {
		auto dir = std::filesystem::temp_directory_path();
		path = (dir / "bsos_sfmorph_test.dat").string();
	}

	~TempMorphFile() { std::remove(path.c_str()); }
};

std::unordered_map<uint16_t, Vector3> GetMorphOffsets(SFMorphFile& file, const std::string& morphName) {
	auto it = file.morphNamesCacheMap.find(morphName);
	REQUIRE(it != file.morphNamesCacheMap.end());
	REQUIRE(it->second < file.morphOffsetsCache.size());
	return file.morphOffsetsCache[it->second];
}

void RequireOffsetsMatch(const std::unordered_map<uint16_t, Vector3>& actual, const std::unordered_map<uint16_t, Vector3>& expected) {
	// Exact same vertex set: a mismatch here means morph data was attributed to the wrong morph.
	REQUIRE(actual.size() == expected.size());

	for (const auto& [index, expectedDelta] : expected) {
		auto it = actual.find(index);
		REQUIRE(it != actual.end());

		REQUIRE(std::fabs(it->second.x - expectedDelta.x) < HalfTolerance);
		REQUIRE(std::fabs(it->second.y - expectedDelta.y) < HalfTolerance);
		REQUIRE(std::fabs(it->second.z - expectedDelta.z) < HalfTolerance);
	}
}

} // namespace

TEST_CASE("Starfield morph file round-trips overlapping morphs", "[SFMorphFile]") {
	// Three morphs with overlapping vertex sets. Overlap is the regression target:
	// the key marker bits must identify each morph by its name table index, so a
	// vertex affected by multiple morphs must get each morph's own deltas back.
	std::unordered_map<uint16_t, Vector3> morphA{
		{0, Vector3(1.0f, 0.0f, 0.0f)},
		{1, Vector3(0.0f, 2.0f, 0.0f)},
		{2, Vector3(0.0f, 0.0f, 3.0f)},
		{3, Vector3(1.0f, 1.0f, 1.0f)},
	};
	std::unordered_map<uint16_t, Vector3> morphB{
		{2, Vector3(-1.0f, 0.0f, 0.0f)},
		{3, Vector3(0.0f, -2.0f, 0.0f)},
		{4, Vector3(0.0f, 0.0f, -3.0f)},
		{5, Vector3(-1.0f, -1.0f, -1.0f)},
	};
	std::unordered_map<uint16_t, Vector3> morphC{
		{3, Vector3(4.0f, 0.0f, 0.0f)},
		{5, Vector3(0.0f, 5.0f, 0.0f)},
		{6, Vector3(0.0f, 0.0f, 6.0f)},
	};

	SFMorphFile outFile;
	outFile.SetVertexCount(8);
	REQUIRE(outFile.AddMorph("MorphA", morphA, {}, {}, {}));
	REQUIRE(outFile.AddMorph("MorphB", morphB, {}, {}, {}));
	REQUIRE(outFile.AddMorph("MorphC", morphC, {}, {}, {}));

	outFile.CacheToFileData();

	TempMorphFile temp;
	REQUIRE(outFile.Write(temp.path));

	// Same sequence the Outfit Studio import path uses
	SFMorphFile inFile;
	REQUIRE(inFile.Read(temp.path));
	REQUIRE(inFile.FileToCacheData());
	inFile.UpdateCachedMorphData();

	auto names = inFile.GetMorphNames();
	REQUIRE(names.size() == 3);
	REQUIRE(names[0] == "MorphA");
	REQUIRE(names[1] == "MorphB");
	REQUIRE(names[2] == "MorphC");

	RequireOffsetsMatch(GetMorphOffsets(inFile, "MorphA"), morphA);
	RequireOffsetsMatch(GetMorphOffsets(inFile, "MorphB"), morphB);
	RequireOffsetsMatch(GetMorphOffsets(inFile, "MorphC"), morphC);
}

TEST_CASE("Starfield morph file round-trips a single morph", "[SFMorphFile]") {
	std::unordered_map<uint16_t, Vector3> morph{
		{0, Vector3(0.5f, 0.0f, 0.0f)},
		{7, Vector3(0.0f, 0.0f, -0.5f)},
	};

	SFMorphFile outFile;
	outFile.SetVertexCount(8);
	REQUIRE(outFile.AddMorph("Morph", morph, {}, {}, {}));

	outFile.CacheToFileData();

	TempMorphFile temp;
	REQUIRE(outFile.Write(temp.path));

	SFMorphFile inFile;
	REQUIRE(inFile.Read(temp.path));
	REQUIRE(inFile.FileToCacheData());
	inFile.UpdateCachedMorphData();

	REQUIRE(inFile.GetMorphCount() == 1);
	RequireOffsetsMatch(GetMorphOffsets(inFile, "Morph"), morph);
}
