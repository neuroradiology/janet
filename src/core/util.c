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

#include <inttypes.h>

#ifndef JANET_AMALG
#include <janet.h>
#include "util.h"
#include "state.h"
#include "gc.h"
#endif

/* Base 64 lookup table for digits */
const char janet_base64[65] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "_=";

/* The JANET value types in order. These types can be used as
 * mnemonics instead of a bit pattern for type checking */
const char *const janet_type_names[16] = {
    "number",
    "nil",
    "boolean",
    "fiber",
    "string",
    "symbol",
    "keyword",
    "array",
    "tuple",
    "table",
    "struct",
    "buffer",
    "function",
    "cfunction",
    "abstract",
    "pointer"
};

const char *const janet_signal_names[14] = {
    "ok",
    "error",
    "debug",
    "yield",
    "user0",
    "user1",
    "user2",
    "user3",
    "user4",
    "user5",
    "user6",
    "user7",
    "user8",
    "user9"
};

const char *const janet_status_names[16] = {
    "dead",
    "error",
    "debug",
    "pending",
    "user0",
    "user1",
    "user2",
    "user3",
    "user4",
    "user5",
    "user6",
    "user7",
    "user8",
    "user9",
    "new",
    "alive"
};

/* Calculate hash for string */

int32_t janet_string_calchash(const uint8_t *str, int32_t len) {
    const uint8_t *end = str + len;
    uint32_t hash = 5381;
    while (str < end)
        hash = (hash << 5) + hash + *str++;
    return (int32_t) hash;
}

/* Computes hash of an array of values */
int32_t janet_array_calchash(const Janet *array, int32_t len) {
    const Janet *end = array + len;
    uint32_t hash = 5381;
    while (array < end)
        hash = (hash << 5) + hash + janet_hash(*array++);
    return (int32_t) hash;
}

/* Computes hash of an array of values */
int32_t janet_kv_calchash(const JanetKV *kvs, int32_t len) {
    const JanetKV *end = kvs + len;
    uint32_t hash = 5381;
    while (kvs < end) {
        hash = (hash << 5) + hash + janet_hash(kvs->key);
        hash = (hash << 5) + hash + janet_hash(kvs->value);
        kvs++;
    }
    return (int32_t) hash;
}

/* Calculate next power of 2. May overflow. If n is 0,
 * will return 0. */
int32_t janet_tablen(int32_t n) {
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

/* Helper to find a value in a Janet struct or table. Returns the bucket
 * containing the key, or the first empty bucket if there is no such key. */
const JanetKV *janet_dict_find(const JanetKV *buckets, int32_t cap, Janet key) {
    int32_t index = janet_maphash(cap, janet_hash(key));
    int32_t i;
    const JanetKV *first_bucket = NULL;
    /* Higher half */
    for (i = index; i < cap; i++) {
        const JanetKV *kv = buckets + i;
        if (janet_checktype(kv->key, JANET_NIL)) {
            if (janet_checktype(kv->value, JANET_NIL)) {
                return kv;
            } else if (NULL == first_bucket) {
                first_bucket = kv;
            }
        } else if (janet_equals(kv->key, key)) {
            return buckets + i;
        }
    }
    /* Lower half */
    for (i = 0; i < index; i++) {
        const JanetKV *kv = buckets + i;
        if (janet_checktype(kv->key, JANET_NIL)) {
            if (janet_checktype(kv->value, JANET_NIL)) {
                return kv;
            } else if (NULL == first_bucket) {
                first_bucket = kv;
            }
        } else if (janet_equals(kv->key, key)) {
            return buckets + i;
        }
    }
    return first_bucket;
}

/* Get a value from a janet struct or table. */
Janet janet_dictionary_get(const JanetKV *data, int32_t cap, Janet key) {
    const JanetKV *kv = janet_dict_find(data, cap, key);
    if (kv && !janet_checktype(kv->key, JANET_NIL)) {
        return kv->value;
    }
    return janet_wrap_nil();
}

/* Iterate through a struct or dictionary generically */
const JanetKV *janet_dictionary_next(const JanetKV *kvs, int32_t cap, const JanetKV *kv) {
    const JanetKV *end = kvs + cap;
    kv = (kv == NULL) ? kvs : kv + 1;
    while (kv < end) {
        if (!janet_checktype(kv->key, JANET_NIL))
            return kv;
        kv++;
    }
    return NULL;
}

/* Compare a janet string with a cstring. More efficient than loading
 * c string as a janet string. */
int janet_cstrcmp(const uint8_t *str, const char *other) {
    int32_t len = janet_string_length(str);
    int32_t index;
    for (index = 0; index < len; index++) {
        uint8_t c = str[index];
        uint8_t k = ((const uint8_t *)other)[index];
        if (c < k) return -1;
        if (c > k) return 1;
        if (k == '\0') break;
    }
    return (other[index] == '\0') ? 0 : -1;
}

/* Do a binary search on a static array of structs. Each struct must
 * have a string as its first element, and the struct must be sorted
 * lexicographically by that element. */
const void *janet_strbinsearch(
    const void *tab,
    size_t tabcount,
    size_t itemsize,
    const uint8_t *key) {
    size_t low = 0;
    size_t hi = tabcount;
    const char *t = (const char *)tab;
    while (low < hi) {
        size_t mid = low + ((hi - low) / 2);
        const char **item = (const char **)(t + mid * itemsize);
        const char *name = *item;
        int comp = janet_cstrcmp(key, name);
        if (comp < 0) {
            hi = mid;
        } else if (comp > 0) {
            low = mid + 1;
        } else {
            return (const void *)item;
        }
    }
    return NULL;
}

/* Register a value in the global registry */
void janet_register(const char *name, JanetCFunction cfun) {
    Janet key = janet_wrap_cfunction(cfun);
    Janet value = janet_csymbolv(name);
    janet_table_put(janet_vm_registry, key, value);
}

/* Add a def to an environment */
void janet_def(JanetTable *env, const char *name, Janet val, const char *doc) {
    JanetTable *subt = janet_table(2);
    janet_table_put(subt, janet_ckeywordv("value"), val);
    if (doc)
        janet_table_put(subt, janet_ckeywordv("doc"), janet_cstringv(doc));
    janet_table_put(env, janet_csymbolv(name), janet_wrap_table(subt));
}

/* Add a var to the environment */
void janet_var(JanetTable *env, const char *name, Janet val, const char *doc) {
    JanetArray *array = janet_array(1);
    JanetTable *subt = janet_table(2);
    janet_array_push(array, val);
    janet_table_put(subt, janet_ckeywordv("ref"), janet_wrap_array(array));
    if (doc)
        janet_table_put(subt, janet_ckeywordv("doc"), janet_cstringv(doc));
    janet_table_put(env, janet_csymbolv(name), janet_wrap_table(subt));
}

/* Load many cfunctions at once */
void janet_cfuns(JanetTable *env, const char *regprefix, const JanetReg *cfuns) {
    while (cfuns->name) {
        Janet name = janet_csymbolv(cfuns->name);
        Janet longname = name;
        if (regprefix) {
            int32_t reglen = 0;
            int32_t nmlen = 0;
            while (regprefix[reglen]) reglen++;
            while (cfuns->name[nmlen]) nmlen++;
            int32_t symlen = reglen + 1 + nmlen;
            uint8_t *longname_buffer = malloc(symlen);
            memcpy(longname_buffer, regprefix, reglen);
            longname_buffer[reglen] = '/';
            memcpy(longname_buffer + reglen + 1, cfuns->name, nmlen);
            longname = janet_wrap_symbol(janet_symbol(longname_buffer, symlen));
            free(longname_buffer);
        }
        Janet fun = janet_wrap_cfunction(cfuns->cfun);
        janet_def(env, cfuns->name, fun, cfuns->documentation);
        janet_table_put(janet_vm_registry, fun, longname);
        cfuns++;
    }
}

/* Abstract type introspection */

static const JanetAbstractType type_wrap = {
    "core/type-info",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

typedef struct {
    const JanetAbstractType *at;
} JanetAbstractTypeWrap;

void janet_register_abstract_type(const JanetAbstractType *at) {
    JanetAbstractTypeWrap *abstract = (JanetAbstractTypeWrap *)
                                      janet_abstract(&type_wrap, sizeof(JanetAbstractTypeWrap));
    abstract->at = at;
    Janet sym = janet_csymbolv(at->name);
    if (!(janet_checktype(janet_table_get(janet_vm_registry, sym), JANET_NIL))) {
        janet_panicf("cannot register abstract type %s, "
                     "a type with the same name exists", at->name);
    }
    janet_table_put(janet_vm_registry, sym, janet_wrap_abstract(abstract));
}

const JanetAbstractType *janet_get_abstract_type(Janet key) {
    Janet twrap = janet_table_get(janet_vm_registry, key);
    if (janet_checktype(twrap, JANET_NIL)) {
        return NULL;
    }
    if (!janet_checktype(twrap, JANET_ABSTRACT) ||
            (janet_abstract_type(janet_unwrap_abstract(twrap)) != &type_wrap)) {
        janet_panic("expected abstract type");
    }
    JanetAbstractTypeWrap *w = (JanetAbstractTypeWrap *)janet_unwrap_abstract(twrap);
    return w->at;
}

#ifndef JANET_BOOTSTRAP
void janet_core_def(JanetTable *env, const char *name, Janet x, const void *p) {
    (void) p;
    Janet key = janet_csymbolv(name);
    Janet value;
    /* During init, allow replacing core library cfunctions with values from
     * the env. */
    Janet check = janet_table_get(env, key);
    if (janet_checktype(check, JANET_NIL)) {
        value = x;
    } else {
        value = check;
    }
    janet_table_put(env, key, value);
    if (janet_checktype(value, JANET_CFUNCTION)) {
        janet_table_put(janet_vm_registry, value, key);
    }
}

void janet_core_cfuns(JanetTable *env, const char *regprefix, const JanetReg *cfuns) {
    (void) regprefix;
    while (cfuns->name) {
        Janet fun = janet_wrap_cfunction(cfuns->cfun);
        janet_core_def(env, cfuns->name, fun, cfuns->documentation);
        cfuns++;
    }
}
#endif

/* Resolve a symbol in the environment */
JanetBindingType janet_resolve(JanetTable *env, const uint8_t *sym, Janet *out) {
    Janet ref;
    JanetTable *entry_table;
    Janet entry = janet_table_get(env, janet_wrap_symbol(sym));
    if (!janet_checktype(entry, JANET_TABLE))
        return JANET_BINDING_NONE;
    entry_table = janet_unwrap_table(entry);
    if (!janet_checktype(
                janet_table_get(entry_table, janet_ckeywordv("macro")),
                JANET_NIL)) {
        *out = janet_table_get(entry_table, janet_ckeywordv("value"));
        return JANET_BINDING_MACRO;
    }
    ref = janet_table_get(entry_table, janet_ckeywordv("ref"));
    if (janet_checktype(ref, JANET_ARRAY)) {
        *out = ref;
        return JANET_BINDING_VAR;
    }
    *out = janet_table_get(entry_table, janet_ckeywordv("value"));
    return JANET_BINDING_DEF;
}

/* Read both tuples and arrays as c pointers + int32_t length. Return 1 if the
 * view can be constructed, 0 if an invalid type. */
int janet_indexed_view(Janet seq, const Janet **data, int32_t *len) {
    if (janet_checktype(seq, JANET_ARRAY)) {
        *data = janet_unwrap_array(seq)->data;
        *len = janet_unwrap_array(seq)->count;
        return 1;
    } else if (janet_checktype(seq, JANET_TUPLE)) {
        *data = janet_unwrap_tuple(seq);
        *len = janet_tuple_length(janet_unwrap_tuple(seq));
        return 1;
    }
    return 0;
}

/* Read both strings and buffer as unsigned character array + int32_t len.
 * Returns 1 if the view can be constructed and 0 if the type is invalid. */
int janet_bytes_view(Janet str, const uint8_t **data, int32_t *len) {
    if (janet_checktype(str, JANET_STRING) || janet_checktype(str, JANET_SYMBOL) ||
            janet_checktype(str, JANET_KEYWORD)) {
        *data = janet_unwrap_string(str);
        *len = janet_string_length(janet_unwrap_string(str));
        return 1;
    } else if (janet_checktype(str, JANET_BUFFER)) {
        *data = janet_unwrap_buffer(str)->data;
        *len = janet_unwrap_buffer(str)->count;
        return 1;
    }
    return 0;
}

/* Read both structs and tables as the entries of a hashtable with
 * identical structure. Returns 1 if the view can be constructed and
 * 0 if the type is invalid. */
int janet_dictionary_view(Janet tab, const JanetKV **data, int32_t *len, int32_t *cap) {
    if (janet_checktype(tab, JANET_TABLE)) {
        *data = janet_unwrap_table(tab)->data;
        *cap = janet_unwrap_table(tab)->capacity;
        *len = janet_unwrap_table(tab)->count;
        return 1;
    } else if (janet_checktype(tab, JANET_STRUCT)) {
        *data = janet_unwrap_struct(tab);
        *cap = janet_struct_capacity(janet_unwrap_struct(tab));
        *len = janet_struct_length(janet_unwrap_struct(tab));
        return 1;
    }
    return 0;
}

int janet_checkint(Janet x) {
    if (!janet_checktype(x, JANET_NUMBER))
        return 0;
    double dval = janet_unwrap_number(x);
    return janet_checkintrange(dval);
}

int janet_checkint64(Janet x) {
    if (!janet_checktype(x, JANET_NUMBER))
        return 0;
    double dval = janet_unwrap_number(x);
    return janet_checkint64range(dval);
}

int janet_checksize(Janet x) {
    if (!janet_checktype(x, JANET_NUMBER))
        return 0;
    double dval = janet_unwrap_number(x);
    return dval == (double)((size_t) dval) &&
           dval <= SIZE_MAX;
}
