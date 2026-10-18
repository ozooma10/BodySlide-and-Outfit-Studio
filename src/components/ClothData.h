/*
BodySlide and Outfit Studio
See the included LICENSE file
*/

#pragma once

#include "NifFile.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Keeps the BSClothExtraData blocks of every loaded NIF with the shapes that came from it, and decides on export which
// of them the exported shapes need.
//
// Cloth reaches a shape only through bones: the simulation writes bones and the shape is skinned to them. Starfield
// payloads record which bones they write, so a payload goes with every exported shape skinned to one of those bones.
// Payloads whose bones can't be read, like Fallout 4 packfiles, fall back to matching bone names found in the payload.
// Nothing is asked and no ownership metadata is written, so interactive and unattended exports give the same result.
class ClothData {
public:
	using Sources = std::vector<std::string>;
	using Blocks = std::map<std::string, std::unique_ptr<nifly::BSClothExtraData>>;
	using ShapeSources = std::unordered_map<std::string, Sources>;

	// Tells whether a bone belongs to the reference skeleton rather than to a loaded outfit
	using SkeletonBones = std::function<bool(const std::string&)>;

	struct ExportPlan {
		// Original cloth that moves bones of the exported shapes
		Sources sources;
		// Standalone HKX imports that move bones of the exported shapes and no original cloth moves
		Sources imports;
		// An import and an original that move the same bones of the exported shapes. Only one of them should be written.
		std::vector<std::pair<std::string, std::string>> replacements;
		// Imports whose bones can't be read
		Sources unknownImports;
		// Per exported shape, Starfield bones outside the reference skeleton that no written cloth moves. The game has
		// nothing to drive them, which usually means the shape's cloth data was lost before it was loaded.
		std::map<std::string, std::vector<std::string>> undrivenBones;
	};

private:
	Blocks blocks;
	// Lowercase bones each block writes, or nothing if its payload couldn't be read
	std::map<std::string, std::optional<std::set<std::string>>> writtenBones;
	// The file each block was loaded from
	std::map<std::string, std::string> sourceFiles;
	ShapeSources shapeSources;
	Sources manualSources;

	std::string Add(const std::string& source, std::unique_ptr<nifly::BSClothExtraData> block);
	bool Readable(const std::string& source) const;
	const std::set<std::string>* Written(const std::string& source) const;

public:
	const Blocks& GetBlocks() const { return blocks; }
	const Sources& GetManualSources() const { return manualSources; }

	// The cloth an exported shape may use. Export keeps only the candidates that move bones of the shape.
	Sources GetShapeSources(const std::string& shape) const;
	void SetShapeSources(const std::string& shape, const Sources& sources);
	void RemoveShape(const std::string& shape) { shapeSources.erase(shape); }
	void RenameShape(const std::string& before, const std::string& after);

	// The cloth of every shape, for project snapshots. Blocks are never removed, so restored sources stay valid.
	const ShapeSources& GetAllShapeSources() const { return shapeSources; }
	void SetAllShapeSources(const ShapeSources& sources) { shapeSources = sources; }

	// Takes the cloth blocks out of the NIF and makes them candidates of the given shapes.
	// Readable payloads become candidates of every given shape, since export checks the bones they write.
	// For unreadable payloads, a block belongs to the shapes of the file skinned to a bone outside the reference skeleton
	// that the payload names, and a block that names no such bone stays with every given shape. The payload names the
	// whole rig, including bones it only collides with, so this fallback can over-claim but never drops cloth.
	void Capture(nifly::NifFile& nif,
				 const std::string& source,
				 const std::vector<nifly::NiShape*>& shapes,
				 const SkeletonBones& isSkeletonBone = {});

	// Standalone HKX import. Export adds it where it moves bones of the exported shapes.
	void AddManual(const std::string& source, std::unique_ptr<nifly::BSClothExtraData> block);

	// Bones of a Starfield shape outside the reference skeleton that none of its candidate cloth moves.
	// Empty when a candidate's bones can't be read, since nothing can be told then.
	std::vector<std::string> UndrivenBones(nifly::NifFile& nif, nifly::NiShape* shape, const SkeletonBones& isSkeletonBone = {}) const;

	// Decides which cloth the shapes left in the NIF need.
	// A candidate is written when the shape is skinned to a bone it writes. A bone outside the reference skeleton that no
	// candidate writes, for example one added by a weight copy, takes the cloth of other project shapes that writes it. When all those bones belong to the reference
	// skeleton, the cloth only adds motion to bones the actor already has, like physics of a body under an outfit. Such
	// cloth is left to the reference when the reference has the same payload and the file the shape was loaded from
	// didn't, for example after merging reference geometry. A file holding both, like an export with the reference,
	// can't tell whose that cloth was, so a shape loaded from it keeps it.
	ExportPlan Plan(nifly::NifFile& nif, const SkeletonBones& isSkeletonBone = {}, const std::string& referenceShape = {}) const;

	// Writes the given cloth to the NIF root, once per distinct payload, replacing any cloth it had
	void Write(nifly::NifFile& nif, const Sources& sources) const;

	// Writes the original cloth the plan keeps for the shapes left in the NIF
	void Apply(nifly::NifFile& nif, const SkeletonBones& isSkeletonBone = {}, const std::string& referenceShape = {}) const;
};
