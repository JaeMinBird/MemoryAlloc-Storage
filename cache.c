#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>

#include "cache.h"
#include "jbod.h"

static cache_entry_t *cache = NULL;
static int cache_size = 0;
static int num_queries = 0;
static int num_hits = 0;

int cache_create(int num_entries) {
    // validate no. of cache entries and check if cache has already been made
    if (num_entries < 2 || num_entries > 4096 || cache != NULL) {
        return -1;
    }
    // allocate memory for cache entries
    cache = malloc(num_entries * sizeof(cache_entry_t));
    if (cache == NULL) {
        return -1;
    }
    // init cache entries to 0
    memset(cache, 0, num_entries * sizeof(cache_entry_t));
    cache_size = num_entries;
    return 1;
}

int cache_destroy(void) {
    // check if cache is already destroyed
    if (cache == NULL) {
        return -1;
    }
    // free allocated memory and reset cache vars
    free(cache);
    cache = NULL;
    cache_size = 0;
    return 1;
}

int cache_lookup(int disk_num, int block_num, uint8_t *buf) {
    // check if either entry is NULL
    if (cache == NULL || buf == NULL) {
        return -1;
    }
    num_queries++;
    // iterate through cache to find matching entry
    for (int i = 0; i < cache_size; i++) {
        if (cache[i].valid && cache[i].disk_num == disk_num && cache[i].block_num == block_num) {
            // copy block to buffer and update access count
            memcpy(buf, cache[i].block, JBOD_BLOCK_SIZE);
            cache[i].num_accesses++;
            num_hits++;
            return 1;
        }
    }
    return -1;
}


void cache_update(int disk_num, int block_num, const uint8_t *buf) {
    // update the cache's entry
    if (cache == NULL) {
        return;
    }
    for (int i = 0; i < cache_size; i++) {
        if (cache[i].valid && cache[i].disk_num == disk_num && cache[i].block_num == block_num) {
            memcpy(cache[i].block, buf, JBOD_BLOCK_SIZE);
            cache[i].num_accesses++;
            return;
        }
    }
}

int cache_insert(int disk_num, int block_num, const uint8_t *buf) {
    // check if either buffer is null
    if (cache == NULL || buf == NULL) {
        return -1;
    }
    // validate disk and block numbers
    if (disk_num < 0 || disk_num >= JBOD_NUM_DISKS || block_num < 0 || block_num >= 256) {
        return -1;
    }

    int lfu_index = -1;
    int min_accesses = INT_MAX;

    // track the earliest inserted entry
    int earliest_insert_index = -1;

    for (int i = 0; i < cache_size; i++) {
         // check if entry already exists
        if (cache[i].valid && cache[i].disk_num == disk_num && cache[i].block_num == block_num) {
            return -1;
        }
        if (!cache[i].valid) {
            // find an empty slot in cache
            lfu_index = i;
            break;
        } else if (cache[i].num_accesses < min_accesses) {
            min_accesses = cache[i].num_accesses;
            lfu_index = i;
            earliest_insert_index = i;
        } else if (cache[i].num_accesses == min_accesses) {
            if (earliest_insert_index == -1 || earliest_insert_index > i) {
                // find earliest inserted among those with equal access counts
                earliest_insert_index = i;
                lfu_index = i;
            }
        }
    }

    // if none is found return an error
    if (lfu_index == -1) {
        return -1;
    }

    // insert new entry into cache
    cache[lfu_index].valid = true;
    cache[lfu_index].disk_num = disk_num;
    cache[lfu_index].block_num = block_num;
    memcpy(cache[lfu_index].block, buf, JBOD_BLOCK_SIZE);
    cache[lfu_index].num_accesses = 1;
    return 1;
}

bool cache_enabled(void) {
    // check if cache is enabled
	return cache != NULL && cache_size > 0;
}

void cache_print_hit_rate(void) {
    // print hit rate
	fprintf(stderr, "num_hits: %d, num_queries: %d\n", num_hits, num_queries);
	fprintf(stderr, "Hit rate: %5.1f%%\n", 100 * (float) num_hits / num_queries);
}
