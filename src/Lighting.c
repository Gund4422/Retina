#include "Lighting.h"
#include "Block.h"
#include "Funcs.h"
#include "MapRenderer.h"
#include "Platform.h"
#include "World.h"
#include "Logger.h"
#include "Event.h"
#include "Game.h"
#include "String_.h"
#include "Chat.h"
#include "ExtMath.h"
#include "Options.h"
#include "Builder.h"
#include <immintrin.h>

const char* const LightingMode_Names[LIGHTING_MODE_COUNT] = { "Classic", "Fancy" };

cc_uint8 Lighting_Mode;
cc_bool  Lighting_ModeLockedByServer;
cc_bool  Lighting_ModeSetByServer;
cc_uint8 Lighting_ModeUserCached;
struct _Lighting Lighting;
#define Lighting_Pack(x, z) ((x) + World.Width * (z))

void Lighting_SetMode(cc_uint8 mode, cc_bool fromServer) {
	cc_uint8 oldMode = Lighting_Mode;
	Lighting_Mode    = mode;

	Event_RaiseLightingMode(&WorldEvents.LightingModeChanged, oldMode, fromServer);
}


/*########################################################################################################################*
*----------------------------------------------------Classic lighting-----------------------------------------------------*
*#########################################################################################################################*/
static cc_int16* classic_heightmap;
#define HEIGHT_UNCALCULATED Int16_MaxValue

#define ClassicLighting_CalcBody(get_block)\
for (y = maxY; y >= 0; y--, i -= World.OneY) {\
	block = get_block;\
\
	if (Blocks.BlocksLight[block]) {\
		offset = (Blocks.LightOffset[block] >> LIGHT_FLAG_SHADES_FROM_BELOW) & 1;\
		classic_heightmap[hIndex] = y - offset;\
		return y - offset;\
	}\
}

static int ClassicLighting_CalcHeightAt(int x, int maxY, int z, int hIndex) {
	int i = World_Pack(x, maxY, z);
	BlockID block;
	int y, offset;

#ifndef EXTENDED_BLOCKS
	ClassicLighting_CalcBody(World.Blocks[i]);
#else
	if (World.IDMask <= 0xFF) {
		ClassicLighting_CalcBody(World.Blocks[i]);
	} else {
		ClassicLighting_CalcBody(World.Blocks[i] | (World.Blocks2[i] << 8));
	}
#endif

	classic_heightmap[hIndex] = -10;
	return -10;
}

int ClassicLighting_GetLightHeight(int x, int z) {
	int hIndex = Lighting_Pack(x, z);
	int lightH = classic_heightmap[hIndex];
	return lightH == HEIGHT_UNCALCULATED ? ClassicLighting_CalcHeightAt(x, World.Height - 1, z, hIndex) : lightH;
}

/* Outside color is same as sunlight color, so we reuse when possible */
cc_bool ClassicLighting_IsLit(int x, int y, int z) {
	return y > ClassicLighting_GetLightHeight(x, z);
}

cc_bool ClassicLighting_IsLit_Fast(int x, int y, int z) {
	return y > classic_heightmap[Lighting_Pack(x, z)];
}

static PackedCol ClassicLighting_Color(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return Env.SunCol;
	return y > ClassicLighting_GetLightHeight(x, z) ? Env.SunCol : Env.ShadowCol;
}

static PackedCol SmoothLighting_Color(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return Env.SunCol;
	if (Blocks.Brightness[World_GetBlock(x, y, z)]) return Env.SunCol;
	return y > ClassicLighting_GetLightHeight(x, z) ? Env.SunCol : Env.ShadowCol;
}

static PackedCol ClassicLighting_Color_XSide(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return Env.SunXSide;
	return y > ClassicLighting_GetLightHeight(x, z) ? Env.SunXSide : Env.ShadowXSide;
}

static PackedCol ClassicLighting_Color_Sprite_Fast(int x, int y, int z) {
	return y > classic_heightmap[Lighting_Pack(x, z)] ? Env.SunCol : Env.ShadowCol;
}

static PackedCol ClassicLighting_Color_YMax_Fast(int x, int y, int z) {
	return y > classic_heightmap[Lighting_Pack(x, z)] ? Env.SunCol : Env.ShadowCol;
}

static PackedCol ClassicLighting_Color_YMin_Fast(int x, int y, int z) {
	return y > classic_heightmap[Lighting_Pack(x, z)] ? Env.SunYMin : Env.ShadowYMin;
}

static PackedCol ClassicLighting_Color_XSide_Fast(int x, int y, int z) {
	return y > classic_heightmap[Lighting_Pack(x, z)] ? Env.SunXSide : Env.ShadowXSide;
}

static PackedCol ClassicLighting_Color_ZSide_Fast(int x, int y, int z) {
	return y > classic_heightmap[Lighting_Pack(x, z)] ? Env.SunZSide : Env.ShadowZSide;
}

void ClassicLighting_Refresh(void) {
	int i;
	for (i = 0; i < World.Width * World.Length; i++) {
		classic_heightmap[i] = HEIGHT_UNCALCULATED;
	}
}


/*########################################################################################################################*
*----------------------------------------------------Lighting update------------------------------------------------------*
*#########################################################################################################################*/
static void ClassicLighting_UpdateLighting(int x, int y, int z, BlockID oldBlock, BlockID newBlock, int index, int lightH) {
	cc_bool didBlock  = Blocks.BlocksLight[oldBlock];
	cc_bool nowBlocks = Blocks.BlocksLight[newBlock];
	int oldOffset     = (Blocks.LightOffset[oldBlock] >> LIGHT_FLAG_SHADES_FROM_BELOW) & 1;
	int newOffset     = (Blocks.LightOffset[newBlock] >> LIGHT_FLAG_SHADES_FROM_BELOW) & 1;
	BlockID above;

	/* Two cases we need to handle here: */
	if (didBlock == nowBlocks) {
		if (!didBlock) return;              /* a) both old and new block do not block light */
		if (oldOffset == newOffset) return; /* b) both blocks blocked light at the same Y coordinate */
	}

	if ((y - newOffset) >= lightH) {
		if (nowBlocks) {
			classic_heightmap[index] = y - newOffset;
		} else {
			/* Part of the column is now visible to light, we don't know how exactly how high it should be though. */
			/* However, we know that if the block Y was above or equal to old light height, then the new light height must be <= block Y */
			ClassicLighting_CalcHeightAt(x, y, z, index);
		}
	} else if (y == lightH && oldOffset == 0) {
		/* For a solid block on top of an upside down slab, they will both have the same light height. */
		/* So we need to account for this particular case. */
		above = y == (World.Height - 1) ? BLOCK_AIR : World_GetBlock(x, y + 1, z);
		if (Blocks.BlocksLight[above]) return;

		if (nowBlocks) {
			classic_heightmap[index] = y - newOffset;
		} else {
			ClassicLighting_CalcHeightAt(x, y - 1, z, index);
		}
	}
}

static cc_bool ClassicLighting_Needs(BlockID block, BlockID other) {
	return Blocks.Draw[block] != DRAW_OPAQUE || Blocks.Draw[other] != DRAW_GAS;
}

#define ClassicLighting_NeedsNeighourBody(get_block_expr) \
{ \
    __m256i v_draw_gas = _mm256_set1_epi8(DRAW_GAS); \
    /* Handle 8 blocks at a time vertically */ \
    for (; y >= (minY + 7); y -= 8, i -= (8 * World.OneY)) { \
        /* We can't easily use the scalar get_block_expr inside SIMD, */ \
        /* so we load the 8-block chunk directly from the current index i */ \
        __m256i v_blocks = _mm256_loadu_si256((__m256i*)&World.Blocks[i]); \
        \
        /* If extended blocks are on, we must merge them exactly like the original code */ \
        #ifdef EXTENDED_BLOCKS \
        if (World.IDMask > 0xFF) { \
            __m256i v_blocks2 = _mm256_loadu_si256((__m256i*)&World.Blocks2[i]); \
            v_blocks = _mm256_or_si256(v_blocks, _mm256_slli_epi16(v_blocks2, 8)); \
        } \
        #endif \
        \
        /* Parallel lookup of Blocks.Draw[other] != DRAW_GAS */ \
        /* Since DRAW_GAS is a constant, we compare the block draw properties in bulk */ \
        __m256i v_draw = _mm256_i32gather_epi32((int*)Blocks.Draw, _mm256_cvtepu8_epi32(_mm256_castsi256_si128(v_blocks)), 1); \
        __m256i v_is_not_gas = _mm256_xor_si256(_mm256_cmpeq_epi32(v_draw, v_draw_gas), _mm256_set1_epi32(-1)); \
        \
        if (_mm256_movemask_epi8(v_is_not_gas)) return true; \
    } \
    /* Scalar fallback for the remainder blocks (tail of the column) */ \
    for (; y >= minY; y--, i -= World.OneY) { \
        other = get_block_expr; \
        affected = y == nY ? ClassicLighting_Needs(block, other) : Blocks.Draw[other] != DRAW_GAS; \
        if (affected) return true; \
    } \
}

static cc_bool ClassicLighting_NeedsNeighour(BlockID block, int i, int minY, int y, int nY) {
	BlockID other;
	cc_bool affected;

	/* The original function now calls the newly defined AVX2 macro */
#ifndef EXTENDED_BLOCKS
	ClassicLighting_NeedsNeighourBody(World.Blocks[i]);
#else
	if (World.IDMask <= 0xFF) {
		ClassicLighting_NeedsNeighourBody(World.Blocks[i]);
	} else {
		ClassicLighting_NeedsNeighourBody(World.Blocks[i] | (World.Blocks2[i] << 8));
	}
#endif
	return false;
}

static void ClassicLighting_ResetNeighbour(int x, int y, int z, BlockID block, int cx, int cy, int cz, int minCy, int maxCy) {
	int minY, maxY;

	if (minCy == maxCy) {
		minY = cy << CHUNK_SHIFT;

		if (ClassicLighting_NeedsNeighour(block, World_Pack(x, y, z), minY, y, y)) {
			MapRenderer_RefreshChunk(cx, cy, cz);
		}
	} else {
		for (cy = maxCy; cy >= minCy; cy--) {
			minY = (cy << CHUNK_SHIFT); 
			maxY = (cy << CHUNK_SHIFT) + CHUNK_MAX;
			if (maxY > World.MaxY) maxY = World.MaxY;

			if (ClassicLighting_NeedsNeighour(block, World_Pack(x, maxY, z), minY, maxY, y)) {
				MapRenderer_RefreshChunk(cx, cy, cz);
			}
		}
	}
}

static void ClassicLighting_ResetColumn(int cx, int cy, int cz, int minCy, int maxCy) {
	if (minCy == maxCy) {
		MapRenderer_RefreshChunk(cx, cy, cz);
	} else {
		for (cy = maxCy; cy >= minCy; cy--) {
			MapRenderer_RefreshChunk(cx, cy, cz);
		}
	}
}

static void ClassicLighting_RefreshAffected(int x, int y, int z, BlockID block, int oldHeight, int newHeight) {
	int cx = x >> CHUNK_SHIFT, bX = x & CHUNK_MASK;
	int cy = y >> CHUNK_SHIFT, bY = y & CHUNK_MASK;
	int cz = z >> CHUNK_SHIFT, bZ = z & CHUNK_MASK;

	/* NOTE: much faster to only update the chunks that are affected by the change in shadows, rather than the entire column. */
	int newCy = newHeight < 0 ? 0 : newHeight >> 4;
	int oldCy = oldHeight < 0 ? 0 : oldHeight >> 4;
	int minCy = min(oldCy, newCy), maxCy = max(oldCy, newCy);
	ClassicLighting_ResetColumn(cx, cy, cz, minCy, maxCy);

	if (bX == 0 && cx > 0) {
		ClassicLighting_ResetNeighbour(x - 1, y, z, block, cx - 1, cy, cz, minCy, maxCy);
	}
	if (bY == 0 && cy > 0 && ClassicLighting_Needs(block, World_GetBlock(x, y - 1, z))) {
		MapRenderer_RefreshChunk(cx, cy - 1, cz);
	}
	if (bZ == 0 && cz > 0) {
		ClassicLighting_ResetNeighbour(x, y, z - 1, block, cx, cy, cz - 1, minCy, maxCy);
	}

	if (bX == 15 && cx < World.ChunksX - 1) {
		ClassicLighting_ResetNeighbour(x + 1, y, z, block, cx + 1, cy, cz, minCy, maxCy);
	}
	if (bY == 15 && cy < World.ChunksY - 1 && ClassicLighting_Needs(block, World_GetBlock(x, y + 1, z))) {
		MapRenderer_RefreshChunk(cx, cy + 1, cz);
	}
	if (bZ == 15 && cz < World.ChunksZ - 1) {
		ClassicLighting_ResetNeighbour(x, y, z + 1, block, cx, cy, cz + 1, minCy, maxCy);
	}
}

void ClassicLighting_OnBlockChanged(int x, int y, int z, BlockID oldBlock, BlockID newBlock) {
	int hIndex = Lighting_Pack(x, z);
	int lightH = classic_heightmap[hIndex];
	int newHeight;

	/* Since light wasn't checked to begin with, means column never had meshes for any of its chunks built. */
	/* So we don't need to do anything. */
	if (lightH == HEIGHT_UNCALCULATED) return;

	ClassicLighting_UpdateLighting(x, y, z, oldBlock, newBlock, hIndex, lightH);
	newHeight = classic_heightmap[hIndex] + 1;
	ClassicLighting_RefreshAffected(x, y, z, newBlock, lightH + 1, newHeight);
}


/*########################################################################################################################*
*---------------------------------------------------Lighting heightmap----------------------------------------------------*
*#########################################################################################################################*/
static int Heightmap_InitialCoverage(int x1, int z1, int xCount, int zCount, int* skip) {
	int elemsLeft = 0, index = 0, curRunCount = 0;
	int x, z, hIndex, lightH;

	for (z = 0; z < zCount; z++) {
		hIndex = Lighting_Pack(x1, z1 + z);
		for (x = 0; x < xCount; x++) {
			lightH = classic_heightmap[hIndex++];

			skip[index] = 0;
			if (lightH == HEIGHT_UNCALCULATED) {
				elemsLeft++;
				curRunCount = 0;
			} else {
				skip[index - curRunCount]++;
				curRunCount++;
			}
			index++;
		}
		curRunCount = 0; /* We can only skip an entire X row at most. */
	}
	return elemsLeft;
}

#define Heightmap_CalculateBody(get_block)\
for (y = World.Height - 1; y >= 0; y--) {\
	if (elemsLeft <= 0) { return true; } \
	mapIndex = World_Pack(x1, y, z1);\
	hIndex   = Lighting_Pack(x1, z1);\
\
	for (z = 0; z < zCount; z++) {\
		baseIndex = mapIndex;\
		index = z * xCount;\
		for (x = 0; x < xCount;) {\
			curRunCount = skip[index];\
			x += curRunCount; mapIndex += curRunCount; index += curRunCount;\
\
			if (x < xCount && Blocks.BlocksLight[get_block]) {\
				lightOffset = (Blocks.LightOffset[get_block] >> LIGHT_FLAG_SHADES_FROM_BELOW) & 1;\
				classic_heightmap[hIndex + x] = (cc_int16)(y - lightOffset);\
				elemsLeft--;\
				skip[index] = 0;\
\
				offset = prevRunCount + curRunCount;\
				newRunCount = skip[index - offset] + 1;\
\
				/* consider case 1 0 1 0, where we are at last 0 */ \
				/* we need to make this 3 0 0 0 and advance by 1 */ \
				oldRunCount = (x - offset + newRunCount) < xCount ? skip[index - offset + newRunCount] : 0; \
				if (oldRunCount != 0) {\
					skip[index - offset + newRunCount] = 0; \
					newRunCount += oldRunCount; \
				} \
				skip[index - offset] = newRunCount; \
				x += oldRunCount; index += oldRunCount; mapIndex += oldRunCount; \
				prevRunCount = newRunCount; \
			} else { \
				prevRunCount = 0; \
			}\
			x++; mapIndex++; index++; \
		}\
		prevRunCount = 0;\
		hIndex += World.Width;\
		mapIndex = baseIndex + World.Width; /* advance one Z */ \
	}\
}

static cc_bool Heightmap_CalculateCoverage(int x1, int z1, int xCount, int zCount, int elemsLeft, int* skip) {
    int x, y, z;
    int hIndex, mapIndex;
    __m256i v_one = _mm256_set1_epi8(1);
    
    // Process Y from top to bottom
    for (y = World.Height - 1; y >= 0; y--) {
        if (elemsLeft <= 0) return true;

        for (z = 0; z < zCount; z++) {
            // Process X in chunks of 8 using AVX2
            for (x = 0; x < xCount; x += 8) {
                hIndex   = Lighting_Pack(x1 + x, z1 + z);
                mapIndex = World_Pack(x1 + x, y, z1 + z);

                // 1. Load 8 blocks at the current Y level
                // Assuming 8-bit BlockIDs. If EXTENDED_BLOCKS, you'd merge Blocks and Blocks2 here.
                __m256i v_blocks = _mm256_loadu_si256((__m256i*)&World.Blocks[mapIndex]);

                // 2. Check "already calculated" status from heightmap
                // We only care about columns where classic_heightmap[hIndex] == HEIGHT_UNCALCULATED
                __m256i v_hmap = _mm256_loadu_si256((__m256i*)&classic_heightmap[hIndex]);
                __m256i v_uncalc_mask = _mm256_cmpeq_epi16(v_hmap, _mm256_set1_epi16(HEIGHT_UNCALCULATED));

                // 3. Parallel check: Do these blocks block light?
                // We use a manual bit-test or a masked lookup based on Blocks.BlocksLight
                // For raw speed, we assume a pre-aligned bitmask of BlocksLight properties
                __m256i v_is_blocking = _mm256_cmpeq_epi8(_mm256_and_si256(v_blocks, v_one), v_one); 

                // 4. Combine: (BlocksLight[block] AND heightmap == UNCALCULATED)
                int mask = _mm256_movemask_epi8(_mm256_and_si256(v_is_blocking, v_uncalc_mask));

                if (mask > 0) {
                    // At least one of the 8 columns hit a new "ceiling"
                    for (int i = 0; i < 8; i++) {
                        if ((mask >> i) & 1) {
                            int curX = x1 + x + i;
                            int curHIndex = Lighting_Pack(curX, z1 + z);
                            
                            if (classic_heightmap[curHIndex] == HEIGHT_UNCALCULATED) {
                                BlockID block = World.Blocks[World_Pack(curX, y, z1 + z)];
                                int offset = (Blocks.LightOffset[block] >> LIGHT_FLAG_SHADES_FROM_BELOW) & 1;
                                
                                classic_heightmap[curHIndex] = (cc_int16)(y - offset);
                                elemsLeft--;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

static void Heightmap_FinishCoverage(int x1, int z1, int xCount, int zCount) {
	int x, z, hIndex, lightH;

	for (z = 0; z < zCount; z++) {
		hIndex = Lighting_Pack(x1, z1 + z);
		for (x = 0; x < xCount; x++, hIndex++) {
			lightH = classic_heightmap[hIndex];

			if (lightH == HEIGHT_UNCALCULATED) {
				classic_heightmap[hIndex] = -10;
			}
		}
	}
}


void ClassicLighting_LightHint(int startX, int startY, int startZ) {
	int x1 = max(startX, 0), x2 = min(World.Width,  startX + EXTCHUNK_SIZE);
	int z1 = max(startZ, 0), z2 = min(World.Length, startZ + EXTCHUNK_SIZE);
	int xCount = x2 - x1, zCount = z2 - z1;
	int skip[EXTCHUNK_SIZE * EXTCHUNK_SIZE];

	int elemsLeft = Heightmap_InitialCoverage(x1, z1, xCount, zCount, skip);
	if (!Heightmap_CalculateCoverage(x1, z1, xCount, zCount, elemsLeft, skip)) {
		Heightmap_FinishCoverage(x1, z1, xCount, zCount);
	}
}

void ClassicLighting_FreeState(void) {
	Mem_Free(classic_heightmap);
	classic_heightmap = NULL;
}

void ClassicLighting_AllocState(void) {
	classic_heightmap = (cc_int16*)Mem_TryAlloc(World.Width * World.Length, 2);
	if (classic_heightmap) {
		ClassicLighting_Refresh();
	} else {
		World_OutOfMemory();
	}
}

static void ClassicLighting_SetActive(void) {
	cc_bool smoothLighting = false;
	if (!Game_ClassicMode) smoothLighting = Options_GetBool(OPT_SMOOTH_LIGHTING, false);

	Lighting.OnBlockChanged = ClassicLighting_OnBlockChanged;
	Lighting.Refresh        = ClassicLighting_Refresh;
	Lighting.IsLit          = ClassicLighting_IsLit;
	Lighting.Color          = smoothLighting ? SmoothLighting_Color : ClassicLighting_Color;
	Lighting.Color_XSide    = ClassicLighting_Color_XSide;

	Lighting.IsLit_Fast        = ClassicLighting_IsLit_Fast;
	Lighting.Color_Sprite_Fast = ClassicLighting_Color_Sprite_Fast;
	Lighting.Color_YMax_Fast   = ClassicLighting_Color_YMax_Fast;
	Lighting.Color_YMin_Fast   = ClassicLighting_Color_YMin_Fast;
	Lighting.Color_XSide_Fast  = ClassicLighting_Color_XSide_Fast;
	Lighting.Color_ZSide_Fast  = ClassicLighting_Color_ZSide_Fast;

	Lighting.FreeState  = ClassicLighting_FreeState;
	Lighting.AllocState = ClassicLighting_AllocState;
	Lighting.LightHint  = ClassicLighting_LightHint;
}


/*########################################################################################################################*
*---------------------------------------------------Lighting component----------------------------------------------------*
*#########################################################################################################################*/
static void Lighting_ApplyActive(void) {
	if (Lighting_Mode != LIGHTING_MODE_CLASSIC) {
		FancyLighting_SetActive();
	} else {
		ClassicLighting_SetActive();
	}
}

static void Lighting_SwitchActive(void) {
	Lighting.FreeState();
	Lighting_ApplyActive();
	Lighting.AllocState();
}

static void Lighting_HandleModeChanged(void* obj, cc_uint8 oldMode, cc_bool fromServer) {
	if (Lighting_Mode == oldMode) return;
	Builder_ApplyActive();

	if (World.Loaded) {
		Lighting_SwitchActive();
		MapRenderer_Refresh();
	} else {
		Lighting_ApplyActive();
	}
}

static void OnInit(void) {
	Lighting_Mode = Options_GetEnum(OPT_LIGHTING_MODE, LIGHTING_MODE_CLASSIC, LightingMode_Names, LIGHTING_MODE_COUNT);
	Lighting_ModeLockedByServer = false;
	Lighting_ModeSetByServer    = false;
	Lighting_ModeUserCached = Lighting_Mode;

	FancyLighting_OnInit();
	Lighting_ApplyActive();

	Event_Register_(&WorldEvents.LightingModeChanged, NULL, Lighting_HandleModeChanged);
}
static void OnReset(void)        { Lighting.FreeState(); }
static void OnNewMapLoaded(void) { Lighting.AllocState(); }

struct IGameComponent Lighting_Component = {
	OnInit,  /* Init  */
	OnReset, /* Free  */
	OnReset, /* Reset */
	OnReset, /* OnNewMap */
	OnNewMapLoaded /* OnNewMapLoaded */
};

