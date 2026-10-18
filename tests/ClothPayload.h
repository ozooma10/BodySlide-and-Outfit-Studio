// Builds Starfield-style cloth payloads: Havok 2016+ tagfiles with a skeleton and cloth simulations whose operators
// record the bones they read and write, laid out the way the game files describe them.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ClothPayload {

struct Simulation {
	std::string name;
	// Bones the simulation moves
	std::vector<std::string> written;
	// Bones it only reads, like the skin input and collision of a real payload
	std::vector<std::string> read;
};

struct Options {
	// Makes the transform set cover a different number of bones than the skeleton
	int transformSetSizeDelta = 0;
};

namespace detail {

enum : uint32_t {
	Char = 1,
	StringPtr,
	Uint32,
	Int,
	Bool,
	Bone,
	BoneArray,
	Skeleton,
	SetDefinition,
	SetDefinitionPtr,
	SetDefinitionPtrArray,
	Uint32Array,
	BitFieldStorage,
	BitFieldBase,
	BitField,
	Tracker,
	TrackerArray,
	SetUsage,
	SetAccess,
	SetAccessArray,
	Operator,
	BoneDeformOperator,
	SkinOperator,
	OperatorPtr,
	OperatorPtrArray,
	ClothData,
	TypeCount
};

struct MemberSpec {
	std::string name;
	uint32_t offset;
	uint32_t type;
};

struct TypeSpec {
	std::string name;
	uint32_t parent = 0;
	uint32_t kind = 0;
	uint32_t pointer = 0;
	uint32_t size = 0;
	std::vector<MemberSpec> members;
};

inline std::vector<TypeSpec> Types() {
	std::vector<TypeSpec> types(TypeCount);
	auto record = [&](uint32_t k, const std::string& name, uint32_t size, std::vector<MemberSpec> members, uint32_t parent = 0) {
		types[k] = {name, parent, 7, 0, size, std::move(members)};
	};
	auto array = [&](uint32_t k, uint32_t element) { types[k] = {"hkArray", 0, 8, element, 16, {}}; };
	auto pointer = [&](uint32_t k, uint32_t target) { types[k] = {"T*", 0, 6, target, 8, {}}; };

	types[Char] = {"char", 0, 4, 0, 1, {}};
	types[StringPtr] = {"hkStringPtr", 0, 3, 0, 8, {}};
	types[Uint32] = {"hkUint32", 0, 4, 0, 4, {}};
	types[Int] = {"int", 0, 4, 0, 4, {}};
	types[Bool] = {"hkBool", 0, 2, 0, 1, {}};
	record(Bone, "hkaBone", 16, {{"name", 0, StringPtr}, {"lockTranslation", 8, Bool}});
	array(BoneArray, Bone);
	record(Skeleton, "hkaSkeleton", 24, {{"name", 0, StringPtr}, {"bones", 8, BoneArray}});
	record(SetDefinition, "hclTransformSetDefinition", 16, {{"name", 0, StringPtr}, {"numTransforms", 8, Uint32}});
	pointer(SetDefinitionPtr, SetDefinition);
	array(SetDefinitionPtrArray, SetDefinitionPtr);
	array(Uint32Array, Uint32);
	record(BitFieldStorage, "hkBitFieldStorage", 24, {{"words", 0, Uint32Array}, {"numBits", 16, Int}});
	record(BitFieldBase, "hkBitFieldBase", 24, {{"storage", 0, BitFieldStorage}});
	record(BitField, "hkBitField", 24, {}, BitFieldBase);
	record(Tracker, "hclTransformSetUsage::TransformTracker", 72, {{"read", 0, BitField}, {"readBeforeWrite", 24, BitField}, {"written", 48, BitField}});
	array(TrackerArray, Tracker);
	record(SetUsage, "hclTransformSetUsage", 24, {{"perComponentTransformTrackers", 8, TrackerArray}});
	record(SetAccess, "hclClothState::TransformSetAccess", 32, {{"transformSetIndex", 0, Uint32}, {"transformSetUsage", 8, SetUsage}});
	array(SetAccessArray, SetAccess);
	record(Operator, "hclOperator", 24, {{"name", 0, StringPtr}, {"usedTransformSets", 8, SetAccessArray}});
	record(BoneDeformOperator, "hclSimpleMeshBoneDeformOperator", 24, {}, Operator);
	record(SkinOperator, "hclObjectSpaceSkinPNOperator", 24, {}, Operator);
	pointer(OperatorPtr, Operator);
	array(OperatorPtrArray, OperatorPtr);
	record(ClothData, "hclClothData", 40, {{"name", 0, StringPtr}, {"transformSetDefinitions", 8, SetDefinitionPtrArray}, {"operators", 24, OperatorPtrArray}});
	return types;
}

inline void Varint(std::string& out, uint32_t value) {
	if (value < 0x80)
		out += char(value);
	else if (value < 0x4000) {
		out += char(0x80 | (value >> 8));
		out += char(value & 0xFF);
	}
	else if (value < 0x200000) {
		out += char(0xC0 | (value >> 16));
		out += char((value >> 8) & 0xFF);
		out += char(value & 0xFF);
	}
	else {
		out += char(0xE0 | (value >> 24));
		out += char((value >> 16) & 0xFF);
		out += char((value >> 8) & 0xFF);
		out += char(value & 0xFF);
	}
}

inline void Put32(std::string& out, size_t offset, uint32_t value) {
	if (out.size() < offset + 4)
		out.resize(offset + 4);
	for (int i = 0; i < 4; ++i)
		out[offset + i] = char((value >> (8 * i)) & 0xFF);
}

inline std::string Section(const char* tag, const std::string& body, bool leaf = true) {
	const uint32_t size = static_cast<uint32_t>(body.size() + 8) | (leaf ? 0x40000000u : 0u);
	std::string out;
	for (int i = 3; i >= 0; --i)
		out += char((size >> (8 * i)) & 0xFF);
	out += std::string(tag, 4);
	return out + body;
}

class Builder {
	struct Item {
		uint32_t type;
		uint32_t offset;
		uint32_t count;
	};

	std::string data;
	std::vector<Item> items{{0, 0, 0}};

public:
	uint32_t Add(uint32_t type, uint32_t count, const std::string& bytes) {
		while (data.size() % 16)
			data += '\0';
		items.push_back({type, static_cast<uint32_t>(data.size()), count});
		data += bytes;
		return static_cast<uint32_t>(items.size() - 1);
	}

	uint32_t String(const std::string& text) { return Add(Char, static_cast<uint32_t>(text.size() + 1), text + '\0'); }

	// An hkBitField value over numBits bits
	std::string Bits(const std::vector<uint32_t>& set, uint32_t numBits) {
		std::string words(((numBits + 31) / 32) * 4, '\0');
		for (uint32_t bit : set) {
			auto byte = static_cast<size_t>(bit / 8);
			words[byte] = char(static_cast<unsigned char>(words[byte]) | (1u << (bit % 8)));
		}
		std::string value(24, '\0');
		Put32(value, 0, Add(Uint32, numBits ? (numBits + 31) / 32 : 0, words));
		Put32(value, 16, numBits);
		return value;
	}

	std::string Finish() const {
		const auto types = Types();
		std::vector<std::string> typeNames{"tT"};
		std::vector<std::string> fieldNames;
		auto indexOf = [](std::vector<std::string>& names, const std::string& name) {
			auto it = std::find(names.begin(), names.end(), name);
			if (it != names.end())
				return static_cast<uint32_t>(it - names.begin());
			names.push_back(name);
			return static_cast<uint32_t>(names.size() - 1);
		};

		std::string table;
		Varint(table, TypeCount);
		for (uint32_t k = 1; k < TypeCount; ++k) {
			Varint(table, indexOf(typeNames, types[k].name));
			Varint(table, types[k].pointer ? 1 : 0);
			if (types[k].pointer) {
				Varint(table, 0);
				Varint(table, types[k].pointer);
			}
		}

		std::string bodies;
		for (uint32_t k = 1; k < TypeCount; ++k) {
			const auto& type = types[k];
			uint32_t flags = 0x1 | 0x8;
			if (type.pointer)
				flags |= 0x2;
			if (!type.members.empty())
				flags |= 0x20;
			Varint(bodies, k);
			Varint(bodies, type.parent);
			Varint(bodies, flags);
			Varint(bodies, type.kind);
			if (type.pointer)
				Varint(bodies, type.pointer);
			Varint(bodies, type.size);
			Varint(bodies, std::min<uint32_t>(type.size, 8));
			if (!type.members.empty()) {
				Varint(bodies, static_cast<uint32_t>(type.members.size()));
				for (const auto& member : type.members) {
					Varint(bodies, indexOf(fieldNames, member.name));
					Varint(bodies, 0x20);
					Varint(bodies, member.offset);
					Varint(bodies, member.type);
				}
			}
		}

		std::string names;
		for (const auto& name : typeNames)
			names += name + '\0';
		std::string fields;
		for (const auto& name : fieldNames)
			fields += name + '\0';

		std::string index;
		for (const auto& item : items) {
			std::string entry;
			Put32(entry, 0, item.type ? (item.type | 0x10000000u) : 0);
			Put32(entry, 4, item.offset);
			Put32(entry, 8, item.count);
			index += entry;
		}

		std::string typeSection = Section("TST1", names) + Section("TNA1", table) + Section("FST1", fields) + Section("TBDY", bodies);
		std::string indexSection = Section("ITEM", index) + Section("PTCH", "");
		return Section("TAG0", Section("SDKV", "20190200") + Section("DATA", data) + Section("TYPE", typeSection, false) + Section("INDX", indexSection, false), false);
	}
};

inline std::vector<uint32_t> Indices(const std::vector<std::string>& skeleton, const std::vector<std::string>& bones) {
	std::vector<uint32_t> indices;
	for (const auto& bone : bones) {
		auto it = std::find(skeleton.begin(), skeleton.end(), bone);
		if (it != skeleton.end())
			indices.push_back(static_cast<uint32_t>(it - skeleton.begin()));
	}
	return indices;
}

} // namespace detail

// A payload whose skeleton is the given rig plus every bone a simulation reads or writes
inline std::string Tagfile(std::vector<std::string> skeleton, const std::vector<Simulation>& simulations, const Options& options = {}) {
	using namespace detail;
	for (const auto& simulation : simulations) {
		for (const auto* bones : {&simulation.read, &simulation.written}) {
			for (const auto& bone : *bones) {
				if (std::find(skeleton.begin(), skeleton.end(), bone) == skeleton.end())
					skeleton.push_back(bone);
			}
		}
	}

	Builder builder;
	const auto numBits = static_cast<uint32_t>(skeleton.size());

	std::string bones(skeleton.size() * 16, '\0');
	for (size_t i = 0; i < skeleton.size(); ++i)
		Put32(bones, i * 16, builder.String(skeleton[i]));
	const uint32_t boneArray = builder.Add(Bone, numBits, bones);

	std::string rig(24, '\0');
	Put32(rig, 0, builder.String(skeleton.empty() ? std::string() : skeleton.front()));
	Put32(rig, 8, boneArray);
	builder.Add(Skeleton, 1, rig);

	auto addOperator = [&](uint32_t type, const std::string& name, const std::vector<uint32_t>& read, const std::vector<uint32_t>& written) {
		std::string tracker(72, '\0');
		tracker.replace(0, 24, builder.Bits(read, numBits));
		tracker.replace(24, 24, builder.Bits({}, numBits));
		tracker.replace(48, 24, builder.Bits(written, numBits));
		const uint32_t trackers = builder.Add(Tracker, 1, tracker);

		std::string access(32, '\0');
		Put32(access, 16, trackers);
		const uint32_t accesses = builder.Add(SetAccess, 1, access);

		std::string op(24, '\0');
		Put32(op, 0, builder.String(name));
		Put32(op, 8, accesses);
		return builder.Add(type, 1, op);
	};

	for (const auto& simulation : simulations) {
		std::string set(16, '\0');
		Put32(set, 0, builder.String("Root Transforms"));
		Put32(set, 8, static_cast<uint32_t>(static_cast<int>(numBits) + options.transformSetSizeDelta));
		const uint32_t setObject = builder.Add(SetDefinition, 1, set);
		std::string setPointers(8, '\0');
		Put32(setPointers, 0, setObject);
		const uint32_t sets = builder.Add(SetDefinitionPtr, 1, setPointers);

		const auto read = Indices(skeleton, simulation.read);
		const auto written = Indices(skeleton, simulation.written);
		std::string operators(16, '\0');
		Put32(operators, 0, addOperator(SkinOperator, "Skin Simulation", read, {}));
		Put32(operators, 8, addOperator(BoneDeformOperator, "Deform", written, written));
		const uint32_t operatorArray = builder.Add(OperatorPtr, 2, operators);

		std::string cloth(40, '\0');
		Put32(cloth, 0, builder.String(simulation.name));
		Put32(cloth, 8, sets);
		Put32(cloth, 24, operatorArray);
		builder.Add(ClothData, 1, cloth);
	}
	return builder.Finish();
}

} // namespace ClothPayload
