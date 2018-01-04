/*
Copyright: Boaz Segev, 2017
License: MIT
*/

#include "fiobj_hash.h"
#include "fiobj_internal.h"

/*
struct test_key_s {
  uint64_t hash;
  FIOBJ key;
};
#define FIO_HASH_COMPARE_KEYS(k1, k2)                                          \
  (((k1).hash) == ((k2).hash) &&                                               \
   ((k1).key == (k2).key || fiobj_is_eq((k1).key, (k2).key)))
#define FIO_HASH_KEY_TYPE struct test_key_s
#define FIO_HASH_KEY2UINT(key) (key.key)
#define FIO_HASH_KEY_INVALID ((struct test_key_s){.hash = 0})
#define FIO_HASH_KEY_ISINVALID(key) ((key).hash == 0 && (key).key == NULL)
*/

#include "fio_hashmap.h"

#include <errno.h>

/* *****************************************************************************
Hash types
***************************************************************************** */
typedef struct {
  struct fiobj_vtable_s *vtable;
  fio_hash_s hash;
} fiobj_hash_s;

/* Hash node */
typedef struct {
  struct fiobj_vtable_s *vtable;
  FIOBJ name;
  FIOBJ obj;
} fiobj_couplet_s;

#define obj2hash(o) ((fiobj_hash_s *)(o))
#define obj2couplet(o) ((fiobj_couplet_s *)(o))

void fiobj_hash_rehash(FIOBJ h) {
  if (!h || FIOBJ_TYPE(h) != FIOBJ_T_HASH)
    return;
  assert(h && FIOBJ_TYPE(h) == FIOBJ_T_HASH);

  fio_hash_rehash(&obj2hash(h)->hash);
}

/* *****************************************************************************
Couplet alloc + Couplet VTable
***************************************************************************** */

const uintptr_t FIOBJ_T_COUPLET;

static void fiobj_couplet_dealloc(FIOBJ o) {
  if (OBJREF_REM(obj2couplet(o)->name) == 0)
    OBJVTBL(obj2couplet(o)->name)->free(obj2couplet(o)->name);
  fiobj_dealloc(o);
}

static size_t fiobj_couplet_each1(FIOBJ o, size_t start_at,
                                  int (*task)(FIOBJ obj, void *arg),
                                  void *arg) {
  if (obj2couplet(o)->obj == NULL)
    return 0;
  return OBJVTBL(obj2couplet(o)->obj)
      ->each1(obj2couplet(o)->obj, start_at, task, arg);
}

static int fiobj_coup_is_eq(const FIOBJ self, const FIOBJ other) {

  if (FIOBJ_TYPE(other) != FIOBJ_T_COUPLET)
    return 0;
  if (obj2couplet(self)->name != obj2couplet(other)->name &&
      (!obj2couplet(other)->name || !obj2couplet(self)->name ||
       !OBJVTBL(obj2couplet(self)->name)
            ->is_eq(obj2couplet(self)->name, obj2couplet(other)->name)))
    return 0;
  return fiobj_iseq(obj2couplet(self)->obj, obj2couplet(other)->obj);
}

/** Returns the number of elements in the Array. */
static size_t fiobj_couplet_count_items(const FIOBJ o) {
  if (obj2couplet(o)->obj == NULL)
    return 0;
  return OBJVTBL(obj2couplet(o)->obj)->count(obj2couplet(o)->obj);
}

FIOBJ fiobj_couplet2obj(const FIOBJ obj);

static struct fiobj_vtable_s FIOBJ_VTABLE_COUPLET = {
    .free = fiobj_couplet_dealloc,
    .to_i = fiobj_noop_i,
    .to_f = fiobj_noop_f,
    .to_str = fiobj_noop_str,
    .is_eq = fiobj_coup_is_eq,
    .count = fiobj_couplet_count_items,
    .unwrap = fiobj_couplet2obj,
    .each1 = fiobj_couplet_each1,
};

const uintptr_t FIOBJ_T_COUPLET = (uintptr_t)(&FIOBJ_VTABLE_COUPLET);

static inline FIOBJ fiobj_couplet_alloc(void *sym, void *obj) {
  FIOBJ o = fiobj_alloc(sizeof(fiobj_couplet_s));
  if (!o)
    perror("ERROR: fiobj hash couldn't allocate couplet"), exit(errno);
  *(obj2couplet(o)) = (fiobj_couplet_s){
      .vtable = &FIOBJ_VTABLE_COUPLET, .name = fiobj_dup(sym), .obj = obj};
  return o;
}

/**
 * If object is a Hash couplet (occurs in `fiobj_each2`), returns the key
 * (Symbol) from the key-value pair.
 *
 * Otherwise returns NULL.
 */
FIOBJ fiobj_couplet2key(const FIOBJ obj) {
  if (!obj)
    return NULL;
  assert(FIOBJ_TYPE(obj) == FIOBJ_T_COUPLET);
  return obj2couplet(obj)->name;
}

/**
 * If object is a Hash couplet (occurs in `fiobj_each2`), returns the object
 * (the value) from the key-value pair.
 *
 * Otherwise returns NULL.
 */
FIOBJ fiobj_couplet2obj(const FIOBJ obj) {
  if (!obj)
    return NULL;
  assert(FIOBJ_TYPE(obj) == FIOBJ_T_COUPLET);
  return obj2couplet(obj)->obj;
}

/* *****************************************************************************
Hash alloc + VTable
***************************************************************************** */

const uintptr_t FIOBJ_T_HASH;

static void fiobj_hash_dealloc(FIOBJ h) {
  fio_hash_free(&obj2hash(h)->hash);
  fiobj_dealloc(h);
}

struct fiobj_inner_task_s {
  void *arg;
  int (*task)(FIOBJ, void *);
};
static int fiobj_inner_task(uint64_t key, void *obj, void *a_) {
  struct fiobj_inner_task_s *t = a_;
  return t->task(obj, t->arg);
  (void)key;
}

static size_t fiobj_hash_each1(FIOBJ o, const size_t start_at,
                               int (*task)(FIOBJ obj, void *arg), void *arg) {
  if (!o)
    return 0;
  assert(FIOBJ_TYPE(o) == FIOBJ_T_HASH);
  struct fiobj_inner_task_s a = {.task = task, .arg = arg};
  return fio_hash_each(&obj2hash(o)->hash, start_at, fiobj_inner_task,
                       (void *)&a);
}

static int fiobj_hash_is_eq(const FIOBJ self, const FIOBJ other) {
  if (FIOBJ_TYPE(other) != FIOBJ_T_HASH)
    return 0;
  if (fio_hash_count(&obj2hash(self)->hash) !=
      fio_hash_count(&obj2hash(other)->hash))
    return 0;
  // fio_ls_s *pos = obj2hash(self)->items.next;
  // while (pos != &obj2hash(self)->items) {
  //   if (!fio_hash_find((fiobj_hash_s *)other,
  //                      fiobj_sym_id(obj2couplet(pos->obj)->name)))
  //     return 0;
  //   pos = pos->next;
  // }
  return 1;
}

/** Returns the number of elements in the Array. */
size_t fiobj_hash_count(const FIOBJ o) {
  if (!o)
    return 0;
  assert(FIOBJ_TYPE(o) == FIOBJ_T_HASH);
  return fio_hash_count(&obj2hash(o)->hash);
}

static struct fiobj_vtable_s FIOBJ_VTABLE_HASH = {
    .free = fiobj_hash_dealloc,
    .to_i = fiobj_noop_i,
    .to_f = fiobj_noop_f,
    .to_str = fiobj_noop_str,
    .is_eq = fiobj_hash_is_eq,
    .count = fiobj_hash_count,
    .unwrap = fiobj_noop_unwrap,
    .each1 = fiobj_hash_each1,
};

const uintptr_t FIOBJ_T_HASH = (uintptr_t)(&FIOBJ_VTABLE_HASH);

/* *****************************************************************************
Hash API
***************************************************************************** */

/**
 * Creates a mutable empty Hash object. Use `fiobj_free` when done.
 *
 * Notice that these Hash objects are designed for smaller collections and
 * retain order of object insertion.
 */
FIOBJ fiobj_hash_new(void) {
  FIOBJ o = fiobj_alloc(sizeof(fiobj_hash_s));
  if (!o)
    perror("ERROR: fiobj hash couldn't allocate memory"), exit(errno);
  *obj2hash(o) = (fiobj_hash_s){
      .vtable = &FIOBJ_VTABLE_HASH,
  };
  fio_hash_new(&obj2hash(o)->hash);
  return o;
}

/**
 * Sets a key-value pair in the Hash, duplicating the Symbol and **moving**
 * the ownership of the object to the Hash.
 *
 * Returns -1 on error.
 */
int fiobj_hash_set(FIOBJ hash, FIOBJ sym, FIOBJ obj) {
  assert(hash && FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  uintptr_t hash_value = 0;
  if (FIOBJ_TYPE(sym) == FIOBJ_T_SYMBOL) {
    hash_value = fiobj_sym_id(sym);
  } else {
    fio_cstr_s str = fiobj_obj2cstr(sym);
    if (!str.data) {
      fiobj_free((FIOBJ)obj);
      return -1;
    }
    hash_value = fiobj_sym_hash(str.value, str.len);
  }

  FIOBJ coup = fiobj_couplet_alloc(sym, obj);
  FIOBJ old = fio_hash_insert(&obj2hash(hash)->hash, hash_value, coup);
  if (old && !OBJREF_REM(old)) {
    fiobj_free(obj2couplet(old)->obj);
    obj2couplet(old)->obj = NULL;
    fiobj_couplet_dealloc(old);
  }
  return 0;
}

/**
 * Replaces the value in a key-value pair, returning the old value (and it's
 * ownership) to the caller.
 *
 * A return value of NULL indicates that no previous object existed (but a new
 * key-value pair was created.
 *
 * Errors are silently ignored.
 */
FIOBJ fiobj_hash_replace(FIOBJ hash, FIOBJ sym, FIOBJ obj) {
  assert(hash && FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  uintptr_t hash_value = 0;
  if (FIOBJ_TYPE(sym) == FIOBJ_T_SYMBOL) {
    hash_value = fiobj_sym_id(sym);
  } else {
    fio_cstr_s str = fiobj_obj2cstr(sym);
    if (!str.data) {
      fiobj_free((FIOBJ)obj);
      return NULL;
    }
    hash_value = fiobj_sym_hash(str.value, str.len);
  }

  FIOBJ coup = fiobj_couplet_alloc(sym, obj);
  FIOBJ old = fio_hash_insert(&obj2hash(hash)->hash, hash_value, coup);
  if (!old)
    return NULL;
  FIOBJ ret = fiobj_couplet2obj(old);
  if (!OBJREF_REM(old)) {
    fiobj_couplet_dealloc(old);
  }
  return ret;
}

/**
 * Removes a key-value pair from the Hash, if it exists, returning the old
 * object (instead of freeing it).
 */
FIOBJ fiobj_hash_remove3(FIOBJ hash, uintptr_t hash_value) {
  if (!hash) {
    return NULL;
  }
  assert(FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  FIOBJ old = fio_hash_insert(&obj2hash(hash)->hash, hash_value, NULL);
  if (!old)
    return NULL;
  FIOBJ ret = fiobj_couplet2obj(old);
  if (!OBJREF_REM(old)) {
    fiobj_couplet_dealloc(old);
  }
  return ret;
}

/**
 * Removes a key-value pair from the Hash, if it exists, returning the old
 * object (instead of freeing it).
 */
FIOBJ fiobj_hash_remove(FIOBJ hash, FIOBJ sym) {
  if (!hash) {
    return NULL;
  }
  assert(FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  uintptr_t hash_value = 0;
  if (FIOBJ_TYPE(sym) == FIOBJ_T_SYMBOL) {
    hash_value = fiobj_sym_id(sym);
  } else {
    fio_cstr_s str = fiobj_obj2cstr(sym);
    if (!str.data) {
      return NULL;
    }
    hash_value = fiobj_sym_hash(str.value, str.len);
  }
  FIOBJ old = fio_hash_insert(&obj2hash(hash)->hash, hash_value, NULL);
  if (!old)
    return NULL;
  FIOBJ ret = fiobj_couplet2obj(old);
  if (!OBJREF_REM(old)) {
    fiobj_couplet_dealloc(old);
  }
  return ret;
}

/**
 * Deletes a key-value pair from the Hash, if it exists, freeing the
 * associated object.
 *
 * This function takes a `uintptr_t` Hash value (see `fiobj_sym_hash`) to
 * perform a lookup in the HashMap, which is slightly faster than the other
 * variations.
 *
 * Returns -1 on type error or if the object never existed.
 */
int fiobj_hash_delete3(FIOBJ hash, uintptr_t key_hash) {
  if (!hash) {
    return -1;
  }
  assert(FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  FIOBJ obj = fiobj_hash_remove3(hash, key_hash);
  if (!obj)
    return -1;
  fiobj_free(obj);
  return 0;
}

/**
 * Deletes a key-value pair from the Hash, if it exists, freeing the
 * associated object.
 *
 * This function takes a C string instead of a Symbol, which is slower if a
 * Symbol can be cached but faster if a Symbol must be created.
 *
 * Returns -1 on type error or if the object never existed.
 */
int fiobj_hash_delete2(FIOBJ hash, const char *str, size_t len) {
  return (fiobj_hash_delete3(hash, fiobj_sym_hash(str, len)));
}

/**
 * Deletes a key-value pair from the Hash, if it exists, freeing the
 * associated object.
 *
 * Returns -1 on type error or if the object never existed.
 */
int fiobj_hash_delete(FIOBJ hash, FIOBJ sym) {
  uintptr_t hash_value = 0;
  if (FIOBJ_TYPE(sym) == FIOBJ_T_SYMBOL) {
    hash_value = fiobj_sym_id(sym);
  } else {
    fio_cstr_s str = fiobj_obj2cstr(sym);
    if (!str.data) {
      return -1;
    }
    hash_value = fiobj_sym_hash(str.value, str.len);
  }
  return (fiobj_hash_delete3(hash, hash_value));
}

/**
 * Returns a temporary handle to the object associated with the Symbol, NULL
 * if none.
 */
FIOBJ fiobj_hash_get(const FIOBJ hash, FIOBJ sym) {
  if (!hash) {
    return NULL;
  }
  assert(hash && FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  uintptr_t hash_value = 0;
  if (FIOBJ_TYPE(sym) == FIOBJ_T_SYMBOL) {
    hash_value = fiobj_sym_id(sym);
  } else {
    fio_cstr_s str = fiobj_obj2cstr(sym);
    if (!str.data) {
      return NULL;
    }
    hash_value = fiobj_sym_hash(str.value, str.len);
  }
  FIOBJ coup = fio_hash_find(&obj2hash(hash)->hash, hash_value);
  if (!coup)
    return NULL;
  return fiobj_couplet2obj(coup);
}

/**
 * Returns a temporary handle to the object associated with the Symbol C string.
 *
 * This function takes a C string instead of a Symbol, which is slower if a
 * Symbol can be cached but faster if a Symbol must be created.
 *
 * Returns NULL if no object is asociated with this String data.
 */
FIOBJ fiobj_hash_get2(const FIOBJ hash, const char *str, size_t len) {
  if (!hash) {
    return NULL;
  }
  assert(FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  uintptr_t hashed_sym = fiobj_sym_hash(str, len);
  FIOBJ coup = fio_hash_find(&obj2hash(hash)->hash, hashed_sym);
  if (!coup)
    return NULL;
  return fiobj_couplet2obj(coup);
}

/**
 * Returns a temporary handle to the object associated hashed key value.
 *
 * This function takes a `uintptr_t` Hash value (see `fiobj_sym_hash`) to
 * perform a lookup in the HashMap.
 *
 * Returns NULL if no object is asociated with this hashed key value.
 */
FIOBJ fiobj_hash_get3(const FIOBJ hash, uintptr_t key_hash) {
  if (!hash) {
    return NULL;
  }
  assert(hash && FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  FIOBJ coup = fio_hash_find(&obj2hash(hash)->hash, key_hash);
  if (!coup)
    return NULL;
  return fiobj_couplet2obj(coup);
}

/**
 * Returns 1 if the key (Symbol) exists in the Hash, even if value is NULL.
 */
int fiobj_hash_haskey(const FIOBJ hash, FIOBJ sym) {
  if (!hash) {
    return 0;
  }
  assert(FIOBJ_TYPE(hash) == FIOBJ_T_HASH);

  uintptr_t hash_value = 0;
  if (FIOBJ_TYPE(sym) == FIOBJ_T_SYMBOL) {
    hash_value = fiobj_sym_id(sym);
  } else {
    fio_cstr_s str = fiobj_obj2cstr(sym);
    if (!str.data) {
      return 0;
    }
    hash_value = fiobj_sym_hash(str.value, str.len);
  }
  FIOBJ coup = fio_hash_find(&obj2hash(hash)->hash, hash_value);
  if (!coup)
    return 0;
  return 1;
}

/**
 * Returns a temporary theoretical Hash map capacity.
 * This could be used for testig performance and memory consumption.
 */
size_t fiobj_hash_capa(const FIOBJ hash) {
  if (!hash) {
    return 0;
  }
  assert(FIOBJ_TYPE(hash) == FIOBJ_T_HASH);
  return fio_hash_capa(&obj2hash(hash)->hash);
}
