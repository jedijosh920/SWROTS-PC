# Battlefront

A conquest battle in the style of Star Wars: Battlefront: clone troopers against battle droids, command posts to take
and hold, tickets, and heroes for points. It is in the game's own main menu: **Multi Player > Battlefront**.

## Setting it up

| Row | Choices |
|---|---|
| Map | the developer map (always there), and every installed custom map with command posts (its `[Battlefront]` section, see [custom maps](modding/custom-maps.md#battlefront)) |
| Side | Republic (you are a clone trooper) or Separatists (a battle droid) |
| Troopers | 4, 6 or 8 a side, you one of them |
| Tickets | each side's: 50 to 300 |
| Heroes | on or off |

**Start** begins the battle. After it ends, the game goes back to the main menu.

## How it plays

- **Command posts** (A, B, C, ...) are marked over the battlefield, and in a row under the tickets in the colour of
  their side (blue: the Republic, red: the Separatists, grey: nobody's). Stand near one with no enemy near it to take
  it: an enemy's post turns neutral first, then yours. More of your side there takes it faster (up to three).
  Each side starts with the post nearest its start.
- **Tickets**: every trooper that comes back costs its side one. A side holding fewer posts than the other loses a
  ticket every 5 seconds (every 2 seconds holding none while the other holds all). A side out of tickets loses.
- **Coming back**: after a fall you come back at one of your side's posts or its start, one with no enemy near
  (picked at random among those). On a custom map, at one of the map's own clear places (`Spawns=`) near it, the
  nearest when none is within 350 units, set down just above the floor (its ceilings may be low).
- **The computer's troopers** have a job: about one in three of a side defends one of its posts, the others attack
  the nearest post their side does not hold. They fight any enemy that comes near, then go back to their post.
  Heroes hunt the enemy.
- **Points**: damage, defeats, assists and posts taken. Hold Tab (Back) for the leaderboard.
- **Heroes**: 500 points buy a hero: when "HERO READY" shows, press **B** (Circle; on the keyboard the critical
  attack key, E). You become one of your side's heroes where you stand, until you fall; then you come back as a
  trooper. The computer's troopers earn points for their side too and call their own hero (one hero a side at a time).
  The Republic's: Anakin, Obi-Wan, Yoda, Commander Cody and the custom heroes installed (Luke...); the Separatists':
  Grievous, Dooku, the Droid Commander, the custom villains installed (Maul, Darth Revan...) and Vader.

As a trooper you shoot like [Commander Cody](controls.md#commander-cody) and the
[Droid Commander](controls.md#droid-commander) (heavy attack held, free, no meter), roll (a clone) or duck (a droid)
with White/L1/Q, and strike with the fast attack; no reinforcements or bombardments.

## How it works (for developers)

Battlefront is Heroes vs Villains (`src/game/hvv.cpp`) with `bf:1`: its fighters are placed in the level (so their
classes load with it, sounds and all), the troopers in groups at their sides' starts; the heroes' classes are placed
too, far off and never woken, so a hero called later has its sounds. Lives become tickets, the respawn place a post of
the side's. The computer's troopers walk to their posts with the game's own goto (`ICharacter::Goto` to an
`ICharacterGoto` marker; the dev map builder copies the level's marker to every post) and are given the nearest enemy
within 800 units to fight; one that cannot get through to its post (no path nodes on these maps) is moved near it. The
player's trooper gets Cody's kit (`cody.cpp`, a trooper kind: no specials). A battle droid's first costume has no
body, so its placed droids carry the plain droid's (`Mesh Choice`). Launch arguments (as the menu writes them):
`hvv:<troopers> hvvheroes:<a side> hvvplayer:<n> hvvlives:<tickets> bf:1 bfhealth:<trooper>,<hero>
bfheroes0:<classes> bfheroes1:<classes> bfherocost:<points> [custommap:<id>]`.
