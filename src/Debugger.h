#ifndef DEBUGGER_H

#include <set>
#include <list>
#include <array>
#include <vector>
#include <memory>
#include "Graphics/Context.h"
#include "Graphics/Parameters.h"
#include "gSP.h"
#include "gDP.h"
#include "Textures.h"

enum TriangleType {
	ttTriangle,
	ttTexrect,
	ttFillrect,
	ttBackground
};

class Debugger
{
public:
	Debugger();
	~Debugger();

	void checkDebugState();

	void addTriangles(const graphics::Context::DrawTriangleParameters & _params);
	void addRects(const graphics::Context::DrawRectParameters & _params);

	void draw();

	void performSceneRip();

	u32 getRippedFrames() { return m_rippedFrames; }

	bool isDebugMode() const { return m_bDebugMode; }
	bool isCaptureMode() const { return m_bCapture; }
	bool isRipMode() const { return m_bRip; }

	bool canPerformSceneRip();
	void resetContinuousRipMode();

private:
	struct TexInfo {
		f32 scales, scalet;
		const CachedTexture * texture;
		gDPLoadTileInfo texLoadInfo;
	};

	struct Vertex {
		f32 x, y, z, w;
		f32 sx, sy, sz;
		f32 r, g, b, a;
		f32 s0, t0, s1, t1;
		u32 modify;

		Vertex() = default;

		Vertex(const SPVertex & _v)
			: x(_v.x)
			, y(_v.y)
			, z(_v.z)
			, w(_v.w)
#ifdef DEBUG_DUMP
			, sx(_v.sx)
			, sy(_v.sy)
			, sz(_v.sz)
#endif
			, r(_v.r)
			, g(_v.g)
			, b(_v.b)
			, a(_v.a)
			, s0(_v.s)
			, t0(_v.t)
			, s1(_v.s)
			, t1(_v.t)
			, modify(_v.modify)
		{}

		Vertex(const RectVertex & _v)
			: x(_v.x)
			, y(_v.y)
			, z(_v.z)
			, w(_v.w)
			, s0(_v.s0)
			, t0(_v.t0)
			, s1(_v.s1)
			, t1(_v.t1)
			, modify(MODIFY_XY | MODIFY_Z)
		{
			r = g = b = a = 0.0f;
		}
	};

	struct TriInfo {
		std::array<Vertex, 3> vertices;
		gDPCombine combine; 	// Combine mode at the time of rendering
		u32 cycle_type;
		gDPInfo::OtherMode otherMode;
		u32	geometryMode;	// geometry mode flags
		u32 frameBufferAddress;
		u32	tri_n;		// Triangle number

		TriangleType type;	// 0-normal, 1-texrect, 2-fillrect

		gSPInfo::Viewport viewport;

		// texture info
		std::array<std::unique_ptr<TexInfo>, 2> tex_info;

		// colors
		gDPInfo::Color fog_color;
		gDPInfo::Color blend_color;
		gDPInfo::Color env_color;
		gDPInfo::FillColor fill_color;
		gDPInfo::PrimColor prim_color;
		f32 primDepthZ, primDepthDeltaZ;
		f32 fogMultiplier, fogOffset;
		s32 K4, K5;

		f32 getScreenX(const Vertex & _v) const;
		f32 getScreenY(const Vertex & _v) const;
		f32 getScreenZ(const Vertex & _v) const;
		f32 getModelX(const Vertex & _v) const;
		f32 getModelY(const Vertex & _v) const;
		f32 getModelZ(const Vertex & _v) const;

		bool isInside(long x, long y) const;
	};

	typedef struct {
		f32 scene_x, scene_y, scene_z;
		f32 r, g, b, a;
		f32 t0_s, t0_t;
		f32 t1_s, t1_t;
	} RipVertex;

	typedef struct {
		RipVertex vertices[3];
		u32 __PAD0 = 0;
		f32 prim_r, prim_g, prim_b, prim_a;
		f32 env_r, env_g, env_b, env_a;
		f32 blend_r, blend_g, blend_b, blend_a;
		u64 t0_g64Crc;
		u64 t1_g64Crc;
		u8 t0_wrapmode, t1_wrapmode;
		u16 __PAD1 = 0;
		u32 __PAD2 = 0;
	} RipTriangle;

	typedef struct
	{
		const u8 MAGIC[6] = { 'G', 'L', '6', '4', 'R', '\0'};
		const u16 VERSION = 1;
		char romName[20] = {0}; // no null terminator
		u32 num_triangles = 0;
		f32 fog_r, fog_g, fog_b;
	} RipHeader;
	
	enum class Page {
		general,
		tex1,
		tex2,
		colors,
		blender,
		othermode_l,
		othermode_h,
		texcoords,
		coords,
		texinfo
	};

	enum class TextureMode {
		texture,
		alpha,
		both
	};

	void _fillTriInfo(TriInfo & _info);
	void _addTriangles(const graphics::Context::DrawTriangleParameters & _params);
	void _addTrianglesByElements(const graphics::Context::DrawTriangleParameters & _params);
	void _debugKeys();
	void _drawFrameBuffer(FrameBuffer * _pBuffer);
	void _drawDebugInfo();
	void _setTextureCombiner();
	void _setLineCombiner();
	void _drawTextureFrame(const RectVertex * _rect);
	void _drawTextureCache();

	void _drawGeneral(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawTex(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawColors(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawBlender(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawOthermodeL(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawOthermodeH(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawTexCoords(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawVertexCoords(f32 _ulx, f32 _uly, f32 _yShift);
	void _drawTexture(f32 _ulx, f32 _uly, f32 _lrx, f32 _lry, f32 _yShift);
	void _drawTriangleFrame();
	void _drawMouseCursor();
	void _findSelected();

	s32 _performSceneRip();

	typedef std::list<TriInfo> Triangles;
	typedef std::list<const TexInfo*> TexInfos;
	typedef std::set<u32> FrameBufferAddrs;

	Triangles m_triangles;
	Triangles::const_iterator m_triSel;
	const TexInfo * m_pCurTexInfo = nullptr;
	TextureMode m_textureMode = TextureMode::both;

	FrameBufferAddrs m_fbAddrs;
	FrameBufferAddrs::const_iterator m_curFBAddr;

	Page m_curPage = Page::general;
	bool m_bDebugMode = false;
	bool m_bCapture = false;
	bool m_bRip = false;

	long m_clickX = 0;
	long m_clickY = 0;

	u32 m_rippedFrames = 0;

	u32 m_tmu = 0;
	u32 m_startTexRow[2];
	TexInfos m_texturesToDisplay[2];
	struct {
		u32 row, col;
	} m_selectedTexPos[2];

	const u32 m_cacheViewerRows = 4;
	const u32 m_cacheViewerCols = 16;
};

extern Debugger g_debugger;

#endif // DEBUGGER_H
