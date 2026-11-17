/*
dodgc_master_http.h - request/response handling for the DoD GC REST master.
Pure C with no engine dependency so the host tests can compile it. Everything
that comes from the network is treated as hostile: the list parser reads only
"ip" and "port" of each entry, copies nothing else and never allocates.

  GET  <master>/v1/servers?gamedir=<dir>   {"servers":[{"ip":"a.b.c.d","port":N,...}]}
  POST <master>/v1/servers                 {"port":N,"gamedir":"dod","map":"..","players":N,"max":N,"name":".."}
*/
#ifndef DODGC_MASTER_HTTP_H
#define DODGC_MASTER_HTTP_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DODGC_MASTER_MAX_ENTRIES 64   /* never act on more than this per reply */
#define DODGC_MASTER_MAX_REPLY   8192 /* longer replies are dropped unparsed */

typedef void ( *dodgc_master_entry_cb )( const uint8_t ip[4], uint16_t port, void *user );

/* ---- reply parsing ------------------------------------------------------ */

static inline int DodGC_JsonSkipWs( const char *s, size_t n, size_t i )
{
	while( i < n && ( s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' )) i++;
	return (int)i;
}

/* Reads a JSON string starting at the opening quote. Copies at most outsz-1 bytes
 * of plain printable ASCII; returns the index after the closing quote, or -1 on
 * an escape, control byte, non-ASCII byte, overlong string or missing quote. */
static inline int DodGC_JsonString( const char *s, size_t n, size_t i, char *out, size_t outsz )
{
	size_t o = 0;

	if( i >= n || s[i] != '"' ) return -1;
	for( i++; i < n; i++ )
	{
		unsigned char c = (unsigned char)s[i];
		if( c == '"' )
		{
			if( out && outsz ) out[o] = 0;
			return (int)i + 1;
		}
		if( c < 0x20 || c > 0x7e || c == '\\' ) return -1;
		if( out )
		{
			if( o + 1 >= outsz ) return -1;
			out[o++] = (char)c;
		}
		else if( ++o > 256 ) return -1;
	}
	return -1;
}

/* Skips one scalar value (string, number, true/false/null). Nested containers are
 * refused: the entries we care about are flat. Returns next index or -1. */
static inline int DodGC_JsonSkipScalar( const char *s, size_t n, size_t i )
{
	size_t start = i;

	if( i >= n ) return -1;
	if( s[i] == '"' ) return DodGC_JsonString( s, n, i, NULL, 0 );
	while( i < n && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\r' && s[i] != '\n' && s[i] != '\t' )
	{
		if( s[i] == '{' || s[i] == '[' || s[i] == '"' ) return -1;
		i++;
	}
	return i > start && i - start <= 24 ? (int)i : -1;
}

/* "a.b.c.d" -> bytes. Rejects leading zeros, out-of-range, extra text. */
static inline int DodGC_ParseIPv4( const char *s, uint8_t ip[4] )
{
	for( int part = 0; part < 4; part++ )
	{
		int v = 0, digits = 0;

		while( *s >= '0' && *s <= '9' )
		{
			if( digits && v == 0 ) return 0; /* leading zero */
			v = v * 10 + ( *s++ - '0' );
			if( ++digits > 3 || v > 255 ) return 0;
		}
		if( !digits ) return 0;
		ip[part] = (uint8_t)v;
		if( part < 3 && *s++ != '.' ) return 0;
	}
	return *s == 0;
}

/* Addresses a master must never hand to a console. Private ranges stay allowed so
 * a LAN test master works. */
static inline int DodGC_IPv4Usable( const uint8_t ip[4] )
{
	if( ip[0] == 0 || ip[0] == 127 || ip[0] >= 224 ) return 0;  /* this, loopback, multicast, reserved, broadcast */
	if( ip[0] == 169 && ip[1] == 254 ) return 0;                 /* link-local */
	return 1;
}

/* Parses one flat object starting at '{'. Returns next index or -1. */
static inline int DodGC_ParseEntry( const char *s, size_t n, size_t i, dodgc_master_entry_cb cb, void *user, int *emitted )
{
	char key[16], ipstr[20];
	uint8_t ip[4] = { 0 };
	int have_ip = 0, have_port = 0, port = 0, j;

	i = (size_t)DodGC_JsonSkipWs( s, n, i + 1 ); /* past '{' */
	while( i < n && s[i] != '}' )
	{
		if(( j = DodGC_JsonString( s, n, i, key, sizeof( key ))) < 0 ) return -1; /* long/odd key */
		i = (size_t)DodGC_JsonSkipWs( s, n, (size_t)j );
		if( i >= n || s[i] != ':' ) return -1;
		i = (size_t)DodGC_JsonSkipWs( s, n, i + 1 );

		if( !strcmp( key, "ip" ))
		{
			if(( j = DodGC_JsonString( s, n, i, ipstr, sizeof( ipstr ))) < 0 ) return -1;
			have_ip = DodGC_ParseIPv4( ipstr, ip ) && DodGC_IPv4Usable( ip );
			i = (size_t)j;
		}
		else if( !strcmp( key, "port" ))
		{
			size_t k = i;
			port = 0;
			while( k < n && s[k] >= '0' && s[k] <= '9' && k - i < 6 ) port = port * 10 + ( s[k++] - '0' );
			if( k == i || k - i > 5 ) return -1;
			have_port = port >= 1024 && port <= 65535;
			i = k;
		}
		else if(( j = DodGC_JsonSkipScalar( s, n, i )) < 0 ) return -1;
		else i = (size_t)j;

		i = (size_t)DodGC_JsonSkipWs( s, n, i );
		if( i < n && s[i] == ',' ) i = (size_t)DodGC_JsonSkipWs( s, n, i + 1 );
		else if( i >= n || s[i] != '}' ) return -1;
	}
	if( i >= n ) return -1;
	if( have_ip && have_port && cb )
	{
		cb( ip, (uint16_t)port, user );
		( *emitted )++;
	}
	return (int)i + 1;
}

/* Finds {"servers":[ ... ]} and calls cb for every valid entry. Returns the number of
 * entries delivered, or -1 when the reply is not what we expect. A malformed entry
 * stops parsing, but entries already delivered stay (each was valid on its own). */
static inline int DodGC_MasterParseList( const char *s, size_t n, dodgc_master_entry_cb cb, void *user )
{
	static const char tag[] = "\"servers\"";
	size_t i = 0, t;
	int emitted = 0, seen = 0, j;

	if( !s || n == 0 || n > DODGC_MASTER_MAX_REPLY ) return -1;
	i = (size_t)DodGC_JsonSkipWs( s, n, 0 );
	if( i >= n || s[i] != '{' ) return -1;
	i = (size_t)DodGC_JsonSkipWs( s, n, i + 1 );
	if( n - i < sizeof( tag ) - 1 || strncmp( s + i, tag, sizeof( tag ) - 1 )) return -1; /* servers key must come first */
	i = (size_t)DodGC_JsonSkipWs( s, n, i + sizeof( tag ) - 1 );
	if( i >= n || s[i] != ':' ) return -1;
	i = (size_t)DodGC_JsonSkipWs( s, n, i + 1 );
	if( i >= n || s[i] != '[' ) return -1;
	i = (size_t)DodGC_JsonSkipWs( s, n, i + 1 );
	(void)t;
	while( i < n && s[i] != ']' )
	{
		if( s[i] != '{' || seen >= DODGC_MASTER_MAX_ENTRIES ) return emitted;
		seen++;
		if(( j = DodGC_ParseEntry( s, n, i, cb, user, &emitted )) < 0 ) return emitted;
		i = (size_t)DodGC_JsonSkipWs( s, n, (size_t)j );
		if( i < n && s[i] == ',' ) i = (size_t)DodGC_JsonSkipWs( s, n, i + 1 );
		else if( i >= n || s[i] != ']' ) return emitted;
	}
	return i < n ? emitted : -1;
}

/* ---- heartbeat body ------------------------------------------------------ */

/* Copies src into dst keeping only characters the master accepts, at most max-1. */
static inline void DodGC_MasterClean( char *dst, size_t dstsz, const char *src, size_t max, int allow_space_punct )
{
	size_t o = 0;

	if( !dstsz ) return;
	for( ; src && *src && o < max && o + 1 < dstsz; src++ )
	{
		unsigned char c = (unsigned char)*src;
		int ok = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
		if( allow_space_punct && !ok ) ok = c == ' ' || c == '.' || c == '(' || c == ')' || c == '!' || c == '?';
		dst[o++] = ok ? (char)c : '_';
	}
	dst[o] = 0;
}

/* Builds the POST body. Returns its length or 0 if it does not fit / is invalid. */
static inline size_t DodGC_MasterBuildHeartbeat( char *buf, size_t size, int port, const char *gamedir,
	const char *map, int players, int maxplayers, const char *name )
{
	char gd[20], mp[36], nm[36];
	int n;

	if( port < 1024 || port > 65535 || maxplayers < 1 || maxplayers > 32 || players < 0 || players > maxplayers )
		return 0;
	DodGC_MasterClean( gd, sizeof( gd ), gamedir, 16, 0 );
	DodGC_MasterClean( mp, sizeof( mp ), map, 32, 0 );
	DodGC_MasterClean( nm, sizeof( nm ), name, 32, 1 );
	if( !gd[0] || !mp[0] ) return 0;
	n = snprintf( buf, size, "{\"port\":%d,\"gamedir\":\"%s\",\"map\":\"%s\",\"players\":%d,\"max\":%d,\"name\":\"%s\"}",
		port, gd, mp, players, maxplayers, nm );
	return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

#endif /* DODGC_MASTER_HTTP_H */
