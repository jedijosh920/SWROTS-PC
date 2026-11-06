// SWROTS-PC online lobby: the public rooms' list (a Cloudflare Worker and one Durable Object).
//
// A host whose room is public announces it every 30 s (POST /rooms); a room not announced for 75 s is gone, and a
// host that closes its room takes it away (DELETE /rooms/<code>). Players list the rooms (GET /rooms.txt for the game,
// GET /rooms for JSON, / for a page). The rooms are also kept in the object's storage, so that they outlive the
// object being taken out of memory (Cloudflare may do it at any time).
//
// The room code is what a player types to join (it holds the host's address): a public room shows it to anyone.

const ROOM_TTL_MS = 75_000;
const MAX_ROOMS = 200;
const MAX_ROOMS_PER_ADDRESS = 3;
const CODE_ALPHABET = /^[0-9ACE-HJ-NP-RT-Z]{15}$/; // the game's room codes: base 31, no B, D, I, O, S

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "OPTIONS")
      return new Response(null, { headers: cors() });
    if (!url.pathname.startsWith("/rooms") && url.pathname !== "/")
      return new Response("not found", { status: 404, headers: cors() });
    const lobby = env.LOBBY.get(env.LOBBY.idFromName("lobby"));
    return lobby.fetch(request);
  },
};

function cors() {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
  };
}

// Text the game shows: printable ASCII, no tabs or line breaks (the game's list is one room a line, tab-separated).
function clean(value, most) {
  return String(value ?? "")
    .replace(/[^\x20-\x7e]/g, "")
    .trim()
    .slice(0, most);
}

function escapeHtml(text) {
  return text.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

export class Lobby {
  constructor(state, env) {
    this.storage = state.storage;
    this.rooms = new Map(); // code -> { code, name, mode, arena, players, slots, version, address, seen }
    state.blockConcurrencyWhile(async () => {
      const stored = await this.storage.list({ prefix: "room:" });
      for (const room of stored.values())
        if (room && room.code) this.rooms.set(room.code, room);
    });
  }

  prune() {
    const now = Date.now();
    const gone = [];
    for (const [code, room] of this.rooms)
      if (now - room.seen > ROOM_TTL_MS) {
        this.rooms.delete(code);
        gone.push("room:" + code);
      }
    if (gone.length) this.storage.delete(gone);
  }

  list(version) {
    this.prune();
    return [...this.rooms.values()]
      .filter((r) => !version || r.version === version)
      .sort((a, b) => a.players - b.players || b.seen - a.seen);
  }

  async fetch(request) {
    const url = new URL(request.url);
    const address = request.headers.get("CF-Connecting-IP") || "";
    const headers = cors();

    if (request.method === "POST" && url.pathname === "/rooms") {
      let body;
      try {
        body = await request.json();
      } catch {
        return new Response("bad json", { status: 400, headers });
      }
      const code = clean(body.code, 15).toUpperCase();
      if (!CODE_ALPHABET.test(code))
        return new Response("bad code", { status: 400, headers });
      this.prune();
      const existing = this.rooms.get(code);
      if (existing && existing.address !== address)
        return new Response("not yours", { status: 403, headers });
      if (!existing) {
        const mine = [...this.rooms.values()].filter((r) => r.address === address).length;
        if (mine >= MAX_ROOMS_PER_ADDRESS || this.rooms.size >= MAX_ROOMS)
          return new Response("too many rooms", { status: 429, headers });
      }
      const players = Math.max(1, Math.min(2, Number(body.players) | 0));
      const room = {
        code,
        name: clean(body.name, 16) || "NONAME",
        mode: clean(body.mode, 16) || "VERSUS",
        arena: clean(body.arena, 24),
        players,
        slots: 2,
        version: Math.max(0, Number(body.version) | 0),
        address,
        seen: Date.now(),
      };
      this.rooms.set(code, room);
      await this.storage.put("room:" + code, room);
      return new Response("ok", { headers });
    }

    if (request.method === "DELETE" && url.pathname.startsWith("/rooms/")) {
      const code = clean(url.pathname.slice("/rooms/".length), 15).toUpperCase();
      const room = this.rooms.get(code);
      if (room && room.address === address) {
        this.rooms.delete(code);
        await this.storage.delete("room:" + code);
      }
      return new Response("ok", { headers });
    }

    if (request.method === "GET" && url.pathname === "/rooms.txt") {
      const version = Number(url.searchParams.get("version")) | 0;
      const lines = this.list(version).map((r) =>
        [r.code, r.name, r.mode, r.arena, r.players, r.slots].join("\t"));
      return new Response(lines.join("\n"), { headers: { ...headers, "Content-Type": "text/plain; charset=utf-8" } });
    }

    if (request.method === "GET" && url.pathname === "/rooms") {
      const version = Number(url.searchParams.get("version")) | 0;
      const rooms = this.list(version).map(({ address, seen, ...r }) => r);
      return Response.json(rooms, { headers });
    }

    if (request.method === "GET" && url.pathname === "/") {
      const rows = this.list(0)
        .map((r) => `<tr><td>${escapeHtml(r.name)}</td><td>${r.players}/${r.slots}</td><td>${escapeHtml(r.mode)}</td>` +
          `<td>${escapeHtml(r.arena)}</td></tr>`)
        .join("");
      const page = `<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>SWROTS-PC Rooms</title>
<meta http-equiv="refresh" content="15">
<style>body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:24px}
h1{font-size:20px}table{border-collapse:collapse;width:100%;max-width:720px}
td,th{padding:8px 12px;border-bottom:1px solid #30363d;text-align:left}th{color:#8b949e;font-weight:600}</style>
</head><body><h1>SWROTS-PC public rooms</h1>
<table><tr><th>Host</th><th>Players</th><th>Mode</th><th>Arena</th></tr>${rows ||
        '<tr><td colspan="4">No public room right now.</td></tr>'}</table>
<p style="color:#8b949e">Join from the game: Online &gt; Public Rooms.</p></body></html>`;
      return new Response(page, { headers: { ...headers, "Content-Type": "text/html; charset=utf-8" } });
    }

    return new Response("not found", { status: 404, headers });
  }
}
