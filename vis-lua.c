#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>

#include "vis-lua.h"
#include "vis-core.h"
#include "text-motions.h"

#if !CONFIG_LUA

void vis_lua_start(Vis *vis) { }
void vis_lua_quit(Vis *vis) { }
void vis_lua_file_open(Vis *vis, File *file) { }
void vis_lua_file_save(Vis *vis, File *file) { }
void vis_lua_file_close(Vis *vis, File *file) { }
void vis_lua_win_open(Vis *vis, Win *win) { }
void vis_lua_win_close(Vis *vis, Win *win) { }
bool vis_theme_load(Vis *vis, const char *name) { return true; }

#else

#if 0
static void stack_dump_entry(lua_State *L, int i) {
	int t = lua_type(L, i);
	switch (t) {
	case LUA_TNIL:
		printf("nil");
		break;
	case LUA_TBOOLEAN:
		printf(lua_toboolean(L, i) ? "true" : "false");
		break;
	case LUA_TLIGHTUSERDATA:
		printf("lightuserdata(%p)", lua_touserdata(L, i));
		break;
	case LUA_TNUMBER:
		printf("%g", lua_tonumber(L, i));
		break;
	case LUA_TSTRING:
		printf("`%s'", lua_tostring(L, i));
		break;
	case LUA_TTABLE:
		printf("table[");
		lua_pushnil(L); /* first key */
		while (lua_next(L, i > 0 ? i : i - 1)) {
			stack_dump_entry(L, -2);
			printf("=");
			stack_dump_entry(L, -1);
			printf(",");
			lua_pop(L, 1); /* remove value, keep key */
		}
		printf("]");
		break;
	case LUA_TUSERDATA:
		printf("userdata(%p)", lua_touserdata(L, i));
		break;
	default:  /* other values */
		printf("%s", lua_typename(L, t));
		break;
	}
}

static void stack_dump(lua_State *L, const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	int top = lua_gettop(L);
	for (int i = 1; i <= top; i++) {
		printf("%d: ", i);
		stack_dump_entry(L, i);
		printf("\n");
	}
	printf("\n\n");
}

#endif

/* returns registry["vis.objects"][addr] if it is of correct type */
static void *obj_get(lua_State *L, void *addr, const char *type) {
	lua_getfield(L, LUA_REGISTRYINDEX, "vis.objects");
	lua_pushlightuserdata(L, addr);
	lua_gettable(L, -2);
	lua_remove(L, -2);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return NULL;
	}
	return luaL_checkudata(L, -1, type);
}

/* expects a userdatum at the top of the stack and sets
 *
 *   registry["vis.objects"][addr] = userdata
 */
static void obj_set(lua_State *L, void *addr) {
	lua_getfield(L, LUA_REGISTRYINDEX, "vis.objects");
	lua_pushlightuserdata(L, addr);
	lua_pushvalue(L, -3);
	lua_settable(L, -3);
	lua_pop(L, 1);
}

static void obj_del(lua_State *L, void *addr) {
	lua_pushnil(L);
	obj_set(L, addr);
}

static void *obj_new(lua_State *L, void *addr, const char *type) {
	if (!addr)
		return NULL;
	void **handle = (void**)obj_get(L, addr, type);
	if (!handle) {
		handle = lua_newuserdata(L, sizeof(addr));
		obj_set(L, addr);
		*handle = addr;
		luaL_getmetatable(L, type);
		lua_setmetatable(L, -2);
		lua_newtable(L);
		lua_setuservalue(L, -2);
	}
	return *handle;
}

static void *obj_arg_get(lua_State *L, int idx, const char *type) {
	void **addr = luaL_checkudata(L, idx, type);
	return obj_get(L, *addr, type);
}

static void *obj_check(lua_State *L, int idx, const char *type) {
	void **addr = luaL_checkudata(L, idx, type);
	if (!obj_get(L, *addr, type))
		return NULL;
	lua_pop(L, 1);
	return *addr;
}

static int index_common(lua_State *L) {
	lua_getmetatable(L, 1);
	lua_pushvalue(L, 2);
	lua_gettable(L, -2);
	if (lua_isnil(L, -1)) {
		lua_getuservalue(L, 1);
		lua_pushvalue(L, 2);
		lua_gettable(L, -2);
	}
	return 1;
}

static int newindex_common(lua_State *L) {
	lua_getuservalue(L, 1);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_settable(L, -3);
	return 0;
}

static int windows_iter(lua_State *L);

static int windows(lua_State *L) {
	Vis *vis = lua_touserdata(L, lua_upvalueindex(1));
	Win **handle = lua_newuserdata(L, sizeof *handle);
	*handle = vis->windows;
	lua_pushcclosure(L, windows_iter, 1);
	return 1;
}

static int windows_iter(lua_State *L) {
	Win **handle = lua_touserdata(L, lua_upvalueindex(1));
	if (!*handle)
		return 0;
	Win *win = obj_new(L, *handle, "vis.window");
	if (!win)
		return 0;
	*handle = win->next;
	return 1;
}

static int files_iter(lua_State *L);

static int files(lua_State *L) {
	Vis *vis = lua_touserdata(L, lua_upvalueindex(1));
	File **handle = lua_newuserdata(L, sizeof *handle);
	*handle = vis->files;
	lua_pushcclosure(L, files_iter, 1);
	return 1;
}

static int files_iter(lua_State *L) {
	File **handle = lua_touserdata(L, lua_upvalueindex(1));
	if (!*handle)
		return 0;
	File *file = obj_new(L, *handle, "vis.file");
	if (!file)
		return 0;
	*handle = file->next;
	return 1;
}

static int command(lua_State *L) {
	Vis *vis = lua_touserdata(L, lua_upvalueindex(1));
	const char *cmd = luaL_checkstring(L, 1);
	bool ret = vis_cmd(vis, cmd);
	lua_pushboolean(L, ret);
	return 1;
}

static int info(lua_State *L) {
	Vis *vis = lua_touserdata(L, lua_upvalueindex(1));
	const char *msg = luaL_checkstring(L, 1);
	vis_info_show(vis, "%s", msg);
	return 0;
}

static const struct luaL_Reg vis_lua[] = {
	{ "files", files },
	{ "windows", windows },
	{ "command", command },
	{ "info", info },
	{ NULL, NULL },
};

static int window_index(lua_State *L) {
	Win *win = obj_check(L, 1, "vis.window");
	if (!win) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_isstring(L, 2)) {
		const char *key = lua_tostring(L, 2);
		if (strcmp(key, "file") == 0) {
			obj_new(L, win->file, "vis.file");
			return 1;
		}
	}

	return index_common(L);
}

static int window_newindex(lua_State *L) {
	Win *win = obj_check(L, 1, "vis.window");
	if (!win)
		return 0;
	return newindex_common(L);
}

static const struct luaL_Reg window_funcs[] = {
	{ "__index", window_index },
	{ "__newindex", window_newindex },
	{ NULL, NULL },
};

static int file_index(lua_State *L) {
	File *file = obj_check(L, 1, "vis.file");
	if (!file) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_isstring(L, 2)) {
		const char *key = lua_tostring(L, 2);
		if (strcmp(key, "name") == 0) {
			lua_pushstring(L, file->name);
			return 1;
		}
	}

	return index_common(L);
}

static int file_newindex(lua_State *L) {
	File *file = obj_check(L, 1, "vis.file");
	if (!file)
		return 0;
	return newindex_common(L);
}

static int file_insert(lua_State *L) {
	File *file = obj_check(L, 1, "vis.file");
	if (file) {
		size_t pos = luaL_checkunsigned(L, 2);
		size_t len;
		luaL_checkstring(L, 3);
		const char *data = lua_tolstring(L, 3, &len);
 		bool ret = text_insert(file->text, pos, data, len);
		lua_pushboolean(L, ret);
	} else {
		lua_pushboolean(L, false);
	}
	return 1;
}

static int file_delete(lua_State *L) {
	File *file = obj_check(L, 1, "vis.file");
	if (file) {
		size_t pos = luaL_checkunsigned(L, 2);
		size_t len = luaL_checkunsigned(L, 3);
		bool ret = text_delete(file->text, pos, len);
		lua_pushboolean(L, ret);
	} else {
		lua_pushboolean(L, false);
	}
	return 1;
}

static int file_lines_iter(lua_State *L);

static int file_lines(lua_State *L) {
	obj_arg_get(L, 1, "vis.file");
	size_t *pos = lua_newuserdata(L, sizeof *pos);
	*pos = 0;
	lua_pushcclosure(L, file_lines_iter, 2);
	return 1;
}

static int file_lines_iter(lua_State *L) {
	File *file = *(File**)lua_touserdata(L, lua_upvalueindex(1));
	size_t *pos = lua_touserdata(L, lua_upvalueindex(2));
	size_t new_pos = text_line_next(file->text, *pos);
	size_t len = new_pos - *pos;
	if (len == 0)
		return 0;
	char *buf = malloc(len);
	if (!buf)
		return 0;
	len = text_bytes_get(file->text, *pos, len, buf);
	lua_pushlstring(L, buf, len);
	free(buf);
	*pos = new_pos;
	return 1;
}

static const struct luaL_Reg file_funcs[] = {
	{ "__index", file_index },
	{ "__newindex", file_newindex },
	{ "insert", file_insert },
	{ "delete", file_delete },
	{ "lines", file_lines },
	{ NULL, NULL },
};


static void vis_lua_event(Vis *vis, const char *name) {
	lua_State *L = vis->lua;
	lua_getglobal(L, "vis");
	lua_getfield(L, -1, "events");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, name);
	}
	lua_remove(L, -2);
}

void vis_lua_start(Vis *vis) {
	lua_State *L = luaL_newstate();
	if (!L)
		return;
	vis->lua = L;
	luaL_openlibs(L);


	/* extends lua's package.path with:
	 * - $VIS_PATH/{,lexers}
	 * - $XDG_CONFIG_HOME/vis/{,lexers} (defaulting to $HOME/.config/vis/{,lexers})
	 * - /usr/local/share/vis/{,lexers}
	 * - /usr/share/vis/{,lexers}
	 * - package.path (standard lua search path)
	 */
	int paths = 3;
	lua_getglobal(L, "package");

	const char *vis_path = getenv("VIS_PATH");
	if (vis_path) {
		lua_pushstring(L, vis_path);
		lua_pushstring(L, "/?.lua;");
		lua_pushstring(L, vis_path);
		lua_pushstring(L, "/lexers/?.lua;");
		lua_concat(L, 4);
		paths++;
	}

	/* try to get users home directory */
	const char *home = getenv("HOME");
	if (!home || !*home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}

	const char *xdg_config = getenv("XDG_CONFIG_HOME");
	if (xdg_config) {
		lua_pushstring(L, xdg_config);
		lua_pushstring(L, "/vis/?.lua;");
		lua_pushstring(L, xdg_config);
		lua_pushstring(L, "/vis/lexers/?.lua;");
		lua_concat(L, 4);
		paths++;
	} else if (home && *home) {
		lua_pushstring(L, home);
		lua_pushstring(L, "/.config/vis/?.lua;");
		lua_pushstring(L, home);
		lua_pushstring(L, "/.config/vis/lexers/?.lua;");
		lua_concat(L, 4);
		paths++;
	}

	lua_pushstring(L, "/usr/local/share/vis/?.lua;/usr/local/share/vis/lexers/?.lua;");
	lua_pushstring(L, "/usr/share/vis/?.lua;/usr/share/vis/lexers/?.lua;");
	lua_getfield(L, -paths, "path");
	lua_concat(L, paths);
	lua_setfield(L, -2, "path");
	lua_pop(L, 1); /* package */

	/* table in registry to track lifetimes of C objects */
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "vis.objects");
	/* metatable used to type check user data */
	luaL_newmetatable(L, "vis.file");
	luaL_setfuncs(L, file_funcs, 0);
	luaL_newmetatable(L, "vis.window");
	luaL_setfuncs(L, window_funcs, 0);
	/* vis module table with up value as the C pointer */
	luaL_newlibtable(L, vis_lua);
	lua_pushlightuserdata(L, vis);
	luaL_setfuncs(L, vis_lua, 1);
	lua_setglobal(L, "vis");

	lua_getglobal(L, "require");
	lua_pushstring(L, "visrc");
	lua_pcall(L, 1, 0, 0);
}

void vis_lua_quit(Vis *vis) {
	lua_State *L = vis->lua;
	if (L)
		lua_close(L);
}

void vis_lua_file_open(Vis *vis, File *file) {

}

void vis_lua_file_save(Vis *vis, File *file) {

}

void vis_lua_file_close(Vis *vis, File *file) {
	lua_State *L = vis->lua;
	vis_lua_event(vis, "file_close");
	if (lua_isfunction(L, -1)) {
		obj_new(L, file, "vis.file");
		lua_pcall(L, 1, 0, 0);
	}
	obj_del(L, file);
	lua_pop(L, 1);
}

void vis_lua_win_open(Vis *vis, Win *win) {
	lua_State *L = vis->lua;
	vis_lua_event(vis, "win_open");
	if (lua_isfunction(L, -1)) {
		obj_new(L, win, "vis.window");
		lua_pcall(L, 1, 0, 0);
	}
	lua_pop(L, 1);
}

void vis_lua_win_close(Vis *vis, Win *win) {
	lua_State *L = vis->lua;
	vis_lua_event(vis, "win_close");
	if (lua_isfunction(L, -1)) {
		obj_new(L, win, "vis.window");
		lua_pcall(L, 1, 0, 0);
	}
	obj_del(L, win);
	lua_pop(L, 1);
}

bool vis_theme_load(Vis *vis, const char *name) {
	lua_State *L = vis->lua;
	if (!L)
		return false;
	/* package.loaded['themes/'..name] = nil
	 * require 'themes/'..name */
	lua_pushstring(L, "themes/");
	lua_pushstring(L, name);
	lua_concat(L, 2);
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "loaded");
	lua_pushvalue(L, -3);
	lua_pushnil(L);
	lua_settable(L, -3);
	lua_pop(L, 2);
	lua_getglobal(L, "require");
	lua_pushvalue(L, -2);
	if (lua_pcall(L, 1, 0, 0))
		return false;
	for (Win *win = vis->windows; win; win = win->next)
		view_syntax_set(win->view, view_syntax_get(win->view));
	return true;
}

#endif