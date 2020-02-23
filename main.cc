#include <cstdlib>
#include <dlfcn.h>

#include <lua.hpp>

#include <ffi.h>

#include "parser.hh"
#include "state.hh"

static int cffi_cdef(lua_State *L) {
    parser::parse(luaL_checkstring(L, 1));
    return 0;
}

static const luaL_Reg cffi_lib[] = {
    {"cdef", cffi_cdef},

    {NULL, NULL}
};

/* FIXME: proof-of-concept for now */

struct cffi_cdata {
    ffi_cif cif;
    void (*sym)();
    parser::c_object *decl;
    ffi_arg rval;
};

static int cffi_func_call(lua_State *L) {
    cffi_cdata *fud = static_cast<cffi_cdata *>(lua_touserdata(L, 1));

    auto &func = *static_cast<parser::c_function *>(fud->decl);
    auto &pdecls = func.params();
    auto &rdecl = func.result();

    ffi_type **args = reinterpret_cast<ffi_type **>(func.ffi_data());
    void **valps = reinterpret_cast<void **>(&args[pdecls.size()]);
    void **vals = reinterpret_cast<void **>(&args[pdecls.size() * 2]);

    for (size_t i = 0; i < pdecls.size(); ++i) {
        args[i] = &ffi_type_pointer;
        /* 1 is the userdata */
        char const **s = reinterpret_cast<char const **>(
            const_cast<void const **>(&valps[i])
        );
        *s = luaL_checkstring(L, i + 2);
        vals[i] = s;
    }

    ffi_call(&fud->cif, fud->sym, &fud->rval, vals);
    lua_pushinteger(L, int(fud->rval));
    return 1;
}

static void cffi_setup_func_handle_meta(lua_State *L) {
    lua_pushcfunction(L, cffi_func_call);
    lua_setfield(L, -2, "__call");
}

static int cffi_handle_index(lua_State *L) {
    void *dl = *static_cast<void **>(lua_touserdata(L, 1));
    char const *fname = luaL_checkstring(L, 2);

    auto *fdecl = state::lookup_decl(fname);
    if (!fdecl) {
        luaL_error(L, "missing declaration for symbol '%s'", fname);
    }

    auto &func = *static_cast<parser::c_function *>(fdecl);
    size_t nargs = func.params().size();

    void *funp = dlsym(dl, fname);
    if (!funp) {
        luaL_error(L, "undefined symbol: %s", fname);
    }

    auto *fud = static_cast<cffi_cdata *>(
        lua_newuserdata(L, sizeof(cffi_cdata))
    );
    luaL_setmetatable(L, "cffi_func_handle");

    fud->sym = reinterpret_cast<void (*)()>(funp);
    fud->decl = fdecl;
    void *args = reinterpret_cast<void *>(
        new unsigned char[3 * nargs * sizeof(void *)]
    );
    /* give ownership to declaration handle asap */
    func.ffi_data(args);

    if (ffi_prep_cif(
        &fud->cif, FFI_DEFAULT_ABI, nargs, &ffi_type_sint,
        reinterpret_cast<ffi_type **>(args)
    ) != FFI_OK) {
        luaL_error(L, "unexpected failure calling '%s'", func.name.c_str());
    }

    return 1;
}

static int cffi_free_handle(lua_State *L) {
    void **c_ud = static_cast<void **>(lua_touserdata(L, 1));
    dlclose(*c_ud);
    return 0;
}

static void cffi_setup_lib_handle_meta(lua_State *L) {
    lua_pushcfunction(L, cffi_free_handle);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, cffi_handle_index);
    lua_setfield(L, -2, "__index");
}

extern "C" int luaopen_cffi(lua_State *L) {
    luaL_newlib(L, cffi_lib);

    void **c_ud = static_cast<void **>(lua_newuserdata(L, sizeof(void *)));
    *c_ud = dlopen(nullptr, RTLD_NOW);
    if (luaL_newmetatable(L, "cffi_lib_handle")) {
        cffi_setup_lib_handle_meta(L);
    }
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "C");

    if (luaL_newmetatable(L, "cffi_func_handle")) {
        cffi_setup_func_handle_meta(L);
    }
    lua_pop(L, 1);

    return 1;
}
