#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace swrots::game::fullbody {

// Full-body select pictures made from a fighter's own model on the disc (the player's copy of the game), for a
// fighter the disc has none for (Yoda): his model posed from one of his animation clips, edited (the body tilted, the
// cape let fall, an arm brought forward, the blade aimed, the other hand onto the hilt), lit and drawn in software,
// then graded, its blade and halo drawn as the game's own full bodies' (the values measured on them), as a 128x256
// DXT5 STX. tools/fullbody makes the same pictures in Blender (fb_render.py, fb_stx.py, yoda.json).

// Reads a resource of the disc (relative, lower case, e.g. "meshes\\chars\\yoda\\yoda.msh"); false when none has it.
using ReadResource = std::function<bool(const std::string& lowerName, std::vector<uint8_t>& data)>;

// The picture of player `player` (0: s_selduel_fb_yoda_v1, 1: _v2, Sith Yoda), as an STX whose texture is named
// `textureName`; empty when a file it is made from cannot be read, with the reason in `report`.
std::vector<uint8_t> MakeYodaPicture(int player, const ReadResource& read, const char* textureName, std::string& report);

} // namespace swrots::game::fullbody
