/*
BodySlide and Outfit Studio
See the included LICENSE file

Reads which bones a Starfield cloth payload moves.

Starfield stores cloth as a Havok 2016+ binary tagfile (TAG0) inside BSClothExtraData. The payload lists the whole
character rig, but the simulation only writes some of it: every operator records per-bone read and written bits for
the transform set it uses, and the transform set is the payload's skeleton. Only the written bits tell which skinned
shapes the cloth moves. The payload never names the NIF shapes it deforms.
*/

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace HKX {

struct ClothSimulation {
	std::string name;
	// Skeleton bones the simulation writes, in skeleton order and spelled as in the payload
	std::vector<std::string> writtenBones;
};

struct ClothBones {
	std::vector<std::string> skeleton;
	std::vector<ClothSimulation> simulations;
};

// Returns nothing for data that is not a tagfile, or whose cloth can't be resolved to skeleton bones completely
std::optional<ClothBones> ReadClothBones(const char* data, size_t size);

} // namespace HKX
