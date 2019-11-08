/*
* Copyright (c) 2019 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef JANET_AMALG
#include <janet.h>
#include "state.h"
#include "fiber.h"
#endif

void janet_panicv(Janet message) {
    if (janet_vm_return_reg != NULL) {
        *janet_vm_return_reg = message;
        longjmp(*janet_vm_jmp_buf, 1);
    } else {
        fputs((const char *)janet_formatc("janet top level panic - %v\n", message), stdout);
        exit(1);
    }
}

void janet_panicf(const char *format, ...) {
    va_list args;
    const uint8_t *ret;
    JanetBuffer buffer;
    int32_t len = 0;
    while (format[len]) len++;
    janet_buffer_init(&buffer, len);
    va_start(args, format);
    janet_formatb(&buffer, format, args);
    va_end(args);
    ret = janet_string(buffer.data, buffer.count);
    janet_buffer_deinit(&buffer);
    janet_panics(ret);
}

void janet_panic(const char *message) {
    janet_panicv(janet_cstringv(message));
}

void janet_panics(const uint8_t *message) {
    janet_panicv(janet_wrap_string(message));
}

void janet_panic_type(Janet x, int32_t n, int expected) {
    janet_panicf("bad slot #%d, expected %T, got %v", n, expected, x);
}

void janet_panic_abstract(Janet x, int32_t n, const JanetAbstractType *at) {
    janet_panicf("bad slot #%d, expected %s, got %v", n, at->name, x);
}

void janet_fixarity(int32_t arity, int32_t fix) {
    if (arity != fix)
        janet_panicf("arity mismatch, expected %d, got %d", fix, arity);
}

void janet_arity(int32_t arity, int32_t min, int32_t max) {
    if (min >= 0 && arity < min)
        janet_panicf("arity mismatch, expected at least %d, got %d", min, arity);
    if (max >= 0 && arity > max)
        janet_panicf("arity mismatch, expected at most %d, got %d", max, arity);
}

#define DEFINE_GETTER(name, NAME, type) \
type janet_get##name(const Janet *argv, int32_t n) { \
    Janet x = argv[n]; \
    if (!janet_checktype(x, JANET_##NAME)) { \
        janet_panic_type(x, n, JANET_TFLAG_##NAME); \
    } \
    return janet_unwrap_##name(x); \
} \
type janet_opt##name(const Janet *argv, int32_t argc, int32_t n, type dflt) { \
    if (argc >= n) return dflt; \
    if (janet_checktype(argv[n], JANET_NIL)) return dflt; \
    return janet_get##name(argv, n); \
}

Janet janet_getmethod(const uint8_t *method, const JanetMethod *methods) {
    while (methods->name) {
        if (!janet_cstrcmp(method, methods->name))
            return janet_wrap_cfunction(methods->cfun);
        methods++;
    }
    return janet_wrap_nil();
}

DEFINE_GETTER(number, NUMBER, double)
DEFINE_GETTER(array, ARRAY, JanetArray *)
DEFINE_GETTER(tuple, TUPLE, const Janet *)
DEFINE_GETTER(table, TABLE, JanetTable *)
DEFINE_GETTER(struct, STRUCT, const JanetKV *)
DEFINE_GETTER(string, STRING, const uint8_t *)
DEFINE_GETTER(keyword, KEYWORD, const uint8_t *)
DEFINE_GETTER(symbol, SYMBOL, const uint8_t *)
DEFINE_GETTER(buffer, BUFFER, JanetBuffer *)
DEFINE_GETTER(fiber, FIBER, JanetFiber *)
DEFINE_GETTER(function, FUNCTION, JanetFunction *)
DEFINE_GETTER(cfunction, CFUNCTION, JanetCFunction)
DEFINE_GETTER(boolean, BOOLEAN, int)
DEFINE_GETTER(pointer, POINTER, void *)

const char *janet_getcstring(const Janet *argv, int32_t n) {
    const uint8_t *jstr = janet_getstring(argv, n);
    const char *cstr = (const char *)jstr;
    if (strlen(cstr) != (size_t) janet_string_length(jstr)) {
        janet_panicf("string %v contains embedded 0s");
    }
    return cstr;
}

int32_t janet_getinteger(const Janet *argv, int32_t n) {
    Janet x = argv[n];
    if (!janet_checkint(x)) {
        janet_panicf("bad slot #%d, expected integer, got %v", n, x);
    }
    return janet_unwrap_integer(x);
}

int64_t janet_getinteger64(const Janet *argv, int32_t n) {
    Janet x = argv[n];
    if (!janet_checkint64(x)) {
        janet_panicf("bad slot #%d, expected 64 bit integer, got %v", n, x);
    }
    return (int64_t) janet_unwrap_number(x);
}

size_t janet_getsize(const Janet *argv, int32_t n) {
    Janet x = argv[n];
    if (!janet_checksize(x)) {
        janet_panicf("bad slot #%d, expected size, got %v", n, x);
    }
    return (size_t) janet_unwrap_number(x);
}

int32_t janet_gethalfrange(const Janet *argv, int32_t n, int32_t length, const char *which) {
    int32_t raw = janet_getinteger(argv, n);
    if (raw < 0) raw += length + 1;
    if (raw < 0 || raw > length)
        janet_panicf("%s index %d out of range [0,%d]", which, raw, length);
    return raw;
}

int32_t janet_getargindex(const Janet *argv, int32_t n, int32_t length, const char *which) {
    int32_t raw = janet_getinteger(argv, n);
    if (raw < 0) raw += length;
    if (raw < 0 || raw > length)
        janet_panicf("%s index %d out of range [0,%d)", which, raw, length);
    return raw;
}

JanetView janet_getindexed(const Janet *argv, int32_t n) {
    Janet x = argv[n];
    JanetView view;
    if (!janet_indexed_view(x, &view.items, &view.len)) {
        janet_panic_type(x, n, JANET_TFLAG_INDEXED);
    }
    return view;
}

JanetByteView janet_getbytes(const Janet *argv, int32_t n) {
    Janet x = argv[n];
    JanetByteView view;
    if (!janet_bytes_view(x, &view.bytes, &view.len)) {
        janet_panic_type(x, n, JANET_TFLAG_BYTES);
    }
    return view;
}

JanetDictView janet_getdictionary(const Janet *argv, int32_t n) {
    Janet x = argv[n];
    JanetDictView view;
    if (!janet_dictionary_view(x, &view.kvs, &view.len, &view.cap)) {
        janet_panic_type(x, n, JANET_TFLAG_DICTIONARY);
    }
    return view;
}

void *janet_getabstract(const Janet *argv, int32_t n, const JanetAbstractType *at) {
    Janet x = argv[n];
    if (!janet_checktype(x, JANET_ABSTRACT)) {
        janet_panic_abstract(x, n, at);
    }
    void *abstractx = janet_unwrap_abstract(x);
    if (janet_abstract_type(abstractx) != at) {
        janet_panic_abstract(x, n, at);
    }
    return abstractx;
}

JanetRange janet_getslice(int32_t argc, const Janet *argv) {
    janet_arity(argc, 1, 3);
    JanetRange range;
    int32_t length = janet_length(argv[0]);
    if (argc == 1) {
        range.start = 0;
        range.end = length;
    } else if (argc == 2) {
        range.start = janet_checktype(argv[1], JANET_NIL)
                      ? 0
                      : janet_gethalfrange(argv, 1, length, "start");
        range.end = length;
    } else {
        range.start = janet_checktype(argv[1], JANET_NIL)
                      ? 0
                      : janet_gethalfrange(argv, 1, length, "start");
        range.end = janet_checktype(argv[2], JANET_NIL)
                    ? length
                    : janet_gethalfrange(argv, 2, length, "end");
        if (range.end < range.start)
            range.end = range.start;
    }
    return range;
}

Janet janet_dyn(const char *name) {
    if (!janet_vm_fiber) return janet_wrap_nil();
    if (janet_vm_fiber->env) {
        return janet_table_get(janet_vm_fiber->env, janet_ckeywordv(name));
    } else {
        return janet_wrap_nil();
    }
}

void janet_setdyn(const char *name, Janet value) {
    if (!janet_vm_fiber) return;
    if (!janet_vm_fiber->env) {
        janet_vm_fiber->env = janet_table(1);
    }
    janet_table_put(janet_vm_fiber->env, janet_ckeywordv(name), value);
}

uint64_t janet_getflags(const Janet *argv, int32_t n, const char *flags) {
    uint64_t ret = 0;
    const uint8_t *keyw = janet_getkeyword(argv, n);
    int32_t klen = janet_string_length(keyw);
    int32_t flen = (int32_t) strlen(flags);
    if (flen > 64) {
        flen = 64;
    }
    for (int32_t j = 0; j < klen; j++) {
        for (int32_t i = 0; i < flen; i++) {
            if (((uint8_t) flags[i]) == keyw[j]) {
                ret |= 1ULL << i;
                goto found;
            }
        }
        janet_panicf("unexpected flag %c, expected one of \"%s\"", (char) keyw[j], flags);
    found:
        ;
    }
    return ret;
}

int32_t janet_optinteger(const Janet *argv, int32_t argc, int32_t n, int32_t dflt) {
    if (argc <= n) return dflt;
    if (janet_checktype(argv[n], JANET_NIL)) return dflt;
    return janet_getinteger(argv, n);
}

int64_t janet_optinteger64(const Janet *argv, int32_t argc, int32_t n, int64_t dflt) {
    if (argc <= n) return dflt;
    if (janet_checktype(argv[n], JANET_NIL)) return dflt;
    return janet_getinteger64(argv, n);
}

size_t janet_optsize(const Janet *argv, int32_t argc, int32_t n, size_t dflt) {
    if (argc <= n) return dflt;
    if (janet_checktype(argv[n], JANET_NIL)) return dflt;
    return janet_getsize(argv, n);
}

void *janet_optabstract(const Janet *argv, int32_t argc, int32_t n, const JanetAbstractType *at, void *dflt) {
    if (argc <= n) return dflt;
    if (janet_checktype(argv[n], JANET_NIL)) return dflt;
    return janet_getabstract(argv, n, at);
}

/* Some definitions for function-like macros */

JANET_API JanetStructHead *(janet_struct_head)(const JanetKV *st) {
    return janet_struct_head(st);
}

JANET_API JanetAbstractHead *(janet_abstract_head)(const void *abstract) {
    return janet_abstract_head(abstract);
}

JANET_API JanetStringHead *(janet_string_head)(const uint8_t *s) {
    return janet_string_head(s);
}

JANET_API JanetTupleHead *(janet_tuple_head)(const Janet *tuple) {
    return janet_tuple_head(tuple);
}
