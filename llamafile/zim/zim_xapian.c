// -*- mode:c;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=c ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "zim_xapian.h"
#include "zim_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <third_party/zlib/zlib.h>

// =====================================================================
// Xapian Glass single-file on-disk format
// =====================================================================
//
// The index is a flat sequence of fixed-size 8 KiB blocks; block N lives at
// byte offset N*BLOCKSIZE. Block 0 is the "version file" (magic + collection
// stats + the root block number/level of each table). Every other block is a
// B-tree node whose fixed-width header fields are BIG-ENDIAN.
//
// Two tables matter for ranked retrieval:
//   POSTLIST : term -> (termfreq, collfreq, [ (docid, wdf) ... ]) chunked, plus
//              a special "\x00\xe0" postlist that maps docid -> document length.
//   DOCDATA  : docid -> stored document data (here: the ZIM content path).
//
// All varints inside tags use Xapian pack_uint: LSB-first base-128.

#define GLASS_BLOCKSIZE   8192
#define GLASS_MAGIC_LEN   14
#define GLASS_DIR_START   11        // offset of the item-offset directory
#define GLASS_MAX_LEVELS  16

// Leaf item "I" (first 2 bytes, big-endian) flag bits, in the high byte.
#define GLASS_I_MASK_SIZE 0x1fff
#define GLASS_FIRST       0x20      // first component of a key in this block
#define GLASS_LAST        0x40      // last component of a key in this block
#define GLASS_COMPRESSED  0x80      // tag is raw-deflate compressed

static const unsigned char GLASS_MAGIC[GLASS_MAGIC_LEN] = {
    0x0f, 0x0d, 'X', 'a', 'p', 'i', 'a', 'n', ' ', 'G', 'l', 'a', 's', 's'
};

// The doclen postlist's term is the 2 bytes \x00\xe0.
static const unsigned char DOCLEN_KEY[2] = { 0x00, 0xe0 };

// =====================================================================
// Handle
// =====================================================================

struct zim_xapian {
    zim_archive *z;          // borrowed, must outlive us
    unsigned char *blob;     // resident copy of the index (owned)
    size_t blob_size;

    uint32_t postlist_root;
    uint32_t docdata_root;

    uint32_t doccount;
    uint64_t total_doclen;
    double avg_doclen;

    // Lazily materialized docid -> document length, indexed by docid.
    uint32_t *doclen;        // doclen[did], 0 if unknown
    uint32_t doclen_cap;     // allocated length (entries)
    uint32_t doclen_max;     // highest docid filled
    int doclen_ready;        // 0 = not yet built, 1 = built, -1 = failed

    // Optional Snowball stemmer hook (STEM_SOME query expansion). NULL = off.
    zim_xapian_stem_fn stem;
    void *stem_ctx;
};

// =====================================================================
// Errors (reuse the zim reader thread-local error channel)
// =====================================================================

const char *zim_xapian_error(void) {
    return zim_error();
}

// =====================================================================
// Primitive readers (all bounds-checked)
// =====================================================================

static inline unsigned rd_be2(const unsigned char *p) {
    return ((unsigned)p[0] << 8) | p[1];
}
static inline uint32_t rd_be4(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

// Xapian pack_uint: LSB-first base-128 varint. Advances *pp on success.
static int unpack_uint(const unsigned char **pp, const unsigned char *end,
                       uint64_t *out) {
    const unsigned char *p = *pp;
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        if (p >= end)
            return 0;
        unsigned char b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *pp = p;
            *out = v;
            return 1;
        }
        shift += 7;
        if (shift > 63)
            return 0;
    }
}

// Xapian pack_string: pack_uint length prefix then raw bytes.
static int unpack_string(const unsigned char **pp, const unsigned char *end,
                         const unsigned char **s, uint64_t *len) {
    uint64_t n;
    if (!unpack_uint(pp, end, &n))
        return 0;
    if ((uint64_t)(end - *pp) < n)
        return 0;
    *s = *pp;
    *len = n;
    *pp += n;
    return 1;
}

// pack_uint_preserving_sort: the sortable, variable-width big-endian docid key
// Xapian uses for DOCDATA (and the leading component of postlist continuation
// keys). The width is signalled in unary by the leading 1-bits of byte 0,
// followed by a 0 bit, then the value's high bits:
//   width 2 (k=0): 0xxxxxxx xxxxxxxx              -> 0 .. 2^15-1
//   width 3 (k=1): 10xxxxxx xxxxxxxx xxxxxxxx     -> 2^15 .. 2^22-1
//   width 4 (k=2): 110xxxxx ...                   -> 2^22 .. 2^29-1
// i.e. usable bits = 7*k + 15. The value is stored big-endian across the
// `width` bytes; byte 0's top (k+1) bits are then overwritten with the prefix.
// Caller buffer must hold >= 9 bytes. This always emits the shortest (canonical)
// form, matching on-disk keys.
static void pack_did_key(uint64_t v, unsigned char *out, int *len) {
    int width = 2, k = 0;
    while (k < 7 && v >= ((uint64_t)1 << (7 * k + 15))) {
        k++;
        width++;
    }
    for (int i = 0; i < width; i++)
        out[width - 1 - i] = (unsigned char)((v >> (8 * i)) & 0xff);
    if (k > 0)
        out[0] = (unsigned char)((out[0] & (0xff >> k)) | (0xff << (8 - k)));
    *len = width;
}

// Decode a pack_uint_preserving_sort value of exactly `len` bytes (the inverse
// of pack_did_key). `len` must equal the natural width implied by the leading
// 1-bits of byte 0.
static uint64_t unpack_did(const unsigned char *p, int len) {
    int k = 0;
    unsigned char f = p[0];
    while (k < 8 && (f & (0x80 >> k)))
        k++;
    uint64_t v = (uint64_t)(f & (0xff >> (k + 1)));
    for (int i = 1; i < len; i++)
        v = (v << 8) | p[i];
    return v;
}

// A postlist continuation key is `pack_string_preserving_sort(term)` followed by
// `pack_uint_preserving_sort(first_did)`. For an ordinary term that is just
// `term` + a single '\0' separator + the did; the special doclen term
// ("\x00\xe0") is stored raw, with the did appended directly. Recover the
// chunk's first docid from a continuation key, requiring the did to consume the
// key exactly (which both rejects sibling terms and disambiguates the
// separator). Returns 0 if the key is not a valid continuation of `term`.
static int continuation_first_did(const unsigned char *key, int keylen,
                                  int tlen, uint64_t *did_out) {
    for (int off = tlen; off <= tlen + 1 && off < keylen; off++) {
        if (off == tlen + 1 && key[tlen] != 0x00)
            break;                       // only skip a real '\0' separator
        unsigned char f = key[off];
        int k = 0;
        while (k < 8 && (f & (0x80 >> k)))
            k++;
        int width = k + 2;
        if (off + width == keylen) {
            *did_out = unpack_did(key + off, width);
            return 1;
        }
    }
    return 0;
}

// =====================================================================
// Block / item accessors
// =====================================================================

// Return a bounds-checked pointer to block `n`, or NULL.
static const unsigned char *xp_block(const zim_xapian *idx, uint32_t n) {
    size_t off = (size_t)n * GLASS_BLOCKSIZE;
    if (off + GLASS_BLOCKSIZE > idx->blob_size)
        return NULL;
    return idx->blob + off;
}

static inline int blk_level(const unsigned char *b) { return b[4]; }
static inline unsigned blk_dir_end(const unsigned char *b) {
    return rd_be2(b + 9);
}

// Number of directory entries (items) in a block. Validated against the block.
static int blk_nitems(const unsigned char *b) {
    unsigned de = blk_dir_end(b);
    if (de < GLASS_DIR_START || de > GLASS_BLOCKSIZE)
        return -1;
    return (int)((de - GLASS_DIR_START) / 2);
}

// Offset of item `i` within the block (from the 2-byte directory slot), with
// validation that the slot itself is inside the directory.
static int blk_item_off(const unsigned char *b, int i, unsigned *off_out) {
    unsigned de = blk_dir_end(b);
    unsigned slot = GLASS_DIR_START + 2u * (unsigned)i;
    if (slot + 2 > de)
        return 0;
    unsigned off = rd_be2(b + slot);
    if (off >= GLASS_BLOCKSIZE)
        return 0;
    *off_out = off;
    return 1;
}

// --- Branch (level>0) item: [child:4 BE][klen:1][key][...] ---
typedef struct {
    uint32_t child;
    const unsigned char *key;
    int klen;
} branch_item;

static int read_branch_item(const unsigned char *b, unsigned off,
                            branch_item *it) {
    if (off + 5 > GLASS_BLOCKSIZE)
        return 0;
    const unsigned char *q = b + off;
    it->child = rd_be4(q);
    it->klen = q[4];
    it->key = q + 5;
    if (off + 5 + (unsigned)it->klen > GLASS_BLOCKSIZE)
        return 0;
    return 1;
}

// --- Leaf (level==0) item: [I:2 BE][klen:1][key][X:2 if !FIRST][tag] ---
typedef struct {
    int first, last, compressed;
    const unsigned char *key;
    int klen;
    const unsigned char *tag;   // points into the block (raw, maybe compressed)
    int taglen;
} leaf_item;

static int read_leaf_item(const unsigned char *b, unsigned off, leaf_item *it) {
    if (off + 3 > GLASS_BLOCKSIZE)
        return 0;
    const unsigned char *q = b + off;
    unsigned I = rd_be2(q);
    int size = (int)(I & GLASS_I_MASK_SIZE) + 3;       // total item bytes
    it->first = (q[0] & GLASS_FIRST) != 0;
    it->last = (q[0] & GLASS_LAST) != 0;
    it->compressed = (q[0] & GLASS_COMPRESSED) != 0;
    it->klen = q[2];
    it->key = q + 3;
    int hdr = 3 + it->klen + (it->first ? 0 : 2);       // bytes before the tag
    if (size < hdr)
        return 0;
    if (off + (unsigned)size > GLASS_BLOCKSIZE)
        return 0;
    it->tag = q + hdr;
    it->taglen = size - hdr;
    return 1;
}

// Compare two byte strings (memcmp then length), like Xapian key order.
static int key_cmp(const unsigned char *a, int al,
                   const unsigned char *b, int bl) {
    int m = al < bl ? al : bl;
    int r = memcmp(a, b, (size_t)m);
    if (r)
        return r;
    return al - bl;
}

// =====================================================================
// B-tree navigation
// =====================================================================

// Descend from `root` to the leaf block that would contain `key`. Glass branch
// separators: child i covers keys >= key(i); we pick the last separator <= key
// (item 0 is always a candidate, its key being the lowest in the subtree).
static int find_leaf(const zim_xapian *idx, uint32_t root,
                     const unsigned char *key, int klen, uint32_t *leaf_out) {
    uint32_t n = root;
    int guard = 0;
    for (;;) {
        const unsigned char *b = xp_block(idx, n);
        if (!b)
            return 0;
        if (blk_level(b) == 0) {
            *leaf_out = n;
            return 1;
        }
        int ni = blk_nitems(b);
        if (ni <= 0)
            return 0;
        // Binary search for the last separator with key <= search key. Item 0
        // is treated as -infinity (its stored key is the subtree minimum).
        int lo = 1, hi = ni - 1, pick = 0;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            unsigned off;
            branch_item bi;
            if (!blk_item_off(b, mid, &off) || !read_branch_item(b, off, &bi))
                return 0;
            if (key_cmp(bi.key, bi.klen, key, klen) <= 0) {
                pick = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        unsigned off;
        branch_item bi;
        if (!blk_item_off(b, pick, &off) || !read_branch_item(b, off, &bi))
            return 0;
        n = bi.child;
        if (++guard > GLASS_MAX_LEVELS)
            return 0;
    }
}

// Forward cursor for in-order leaf-item traversal across blocks. Glass blocks
// hold no sibling pointers, so we keep the descent path and re-ascend on leaf
// exhaustion.
typedef struct {
    const zim_xapian *idx;
    struct {
        uint32_t block;
        int item;
        int nitems;
    } path[GLASS_MAX_LEVELS];
    int depth;       // number of levels on the path; leaf is path[depth-1]
    int valid;
} glass_cursor;

// Push the leftmost descent from branch block `n` onto the path, stopping at a
// leaf. Returns 0 on malformed structure.
static int cursor_descend_left(glass_cursor *c, uint32_t n) {
    for (;;) {
        if (c->depth >= GLASS_MAX_LEVELS)
            return 0;
        const unsigned char *b = xp_block(c->idx, n);
        if (!b)
            return 0;
        int ni = blk_nitems(b);
        if (ni <= 0)
            return 0;
        c->path[c->depth].block = n;
        c->path[c->depth].item = 0;
        c->path[c->depth].nitems = ni;
        c->depth++;
        if (blk_level(b) == 0)
            return 1;
        unsigned off;
        branch_item bi;
        if (!blk_item_off(b, 0, &off) || !read_branch_item(b, off, &bi))
            return 0;
        n = bi.child;
    }
}

// Position the cursor at the first leaf item whose key is >= `key`.
static int cursor_seek(glass_cursor *c, const zim_xapian *idx, uint32_t root,
                       const unsigned char *key, int klen) {
    memset(c, 0, sizeof(*c));
    c->idx = idx;
    uint32_t n = root;
    int guard = 0;
    for (;;) {
        if (c->depth >= GLASS_MAX_LEVELS)
            return 0;
        const unsigned char *b = xp_block(idx, n);
        if (!b)
            return 0;
        int ni = blk_nitems(b);
        if (ni <= 0)
            return 0;
        if (blk_level(b) == 0) {
            // lower_bound over leaf items (directory is key-sorted)
            int lo = 0, hi = ni, pick = ni;
            while (lo < hi) {
                int mid = lo + (hi - lo) / 2;
                unsigned off;
                leaf_item li;
                if (!blk_item_off(b, mid, &off) || !read_leaf_item(b, off, &li))
                    return 0;
                if (key_cmp(li.key, li.klen, key, klen) >= 0) {
                    pick = mid;
                    hi = mid;
                } else {
                    lo = mid + 1;
                }
            }
            c->path[c->depth].block = n;
            c->path[c->depth].item = pick;
            c->path[c->depth].nitems = ni;
            c->depth++;
            c->valid = 1;
            // pick may equal ni (key after last item); advance handles that.
            return 1;
        }
        // branch: pick last separator <= key (item 0 = -inf)
        int lo = 1, hi = ni - 1, pick = 0;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            unsigned off;
            branch_item bi;
            if (!blk_item_off(b, mid, &off) || !read_branch_item(b, off, &bi))
                return 0;
            if (key_cmp(bi.key, bi.klen, key, klen) <= 0) {
                pick = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        c->path[c->depth].block = n;
        c->path[c->depth].item = pick;
        c->path[c->depth].nitems = ni;
        c->depth++;
        unsigned off;
        branch_item bi;
        if (!blk_item_off(b, pick, &off) || !read_branch_item(b, off, &bi))
            return 0;
        n = bi.child;
        if (++guard > GLASS_MAX_LEVELS)
            return 0;
    }
}

// Read the leaf item the cursor currently points at. Returns 0 if the cursor is
// past the end of its leaf (caller should advance) or invalid.
static int cursor_get(glass_cursor *c, leaf_item *li) {
    if (!c->valid || c->depth == 0)
        return 0;
    int d = c->depth - 1;
    if (c->path[d].item >= c->path[d].nitems)
        return 0;
    const unsigned char *b = xp_block(c->idx, c->path[d].block);
    if (!b)
        return 0;
    unsigned off;
    if (!blk_item_off(b, c->path[d].item, &off))
        return 0;
    return read_leaf_item(b, off, li);
}

// Advance the cursor to the next leaf item in key order. Returns 0 when the
// whole tree is exhausted.
static int cursor_advance(glass_cursor *c) {
    if (!c->valid || c->depth == 0)
        return 0;
    int d = c->depth - 1;
    c->path[d].item++;
    if (c->path[d].item < c->path[d].nitems)
        return 1;
    // leaf exhausted: ascend until a branch has a further child, then descend
    c->depth--;  // pop leaf
    while (c->depth > 0) {
        int p = c->depth - 1;
        c->path[p].item++;
        if (c->path[p].item < c->path[p].nitems) {
            const unsigned char *b = xp_block(c->idx, c->path[p].block);
            if (!b)
                return 0;
            unsigned off;
            branch_item bi;
            if (!blk_item_off(b, c->path[p].item, &off) ||
                !read_branch_item(b, off, &bi))
                return 0;
            return cursor_descend_left(c, bi.child);
        }
        c->depth--;  // pop this branch too
    }
    c->valid = 0;
    return 0;
}

// =====================================================================
// Tag access (transparently decompresses)
// =====================================================================

// Materialize a leaf item's tag into *out (caller-owned scratch). On success
// sets *out_ptr to a readable buffer of *out_len bytes; for uncompressed tags
// that aliases the block, otherwise it is `scratch`.
static int get_tag(const leaf_item *li, unsigned char *scratch,
                   size_t scratch_cap, const unsigned char **out_ptr,
                   size_t *out_len) {
    if (!li->compressed) {
        *out_ptr = li->tag;
        *out_len = (size_t)li->taglen;
        return 1;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return 0;
    zs.next_in = (Bytef *)li->tag;
    zs.avail_in = (uInt)li->taglen;
    zs.next_out = (Bytef *)scratch;
    zs.avail_out = (uInt)scratch_cap;
    int r = inflate(&zs, Z_FINISH);
    size_t produced = scratch_cap - zs.avail_out;
    inflateEnd(&zs);
    if (r != Z_STREAM_END)
        return 0;
    *out_ptr = scratch;
    *out_len = produced;
    return 1;
}

// =====================================================================
// Version header
// =====================================================================

static int parse_version(zim_xapian *idx) {
    const unsigned char *b = xp_block(idx, 0);
    if (!b) {
        zim_set_error("xapian: index too small for version block");
        return 0;
    }
    const unsigned char *p = b;
    const unsigned char *end = b + GLASS_BLOCKSIZE;
    if (memcmp(p, GLASS_MAGIC, GLASS_MAGIC_LEN) != 0) {
        zim_set_error("xapian: not a Glass index (bad magic)");
        return 0;
    }
    p += GLASS_MAGIC_LEN;
    p += 2;            // format version (BE), not needed
    p += 16;           // uuid
    if (p > end) {
        zim_set_error("xapian: truncated version header");
        return 0;
    }
    uint64_t rev;
    if (!unpack_uint(&p, end, &rev)) {
        zim_set_error("xapian: bad revision varint");
        return 0;
    }
    // 6 RootInfo: POSTLIST, DOCDATA, TERMLIST, POSITION, SPELLING, SYNONYM
    uint32_t roots[6] = {0};
    for (int t = 0; t < 6; t++) {
        uint64_t root, val, num_entries, bsz, cmin, fllen;
        const unsigned char *fl;
        if (!unpack_uint(&p, end, &root) || !unpack_uint(&p, end, &val) ||
            !unpack_uint(&p, end, &num_entries) || !unpack_uint(&p, end, &bsz) ||
            !unpack_uint(&p, end, &cmin) ||
            !unpack_string(&p, end, &fl, &fllen)) {
            zim_set_error("xapian: bad RootInfo[%d]", t);
            return 0;
        }
        roots[t] = (uint32_t)root;
    }
    // Collection stats.
    uint64_t doccount, last_docid, doclen_lb, wdf_ub, doclen_ub, oldest_cs,
        total_doclen, spell_ub;
    if (!unpack_uint(&p, end, &doccount) ||
        !unpack_uint(&p, end, &last_docid) ||
        !unpack_uint(&p, end, &doclen_lb) || !unpack_uint(&p, end, &wdf_ub) ||
        !unpack_uint(&p, end, &doclen_ub) ||
        !unpack_uint(&p, end, &oldest_cs) ||
        !unpack_uint(&p, end, &total_doclen) ||
        !unpack_uint(&p, end, &spell_ub)) {
        zim_set_error("xapian: bad collection stats");
        return 0;
    }
    idx->postlist_root = roots[0];
    idx->docdata_root = roots[1];
    idx->doccount = (uint32_t)doccount;
    idx->total_doclen = total_doclen;
    idx->avg_doclen = doccount ? (double)total_doclen / (double)doccount : 0.0;
    if (!doccount) {
        zim_set_error("xapian: empty index (doccount=0)");
        return 0;
    }
    // POSTLIST must be present to do anything useful.
    const unsigned char *pr = xp_block(idx, idx->postlist_root);
    const unsigned char *dr = xp_block(idx, idx->docdata_root);
    if (!pr || !dr) {
        zim_set_error("xapian: root block out of range");
        return 0;
    }
    return 1;
}

// =====================================================================
// Postlist chunk decoding
// =====================================================================

// Visitor invoked for every (docid, wdf) posting. wdf is the within-document
// frequency for terms, or the document length for the doclen postlist.
typedef void (*posting_fn)(void *ctx, uint32_t did, uint32_t wdf);

// Decode one chunk's body starting at *p (already past any first-chunk
// termfreq/collfreq/first_did header). `first_did` is the docid of the first
// posting in this chunk. Returns the last docid, and whether this was the last
// chunk, via out params. Calls `fn` for each posting.
static int decode_chunk_body(const unsigned char *p, const unsigned char *end,
                             uint32_t first_did, int *is_last_out,
                             uint32_t *last_did_out, posting_fn fn, void *ctx) {
    if (p >= end)
        return 0;
    int is_last = (*p == '1');     // pack_bool: ASCII '0' / '1'
    p++;
    uint64_t inc_to_last;
    if (!unpack_uint(&p, end, &inc_to_last))
        return 0;
    uint64_t last_did = (uint64_t)first_did + inc_to_last;
    uint64_t wdf0;
    if (!unpack_uint(&p, end, &wdf0))
        return 0;
    uint64_t did = first_did;
    if (fn)
        fn(ctx, (uint32_t)did, (uint32_t)wdf0);
    while (p < end) {
        uint64_t inc, wdf;
        if (!unpack_uint(&p, end, &inc))
            return 0;
        did += inc + 1;
        if (!unpack_uint(&p, end, &wdf))
            return 0;
        if (fn)
            fn(ctx, (uint32_t)did, (uint32_t)wdf);
    }
    if (did != last_did)        // self-consistency check
        return 0;
    *is_last_out = is_last;
    *last_did_out = (uint32_t)last_did;
    return 1;
}

// Iterate the full postlist for `term` (term bytes, length tlen): decode the
// first chunk and any continuation chunks. The first chunk's tag begins with
// termfreq/collfreq/(first_did-1); continuations are keyed `term \x00 <did>`
// and chain from the previous chunk's last docid. `*termfreq_out` receives the
// document frequency. Returns 0 on malformed data, 1 on success (incl. the
// "term absent" case, with termfreq 0 and no postings).
static int iterate_postlist(zim_xapian *idx, const unsigned char *term,
                            int tlen, uint64_t *termfreq_out, posting_fn fn,
                            void *ctx) {
    *termfreq_out = 0;
    glass_cursor c;
    if (!cursor_seek(&c, idx, idx->postlist_root, term, tlen))
        return 0;

    unsigned char scratch[1 << 16];
    leaf_item li;
    if (!cursor_get(&c, &li)) {
        if (!cursor_advance(&c))
            return 1;            // tree exhausted: term absent
        if (!cursor_get(&c, &li))
            return 1;
    }
    // Expect the first chunk: key exactly == term, FIRST flag set.
    if (!(li.first && li.klen == tlen && memcmp(li.key, term, (size_t)tlen) == 0))
        return 1;                // term absent

    const unsigned char *tag;
    size_t taglen;
    if (!get_tag(&li, scratch, sizeof(scratch), &tag, &taglen))
        return 0;
    const unsigned char *p = tag, *e = tag + taglen;
    uint64_t termfreq, collfreq, fd_minus1;
    if (!unpack_uint(&p, e, &termfreq) || !unpack_uint(&p, e, &collfreq) ||
        !unpack_uint(&p, e, &fd_minus1))
        return 0;
    *termfreq_out = termfreq;
    uint32_t first_did = (uint32_t)(fd_minus1 + 1);
    int is_last;
    uint32_t last_did;
    if (!decode_chunk_body(p, e, first_did, &is_last, &last_did, fn, ctx))
        return 0;

    // Continuation chunks: each is keyed `term [<\0>] pack_uint_preserving_sort
    // (first_did)`. The postlist is sparse (only documents containing the term),
    // so the chunk's first docid must come from its KEY, not from chaining off
    // the previous chunk's last docid.
    (void)last_did;
    while (!is_last) {
        if (!cursor_advance(&c))
            break;
        if (!cursor_get(&c, &li))
            break;
        if (!(li.klen > tlen && memcmp(li.key, term, (size_t)tlen) == 0))
            break;               // next term reached
        uint64_t cfirst64;
        if (!continuation_first_did(li.key, li.klen, tlen, &cfirst64))
            break;               // not a continuation of this term
        if (!get_tag(&li, scratch, sizeof(scratch), &tag, &taglen))
            return 0;
        if (!decode_chunk_body(tag, tag + taglen, (uint32_t)cfirst64, &is_last,
                               &last_did, fn, ctx))
            return 0;
    }
    return 1;
}

// =====================================================================
// Doclen table
// =====================================================================

struct doclen_ctx {
    zim_xapian *idx;
    int ok;
};

static void doclen_collect(void *vctx, uint32_t did, uint32_t len) {
    struct doclen_ctx *dc = (struct doclen_ctx *)vctx;
    zim_xapian *idx = dc->idx;
    if (did >= idx->doclen_cap) {
        uint32_t ncap = idx->doclen_cap ? idx->doclen_cap * 2 : 1024;
        while (ncap <= did)
            ncap *= 2;
        uint32_t *n = (uint32_t *)realloc(idx->doclen, ncap * sizeof(uint32_t));
        if (!n) {
            dc->ok = 0;
            return;
        }
        memset(n + idx->doclen_cap, 0,
               (ncap - idx->doclen_cap) * sizeof(uint32_t));
        idx->doclen = n;
        idx->doclen_cap = ncap;
    }
    idx->doclen[did] = len;
    if (did > idx->doclen_max)
        idx->doclen_max = did;
}

// Materialize the doclen array on first use.
static int ensure_doclen(zim_xapian *idx) {
    if (idx->doclen_ready)
        return idx->doclen_ready > 0;
    struct doclen_ctx dc = {idx, 1};
    uint64_t tf;
    int r = iterate_postlist(idx, DOCLEN_KEY, (int)sizeof(DOCLEN_KEY), &tf,
                             doclen_collect, &dc);
    idx->doclen_ready = (r && dc.ok) ? 1 : -1;
    return idx->doclen_ready > 0;
}

// =====================================================================
// DOCDATA lookup (docid -> stored path)
// =====================================================================

// Copy the stored data for `did` into `out` (NUL-terminated), returning length
// (excluding NUL) or -1 if absent / malformed.
static int docdata_lookup(zim_xapian *idx, uint32_t did, char *out,
                          size_t out_cap) {
    unsigned char key[9];
    int klen;
    pack_did_key(did, key, &klen);
    uint32_t leaf;
    if (!find_leaf(idx, idx->docdata_root, key, klen, &leaf))
        return -1;
    const unsigned char *b = xp_block(idx, leaf);
    if (!b)
        return -1;
    int ni = blk_nitems(b);
    if (ni < 0)
        return -1;
    for (int i = 0; i < ni; i++) {
        unsigned off;
        leaf_item li;
        if (!blk_item_off(b, i, &off) || !read_leaf_item(b, off, &li))
            return -1;
        if (li.first && li.klen == klen &&
            memcmp(li.key, key, (size_t)klen) == 0) {
            unsigned char scratch[1 << 16];
            const unsigned char *tag;
            size_t taglen;
            if (!get_tag(&li, scratch, sizeof(scratch), &tag, &taglen))
                return -1;
            if (taglen + 1 > out_cap)
                taglen = out_cap - 1;
            memcpy(out, tag, taglen);
            out[taglen] = '\0';
            return (int)taglen;
        }
    }
    return -1;
}

// =====================================================================
// Tokenization
// =====================================================================

// Split `query` into lowercased terms. ASCII letters/digits and any byte >=
// 0x80 (UTF-8 continuation/lead) are term characters; everything else is a
// separator. Only ASCII is case-folded. Writes pointers/lengths into the caller
// arrays, up to `max` terms. Returns the term count.
static int tokenize(const char *query, char terms[][128], int max) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)query;
    while (*p && n < max) {
        while (*p && !(isalnum(*p) || *p >= 0x80))
            p++;
        if (!*p)
            break;
        int len = 0;
        char *dst = terms[n];
        while (*p && (isalnum(*p) || *p >= 0x80)) {
            if (len < 127)
                dst[len++] = (char)tolower(*p);
            p++;
        }
        dst[len] = '\0';
        if (len > 0)
            n++;
    }
    return n;
}

// =====================================================================
// BM25 scoring
// =====================================================================

#define BM25_K1 1.2
#define BM25_B  0.75

struct score_ctx {
    zim_xapian *idx;
    float *score;      // indexed by docid
    double idf;
    double avgdl;
    int maxmerge;      // 0 = accumulate (+=), 1 = keep the max (OR same concept)
};

static void score_posting(void *vctx, uint32_t did, uint32_t wdf) {
    struct score_ctx *sc = (struct score_ctx *)vctx;
    zim_xapian *idx = sc->idx;
    if (did > idx->doclen_max)
        return;
    double dl = (did < idx->doclen_cap && idx->doclen[did])
                    ? (double)idx->doclen[did]
                    : sc->avgdl;
    double denom = (double)wdf + BM25_K1 * (1.0 - BM25_B + BM25_B * dl / sc->avgdl);
    if (denom <= 0.0)
        return;
    float contrib = (float)(sc->idf * ((double)wdf * (BM25_K1 + 1.0)) / denom);
    if (sc->maxmerge) {
        if (contrib > sc->score[did])
            sc->score[did] = contrib;     // raw vs Z-stem: count the concept once
    } else {
        sc->score[did] += contrib;        // distinct query terms: BM25 OR sum
    }
}

// Score one term's full postlist into `target` (BM25 with this index's stats).
// `maxmerge` controls how postings combine into `target` (see score_posting).
// No-op (term absent) leaves `target` untouched. `N` is the doccount.
static void score_term_into(zim_xapian *idx, const unsigned char *term, int tlen,
                            double N, float *target, int maxmerge) {
    uint64_t tf = 0;
    if (!iterate_postlist(idx, term, tlen, &tf, NULL, NULL) || tf == 0)
        return;
    struct score_ctx sc;
    sc.idx = idx;
    sc.score = target;
    sc.avgdl = idx->avg_doclen > 0 ? idx->avg_doclen : 1.0;
    // Okapi BM25 IDF with the +1 guard (Lucene-style), always positive.
    sc.idf = log(1.0 + (N - (double)tf + 0.5) / ((double)tf + 0.5));
    sc.maxmerge = maxmerge;
    iterate_postlist(idx, term, tlen, &tf, score_posting, &sc);
}

// =====================================================================
// Public API
// =====================================================================

zim_xapian *zim_xapian_open(zim_archive *z, const char *entry_path) {
    if (!z || !entry_path) {
        zim_set_error("xapian: null argument");
        return NULL;
    }
    zim_entry e;
    if (!zim_get_entry_by_path(z, entry_path, &e)) {
        zim_set_error("xapian: index entry '%s' not found", entry_path);
        return NULL;
    }
    size_t n = 0;
    void *buf = zim_get_content(z, &e, &n);
    if (!buf) {
        // zim_get_content already set an error
        return NULL;
    }
    if (n < GLASS_BLOCKSIZE) {
        zim_set_error("xapian: index '%s' too small (%zu bytes)", entry_path, n);
        zim_free(buf);
        return NULL;
    }
    zim_xapian *idx = (zim_xapian *)calloc(1, sizeof(*idx));
    if (!idx) {
        zim_set_error("xapian: out of memory");
        zim_free(buf);
        return NULL;
    }
    idx->z = z;
    idx->blob = (unsigned char *)buf;
    idx->blob_size = n;
    if (!parse_version(idx)) {
        zim_xapian_close(idx);
        return NULL;
    }
    return idx;
}

void zim_xapian_close(zim_xapian *idx) {
    if (!idx)
        return;
    if (idx->blob)
        zim_free(idx->blob);
    free(idx->doclen);
    free(idx);
}

void zim_xapian_set_stemmer(zim_xapian *idx, zim_xapian_stem_fn stem,
                            void *ctx) {
    if (!idx)
        return;
    idx->stem = stem;
    idx->stem_ctx = ctx;
}

uint32_t zim_xapian_doccount(const zim_xapian *idx) {
    return idx ? idx->doccount : 0;
}
double zim_xapian_avg_doclen(const zim_xapian *idx) {
    return idx ? idx->avg_doclen : 0.0;
}

uint32_t zim_xapian_termfreq(zim_xapian *idx, const char *term) {
    if (!idx || !term)
        return 0;
    uint64_t tf = 0;
    iterate_postlist(idx, (const unsigned char *)term, (int)strlen(term), &tf,
                     NULL, NULL);
    return (uint32_t)tf;
}

int zim_xapian_search(zim_xapian *idx, const char *query, int limit,
                      zim_xapian_hit **hits_out) {
    if (hits_out)
        *hits_out = NULL;
    if (!idx || !query || !hits_out || limit <= 0) {
        zim_set_error("xapian: bad search argument");
        return -1;
    }
    if (!ensure_doclen(idx)) {
        zim_set_error("xapian: failed to load document lengths");
        return -1;
    }

    char terms[32][128];
    int nterms = tokenize(query, terms, 32);
    if (nterms == 0)
        return 0;

    size_t nscore = (size_t)idx->doclen_max + 1;
    float *score = (float *)calloc(nscore, sizeof(float));
    if (!score) {
        zim_set_error("xapian: out of memory for scores");
        return -1;
    }

    // When stemming, OR(raw, "Z"+stem) for one query term is max-merged into a
    // per-term scratch buffer (so a doc with BOTH forms is counted once), then
    // summed into the global score. Without a stemmer we score straight into the
    // global buffer (unchanged behaviour).
    float *tscore = NULL;
    if (idx->stem) {
        tscore = (float *)calloc(nscore, sizeof(float));
        if (!tscore) {
            free(score);
            zim_set_error("xapian: out of memory for scores");
            return -1;
        }
    }

    double N = (double)idx->doccount;
    for (int t = 0; t < nterms; t++) {
        // Deduplicate repeated query terms (their IDF would otherwise double).
        int dup = 0;
        for (int u = 0; u < t; u++)
            if (strcmp(terms[t], terms[u]) == 0) {
                dup = 1;
                break;
            }
        if (dup)
            continue;
        int tlen = (int)strlen(terms[t]);

        if (!idx->stem) {
            // No stemmer: score the raw term straight into the global buffer.
            score_term_into(idx, (const unsigned char *)terms[t], tlen, N, score,
                            0);
            continue;
        }

        // OR(raw, stem, "Z"+stem) into the scratch buffer, max-merged so a doc
        // holding more than one of the forms is counted once.
        //
        // We probe BOTH stem forms because ZIM archives index full text with
        // either Xapian stemming strategy:
        //   * STEM_SOME (default): stores the unstemmed term AND a "Z"+stem term
        //     -> raw matches the surface word, "Z"+stem matches variants.
        //   * STEM_ALL: stores ONLY the bare (unprefixed) stem
        //     -> the bare stem matches; raw only hits when it equals its stem.
        // Querying raw + bare-stem + "Z"+stem matches either layout; the form
        // that is absent simply contributes an empty postlist.
        score_term_into(idx, (const unsigned char *)terms[t], tlen, N, tscore, 1);
        char stembuf[128];
        const char *st = idx->stem(idx->stem_ctx, terms[t], stembuf,
                                   sizeof(stembuf));
        if (st && *st) {
            size_t sl = strlen(st);
            if (sl > 126)
                sl = 126;
            // bare stem (STEM_ALL layout), unless identical to the raw term
            if (!(sl == (size_t)tlen && memcmp(st, terms[t], sl) == 0))
                score_term_into(idx, (const unsigned char *)st, (int)sl, N,
                                tscore, 1);
            // "Z"+stem (STEM_SOME layout)
            char zterm[130];
            zterm[0] = 'Z';
            memcpy(zterm + 1, st, sl);
            zterm[1 + sl] = '\0';
            score_term_into(idx, (const unsigned char *)zterm, (int)(1 + sl), N,
                            tscore, 1);
        }
        // Fold the per-term max into the global sum and clear the scratch.
        for (uint32_t d = 0; d < nscore; d++) {
            if (tscore[d] > 0.0f) {
                score[d] += tscore[d];
                tscore[d] = 0.0f;
            }
        }
    }
    free(tscore);

    // Partial top-`limit` selection over the score array.
    typedef struct {
        uint32_t did;
        float score;
    } cand;
    cand *top = (cand *)calloc((size_t)limit, sizeof(cand));
    if (!top) {
        free(score);
        zim_set_error("xapian: out of memory");
        return -1;
    }
    int ntop = 0;
    for (uint32_t did = 1; did < nscore; did++) {
        float s = score[did];
        if (s <= 0.0f)
            continue;
        if (ntop < limit) {
            int j = ntop++;
            while (j > 0 && top[j - 1].score < s) {
                top[j] = top[j - 1];
                j--;
            }
            top[j].did = did;
            top[j].score = s;
        } else if (s > top[limit - 1].score) {
            int j = limit - 1;
            while (j > 0 && top[j - 1].score < s) {
                top[j] = top[j - 1];
                j--;
            }
            top[j].did = did;
            top[j].score = s;
        }
    }
    free(score);

    if (ntop == 0) {
        free(top);
        return 0;
    }

    zim_xapian_hit *hits =
        (zim_xapian_hit *)calloc((size_t)ntop, sizeof(zim_xapian_hit));
    if (!hits) {
        free(top);
        zim_set_error("xapian: out of memory for hits");
        return -1;
    }
    int out = 0;
    for (int i = 0; i < ntop; i++) {
        char path[1024];
        int pl = docdata_lookup(idx, top[i].did, path, sizeof(path));
        hits[out].docid = top[i].did;
        hits[out].score = top[i].score;
        if (pl < 0) {
            hits[out].path = strdup("");
            hits[out].title = strdup("");
        } else {
            hits[out].path = strdup(path);
            // Resolve the ZIM entry title for this stored path.
            zim_entry e;
            const char *title = NULL;
            if (zim_get_entry_by_path(idx->z, path, &e) && e.title)
                title = e.title;
            hits[out].title = strdup(title ? title : path);
        }
        out++;
    }
    free(top);
    *hits_out = hits;
    return out;
}

void zim_xapian_free_hits(zim_xapian_hit *hits, int count) {
    if (!hits)
        return;
    for (int i = 0; i < count; i++) {
        free(hits[i].path);
        free(hits[i].title);
    }
    free(hits);
}
