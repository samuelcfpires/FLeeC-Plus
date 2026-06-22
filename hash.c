/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "memcached.h"
#include "jenkins_hash.h"
#include "murmur3_hash.h"
#define XXH_INLINE_ALL // modifier for xxh3's include below
#include "xxhash.h"

hash_func hash;

static uint64_t XXH3_hash(const void *key, size_t length) {
    return XXH3_64bits(key, length);
}

static uint64_t jenkins_hash64(const void *key, size_t length) {
    uint32_t h32 = jenkins_hash(key, length);
    return ((uint64_t)h32 << 32) | h32;
}

static uint64_t MurmurHash3_x86_64(const void *key, size_t length) {
    uint32_t h32 = MurmurHash3_x86_32(key, length);
    return ((uint64_t)h32 << 32) | h32;
}

int hash_init(enum hashfunc_type type) {
    switch(type) {
        case JENKINS_HASH:
            hash = jenkins_hash64;
            settings.hash_algorithm = "jenkins";
            break;
        case MURMUR3_HASH:
            hash = MurmurHash3_x86_64;
            settings.hash_algorithm = "murmur3";
            break;
        case XXH3_HASH:
            hash = XXH3_hash;
            settings.hash_algorithm = "xxh3";
            break;
        default:
            return -1;
    }
    return 0;
}
