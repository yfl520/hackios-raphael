/* where red and blue are in a cell (0 top left, 1 top right, 2 bottom left, 3 bottom right) */
static void
cell_places(uint32_t bayer, int *r, int *b)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };

	*r = red[bayer & 3];
	*b = blue[bayer & 3];
}

/* the four values of the cell at cx of the pair of lines from `top' (above black, 0 and up) */
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, int v[4])
{
	const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
	unsigned o = (cx & 1) * 2;

	v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - 64;
	v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - 64;
	v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - 64;
	v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - 64;
	for (int i = 0; i < 4; i++) {
		v[i] = v[i] < 0 ? 0 : v[i];
	}
}

float
isp_sharpness(const struct hakios_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level)
{
	enum { BLOCK = 4, MAX_BLOCKS = 512 };
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	uint32_t cx0 = (uint32_t)(x0 * cells_w), cx1 = (uint32_t)(x1 * cells_w), cy0 = (uint32_t)(y0 * cells_h),
	    cy1 = (uint32_t)(y1 * cells_h);
	float above[MAX_BLOCKS], here[MAX_BLOCKS];
	double steps = 0, sum = 0;
	uint32_t blocks_w, rows = 0, n = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	cx1 = cx1 > cells_w ? cells_w : cx1;
	cy1 = cy1 > cells_h ? cells_h : cy1;
	blocks_w = cx1 > cx0 ? (cx1 - cx0) / BLOCK : 0;
	blocks_w = blocks_w > MAX_BLOCKS ? MAX_BLOCKS : blocks_w;
	/*
	 * Green's mean over blocks of 4x4 cells, and the squares of the steps between neighbouring
	 * blocks: a cell's own noise is a quarter of itself in a block's mean, and the picture's edges are
	 * not -- in the dark, at 32x and more, the steps from cell to cell were the noise's alone and the
	 * focus went wherever it liked.
	 */
	for (uint32_t by = cy0; by + BLOCK <= cy1; by += BLOCK, rows++) {
		for (uint32_t bx = 0; bx < blocks_w; bx++) {
			uint32_t g = 0;

			for (uint32_t cy = by; cy < by + BLOCK; cy++) {
				const uint8_t *top = raw + (size_t)cy * 2 * f->stride;

				for (uint32_t cx = cx0 + bx * BLOCK; cx < cx0 + bx * BLOCK + BLOCK; cx++) {
					int v[4];

					cell(top, f->stride, cx, v);
					for (int i = 0; i < 4; i++) {
						g += i != r && i != b ? (uint32_t)v[i] : 0;
					}
				}
			}
			here[bx] = g / (2.0f * BLOCK * BLOCK);
			sum += here[bx];
			n++;
			if (bx > 0) {
				steps += (here[bx] - here[bx - 1]) * (here[bx] - here[bx - 1]);
			}
			if (rows > 0) {
				steps += (here[bx] - above[bx]) * (here[bx] - above[bx]);
			}
		}
		memcpy(above, here, sizeof(float) * blocks_w);
	}
	double mean = n ? sum / n : 0;

	if (level != NULL) {
		*level = (float)mean;
	}
	return mean > 0 ? (float)(steps / n / (mean * mean)) : 0;
}

