# SWROTS-PC online lobby

The list of public rooms (Online > Public Rooms in the game), as a Cloudflare Worker with one Durable Object. It
keeps the rooms in memory only: a host whose room is public announces it every 30 s, a room not announced for 75 s
disappears, and a host that closes its room takes it away.

## API

| Request | What |
| --- | --- |
| `POST /rooms` | announce or refresh a room: JSON `{code, name, mode, arena, players, version}` |
| `DELETE /rooms/<code>` | take a room away (only from the address that announced it) |
| `GET /rooms.txt?version=N` | the rooms of protocol `N`, one a line: code, name, mode, arena, players, slots (tab-separated) |
| `GET /rooms` | the same as JSON |
| `GET /` | a page listing the rooms |

At most 3 rooms per address and 200 in all; names, modes and arenas are cut to printable ASCII.

## Deploying (Cloudflare's free Workers plan)

1. Create a free account at <https://dash.cloudflare.com/sign-up>.
2. Install Node.js (LTS) from <https://nodejs.org>.
3. In this folder: `npx wrangler login` (a browser window asks to allow Wrangler), then `npx wrangler deploy`.
4. Wrangler prints the lobby's address, `https://swrots-lobby.<your-subdomain>.workers.dev`. Open it in a browser:
   the page lists the public rooms.
5. In the game's `settings.ini`, under `[Online]`: `Lobby=https://swrots-lobby.<your-subdomain>.workers.dev` (or
   build the game with that address as `kDefaultLobby` in `src/net/lobby.cpp`, so that every player has it).

The free plan allows 100,000 requests a day (a public room takes about 2,900 a day; a player on Public Rooms 720
an hour), which is plenty for a small community.
