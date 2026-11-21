/* Shared with the sanitizer test. No app-specific decisions. */
static inline uint32_t minifb_over(uint32_t s, uint32_t d)
{
    unsigned a = s >> 24;
    if (a == 255) return s;
    if (!a) return d;
    uint32_t out = 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned c = ((s >> shift) & 255) + ((((d >> shift) & 255) * (255 - a) + 127) / 255);
        out |= (c > 255 ? 255 : c) << shift;
    }
    return out;
}

static inline void minifb_layer_fields(uint8_t *to, const uint8_t *from, unsigned i)
{
    memcpy(to + SWAP_SURFACE(i), from + SWAP_SURFACE(i), 4);
    memcpy(to + SWAP_SOURCE(i), from + SWAP_SOURCE(i), 16);
    memcpy(to + SWAP_ENABLED(i), from + SWAP_ENABLED(i), 4);
    memcpy(to + SWAP_DEST(i), from + SWAP_DEST(i), 16);
}

/* Lock held by caller. Retain new surfaces before dropping replaced ones;
 * an unmentioned layer is unchanged, including its geometry and lifetime.
 * A failed lookup leaves the entire accepted state untouched. */
template<class Surface, class Copy, class Drop>
static bool minifb_update_layers(uint8_t *state, Surface **active,
    const uint8_t *delta, Copy copy, Drop drop)
{
    uint32_t set = swapWord(delta, SWAP_LAYERS_SET);
    uint32_t clear = swapWord(delta, SWAP_LAYERS_CLEARED);
    Surface *next[SWAP_LAYERS] = {};
    for (unsigned i = 0; i < SWAP_LAYERS; i++) {
        unsigned bit = 1u << i;
        uint32_t sid = (clear & bit) ? 0 : swapWord(delta, SWAP_SURFACE(i));
        if (((set | clear) & bit) && sid) {
            next[i] = copy(sid);
            if (!next[i]) {
                for (unsigned j = 0; j < SWAP_LAYERS; j++) if (next[j]) drop(next[j]);
                return false;
            }
        }
    }
    for (unsigned i = 0; i < SWAP_LAYERS; i++) {
        if (!((set | clear) & (1u << i))) continue;
        if (active[i]) drop(active[i]);
        active[i] = next[i];
        minifb_layer_fields(state, delta, i);
        if (!next[i]) {
            uint32_t zero = 0;
            memcpy(state + SWAP_SURFACE(i), &zero, 4);
            memcpy(state + SWAP_ENABLED(i), &zero, 4);
        }
    }
    return true;
}


/* Four premultiplied BGRA pixels at a time. Exact rounding by 255, with
 * transparent input preserving every destination bit (including its alpha).
 * No unaligned wide tail reads, and no change to opaque-plane semantics. */
static inline void minifb_over_row(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    uint32_t i=0;
#if defined(__arm64__) || defined(__aarch64__)
    uint32_t blocks=count/16;
    if (blocks) {
        const uint32_t *in=src;
        uint32_t *out=dst, n=blocks, alpha;
        __asm__ volatile(
            "movi v20.8h, #128\n"
            "movi v21.16b, #255\n"
            "1:\n"
            "ld4 {v0.16b-v3.16b}, [%[in]], #64\n"
            "umaxv b16, v3.16b\n"
            "umov %w[a], v16.b[0]\n"
            "cbz %w[a], 3f\n"
            "uminv b16, v3.16b\n"
            "umov %w[a], v16.b[0]\n"
            "cmp %w[a], #255\n"
            "b.eq 2f\n"
            "ld4 {v4.16b-v7.16b}, [%[out]]\n"
            "mvn v18.16b, v3.16b\n"
            "cmeq v19.16b, v3.16b, #0\n"
            "umull v16.8h, v4.8b, v18.8b\n"
            "umull2 v17.8h, v4.16b, v18.16b\n"
            "add v16.8h, v16.8h, v20.8h\n"
            "add v17.8h, v17.8h, v20.8h\n"
            "ushr v22.8h, v16.8h, #8\n"
            "add v16.8h, v16.8h, v22.8h\n"
            "ushr v22.8h, v17.8h, #8\n"
            "add v17.8h, v17.8h, v22.8h\n"
            "shrn v16.8b, v16.8h, #8\n"
            "shrn2 v16.16b, v17.8h, #8\n"
            "uqadd v0.16b, v0.16b, v16.16b\n"
            "bit v0.16b, v4.16b, v19.16b\n"
            "umull v16.8h, v5.8b, v18.8b\n"
            "umull2 v17.8h, v5.16b, v18.16b\n"
            "add v16.8h, v16.8h, v20.8h\n"
            "add v17.8h, v17.8h, v20.8h\n"
            "ushr v22.8h, v16.8h, #8\n"
            "add v16.8h, v16.8h, v22.8h\n"
            "ushr v22.8h, v17.8h, #8\n"
            "add v17.8h, v17.8h, v22.8h\n"
            "shrn v16.8b, v16.8h, #8\n"
            "shrn2 v16.16b, v17.8h, #8\n"
            "uqadd v1.16b, v1.16b, v16.16b\n"
            "bit v1.16b, v5.16b, v19.16b\n"
            "umull v16.8h, v6.8b, v18.8b\n"
            "umull2 v17.8h, v6.16b, v18.16b\n"
            "add v16.8h, v16.8h, v20.8h\n"
            "add v17.8h, v17.8h, v20.8h\n"
            "ushr v22.8h, v16.8h, #8\n"
            "add v16.8h, v16.8h, v22.8h\n"
            "ushr v22.8h, v17.8h, #8\n"
            "add v17.8h, v17.8h, v22.8h\n"
            "shrn v16.8b, v16.8h, #8\n"
            "shrn2 v16.16b, v17.8h, #8\n"
            "uqadd v2.16b, v2.16b, v16.16b\n"
            "bit v2.16b, v6.16b, v19.16b\n"
            "mov v3.16b, v21.16b\n"
            "bit v3.16b, v7.16b, v19.16b\n"
            "2:\n"
            "st4 {v0.16b-v3.16b}, [%[out]]\n"
            "3:\n"
            "add %[out], %[out], #64\n"
            "subs %w[n], %w[n], #1\n"
            "b.ne 1b\n"
            : [in] "+r"(in), [out] "+r"(out), [n] "+r"(n), [a] "=&r"(alpha)
            :
            : "v0","v1","v2","v3","v4","v5","v6","v7","v16","v17",
              "v18","v19","v20","v21","v22","cc","memory");
        i=blocks*16;
    }
    static const uint8_t alphaIndex[16]={3,3,3,3,7,7,7,7,11,11,11,11,15,15,15,15};
    for (; i+4<=count; i+=4) {
        uint32_t alpha;
        __asm__ volatile(
            "ldr q0, [%[src]]\n"
            "ldr q7, [%[indices]]\n"
            "tbl v2.16b, {v0.16b}, v7.16b\n"
            "uminv b4, v2.16b\n"
            "umov %w[a], v4.b[0]\n"
            "cmp %w[a], #255\n"
            "b.eq 2f\n"
            "umaxv b4, v2.16b\n"
            "umov %w[a], v4.b[0]\n"
            "cbz %w[a], 3f\n"
            "ldr q1, [%[dst]]\n"
            "cmeq v3.16b, v2.16b, #0\n"
            "mvn v2.16b, v2.16b\n"
            "umull v4.8h, v1.8b, v2.8b\n"
            "umull2 v5.8h, v1.16b, v2.16b\n"
            "movi v6.8h, #128\n"
            "add v4.8h, v4.8h, v6.8h\n"
            "add v5.8h, v5.8h, v6.8h\n"
            "ushr v6.8h, v4.8h, #8\n"
            "add v4.8h, v4.8h, v6.8h\n"
            "ushr v6.8h, v5.8h, #8\n"
            "add v5.8h, v5.8h, v6.8h\n"
            "shrn v4.8b, v4.8h, #8\n"
            "shrn2 v4.16b, v5.8h, #8\n"
            "uqadd v0.16b, v0.16b, v4.16b\n"
            "movi v2.4s, #255, lsl #24\n"
            "orr v0.16b, v0.16b, v2.16b\n"
            "bsl v3.16b, v1.16b, v0.16b\n"
            "mov v0.16b, v3.16b\n"
            "2:\n"
            "str q0, [%[dst]]\n"
            "3:\n"
            : [a] "=&r"(alpha)
            : [src] "r"(src+i), [dst] "r"(dst+i), [indices] "r"(alphaIndex)
            : "v0","v1","v2","v3","v4","v5","v6","v7","cc","memory");
    }
#endif
    for (; i<count; i++) dst[i]=minifb_over(src[i],dst[i]);
}


/* True only if every input alpha in [first,last] is zero. Call after bounds
 * validation, before bilinear filtering. Do not inspect row padding. */
static inline bool minifb_transparent_span(const uint32_t *row, uint32_t first, uint32_t last)
{
    if (first > last) return false;
    uint32_t c=first;
#if defined(__arm64__) || defined(__aarch64__)
    for (; last-c >= 3; c+=4) {
        uint32_t alpha;
        __asm__ volatile(
            "ldr q0, [%[p]]\n"
            "ushr v0.4s, v0.4s, #24\n"
            "umaxv s0, v0.4s\n"
            "umov %w[a], v0.s[0]\n"
            : [a] "=&r"(alpha)
            : [p] "r"(row+c)
            : "v0","memory");
        if (alpha) return false;
        // Avoid unsigned underflow in last-c when this consumed the last pixel.
        if (last-c == 3) return true;
    }
#endif
    for (; c<=last; c++) if (row[c] >> 24) return false;
    return true;
}


/* Classify the full interpolation footprint, not just output sample centers.
 * 0/255 mean all transparent/opaque; 256 means mixed. RGB is irrelevant to
 * these two extreme alpha cases. No padding reads or state cached by seed. */
static inline unsigned minifb_alpha_span(const uint32_t *row, uint32_t first, uint32_t last)
{
    if (first>last) return 256;
    unsigned alpha=row[first]>>24;
    if (alpha!=0 && alpha!=255) return 256;
    uint32_t c=first;
#if defined(__arm64__) || defined(__aarch64__)
    for (; last-c>=3; c+=4) {
        uint32_t same;
        __asm__ volatile(
            "ldr q0, [%[p]]\n"
            "ushr v0.4s, v0.4s, #24\n"
            "dup v1.4s, %w[alpha]\n"
            "cmeq v0.4s, v0.4s, v1.4s\n"
            "uminv s0, v0.4s\n"
            "umov %w[same], v0.s[0]\n"
            : [same] "=&r"(same)
            : [p] "r"(row+c), [alpha] "r"(alpha)
            : "v0","v1","memory");
        if (!same) return 256;
        if (last-c==3) return alpha;
    }
#endif
    for (; c<=last; c++) if ((row[c]>>24)!=alpha) return 256;
    return alpha;
}
