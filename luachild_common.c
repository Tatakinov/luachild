
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include "luachild.h"

int set_table_field(lua_State *L, const char * field_name){
  lua_pushstring(L, field_name);
  lua_insert(L, -2);
  lua_settable(L, -3);
  return 0;
}

SHFUNC int luaopen_luachild(lua_State *L)
{

  /* Dirent methods */
  luaL_newmetatable(L, DIR_HANDLE);
  
  /* Process methods */

  luaL_newmetatable(L, PROCESS_HANDLE);

  lua_pushcfunction(L, process_tostring);
  set_table_field(L, "__tostring");

  lua_pushcfunction(L, process_gc);
  set_table_field(L, "__gc");

  lua_pushcfunction(L, process_terminate);
  set_table_field(L, "terminate");

  lua_pushcfunction(L, process_wait);
  set_table_field(L, "wait");

  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");

  /* Top module functions */

  lua_newtable(L);
 
  lua_pushcfunction(L, lc_pipe);
  set_table_field(L, "pipe");

  lua_pushcfunction(L, lc_setenv);
  set_table_field(L, "setenv");

  lua_pushcfunction(L, lc_environ);
  set_table_field(L, "environ");

  lua_pushcfunction(L, lc_currentdir);
  set_table_field(L, "currentdir");

  lua_pushcfunction(L, lc_chdir);
  set_table_field(L, "chdir");

  lua_pushcfunction(L, lc_dirent);
  set_table_field(L, "dirent");

  lua_pushcfunction(L, lc_dir);
  set_table_field(L, "dir");

  lua_pushcfunction(L, lc_spawn);
  set_table_field(L, "spawn");

  lua_pushcfunction(L, process_terminate);
  set_table_field(L, "terminate");

  lua_pushcfunction(L, process_wait);
  set_table_field(L, "wait");

  return 1;
}

