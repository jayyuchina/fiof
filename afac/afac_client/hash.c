/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "jenkins_hash.h"
#include "murmur3_hash.h"
#include "thomas_jenkins_hash.h"
#include "hash.h"

int hash_init(enum hashfunc_type type) {
    switch(type) {
        case JENKINS_HASH:
            hash = jenkins_hash;
            break;
        case MURMUR3_HASH:
            hash = MurmurHash3_x86_32_no_seed;
            break;
	case THOMAS_JENKINS_HASH:
	    hash = thomas_jenkins_hash;
	    break;
        default:
            return -1;
    }
    return 0;
}

