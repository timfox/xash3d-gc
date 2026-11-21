/*
pm_layout.h - playermove_t layout check shared by the engine and its host test

The game DLL is compiled against its own copy of pm_defs.h, so its playermove_t must have the
engine's size and array caps. A DLL that publishes "<sizeof>,<MAX_PHYSENTS>,<MAX_MOVEENTS>" in the
cvar gc_pm_layout_dll lets the engine compare before PM_Init (a mismatch used to crash with PC 0 in
PM_InitTextureTypes). A DLL that publishes nothing (the stock SDK) is not checked.
No dependencies on purpose.
*/
#ifndef PM_LAYOUT_H
#define PM_LAYOUT_H

/* Parse "a,b,c" into out[3]. Returns 1 on success, 0 on anything else. */
static inline int PM_LayoutParse( const char *s, int out[3] )
{
	int i;

	if( !s || !s[0] )
		return 0;
	for( i = 0; i < 3; i++ )
	{
		int v = 0, digits = 0;

		while( *s >= '0' && *s <= '9' )
		{
			v = v * 10 + ( *s++ - '0' );
			digits++;
			if( v > 100000000 )
				return 0;
		}
		if( !digits )
			return 0;
		out[i] = v;
		if( i < 2 )
		{
			if( *s != ',' )
				return 0;
			s++;
		}
	}
	return *s == '\0';
}

/* 1 when the published layout is absent (no check possible) or equal to the engine's, 0 on a mismatch. */
static inline int PM_LayoutMatches( const char *dll, int size, int physents, int moveents )
{
	int v[3];

	if( !dll || !dll[0] )
		return 1;
	if( !PM_LayoutParse( dll, v ))
		return 0;
	return v[0] == size && v[1] == physents && v[2] == moveents;
}

#endif
