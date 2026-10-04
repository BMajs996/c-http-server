#ifndef FILE_CACHE_H
#define FILE_CACHE_H
#include "static_files.h"
#include <stdint.h>
typedef struct file_cache_entry file_cache_entry;
struct file_cache_stats { uintmax_t bytes, entries, hits, misses, evictions, expirations, bypasses; };
file_cache_entry *file_cache_get(const char *path);
file_cache_entry *file_cache_get_variant(const char *path, int gzip);
/* Takes data ownership only on success. Entry comes with one response reference. */
file_cache_entry *file_cache_insert(const char *path, struct static_file *file);
struct static_file file_cache_metadata(const file_cache_entry *entry);
const char *file_cache_data(const file_cache_entry *entry);
void file_cache_release(file_cache_entry *entry);
void file_cache_snapshot(struct file_cache_stats *stats);
void file_cache_close(void);
#endif
