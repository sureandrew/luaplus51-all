/*
 --
 -- KEEPER.C
 --
 -- Keeper state logic
 --
 -- This code is read in for each "keeper state", which are the hidden, inter-
 -- mediate data stores used by Lanes inter-state communication objects.
 --
 -- Author: Benoit Germain <bnt.germain@gmail.com>
 --
 -- C implementation replacement of the original keeper.lua
 --
 --[[
 ===============================================================================

 Copyright (C) 2011-2012 Benoit Germain <bnt.germain@gmail.com>

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.

 ===============================================================================
 ]]--
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#include "lua.h"
#include "lauxlib.h"

#include "threading.h"
#include "tools.h"
#include "keeper.h"

#if KEEPER_MODEL == KEEPER_MODEL_C
//###################################################################################
// Keeper implementation
//###################################################################################

#ifndef __min
#define __min( a, b) (((a) < (b)) ? (a) : (b))
#endif // __min

typedef struct
{
	int first;
	int count;
	int limit;
} keeper_fifo;

// replaces the fifo ud by its uservalue on the stack
static keeper_fifo* prepare_fifo_access( lua_State* L, int idx)
{
	keeper_fifo* fifo = (keeper_fifo*) lua_touserdata( L, idx);
	if( fifo)
	{
		idx = lua_absindex( L, idx);
		STACK_GROW( L, 1);
		// we can replace the fifo userdata in the stack without fear of it being GCed, there are other references around
		lua_getuservalue( L, idx);
		lua_replace( L, idx);
	}
	return fifo;
}

// in: nothing
// out: { first = 1, count = 0, limit = -1}
static void fifo_new( lua_State* L)
{
	keeper_fifo* fifo;
	STACK_GROW( L, 2);
	fifo = (keeper_fifo*) lua_newuserdata( L, sizeof( keeper_fifo));
	fifo->first = 1;
	fifo->count = 0;
	fifo->limit = -1;
	lua_newtable( L);
	lua_setuservalue( L, -2);
}

// in: expect fifo ... on top of the stack
// out: nothing, removes all pushed values on the stack
static void fifo_push( lua_State* L, keeper_fifo* fifo, int _count)
{
	int idx = lua_gettop( L) - _count;
	int start = fifo->first + fifo->count - 1;
	int i;
	// pop all additional arguments, storing them in the fifo
	for( i = _count; i >= 1; -- i)
	{
		// store in the fifo the value at the top of the stack at the specified index, popping it from the stack
		lua_rawseti( L, idx, start + i);
	}
	fifo->count += _count;
}

// in: fifo
// out: ...|nothing
// expects exactly 1 value on the stack!
// currently only called with a count of 1, but this may change in the future
// function assumes that there is enough data in the fifo to satisfy the request
static void fifo_peek( lua_State* L, keeper_fifo* fifo, int _count)
{
	int i;
	STACK_GROW( L, _count);
	for( i = 0; i < _count; ++ i)
	{
		lua_rawgeti( L, 1, fifo->first + i);
	}
}

// in: fifo
// out: pushes as many items as required on the stack (function assumes they exist in sufficient number)
static void fifo_pop( lua_State* L, keeper_fifo* fifo, int _count)
{
	int fifo_idx = lua_gettop( L);           // ... fifo
	int i;
	// each iteration pushes a value on the stack!
	STACK_GROW( L, _count + 1);
	for( i = 0; i < _count; ++ i)
	{
		int at = fifo->first + i;
		lua_rawgeti( L, fifo_idx, at);         // ... fifo val
		lua_pushnil( L);                       // ... fifo val nil
		lua_rawseti( L, fifo_idx, at);         // ... fifo val
	}
	fifo->first += _count;
	fifo->count -= _count;
}

// in: linda_ud expected at *absolute* stack slot idx
// out: fifos[ud]
static void* const fifos_key = (void*) prepare_fifo_access;
static void push_table( lua_State* L, int idx)
{
	STACK_GROW( L, 4);
	STACK_CHECK( L)
	idx = lua_absindex( L, idx);
	lua_pushlightuserdata( L, fifos_key);        // ud fifos_key
	lua_rawget( L, LUA_REGISTRYINDEX);           // ud fifos
	lua_pushvalue( L, idx);                      // ud fifos ud
	lua_rawget( L, -2);                          // ud fifos fifos[ud]
	STACK_MID( L, 2);
	if( lua_isnil( L, -1))
	{
		lua_pop( L, 1);                            // ud fifos
		// add a new fifos table for this linda
		lua_newtable( L);                          // ud fifos fifos[ud]
		lua_pushvalue( L, idx);                    // ud fifos fifos[ud] ud
		lua_pushvalue( L, -2);                     // ud fifos fifos[ud] ud fifos[ud]
		lua_rawset( L, -4);                        // ud fifos fifos[ud]
	}
	lua_remove( L, -2);                          // ud fifos[ud]
	STACK_END( L, 1);
}

// in: linda_ud
int keepercall_clear( lua_State* L)
{
	STACK_GROW( L, 3);
	lua_pushlightuserdata( L, fifos_key);        // ud fifos_key
	lua_rawget( L, LUA_REGISTRYINDEX);           // ud fifos
	lua_pushvalue( L, 1);                        // ud fifos ud
	lua_pushnil( L);                             // ud fifos ud nil
	lua_rawset( L, -3);                          // ud fifos
	lua_pop( L, 1);                              // ud
	return 0;
}


// in: linda_ud, key, ...
// out: true|false
int keepercall_send( lua_State* L)
{
	keeper_fifo* fifo;
	int n = lua_gettop( L) - 2;
	push_table( L, 1);                           // ud key ... fifos
	// get the fifo associated to this key in this linda, create it if it doesn't exist
	lua_pushvalue( L, 2);                        // ud key ... fifos key
	lua_rawget( L, -2);                          // ud key ... fifos fifo
	if( lua_isnil( L, -1))
	{
		lua_pop( L, 1);                            // ud key ... fifos
		fifo_new( L);                              // ud key ... fifos fifo
		lua_pushvalue( L, 2);                      // ud key ... fifos fifo key
		lua_pushvalue( L, -2);                     // ud key ... fifos fifo key fifo
		lua_rawset( L, -4);                        // ud key ... fifos fifo
	}
	lua_remove( L, -2);                          // ud key ... fifo
	fifo = (keeper_fifo*) lua_touserdata( L, -1);
	if( fifo->limit >= 0 && fifo->count + n > fifo->limit)
	{
		lua_settop( L, 0);                         //
		lua_pushboolean( L, 0);                    // false
	}
	else
	{
		fifo = prepare_fifo_access( L, -1);
		lua_replace( L, 2);                        // ud fifo ...
		fifo_push( L, fifo, n);                    // ud fifo
		lua_settop( L, 0);                         //
		lua_pushboolean( L, 1);                    // true
	}
	return 1;
}

// in: linda_ud, key [, key]?
// out: (val, key) or nothing
int keepercall_receive( lua_State* L)
{
	int top = lua_gettop( L);
	int i;
	keeper_fifo* fifo;
	push_table( L, 1);                           // ud keys fifos
	lua_replace( L, 1);                          // fifos keys
	for( i = 2; i <= top; ++ i)
	{
		lua_pushvalue( L, i);                      // fifos keys key[i]
		lua_rawget( L, 1);                         // fifos keys fifo
		fifo = prepare_fifo_access( L, -1);        // fifos keys fifo
		if( fifo && fifo->count > 0)
		{
			fifo_pop( L, fifo, 1);                   // fifos keys fifo val
			if( !lua_isnil( L, -1))
			{
				lua_replace( L, 1);                    // val keys fifo
				if( i != 2)
				{
					lua_pushvalue( L, i);                // val keys fifo key[i]
					lua_replace( L, 2);                  // val key keys fifo
				}
				lua_settop( L, 2);                     // val key
				return 2;
			}
		}
		lua_settop( L, top);                       // data keys
	}
	// nothing to receive
	return 0;
}

//in: linda_ud key mincount [maxcount]
int keepercall_receive_batched( lua_State* L)
{
	int const min_count = (int) lua_tointeger( L, 3);
	if( min_count > 0)
	{
		keeper_fifo* fifo;
		int const max_count = (int) luaL_optinteger( L, 4, min_count);
		lua_settop( L, 2);                                    // ud key
		push_table( L, 1);                                    // ud key fifos
		lua_replace( L, 1);                                   // fifos key
		lua_rawget( L, -2);                                   // fifos fifo
		lua_remove( L, 1);                                    // fifo
		fifo = prepare_fifo_access( L, 1);                    // fifo
		if( fifo && fifo->count >= min_count)
		{
			fifo_pop( L, fifo, __min( max_count, fifo->count)); // fifo ...
		}
		return lua_gettop( L) - 1;
	}
	else
	{
		return 0;
	}
}

// in: linda_ud key n
// out: nothing
int keepercall_limit( lua_State* L)
{
	keeper_fifo* fifo;
	int limit = (int) lua_tointeger( L, 3);
	push_table( L, 1);                                 // ud key n fifos
	lua_replace( L, 1);                                // fifos key n
	lua_pop( L, 1);                                    // fifos key
	lua_pushvalue( L, -1);                             // fifos key key
	lua_rawget( L, -3);                                // fifos key fifo
	fifo = (keeper_fifo*) lua_touserdata( L, -1);
	if( !fifo)
	{
		lua_pop( L, 1);                                  // fifos key
		fifo_new( L);                                    // fifos key fifo
		fifo = (keeper_fifo*) lua_touserdata( L, -1);
		lua_rawset( L, -3);                              // fifos
	}
	fifo->limit = limit;
	return 0;
}

//in: linda_ud key [val]
int keepercall_set( lua_State* L)
{
	// make sure we have a value on the stack
	if( lua_gettop( L) == 2)
	{
		lua_pushnil( L);
	}
	push_table( L, 1);                                // ud key val fifos
	lua_replace( L, 1);                               // fifos key val
	if( !lua_isnil( L, 3)) // replace contents stored at the specified key?
	{
		keeper_fifo* fifo;
		fifo_new( L);                                   // fifos key val fifo
		lua_pushvalue( L, 2);                           // fifos key val fifo key
		lua_pushvalue( L, -2);                          // fifos key val fifo key fifo
		lua_rawset( L, 1);                              // fifos key val fifo
		fifo = prepare_fifo_access( L, -1);
		lua_insert( L, -2);                             // fifos key fifo val
		fifo_push( L, fifo, 1);                         // fifos key fifo
	}
	else
	{
		// val == nil => stack contents: fifos key nil => remove the fifo associated with the current key
		lua_rawset( L, 1);                                // fifos
	}
	return 0;
}

// in: linda_ud key
int keepercall_get( lua_State* L)
{
	keeper_fifo* fifo;
	push_table( L, 1);                                // ud key fifos
	lua_replace( L, 1);                               // fifos key
	lua_rawget( L, 1);                                // fifos fifo
	fifo = prepare_fifo_access( L, -1);               // fifos fifo
	if( fifo && fifo->count > 0)
	{
		lua_remove( L, 1);                              // fifo
		// read one value off the fifo
		fifo_peek( L, fifo, 1);                         // fifo ...
		return 1;
	}
	// no fifo was ever registered for this key, or it is empty
	return 0;
}

// in: linda_ud [, key [, ...]]
int keepercall_count( lua_State* L)
{
	int top;
	push_table( L, 1);                                   // ud keys fifos
	switch( lua_gettop( L))
	{
		// no key is specified: return a table giving the count of all known keys
		case 2:                                            // ud fifos
		lua_newtable( L);                                  // ud fifos out
		lua_replace( L, 1);                                // out fifos
		lua_pushnil( L);                                   // out fifos nil
		while( lua_next( L, 2))                            // out fifos key fifo
		{
			keeper_fifo* fifo = prepare_fifo_access( L, -1); // out fifos key fifo
			lua_pop( L, 1);                                  // out fifos key
			lua_pushvalue( L, -1);                           // out fifos key key
			lua_pushinteger( L, fifo->count);                // out fifos key key count
			lua_rawset( L, -5);                              // out fifos key
		}
		lua_pop( L, 1);                                    // out
		break;

		// 1 key is specified: return its count
		case 3:                                            // ud key fifos
		{
			keeper_fifo* fifo;
			lua_replace( L, 1);                              // fifos key
			lua_rawget( L, -2);                              // fifos fifo
			fifo = prepare_fifo_access( L, -1);              // fifos fifo
			lua_pushinteger( L, fifo->count);                // fifos fifo count
			lua_replace( L, -3);                             // count fifo
			lua_pop( L, 1);                                  // count
		}
		break;

		// a variable number of keys is specified: return a table of their counts
		default:                                           // ud keys fifos
		lua_newtable( L);                                  // ud keys fifos out
		lua_replace( L, 1);                                // out keys fifos
		// shifts all keys up in the stack. potentially slow if there are a lot of them, but then it should be bearable
		lua_insert( L, 2);                                 // out fifos keys
		while( (top = lua_gettop( L)) > 2)
		{
			keeper_fifo* fifo;
			lua_pushvalue( L, -1);                           // out fifos keys key
			lua_rawget( L, 2);                               // out fifos keys fifo
			fifo = prepare_fifo_access( L, -1);              // out fifos keys fifo
			lua_pop( L, 1);                                  // out fifos keys
			if( fifo)
			{
				lua_pushinteger( L, fifo->count);              // out fifos keys count
				lua_rawset( L, 1);                             // out fifos keys
			}
			else
			{
				lua_pop( L, 1);                                // out fifos keys
			}
		}
		lua_pop( L, 1);                                    // out
	}
	return 1;
}
#endif // KEEPER_MODEL == KEEPER_MODEL_C

//###################################################################################
// Keeper API, accessed from linda methods
//###################################################################################

/*---=== Keeper states ===---
*/

/*
* Pool of keeper states
*
* Access to keeper states is locked (only one OS thread at a time) so the 
* bigger the pool, the less chances of unnecessary waits. Lindas map to the
* keepers randomly, by a hash.
*/
static struct s_Keeper *GKeepers = NULL;
static int GNbKeepers = 0;

static void atexit_close_keepers(void)
{
	int i;
	// 2-pass close, in case a keeper holds a reference to a linda bound to another keeoer
	for( i = 0; i < GNbKeepers; ++ i)
	{
		lua_State* L = GKeepers[i].L;
		GKeepers[i].L = 0;
		lua_close( L);
	}
	for( i = 0; i < GNbKeepers; ++ i)
	{
		MUTEX_FREE( &GKeepers[i].lock_);
	}
	if( GKeepers) free( GKeepers);
	GKeepers = NULL;
	GNbKeepers = 0;
}

/*
* Initialize keeper states
*
* If there is a problem, return an error message (NULL for okay).
*
* Note: Any problems would be design flaws; the created Lua state is left
*       unclosed, because it does not really matter. In production code, this
*       function never fails.
*/
char const* init_keepers( int const _nbKeepers, lua_CFunction _on_state_create)
{
	int i;
	assert( _nbKeepers >= 1);
	GNbKeepers = _nbKeepers;
	GKeepers = malloc( _nbKeepers * sizeof( struct s_Keeper));
	for( i = 0; i < _nbKeepers; ++ i)
	{

		// Initialize Keeper states with bare minimum of libs (those required by 'keeper.lua')
		// 
		// 'io' for debugging messages, 'package' because we need to require modules exporting idfuncs
		// the others because they export functions that we may store in a keeper for transfer between lanes
		lua_State* K = luaG_newstate( "*", _on_state_create);
		if (!K)
			return "out of memory";

		STACK_CHECK( K)
		// to see VM name in Decoda debugger
		lua_pushliteral( K, "Keeper #");
		lua_pushinteger( K, i + 1);
		lua_concat( K, 2);
		lua_setglobal( K, "decoda_name");

#if KEEPER_MODEL == KEEPER_MODEL_C
		// create the fifos table in the keeper state
		lua_pushlightuserdata( K, fifos_key);
		lua_newtable( K);
		lua_rawset( K, LUA_REGISTRYINDEX);
#endif // KEEPER_MODEL == KEEPER_MODEL_C

#if KEEPER_MODEL == KEEPER_MODEL_LUA
		// use package.loaders[2] to find keeper microcode
		lua_getglobal( K, "package");                  // package
		lua_getfield( K, -1, "loaders");               // package package.loaders
		lua_rawgeti( K, -1, 2);                        // package package.loaders package.loaders[2]
		lua_pushliteral( K, "lanes-keeper");           // package package.loaders package.loaders[2] "lanes-keeper"
		STACK_MID( K, 4);
		// first pcall loads lanes-keeper.lua, second one runs the chunk
		if( lua_pcall( K, 1 /*args*/, 1 /*results*/, 0 /*errfunc*/) || lua_pcall( K, 0 /*args*/, 0 /*results*/, 0 /*errfunc*/))
		{
			// LUA_ERRRUN / LUA_ERRMEM / LUA_ERRERR
			//
			char const* err = lua_tostring( K, -1);
			assert( err);
			return err;
		}                                              // package package.loaders
		STACK_MID( K, 2);
		lua_pop( K, 2);
#endif // KEEPER_MODEL == KEEPER_MODEL_LUA
		STACK_END( K, 0)
		MUTEX_INIT( &GKeepers[i].lock_);
		GKeepers[i].L = K;
		//GKeepers[i].count = 0;
	}
	// call close_keepers at the very last as we want to be sure no thread is GCing after.
	// (and therefore may perform linda object dereferencing after keepers are gone)
	atexit( atexit_close_keepers);
	return NULL;    // ok
}

// cause each keeper state to populate its database of transferable functions with those from the specified module
void populate_keepers( lua_State *L)
{
	size_t name_len;
	char const *name = luaL_checklstring( L, -1, &name_len);
	size_t package_path_len;
	char const *package_path;
	size_t package_cpath_len;
	char const *package_cpath;
	int i;

	// we need to make sure that package.path & package.cpath are the same in the keepers
// than what is currently in use when the module is required in the caller's Lua state
	STACK_CHECK(L)
	STACK_GROW( L, 3);
	lua_getglobal( L, "package");
	lua_getfield( L, -1, "path");
	package_path = luaL_checklstring( L, -1, &package_path_len);
	lua_getfield( L, -2, "cpath");
	package_cpath = luaL_checklstring( L, -1, &package_cpath_len);

	for( i = 0; i < GNbKeepers; ++ i)
	{
		lua_State *K = GKeepers[i].L;
		int res;
		MUTEX_LOCK( &GKeepers[i].lock_);
		STACK_CHECK(K)
		STACK_GROW( K, 2);
		lua_getglobal( K, "package");
		lua_pushlstring( K, package_path, package_path_len);
		lua_setfield( K, -2, "path");
		lua_pushlstring( K, package_cpath, package_cpath_len);
		lua_setfield( K, -2, "cpath");
		lua_pop( K, 1);
		lua_getglobal( K, "require");
		lua_pushlstring( K, name, name_len);
		res = lua_pcall( K, 1, 0, 0);
		if( res != 0)
		{
			char const *err = luaL_checkstring( K, -1);
			luaL_error( L, "error requiring '%s' in keeper state: %s", name, err);
		}
		STACK_END(K, 0)
		MUTEX_UNLOCK( &GKeepers[i].lock_);
	}
	lua_pop( L, 3);
	STACK_END(L, 0)
}

struct s_Keeper *keeper_acquire( const void *ptr)
{
	// can be 0 if this happens during main state shutdown (lanes is being GC'ed -> no keepers)
	if( GNbKeepers == 0)
	{
		return NULL;
	}
	else
	{
		/*
		* Any hashing will do that maps pointers to 0..GNbKeepers-1 
		* consistently.
		*
		* Pointers are often aligned by 8 or so - ignore the low order bits
		*/
		unsigned int i= ((uintptr_t)(ptr) >> 3) % GNbKeepers;
		struct s_Keeper *K= &GKeepers[i];

		MUTEX_LOCK( &K->lock_);
		//++ K->count;
		return K;
	}
}

void keeper_release( struct s_Keeper *K)
{
	//-- K->count;
	if( K) MUTEX_UNLOCK( &K->lock_);
}

void keeper_toggle_nil_sentinels( lua_State *L, int _val_i, int _nil_to_sentinel)
{
	int i, n = lua_gettop( L);
	/* We could use an empty table in 'keeper.lua' as the sentinel, but maybe
	* checking for a lightuserdata is faster. (any unique value will do -> take the address of some global of ours)
	*/
	void *nil_sentinel = &GNbKeepers;
	for( i = _val_i; i <= n; ++ i)
	{
		if( _nil_to_sentinel)
		{
			if( lua_isnil( L, i))
			{
				lua_pushlightuserdata( L, nil_sentinel);
				lua_replace( L, i);
			}
		}
		else
		{
			if( lua_touserdata( L, i) == nil_sentinel)
			{
				lua_pushnil( L);
				lua_replace( L, i);
			}
		}
	}
}

/*
* Call a function ('func_name') in the keeper state, and pass on the returned
* values to 'L'.
*
* 'linda':          deep Linda pointer (used only as a unique table key, first parameter)
* 'starting_index': first of the rest of parameters (none if 0)
*
* Returns: number of return values (pushed to 'L') or -1 in case of error
*/
int keeper_call( lua_State *K, keeper_api_t _func, lua_State *L, void *linda, uint_t starting_index)
{
	int const args = starting_index ? (lua_gettop( L) - starting_index + 1) : 0;
	int const Ktos = lua_gettop( K);
	int retvals = -1;

	STACK_GROW( K, 2);

	PUSH_KEEPER_FUNC( K, _func);

	lua_pushlightuserdata( K, linda);

	if( (args == 0) || luaG_inter_copy( L, K, args) == 0) // L->K
	{
		lua_call( K, 1 + args, LUA_MULTRET);

		retvals = lua_gettop( K) - Ktos;
		if( (retvals > 0) && luaG_inter_move( K, L, retvals) != 0) // K->L
		{
			retvals = -1;
		}
	}
	// whatever happens, restore the stack to where it was at the origin
	lua_settop( K, Ktos);
	return retvals;
}
