#pragma once

namespace swrots::game {

// Other duelists' moves for custom characters on Serra Keto's or Mace Windu's class (ISerra or IMace base;
// Lightning=, Grapples=IDooku, ForceStorm= in docs/modding/custom-characters.md): Force lightning instead of Serra's
// or Mace's stun. On Serra, this also adds Dooku's grapple throws and the Force Storm special. The built-in fighters
// keep their moves.

// Every start of the game image, after the custom characters are read.
void InstallRevan();

// Each frame, outside the scripts (the storm's strikes).
void RevanFrame();

} // namespace swrots::game
