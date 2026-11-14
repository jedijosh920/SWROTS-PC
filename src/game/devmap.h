#pragma once

#include <string>

#include "game/devmapbuild.h"

namespace swrots::game {

// The developer test map (devmapbuild.h, docs/research/levels.md): Grievous Survival's level made an empty, lit,
// flat floor under the sky, built by the port as the level loads. It is wanted by the launch argument `devmap:1`,
// in the launch data (StartDevMap) or in Default_Xbox.cfg next to `map:u107_uta_grievoussurvival`.

// Each start of the game image: whether this start wants the dev map; registers the level's resource patches.
// Before the port's modes (which read their arguments with BootArgument and lay out the map).
void InstallDevMap(const std::wstring& gameData, const std::wstring& mods);

// True when this start of the game loads the dev map in place of Grievous Survival.
bool DevMapActive();
// Once a frame on the game thread: a custom map's falls (KillHeight=).
void DevMapFrame();

// The map of a mod's own this start's dev map is (launch argument `custommap:<id>`, custommaps.h), or null.
struct CustomMap;
const CustomMap* DevMapCustomMap();

// A launch argument of this start of the game (`name:value`, from its launch data, else from Default_Xbox.cfg).
bool BootArgument(const char* name, std::string& value);
// Whether this start is a Versus duel (one of the game's duel arenas, or the dev map's duel).
bool BootIsVersusDuel();
// Whether this start is a match (a Versus duel, Heroes vs Villains or a mission of a mod's own): co-op is no part of
// it, and duelists have their Versus moves (intros).
bool BootIsMatch();

// What a mode puts on this start's dev map (before the level loads).
void SetDevMapLayout(const DevMapLayout& layout);

// Starts the dev map, with more launch arguments (e.g. a mode's), as a level change does: the game restarts into
// it. Any thread; returns at once.
void StartDevMap(const std::string& more = {});

// Starts a Versus duel (the game's own) on the dev map or a custom map (`map`: its id; empty: the floor): `first`
// (player 1) against `second` (the computer's), `rounds` to win (0: 3), with more launch arguments (the profile's).
// The game restarts into it; at its end it goes back to the menus as any duel.
void StartDuel(const std::string& first, const std::string& second, const std::string& map, int rounds,
    const std::string& more = {}, bool secondHuman = false);

} // namespace swrots::game
