# Edict survey: what fills the 460-edict table, and what could leave it

Local working note (no code change). M = measured from the BSP lump or a Dolphin log, R = read
from source, D = derived. Maps: dod_charlie, dod_avalanche, dod_caen, dod_zalec, dod_flash.

## 1. How the table fills (M, engine census line `entities=... always=... parity=... budget=...`)

| Map | BSP entities | always | parity classes | parity budget | optional (dropped) | static props (no edict) |
|---|--:|--:|--:|--:|--:|--:|
| charlie | 903 | 290 | 304 | **132** | 69 | 144 |
| avalanche | 479 | 78 | 249 | 344 | 103 | 20 |
| caen | 620 | 172 | 290 | **250** | 41 | 77 |
| zalec | 663 | 230 | 158 | 192 | 96 | 116 |
| flash | 399 | 143 | 94 | 281 | 4 | 75 |

Edicts in use at map active are about `always + parity kept`: charlie 290 + 132 = **422 of 460**,
caen 172 + 250 = **422**, avalanche 78 + 249 = 327, zalec 230 + 158 = 388, flash 143 + 94 = 237 (D).
On charlie 172 of the 304 parity entities are dropped today, on caen 40 of 290; the rest of the
parity classes are kept up to the budget (`SV_GCCapFacesOptionalEntity`, `sv_game.c` ~6122).
"Optional" classes (func_illusionary baked into display lists, infodecal, env_glow, env_message,
light, info_target, trigger_relay...) are dropped outright under the CapFaces policy.

## 2. Which parity classes could leave the table (class total / unnamed; unnamed = nothing can target it)

| Class | charlie | avalanche | caen | zalec | flash | Needs an edict because |
|---|--:|--:|--:|--:|--:|---|
| func_wall | 55/55 | **231/231** | **148/148** | 51/51 | 79/79 | solid brush: pmove physent, bullet and monster traces |
| env_sprite | 55/3 | 0 | 31/4 | 32/8 | 1/1 | named ones are toggled by relays; unnamed ones are decoration |
| ambient_generic | 47/7 | 18/9 | 25/2 | 31/3 | 11/1 | unnamed ones are looping ambience; named ones are triggered |
| trigger_relay | 51/0 | 0 | 0 | 12/0 | 3/0 | map logic (all named) |
| info_target | 23/0 | 0 | 9/0 | 4/0 | 0 | all named: target positions found by name |
| env_shake / env_explosion | 18/0, 16/0 | 0 | 10/0, 20/0 | 2/0, 9/0 | 0 | named effects fired by relays |
| func_wall_toggle | 6/0 | 0 | 15/0 | 14/0 | 0 | named; switched by logic |
| light | 29/18 | 0 | 17/17 | 73/73 | 2/2 | none at runtime (already optional) |
| func_illusionary | 96/95 | 13/13 | 24/13 | 31/31 | 83/83 | none (baked, already optional) |

Everything that carries a targetname is part of the map's logic and has to stay an entity (or the
game DLL's `FIND_ENTITY_BY_STRING` has to learn a second table, which is a rewrite of the lookup).
That leaves two honest levers.

**Lever 1: unnamed func_wall as an edict-free static brush list (the big one).** func_wall is
231 of avalanche's 460 and 148 of caen's. Unnamed means it never moves or fires; it is solid
geometry that stands in the way. R: the engine already draws static props from a list with no edict
(`SV_GCStaticProp...`, PR 157). Collision needs more: `SV_AddLinksToPmove` builds the pmove physent
list from area-node links of edicts, and `SV_Move`/trace walk the same links. A static brush list
would need (a) its own physent source in `SV_AddLinksToPmove` and the client equivalent,
(b) a trace path (`SV_ClipMoveToEntity` takes an `edict_t`, so a thin wrapper), (c) rendering from
the BSP submodel as today. Saves up to **55 / 231 / 148 / 51 edicts** (charlie / avalanche /
caen / zalec) and lets charlie's parity budget cover more of its dropped 172. Risk: medium to
high (collision and trace paths, both client and server). Not started.

**Lever 2: the weapon entities per player.** See section 3. About 2 to 3 edicts per bot.

Small ones (D): unnamed env_sprite as server static entities (3 / 0 / 4 / 8 on the four maps) and
unnamed ambient_generic as a client ambient list (7 / 9 / 2 / 3). Together **4 to 12** edicts,
low risk, but little.

## 3. Why a bot costs about 3 edicts (R, `src/dod_runtime.cpp` ~415-450)

Spawn calls `GiveNamedItem` for each weapon the class carries: `weapon_crowbar` (always), the
class sidearm when it has one, the primary weapon, and `weapon_handgrenade` when the loadout has
grenades. In the stock HL SDK each of those is a `CBasePlayerItem`, an edict of its own, beside the
player's body edict. So a bot is **1 body + 2 to 4 weapon entities** (3 on a class with no
sidearm, up to 5 with all four). A dead bot's weapons are removed (no box drop, `DOD_PARITY.md`);
a human's death can add a weapon box while `DOD_DROP_EDICT_MARGIN` (45) edicts are free.

Cheapest cut: for bots only, create the entities lazily, one for the weapon the bot actually holds,
and keep the others as inventory data (ammo and clip are already `GiveAmmo` data) until the bot
switches. If the bot AI never selects the crowbar or sidearm, that is **2 fewer edicts per bot**; with
the grenade created at the first throw, **3 fewer**. Humans keep stock behaviour. This is game-DLL
code in dod-gc, no engine change. Risk: low to medium (weapon switching, `DeadPlayerWeapons`).

## 4. Achievable bot count (D)

`DOD_BOT_EDICT_RESERVE` is 22. Free edicts on charlie and caen = 460 - 422 - 22 = **16**, so at 3
edicts per bot the table allows **5 bots** and at 5 edicts **3 bots**, before any weapon boxes. With
lazy weapon entities (1 body + 1 weapon = 2 per bot) the same 16 holds **8 bots**. The 5-slot
build is the other limit: 4 bots. Seven bots (`dod_bots 7`, the cvar default) needs about 21 edicts
at 3 each, so today it needs the lazy weapons or lever 1.

## 5. BSS cost per edict (M, ELF symbols, 460-edict tables)

| Table | Bytes | Per edict |
|---|--:|--:|
| `gc_singleplayer_edicts` (edict_t) | 369,840 | 804 |
| `gc_singleplayer_baselines` (entity_state_t) | 156,400 | 340 |
| `gc_gcmap_bootstrap_entities` (cl_entity_t) | 761,760 | 1,656 |
| client remap pointers | 1,844 | 4 |
| **Total fixed BSS** | | **about 2,804 B (2.74 KiB)** |

plus the game's private data for each entity (heap slab, varies by class). So the table at 460
costs about 1.26 MiB of BSS, and lowering the cap would return 2.74 KiB per edict; the cap stays
460 (Tim wants parity). Freed edicts are headroom for bots and for the dropped parity classes, not
BSS.

## 6. Suggested order

1. Lazy weapon entities for bots (dod-gc): 2 to 3 edicts per bot, low risk.
2. Static env_sprite and ambient_generic lists: 4 to 12 edicts per map, low risk.
3. Static brush list for unnamed func_wall: 51 to 231 edicts per map, the real fix for avalanche and
   caen, medium to high risk, engine and client.
