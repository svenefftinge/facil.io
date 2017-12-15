#ifndef H_FIO_SIMPLE_HASH_H
/*
Copyright: Boaz Segev, 2017
License: MIT
*/

/**
 * A simple ordered Hash Table implementation, offering a minimal API.
 *
 * This Hash Table has zero tolerance for hash value collisions.
 *
 * Bin collisions are handled by seeking forward (in leaps) and attempting to
 * find a close enough spot. If a close enough spot isn't found, rehashing is
 * initiated.
 *
 * The Hash Table is ordered using an internal linked list of data containers
 * with duplicates of the hash key data.
 */
#define H_FIO_SIMPLE_HASH_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef FIO_FUNC
#define FIO_FUNC static __attribute__((unused))
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef HASH_INITIAL_CAPACITY
/* MUST be a power of 2 */
#define HASH_INITIAL_CAPACITY 16
#endif

#ifndef FIO_HASH_MAX_MAP_SEEK
/* MUST be a power of 2 */
#define FIO_HASH_MAX_MAP_SEEK (256)
#endif

/* *****************************************************************************
Hash API
***************************************************************************** */

/** The Hash Table container type. */
typedef struct fio_hash_s fio_hash_s;

/** Allocates and initializes internal data and resources. */
FIO_FUNC void fio_hash_new(fio_hash_s *h);

/** Deallocates any internal resources. */
FIO_FUNC void fio_hash_free(fio_hash_s *hash);

/** Locates an object in the Hash Map according to the hash key value. */
inline FIO_FUNC void *fio_hash_find(fio_hash_s *hash, uintptr_t key);

/** Locates an object in the Hash Map according to the hash key value. */
FIO_FUNC void *fio_hash_find(fio_hash_s *hash, uintptr_t key);

/** Forces a rehashinh of the hash. */
FIO_FUNC void fio_hash_rehash(fio_hash_s *hash);

/**
 * Single layer iteration using a callback for each entry in the Hash Table.
 *
 * The callback task function must accept the hash key, the entry data and an
 * opaque user pointer.
 *
 * If the callback returns -1, the loop is broken. Any other value is ignored.
 *
 * Returns the relative "stop" position, i.e., the number of items processed +
 * the starting point.
 */
FIO_FUNC size_t fio_hash_each(fio_hash_s *hash, const size_t start_at,
                              int (*task)(uintptr_t key, void *obj, void *arg),
                              void *arg);

/** Returns the number of elements currently in the Hash Table. */
inline FIO_FUNC size_t fio_hash_count(const fio_hash_s *hash);

/**
 * Returns a temporary theoretical Hash map capacity.
 * This could be used for testig performance and memory consumption.
 */
inline FIO_FUNC size_t fio_hash_capa(const fio_hash_s *hash);

/* *****************************************************************************
Hash Types and other required perlimenaries
***************************************************************************** */

typedef struct fio_hash_ls_s {
  struct fio_hash_ls_s *prev;
  struct fio_hash_ls_s *next;
  uintptr_t key;
  const void *obj;
} fio_hash_ls_s;

typedef struct {
  uintptr_t key; /* another copy for memory cache locality */
  fio_hash_ls_s *container;
} fio_hash_map_info_s;

typedef struct {
  uintptr_t capa;
  fio_hash_map_info_s *data;
} fio_hash_map_s;

struct fio_hash_s {
  uintptr_t count;
  uintptr_t mask;
  fio_hash_ls_s items;
  fio_hash_map_s map;
};

/* *****************************************************************************
Hash Data Container Linked List - (object + hash key).
***************************************************************************** */

#define FIO_HASH_LS_INIT(name)                                                 \
  { .next = &(name), .prev = &(name) }

/** Adds an object to the list's head. */
inline FIO_FUNC void fio_hash_ls_push(fio_hash_ls_s *pos, const void *obj,
                                      uintptr_t key) {
  /* prepare item */
  fio_hash_ls_s *item = (fio_hash_ls_s *)malloc(sizeof(*item));
  if (!item)
    perror("ERROR: hash list couldn't allocate memory"), exit(errno);
  *item =
      (fio_hash_ls_s){.prev = pos, .next = pos->next, .obj = obj, .key = key};
  /* inject item */
  pos->next->prev = item;
  pos->next = item;
}

/** Adds an object to the list's tail. */
inline FIO_FUNC void fio_hash_ls_unshift(fio_hash_ls_s *pos, const void *obj,
                                         uintptr_t key) {
  pos = pos->prev;
  fio_hash_ls_push(pos, obj, key);
}

/** Removes an object from the list's head. */
inline FIO_FUNC void *fio_hash_ls_pop(fio_hash_ls_s *list) {
  if (list->next == list)
    return NULL;
  fio_hash_ls_s *item = list->next;
  const void *ret = item->obj;
  list->next = item->next;
  list->next->prev = list;
  free(item);
  return (void *)ret;
}

/** Removes an object from the list's tail. */
inline FIO_FUNC void *fio_hash_ls_shift(fio_hash_ls_s *list) {
  if (list->prev == list)
    return NULL;
  fio_hash_ls_s *item = list->prev;
  const void *ret = item->obj;
  list->prev = item->prev;
  list->prev->next = list;
  free(item);
  return (void *)ret;
}

/** Removes an object from the containing node. */
inline FIO_FUNC void *fio_hash_ls_remove(fio_hash_ls_s *node) {
  const void *ret = node->obj;
  node->next->prev = node->prev->next;
  node->prev->next = node->next->prev;
  free(node);
  return (void *)ret;
}

/* *****************************************************************************
Hash allocation / deallocation.
***************************************************************************** */

FIO_FUNC void fio_hash_new(fio_hash_s *h) {
  *h = (fio_hash_s){
      .mask = (HASH_INITIAL_CAPACITY - 1),
      .items = FIO_HASH_LS_INIT((h->items)),
      .map.data = (fio_hash_map_info_s *)calloc(sizeof(*h->map.data),
                                                HASH_INITIAL_CAPACITY),
      .map.capa = HASH_INITIAL_CAPACITY,
  };
  if (!h->map.data)
    perror("ERROR: Hash Table couldn't allocate memory"), exit(errno);
}

FIO_FUNC void fio_hash_free(fio_hash_s *h) {
  while (fio_hash_ls_pop(&h->items))
    ;
  free(h->map.data);
  h->map.data = NULL;
  h->map.capa = 0;
}

/* *****************************************************************************
Internal HashMap Functions
***************************************************************************** */
inline FIO_FUNC uintptr_t fio_hash_map_cuckoo_steps(uintptr_t step) {
  // return ((step * (step + 1)) >> 1);
  return (step * 3);
}

/* seeks the hash's position in the map */
FIO_FUNC fio_hash_map_info_s *fio_hash_seek(fio_hash_s *hash, uintptr_t key) {
  /* TODO: consider implementing Robing Hood reordering during seek? */
  fio_hash_map_info_s *pos = hash->map.data + (key & hash->mask);
  uintptr_t i = 0;
  const uintptr_t limit = hash->map.capa > FIO_HASH_MAX_MAP_SEEK
                              ? FIO_HASH_MAX_MAP_SEEK
                              : (hash->map.capa >> 1);
  while (i < limit) {
    if (!pos->key || pos->key == key)
      return pos;
    pos = hash->map.data +
          (((key & hash->mask) + fio_hash_map_cuckoo_steps(i++)) & hash->mask);
  }
  return NULL;
}

/* finds an object in the map */
inline FIO_FUNC void *fio_hash_find(fio_hash_s *hash, uintptr_t key) {
  fio_hash_map_info_s *info = fio_hash_seek(hash, key);
  if (!info || !info->container)
    return NULL;
  return (void *)info->container->obj;
}

/* inserts an object to the map, rehashing if required, returning old object.
 * set obj to NULL to remove existing data.
 */
static void *fio_hash_insert(fio_hash_s *hash, uintptr_t key, void *obj) {
  fio_hash_map_info_s *info = fio_hash_seek(hash, key);
  if (!info && !obj)
    return NULL;
  while (!info) {
    fio_hash_rehash(hash);
    info = fio_hash_seek(hash, key);
  }
  if (!info->container) {
    /* a fresh object */
    if (obj == NULL)
      return NULL; /* nothing to delete */
    /* create container and set hash */
    fio_hash_ls_unshift(&hash->items, obj, key);
    *info = (fio_hash_map_info_s){.key = key, .container = hash->items.prev};
    hash->count++;
    return NULL;
  }
  /* a container object exists, this is a "replace/delete" operation */
  if (!obj) {
    /* delete */
    hash->count--;
    obj = fio_hash_ls_remove(info->container);
    *info = (fio_hash_map_info_s){
        .key = key}; /* hash is set to seek over position */
    return obj;
  }
  /* replace */
  void *old = (void *)info->container->obj;
  info->container->obj = obj;
  return old;
}

/* attempts to rehash the hashmap. */
void FIO_FUNC fio_hash_rehash(fio_hash_s *h) {
retry_rehashing:
  h->mask = ((h->mask) << 1) | 1;
  {
    /* It's better to reallocate using calloc than manually zero out memory */
    /* Maybe there's enough zeroed out pages available in the system */
    h->map.capa = h->mask + 1;
    free(h->map.data);
    h->map.data =
        (fio_hash_map_info_s *)calloc(sizeof(*h->map.data), h->map.capa);
    if (!h->map.data)
      perror("HashMap Allocation Failed"), exit(errno);
  }
  fio_hash_ls_s *pos = h->items.next;
  while (pos != &h->items) {
    /* can't use fio_hash_insert, because we're recycling containers */
    fio_hash_map_info_s *info = fio_hash_seek(h, pos->key);
    if (!info) {
      goto retry_rehashing;
    }
    *info = (fio_hash_map_info_s){.key = pos->key, .container = pos};
    pos = pos->next;
  }
}

FIO_FUNC size_t fio_hash_each(fio_hash_s *hash, const size_t start_at,
                              int (*task)(uintptr_t key, void *obj, void *arg),
                              void *arg) {
  if (start_at >= hash->count)
    return hash->count;
  size_t i = 0;
  fio_hash_ls_s *pos = hash->items.next;
  while (pos != &hash->items && start_at > i) {
    pos = pos->next;
    ++i;
  }
  while (pos != &hash->items) {
    ++i;
    if (task(pos->key, (void *)pos->obj, arg) == -1)
      return i;
    pos = pos->next;
  }
  return i;
}

/** Returns the number of elements in the Hash. */
inline FIO_FUNC size_t fio_hash_count(const fio_hash_s *hash) {
  if (!hash)
    return 0;
  return hash->count;
}

/**
 * Returns a temporary theoretical Hash map capacity.
 * This could be used for testig performance and memory consumption.
 */
inline FIO_FUNC size_t fio_hash_capa(const fio_hash_s *hash) {
  if (!hash)
    return 0;
  return hash->map.capa;
}

#endif