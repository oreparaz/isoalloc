/* iso_alloc.c - A secure memory allocator
 * Copyright 2020 - chris.rohlf@gmail.com */

#include "iso_alloc_internal.h"

/* Select a random number of chunks to be canaries. These
 * can be verified anytime by calling check_canary()
 * or check_canary_no_abort() */
INTERNAL_HIDDEN void create_canary_chunks(iso_alloc_zone *zone) {
    /* Canary chunks are only for default zone sizes. This
     * is because larger zones would waste a lot of memory
     * if we set aside some of their chunks as canaries */
    if(zone->chunk_size > MAX_DEFAULT_ZONE_SZ) {
        return;
    }

    int32_t *bm = (int32_t *) zone->bitmap_start;
    int32_t max_bitmap_idx = (zone->bitmap_size / sizeof(int32_t));
    int32_t chunk_count = (ZONE_USER_SIZE / zone->chunk_size);
    int64_t bit_slot;

    /* Roughly %1 of the chunks in this zone will become a canary */
    zone->canary_count = (chunk_count / CANARY_COUNT_DIV);

    /* This function is only ever called during zone
     * initialization so we don't need to check the
     * current state of any chunks, they're all free.
     * It's possible the call to rand() above picked
     * the same index twice, we can live with that
     * collision as canary chunks only provide a small
     * security property anyway */
    for(int32_t i = 0; i < zone->canary_count; i++) {
        int32_t bm_idx = ALIGN_SZ_DOWN((rand() % max_bitmap_idx));

        if(0 > bm_idx) {
            bm_idx = 0;
        }

        /* Set the 1st and 2nd bits as 1 */
        SET_BIT(bm[bm_idx], 0);
        SET_BIT(bm[bm_idx], 1);
        bit_slot = (bm_idx * BITS_PER_DWORD);
        void *p = POINTER_FROM_BITSLOT(zone, bit_slot);
        write_canary(zone, p);
    }
}

/* Verify the integrity of all canary chunks and the
 * canary written to all free chunks. This function
 * either aborts or returns nothing */
INTERNAL_HIDDEN void verify_all_zones() {
    for(size_t i = 0; i < _root->zones_used; i++) {
        iso_alloc_zone *zone = &_root->zones[i];

        if(zone == NULL) {
            break;
        }

        verify_zone(zone);
    }
}

INTERNAL_HIDDEN void verify_zone(iso_alloc_zone *zone) {
    LOCK_ZONE_MUTEX(zone);
    UNMASK_ZONE_PTRS(zone);
    int32_t *bm = (int32_t *) zone->bitmap_start;
    int64_t bit_slot;
    int32_t bit;

    for(int32_t i = 0; i < zone->bitmap_size / sizeof(int32_t); i++) {
        for(int32_t j = 0; j < BITS_PER_DWORD; j += BITS_PER_CHUNK) {
            bit_slot = (i * BITS_PER_DWORD);
            bit = GET_BIT(bm[i], (j + 1));

            /* Chunk is free but was used, it should have a canary */
            if(bit == 1) {
                void *p = POINTER_FROM_BITSLOT(zone, bit_slot);
                check_canary(zone, p);
            }
        }
    }

    MASK_ZONE_PTRS(zone);
    UNLOCK_ZONE_MUTEX(zone);

    return;
}

/* Pick a random index in the bitmap and start looking
 * for free bit slots we can add to the cache. The random
 * bitmap index is to protect against biasing the free
 * slot cache with only chunks towards the start of the
 * user mapping. Theres no guarantee this function will
 * find any free slots. */
INTERNAL_HIDDEN INLINE void fill_free_bit_slot_cache(iso_alloc_zone *zone) {
    int32_t *bm = (int32_t *) zone->bitmap_start;
    int64_t bit_slot;
    int32_t max_bitmap_idx = zone->bitmap_size / sizeof(int32_t);

    /* This gives us an arbitrary spot in the bitmap to 
     * start searching but may mean we end up with a smaller
     * cache. This will negatively affect performance but
     * leads to a less predictable free list */
    int32_t bm_idx = ALIGN_SZ_DOWN((rand() % max_bitmap_idx / 4));

    if(0 > bm_idx) {
        bm_idx = 0;
    }

    memset(zone->free_bit_slot_cache, BAD_BIT_SLOT, sizeof(zone->free_bit_slot_cache));
    zone->free_bit_slot_cache_usable = 0;

    for(zone->free_bit_slot_cache_index = 0; zone->free_bit_slot_cache_index < BIT_SLOT_CACHE_SZ; bm_idx++) {
        /* Don't index outside of the bitmap or
         * we will return inaccurate bit slots */
        if(bm_idx >= max_bitmap_idx) {
            return;
        }

        for(int j = 0; j < BITS_PER_DWORD; j += BITS_PER_CHUNK) {
            if(zone->free_bit_slot_cache_index >= BIT_SLOT_CACHE_SZ) {
                return;
            }

            int32_t bit = GET_BIT(bm[bm_idx], j);

            if(bit == 0) {
                bit_slot = (bm_idx * BITS_PER_DWORD) + j;
                zone->free_bit_slot_cache[zone->free_bit_slot_cache_index] = bit_slot;
                zone->free_bit_slot_cache_index++;
            }
        }
    }
}

INTERNAL_HIDDEN void insert_free_bit_slot(iso_alloc_zone *zone, int64_t bit_slot) {
#if DEBUG
    for(int32_t i = 0; i < sizeof(zone->free_bit_slot_cache) / sizeof(uint64_t); i++) {
        if(zone->free_bit_slot_cache[i] == bit_slot) {
            LOG_AND_ABORT("bit slot %ld already in zone %d slot cache index=%d (free_bit_slot_cache_index=%d free_bit_slot_cache_usable=%d", bit_slot, zone->index, zone->free_bit_slot_cache_index, zone->free_bit_slot_cache_index, zone->free_bit_slot_cache_usable);
            return;
        }
    }
#endif

    if(zone->free_bit_slot_cache_index >= BIT_SLOT_CACHE_SZ) {
        return;
    }

    zone->free_bit_slot_cache[zone->free_bit_slot_cache_index] = bit_slot;
    zone->free_bit_slot_cache_index++;
}

INTERNAL_HIDDEN int64_t get_next_free_bit_slot(iso_alloc_zone *zone) {
    if(0 > zone->free_bit_slot_cache_usable || zone->free_bit_slot_cache_usable >= BIT_SLOT_CACHE_SZ ||
       zone->free_bit_slot_cache_usable > zone->free_bit_slot_cache_index) {
        return BAD_BIT_SLOT;
    }

    zone->next_free_bit_slot = zone->free_bit_slot_cache[zone->free_bit_slot_cache_usable];
    zone->free_bit_slot_cache[zone->free_bit_slot_cache_usable++] = BAD_BIT_SLOT;
    return zone->next_free_bit_slot;
}

INTERNAL_HIDDEN INLINE void *get_base_page(void *addr) {
    return (void *) ((uintptr_t) addr & ~(_root->system_page_size - 1));
}

INTERNAL_HIDDEN INLINE void iso_clear_user_chunk(uint8_t *p, size_t size) {
    memset(p, POISON_BYTE, size);
}

INTERNAL_HIDDEN INLINE void *mmap_rw_pages(size_t size) {
    size = ROUND_UP_PAGE(size);
    void *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if(p == MAP_FAILED) {
        return NULL;
    }

    return p;
}

INTERNAL_HIDDEN void iso_alloc_new_root() {
    void *p = NULL;

    if(_root == NULL) {
        p = (void *) mmap_rw_pages(sizeof(iso_alloc_root) + (g_page_size * 2));
    }

    if(p == NULL) {
        LOG_AND_ABORT("Cannot allocate pages for _root");
    }

    _root = (iso_alloc_root *) (p + g_page_size);
    _root->system_page_size = g_page_size;

    _root->guard_below = p;
    mprotect(_root->guard_below, _root->system_page_size, PROT_NONE);
    madvise(_root->guard_below, _root->system_page_size, MADV_DONTNEED);

    _root->guard_above = (void *) ROUND_PAGE_UP((uintptr_t)(p + sizeof(iso_alloc_root) + _root->system_page_size));
    mprotect(_root->guard_above, _root->system_page_size, PROT_NONE);
    madvise(_root->guard_above, _root->system_page_size, MADV_DONTNEED);
}

__attribute__((constructor)) void iso_alloc_ctor() {
    struct timeval t;
    gettimeofday(&t, NULL);
    g_page_size = sysconf(_SC_PAGESIZE);

    iso_alloc_new_root();

    for(size_t i = 0; i < sizeof(default_zones) / sizeof(uint32_t); i++) {
        if(!(iso_new_zone(default_zones[i], true))) {
            LOG_AND_ABORT("Failed to create a new zone");
        }
    }

    struct timeval nt;
    gettimeofday(&nt, NULL);
    srand((t.tv_usec * t.tv_sec) + (nt.tv_usec * nt.tv_sec) + getpid());
}

INTERNAL_HIDDEN void _iso_alloc_destroy_zone(iso_alloc_zone *zone) {
    LOCK_ZONE_MUTEX(zone);
    UNMASK_ZONE_PTRS(zone);

    if(zone->internally_managed == false) {
        /* If this zone was a special case then we don't want
         * to reuse any of its backing pages. Mark them unusable
         * and ensure any future accesses result in a segfault */
        mprotect(zone->bitmap_start, zone->bitmap_size, PROT_NONE);
        mprotect(zone->user_pages_start, ZONE_USER_SIZE, PROT_NONE);
        /* Purposefully keep the mutex locked. Any thread
         * that tries to allocate/free in this zone should
         * rightfully deadlock */
    } else {
        munmap(zone->bitmap_start, zone->bitmap_size);
        munmap(zone->bitmap_pages_guard_below, _root->system_page_size);
        munmap(zone->bitmap_pages_guard_above, _root->system_page_size);

        munmap(zone->user_pages_start, ZONE_USER_SIZE);
        munmap(zone->user_pages_guard_below, _root->system_page_size);
        munmap(zone->user_pages_guard_above, _root->system_page_size);

        /* This zone has already been wiped, and we already
         * destroyed the mutex. At this point a race condition
         * is unlikely */
#if THREAD_SUPPORT
        pthread_mutex_destroy(&zone->mutex);
#endif
        memset(zone, 0x0, sizeof(iso_alloc_zone));
    }
}

__attribute__((destructor)) void iso_alloc_dtor() {
#if DEBUG || LEAK_DETECTOR || MEM_USAGE
    uint64_t mb = 0;

    for(size_t i = 0; i < _root->zones_used; i++) {
        iso_alloc_zone *zone = &_root->zones[i];

        if(zone == NULL) {
            break;
        }

        _iso_alloc_zone_leak_detector(zone);
        mb += _iso_alloc_zone_mem_usage(zone);
    }

#if MEM_USAGE
    LOG("Total megabytes consumed by all zones: %ld", mb);
#endif

#endif

    for(int32_t i = 0; i < _root->zones_used; i++) {
        iso_alloc_zone *zone = &_root->zones[i];

        if(zone == NULL) {
            break;
        }

        verify_zone(zone);
        _iso_alloc_destroy_zone(zone);
    }

    munmap(_root->guard_below, _root->system_page_size);
    munmap(_root->guard_above, _root->system_page_size);
    munmap(_root, sizeof(iso_alloc_root));
}

INTERNAL_HIDDEN iso_alloc_zone *iso_new_zone(size_t size, bool internal) {
    if(_root->zones_used >= MAX_ZONES) {
        LOG_AND_ABORT("Cannot allocate additional zones");
    }

    if((size % ALIGNMENT) != 0) {
        size = ROUND_UP_SZ(size);
    }

    iso_alloc_zone *new_zone = &_root->zones[_root->zones_used];
    LOCK_ZONE_MUTEX(new_zone);

    new_zone->internally_managed = internal;
    new_zone->is_full = false;
    new_zone->chunk_size = size;
    new_zone->bitmap_size = (GET_CHUNK_COUNT(new_zone) * BITS_PER_CHUNK) / BITS_PER_BYTE;

#if THREAD_SUPPORT
    if((pthread_mutex_init(&new_zone->mutex, NULL)) != 0) {
        LOG_AND_ABORT("Cannot initialize mutex for zone")
    }
#endif

    /* Most of these fields are effectively immutable
     * and should not change once they are set */
    void *p = mmap_rw_pages(new_zone->bitmap_size + (_root->system_page_size * 2));
    new_zone->bitmap_pages_guard_below = p;
    new_zone->bitmap_start = (p + _root->system_page_size);
    new_zone->bitmap_end = (p + (new_zone->bitmap_size + _root->system_page_size));
    new_zone->bitmap_pages_guard_above = (void *) ROUND_PAGE_UP((uintptr_t) p + (new_zone->bitmap_size + _root->system_page_size));

    mprotect(new_zone->bitmap_pages_guard_below, _root->system_page_size, PROT_NONE);
    madvise(new_zone->bitmap_pages_guard_below, _root->system_page_size, MADV_DONTNEED);

    mprotect(new_zone->bitmap_pages_guard_above, _root->system_page_size, PROT_NONE);
    madvise(new_zone->bitmap_pages_guard_above, _root->system_page_size, MADV_DONTNEED);

    /* Bitmap pages are accessed often and usually in sequential order */
    madvise(new_zone->bitmap_start, new_zone->bitmap_size, MADV_WILLNEED);
    madvise(new_zone->bitmap_start, new_zone->bitmap_size, MADV_SEQUENTIAL);

    p = mmap_rw_pages(ZONE_USER_SIZE + (_root->system_page_size * 2));

    new_zone->user_pages_guard_below = p;
    new_zone->user_pages_start = (p + _root->system_page_size);
    new_zone->user_pages_end = (p + (_root->system_page_size + ZONE_USER_SIZE));
    new_zone->user_pages_guard_above = (void *) ROUND_PAGE_UP((uintptr_t) p + (ZONE_USER_SIZE + _root->system_page_size));

    mprotect(new_zone->user_pages_guard_below, _root->system_page_size, PROT_NONE);
    madvise(new_zone->user_pages_guard_below, _root->system_page_size, MADV_DONTNEED);

    mprotect(new_zone->user_pages_guard_above, _root->system_page_size, PROT_NONE);
    madvise(new_zone->user_pages_guard_above, _root->system_page_size, MADV_DONTNEED);

    /* User pages will be accessed in an unpredictable order */
    madvise(new_zone->user_pages_start, ZONE_USER_SIZE, MADV_WILLNEED);
    madvise(new_zone->user_pages_start, ZONE_USER_SIZE, MADV_RANDOM);

    new_zone->index = _root->zones_used;
    new_zone->canary_secret = (rand() % 0xffffffffffffffff);
    new_zone->pointer_mask = (rand() % 0xffffffffffffffff);

    /* This should be the only place we call this function */
    create_canary_chunks(new_zone);

    /* When we create a new zone its an opportunity to
     * populate our free list cache with random entries */
    fill_free_bit_slot_cache(new_zone);

    /* Prime the next_free_bit_slot member */
    get_next_free_bit_slot(new_zone);

    MASK_ZONE_PTRS(new_zone);

    _root->zones_used++;
    UNLOCK_ZONE_MUTEX(new_zone);

    return new_zone;
}

/* Iterate through a zone bitmap a dword at a time
 * looking for empty holes (i.e. slot == 0) */
INTERNAL_HIDDEN int64_t iso_scan_zone_free_slot(iso_alloc_zone *zone) {
    int32_t *bm = (int32_t *) zone->bitmap_start;
    int64_t bit_position = BAD_BIT_SLOT;

    /* Iterate the entire bitmap a dword at a time */
    for(int32_t i = 0; i < zone->bitmap_size / sizeof(int32_t); i++) {
        /* If the byte is 0 then there are some free
         * slots we can use at this location */
        if(bm[i] == 0x0) {
            bit_position = (i * BITS_PER_DWORD);
            return bit_position;
        }
    }

    return bit_position;
}

/* This function scans an entire bitmap bit-by-bit
 * and returns the first free bit position. In a heavily
 * used zone this function will be slow to search */
INTERNAL_HIDDEN INLINE int64_t iso_scan_zone_free_slot_slow(iso_alloc_zone *zone) {
    int32_t *bm = (int32_t *) zone->bitmap_start;
    int64_t bit_position = BAD_BIT_SLOT;
    int32_t bit;

    for(int32_t i = 0; i < zone->bitmap_size / sizeof(int32_t); i++) {
        for(int32_t j = 0; j < BITS_PER_DWORD; j += BITS_PER_CHUNK) {
            bit = GET_BIT(bm[i], j);

            if(bit == 0) {
                bit_position = (i * BITS_PER_DWORD) + j;
                return bit_position;
            }
        }
    }

    return bit_position;
}

/* Finds a zone that can fit this allocation request */
INTERNAL_HIDDEN iso_alloc_zone *iso_find_zone_fit(size_t size) {
    iso_alloc_zone *zone = NULL;

    for(int32_t i = 0; i < _root->zones_used; i++) {
        zone = &_root->zones[i];

        if(zone == NULL) {
            return NULL;
        }

        if(zone->chunk_size < size || zone->internally_managed == false || zone->is_full == true) {
            continue;
        }

        /* We found a zone, lets try to find a free slot in it */
        if(zone->chunk_size >= size) {
            if(zone->next_free_bit_slot != BAD_BIT_SLOT) {
                return zone;
            }

            UNMASK_ZONE_PTRS(zone);

            /* This zone may fit this chunk but if the zone was
             * created for chunks more than N* larger than the
             * requested allocation size then we would be wasting
             * a lot memory by using it. Lets force the creation
             * of a new zone instead. We only do this for sizes
             * beyond the max default zone size */
            if(zone->chunk_size >= (size * WASTED_SZ_MULTIPLIER) && size > MAX_DEFAULT_ZONE_SZ) {
                MASK_ZONE_PTRS(zone);
                return NULL;
            }

            /* If the cache for this zone is empty we should
             * refill it to make future allocations faster */
            if(zone->free_bit_slot_cache_usable == zone->free_bit_slot_cache_index) {
                fill_free_bit_slot_cache(zone);
            }

            int64_t bit_slot = get_next_free_bit_slot(zone);

            if(bit_slot != BAD_BIT_SLOT) {
                MASK_ZONE_PTRS(zone);
                return zone;
            }

            /* Free list failed, use a fast search */
            bit_slot = iso_scan_zone_free_slot(zone);

            if(bit_slot == BAD_BIT_SLOT) {
                /* Fast search failed, search bit by bit */
                bit_slot = iso_scan_zone_free_slot_slow(zone);
                MASK_ZONE_PTRS(zone);

                /* This zone may be entirely full, try the next one
                 * but mark this zone full so future allocations can
                 * take a faster path */
                if(bit_slot == BAD_BIT_SLOT) {
                    zone->is_full = true;
                    continue;
                } else {
                    zone->next_free_bit_slot = bit_slot;
                    return zone;
                }
            } else {
                zone->next_free_bit_slot = bit_slot;
                MASK_ZONE_PTRS(zone);
                return zone;
            }
        }
    }

    return NULL;
}

INTERNAL_HIDDEN void *_iso_calloc(size_t nmemb, size_t size) {
    if(nmemb > (nmemb * size)) {
        LOG_AND_ABORT("Call to calloc() will overflow nmemb=%zu size=%zu", nmemb, size);
        return NULL;
    }

    void *p = _iso_alloc(nmemb * size, NULL);

    memset(p, 0x0, nmemb * size);
    return p;
}

INTERNAL_HIDDEN void *_iso_alloc(size_t size, iso_alloc_zone *zone) {
    if(zone == NULL) {
        zone = iso_find_zone_fit(size);
    }

    int64_t free_bit_slot = BAD_BIT_SLOT;

    if(zone == NULL) {
        /* In order to guarantee an 8 byte memory alignment
         * for all allocations we only create zones that
         * work with default allocation sizes */
        for(size_t i = 0; i < sizeof(default_zones) / sizeof(uint32_t); i++) {
            if(size < default_zones[i]) {
                size = default_zones[i];
                zone = iso_new_zone(size, true);

                if(zone == NULL) {
                    LOG_AND_ABORT("Failed to create a new zone for allocation of %zu bytes", size);
                } else {
                    break;
                }
            }
        }

        /* The size requested is above default zone sizes
         * but we can still create it. iso_new_zone will
         * align the requested size for us */
        if(zone == NULL) {
            zone = iso_new_zone(size, true);

            if(zone == NULL) {
                LOG_AND_ABORT("Failed to create a zone for allocation of %zu bytes", size);
            }
        }

        LOCK_ZONE_MUTEX(zone);

        /* This is a brand new zone, so the fast path
         * should always work. Abort if it doesn't */
        free_bit_slot = zone->next_free_bit_slot;

        if(free_bit_slot == BAD_BIT_SLOT) {
            LOG_AND_ABORT("Allocated a new zone with no free bit slots");
        }
    } else {
        LOCK_ZONE_MUTEX(zone);
        free_bit_slot = zone->next_free_bit_slot;
    }

    if(free_bit_slot == BAD_BIT_SLOT) {
        return NULL;
    }

    UNMASK_ZONE_PTRS(zone);

    zone->next_free_bit_slot = BAD_BIT_SLOT;

    int32_t dwords_to_bit_slot = (free_bit_slot / BITS_PER_DWORD);
    int64_t remainder = (free_bit_slot % BITS_PER_DWORD);

    void *p = POINTER_FROM_BITSLOT(zone, free_bit_slot);
    int32_t *bm = (int32_t *) zone->bitmap_start;
    int32_t b = bm[dwords_to_bit_slot];

    if(p > zone->user_pages_end) {
        LOG_AND_ABORT("Allocating an address %p from zone[%d], bit slot %ld %ld bytes %ld pages outside zones user pages %p %p",
                      p, zone->index, free_bit_slot, p - zone->user_pages_end, (p - zone->user_pages_end) / _root->system_page_size, zone->user_pages_start, zone->user_pages_end);
    }

    if((GET_BIT(b, remainder)) != 0) {
        LOG_AND_ABORT("Zone[%d] for chunk size %zu Cannot return allocated chunk at %p bitmap location @ %p. bit slot was %ld, remainder was %ld",
                      zone->index, zone->chunk_size, p, &bm[dwords_to_bit_slot], free_bit_slot, remainder);
    }

    /* This chunk was previously allocated and free'd which
     * means it must have a canary written in its first dword.
     * Here we check the validity of that canary and abort
     * if its been corrupted */
    if((GET_BIT(b, (remainder + 1))) == 1) {
        check_canary(zone, p);
        memset(p, 0x0, CANARY_SIZE);
    }

    /* Set the in-use bit */
    SET_BIT(b, remainder);

    /* The second bit is flipped to 0 while in use. This
     * is because a previously in use chunk would have
     * a bit pattern of 11 which makes it looks the same
     * as a canary chunk. This bit is set again upon free. */
    UNSET_BIT(b, (remainder + 1));

    bm[dwords_to_bit_slot] = b;

    MASK_ZONE_PTRS(zone);
    UNLOCK_ZONE_MUTEX(zone);

    return p;
}

INTERNAL_HIDDEN iso_alloc_zone *iso_find_zone_range(void *p) {
    iso_alloc_zone *zone = NULL;

    for(int32_t i = 0; i <= _root->zones_used; i++) {
        zone = &_root->zones[i];

        if(zone == NULL) {
            LOG_AND_ABORT("Zone pointer overwritten with NULL");
        }

        if(i == _root->zones_used) {
            LOG_AND_ABORT("Passed a pointer that wasn't allocated by iso_alloc %p start=%p end=%p", p, zone->user_pages_start, zone->user_pages_end);
        }

        UNMASK_ZONE_PTRS(zone);

        if(zone->user_pages_start <= p && zone->user_pages_end > p) {
            MASK_ZONE_PTRS(zone);
            return zone;
        }

        MASK_ZONE_PTRS(zone);
    }

    if(zone == NULL) {
        LOG_AND_ABORT("Cannot free %p without a zone", p);
    }

    return NULL;
}

/* All free chunks get a canary written at both
 * the start and end of their chunks. These canaries
 * are verified when adjacent chunks are allocated,
 * freed, or when the API requests validation */
INTERNAL_HIDDEN INLINE void write_canary(iso_alloc_zone *zone, void *p) {
    uint64_t canary = zone->canary_secret ^ (uint64_t) p;
    memcpy(p, &canary, CANARY_SIZE);
}

/* Verify the canary value in an allocation */
INTERNAL_HIDDEN INLINE void check_canary(iso_alloc_zone *zone, void *p) {
    uint64_t c = (uint64_t) * (uint64_t *) p;

    if(c != (uint64_t)(zone->canary_secret ^ (uint64_t) p)) {
        LOG_AND_ABORT("Canary at chunk %p has been corrupted! 0x%lx 0x%lx", p, c, (uint64_t)(zone->canary_secret ^ (uint64_t) p));
    }
}

INTERNAL_HIDDEN INLINE int32_t check_canary_no_abort(iso_alloc_zone *zone, void *p) {
    uint64_t c = (uint64_t) * (uint64_t *) p;

    if(c != (uint64_t)(zone->canary_secret ^ (uint64_t) p)) {
        return ERR;
    }

    return OK;
}

INTERNAL_HIDDEN void iso_free_chunk_from_zone(iso_alloc_zone *zone, void *p, bool permanent) {
    uint64_t chunk_offset = (uint64_t)(p - zone->user_pages_start);

    if(chunk_offset % zone->chunk_size != 0) {
        LOG_AND_ABORT("Chunk at %p is not a multiple of zone %d chunk size %zu", p, zone->index, zone->chunk_size);
    }

    size_t chunk_number = chunk_offset / zone->chunk_size;
    int64_t bit_position = (chunk_number * BITS_PER_CHUNK);

    int64_t dwords_to_bit_slot = (bit_position / BITS_PER_DWORD);
    int32_t remainder = (bit_position % BITS_PER_DWORD);

    if((zone->bitmap_start + dwords_to_bit_slot) >= zone->bitmap_end) {
        LOG_AND_ABORT("Cannot calculate this chunks location in the bitmap %p", p);
    }

    /* TODO make configurable */
    iso_clear_user_chunk(p, zone->chunk_size);

    int32_t *bm = (int32_t *) zone->bitmap_start;
    int32_t b = bm[dwords_to_bit_slot];

    /* Double free detection */
    /* TODO: make configurable */
    if((GET_BIT(b, remainder)) == 0) {
        LOG_AND_ABORT("Double free of chunk %p detected from zone[%d] dwords_to_bit_slot=%ld bit_position=%ld", p, zone->index, dwords_to_bit_slot, bit_position);
    }

    /* Set the next bit so we know this chunk was used */
    SET_BIT(b, (remainder + 1));

    /* Unset the bit and write the value into the bitmap
     * if this is not a permanent free. A permanent free
     * means this chunk will be marked as if it is a canary */
    if(permanent == false) {
        UNSET_BIT(b, remainder);
    }

    bm[dwords_to_bit_slot] = b;

    write_canary(zone, p);

    insert_free_bit_slot(zone, bit_position);
    zone->is_full = false;

    return;
}

INTERNAL_HIDDEN void _iso_free(void *p, bool permanent) {
    if(p == NULL) {
        return;
    }

    iso_alloc_zone *zone = iso_find_zone_range(p);

    if(zone == NULL) {
        return;
    }

    LOCK_ZONE_MUTEX(zone);
    UNMASK_ZONE_PTRS(zone);

    iso_free_chunk_from_zone(zone, p, permanent);

    MASK_ZONE_PTRS(zone);
    UNLOCK_ZONE_MUTEX(zone);
}

/* Disable all use of iso_alloc by protecting the _root */
INTERNAL_HIDDEN void _iso_alloc_protect_root() {
    mprotect(_root, sizeof(iso_alloc_root), PROT_NONE);
}

/* Unprotect all use of iso_alloc by allowing R/W of the _root */
INTERNAL_HIDDEN void _iso_alloc_unprotect_root() {
    mprotect(_root, sizeof(iso_alloc_root), PROT_READ | PROT_WRITE);
}

INTERNAL_HIDDEN int32_t _iso_chunk_size(void *p) {
    iso_alloc_zone *zone = iso_find_zone_range(p);

    if(zone == NULL) {
        return 0;
    }

    return zone->chunk_size;
}
