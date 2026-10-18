/*
BodySlide and Outfit Studio
See the included LICENSE file

Minimal reader for Havok 2016+ binary tagfiles, as embedded in Starfield BSClothExtraData. See HkxCloth.h.

A tagfile is a tree of sections, each with a big-endian size and a four-character tag. TAG0 holds the DATA blob, the
type descriptions (TYPE: TST1 type names, TNA1 type table, FST1 field names, TBDY type bodies) and the item index
(INDX: ITEM). Every object, array and string in DATA is an item; fields that point at one hold its item index.
Members are found by name through the type descriptions, so nothing depends on a particular SDK layout.
*/

#include "HkxCloth.h"

#include <cstdint>
#include <map>
#include <set>

namespace HKX {

namespace {

// Kinds in the low bits of a type's sub-type flags
constexpr uint32_t KindString = 3;
constexpr uint32_t KindArray = 8;

// Type body flags
constexpr uint32_t TypeSubType = 0x1;
constexpr uint32_t TypePointer = 0x2;
constexpr uint32_t TypeVersion = 0x4;
constexpr uint32_t TypeByteSize = 0x8;
constexpr uint32_t TypeAbstractValue = 0x10;
constexpr uint32_t TypeMembers = 0x20;
constexpr uint32_t TypeInterfaces = 0x40;
constexpr uint32_t TypeAttribute = 0x80;

// Member flag of a member that carries one more value, as hkPackedVector3::values does in Starfield cloth
constexpr uint32_t MemberExtraValue = 0x80;

// Inheritance and typedef chains are short. Anything longer is a broken or hostile file.
constexpr int MaxChain = 32;

struct Member {
	std::string name;
	uint32_t offset = 0;
	uint32_t type = 0;
};

struct Type {
	std::string name;
	uint32_t parent = 0;
	uint32_t flags = 0;
	uint32_t subType = 0;
	uint32_t size = 0;
	std::vector<Member> members;
};

struct Item {
	uint32_t type = 0;
	uint32_t offset = 0;
	uint32_t count = 0;
};

struct Section {
	size_t begin = 0;
	size_t end = 0;
};

uint32_t ReadU32LE(const uint8_t* d) {
	return uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
}

uint32_t ReadU32BE(const uint8_t* d) {
	return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | uint32_t(d[3]);
}

// Variable-length integers of the type sections. The leading bits of the first byte give the length.
class VarintReader {
	const uint8_t* data;
	size_t size;
	size_t pos = 0;
	bool ok = true;

public:
	VarintReader(const uint8_t* d, size_t s)
		: data(d)
		, size(s) {}

	bool AtEnd() const { return !ok || pos >= size; }
	bool Ok() const { return ok; }

	uint32_t Next() {
		if (pos >= size) {
			ok = false;
			return 0;
		}

		const uint8_t first = data[pos];
		size_t length = 0;
		uint32_t value = 0;
		if ((first & 0x80) == 0) {
			length = 1;
			value = first;
		}
		else if ((first & 0xC0) == 0x80) {
			length = 2;
			value = first & 0x3F;
		}
		else if ((first & 0xE0) == 0xC0) {
			length = 3;
			value = first & 0x1F;
		}
		else if ((first & 0xF8) == 0xE0) {
			length = 4;
			value = first & 0x07;
		}
		else if (first == 0xE8) {
			length = 5;
		}
		else {
			ok = false;
			return 0;
		}

		if (size - pos < length) {
			ok = false;
			return 0;
		}

		for (size_t i = 1; i < length; ++i)
			value = (value << 8) | data[pos + i];
		pos += length;
		return value;
	}
};

std::vector<std::string> SplitStrings(const uint8_t* data, size_t size) {
	std::vector<std::string> strings;
	std::string current;
	for (size_t i = 0; i < size; ++i) {
		if (data[i] == 0) {
			strings.push_back(current);
			current.clear();
		}
		else
			current += static_cast<char>(data[i]);
	}
	if (!current.empty())
		strings.push_back(current);
	return strings;
}

class Tagfile {
	const uint8_t* file = nullptr;
	Section dataSection;
	std::vector<Type> types;
	std::vector<Item> items;

	bool Walk(size_t begin, size_t end, std::map<std::string, Section>& leaves) {
		size_t pos = begin;
		while (pos < end) {
			if (end - pos < 8)
				return false;

			const uint32_t size = ReadU32BE(file + pos) & 0x3FFFFFFF;
			if (size < 8 || size > end - pos)
				return false;

			const std::string tag(reinterpret_cast<const char*>(file + pos + 4), 4);
			if (tag == "TYPE" || tag == "INDX") {
				if (!Walk(pos + 8, pos + size, leaves))
					return false;
			}
			else
				leaves.emplace(tag, Section{pos + 8, pos + size});
			pos += size;
		}
		return true;
	}

	bool ReadTypes(const Section& names, const Section& table, const Section& fields, const Section& bodies) {
		const auto typeNames = SplitStrings(file + names.begin, names.end - names.begin);
		const auto fieldNames = SplitStrings(file + fields.begin, fields.end - fields.begin);

		VarintReader tableReader(file + table.begin, table.end - table.begin);
		const uint32_t count = tableReader.Next();
		if (!tableReader.Ok() || count == 0 || count > table.end - table.begin)
			return false;

		types.resize(count);
		for (uint32_t k = 1; k < count; ++k) {
			const uint32_t nameIndex = tableReader.Next();
			if (nameIndex >= typeNames.size())
				return false;
			types[k].name = typeNames[nameIndex];

			const uint32_t parameters = tableReader.Next();
			for (uint32_t p = 0; p < parameters && tableReader.Ok(); ++p) {
				tableReader.Next();
				tableReader.Next();
			}
			if (!tableReader.Ok())
				return false;
		}

		VarintReader bodyReader(file + bodies.begin, bodies.end - bodies.begin);
		while (!bodyReader.AtEnd()) {
			const uint32_t k = bodyReader.Next();
			if (k == 0)
				continue;
			if (k >= count)
				return false;

			Type& type = types[k];
			type.parent = bodyReader.Next();
			type.flags = bodyReader.Next();
			if (type.flags & TypeSubType)
				type.subType = bodyReader.Next();
			if ((type.flags & TypePointer) && (type.subType & 0xF) >= 6)
				bodyReader.Next();
			if (type.flags & TypeVersion)
				bodyReader.Next();
			if (type.flags & TypeByteSize) {
				type.size = bodyReader.Next();
				bodyReader.Next(); // alignment
			}
			if (type.flags & TypeAbstractValue)
				bodyReader.Next();
			if (type.flags & TypeMembers) {
				const uint32_t memberCount = bodyReader.Next();
				for (uint32_t m = 0; m < memberCount && bodyReader.Ok(); ++m) {
					Member member;
					const uint32_t nameIndex = bodyReader.Next();
					const uint32_t memberFlags = bodyReader.Next();
					if (memberFlags & MemberExtraValue)
						bodyReader.Next();
					member.offset = bodyReader.Next();
					member.type = bodyReader.Next();
					if (nameIndex >= fieldNames.size() || member.type >= count)
						return false;
					member.name = fieldNames[nameIndex];
					type.members.push_back(member);
				}
			}
			if (type.flags & TypeInterfaces) {
				const uint32_t interfaces = bodyReader.Next();
				for (uint32_t i = 0; i < interfaces && bodyReader.Ok(); ++i) {
					bodyReader.Next();
					bodyReader.Next();
				}
			}
			if (type.flags & TypeAttribute)
				bodyReader.Next();

			if (!bodyReader.Ok() || type.parent >= count)
				return false;
		}
		return bodyReader.Ok();
	}

public:
	bool Load(const uint8_t* data, size_t size) {
		file = data;
		if (size < 8 || std::string(reinterpret_cast<const char*>(data + 4), 4) != "TAG0")
			return false;

		const uint32_t rootSize = ReadU32BE(data) & 0x3FFFFFFF;
		if (rootSize < 8 || rootSize > size)
			return false;

		std::map<std::string, Section> leaves;
		if (!Walk(8, rootSize, leaves))
			return false;

		for (const char* required : {"DATA", "TST1", "TNA1", "FST1", "TBDY", "ITEM"}) {
			if (leaves.find(required) == leaves.end())
				return false;
		}

		dataSection = leaves["DATA"];
		if (!ReadTypes(leaves["TST1"], leaves["TNA1"], leaves["FST1"], leaves["TBDY"]))
			return false;

		const Section& index = leaves["ITEM"];
		for (size_t pos = index.begin; pos + 12 <= index.end; pos += 12) {
			Item item;
			item.type = ReadU32LE(file + pos) & 0xFFFFFF;
			item.offset = ReadU32LE(file + pos + 4);
			item.count = ReadU32LE(file + pos + 8);
			if (item.type >= types.size())
				return false;
			items.push_back(item);
		}
		return true;
	}

	const Type* TypeAt(uint32_t k) const { return k > 0 && k < types.size() ? &types[k] : nullptr; }

	uint32_t KindOf(uint32_t k) const {
		for (int depth = 0; depth < MaxChain; ++depth) {
			const Type* type = TypeAt(k);
			if (!type)
				return 0;
			if (type->flags & TypeSubType)
				return type->subType & 0xF;
			k = type->parent;
		}
		return 0;
	}

	uint32_t SizeOf(uint32_t k) const {
		for (int depth = 0; depth < MaxChain; ++depth) {
			const Type* type = TypeAt(k);
			if (!type)
				return 0;
			if (type->flags & TypeByteSize)
				return type->size;
			k = type->parent;
		}
		return 0;
	}

	const Member* FindMember(uint32_t k, const std::string& name) const {
		for (int depth = 0; depth < MaxChain; ++depth) {
			const Type* type = TypeAt(k);
			if (!type)
				return nullptr;
			for (const auto& member : type->members) {
				if (member.name == name)
					return &member;
			}
			k = type->parent;
		}
		return nullptr;
	}

	bool InData(size_t offset, size_t length) const {
		const size_t size = dataSection.end - dataSection.begin;
		return offset <= size && length <= size - offset;
	}

	bool U32(size_t offset, uint32_t& value) const {
		if (!InData(offset, 4))
			return false;
		value = ReadU32LE(file + dataSection.begin + offset);
		return true;
	}

	// The offset and type of a named member of the object of type k at offset
	bool Field(uint32_t k, size_t offset, const std::string& name, size_t& fieldOffset, uint32_t& fieldType) const {
		const Member* member = FindMember(k, name);
		if (!member)
			return false;
		fieldOffset = offset + member->offset;
		fieldType = member->type;
		return InData(fieldOffset, SizeOf(fieldType));
	}

	bool ItemAt(uint32_t index, Item& item) const {
		if (index == 0 || index >= items.size())
			return false;
		item = items[index];
		return true;
	}

	bool U32Field(uint32_t k, size_t offset, const std::string& name, uint32_t& value) const {
		size_t fieldOffset = 0;
		uint32_t fieldType = 0;
		return Field(k, offset, name, fieldOffset, fieldType) && SizeOf(fieldType) >= 4 && U32(fieldOffset, value);
	}

	bool StringField(uint32_t k, size_t offset, const std::string& name, std::string& value) const {
		size_t fieldOffset = 0;
		uint32_t fieldType = 0;
		uint32_t index = 0;
		if (!Field(k, offset, name, fieldOffset, fieldType) || KindOf(fieldType) != KindString || !U32(fieldOffset, index))
			return false;

		value.clear();
		if (index == 0)
			return true;

		Item item;
		if (!ItemAt(index, item) || !InData(item.offset, item.count))
			return false;
		const char* text = reinterpret_cast<const char*>(file + dataSection.begin + item.offset);
		size_t length = 0;
		while (length < item.count && text[length] != 0)
			++length;
		value.assign(text, length);
		return true;
	}

	// Element offsets and element type of a named array member
	bool ArrayField(uint32_t k, size_t offset, const std::string& name, uint32_t& elementType, std::vector<size_t>& elements) const {
		size_t fieldOffset = 0;
		uint32_t fieldType = 0;
		uint32_t index = 0;
		elements.clear();
		if (!Field(k, offset, name, fieldOffset, fieldType) || KindOf(fieldType) != KindArray || !U32(fieldOffset, index))
			return false;
		if (index == 0)
			return true;

		Item item;
		if (!ItemAt(index, item))
			return false;
		const size_t elementSize = SizeOf(item.type);
		if (elementSize == 0 || item.count > (dataSection.end - dataSection.begin) / elementSize || !InData(item.offset, size_t(item.count) * elementSize))
			return false;

		elementType = item.type;
		for (uint32_t i = 0; i < item.count; ++i)
			elements.push_back(item.offset + size_t(i) * elementSize);
		return true;
	}

	// The object a pointer element refers to
	bool Deref(size_t pointerOffset, uint32_t& type, size_t& offset) const {
		uint32_t index = 0;
		Item item;
		if (!U32(pointerOffset, index) || !ItemAt(index, item) || item.count == 0 || !InData(item.offset, SizeOf(item.type)))
			return false;
		type = item.type;
		offset = item.offset;
		return true;
	}

	std::vector<std::pair<uint32_t, size_t>> Objects(const std::string& typeName) const {
		std::vector<std::pair<uint32_t, size_t>> objects;
		for (const auto& item : items) {
			const Type* type = TypeAt(item.type);
			const size_t size = SizeOf(item.type);
			if (!type || type->name != typeName || size == 0 || !InData(item.offset, size_t(item.count) * size))
				continue;
			for (uint32_t i = 0; i < item.count; ++i)
				objects.emplace_back(item.type, item.offset + size_t(i) * size);
		}
		return objects;
	}

	// The set bits of an hkBitField
	bool Bits(uint32_t k, size_t offset, std::vector<uint32_t>& bits) const {
		size_t storageOffset = 0;
		uint32_t storageType = 0;
		uint32_t numBits = 0;
		uint32_t wordType = 0;
		std::vector<size_t> words;
		if (!Field(k, offset, "storage", storageOffset, storageType) || !U32Field(storageType, storageOffset, "numBits", numBits)
			|| !ArrayField(storageType, storageOffset, "words", wordType, words) || SizeOf(wordType) != 4)
			return false;

		for (size_t w = 0; w < words.size(); ++w) {
			uint32_t word = 0;
			if (!U32(words[w], word))
				return false;
			for (uint32_t b = 0; b < 32; ++b) {
				const uint64_t bit = uint64_t(w) * 32 + b;
				if ((word >> b & 1) && bit < numBits)
					bits.push_back(static_cast<uint32_t>(bit));
			}
		}
		return true;
	}
};

bool ReadSkeleton(const Tagfile& file, std::vector<std::string>& skeleton) {
	const auto skeletons = file.Objects("hkaSkeleton");
	if (skeletons.empty())
		return false;

	uint32_t boneType = 0;
	std::vector<size_t> bones;
	if (!file.ArrayField(skeletons.front().first, skeletons.front().second, "bones", boneType, bones))
		return false;

	for (size_t bone : bones) {
		std::string name;
		if (!file.StringField(boneType, bone, "name", name))
			return false;
		skeleton.push_back(name);
	}
	return !skeleton.empty();
}

bool ReadSimulation(const Tagfile& file, uint32_t clothType, size_t cloth, const std::vector<std::string>& skeleton, ClothSimulation& simulation) {
	const size_t skeletonSize = skeleton.size();
	if (!file.StringField(clothType, cloth, "name", simulation.name))
		return false;

	// Written bits index the transform set, which only maps to skeleton bones when it covers the whole skeleton
	uint32_t pointerType = 0;
	std::vector<size_t> pointers;
	std::vector<uint32_t> setSizes;
	if (!file.ArrayField(clothType, cloth, "transformSetDefinitions", pointerType, pointers))
		return false;
	for (size_t pointer : pointers) {
		uint32_t setType = 0;
		size_t set = 0;
		uint32_t transforms = 0;
		if (!file.Deref(pointer, setType, set) || !file.U32Field(setType, set, "numTransforms", transforms))
			return false;
		setSizes.push_back(transforms);
	}

	if (!file.ArrayField(clothType, cloth, "operators", pointerType, pointers))
		return false;

	std::set<uint32_t> written;
	for (size_t pointer : pointers) {
		uint32_t operatorType = 0;
		size_t op = 0;
		uint32_t accessType = 0;
		std::vector<size_t> accesses;
		if (!file.Deref(pointer, operatorType, op) || !file.ArrayField(operatorType, op, "usedTransformSets", accessType, accesses))
			return false;

		for (size_t access : accesses) {
			uint32_t setIndex = 0;
			size_t usage = 0;
			uint32_t usageType = 0;
			uint32_t trackerType = 0;
			std::vector<size_t> trackers;
			if (!file.U32Field(accessType, access, "transformSetIndex", setIndex) || !file.Field(accessType, access, "transformSetUsage", usage, usageType)
				|| !file.ArrayField(usageType, usage, "perComponentTransformTrackers", trackerType, trackers))
				return false;

			for (size_t tracker : trackers) {
				size_t bitsOffset = 0;
				uint32_t bitsType = 0;
				std::vector<uint32_t> bits;
				if (!file.Field(trackerType, tracker, "written", bitsOffset, bitsType) || !file.Bits(bitsType, bitsOffset, bits))
					return false;
				if (bits.empty())
					continue;
				if (setIndex >= setSizes.size() || setSizes[setIndex] != skeletonSize)
					return false;
				for (uint32_t bit : bits) {
					if (bit >= skeletonSize)
						return false;
					written.insert(bit);
				}
			}
		}
	}

	for (uint32_t bone : written)
		simulation.writtenBones.push_back(skeleton[bone]);
	return true;
}

} // namespace

std::optional<ClothBones> ReadClothBones(const char* data, size_t size) {
	if (!data)
		return std::nullopt;

	Tagfile file;
	if (!file.Load(reinterpret_cast<const uint8_t*>(data), size))
		return std::nullopt;

	ClothBones result;
	if (!ReadSkeleton(file, result.skeleton))
		return std::nullopt;

	const auto cloths = file.Objects("hclClothData");
	if (cloths.empty())
		return std::nullopt;

	bool writesAny = false;
	for (const auto& [clothType, cloth] : cloths) {
		ClothSimulation simulation;
		if (!ReadSimulation(file, clothType, cloth, result.skeleton, simulation))
			return std::nullopt;

		writesAny = writesAny || !simulation.writtenBones.empty();
		result.simulations.push_back(std::move(simulation));
	}

	// Cloth that moves nothing would be dropped from every export, so treat it as unreadable instead
	if (!writesAny)
		return std::nullopt;
	return result;
}

} // namespace HKX
