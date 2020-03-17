#ifndef FFI_HH
#define FFI_HH

#include <cstddef>
#include <limits>
#include <type_traits>
#include <list>

#include "libffi.hh"

#include "lua.hh"
#include "lib.hh"
#include "ast.hh"

namespace ffi {

enum metatype_flag {
    /* all versions */
    METATYPE_FLAG_ADD      = 1 << 0,
    METATYPE_FLAG_SUB      = 1 << 1,
    METATYPE_FLAG_MUL      = 1 << 2,
    METATYPE_FLAG_DIV      = 1 << 3,
    METATYPE_FLAG_MOD      = 1 << 4,
    METATYPE_FLAG_POW      = 1 << 5,
    METATYPE_FLAG_UNM      = 1 << 6,
    METATYPE_FLAG_CONCAT   = 1 << 7,
    METATYPE_FLAG_LEN      = 1 << 8,
    METATYPE_FLAG_EQ       = 1 << 9,
    METATYPE_FLAG_LT       = 1 << 10,
    METATYPE_FLAG_LE       = 1 << 11,
    METATYPE_FLAG_INDEX    = 1 << 12,
    METATYPE_FLAG_NEWINDEX = 1 << 13,
    METATYPE_FLAG_CALL     = 1 << 14,
    METATYPE_FLAG_GC       = 1 << 15,
    METATYPE_FLAG_TOSTRING = 1 << 16,

#if LUA_VERSION_NUM > 501
    /* lua 5.2+ */
    METATYPE_FLAG_PAIRS    = 1 << 17,

#if LUA_VERSION_NUM == 502
    /* lua 5.2 only */
    METATYPE_FLAG_IPAIRS   = 1 << 18,
#endif

#if LUA_VERSION_NUM > 502
    /* lua 5.3+ */
    METATYPE_FLAG_IDIV     = 1 << 19,
    METATYPE_FLAG_BAND     = 1 << 20,
    METATYPE_FLAG_BOR      = 1 << 21,
    METATYPE_FLAG_BXOR     = 1 << 22,
    METATYPE_FLAG_BNOT     = 1 << 23,
    METATYPE_FLAG_SHL      = 1 << 24,
    METATYPE_FLAG_SHR      = 1 << 25,
#endif /* LUA_VERSION_NUM > 502 */
#endif /* LUA_VERSION_NUM > 501 */
};

struct arg_stor_t {
    std::max_align_t pad;

    /* only use with types that will fit */
    template<typename T>
    T as() const {
        return *reinterpret_cast<T const *>(this);
    }

    template<typename T>
    T &as() {
        return *reinterpret_cast<T *>(this);
    }
};

struct noval {};

template<typename T>
struct cdata {
    ast::c_type decl;
    int gc_ref;
    /* auxiliary data that can be used by different cdata
     *
     * vararg functions store the number of arguments they have storage
     * prepared for here to avoid reallocating every time; arrays store
     * the "weak flag" here which indicates they only point to others'
     * memory rather than having their own
     */
    int aux;
    alignas(arg_stor_t) T val;

    void *get_addr() {
        switch (decl.type()) {
            case ast::C_BUILTIN_PTR:
            case ast::C_BUILTIN_REF:
            case ast::C_BUILTIN_FUNC:
                return *reinterpret_cast<void **>(&val);
            default:
                break;
        }
        return &val;
    }
};

static constexpr size_t cdata_value_base() {
    /* can't use cdata directly for the offset, as it's not considered
     * a standard layout type because of ast::c_type, but we don't care
     * about that, we just want to know which offset val is at
     */
    using T = struct {
        alignas(ast::c_type) char tpad[sizeof(ast::c_type)];
        int pad1, pad2;
        arg_stor_t val;
    };
    return offsetof(T, val);
}

/* careful with this; use only if you're sure you have cdata at the index
 * as otherwise it will underflow size_t and get you a ridiculous value
 */
static inline size_t cdata_value_size(lua_State *L, int idx) {
    return lua_rawlen(L, idx) - cdata_value_base();
}

struct ctype {
    ast::c_type decl;
    int ct_tag;
};

struct closure_data {
    ffi_cif cif; /* closure data needs its own cif */
    ffi_closure *closure = nullptr;
    lua_State *L = nullptr;
    int fref = LUA_REFNIL;
    std::list<closure_data **> refs{};
    ffi_type *targs[];

    ~closure_data() {
        if (!closure) {
            return;
        }
        /* invalidate any registered references to the closure data */
        while (!refs.empty()) {
            *refs.front() = nullptr;
            refs.pop_front();
        }
        luaL_unref(L, LUA_REGISTRYINDEX, fref);
        ffi_closure_free(closure);
    }
};

/* data used for function types */
struct fdata {
    void (*sym)();
    closure_data *cd; /* only for callbacks, otherwise nullptr */
    ffi_cif cif;
    arg_stor_t rarg;
    arg_stor_t args[];
};

template<typename T>
static inline cdata<T> &newcdata(
    lua_State *L, ast::c_type &&tp, size_t extra = 0
) {
    auto *cd = lua::newuserdata<cdata<T>>(L, extra);
    new (&cd->decl) ast::c_type{std::move(tp)};
    cd->gc_ref = LUA_REFNIL;
    cd->aux = 0;
    lua::mark_cdata(L);
    return *cd;
}

template<typename T>
static inline cdata<T> &newcdata(
    lua_State *L, ast::c_type const &tp, size_t extra = 0
) {
    return newcdata<T>(L, ast::c_type{tp}, extra);
}

static inline cdata<ffi::noval> &newcdata(
    lua_State *L, ast::c_type const &tp, size_t vals
) {
    auto *cd = static_cast<cdata<ffi::noval> *>(
        lua_newuserdata(L, vals + cdata_value_base())
    );
    new (&cd->decl) ast::c_type{std::move(tp)};
    cd->gc_ref = LUA_REFNIL;
    cd->aux = 0;
    lua::mark_cdata(L);
    return *cd;
}

template<typename ...A>
static inline ctype &newctype(lua_State *L, A &&...args) {
    auto *cd = lua::newuserdata<ctype>(L);
    cd->ct_tag = lua::CFFI_CTYPE_TAG;
    new (&cd->decl) ast::c_type{std::forward<A>(args)...};
    lua::mark_cdata(L);
    return *cd;
}

static inline bool iscdata(lua_State *L, int idx) {
    auto *p = static_cast<ctype *>(luaL_testudata(L, idx, lua::CFFI_CDATA_MT));
    return p && (p->ct_tag != lua::CFFI_CTYPE_TAG);
}

static inline bool isctype(lua_State *L, int idx) {
    auto *p = static_cast<ctype *>(luaL_testudata(L, idx, lua::CFFI_CDATA_MT));
    return p && (p->ct_tag == lua::CFFI_CTYPE_TAG);
}

static inline bool iscval(lua_State *L, int idx) {
    return luaL_testudata(L, idx, lua::CFFI_CDATA_MT);
}

template<typename T>
static inline bool isctype(cdata<T> const &cd) {
    return cd.gc_ref == lua::CFFI_CTYPE_TAG;
}

template<typename T>
static inline cdata<T> &checkcdata(lua_State *L, int idx) {
    auto ret = static_cast<cdata<T> *>(
        luaL_checkudata(L, idx, lua::CFFI_CDATA_MT)
    );
    if (isctype(*ret)) {
        lua::type_error(L, idx, "cdata");
    }
    return *ret;
}

template<typename T>
static inline cdata<T> *testcdata(lua_State *L, int idx) {
    auto ret = static_cast<cdata<T> *>(
        luaL_testudata(L, idx, lua::CFFI_CDATA_MT)
    );
    if (!ret || isctype(*ret)) {
        return nullptr;
    }
    return ret;
}

template<typename T>
static inline cdata<T> &tocdata(lua_State *L, int idx) {
    return *lua::touserdata<ffi::cdata<T>>(L, idx);
}

void destroy_cdata(lua_State *L, cdata<ffi::noval> &cd);
void destroy_closure(closure_data *cd);

int call_cif(cdata<fdata> &fud, lua_State *L, size_t largs);

enum conv_rule {
    RULE_CONV = 0,
    RULE_PASS,
    RULE_CAST,
    RULE_RET
};

/* this pushes a value from `value` on the Lua stack; its type
 * and necessary conversions are done based on the info in `tp` and `rule`
 *
 * `lossy` implies that numbers will always be converted to a lua number
 */
int to_lua(
    lua_State *L, ast::c_type const &tp, void const *value, int rule,
    bool lossy = false
);

/* this returns a pointer to a C value counterpart of the Lua value
 * on the stack (as given by `index`) while checking types (`rule`)
 *
 * necessary conversions are done according to `tp`; `stor` is used to
 * write scalar values (therefore its alignment and size must be enough
 * to fit the converted value - the arg_stor_t type can store any scalar
 * so you can use that) while non-scalar values may have their address
 * returned directly
 */
void *from_lua(
    lua_State *L, ast::c_type const &tp, void *stor, int index,
    size_t &dsz, int rule
);

void get_global(lua_State *L, lib::handle dl, const char *sname);
void set_global(lua_State *L, lib::handle dl, char const *sname, int idx);

void make_cdata(lua_State *L, ast::c_type const &decl, int rule, int idx);

static inline bool metatype_getfield(lua_State *L, int mt, char const *fname) {
    luaL_getmetatable(L, lua::CFFI_CDATA_MT);
    lua_getfield(L, -1, "__ffi_metatypes");
    lua_rawgeti(L, -1, mt);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, fname);
        if (!lua_isnil(L, -1)) {
            lua_insert(L, -4);
            lua_pop(L, 3);
            return true;
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 3);
    return false;
}

template<typename T>
static inline T check_arith(lua_State *L, int idx) {
    auto *cd = testcdata<arg_stor_t>(L, idx);
    if (!cd) {
        goto lval;
    }
    switch (cd->decl.type()) {
        case ast::C_BUILTIN_ENUM:
            /* TODO: large enums */
            return T(cd->val.as<int>());
        case ast::C_BUILTIN_BOOL:
            return T(cd->val.as<bool>());
        case ast::C_BUILTIN_CHAR:
            return T(cd->val.as<char>());
        case ast::C_BUILTIN_SCHAR:
            return T(cd->val.as<signed char>());
        case ast::C_BUILTIN_UCHAR:
            return T(cd->val.as<unsigned char>());
        case ast::C_BUILTIN_SHORT:
            return T(cd->val.as<short>());
        case ast::C_BUILTIN_USHORT:
            return T(cd->val.as<unsigned short>());
        case ast::C_BUILTIN_INT:
            return T(cd->val.as<int>());
        case ast::C_BUILTIN_UINT:
            return T(cd->val.as<unsigned int>());
        case ast::C_BUILTIN_LONG:
            return T(cd->val.as<long>());
        case ast::C_BUILTIN_ULONG:
            return T(cd->val.as<unsigned long>());
        case ast::C_BUILTIN_LLONG:
            return T(cd->val.as<long long>());
        case ast::C_BUILTIN_ULLONG:
            return T(cd->val.as<unsigned long long>());
        case ast::C_BUILTIN_FLOAT:
            return T(cd->val.as<float>());
        case ast::C_BUILTIN_DOUBLE:
            return T(cd->val.as<double>());
        case ast::C_BUILTIN_LDOUBLE:
            return T(cd->val.as<long double>());
        default:
            break;
    }
lval:
    if (std::is_integral<T>::value) {
        return T(luaL_checkinteger(L, idx));
    }
    return T(luaL_checknumber(L, idx));
}

static inline ast::c_expr_type check_arith_expr(
    lua_State *L, int idx, ast::c_value &v
) {
    auto *cd = testcdata<arg_stor_t>(L, idx);
    if (!cd) {
        goto lval;
    }
    switch (cd->decl.type()) {
        case ast::C_BUILTIN_ENUM:
            /* TODO: large enums */
            v.i = cd->val.as<int>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_BOOL:
            v.i = cd->val.as<bool>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_CHAR:
            v.i = cd->val.as<char>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_SCHAR:
            v.i = cd->val.as<signed char>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_UCHAR:
            v.i = cd->val.as<unsigned char>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_SHORT:
            v.i = cd->val.as<short>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_USHORT:
            v.i = cd->val.as<unsigned short>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_INT:
            v.i = cd->val.as<int>();
            return ast::c_expr_type::INT;
        case ast::C_BUILTIN_UINT:
            v.u = cd->val.as<unsigned int>();
            return ast::c_expr_type::UINT;
        case ast::C_BUILTIN_LONG:
            v.l = cd->val.as<long>();
            return ast::c_expr_type::LONG;
        case ast::C_BUILTIN_ULONG:
            v.ul = cd->val.as<unsigned long>();
            return ast::c_expr_type::ULONG;
        case ast::C_BUILTIN_LLONG:
            v.ll = cd->val.as<long long>();
            return ast::c_expr_type::LLONG;
        case ast::C_BUILTIN_ULLONG:
            v.ull = cd->val.as<unsigned long long>();
            return ast::c_expr_type::ULLONG;
        case ast::C_BUILTIN_FLOAT:
            v.f = cd->val.as<float>();
            return ast::c_expr_type::FLOAT;
        case ast::C_BUILTIN_DOUBLE:
            v.d = cd->val.as<double>();
            return ast::c_expr_type::DOUBLE;
        case ast::C_BUILTIN_LDOUBLE:
            v.ld = cd->val.as<long double>();
            return ast::c_expr_type::LDOUBLE;
        default:
            break;
    }
lval:
    /* some logic for conversions of lua numbers into cexprs */
    static_assert(
        std::is_integral<lua_Number>::value
            ? (sizeof(lua_Number) <= sizeof(long long))
            : (sizeof(lua_Number) <= sizeof(long double)),
        "invalid lua_Number format"
    );
    auto n = luaL_checknumber(L, idx);
    if (std::is_integral<lua_Number>::value) {
        if (std::is_signed<lua_Number>::value) {
            if (sizeof(lua_Number) <= sizeof(int)) {
                v.i = n;
                return ast::c_expr_type::INT;
            } else if (sizeof(lua_Number) <= sizeof(long)) {
                v.l = n;
                return ast::c_expr_type::LONG;
            } else {
                v.ll = n;
                return ast::c_expr_type::LLONG;
            }
        } else {
            if (sizeof(lua_Number) <= sizeof(unsigned int)) {
                v.u = n;
                return ast::c_expr_type::UINT;
            } else if (sizeof(lua_Number) <= sizeof(unsigned long)) {
                v.ul = n;
                return ast::c_expr_type::ULONG;
            } else {
                v.ull = n;
                return ast::c_expr_type::ULLONG;
            }
        }
    } else if (sizeof(lua_Number) <= sizeof(float)) {
        v.f = n;
        return ast::c_expr_type::FLOAT;
    } else if (sizeof(lua_Number) <= sizeof(double)) {
        v.d = n;
        return ast::c_expr_type::DOUBLE;
    }
    v.ld = n;
    return ast::c_expr_type::LDOUBLE;
}

static inline cdata<arg_stor_t> &make_cdata_arith(
    lua_State *L, ast::c_expr_type et, ast::c_value const &cv
) {
    auto bt = ast::to_builtin_type(et);
    if (bt == ast::C_BUILTIN_INVALID) {
        luaL_error(L, "invalid value type");
    }
    auto tp = ast::c_type{bt, 0};
    auto as = tp.alloc_size();
    auto &cd = newcdata(L, std::move(tp), as);
    memcpy(&cd.val, &cv, as);
    return *reinterpret_cast<cdata<arg_stor_t> *>(&cd);
}

} /* namespace ffi */

#endif /* FFI_HH */
