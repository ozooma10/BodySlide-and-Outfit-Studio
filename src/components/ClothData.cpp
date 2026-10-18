/*
BodySlide and Outfit Studio
See the included LICENSE file
*/

#include "ClothData.h"
#include "../../lib/LZ4F/xxhash.h"
#include "../files/HkxCloth.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

using namespace nifly;

namespace {
std::string Fingerprint(const BSClothExtraData& block) {
	return std::to_string(block.data.size()) + ":" + std::to_string(XXH64(block.data.data(), block.data.size(), 0));
}

std::string Lower(const std::string& text) {
	std::string lower(text);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower;
}

// The Havok payload keeps its bone names as plain text, in the string table of a tagfile and inline in a packfile.
// Node and HKX spellings can differ in case, so names are compared lowercase.
std::unordered_set<std::string> Names(const BSClothExtraData& block) {
	std::unordered_set<std::string> names;
	std::string current;
	auto flush = [&]() {
		if (current.size() > 1)
			names.insert(Lower(current));
		current.clear();
	};

	const char* data = block.data.data();
	for (size_t i = 0; i < block.data.size(); ++i) {
		char c = data[i];
		if (c >= 0x20 && c <= 0x7e)
			current += c;
		else
			flush();
	}
	flush();
	return names;
}

struct ShapeBones {
	std::string name;
	std::vector<std::string> bones;
	std::vector<std::string> lower;
};

ShapeBones GetShapeBones(NifFile& nif, NiShape* shape) {
	ShapeBones entry;
	entry.name = shape->name.get();
	nif.GetShapeBoneList(shape, entry.bones);
	for (const auto& bone : entry.bones)
		entry.lower.push_back(Lower(bone));
	return entry;
}

// Starfield binds bones by name, so a bone outside the skeleton exists in game only if cloth creates and moves it.
// Earlier games keep bones as nodes in the NIF.
std::vector<std::string> Undriven(NifFile& nif, const ShapeBones& shape, const ClothData::SkeletonBones& isSkeletonBone, const std::set<std::string>& moved) {
	std::vector<std::string> undriven;
	if (!isSkeletonBone || !nif.GetHeader().GetVersion().IsSF())
		return undriven;

	for (size_t i = 0; i < shape.bones.size(); ++i) {
		if (!isSkeletonBone(shape.bones[i]) && !moved.count(shape.lower[i]))
			undriven.push_back(shape.bones[i]);
	}
	return undriven;
}

} // namespace

std::string ClothData::Add(const std::string& source, std::unique_ptr<BSClothExtraData> block) {
	// Reloading a file reuses the blocks it stored before. Blocks are never removed, as undo can still refer to them.
	std::string key = source;
	for (size_t suffix = 2;; ++suffix) {
		auto existing = blocks.find(key);
		if (existing == blocks.end())
			break;

		const auto& data = existing->second->data;
		if (data.size() == block->data.size() && std::equal(data.data(), data.data() + data.size(), block->data.data()))
			return key;

		key = source + " (" + std::to_string(suffix) + ")";
	}

	std::optional<std::set<std::string>> written;
	if (auto cloth = HKX::ReadClothBones(block->data.data(), block->data.size())) {
		written.emplace();
		for (const auto& simulation : cloth->simulations) {
			for (const auto& bone : simulation.writtenBones)
				written->insert(Lower(bone));
		}
	}

	writtenBones[key] = std::move(written);
	sourceFiles[key] = source;
	blocks.emplace(key, std::move(block));
	return key;
}

bool ClothData::Readable(const std::string& source) const {
	return Written(source) != nullptr;
}

const std::set<std::string>* ClothData::Written(const std::string& source) const {
	auto it = writtenBones.find(source);
	return it != writtenBones.end() && it->second ? &*it->second : nullptr;
}

ClothData::Sources ClothData::GetShapeSources(const std::string& shape) const {
	auto it = shapeSources.find(shape);
	return it != shapeSources.end() ? it->second : Sources{};
}

void ClothData::SetShapeSources(const std::string& shape, const Sources& sources) {
	if (sources.empty())
		shapeSources.erase(shape);
	else
		shapeSources[shape] = sources;
}

void ClothData::RenameShape(const std::string& before, const std::string& after) {
	if (before == after)
		return;

	auto sources = GetShapeSources(before);
	RemoveShape(before);
	SetShapeSources(after, sources);
}

void ClothData::Capture(NifFile& nif, const std::string& source, const std::vector<NiShape*>& shapes, const SkeletonBones& isSkeletonBone) {
	struct Block {
		std::string key;
		std::unordered_set<std::string> names;
	};

	std::vector<Block> captured;
	for (auto& block : nif.GetChildren<BSClothExtraData>(nullptr, true)) {
		auto key = Add(source, block->Clone());
		if (std::none_of(captured.begin(), captured.end(), [&](const Block& other) { return other.key == key; }))
			captured.push_back({key, Readable(key) ? std::unordered_set<std::string>{} : Names(*block)});
	}

	// Unreadable payloads are claimed by bone names. Only the bones a shape adds to the reference skeleton tell their
	// cloth apart, as the payload names the whole skeleton. Every shape of the file is resolved, so cloth owned by a
	// shape outside the capture is not handed to another.
	std::unordered_map<NiShape*, std::set<std::string>> owned;
	std::set<std::string> claimed;
	for (auto& shape : nif.GetShapes()) {
		std::vector<std::string> bones;
		nif.GetShapeBoneList(shape, bones);

		std::vector<std::string> added;
		for (auto& bone : bones) {
			if (!isSkeletonBone || !isSkeletonBone(bone))
				added.push_back(Lower(bone));
		}

		for (auto& block : captured) {
			if (Readable(block.key))
				continue;

			bool names = std::any_of(added.begin(), added.end(), [&](const std::string& bone) { return block.names.count(bone) > 0; });
			if (names) {
				owned[shape].insert(block.key);
				claimed.insert(block.key);
			}
		}
	}

	for (auto& shape : shapes) {
		if (!shape)
			continue;

		Sources sources;
		for (auto& block : captured) {
			if (Readable(block.key) || owned[shape].count(block.key) || !claimed.count(block.key))
				sources.push_back(block.key);
		}

		SetShapeSources(shape->name.get(), sources);
	}

	nif.GetHeader().DeleteBlockByType("BSClothExtraData");
}

void ClothData::AddManual(const std::string& source, std::unique_ptr<BSClothExtraData> block) {
	auto key = Add(source, std::move(block));
	if (std::find(manualSources.begin(), manualSources.end(), key) == manualSources.end())
		manualSources.push_back(key);
}

std::vector<std::string> ClothData::UndrivenBones(NifFile& nif, NiShape* shape, const SkeletonBones& isSkeletonBone) const {
	if (!shape)
		return {};

	std::set<std::string> moved;
	for (const auto& source : GetShapeSources(shape->name.get())) {
		const auto* written = Written(source);
		if (!written)
			return {};
		moved.insert(written->begin(), written->end());
	}
	return Undriven(nif, GetShapeBones(nif, shape), isSkeletonBone, moved);
}

ClothData::ExportPlan ClothData::Plan(NifFile& nif, const SkeletonBones& isSkeletonBone, const std::string& referenceShape) const {
	ExportPlan plan;
	std::vector<ShapeBones> shapes;
	for (auto& shape : nif.GetShapes())
		shapes.push_back(GetShapeBones(nif, shape));

	std::set<std::string> referencePrints;
	if (!referenceShape.empty()) {
		for (const auto& source : GetShapeSources(referenceShape)) {
			auto block = blocks.find(source);
			if (block != blocks.end())
				referencePrints.insert(Fingerprint(*block->second));
		}
	}

	auto fileOf = [&](const std::string& source) {
		auto file = sourceFiles.find(source);
		return file != sourceFiles.end() ? file->second : std::string();
	};

	// Several shapes can share cloth and different imports can hold the same payload, so each payload is kept once
	std::set<std::string> keptPrints;
	std::set<std::string> moved;
	auto keep = [&](const std::string& source, Sources& into) {
		if (!keptPrints.insert(Fingerprint(*blocks.at(source))).second)
			return;
		into.push_back(source);
		if (const auto* written = Written(source))
			moved.insert(written->begin(), written->end());
	};

	for (const auto& shape : shapes) {
		const auto candidates = GetShapeSources(shape.name);
		// Capture lists the cloth of the file a shape was loaded from first. Merges and copies append theirs.
		const auto origin = candidates.empty() ? std::string() : fileOf(candidates.front());
		for (const auto& source : candidates) {
			auto block = blocks.find(source);
			if (block == blocks.end())
				continue;

			const auto* written = Written(source);
			if (!written) {
				keep(source, plan.sources);
				continue;
			}

			bool drives = false;
			bool required = false;
			for (size_t i = 0; i < shape.bones.size(); ++i) {
				if (written->count(shape.lower[i])) {
					drives = true;
					required = required || !isSkeletonBone || !isSkeletonBone(shape.bones[i]);
				}
			}
			if (!drives)
				continue;

			// Physics of reference skeleton bones that the shape's own file didn't carry stays with the reference
			const auto print = Fingerprint(*block->second);
			if (!required && shape.name != referenceShape && referencePrints.count(print)) {
				bool own = std::any_of(candidates.begin(), candidates.end(), [&](const std::string& other) {
					auto otherBlock = blocks.find(other);
					return otherBlock != blocks.end() && fileOf(other) == origin && Fingerprint(*otherBlock->second) == print;
				});
				if (!own)
					continue;
			}

			keep(source, plan.sources);
		}
	}

	// A bone outside the skeleton exists only through cloth that writes it, so the cloth of another shape is needed too
	// when a shape got such a bone by an edit like a weight copy. Bones the shape's own cloth writes stay with it, as
	// different outfits can reuse bone names.
	std::set<std::string> pool;
	for (const auto& entry : shapeSources)
		pool.insert(entry.second.begin(), entry.second.end());

	for (const auto& shape : shapes) {
		std::set<std::string> own;
		for (const auto& source : GetShapeSources(shape.name)) {
			if (const auto* written = Written(source))
				own.insert(written->begin(), written->end());
		}

		for (size_t i = 0; i < shape.bones.size(); ++i) {
			if ((isSkeletonBone && isSkeletonBone(shape.bones[i])) || own.count(shape.lower[i]))
				continue;

			for (const auto& entry : blocks) {
				const auto* written = Written(entry.first);
				if (pool.count(entry.first) && written && written->count(shape.lower[i]))
					keep(entry.first, plan.sources);
			}
		}
	}

	for (const auto& source : manualSources) {
		const auto* written = Written(source);
		if (!written) {
			plan.unknownImports.push_back(source);
			continue;
		}

		std::set<std::string> used;
		for (const auto& shape : shapes) {
			for (const auto& bone : shape.lower) {
				if (written->count(bone))
					used.insert(bone);
			}
		}
		if (used.empty() || keptPrints.count(Fingerprint(*blocks.at(source))))
			continue;

		Sources replaced;
		for (const auto& original : plan.sources) {
			const auto* originalWritten = Written(original);
			if (originalWritten && std::any_of(used.begin(), used.end(), [&](const std::string& bone) { return originalWritten->count(bone) > 0; }))
				replaced.push_back(original);
		}

		if (!replaced.empty()) {
			plan.replacements.emplace_back(source, std::move(replaced));
			moved.insert(written->begin(), written->end());
		}
		else
			keep(source, plan.imports);
	}

	for (const auto& shape : shapes) {
		const auto candidates = GetShapeSources(shape.name);
		if (std::any_of(candidates.begin(), candidates.end(), [&](const std::string& source) { return blocks.count(source) && !Readable(source); }))
			continue;

		auto undriven = Undriven(nif, shape, isSkeletonBone, moved);
		if (!undriven.empty())
			plan.undrivenBones[shape.name] = std::move(undriven);
	}
	return plan;
}

ClothData::Sources ClothData::ExportPlan::Resolve(const ReplaceChoice& replace) const {
	Sources resolved = sources;
	resolved.insert(resolved.end(), imports.begin(), imports.end());

	// Declining an import keeps the original cloth, but must not bring back cloth another import already replaced
	for (const auto& [import, originals] : replacements) {
		if (!replace(import, originals))
			continue;

		for (const auto& original : originals)
			resolved.erase(std::remove(resolved.begin(), resolved.end(), original), resolved.end());
		resolved.push_back(import);
	}
	return resolved;
}

void ClothData::Write(NifFile& nif, const Sources& sources) const {
	auto root = nif.GetRootNode();
	if (!root)
		return;

	nif.GetHeader().DeleteBlockByType("BSClothExtraData");

	std::set<std::string> written;
	for (auto& source : sources) {
		auto block = blocks.find(source);
		if (block != blocks.end() && written.insert(Fingerprint(*block->second)).second)
			root->extraDataRefs.AddBlockRef(nif.GetHeader().AddBlock(block->second->Clone()));
	}
}

void ClothData::Apply(NifFile& nif, const SkeletonBones& isSkeletonBone, const std::string& referenceShape) const {
	Write(nif, Plan(nif, isSkeletonBone, referenceShape).sources);
}
