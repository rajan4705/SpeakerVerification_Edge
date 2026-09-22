/*
 * tinfl.c - Tiny Inflate / Zlib decompressor
 * Copyright (c) 2003-2019 Joergen Ibsen
 * zlib license
 */
#include "tinf.h"

typedef struct {
    const unsigned char *source;
    unsigned int tag;
    int bitcount;
    unsigned char *dest;
    unsigned int destLen;
    unsigned int cur_dest;
} TINF_DATA;

typedef struct {
    unsigned short table[16];
    unsigned short trans[288];
} TINF_TREE;

static const unsigned char tinf_default_lengths[288] = {
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8
};

static const unsigned char tinf_default_distance[32] = {
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
};

static const unsigned char tinf_clcidx[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static const unsigned short tinf_length_base[31] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0
};

static const unsigned char tinf_length_extra[31] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0
};

static const unsigned short tinf_dist_base[32] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577, 0, 0
};

static const unsigned char tinf_dist_extra[32] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 0, 0
};

static unsigned int tinf_getbits(TINF_DATA *d, int num) {
    unsigned int val = 0;
    int i;
    for (i = 0; i < num; ++i) {
        if (!d->bitcount) {
            d->tag = *d->source++;
            d->bitcount = 8;
        }
        val |= ((d->tag & 1) << i);
        d->tag >>= 1;
        d->bitcount--;
    }
    return val;
}

static void tinf_build_tree(TINF_TREE *t, const unsigned char *lengths, unsigned int num) {
    unsigned short offs[16];
    unsigned int i, sum = 0;
    for (i = 0; i < 16; ++i) t->table[i] = 0;
    for (i = 0; i < num; ++i) t->table[lengths[i]]++;
    t->table[0] = 0;
    for (i = 0; i < 16; ++i) {
        offs[i] = sum;
        sum += t->table[i];
    }
    for (i = 0; i < num; ++i) {
        if (lengths[i]) t->trans[offs[lengths[i]]++] = i;
    }
}

static int tinf_decode_symbol(TINF_DATA *d, const TINF_TREE *t) {
    int sum = 0, cur = 0, len = 0;
    do {
        cur = (cur << 1) | tinf_getbits(d, 1);
        len++;
        sum += t->table[len];
        cur -= t->table[len];
    } while (cur >= 0);
    return t->trans[sum + cur];
}

static int tinf_inflate_block_data(TINF_DATA *d, const TINF_TREE *lt, const TINF_TREE *dt) {
    while (1) {
        int sym = tinf_decode_symbol(d, lt);
        if (sym < 256) {
            if (d->cur_dest >= d->destLen) return TINF_BUF_ERROR;
            d->dest[d->cur_dest++] = (unsigned char)sym;
        } else if (sym == 256) {
            return TINF_OK;
        } else {
            int length, dist, i;
            sym -= 257;
            length = tinf_length_base[sym] + tinf_getbits(d, tinf_length_extra[sym]);
            sym = tinf_decode_symbol(d, dt);
            dist = tinf_dist_base[sym] + tinf_getbits(d, tinf_dist_extra[sym]);
            if (d->cur_dest + length > d->destLen) return TINF_BUF_ERROR;
            for (i = 0; i < length; ++i) {
                d->dest[d->cur_dest] = d->dest[d->cur_dest - dist];
                d->cur_dest++;
            }
        }
    }
}

static int tinf_inflate_uncompressed_block(TINF_DATA *d) {
    unsigned int length, invlength;
    d->bitcount = 0;
    length = d->source[0] | (d->source[1] << 8);
    d->source += 2;
    invlength = d->source[0] | (d->source[1] << 8);
    d->source += 2;
    if (length != (~invlength & 0x0000ffff)) return TINF_DATA_ERROR;
    if (d->cur_dest + length > d->destLen) return TINF_BUF_ERROR;
    while (length--) d->dest[d->cur_dest++] = *d->source++;
    return TINF_OK;
}

static int tinf_inflate_fixed_block(TINF_DATA *d) {
    TINF_TREE lt, dt;
    tinf_build_tree(&lt, tinf_default_lengths, 288);
    tinf_build_tree(&dt, tinf_default_distance, 32);
    return tinf_inflate_block_data(d, &lt, &dt);
}

static int tinf_inflate_dynamic_block(TINF_DATA *d) {
    TINF_TREE lt, dt, ct;
    unsigned char lengths[288 + 32];
    unsigned char clen[19];
    unsigned int hlit, hdist, hclen, num, i;
    hlit = tinf_getbits(d, 5) + 257;
    hdist = tinf_getbits(d, 5) + 1;
    hclen = tinf_getbits(d, 4) + 4;
    for (i = 0; i < 19; ++i) clen[i] = 0;
    for (i = 0; i < hclen; ++i) clen[tinf_clcidx[i]] = tinf_getbits(d, 3);
    tinf_build_tree(&ct, clen, 19);
    num = hlit + hdist;
    i = 0;
    while (i < num) {
        int sym = tinf_decode_symbol(d, &ct);
        if (sym < 16) {
            lengths[i++] = sym;
        } else if (sym == 16) {
            unsigned char val = lengths[i - 1];
            unsigned int count = tinf_getbits(d, 2) + 3;
            while (count--) lengths[i++] = val;
        } else if (sym == 17) {
            unsigned int count = tinf_getbits(d, 3) + 3;
            while (count--) lengths[i++] = 0;
        } else if (sym == 18) {
            unsigned int count = tinf_getbits(d, 7) + 11;
            while (count--) lengths[i++] = 0;
        }
    }
    tinf_build_tree(&lt, lengths, hlit);
    tinf_build_tree(&dt, lengths + hlit, hdist);
    return tinf_inflate_block_data(d, &lt, &dt);
}

int tinf_zlib_uncompress(void *dest, unsigned int *destLen, const void *source, unsigned int sourceLen) {
    TINF_DATA d;
    int bfinal, btype, res;
    const unsigned char *src = (const unsigned char *)source;
    if (sourceLen < 6) return TINF_DATA_ERROR;
    /* Check zlib header (CM=8, CINFO=7) */
    if ((src[0] & 0x0f) != 8 || ((src[0] * 256 + src[1]) % 31 != 0)) return TINF_DATA_ERROR;
    src += 2;
    d.source = src;
    d.bitcount = 0;
    d.dest = (unsigned char *)dest;
    d.destLen = *destLen;
    d.cur_dest = 0;
    do {
        bfinal = tinf_getbits(&d, 1);
        btype = tinf_getbits(&d, 2);
        if (btype == 0) res = tinf_inflate_uncompressed_block(&d);
        else if (btype == 1) res = tinf_inflate_fixed_block(&d);
        else if (btype == 2) res = tinf_inflate_dynamic_block(&d);
        else return TINF_DATA_ERROR;
        if (res != TINF_OK) return res;
    } while (!bfinal);
    *destLen = d.cur_dest;
    return TINF_OK;
}
