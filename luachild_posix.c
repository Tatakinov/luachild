/*
This software is licensed under the terms of the MIT license reproduced below.

===============================================================================

Copyright 2006-2007 Mark Edgar <medgar123@gmail.com>

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

*/

#include "luachild.h"
#ifdef USE_POSIX

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <fcntl.h>
#include <sys/wait.h>

#include <dirent.h>
#include <sys/stat.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#if __STDC_VERSION__ < 199901L
#define restrict
#endif

#ifdef __APPLE__

#include <crt_externs.h>
#define environ (*_NSGetEnviron())

#else

extern char **environ;

#endif

#define absindex(L,i) ((i)>0?(i):lua_gettop(L)+(i)+1)

#ifndef OPEN_MAX
#define OPEN_MAX sysconf(_SC_OPEN_MAX)
#endif

/* -- nil error */
extern int push_error(lua_State *L)
{
  lua_pushnil(L);
  lua_pushstring(L, strerror(errno));
  return 2;
}

/* ----------------------------------------------------------------------------- */

/* name value -- true/nil error
 * name nil -- true/nil error */
int lc_setenv(lua_State *L)
{
  const char *nam = luaL_checkstring(L, 1);
  const char *val = lua_tostring(L, 2);
  int err = val ? setenv(nam, val, 1) : unsetenv(nam);
  if (err == -1) return push_error(L);
  lua_pushboolean(L, 1);
  return 1;
}

/* -- environment-table */
int lc_environ(lua_State *L)
{
  const char *nam, *val, *end;
  const char **env;
  lua_newtable(L);
  for (env = (const char **)environ; (nam = *env); env++) {
    end = strchr(val = strchr(nam, '=') + 1, '\0');
    lua_pushlstring(L, nam, val - nam - 1);
    lua_pushlstring(L, val, end - val);
    lua_settable(L, -3);
  }
  return 1;
}

/* -- pathname/nil error */
int lc_currentdir(lua_State *L)
{
  char pathname[PATH_MAX + 1];
  if (!getcwd(pathname, sizeof pathname))
    return push_error(L);
  lua_pushstring(L, pathname);
  return 1;
}

/* pathname -- true/nil error */
int lc_chdir(lua_State *L)
{
  const char *pathname = luaL_checkstring(L, 1);
  if (-1 == chdir(pathname))
    return push_error(L);
  lua_pushboolean(L, 1);
  return 1;
}

static int closeonexec(int d)
{
  int fl = fcntl(d, F_GETFD);
  if (fl != -1)
    fl = fcntl(d, F_SETFD, fl | FD_CLOEXEC);
  return fl;
}

/* -- in out/nil error */
int lc_pipe(lua_State *L)
{
  if (!file_handler_creator(L, "/dev/null", 0)) return 0;
  int fd[2];
  if (-1 == pipe(fd))
    return push_error(L);
  closeonexec(fd[0]);
  closeonexec(fd[1]);
  lua_pushcfile(L, fdopen(fd[0], "r"));
  lua_pushcfile(L, fdopen(fd[1], "w"));
  return 2;
}

/* ----------------------------------------------------------------------------- */

#ifndef INTERNAL_SPAWN_API
#include <spawn.h>
#else

typedef void *posix_spawnattr_t;

typedef struct posix_spawn_file_actions posix_spawn_file_actions_t;
struct posix_spawn_file_actions {
  int dups[3];
};

static int posix_spawn_file_actions_destroy(
  posix_spawn_file_actions_t *act)
{
  return 0;
}

static int posix_spawn_file_actions_adddup2(
  posix_spawn_file_actions_t *act,
  int d,
  int n)
{
  /* good faith effort to determine validity of descriptors */
  if (d < 0 || OPEN_MAX < d || n < 0 || OPEN_MAX < n) {
    errno = EBADF;
    return -1;
  }
  /* we only support duplication to 0,1,2 */
  if (2 < n) {
    errno = EINVAL;
    return -1;
  }
  act->dups[n] = d;
  return 0;
}

static int posix_spawn_file_actions_init(
  posix_spawn_file_actions_t *act)
{
  act->dups[0] = act->dups[1] = act->dups[2] = -1;
  return 0;
}

static int posix_spawnp(
  pid_t *restrict ppid,
  const char *restrict path,
  const posix_spawn_file_actions_t *act,
  const posix_spawnattr_t *restrict attrp,
  char *const argv[restrict],
  char *const envp[restrict])
{
  if (!ppid || !path || !argv || !envp)
    return EINVAL;
  if (attrp)
    return EINVAL;
  switch (*ppid = fork()) {
  case -1: return -1;
  default: return 0;
  case 0:
    if (act) {
      int i;
      for (i = 0; i < 3; i++)
        if (act->dups[i] != -1 && -1 == dup2(act->dups[i], i))
          _exit(111);
    }
    environ = (char **)envp;
    execvp(path, argv);
    _exit(111);
    /*NOTREACHED*/
  }
}

#endif // INTERNAL_SPAWN_API

struct process {
  int status;
  pid_t pid;
};

int _process_wait(struct process *p, int blocking, int *status);
int _process_terminate(struct process *p);

int _process_terminate(struct process *p) {
  if (p->status == -1) {
    return kill(p->pid, SIGTERM);
  }
  return 0;
}

/* proc -- */
int process_terminate(lua_State *L)
{
  struct process *p = luaL_checkudata(L, 1, PROCESS_HANDLE);
  if (-1 == _process_terminate(p)) {
    return push_error(L);
  }
  lua_pushboolean(L, 1);
  return 1;
}

int _process_wait(struct process *p, int blocking, int *status) {
  int options = (blocking) ? (0) : (WNOHANG);
  int ret = waitpid(p->pid, status, options);
  return ret;
}

/* proc [blocking] -- exitcode/true timeout/nil error */
int process_wait(lua_State *L)
{
  struct process *p = luaL_checkudata(L, 1, PROCESS_HANDLE);
  int blocking  = 1;                    /* true */
  int status;
  if (lua_isboolean(L, 2)) {
    blocking  = lua_toboolean(L, 2);
  }
  if (p->status == -1) {
    int ret = _process_wait(p, blocking, &status);
    if (-1 == ret) {
      return push_error(L);
    }
    else if (0 == ret) {
      lua_pushboolean(L, 1);
      return 1;
    }
    p->status = WEXITSTATUS(status);
  }
  lua_pushnumber(L, p->status);
  return 1;
}

/* proc -- nil */
int process_gc(lua_State *L) {
  struct process *p = luaL_checkudata(L, 1, PROCESS_HANDLE);
  int status;
  _process_terminate(p);
  _process_wait(p, 1, &status);
  return 0;
}

/* proc -- string */
int process_tostring(lua_State *L)
{
  struct process *p = luaL_checkudata(L, 1, PROCESS_HANDLE);
  char buf[40];
  lua_pushlstring(L, buf,
    sprintf(buf, "process (%lu, %s)", (unsigned long)p->pid,
      p->status==-1 ? "running" : "terminated"));
  return 1;
}

struct spawn_params {
  lua_State *L;
  const char *command, **argv, **envp;
  posix_spawn_file_actions_t redirect;
};

struct spawn_params *spawn_param_init(lua_State *L)
{
  struct spawn_params *p = lua_newuserdata(L, sizeof *p);
  p->L = L;
  p->command = 0;
  p->argv = p->envp = 0;
  posix_spawn_file_actions_init(&p->redirect);
  return p;
}

static void spawn_param_filename(struct spawn_params *p, const char *filename)
{
  p->command = filename;
}

static void spawn_param_redirect(struct spawn_params *p, const char *stdname, int fd)
{
  int d;
  switch (stdname[3]) {
  case 'i': d = STDIN_FILENO; break;
  case 'o': d = STDOUT_FILENO; break;
  case 'e': d = STDERR_FILENO; break;
  }
  posix_spawn_file_actions_adddup2(&p->redirect, fd, d);
}

static int spawn_param_execute(struct spawn_params *p)
{
  lua_State *L = p->L;
  int ret;
  struct process *proc;
  if (!p->argv) {
    p->argv = lua_newuserdata(L, 2 * sizeof *p->argv);
    p->argv[0] = p->command;
    p->argv[1] = 0;
  }
  if (!p->envp)
    p->envp = (const char **)environ;
  proc = lua_newuserdata(L, sizeof *proc);
  luaL_getmetatable(L, PROCESS_HANDLE);
  lua_setmetatable(L, -2);
  proc->status = -1;
  ret = posix_spawnp(&proc->pid, p->command, &p->redirect, 0,
                     (char *const *)p->argv, (char *const *)p->envp);
  posix_spawn_file_actions_destroy(&p->redirect);
  return ret != 0 ? push_error(L) : 1;
}

/* Converts a Lua array of strings to a null-terminated array of char pointers.
 * Pops a (0-based) Lua array and replaces it with a userdatum which is the
 * null-terminated C array of char pointers.  The elements of this array point
 * to the strings in the Lua array.  These strings should be associated with
 * this userdatum via a weak table for GC purposes, but they are not here.
 * Therefore, any function which calls this must make sure that these strings
 * remain available until the userdatum is thrown away.
 */
/* ... array -- ... vector */
static const char **make_vector(lua_State *L)
{
  size_t i, n = lua_value_length(L, -1);
  const char **vec = lua_newuserdata(L, (n + 2) * sizeof *vec);
                                        /* ... arr vec */
  for (i = 0; i <= n; i++) {
    lua_rawgeti(L, -2, i);              /* ... arr vec elem */
    vec[i] = lua_tostring(L, -1);
    if (!vec[i] && i > 0) {
      luaL_error(L, "expected string for argument %d, got %s",
                 i, lua_typename(L, lua_type(L, -1)));
      return 0;
    }
    lua_pop(L, 1);                      /* ... arr vec */
  }
  vec[n + 1] = 0;
  lua_replace(L, -2);                   /* ... vector */
  return vec;
}

/* ... envtab -- ... envtab vector */
static void spawn_param_env(struct spawn_params *p)
{
  lua_State *L = p->L;
  size_t i = 0;
  lua_newtable(L);                      /* ... envtab arr */
  lua_pushliteral(L, "=");              /* ... envtab arr "=" */
  lua_pushnil(L);                       /* ... envtab arr "=" nil */
  for (i = 0; lua_next(L, -4); i++) {   /* ... envtab arr "=" k v */
    if (!lua_tostring(L, -2)) {
      luaL_error(L, "expected string for environment variable name, got %s",
                 lua_typename(L, lua_type(L, -2)));
      return;
    }
    if (!lua_tostring(L, -1)) {
      luaL_error(L, "expected string for environment variable value, got %s",
                 lua_typename(L, lua_type(L, -1)));
      return;
    }
    lua_pushvalue(L, -2);               /* ... envtab arr "=" k v k */
    lua_pushvalue(L, -4);               /* ... envtab arr "=" k v k "=" */
    lua_pushvalue(L, -3);               /* ... envtab arr "=" k v k "=" v */
    lua_concat(L, 3);                   /* ... envtab arr "=" k v "k=v" */
    lua_rawseti(L, -5, i);              /* ... envtab arr "=" k v */
    lua_pop(L, 1);                      /* ... envtab arr "=" k */
  }                                     /* ... envtab arr "=" */
  lua_pop(L, 1);                        /* ... envtab arr */
  p->envp = make_vector(L);             /* ... envtab arr vector */
}

/* ... argtab -- ... argtab vector */
static void spawn_param_args(struct spawn_params *p)
{
  const char **argv = make_vector(p->L);
  if (!argv[0]) argv[0] = p->command;
  p->argv = argv;
}

static FILE *check_file(lua_State *L, int idx, const char *argname)
{
  FILE **pf;
  if (idx > 0) pf = luaL_checkudata(L, idx, LUA_FILEHANDLE);
  else {
    idx = absindex(L, idx);
    pf = lua_touserdata(L, idx);
    luaL_getmetatable(L, LUA_FILEHANDLE);
    if (!pf || !lua_getmetatable(L, idx) || !lua_rawequal(L, -1, -2))
      luaL_error(L, "bad %s option (%s expected, got %s)",
                 argname, LUA_FILEHANDLE, luaL_typename(L, idx));
    lua_pop(L, 2);
  }
  if (!*pf) return luaL_error(L, "attempt to use a closed file"), NULL;
  return *pf;
}

#define new_dirent(L) lua_newtable(L)

static void get_redirect(lua_State *L,
                         int idx, const char *stdname, struct spawn_params *p)
{
  lua_getfield(L, idx, stdname);
  if (!lua_isnil(L, -1))
    spawn_param_redirect(p, stdname, fileno(check_file(L, -1, stdname)));
  lua_pop(L, 1);
}

/* filename [args-opts] -- proc/nil error */
/* args-opts -- proc/nil error */
int lc_spawn(lua_State *L)
{
  struct spawn_params *params;
  int have_options;
  switch (lua_type(L, 1)) {
  default: return lua_report_type_error(L, 1, "string or table");
  case LUA_TSTRING:
    switch (lua_type(L, 2)) {
    default: return lua_report_type_error(L, 2, "table");
    case LUA_TNONE: have_options = 0; break;
    case LUA_TTABLE: have_options = 1; break;
    }
    break;
  case LUA_TTABLE:
    have_options = 1;
    lua_getfield(L, 1, "command");      /* opts ... cmd */
    if (!lua_isnil(L, -1)) {
      /* convert {command=command,arg1,...} to command {arg1,...} */
      lua_insert(L, 1);                 /* cmd opts ... */
    }
    else {
      /* convert {arg0,arg1,...} to arg0 {arg1,...} */
      size_t i, n = lua_value_length(L, 1);
      lua_rawgeti(L, 1, 1);             /* opts ... nil cmd */
      lua_insert(L, 1);                 /* cmd opts ... nil */
      for (i = 2; i <= n; i++) {
        lua_rawgeti(L, 2, i);           /* cmd opts ... nil argi */
        lua_rawseti(L, 2, i - 1);       /* cmd opts ... nil */
      }
      lua_rawseti(L, 2, n);             /* cmd opts ... */
    }
    if (lua_type(L, 1) != LUA_TSTRING)
      return luaL_error(L, "bad command option (string expected, got %s)",
                        luaL_typename(L, 1));
    break;
  }
  params = spawn_param_init(L);
  /* get filename to execute */
  spawn_param_filename(params, lua_tostring(L, 1));
  /* get arguments, environment, and redirections */
  if (have_options) {
    lua_getfield(L, 2, "args");         /* cmd opts ... argtab */
    switch (lua_type(L, -1)) {
    default:
      return luaL_error(L, "bad args option (table expected, got %s)",
                        luaL_typename(L, -1));
    case LUA_TNIL:
      lua_pop(L, 1);                    /* cmd opts ... */
      lua_pushvalue(L, 2);              /* cmd opts ... opts */
      if (0) /*FALLTHRU*/
    case LUA_TTABLE:
      if (lua_value_length(L, 2) > 0)
        return
          luaL_error(L, "cannot specify both the args option and array values");
      spawn_param_args(params);         /* cmd opts ... */
      break;
    }
    lua_getfield(L, 2, "env");          /* cmd opts ... envtab */
    switch (lua_type(L, -1)) {
    default:
      return luaL_error(L, "bad env option (table expected, got %s)",
                        luaL_typename(L, -1));
    case LUA_TNIL:
      break;
    case LUA_TTABLE:
      spawn_param_env(params);          /* cmd opts ... */
      break;
    }
    get_redirect(L, 2, "stdin", params);    /* cmd opts ... */
    get_redirect(L, 2, "stdout", params);   /* cmd opts ... */
    get_redirect(L, 2, "stderr", params);   /* cmd opts ... */
  }
  return spawn_param_execute(params);   /* proc/nil error */
}

#define new_dirent(L) lua_newtable(L)

/* pathname/file [entry] -- entry */
int lc_dirent(lua_State *L)
{
  struct stat st;
  switch (lua_type(L, 1)) {
  default: return luaL_error(L, "expected file or pathname for argument %d, got dirent", 1);
  case LUA_TSTRING: {
    const char *name = lua_tostring(L, 1);
    if (-1 == stat(name, &st))
      return push_error(L);
    } break;
  case LUA_TUSERDATA: {
    FILE *f = check_file(L, 1, NULL);
    if (-1 == fstat(fileno(f), &st))
      return push_error(L);
    } break;
  }
  if (lua_type(L, 2) != LUA_TTABLE) {
    lua_settop(L, 1);
    new_dirent(L);
  }
  else {
    lua_settop(L, 2);
  }
  if (S_ISDIR(st.st_mode))
    lua_pushliteral(L, "directory");
  else
    lua_pushliteral(L, "file");
  lua_setfield(L, 2, "type");
  lua_pushnumber(L, st.st_size);
  lua_setfield(L, 2, "size");
  return 1;
}

/* ...diriter... -- ...diriter... pathname */
static int diriter_getpathname(lua_State *L, int index)
{
  lua_pushvalue(L, index);
  lua_gettable(L, LUA_REGISTRYINDEX);
  return 1;
}

/* ...diriter... pathname -- ...diriter... */
static int diriter_setpathname(lua_State *L, int index)
{
  size_t len;
  const char *path = lua_tolstring(L, -1, &len);
  if (path && path[len - 1] != *LUA_DIRSEP) {
    lua_pushliteral(L, LUA_DIRSEP);
    lua_concat(L, 2);
  }
  lua_pushvalue(L, index);              /* ... pathname diriter */
  lua_insert(L, -2);                    /* ... diriter pathname */
  lua_settable(L, LUA_REGISTRYINDEX);   /* ... */
  return 0;
}

/* diriter -- diriter */
static int diriter_close(lua_State *L)
{
  DIR **pd = lua_touserdata(L, 1);
  if (*pd) {
    closedir(*pd);
    *pd = 0;
  }
  lua_pushnil(L);
  diriter_setpathname(L, 1);
  return 0;
}

static int isdotfile(const char *name)
{
  return name[0] == '.' && (name[1] == '\0'
         || (name[1] == '.' && name[2] == '\0'));
}

/* pathname -- iter state nil */
/* diriter ... -- entry */
int lc_dir(lua_State *L)
{
  const char *pathname;
  DIR **pd;
  struct dirent *d;
  switch (lua_type(L, 1)) {
  default: return luaL_error(L, "expected pathname for argument %d, got dir", 1);
  case LUA_TSTRING:
    pathname = lua_tostring(L, 1);
    lua_pushcfunction(L, lc_dir);       /* pathname ... iter */
    pd = lua_newuserdata(L, sizeof *pd);/* pathname ... iter state */
    *pd = opendir(pathname);
    if (!*pd) return push_error(L);
    luaL_getmetatable(L, DIR_HANDLE);   /* pathname ... iter state M */
    lua_setmetatable(L, -2);            /* pathname ... iter state */
    lua_pushvalue(L, 1);                /* pathname ... iter state pathname */
    diriter_setpathname(L, -2);         /* pathname ... iter state */
    return 2;
  case LUA_TUSERDATA:
    pd = luaL_checkudata(L, 1, DIR_HANDLE);
    do d = readdir(*pd);
    while (d && isdotfile(d->d_name));
    if (!d) { diriter_close(L); return push_error(L); }
    new_dirent(L);                      /* diriter ... entry */
    diriter_getpathname(L, 1);          /* diriter ... entry dir */
    lua_pushstring(L, d->d_name);       /* diriter ... entry dir name */
    lua_pushvalue(L, -1);               /* diriter ... entry dir name name */
    lua_setfield(L, -4, "name");        /* diriter ... entry dir name */
    lua_concat(L, 2);                   /* diriter ... entry fullpath */
    lua_replace(L, 1);                  /* fullpath ... entry */
    lua_replace(L, 2);                  /* fullpath entry ... */
    return lc_dirent(L);
  }
  /*NOTREACHED*/
}

#endif // USE_POSIX

