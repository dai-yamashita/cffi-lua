local ffi = require("cffi")

ffi.cdef [[
    int foo(char *buf, size_t n, char const *fmt, ...) __asm__("test_snprintf");
]]

local buf = ffi.new("char[256]")
local ret = ffi.C.foo(ffi.cast("char *", buf), 256, "%s", "test")
assert(ret == 4)
assert(ffi.string(buf) == "test")
