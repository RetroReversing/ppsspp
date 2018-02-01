// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <string.h>
#include <algorithm>

#include "profiler/profiler.h"

#include "Common/CPUDetect.h"
#include "Common/MemoryUtil.h"
#include "Core/Config.h"

#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"  // only needed for UVScale stuff

static void CopyQuadIndex(u16 *&indices, GEPatchPrimType type, const int idx0, const int idx1, const int idx2, const int idx3) {
	if (type == GE_PATCHPRIM_LINES) {
		*(indices++) = idx0;
		*(indices++) = idx2;
		*(indices++) = idx1;
		*(indices++) = idx3;
		*(indices++) = idx1;
		*(indices++) = idx2;
	} else {
		*(indices++) = idx0;
		*(indices++) = idx2;
		*(indices++) = idx1;
		*(indices++) = idx1;
		*(indices++) = idx2;
		*(indices++) = idx3;
	}
}

static void BuildIndex(u16 *indices, int &count, int num_u, int num_v, GEPatchPrimType prim_type, int total = 0) {
	for (int v = 0; v < num_v; ++v) {
		for (int u = 0; u < num_u; ++u) {
			int idx0 = v * (num_u + 1) + u + total; // Top left
			int idx2 = (v + 1) * (num_u + 1) + u + total; // Bottom left

			CopyQuadIndex(indices, prim_type, idx0, idx0 + 1, idx2, idx2 + 1);
			count += 6;
		}
	}
}

struct Weight {
	float weights[4], derivs[4];
};

class Bezier3DWeight {
private:
	void CalcWeights(float t, Weight &w) {
		// Bernstein 3D basis polynomial
		w.weights[0] = (1 - t) * (1 - t) * (1 - t);
		w.weights[1] = 3 * t * (1 - t) * (1 - t);
		w.weights[2] = 3 * t * t * (1 - t);
		w.weights[3] = t * t * t;

		// Derivative
		w.derivs[0] = -3 * (1 - t) * (1 - t);
		w.derivs[1] = 9 * t * t - 12 * t + 3;
		w.derivs[2] = 3 * (2 - 3 * t) * t;
		w.derivs[3] = 3 * t * t;
	}
public:
	Weight *CalcWeightsAll(u32 key) {
		int tess = (int)key;
		Weight *weights = new Weight[tess + 1];
		const float inv_u = 1.0f / (float)tess;
		for (int i = 0; i < tess + 1; ++i) {
			const float t = (float)i * inv_u;
			CalcWeights(t, weights[i]);
		}
		return weights;
	}
};

class Spline3DWeight {
private:
	struct KnotDiv {
		float _3_0 = 1.0f / 3.0f;
		float _4_1 = 1.0f / 3.0f;
		float _5_2 = 1.0f / 3.0f;
		float _3_1 = 1.0f / 2.0f;
		float _4_2 = 1.0f / 2.0f;
		float _3_2 = 1.0f; // Always 1
	};

	// knot should be an array sized n + 5  (n + 1 + 1 + degree (cubic))
	void CalcKnots(int n, int type, float *knots, KnotDiv *divs) {
		// Basic theory (-2 to +3), optimized with KnotDiv (-2 to +0) 
	//	for (int i = 0; i < n + 5; ++i) {
		for (int i = 0; i < n + 2; ++i) {
			knots[i] = (float)i - 2;
		}

		// The first edge is open
		if ((type & 1) != 0) {
			knots[0] = 0;
			knots[1] = 0;

			divs[0]._3_0 = 1.0f;
			divs[0]._4_1 = 1.0f / 2.0f;
			divs[0]._3_1 = 1.0f;
			if (n > 1)
				divs[1]._3_0 = 1.0f / 2.0f;
		}
		// The last edge is open
		if ((type & 2) != 0) {
			//	knots[n + 2] = (float)n; // Got rid of this line optimized with KnotDiv
			//	knots[n + 3] = (float)n; // Got rid of this line optimized with KnotDiv
			//	knots[n + 4] = (float)n; // Got rid of this line optimized with KnotDiv
			divs[n - 1]._4_1 = 1.0f / 2.0f;
			divs[n - 1]._5_2 = 1.0f;
			divs[n - 1]._4_2 = 1.0f;
			if (n > 1)
				divs[n - 2]._5_2 = 1.0f / 2.0f;
		}
	}

	void CalcWeights(float t, const float *knots, const KnotDiv &div, Weight &w) {
#ifdef _M_SSE
		const __m128 knot012 = _mm_loadu_ps(knots);
		const __m128 t012 = _mm_sub_ps(_mm_set_ps1(t), knot012);
		const __m128 f30_41_52 = _mm_mul_ps(t012, _mm_loadu_ps(&div._3_0));
		const __m128 f52_31_42 = _mm_mul_ps(t012, _mm_loadu_ps(&div._5_2));
		const float &f32 = t012.m128_f32[2];

		// Following comments are for explains order of the multiply.
	//	float a = (1-f30)*(1-f31);
	//	float c = (1-f41)*(1-f42);
	//	float b = (  f31 *   f41);
	//	float d = (  f42 *   f52);
		const __m128 f30_41_31_42 = _mm_shuffle_ps(f30_41_52, f52_31_42, _MM_SHUFFLE(2, 1, 1, 0));
		const __m128 f31_42_41_52 = _mm_shuffle_ps(f52_31_42, f30_41_52, _MM_SHUFFLE(2, 1, 2, 1));
		const __m128 c1_1_0_0 = { 1, 1, 0, 0 };
		const __m128 acbd = _mm_mul_ps(_mm_sub_ps(c1_1_0_0, f30_41_31_42), _mm_sub_ps(c1_1_0_0, f31_42_41_52));
		const float &a = acbd.m128_f32[0];
		const float &b = acbd.m128_f32[2];
		const float &c = acbd.m128_f32[1];
		const float &d = acbd.m128_f32[3];

		// For derivative
		const float &f31 = f30_41_31_42.m128_f32[2];
		const float &f42 = f30_41_31_42.m128_f32[3];
#else
		// TODO: Maybe compilers could be coaxed into vectorizing this code without the above explicitly...
		float t0 = (t - knots[0]);
		float t1 = (t - knots[1]);
		float t2 = (t - knots[2]);

		float f30 = t0 * div._3_0;
		float f41 = t1 * div._4_1;
		float f52 = t2 * div._5_2;
		float f31 = t1 * div._3_1;
		float f42 = t2 * div._4_2;
		float f32 = t2 * div._3_2;

		float a = (1 - f30) * (1 - f31);
		float b = (f31 * f41);
		float c = (1 - f41) * (1 - f42);
		float d = (f42 * f52);
#endif
		w.weights[0] = a * (1 - f32); // (1-f30)*(1-f31)*(1-f32)
		w.weights[1] = 1 - a - b + ((a + b + c - 1) * f32);
		w.weights[2] = b + ((1 - b - c - d) * f32);
		w.weights[3] = d * f32; // f32*f42*f52

		// Derivative
		float i1 = (1 - f31) * (1 - f32);
		float i2 = f31 * (1 - f32) + (1 - f42) * f32;
		float i3 = f42 * f32;

		float f130 = i1 * div._3_0;
		float f241 = i2 * div._4_1;
		float f352 = i3 * div._5_2;

		w.derivs[0] = 3 * (0 - f130);
		w.derivs[1] = 3 * (f130 - f241);
		w.derivs[2] = 3 * (f241 - f352);
		w.derivs[3] = 3 * (f352 - 0);
	}
public:
	Weight *CalcWeightsAll(u32 key) {
		int tess, count, type;
		FromKey(key, tess, count, type);
		const int num_patches = count - 3;
		Weight *weights = new Weight[tess * num_patches + 1];

	//	float *knots = new float[num_patches + 5];
		float *knots = new float[num_patches + 2]; // Optimized with KnotDiv, must use +5 in theory 
		KnotDiv *divs = new KnotDiv[num_patches];
		CalcKnots(num_patches, type, knots, divs);

		const float inv_tess = 1.0f / (float)tess;
		for (int i = 0; i < num_patches; ++i) {
			const int _tess = (i == num_patches - 1) ? (tess + 1) : tess;
			for (int j = 0; j < _tess; ++j) {
				const int index = i * tess + j;
				const float t = (float)index * inv_tess;
				CalcWeights(t, knots + i, divs[i], weights[index]);
			}
		}

		delete[] knots;
		delete[] divs;

		return weights;
	}

	u32 ToKey(int tess, int count, int type) {
		return tess | (count << 8) | (type << 16);
	}

	void FromKey(u32 key, int &tess, int &count, int &type) {
		tess = key & 0xFF; count = (key >> 8) & 0xFF; type = (key >> 16) & 0xFF;
	}
};

template<class T>
class WeightCache : public T {
private:
	std::unordered_map<u32, Weight*> weightsCache;
public:
	Weight* operator [] (u32 key) {
		Weight *&weights = weightsCache[key];
		if (!weights)
			weights = CalcWeightsAll(key);
		return weights;
	}

	void Clear() {
		for (auto it : weightsCache)
			delete[] it.second;
		weightsCache.clear();
	}
};

static WeightCache<Bezier3DWeight> bezierWeightsCache;
static WeightCache<Spline3DWeight> splineWeightsCache;

struct Weight2D {
	const Weight *u, *v;

	template<class T>
	Weight2D(WeightCache<T> &cache, u32 key_u, u32 key_v) {
		u = cache[key_u];
		v = (key_u != key_v) ? cache[key_v] : u; // Use same weights if u == v
	}
};

void DrawEngineCommon::ClearSplineBezierWeights() {
	bezierWeightsCache.Clear();
	splineWeightsCache.Clear();
}

bool CanUseHardwareTessellation(GEPatchPrimType prim) {
	if (g_Config.bHardwareTessellation && !g_Config.bSoftwareRendering) {
		return CanUseHardwareTransform(PatchPrimToPrim(prim));
	}
	return false;
}

// Prepare mesh of one patch for "Instanced Tessellation".
static void TessellateSplinePatchHardware(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch) {
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float inv_u = 1.0f / (float)spatch.tess_u;
	float inv_v = 1.0f / (float)spatch.tess_v;

	// Generating simple input vertices for the spline-computing vertex shader.
	for (int tile_v = 0; tile_v < spatch.tess_v + 1; ++tile_v) {
		for (int tile_u = 0; tile_u < spatch.tess_u + 1; ++tile_u) {
			SimpleVertex &vert = vertices[tile_v * (spatch.tess_u + 1) + tile_u];
			vert.pos.x = (float)tile_u * inv_u;
			vert.pos.y = (float)tile_v * inv_v;

			// TODO: Move to shader uniform and unify this method spline and bezier if necessary.
			// For compute normal
			vert.nrm.x = inv_u;
			vert.nrm.y = inv_v;
		}
	}

	BuildIndex(indices, count, spatch.tess_u, spatch.tess_v, spatch.primType);
}

template <bool sampleNrm, bool sampleCol, bool sampleTex, bool useSSE4>
static void SplinePatchFullQuality(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	// Full (mostly) correct tessellation of spline patches.
	// Not very fast.

	u32 key_u = splineWeightsCache.ToKey(spatch.tess_u, spatch.count_u, spatch.type_u);
	u32 key_v = splineWeightsCache.ToKey(spatch.tess_v, spatch.count_v, spatch.type_v);
	Weight2D weights(splineWeightsCache, key_u, key_v);

	// Increase tessellation based on the size. Should be approximately right?
	int patch_div_s = (spatch.count_u - 3) * spatch.tess_u;
	int patch_div_t = (spatch.count_v - 3) * spatch.tess_v;
	if (quality == 0) {
		// Low quality
		patch_div_s = (spatch.count_u - 3) * 2;
		patch_div_t = (spatch.count_v - 3) * 2;
	}
	if (quality > 1) {
		// Don't cut below 2, though.
		if (patch_div_s > 2) {
			patch_div_s /= quality;
		}
		if (patch_div_t > 2) {
			patch_div_t /= quality;
		}
	}

	// Downsample until it fits, in case crazy tessellation factors are sent.
	while ((patch_div_s + 1) * (patch_div_t + 1) > maxVertices) {
		patch_div_s /= 2;
		patch_div_t /= 2;
	}

	if (patch_div_s < 1) patch_div_s = 1;
	if (patch_div_t < 1) patch_div_t = 1;

	// First compute all the vertices and put them in an array
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	const float inv_u = 1.0f / (float)spatch.tess_u;
	const float inv_v = 1.0f / (float)spatch.tess_v;

	int num_patches_u = spatch.count_u - 3;
	int num_patches_v = spatch.count_v - 3;
	for (int patch_u = 0; patch_u < num_patches_u; ++patch_u) {
		int tess_u = (patch_u - 1 == num_patches_u) ? spatch.tess_u + 1 : spatch.tess_u;
		for (int patch_v = 0; patch_v < num_patches_v; ++patch_v) {
			int tess_v = (patch_v - 1 == num_patches_v) ? spatch.tess_v + 1 : spatch.tess_v;

			// Prepare 4x4 control points to tessellate
			const int idx = patch_v * spatch.count_u + patch_u;
			const int idx_v[4] = { idx, idx + spatch.count_u, idx + spatch.count_u * 2, idx + spatch.count_u * 3 };
			Tessellator<Vec3f> tess_pos(spatch.pos, idx_v);
			Tessellator<Vec4f> tess_col(spatch.col, idx_v);
			Tessellator<Vec2f> tess_tex(spatch.tex, idx_v);
			Tessellator<Vec3f> tess_nrm(spatch.pos, idx_v);

			for (int tile_u = 0; tile_u < tess_u + 1; ++tile_u) {
				int index_u = patch_u * spatch.tess_u + tile_u;
				const Weight &wu = weights.u[index_u];

				// Pre-tessellate U lines
				tess_pos.SampleU(wu.weights);
				if (sampleCol)
					tess_col.SampleU(wu.weights);
				if (sampleTex)
					tess_tex.SampleU(wu.weights);
				if (sampleNrm)
					tess_nrm.SampleU(wu.derivs);

				for (int tile_v = 0; tile_v < tess_v + 1; ++tile_v) {
					int index_v = patch_v * spatch.tess_v + tile_v;
					const Weight &wv = weights.v[index_v];

					SimpleVertex &vert = vertices[index_v * (patch_div_s + 1) + index_u];

					// Tessellate
					vert.pos = tess_pos.SampleV(wv.weights);
					if (sampleCol) {
						vert.color_32 = tess_col.SampleV(wv.weights).ToRGBA();
					} else {
						vert.color_32 = spatch.defcolor;
					}
					if (sampleTex) {
						tess_tex.SampleV(wv.weights).Write(vert.uv);
					} else {
						// Generate texcoord
						vert.uv[0] = patch_u + tile_u * inv_u;
						vert.uv[1] = patch_v + tile_v * inv_v;
					}
					if (sampleNrm) {
						const Vec3f derivU = tess_nrm.SampleV(wv.weights);
						const Vec3f derivV = tess_pos.SampleV(wv.derivs);

						vert.nrm = Cross(derivU, derivV).Normalized(useSSE4);
						if (spatch.patchFacing)
							vert.nrm *= -1.0f;
					} else {
						vert.nrm.SetZero();
					}
				}
			}
		}
	}

	BuildIndex(indices, count, patch_div_s, patch_div_t, spatch.primType);
}

template <bool origNrm, bool origCol, bool origTc>
static inline void SplinePatchFullQualityDispatch4(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	if (cpu_info.bSSE4_1)
		SplinePatchFullQuality<origNrm, origCol, origTc, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQuality<origNrm, origCol, origTc, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

template <bool origNrm, bool origCol>
static inline void SplinePatchFullQualityDispatch3(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origTc = (origVertType & GE_VTYPE_TC_MASK) != 0;

	if (origTc)
		SplinePatchFullQualityDispatch4<origNrm, origCol, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch4<origNrm, origCol, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

template <bool origNrm>
static inline void SplinePatchFullQualityDispatch2(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origCol = (origVertType & GE_VTYPE_COL_MASK) != 0;

	if (origCol)
		SplinePatchFullQualityDispatch3<origNrm, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch3<origNrm, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

static void SplinePatchFullQualityDispatch(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origNrm = (origVertType & GE_VTYPE_NRM_MASK) != 0;

	if (origNrm)
		SplinePatchFullQualityDispatch2<true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch2<false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

void TessellateSplinePatch(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int maxVertexCount) {
	switch (g_Config.iSplineBezierQuality) {
	case LOW_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 0, maxVertexCount);
		break;
	case MEDIUM_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 2, maxVertexCount);
		break;
	case HIGH_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 1, maxVertexCount);
		break;
	}
}

// Tessellate single patch (4x4 control points)
template<typename T>
class Tessellator {
private:
	const T *const p[4]; // T p[v][u]; 4x4 control points
	T u[4]; // Pre-tessellated U lines
public:
	Tessellator(const T *p, const int idx[4]) : p{ p + idx[0], p + idx[1], p + idx[2], p + idx[3] } {}

	// Linear combination
	T Sample(const T p[4], const float w[4]) {
		return p[0] * w[0] + p[1] * w[1] + p[2] * w[2] + p[3] * w[3];
	}

	void SampleEdgeU(int idx) {
		u[0] = p[0][idx];
		u[1] = p[1][idx];
		u[2] = p[2][idx];
		u[3] = p[3][idx];
	}

	void SampleU(const float weights[4]) {
		if (weights[0] == 1.0f) { SampleEdgeU(0); return; } // weights = {1,0,0,0}, first edge is open.
		if (weights[3] == 1.0f) { SampleEdgeU(3); return; } // weights = {0,0,0,1}, last edge is open.

		u[0] = Sample(p[0], weights);
		u[1] = Sample(p[1], weights);
		u[2] = Sample(p[2], weights);
		u[3] = Sample(p[3], weights);
	}

	T SampleV(const float weights[4]) {
		if (weights[0] == 1.0f) return u[0]; // weights = {1,0,0,0}, first edge is open.
		if (weights[3] == 1.0f) return u[3]; // weights = {0,0,0,1}, last edge is open.

		return Sample(u, weights);
	}
};

static void _BezierPatchHighQuality(u8 *&dest, u16 *&indices, int &count, int tess_u, int tess_v, const BezierPatch &patch, u32 origVertType) {
	const float inv_u = 1.0f / (float)tess_u;
	const float inv_v = 1.0f / (float)tess_v;

	// First compute all the vertices and put them in an array
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	const bool sampleNrm = (origVertType & GE_VTYPE_NRM_MASK) != 0;
	const bool sampleCol = (origVertType & GE_VTYPE_COL_MASK) != 0;
	const bool sampleTex = (origVertType & GE_VTYPE_TC_MASK) != 0;

	Weight2D weights(bezierWeightsCache, tess_u, tess_v);

	int num_patches_u = (patch.count_u - 1) / 3;
	int num_patches_v = (patch.count_v - 1) / 3;
	for (int patch_u = 0; patch_u < num_patches_u; ++patch_u) {
		for (int patch_v = 0; patch_v < num_patches_v; ++patch_v) {

			// Prepare 4x4 control points to tessellate
			const int idx = patch_v * 3 * patch.count_u + patch_u * 3;
			const int idx_v[4] = { idx, idx + patch.count_u, idx + patch.count_u * 2, idx + patch.count_u * 3 };
			Tessellator<Vec3f> tess_pos(patch.pos, idx_v);
			Tessellator<Vec4f> tess_col(patch.col, idx_v);
			Tessellator<Vec2f> tess_tex(patch.tex, idx_v);
			Tessellator<Vec3f> tess_nrm(patch.pos, idx_v);

			for (int tile_u = 0; tile_u < tess_u + 1; ++tile_u) {
				const Weight &wu = weights.u[tile_u];

				// Pre-tessellate U lines
				tess_pos.SampleU(wu.weights);
				if (sampleCol)
					tess_col.SampleU(wu.weights);
				if (sampleTex)
					tess_tex.SampleU(wu.weights);
				if (sampleNrm)
					tess_nrm.SampleU(wu.derivs);

				for (int tile_v = 0; tile_v < tess_v + 1; ++tile_v) {
					const Weight &wv = weights.v[tile_v];

					SimpleVertex &vert = vertices[tile_v * (tess_u + 1) + tile_u];

					// Tessellate
					vert.pos = tess_pos.SampleV(wv.weights);
					if (sampleCol) {
						vert.color_32 = tess_col.SampleV(wv.weights).ToRGBA();
					} else {
						vert.color_32 = patch.defcolor;
					}
					if (sampleTex) {
						tess_tex.SampleV(wv.weights).Write(vert.uv);
					} else {
						// Generate texcoord
						vert.uv[0] = patch_u + tile_u * inv_u;
						vert.uv[1] = patch_v + tile_v * inv_v;
					}
					if (sampleNrm) {
						const Vec3f derivU = tess_nrm.SampleV(wv.weights);
						const Vec3f derivV = tess_pos.SampleV(wv.derivs);

						vert.nrm = Cross(derivU, derivV).Normalized();
						if (patch.patchFacing)
							vert.nrm *= -1.0f;
					} else {
						vert.nrm.SetZero();
					}
				}
			}

			int patch_index = patch_v * num_patches_u + patch_u;
			int total = patch_index * (tess_u + 1) * (tess_v + 1);
			BuildIndex(indices + count, count, tess_u, tess_v, patch.primType, total);

			dest += (tess_u + 1) * (tess_v + 1) * sizeof(SimpleVertex);
		}
	}
}

// Prepare mesh of one patch for "Instanced Tessellation".
static void TessellateBezierPatchHardware(u8 *&dest, u16 *indices, int &count, int tess_u, int tess_v, GEPatchPrimType primType) {
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float inv_u = 1.0f / (float)tess_u;
	float inv_v = 1.0f / (float)tess_v;

	// Generating simple input vertices for the bezier-computing vertex shader.
	for (int tile_v = 0; tile_v < tess_v + 1; ++tile_v) {
		for (int tile_u = 0; tile_u < tess_u + 1; ++tile_u) {
			SimpleVertex &vert = vertices[tile_v * (tess_u + 1) + tile_u];

			vert.pos.x = (float)tile_u * inv_u;
			vert.pos.y = (float)tile_v * inv_v;
		}
	}

	BuildIndex(indices, count, tess_u, tess_v, primType);
}

void TessellateBezierPatch(u8 *&dest, u16 *&indices, int &count, int tess_u, int tess_v, const BezierPatch &patch, u32 origVertType) {
	switch (g_Config.iSplineBezierQuality) {
	case LOW_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, 2, 2, patch, origVertType);
		break;
	case MEDIUM_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, std::max(tess_u / 2, 1), std::max(tess_v / 2, 1), patch, origVertType);
		break;
	case HIGH_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, tess_u, tess_v, patch, origVertType);
		break;
	}
}

static void CopyControlPoints(const SimpleVertex *const *points, float *pos, float *tex, float *col, int posStride, int texStride, int colStride, int size, bool hasColor, bool hasTexCoords) {
	for (int idx = 0; idx < size; idx++) {
		memcpy(pos, points[idx]->pos.AsArray(), 3 * sizeof(float));
		pos += posStride;
		if (hasTexCoords) {
			memcpy(tex, points[idx]->uv, 2 * sizeof(float));
			tex += texStride;
		}
		if (hasColor) {
			memcpy(col, Vec4f::FromRGBA(points[idx]->color_32).AsArray(), 4 * sizeof(float));
			col += colStride;
		}
	}
	if (!hasColor)
		memcpy(col, Vec4f::FromRGBA(points[0]->color_32).AsArray(), 4 * sizeof(float));
}

class SimpleBufferManager {
private:
	u8 *buf_;
	size_t totalSize, maxSize_;
public:
	SimpleBufferManager(u8 *buf, size_t maxSize)
		: buf_(buf), totalSize(0), maxSize_(maxSize) {}

	u8 *Allocate(size_t size) {
		size = (size + 15) & ~15; // Align for 16 bytes

		if ((totalSize + size) > maxSize_)
			return nullptr; // No more memory

		size_t tmp = totalSize;
		totalSize += size;
		return buf_ + tmp;
	}
};

// This maps GEPatchPrimType to GEPrimitiveType.
const GEPrimitiveType primType[] = { GE_PRIM_TRIANGLES, GE_PRIM_LINES, GE_PRIM_POINTS, GE_PRIM_POINTS };

void DrawEngineCommon::SubmitSpline(const void *control_points, const void *indices, int tess_u, int tess_v, int count_u, int count_v, int type_u, int type_v, GEPatchPrimType prim_type, bool computeNormals, bool patchFacing, u32 vertType, int *bytesRead) {
	PROFILE_THIS_SCOPE("spline");
	DispatchFlush();

	// Real hardware seems to draw nothing when given < 4 either U or V.
	if (count_u < 4 || count_v < 4)
		return;

	SimpleBufferManager managedBuf(decoded, DECODED_VERTEX_BUFFER_SIZE);

	u16 index_lower_bound = 0;
	u16 index_upper_bound = count_u * count_v - 1;
	IndexConverter ConvertIndex(vertType, indices);
	if (indices)
		GetIndexBounds(indices, count_u * count_v, vertType, &index_lower_bound, &index_upper_bound);

	VertexDecoder *origVDecoder = GetVertexDecoder((vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24));
	*bytesRead = count_u * count_v * origVDecoder->VertexSize();

	// Simplify away bones and morph before proceeding
	SimpleVertex *simplified_control_points = (SimpleVertex *)managedBuf.Allocate(sizeof(SimpleVertex) * (index_upper_bound + 1));
	u8 *temp_buffer = managedBuf.Allocate(sizeof(SimpleVertex) * count_u * count_v);

	u32 origVertType = vertType;
	vertType = NormalizeVertices((u8 *)simplified_control_points, temp_buffer, (u8 *)control_points, index_lower_bound, index_upper_bound, vertType);

	VertexDecoder *vdecoder = GetVertexDecoder(vertType);

	int vertexSize = vdecoder->VertexSize();
	if (vertexSize != sizeof(SimpleVertex)) {
		ERROR_LOG(G3D, "Something went really wrong, vertex size: %i vs %i", vertexSize, (int)sizeof(SimpleVertex));
	}

	// Make an array of pointers to the control points, to get rid of indices.
	const SimpleVertex **points = (const SimpleVertex **)managedBuf.Allocate(sizeof(SimpleVertex *) * count_u * count_v);
	for (int idx = 0; idx < count_u * count_v; idx++)
		points[idx] = simplified_control_points + (indices ? ConvertIndex(idx) : idx);

	int count = 0;
	u8 *dest = splineBuffer;

	SplinePatchLocal patch;
	patch.tess_u = tess_u;
	patch.tess_v = tess_v;
	patch.type_u = type_u;
	patch.type_v = type_v;
	patch.count_u = count_u;
	patch.count_v = count_v;
	patch.primType = prim_type;
	patch.patchFacing = patchFacing;
	patch.defcolor = points[0]->color_32;

	if (CanUseHardwareTessellation(prim_type)) {
		tessDataTransfer->SendDataToShader(points, count_u * count_v, origVertType);
		TessellateSplinePatchHardware(dest, quadIndices_, count, patch);
		numPatches = (count_u - 3) * (count_v - 3);
	} else {
		patch.pos = (Vec3f *)managedBuf.Allocate(sizeof(Vec3f) * count_u * count_v);
		patch.tex = (Vec2f *)managedBuf.Allocate(sizeof(Vec2f) * count_u * count_v);
		patch.col = (Vec4f *)managedBuf.Allocate(sizeof(Vec4f) * count_u * count_v);
		for (int idx = 0; idx < count_u * count_v; idx++) {
			patch.pos[idx] = Vec3f(points[idx]->pos);
			patch.tex[idx] = Vec2f(points[idx]->uv);
			patch.col[idx] = Vec4f::FromRGBA(points[idx]->color_32);
		}
		int maxVertexCount = SPLINE_BUFFER_SIZE / vertexSize;
		TessellateSplinePatch(dest, quadIndices_, count, patch, origVertType, maxVertexCount);
	}

	u32 vertTypeWithIndex16 = (vertType & ~GE_VTYPE_IDX_MASK) | GE_VTYPE_IDX_16BIT;

	UVScale prevUVScale;
	if ((origVertType & GE_VTYPE_TC_MASK) != 0) {
		// We scaled during Normalize already so let's turn it off when drawing.
		prevUVScale = gstate_c.uv;
		gstate_c.uv.uScale = 1.0f;
		gstate_c.uv.vScale = 1.0f;
		gstate_c.uv.uOff = 0.0f;
		gstate_c.uv.vOff = 0.0f;
	}

	uint32_t vertTypeID = GetVertTypeID(vertTypeWithIndex16, gstate.getUVGenMode());

	int generatedBytesRead;
	DispatchSubmitPrim(splineBuffer, quadIndices_, PatchPrimToPrim(prim_type), count, vertTypeID, &generatedBytesRead);

	DispatchFlush();

	if ((origVertType & GE_VTYPE_TC_MASK) != 0) {
		gstate_c.uv = prevUVScale;
	}
}

void DrawEngineCommon::SubmitBezier(const void *control_points, const void *indices, int tess_u, int tess_v, int count_u, int count_v, GEPatchPrimType prim_type, bool computeNormals, bool patchFacing, u32 vertType, int *bytesRead) {
	PROFILE_THIS_SCOPE("bezier");
	DispatchFlush();

	// Real hardware seems to draw nothing when given < 4 either U or V.
	// This would result in num_patches_u / num_patches_v being 0.
	if (count_u < 4 || count_v < 4)
		return;

	SimpleBufferManager managedBuf(decoded, DECODED_VERTEX_BUFFER_SIZE);

	u16 index_lower_bound = 0;
	u16 index_upper_bound = count_u * count_v - 1;
	IndexConverter ConvertIndex(vertType, indices);
	if (indices)
		GetIndexBounds(indices, count_u*count_v, vertType, &index_lower_bound, &index_upper_bound);

	VertexDecoder *origVDecoder = GetVertexDecoder((vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24));
	*bytesRead = count_u * count_v * origVDecoder->VertexSize();

	// Simplify away bones and morph before proceeding
	// There are normally not a lot of control points so just splitting decoded should be reasonably safe, although not great.
	SimpleVertex *simplified_control_points = (SimpleVertex *)managedBuf.Allocate(sizeof(SimpleVertex) * (index_upper_bound + 1));
	u8 *temp_buffer = managedBuf.Allocate(sizeof(SimpleVertex) * count_u * count_v);

	u32 origVertType = vertType;
	vertType = NormalizeVertices((u8 *)simplified_control_points, temp_buffer, (u8 *)control_points, index_lower_bound, index_upper_bound, vertType);

	VertexDecoder *vdecoder = GetVertexDecoder(vertType);

	int vertexSize = vdecoder->VertexSize();
	if (vertexSize != sizeof(SimpleVertex)) {
		ERROR_LOG(G3D, "Something went really wrong, vertex size: %i vs %i", vertexSize, (int)sizeof(SimpleVertex));
	}

	// If specified as 0, uses 1.
	if (tess_u < 1) tess_u = 1;
	if (tess_v < 1) tess_v = 1;

	// Make an array of pointers to the control points, to get rid of indices.
	const SimpleVertex **points = (const SimpleVertex **)managedBuf.Allocate(sizeof(SimpleVertex *) * count_u * count_v);
	for (int idx = 0; idx < count_u * count_v; idx++)
		points[idx] = simplified_control_points + (indices ? ConvertIndex(idx) : idx);

	int count = 0;
	u8 *dest = splineBuffer;
	u16 *inds = quadIndices_;

	// Bezier patches share less control points than spline patches. Otherwise they are pretty much the same (except bezier don't support the open/close thing)
	int num_patches_u = (count_u - 1) / 3;
	int num_patches_v = (count_v - 1) / 3;
	if (CanUseHardwareTessellation(prim_type)) {
		tessDataTransfer->SendDataToShader(points, count_u * count_v, origVertType);
		TessellateBezierPatchHardware(dest, inds, count, tess_u, tess_v, prim_type);
		numPatches = num_patches_u * num_patches_v;
	} else {
		BezierPatch patch;
		patch.count_u = count_u;
		patch.count_v = count_v;
		patch.primType = prim_type;
		patch.patchFacing = patchFacing;
		patch.defcolor = points[0]->color_32;
		patch.pos = (Vec3f *)managedBuf.Allocate(sizeof(Vec3f) * count_u * count_v);
		patch.tex = (Vec2f *)managedBuf.Allocate(sizeof(Vec2f) * count_u * count_v);
		patch.col = (Vec4f *)managedBuf.Allocate(sizeof(Vec4f) * count_u * count_v);
		for (int idx = 0; idx < count_u * count_v; idx++) {
			patch.pos[idx] = Vec3f(points[idx]->pos);
			patch.tex[idx] = Vec2f(points[idx]->uv);
			patch.col[idx] = Vec4f::FromRGBA(points[idx]->color_32);
		}
		int maxVertices = SPLINE_BUFFER_SIZE / vertexSize;
		// Downsample until it fits, in case crazy tessellation factors are sent.
		while ((tess_u + 1) * (tess_v + 1) * num_patches_u * num_patches_v > maxVertices) {
			tess_u /= 2;
			tess_v /= 2;
		}
		TessellateBezierPatch(dest, inds, count, tess_u, tess_v, patch, origVertType);
	}

	u32 vertTypeWithIndex16 = (vertType & ~GE_VTYPE_IDX_MASK) | GE_VTYPE_IDX_16BIT;

	UVScale prevUVScale;
	if (origVertType & GE_VTYPE_TC_MASK) {
		// We scaled during Normalize already so let's turn it off when drawing.
		prevUVScale = gstate_c.uv;
		gstate_c.uv.uScale = 1.0f;
		gstate_c.uv.vScale = 1.0f;
		gstate_c.uv.uOff = 0;
		gstate_c.uv.vOff = 0;
	}

	uint32_t vertTypeID = GetVertTypeID(vertTypeWithIndex16, gstate.getUVGenMode());
	int generatedBytesRead;
	DispatchSubmitPrim(splineBuffer, quadIndices_, PatchPrimToPrim(prim_type), count, vertTypeID, &generatedBytesRead);

	DispatchFlush();

	if (origVertType & GE_VTYPE_TC_MASK) {
		gstate_c.uv = prevUVScale;
	}
}
