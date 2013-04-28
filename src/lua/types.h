
/*
    This file is part of darktable,
    copyright (c) 2012 Jeremy Rosen

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef DT_LUA_TYPES_H
#define DT_LUA_TYPES_H
#include <lualib.h>
#include <lua.h>
#include <lauxlib.h>
#include <lautoc.h>

/**
  these defines can be used with luaA_struct_member to have checks on read added
  */
typedef char* char_20;
typedef char* char_32;
typedef char* char_52;
typedef char* char_1024;
typedef char* char_filename_length;
typedef char* char_path_length;



/**
  (0,0)
  register a C type to the dt-lua subsystem

  the type can be converted to/from C using the usual luaA functions.
  the type becomes a full userdata (i.e malloc+memcpy then pushed on the lua stack, released when not referenced in lua)
  you can use luaL_checkudata to get and check the data from the stack

  the following metamethods are defined for the type
 * __luaA_TypeName : string with the associated C type
 * __luaA_Type : int, the associated luaA_Type
 * __pairs : will retun (__next,obj,nil)
 * __next : will iteratethrough the __get table of obj
 * __index : will look into the __get table to find a callback, then for a __default_index in its metatable then raise an error
 * __newindex : will look into the __set table to find a callback, then for a __default_newindex in its metatable then raise an error
 * __get : empty table, contains getters, similar API to __index
 * __set : empty table, contains setters, similar API to __newindex

   */
   
#define dt_lua_init_type(L,type_name) \
  dt_lua_init_type_internal(L,#type_name,sizeof(type_name)) 
luaA_Type dt_lua_init_type_internal(lua_State* L,const char*type_name,size_t size);

/* helper functions to register index hanlers
   each one follow the same logic, you give an index, optionally a newindex and a list of entries it can handle
   * using a NULL terminated list as a vararg
   * using a NULL terminated array of entries
   * using a struct, which will register all its luaA-known members.
     for that last one, if index and newindex are NULL an automated one based on lautoc will be used
   * setting default for unhandled entries (won't be seen by __next)
   */
#define dt_lua_register_type_callback(L,type_name,index,newindex,...) \
  dt_lua_register_type_callback_internal(L,#type_name,index,newindex,__VA_ARGS__)
void dt_lua_register_type_callback_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex,...);
#define dt_lua_register_type_callback_list(L,type_name,index,newindex,name_list) \
  dt_lua_register_type_callback_list_internal(L,#type_name,index,newindex,name_list)
void dt_lua_register_type_callback_list_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex,const char**list);
#define dt_lua_register_type_callback_default(L,type_name,index,newindex) \
  dt_lua_register_type_callback_default_internal(L,#type_name,index,newindex)
void dt_lua_register_type_callback_default_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex);
#define dt_lua_register_type_callback_type(L,type_name,index,newindex,struct_type_name) \
  dt_lua_register_type_callback_type_internal(L,#type_name,index,newindex,#struct_type_name)
void dt_lua_register_type_callback_type_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex,const char* struct_type_name);

void dt_lua_register_type_struct();

void dt_lua_initialize_types(lua_State *L);



int dt_lua_autotype_next(lua_State *L);
int dt_lua_autotype_pairs(lua_State *L);
int dt_lua_autotype_index(lua_State *L);
int dt_lua_autotype_newindex(lua_State *L);
int autotype_full_pushfunc(lua_State *L, luaA_Type type_id, const void *cin);
void dt_lua_autotype_tofunc(lua_State*L, luaA_Type type_id, void* cout, int index);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
