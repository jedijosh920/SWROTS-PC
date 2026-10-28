#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Hooks the engine's resource reader so loose files under mods\ override
// resources inside the level PAKs. Generated resources are written under `cacheDir`\disc. A
// non-empty `dumpDir` saves every resource's raw bytes there as it loads; `logResources` logs every
// resource read.
void InstallResourceHooks(const std::wstring& gameData, const std::wstring& modsDir, const std::wstring& cacheDir,
    const std::wstring& dumpDir, bool logResources);

// True when a level PAK on the disc (or a copy under mods\pak) has `lowerName` (relative, e.g.
// "cinematics\introcamera\anakin_intro_cam.cin") with its data.
bool DiscHasResource(const std::string& lowerName);

// A body (a mesh under meshes\chars) worn by a class it was not made for gets a private name, so that
// its animation binding (built per mesh, for the classes wearing it) is its own: the level's characters
// in the original keep theirs. The private name is the original's with a tag and the class's hash
// ("clonetrooper\hordetrooper__pb1a2b3c4d"); everything named after it (the mesh, its binding, its
// limbs) is served as the original's.
constexpr const char* kPrivateBodyTag = "__pb";
std::string PrivateBodyName(const std::string& mesh, const std::string& className);
// The original name of a resource named after a private body (unchanged for any other name).
std::string PrivateBodyOriginal(const std::string& name);

// True when mods\ (or cache\disc\) has a loose copy of `lowerName`, as above.
bool HasLooseResource(const std::string& lowerName);

// Edits a resource's data as a PAK serves it, before the engine decodes it (a loose copy under
// mods\ is used as it is). `lowerName` is relative and the name the engine asks for, e.g.
// "interfc\front_end\xml\select_jedi.xml" (in the PAK, the compiled select_jedi.xbl_xml);
// the patch changes `data` in place. Memory-image resources cannot be patched.
using ResourcePatch = void (*)(std::vector<uint8_t>& data);
void RegisterResourcePatch(const std::string& lowerName, ResourcePatch patch);

// Resources no PAK has, made from others when asked for (e.g. a darker copy of a texture). A
// generator fills `data` with the resource's file contents and returns true when `lowerName`
// (relative, with its extension) is one it makes; results are kept for the session. The port serves
// them like PAK resources and declares them in a level that asks for them.
using ResourceGenerator = bool (*)(const std::string& lowerName, std::vector<uint8_t>& data);
void RegisterResourceGenerator(ResourceGenerator generator);

// Makes a generated resource now (the generators above), written to cache\disc so that a direct load from its file
// (a picture the port loads itself) finds it; false when no generator makes it.
bool MakeGeneratedResource(const std::string& lowerName);

// A resource's file contents from the disc's PAKs; false when none has it with its data.
bool ReadDiscResource(const std::string& lowerName, std::vector<uint8_t>& data);

// The names of the resources on the disc's PAKs starting with `lowerPrefix`.
std::vector<std::string> DiscResourceNames(const std::string& lowerPrefix);

// The loose files under mods\ starting with `lowerPrefix` (a folder, e.g. "meshes\\chars\\"), named as
// resources (lower case, relative).
std::vector<std::string> LooseResourceNames(const std::string& lowerPrefix);

// Declares a generated resource of `typeId` in the current level, for engine code that checks a
// resource is declared before asking for it. False when it was declared already or is not made.
bool DeclareGeneratedResource(const std::string& lowerName, int typeId);

// Declares a resource another level's PAK has (relative, lower case, e.g.
// "meshes\\chars\\clonetrooper\\hordetrooper_var01.stx") in the running level, so the
// game loads it when asked for. False when the level already has it or no PAK does.
bool DeclareDiscResource(const std::string& lowerName);

// Animations (file names, lower case, e.g. "anakin_atk_kill_lunge_humangr.bnm") another class's sequences play (a
// mission's [Animations]): when a class asks for one its folder lacks, it is declared in that class's folder, as
// another duelist's Versus intro is. Replaces the last list; every start of the game image.
void SetBorrowedAnimations(std::vector<std::string> lowerFiles);

// Declares and loads a generated resource in the current level now, for engine code that only
// finds resources already loaded (a menu's pictures, by name). False when it did not load.
bool LoadGeneratedResource(const std::string& lowerName, int typeId);

} // namespace swrots::game
