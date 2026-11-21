/*
sv_lazy_logic.h - deferred spawn of map logic entities (platform independent)
Copyright (C) 2026 xash3d-gc contributors

On the GameCube the 460-entry edict table cannot hold every parity entity of a DoD map: the logic entities that
come late in the lump (trigger_relay, trigger_changetarget, info_target) are skipped. Their text is still in the
entity lump, so instead of dropping them the server remembers where each one starts (an offset and a length into
the lump text, a hash of its targetname) and spawns it the first time something looks it up by name. The game DLL's
own code then runs, unchanged, so firing semantics are the DLL's.

Entries are spawned in lump order, all of those with the same name at once, which keeps the relative order of
same-named entities (the budget keeps the first entities of the lump, so the deferred ones always come after the
kept ones). No libogc or engine dependency: tested on the host (tests/test_lazy_logic.py).
*/
#ifndef SV_LAZY_LOGIC_H
#define SV_LAZY_LOGIC_H

#include <stdint.h>

#define LAZY_LOGIC_MAX 256

/* The same string hash on both sides (the engine hashes the name it is asked for). */
uint32_t LazyLogic_Hash( const char *name );

void LazyLogic_Reset( void );
/* Remember a skipped entity: lump text offset and length, hash of its targetname. -1 when the table is full. */
int  LazyLogic_Add( uint32_t name_hash, uint32_t ofs, uint32_t len );
/* Entries not spawned yet. */
int  LazyLogic_Pending( void );
/* Entries that were remembered / spawned since the last reset. */
int  LazyLogic_Total( void );
int  LazyLogic_Spawned( void );
/* Lookups that wanted to spawn something and could not (no edict, no heap): the entries stay pending and retry. */
int  LazyLogic_Failed( void );
/* Entities that could not be remembered because the table was full (they are dropped, as before the lazy path). */
int  LazyLogic_Dropped( void );
/* The engine records which edict a spawned entry got, so a report can say whether it still exists. */
void LazyLogic_NoteEdict( uint32_t ofs, int edict );
/* Entry i in the order they were added: lump offset, length, spawned flag, edict (0 = none recorded). 0 when i is out of range. */
int  LazyLogic_Entry( int i, uint32_t *ofs, uint32_t *len, int *spawned, int *edict );

/* Spawn callback: returns 1 when the entity now exists, 0 when it was not this name (hash collision: it stays
 * pending), -1 when it could not be spawned now (no edict, no heap: it stays pending for a later lookup). */
typedef int ( *lazy_spawn_fn )( void *ctx, uint32_t ofs, uint32_t len );

/* Spawn every pending entry whose name hash matches, in the order they were added. Returns how many spawned. An entry
 * is marked spawned before the callback runs, so a callback that looks names up again cannot re-enter itself. */
int  LazyLogic_Materialize( uint32_t name_hash, lazy_spawn_fn spawn, void *ctx );

#endif
