#include <algorithm>
#include <thread>
#include <chrono>
#include <Revision.h>
#include "assert.h"
#include "math.h"
#include "Platform.h"
#include "GLideN64.h"
#include "RSP.h"
#include "Config.h"
#include "Combiner.h"
#include "FrameBuffer.h"
#include "DisplayLoadProgress.h"
#include "DisplayWindow.h"
#include "TextDrawer.h"
#include "DebugDump.h"
#include "Debugger.h"
#include "osal_keys.h"
#include <osal_files.h>
#include "TxFilterExport.h"
#include "PluginAPI.h"
#include "wst.h"
#include <filesystem>
#ifndef MUPENPLUSAPI
#include "windows/GLideN64_windows.h"
#endif

Debugger g_debugger;

using namespace std;
using namespace graphics;

#ifdef DEBUG_DUMP

static
bool getCursorPos(long & _x, long & _y)
{
#ifdef OS_WINDOWS
	POINT pt;
	const bool res = GetCursorPos(&pt) == TRUE;
#ifndef MUPENPLUSAPI
	ScreenToClient(hWnd, &pt);
#else
	static HWND hWnd = NULL;
	if (hWnd == NULL) {
		wchar_t caption[64];
# ifdef _DEBUG
		swprintf(caption, 64, L"mupen64plus: %ls debug. Revision %ls", pluginNameW, PLUGIN_REVISION_W);
# else // _DEBUG
		swprintf(caption, 64, L"%s. Revision %s", pluginName, PLUGIN_REVISION);
# endif // _DEBUG
		hWnd = FindWindowExW(NULL, NULL, NULL, caption);
	}
	ScreenToClient(hWnd, &pt);
#endif // MUPENPLUSAPI
	_x = pt.x;
	_y = pt.y;
	return res;
#else // OS_WINDOWS
	_x = _y = 0;
	return false;
#endif
}

f32 Debugger::TriInfo::getScreenX(const Debugger::Vertex & _v) const
{
	if ((_v.modify & MODIFY_XY) != 0)
		return _v.x;
	return _v.x / _v.w * viewport.vscale[0] + viewport.vtrans[0];
}

f32 Debugger::TriInfo::getScreenY(const Debugger::Vertex & _v) const
{
	if ((_v.modify & MODIFY_XY) != 0)
		return _v.y;
	return -_v.y / _v.w * viewport.vscale[1] + viewport.vtrans[1];
}

f32 Debugger::TriInfo::getScreenZ(const Debugger::Vertex & _v) const
{
	if ((_v.modify & MODIFY_Z) != 0)
		return _v.z;
	return (_v.z / _v.w * viewport.vscale[2] + viewport.vtrans[2]);
}

f32 Debugger::TriInfo::getModelX(const Debugger::Vertex & _v) const
{
	if ((_v.modify & MODIFY_XY) == 0)
		return _v.x;

	f32 scaleX, scaleY;
	calcCoordsScales(frameBufferList().findBuffer(frameBufferAddress), scaleX, scaleY);
	return (2.0f * _v.x * scaleX - 1.0f) * _v.w;
}

f32 Debugger::TriInfo::getModelY(const Debugger::Vertex & _v) const
{
	if ((_v.modify & MODIFY_XY) == 0)
		return _v.y;

	f32 scaleX, scaleY;
	calcCoordsScales(frameBufferList().findBuffer(frameBufferAddress), scaleX, scaleY);
	return (-2.0f * _v.y * scaleY + 1.0f) * _v.w;
}

f32 Debugger::TriInfo::getModelZ(const Debugger::Vertex & _v) const
{
	if ((_v.modify & MODIFY_Z) == 0)
		return _v.z;

	return _v.z * _v.w;
}


bool Debugger::TriInfo::isInside(long x, long y) const
{
	if (vertices[0].x == vertices[1].x && vertices[0].y == vertices[1].y)
		return false;

	u32 i, j;

	for (i = 0; i < vertices.size(); i++) {
		j = i + 1;
		if (j == vertices.size())
			j = 0;

		if ((y - getScreenY(vertices[i]))*(getScreenX(vertices[j]) - getScreenX(vertices[i])) -
			(x - getScreenX(vertices[i]))*(getScreenY(vertices[j]) - getScreenY(vertices[i])) < 0)
			break;    // It's outside
	}

	if (i == vertices.size()) // all lines passed
		return true;

	for (i = 0; i < vertices.size(); i++)	{
		j = i + 1;
		if (j == vertices.size())
			j = 0;

		if ((y - getScreenY(vertices[i]))*(getScreenX(vertices[j]) - getScreenX(vertices[i])) -
			(x - getScreenX(vertices[i]))*(getScreenY(vertices[j]) - getScreenY(vertices[i])) > 0)
			break;    // It's outside
	}

	return i == vertices.size(); // all lines passed
}

Debugger::Debugger()
{
	m_triSel = m_triangles.end();
	m_startTexRow[0] = m_startTexRow[1] = 0;
	for (u32 i = 0; i < 2; ++i) {
		m_selectedTexPos[i].col = 0;
		m_selectedTexPos[i].row = 0;
	}
}

Debugger::~Debugger()
{
}

void Debugger::checkDebugState()
{
	if (osal_is_key_pressed(KEY_ScrollLock, 0x0001))
		m_bDebugMode = !m_bDebugMode;

	if (m_bDebugMode && osal_is_key_pressed(KEY_Insert, 0x0001))
		m_bCapture = true;
}

void Debugger::_debugKeys()
{
	if (osal_is_key_pressed(KEY_Right, 0x0001)) {
		if (std::next(m_triSel) != m_triangles.cend())
			++m_triSel;
		else
			m_triSel = m_triangles.cbegin();
	}

	if (osal_is_key_pressed(KEY_Left, 0x0001)) {
		if (m_triSel != m_triangles.cbegin())
			--m_triSel;
		else
			m_triSel = std::prev(m_triangles.cend());
	}

	if (osal_is_key_pressed(KEY_F, 0x0001)) {
		if (m_pCurTexInfo != nullptr) {
			auto curTexName = m_pCurTexInfo->texture->name;
			auto beginItr =
				(std::next(m_triSel) != m_triangles.cend() &&
				((m_triSel->tex_info[0] && m_triSel->tex_info[0]->texture->name == curTexName) ||
				(m_triSel->tex_info[1] && m_triSel->tex_info[1]->texture->name == curTexName))) ?
				std::next(m_triSel) :
				m_triangles.cbegin();
			auto predicate = [curTexName](const Triangles::value_type & val) {
				if (val.tex_info[0].operator bool() && val.tex_info[0]->texture->name == curTexName)
					return true;
				if (val.tex_info[1].operator bool() && val.tex_info[1]->texture->name == curTexName)
					return true;
				return false;
			};
			auto iter = std::find_if(beginItr, m_triangles.cend(), predicate);
			if (iter == m_triangles.cend() && beginItr != m_triangles.cbegin())
				iter = std::find_if(m_triangles.cbegin(), beginItr, predicate);
			if (iter != m_triangles.cend())
				m_triSel = iter;
		}
	}

	if (osal_is_key_pressed(KEY_B, 0x0001)) {
		if (std::next(m_curFBAddr) != m_fbAddrs.end())
			++m_curFBAddr;
		else
			m_curFBAddr = m_fbAddrs.begin();
	}

	if (osal_is_key_pressed(KEY_V, 0x0001)) {
		if (m_curFBAddr != m_fbAddrs.begin())
			--m_curFBAddr;
		else
			m_curFBAddr = std::prev(m_fbAddrs.end());
	}

	if(osal_is_key_pressed(KEY_Home, 0x0001))
	{
		if (config.sceneRipper.enableRipping != 0 &&
			config.textureFilter.txHiresEnable != 0 &&
			textureCache().getDmpTxStatus() &&
			config.textureFilter.txEnhancementMode == 0 &&
			config.frameBufferEmulation.enable != 0) {
			performSceneRip();
		}
	}

	if (osal_is_key_pressed(KEY_Q, 0x0001))
		m_tmu = 0;
	if (osal_is_key_pressed(KEY_W, 0x0001))
		m_tmu = 1;

	if (osal_is_key_pressed(KEY_A, 0x0001))
		m_textureMode = TextureMode::both;  // texture & texture alpha
	if (osal_is_key_pressed(KEY_S, 0x0001))
		m_textureMode = TextureMode::texture;  // texture
	if (osal_is_key_pressed(KEY_D, 0x0001))
		m_textureMode = TextureMode::alpha;  // texture alpha

	if (osal_is_key_pressed(KEY_1, 0x0001))
		m_curPage = Page::general;
	if (osal_is_key_pressed(KEY_2, 0x0001))
		m_curPage = Page::tex1;
	if (osal_is_key_pressed(KEY_3, 0x0001))
		m_curPage = Page::tex2;
	if (osal_is_key_pressed(KEY_4, 0x0001))
		m_curPage = Page::colors;
	if (osal_is_key_pressed(KEY_5, 0x0001))
		m_curPage = Page::blender;
	if (osal_is_key_pressed(KEY_6, 0x0001))
		m_curPage = Page::othermode_l;
	if (osal_is_key_pressed(KEY_7, 0x0001))
		m_curPage = Page::othermode_h;
	if (osal_is_key_pressed(KEY_8, 0x0001))
		m_curPage = Page::texcoords;
	if (osal_is_key_pressed(KEY_9, 0x0001))
		m_curPage = Page::coords;
	if (osal_is_key_pressed(KEY_0, 0x0001))
		m_curPage = Page::texinfo;
}

void Debugger::_fillTriInfo(TriInfo & _info)
{
	_info.cycle_type = gDP.otherMode.cycleType;
	_info.combine = gDP.combine;
	_info.otherMode = gDP.otherMode;
	_info.geometryMode = gSP.geometryMode;
	_info.fog_color = gDP.fogColor;
	_info.fill_color = gDP.fillColor;
	_info.blend_color = gDP.blendColor;
	_info.env_color = gDP.envColor;
	_info.prim_color = gDP.primColor;
	_info.primDepthZ = gDP.primDepth.z;
	_info.primDepthDeltaZ = gDP.primDepth.deltaZ;
	_info.fogMultiplier = gSP.fog.multiplierf;
	_info.fogOffset = gSP.fog.offsetf;
	_info.K4 = gDP.convert.k4;
	_info.K5 = gDP.convert.k5;
	_info.viewport = gSP.viewport;
	_info.frameBufferAddress = gDP.colorImage.address;

	if (currentCombiner()->usesTexture()) {
		TextureCache& cache = TextureCache::get();
		for (u32 i = 0; i < 2; ++i) {
			if (cache.current[i] == nullptr)
				continue;
			TexInfo * pInfo = new TexInfo;
			pInfo->scales = gSP.texture.scales;
			pInfo->scalet = gSP.texture.scalet;
			pInfo->texture = cache.current[i];
			pInfo->texLoadInfo = gDP.loadInfo[gSP.textureTile[i]->tmem];
			_info.tex_info[i].reset(pInfo);
		}
	}
}

void Debugger::_addTrianglesByElements(const Context::DrawTriangleParameters & _params)
{
	u16 * elements = reinterpret_cast<u16*>(_params.elements);
	u32 cur_tri = static_cast<u32>(m_triangles.size());
	for (u32 i = 0; i < _params.elementsCount;) {
		m_triangles.emplace_back();
		TriInfo & info = m_triangles.back();
		for (u32 j = 0; j < 3; ++j)
			info.vertices[j] = Vertex(_params.vertices[elements[i++]]);
		info.tri_n = cur_tri++;
		info.type = ttTriangle;
		_fillTriInfo(info);
	}
}

void Debugger::_addTriangles(const Context::DrawTriangleParameters & _params)
{
	u32 cur_tri = static_cast<u32>(m_triangles.size());
	for (u32 i = 0; i < _params.verticesCount;) {
		m_triangles.emplace_back();
		TriInfo & info = m_triangles.back();
		if (_params.mode == drawmode::TRIANGLES) {
			for (u32 j = 0; j < 3; ++j)
				info.vertices[j] = Vertex(_params.vertices[i++]);
		} else {
			assert(_params.mode == drawmode::TRIANGLE_STRIP);
			for (u32 j = 0; j < 3; ++j)
				info.vertices[j] = Vertex(_params.vertices[i + j]);
			++i;
		}
		info.tri_n = cur_tri++;
		info.type = ttTriangle;
		_fillTriInfo(info);
		if (i + 3 > _params.verticesCount)
			return;
	}
}

void Debugger::addTriangles(const graphics::Context::DrawTriangleParameters & _params)
{
	if (!m_bCapture)
		return;

	if (_params.elements != nullptr) {
		_addTrianglesByElements(_params);
	} else {
		_addTriangles(_params);
	}
}

void Debugger::addRects(const graphics::Context::DrawRectParameters & _params)
{
	if (!m_bCapture)
		return;

	u32 cur_tri = static_cast<u32>(m_triangles.size());
	for (u32 i = 0; i < 2; ++i) {
		m_triangles.emplace_back();
		TriInfo & info = m_triangles.back();
		for (u32 j = 0; j < 3; ++j)
			info.vertices[j] = Vertex(_params.vertices[i+j]);
		info.tri_n = cur_tri++;
		info.type = _params.texrect ? ttTexrect : ttFillrect;
		_fillTriInfo(info);
	}
}

void Debugger::_setTextureCombiner()
{
	gDP.otherMode.cycleType = G_CYC_1CYCLE;
	CombinerInfo & cmbInfo = CombinerInfo::get();
	cmbInfo.setPolygonMode(DrawingState::TexRect);
	switch (m_textureMode) {
	case TextureMode::texture:
		cmbInfo.setCombine(EncodeCombineMode(0, 0, 0, TEXEL0, 0, 0, 0, 1, 0, 0, 0, TEXEL0, 0, 0, 0, 1));
		break;
	case TextureMode::alpha:
		cmbInfo.setCombine(EncodeCombineMode(0, 0, 0, 1, 0, 0, 0, TEXEL0, 0, 0, 0, 1, 0, 0, 0, TEXEL0));
		break;
	case TextureMode::both:
		cmbInfo.setCombine(EncodeCombineMode(0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0));
		break;
	}
	cmbInfo.getCurrent()->update(false);
}

void Debugger::_setLineCombiner()
{
	gDP.otherMode.cycleType = G_CYC_1CYCLE;
	gDP.envColor.r = gDP.envColor.g = 0;
	gDP.envColor.b = gDP.envColor.a = 1;
	CombinerInfo & cmbInfo = CombinerInfo::get();
	cmbInfo.setPolygonMode(DrawingState::Triangle);
	cmbInfo.setCombine(EncodeCombineMode(0, 0, 0, ENVIRONMENT, 0, 0, 0, ENVIRONMENT, 0, 0, 0, ENVIRONMENT, 0, 0, 0, ENVIRONMENT));
	cmbInfo.getCurrent()->update(true);
}

void Debugger::_drawTextureFrame(const RectVertex * _rect)
{
	//Draw lines for selected texture
	_setLineCombiner();
	const f32 lineWidth = 1.5f;
	SPVertex vertexBuf[2];
	memset(vertexBuf, 0, sizeof(vertexBuf));
	SPVertex & v0 = vertexBuf[0];
	v0.x = _rect[0].x;
	v0.y = -_rect[0].y;
	v0.z = _rect[0].z;
	v0.w = _rect[0].w;
	SPVertex & v1 = vertexBuf[1];
	v1 = v0;
	v1.x = _rect[1].x;
	gfxContext.drawLine(lineWidth, vertexBuf);
	v1.x = _rect[0].x;
	v1.y = -_rect[2].y;
	gfxContext.drawLine(lineWidth, vertexBuf);
	v0.x = _rect[1].x;
	v1.x = _rect[1].x;
	gfxContext.drawLine(lineWidth, vertexBuf);
	v0.x = _rect[0].x;
	v0.y = -_rect[2].y;
	gfxContext.drawLine(lineWidth, vertexBuf);

	_setTextureCombiner();
}

void Debugger::_drawTriangleFrame()
{
	if (m_triSel == m_triangles.end())
		return;

	FrameBuffer * pBuffer = frameBufferList().findBuffer(m_triSel->frameBufferAddress);
	if (pBuffer == nullptr)
		return;

	DisplayWindow & wnd = dwnd();

	const s32 hOffset = (wnd.getScreenWidth() - wnd.getWidth()) / 2;
	const s32 vOffset = (wnd.getScreenHeight() - wnd.getHeight()) / 2 + wnd.getHeightOffset();

	const u32 areaWidth = wnd.getWidth() * 5 / 8;
	const u32 areaHeight = wnd.getHeight() * 5 / 8;

	gfxContext.setViewport(hOffset, vOffset + wnd.getHeight() * 3 / 8, areaWidth, areaHeight);

	const Debugger::Vertex * vertices = m_triSel->vertices.data();
	//Draw lines for selected triangle
	_setLineCombiner();
	const f32 lineWidth = 1.5f;
	SPVertex vertexBuf[2];
	memset(vertexBuf, 0, sizeof(vertexBuf));
	SPVertex & v0 = vertexBuf[0];
	v0.x = m_triSel->getModelX(vertices[0]);
	v0.y = -m_triSel->getModelY(vertices[0]);
	v0.z = m_triSel->getModelZ(vertices[0]);
	v0.w = vertices[0].w;
	SPVertex & v1 = vertexBuf[1];
	v1.x = m_triSel->getModelX(vertices[1]);
	v1.y = -m_triSel->getModelY(vertices[1]);
	v1.z = m_triSel->getModelZ(vertices[1]);
	v1.w = vertices[1].w;
	gfxContext.drawLine(lineWidth, vertexBuf);
	v0.x = m_triSel->getModelX(vertices[2]);
	v0.y = -m_triSel->getModelY(vertices[2]);
	v0.z = m_triSel->getModelZ(vertices[2]);
	v0.w = vertices[2].w;
	gfxContext.drawLine(lineWidth, vertexBuf);
	v1.x = m_triSel->getModelX(vertices[0]);
	v1.y = -m_triSel->getModelY(vertices[0]);
	v1.z = m_triSel->getModelZ(vertices[0]);
	v1.w = vertices[0].w;
	gfxContext.drawLine(lineWidth, vertexBuf);

	_setTextureCombiner();
}

void Debugger::_drawTextureCache()
{
	DisplayWindow & wnd = dwnd();
	TextureCache & cache = textureCache();

	const s32 hOffset = (wnd.getScreenWidth() - wnd.getWidth()) / 2;
	const s32 vOffset = (wnd.getScreenHeight() - wnd.getHeight()) / 2 + wnd.getHeightOffset();
	const u32 areaHeight = wnd.getHeight() * 3 / 8;

	gfxContext.setViewport(hOffset, vOffset, wnd.getWidth(), areaHeight);

	gfxContext.enable(enable::CULL_FACE, false);
	gfxContext.enable(enable::BLEND, m_textureMode != TextureMode::texture);
	gfxContext.enable(enable::DEPTH_TEST, false);
	gfxContext.enableDepthWrite(false);
	gfxContext.setDepthCompare(compare::ALWAYS);
	gfxContext.enable(enable::SCISSOR_TEST, false);
	_setTextureCombiner();

	gSP.changed |= CHANGED_GEOMETRYMODE | CHANGED_VIEWPORT;
	gDP.changed |= CHANGED_RENDERMODE | CHANGED_TILE | CHANGED_COMBINE;

	const f32 rectWidth = 2.0f / m_cacheViewerCols;
	const f32 rectHeight = 2.0f / m_cacheViewerRows;
	const f32 Z = 0.0f;
	const f32 W = 1.0f;

	if (m_clickY >= (long)(wnd.getHeight() * 5 / 8)) {
		long y = m_clickY - wnd.getHeight() * 5 / 8;
		m_selectedTexPos[m_tmu].row = y * m_cacheViewerRows/ areaHeight;
		m_selectedTexPos[m_tmu].col = m_clickX * m_cacheViewerCols/ wnd.getWidth();
	}

	f32 X = -1.0f;
	f32 Y = 1.0f;
	RectVertex rect[4];
	RectVertex rectSelected[4];
	struct Framer {
		Framer(Debugger * _d, RectVertex * _r) : d(_d), r(_r) {}
		~Framer() {
			d->_drawTextureFrame(r);
		}
		Debugger * d;
		RectVertex * r;
	} framer(this, rectSelected);

	TexInfos & texInfos = m_texturesToDisplay[m_tmu];
	if (texInfos.empty()) {
		std::set<graphics::ObjectHandle> displayedTextures;
		for (auto& t : m_triangles) {
			if (!t.tex_info[m_tmu])
				continue;
			auto res = displayedTextures.insert(t.tex_info[m_tmu]->texture->name);
			if(res.second)
				texInfos.push_back(t.tex_info[m_tmu].get());
		}
	}

	if (osal_is_key_pressed(KEY_Up, 0x0001)) {
		if ((m_startTexRow[m_tmu] + 1) * m_cacheViewerCols < texInfos.size())
			m_startTexRow[m_tmu]++;
	}

	if (osal_is_key_pressed(KEY_Down, 0x0001)) {
		if (m_startTexRow[m_tmu] > 0)
			--m_startTexRow[m_tmu];
	}

	if (osal_is_key_pressed(KEY_Space, 0x0001)) {
		if (m_triSel->tex_info[m_tmu]) {
			graphics::ObjectHandle tex = m_triSel->tex_info[m_tmu]->texture->name;
			auto iter = std::find_if(texInfos.begin(),
									 texInfos.end(),
									 [tex](const TexInfos::value_type & val) {
										return val->texture->name == tex;
									 });
			auto d = std::distance(texInfos.begin(), iter);
			m_startTexRow[m_tmu] = static_cast<u32>(d) / m_cacheViewerCols;
			m_clickY = 0;
			m_selectedTexPos[m_tmu].row = 0;
			m_selectedTexPos[m_tmu].col = d % m_cacheViewerCols;
		}
	}

	if (texInfos.empty())
		return;
	auto infoIter = texInfos.begin();
	std::advance(infoIter, m_startTexRow[m_tmu] * m_cacheViewerCols);

	for (u32 r = 0; r < m_cacheViewerRows; ++r) {

		for (u32 c = 0; c < m_cacheViewerCols; ++c) {
			rect[0].x = X;
			rect[0].y = Y;
			rect[0].z = Z;
			rect[0].w = W;
			rect[1].x = X + rectWidth;
			rect[1].y = rect[0].y;
			rect[1].z = Z;
			rect[1].w = W;
			rect[2].x = rect[0].x;
			rect[2].y = Y - rectHeight;
			rect[2].z = Z;
			rect[2].w = W;
			rect[3].x = rect[1].x;
			rect[3].y = rect[2].y;
			rect[3].z = Z;
			rect[3].w = W;

			rect[0].s0 = 0;
			rect[0].t0 = 0;
			rect[1].s0 = (*infoIter)->texture->width;
			rect[1].t0 = 0;
			rect[2].s0 = 0;
			rect[2].t0 = (*infoIter)->texture->height;
			rect[3].s0 = rect[1].s0;
			rect[3].t0 = rect[2].t0;

			if (r == m_selectedTexPos[m_tmu].row && c == m_selectedTexPos[m_tmu].col) {
				memcpy(rectSelected, rect, sizeof(rect));
				m_pCurTexInfo = *infoIter;
			}
			Context::TexParameters texParams;
			texParams.handle = (*infoIter)->texture->name;
			texParams.target = textureTarget::TEXTURE_2D;
			texParams.textureUnitIndex = textureIndices::Tex[0];
			texParams.minFilter = textureParameters::FILTER_NEAREST;
			texParams.magFilter = textureParameters::FILTER_NEAREST;
			texParams.wrapS = textureParameters::WRAP_CLAMP_TO_EDGE;
			texParams.wrapT = textureParameters::WRAP_CLAMP_TO_EDGE;
			gfxContext.setTextureParameters(texParams);

			cache.current[0] = const_cast<CachedTexture*>((*infoIter)->texture);
			Context::DrawRectParameters rectParams;
			rectParams.mode = drawmode::TRIANGLE_STRIP;
			rectParams.verticesCount = 4;
			rectParams.vertices = rect;
			rectParams.combiner = currentCombiner();
			currentCombiner()->update(false);
			gfxContext.drawRects(rectParams);

			X += rectWidth;
			++infoIter;
			if (infoIter == texInfos.end())
				return;
		}

		X = -1.0f;
		Y -= rectHeight;
	}

	gfxContext.enable(enable::SCISSOR_TEST, true);
}

void Debugger::_drawFrameBuffer(FrameBuffer * _pBuffer)
{
	DisplayWindow & wnd = dwnd();
	GraphicsDrawer & drawer = wnd.getDrawer();
	CachedTexture * pBufferTexture = _pBuffer->m_pTexture;
	ObjectHandle readBuffer = _pBuffer->m_FBO;

	if (pBufferTexture->frameBufferTexture == CachedTexture::fbMultiSample) {
		_pBuffer->resolveMultisampledTexture(true);
		readBuffer = _pBuffer->m_resolveFBO;
		pBufferTexture = _pBuffer->m_pResolveTexture;
	}

	s32 srcCoord[4] = { 0, 0, pBufferTexture->width, (s32)(_pBuffer->m_height * _pBuffer->m_scale) };
	const s32 hOffset = (wnd.getScreenWidth() - wnd.getWidth()) / 2;
	const s32 vOffset = (wnd.getScreenHeight() - wnd.getHeight()) / 2 + wnd.getHeightOffset() + wnd.getHeight()*3/8;
	s32 dstCoord[4] = { hOffset, vOffset, hOffset + (s32)wnd.getWidth()*5/8, vOffset + (s32)wnd.getHeight()*5/8 };

	gfxContext.bindFramebuffer(bufferTarget::DRAW_FRAMEBUFFER, ObjectHandle::defaultFramebuffer);

	float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	drawer.clearColorBuffer(clearColor);

	GraphicsDrawer::BlitOrCopyRectParams blitParams;
	blitParams.srcX0 = srcCoord[0];
	blitParams.srcY0 = srcCoord[3];
	blitParams.srcX1 = srcCoord[2];
	blitParams.srcY1 = srcCoord[1];
	blitParams.srcWidth = pBufferTexture->width;
	blitParams.srcHeight = pBufferTexture->height;
	blitParams.dstX0 = dstCoord[0];
	blitParams.dstY0 = dstCoord[1];
	blitParams.dstX1 = dstCoord[2];
	blitParams.dstY1 = dstCoord[3];
	blitParams.dstWidth = wnd.getScreenWidth();
	blitParams.dstHeight = wnd.getScreenHeight() + wnd.getHeightOffset();
	const bool downscale = blitParams.srcWidth >= blitParams.dstWidth || blitParams.srcHeight >= blitParams.dstHeight;
	blitParams.filter = downscale || config.generalEmulation.enableHybridFilter > 0 ?
		textureParameters::FILTER_LINEAR :
		textureParameters::FILTER_NEAREST; //upscale; hybridFilter disabled
	blitParams.mask = blitMask::COLOR_BUFFER;
	blitParams.tex[0] = pBufferTexture;
	blitParams.combiner = downscale ? CombinerInfo::get().getTexrectDownscaleCopyProgram() :
		CombinerInfo::get().getTexrectUpscaleCopyProgram();
	blitParams.readBuffer = readBuffer;

	drawer.blitOrCopyTexturedRect(blitParams);
}

inline
void setTextColor(u32 _c)
{
	float color[4] = { _SHIFTR(_c, 24, 8) / 255.0f, _SHIFTR(_c, 16, 8) / 255.0f,
		_SHIFTR(_c, 8, 8) / 255.0f, _SHIFTR(_c, 0, 8) / 255.0f };
	g_textDrawer.setTextColor(color);
}

#define COL_CATEGORY()	setTextColor(0xD288F4FF)
#define COL_CC()		setTextColor(0x88C3F4FF)
#define COL_AC()		setTextColor(0x3CEE5EFF)
#define COL_TEXT()		setTextColor(0xFFFFFFFF)
#define COL_SEL(x)		setTextColor((x)?0x00FF00FF:0x800000FF)

#define DRAW_TEXT() g_textDrawer.drawText(buf, ulx, uly)

#define OUTPUT1(fmt,other) sprintf(buf, fmt, other); ulx = _ulx; DRAW_TEXT(); uly -= _yShift

#define OUTPUT0(txt) strncpy(buf, txt, sizeof(buf)); ulx = _ulx; DRAW_TEXT(); uly -= _yShift

#define OUTPUT2(fmt, other1, other2) sprintf(buf, fmt, other1, other2); ulx = _ulx; DRAW_TEXT(); uly -= _yShift

#define OUTPUT_COLOR(fmt, r, g, b, a) sprintf(buf, fmt, r, g, b, a); ulx = _ulx; DRAW_TEXT(); uly -= _yShift

#define OUTPUT_(txt,cc) strncpy(buf, txt, sizeof(buf)); ulx = _ulx; COL_SEL(cc); DRAW_TEXT();\
	g_textDrawer.getTextSize(txt, tW, tH); ulx += tW

#define _OUTPUT1(txt,cc) strncpy(buf, txt, sizeof(buf)); COL_SEL(cc); DRAW_TEXT();\
	g_textDrawer.getTextSize(txt, tW, tH); ulx += tW

#define LINE_FEED() uly -= _yShift

void Debugger::_drawGeneral(f32 _ulx, f32 _uly, f32 _yShift)
{
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;

	COL_CATEGORY();
	OUTPUT0("GENERAL (page 1):");
	COL_TEXT();
	OUTPUT1("tri #%d", m_triSel->tri_n);
	OUTPUT1("type: %s", tri_type[(u32)(m_triSel->type)]);
	OUTPUT1("geom:   0x%08x", m_triSel->geometryMode);
	OUTPUT1("othermode_h: 0x%08x", m_triSel->otherMode.h);
	OUTPUT1("othermode_l: 0x%08x", m_triSel->otherMode.l);
	LINE_FEED();
	COL_CATEGORY();
	OUTPUT0("COMBINE:");
	COL_TEXT();
	OUTPUT1("cycle_mode: %s", CycleTypeText[m_triSel->cycle_type]);
	OUTPUT1("muxs0: 0x%08x", m_triSel->combine.muxs0);
	OUTPUT1("muxs1: 0x%08x", m_triSel->combine.muxs1);
	COL_CC();
	OUTPUT1("a0: %s", saRGBText[m_triSel->combine.saRGB0]);
	OUTPUT1("b0: %s", sbRGBText[m_triSel->combine.sbRGB0]);
	OUTPUT1("c0: %s", mRGBText[m_triSel->combine.mRGB0]);
	OUTPUT1("d0: %s", aRGBText[m_triSel->combine.aRGB0]);
	COL_AC();
	OUTPUT1("Aa0: %s", saAText[m_triSel->combine.saA0]);
	OUTPUT1("Ab0: %s", sbAText[m_triSel->combine.sbA0]);
	OUTPUT1("Ac0: %s", mAText[m_triSel->combine.mA0]);
	OUTPUT1("Ad0: %s", aAText[m_triSel->combine.aA0]);
	COL_CC();
	OUTPUT1("a1: %s", saRGBText[m_triSel->combine.saRGB1]);
	OUTPUT1("b1: %s", sbRGBText[m_triSel->combine.sbRGB1]);
	OUTPUT1("c1: %s", mRGBText[m_triSel->combine.mRGB1]);
	OUTPUT1("d1: %s", aRGBText[m_triSel->combine.aRGB1]);
	COL_AC();
	OUTPUT1("Aa1: %s", saAText[m_triSel->combine.saA1]);
	OUTPUT1("Ab1: %s", sbAText[m_triSel->combine.sbA1]);
	OUTPUT1("Ac1: %s", mAText[m_triSel->combine.mA1]);
	OUTPUT1("Ad1: %s", aAText[m_triSel->combine.aA1]);
}

void Debugger::_drawTex(f32 _ulx, f32 _uly, f32 _yShift)
{
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;

	const u32 tex = m_curPage == Page::tex1 ? 0 : 1;
	COL_CATEGORY();
	OUTPUT2("TEXTURE %d (page %d):", tex, tex + 2);
	COL_TEXT();
	if (!m_triSel->tex_info[tex]) {
		OUTPUT0("NOT USED");
		return;
	}
	const CachedTexture * texture = m_triSel->tex_info[tex]->texture;
	const gDPLoadTileInfo & texLoadInfo = m_triSel->tex_info[tex]->texLoadInfo;
	OUTPUT1("CRC: 0x%llx", texture->crc);
	OUTPUT1("riceCrc: 0x%llx", texture->riceCrc);
	OUTPUT1("tex_size: %s", ImageSizeText[texture->size]);
	OUTPUT1("tex_format: %s", ImageFormatText[texture->format]);
	OUTPUT1("width: %d", texture->width);
	OUTPUT1("height: %d", texture->height);
	OUTPUT1("palette: %d", texture->palette);
	OUTPUT1("clamp_s: %d", texture->clampS);
	OUTPUT1("clamp_t: %d", texture->clampT);
	OUTPUT1("mirror_s: %d", texture->mirrorS);
	OUTPUT1("mirror_t: %d", texture->mirrorT);
	OUTPUT1("mask_s: %d", texture->maskS);
	OUTPUT1("mask_t: %d", texture->maskT);
	OUTPUT1("offset_s: %.2f", texture->offsetS);
	OUTPUT1("offset_t: %.2f", texture->offsetT);
	OUTPUT1("ul_s: %d", texLoadInfo.uls);
	OUTPUT1("ul_t: %d", texLoadInfo.ult);
	OUTPUT1("lr_s: %d", texLoadInfo.lrs);
	OUTPUT1("lr_t: %d", texLoadInfo.lrt);
	OUTPUT1("scale_s: %f", texture->scaleS);
	OUTPUT1("scale_t: %f", texture->scaleT);
	OUTPUT1("s_mode: %s", tex_type[((texture->clampS << 1) | texture->mirrorS) & 3]);
	OUTPUT1("t_mode: %s", tex_type[((texture->clampT << 1) | texture->mirrorT) & 3]);
}

void Debugger::_drawColors(f32 _ulx, f32 _uly, f32 _yShift)
{
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;

	COL_CATEGORY();
	OUTPUT0("COLORS (page 4)");
	COL_TEXT();
	const gDPInfo::PrimColor & prim = m_triSel->prim_color;
	OUTPUT_COLOR("prim:  r %.2f g %.2f b %.2f a %.2f", prim.r, prim.g, prim.b, prim.a);
	const gDPInfo::Color & env = m_triSel->env_color;
	OUTPUT_COLOR("env:   r %.2f g %.2f b %.2f a %.2f", env.r, env.g, env.b, env.a);
	const gDPInfo::Color & fog = m_triSel->fog_color;
	OUTPUT_COLOR("fog:   r %.2f g %.2f b %.2f a %.2f", fog.r, fog.g, fog.b, fog.a);
	const gDPInfo::Color & blend = m_triSel->blend_color;
	OUTPUT_COLOR("blend: r %.2f g %.2f b %.2f a %.2f", blend.r, blend.g, blend.b, blend.a);
	OUTPUT1("K4:  %02x", m_triSel->K4);
	OUTPUT1("K5:  %02x", m_triSel->K5);
	OUTPUT1("prim_lodmin:  %.2f", prim.m);
	OUTPUT1("prim_lodfrac: %.2f", prim.l);
	const gDPInfo::FillColor & fill = m_triSel->fill_color;
	OUTPUT1("fill:  %08x", fill.color);
	OUTPUT1("prim_z:  %.2f", m_triSel->primDepthZ);
	OUTPUT1("prim_dz: %.2f", m_triSel->primDepthDeltaZ);
}

void Debugger::_drawBlender(f32 _ulx, f32 _uly, f32 _yShift)
{
	static const char *FBLa[] = { "G_BL_CLR_IN", "G_BL_CLR_MEM", "G_BL_CLR_BL", "G_BL_CLR_FOG" };
	static const char *FBLb[] = { "G_BL_A_IN", "G_BL_A_FOG", "G_BL_A_SHADE", "G_BL_0" };
	static const char *FBLc[] = { "G_BL_CLR_IN", "G_BL_CLR_MEM", "G_BL_CLR_BL", "G_BL_CLR_FOG" };
	static const char *FBLd[] = { "G_BL_1MA", "G_BL_A_MEM", "G_BL_1", "G_BL_0" };

	const gDPInfo::OtherMode & om = m_triSel->otherMode;
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;

	COL_CATEGORY();
	OUTPUT0("BLENDER (page 5)");
	COL_TEXT();
	OUTPUT1("cycle_mode: %s", CycleTypeText[m_triSel->cycle_type]);
	LINE_FEED();
	OUTPUT1("fbl_a0: %s", FBLa[om.c1_m1a]);
	OUTPUT1("fbl_b0: %s", FBLb[om.c1_m1b]);
	OUTPUT1("fbl_c0: %s", FBLc[om.c1_m2a]);
	OUTPUT1("fbl_d0: %s", FBLd[om.c1_m2b]);
	OUTPUT1("fbl_a1: %s", FBLa[om.c2_m1a]);
	OUTPUT1("fbl_b1: %s", FBLb[om.c2_m1b]);
	OUTPUT1("fbl_c1: %s", FBLc[om.c2_m2a]);
	OUTPUT1("fbl_d1: %s", FBLd[om.c2_m2b]);
	LINE_FEED();
	OUTPUT1("fbl:    %08x", om.l & 0xFFFF0000);
	OUTPUT1("fbl #1: %08x", om.l & 0xCCCC0000);
	OUTPUT1("fbl #2: %08x", om.l & 0x33330000);
}

void Debugger::_drawOthermodeL(f32 _ulx, f32 _uly, f32 _yShift)
{
	const u32 othermode_l = m_triSel->otherMode.l;
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;
	float tW, tH;

	COL_CATEGORY();
	OUTPUT1("OTHERMODE_L: %08x (page 6)", othermode_l);
	OUTPUT_("AC_NONE ", (othermode_l & 3) == 0);
	_OUTPUT1("AC_THRESHOLD ", (othermode_l & 3) == 1);
	_OUTPUT1("AC_DITHER", (othermode_l & 3) == 3);
	LINE_FEED();
	OUTPUT_("ZS_PIXEL ", !(othermode_l & 4));
	_OUTPUT1("ZS_PRIM", (othermode_l & 4));
	LINE_FEED();
	LINE_FEED();
	COL_CATEGORY();
	OUTPUT1("RENDERMODE: %08x", othermode_l);
	OUTPUT_("AA_EN", othermode_l & 0x08);
	LINE_FEED();
	OUTPUT_("Z_CMP", othermode_l & 0x10);
	LINE_FEED();
	OUTPUT_("Z_UPD", othermode_l & 0x20);
	LINE_FEED();
	OUTPUT_("IM_RD", othermode_l & 0x40);
	LINE_FEED();
	OUTPUT_("CLR_ON_CVG", othermode_l & 0x80);
	LINE_FEED();
	OUTPUT_("CVG_DST_CLAMP ", (othermode_l & 0x300) == 0x000);
	_OUTPUT1(".._WRAP ", (othermode_l & 0x300) == 0x100);
	_OUTPUT1(".._FULL ", (othermode_l & 0x300) == 0x200);
	_OUTPUT1(".._SAVE", (othermode_l & 0x300) == 0x300);
	LINE_FEED();
	OUTPUT_("ZM_OPA ", (othermode_l & 0xC00) == 0x000);
	_OUTPUT1("ZM_INTER ", (othermode_l & 0xC00) == 0x400);
	_OUTPUT1("ZM_XLU ", (othermode_l & 0xC00) == 0x800);
	_OUTPUT1("ZM_DEC ", (othermode_l & 0xC00) == 0xC00);
	LINE_FEED();
	OUTPUT_("CVG_X_ALPHA", othermode_l & 0x1000);
	LINE_FEED();
	OUTPUT_("ALPHA_CVG_SEL", othermode_l & 0x2000);
	LINE_FEED();
	OUTPUT_("FORCE_BL", othermode_l & 0x4000);
}

void Debugger::_drawOthermodeH(f32 _ulx, f32 _uly, f32 _yShift)
{
	const u32 othermode_h = m_triSel->otherMode.h;
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;
	float tW, tH;

	COL_CATEGORY();
	OUTPUT1("OTHERMODE_H: %08x (page 7)", othermode_h);
	OUTPUT_("CK_NONE", (othermode_h & 0x100) == 0);
	_OUTPUT1("CK_KEY", (othermode_h & 0x100) == 1);
	LINE_FEED();
	OUTPUT_("TC_CONV", (othermode_h & 0xE00) == 0x200);
	_OUTPUT1("TC_FILTCONV", (othermode_h & 0xE00) == 0xA00);
	_OUTPUT1("TC_FILT", (othermode_h & 0xE00) == 0xC00);
	LINE_FEED();
	OUTPUT_("TF_POINT", (othermode_h & 0x3000) == 0x0000);
	_OUTPUT1("TF_AVERAGE", (othermode_h & 0x3000) == 0x3000);
	_OUTPUT1("TF_BILERP", (othermode_h & 0x3000) == 0x2000);
	LINE_FEED();
	OUTPUT_("TT_NONE", (othermode_h & 0xC000) == 0x0000);
	_OUTPUT1("TT_RGBA16", (othermode_h & 0xC000) == 0x8000);
	_OUTPUT1("TT_IA16", (othermode_h & 0xC000) == 0xC000);
	LINE_FEED();
	OUTPUT_("TL_TILE", (othermode_h & 0x10000) == 0x00000);
	_OUTPUT1("TL_LOD", (othermode_h & 0x10000) == 0x10000);
	LINE_FEED();
	OUTPUT_("TD_CLAMP", (othermode_h & 0x60000) == 0x00000);
	_OUTPUT1("TD_SHARPEN", (othermode_h & 0x60000) == 0x20000);
	_OUTPUT1("TD_DETAIL", (othermode_h & 0x60000) == 0x40000);
	LINE_FEED();
	OUTPUT_("TP_NONE", (othermode_h & 0x80000) == 0x00000);
	_OUTPUT1("TP_PERSP", (othermode_h & 0x80000) == 0x80000);
	LINE_FEED();
	OUTPUT_("1CYCLE", (othermode_h & 0x300000) == 0x000000);
	_OUTPUT1("2CYCLE", (othermode_h & 0x300000) == 0x100000);
	_OUTPUT1("COPY", (othermode_h & 0x300000) == 0x200000);
	_OUTPUT1("FILL", (othermode_h & 0x300000) == 0x300000);
	LINE_FEED();
	OUTPUT_("PM_1PRIM", (othermode_h & 0x400000) == 0x000000);
	_OUTPUT1("PM_NPRIM", (othermode_h & 0x400000) == 0x400000);
}

void Debugger::_drawTexCoords(f32 _ulx, f32 _uly, f32 _yShift)
{
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;

	COL_CATEGORY();
	OUTPUT0("TEXCOORDS (page 8)");
	COL_TEXT();
	OUTPUT1("type: %s", tri_type[(u32)(m_triSel->type)]);
	if (m_triSel->type == ttFillrect)
		return;

	LINE_FEED();

	if (m_triSel->type == ttTriangle) {
		for (u32 j = 0; j < m_triSel->vertices.size(); ++j) {
			OUTPUT2("v[%d].s: %f", j, m_triSel->vertices[j].s0);
			OUTPUT2("v[%d].t: %f", j, m_triSel->vertices[j].t0);
		}
		return;
	}

	for (u32 j = 0; j < m_triSel->vertices.size(); ++j) {
		OUTPUT2("v[%d].s0: %f", j, m_triSel->vertices[j].s0);
		OUTPUT2("v[%d].t0: %f", j, m_triSel->vertices[j].t0);
		OUTPUT2("v[%d].s1: %f", j, m_triSel->vertices[j].s1);
		OUTPUT2("v[%d].t1: %f", j, m_triSel->vertices[j].t1);
	}

}

void Debugger::_drawVertexCoords(f32 _ulx, f32 _uly, f32 _yShift)
{
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;

	COL_CATEGORY();
	OUTPUT0("VERTEX (page 9)");
	COL_TEXT();
	OUTPUT1("type: %s", tri_type[(u32)(m_triSel->type)]);
	LINE_FEED();

	if (m_triSel->type == ttTriangle) {
		for (u32 j = 0; j < m_triSel->vertices.size(); ++j) {
			const Vertex & v = m_triSel->vertices[j];
			if (RSP.LLE) {
				OUTPUT2("v[%d].x: %f", j, v.x);
				OUTPUT2("v[%d].y: %f", j, v.y);
				OUTPUT2("v[%d].z: %f", j, v.z);
			}
			else {
				OUTPUT2("v[%d].x: %f", j, m_triSel->getScreenX(v));
				OUTPUT2("v[%d].y: %f", j, m_triSel->getScreenY(v));
				OUTPUT2("v[%d].z: %f", j, m_triSel->getScreenZ(v));
			}
			OUTPUT2("v[%d].w: %f", j, v.w);
			OUTPUT2("v[%d].sx: %f", j, v.sx);
			OUTPUT2("v[%d].sy: %f", j, v.sy);
			OUTPUT2("v[%d].sz: %f", j, v.sz);
			OUTPUT2("v[%d].r: %.2f", j, v.r);
			OUTPUT2("v[%d].g: %.2f", j, v.g);
			OUTPUT2("v[%d].b: %.2f", j, v.b);
			if (RSP.LLE) {
				OUTPUT2("v[%d].a: %.2f", j, v.a);
			}
			else {
				if ((m_triSel->geometryMode & G_FOG) == 0) {
					OUTPUT2("v[%d].a: %.2f", j, v.a);
				}
				else {
					f32 f = v.z / v.w * m_triSel->fogMultiplier + m_triSel->fogOffset;
					if (f < 0.0f)
						f = 0.0f;
					if (f > 1.0f)
						f = 1.0f;
					OUTPUT2("v[%d].a: %.2f", j, f);
				}
			}

		}
		return;
	}

	for (u32 j = 0; j < m_triSel->vertices.size(); ++j) {
		const Vertex & v = m_triSel->vertices[j];
		OUTPUT2("v[%d].x: %f", j, v.x);
		OUTPUT2("v[%d].y: %f", j, v.y);
		OUTPUT2("v[%d].z: %f", j, v.z);
	}
}

void Debugger::_drawTexture(f32 _ulx, f32 _uly, f32 _lrx, f32 _lry, f32 _yShift)
{
	static const char *LoadType[] = { "LOADTYPE_BLOCK", "LOADTYPE_TILE" };
	static const char *FrameBufferType[] = { "NONE", "ONE_SAMPLE", "MULTI_SAMPLE" };
	char buf[256];
	f32 ulx = _ulx;
	f32 uly = _uly;

	COL_CATEGORY();
	OUTPUT0("TEXTURE (page 0)");
	if (m_pCurTexInfo == nullptr)
		return;
	const CachedTexture * pTexture = m_pCurTexInfo->texture;
	textureCache().current[0] = const_cast<CachedTexture *>(pTexture);
	const gDPLoadTileInfo & texLoadInfo = m_pCurTexInfo->texLoadInfo;

	COL_TEXT();
	OUTPUT1("addr: %08x", texLoadInfo.texAddress);
	OUTPUT1("scale_s: %f", m_pCurTexInfo->scales);
	OUTPUT1("scale_t: %f", m_pCurTexInfo->scalet);
	OUTPUT1("load: %s", LoadType[texLoadInfo.loadType&1]);
	OUTPUT1("t_mem: %04x", pTexture->tMem);
	OUTPUT1("framebuffer: %s", FrameBufferType[(u32)pTexture->frameBufferTexture]);
	OUTPUT1("crc: %llx", pTexture->crc);
	OUTPUT1("riceCrc: %llx", pTexture->riceCrc);
	//	OUTPUT1("texrecting: %d", cache[_debugger.tex_sel].texrecting);
	OUTPUT1("tex_size: %s", ImageSizeText[pTexture->size]);
	OUTPUT1("tex_format: %s", ImageFormatText[pTexture->format]);
	OUTPUT1("width: %d", pTexture->width);
	OUTPUT1("height: %d", pTexture->height);
	OUTPUT1("palette: %d", pTexture->palette);
	OUTPUT1("line: %d", pTexture->line);
	OUTPUT1("lod: %d", pTexture->max_level);

	const f32 Z = 0.0f;
	const f32 W = 1.0f;

	DisplayWindow & wnd = dwnd();
	const u32 winWidth = wnd.getWidth();
	const u32 winHeight = wnd.getHeight();
	f32 winAspect = f32(winWidth) / f32(winHeight);

	f32 width = fabsf(_lrx - ulx);
	f32 height = fabsf(_lry - uly);

	if (width > height) {
		f32 scale = height / width;
		f32 diff = 0.5f * width * (1.0f - scale);
		ulx += diff;
		_lrx -= diff;
		width = fabsf(_lrx - ulx);
	} else {
		f32 scale = width / height;
		f32 diff = 0.5f * height * (1.0f - scale);
		uly += diff;
		_lry -= diff;
		height = fabsf(_lry - uly);
	}

	if (pTexture->width <= pTexture->height) {
		f32 tex_aspect = f32(pTexture->width) / f32(pTexture->height);
		f32 scale = tex_aspect / winAspect;
		f32 diff = 0.5f * width * (1.0f - scale);
		ulx += diff;
		_lrx -= diff;
	} else {
		f32 tex_aspect = f32(pTexture->height) / f32(pTexture->width);
		f32 scale = tex_aspect / winAspect;
		f32 diff = 0.5f * height * (1.0f - scale);
		uly -= diff;
		_lry += diff;
	}

	RectVertex rect[4];
	rect[0].x = ulx;
	rect[0].y = -uly;
	rect[0].z = Z;
	rect[0].w = W;
	rect[1].x = _lrx;
	rect[1].y = rect[0].y;
	rect[1].z = Z;
	rect[1].w = W;
	rect[2].x = rect[0].x;
	rect[2].y = -_lry;
	rect[2].z = Z;
	rect[2].w = W;
	rect[3].x = rect[1].x;
	rect[3].y = rect[2].y;
	rect[3].z = Z;
	rect[3].w = W;

	f32 s0 = 0, t0 = 0, s1 = f32(pTexture->width), t1 = f32(pTexture->height);

	rect[0].s0 = s0;
	rect[0].t0 = t1;
	rect[1].s0 = s1;
	rect[1].t0 = t1;
	rect[2].s0 = s0;
	rect[2].t0 = t0;
	rect[3].s0 = s1;
	rect[3].t0 = t0;

	_setTextureCombiner();
	Context::TexParameters texParams;
	texParams.handle = m_pCurTexInfo->texture->name;
	texParams.target = textureTarget::TEXTURE_2D;
	texParams.textureUnitIndex = textureIndices::Tex[0];
	texParams.minFilter = textureParameters::FILTER_NEAREST;
	texParams.magFilter = textureParameters::FILTER_NEAREST;
	texParams.wrapS = textureParameters::WRAP_CLAMP_TO_EDGE;
	texParams.wrapT = textureParameters::WRAP_CLAMP_TO_EDGE;

	gfxContext.setTextureParameters(texParams);

	Context::DrawRectParameters rectParams;
	rectParams.mode = drawmode::TRIANGLE_STRIP;
	rectParams.verticesCount = 4;
	rectParams.vertices = rect;
	rectParams.combiner = currentCombiner();
	gfxContext.drawRects(rectParams);
}

void Debugger::_drawMouseCursor()
{
	long x, y;
	if (!getCursorPos(x, y))
		return;

	DisplayWindow & wnd = dwnd();
	const u32 winWidth = wnd.getWidth();
	const u32 winHeight = wnd.getHeight();

	if (x < 0 || x > (long)winWidth || y < 0 || y > (long)winHeight)
		return;

	const f32 scaleX = 1.0f / winWidth;
	const f32 scaleY = 1.0f / winHeight;

	SPVertex vertices[3];
	memset(vertices, 0,sizeof (vertices));

	SPVertex & v0 = vertices[0];
	v0.x = (f32)x * (2.0f * scaleX) - 1.0f;
	v0.y = (f32)y * (2.0f * scaleY) - 1.0f;
	v0.z = 0.0f;
	v0.w = 1.0f;
	SPVertex & v1 = vertices[1];
	v1 = v0;
	v1.y = (f32)(y + 10) * (2.0f * scaleY) - 1.0f;
	SPVertex & v2 = vertices[2];
	v2 = v1;
	v2.x = (f32)(x + 6) * (2.0f * scaleX) - 1.0f;

	const s32 hOffset = (wnd.getScreenWidth() - winWidth) / 2;
	const s32 vOffset = (wnd.getScreenHeight() - winHeight) / 2 + wnd.getHeightOffset();
	gfxContext.setViewport(hOffset,
		vOffset,
		winWidth, winHeight);

	_setLineCombiner();
	Context::DrawTriangleParameters triParams;
	triParams.mode = drawmode::TRIANGLES;
	triParams.verticesCount = 3;
	triParams.vertices = vertices;
	triParams.combiner = currentCombiner();
	gfxContext.drawTriangles(triParams);
}

void Debugger::_findSelected()
{
	if (!getCursorPos(m_clickX, m_clickY))
		return;

	const long x = m_clickX;
	const long y = m_clickY;
	DisplayWindow & wnd = dwnd();
	const long winWidth = (long)wnd.getWidth();
	const long winHeight = (long)wnd.getHeight();

	if (x < 0 || x > winWidth || y < 0 || y > winHeight)
		return;

	if (x < winWidth * 5 / 8 && y < winHeight * 5 / 8) {
		for (auto iter = std::next(m_triSel); iter != m_triangles.end(); ++iter) {
			if (iter->isInside(x, y)) {
				m_triSel = iter;
				return;
			}
		}

		for (auto iter = m_triangles.begin(); iter != m_triSel; ++iter) {
			if (iter->isInside(x, y)) {
				m_triSel = iter;
				return;
			}
		}
	}
}

void Debugger::_drawDebugInfo()
{
	DisplayWindow & wnd = dwnd();
	m_triSel = m_triangles.begin();
	m_clickX = m_clickY = 0;
	m_tmu = 0;
	m_startTexRow[0] = m_startTexRow[1] = 0;
	m_texturesToDisplay[0].clear();
	m_texturesToDisplay[1].clear();
	memset(m_selectedTexPos, 0, sizeof(m_selectedTexPos));

	for (auto& i : m_triangles) {
		if (i.frameBufferAddress != gDP.depthImageAddress)
			m_fbAddrs.insert(i.frameBufferAddress);
	}
	FrameBuffer * pBuffer = frameBufferList().getCurrent();
	if (pBuffer == nullptr)
		return;
	m_curFBAddr = m_fbAddrs.find(pBuffer->m_startAddress);
	if (m_curFBAddr == m_fbAddrs.end()) {
		for (m_curFBAddr = m_fbAddrs.begin(); m_curFBAddr != m_fbAddrs.end(); ++m_curFBAddr) {
			pBuffer = frameBufferList().findBuffer(*m_curFBAddr);
			if (pBuffer != nullptr && pBuffer->m_isMainBuffer && !pBuffer->m_isDepthBuffer)
				break;
		}
	}
	if (m_curFBAddr == m_fbAddrs.end())
		return;

	const u32 winWidth = wnd.getWidth();
	const u32 winHeight = wnd.getHeight();
	const s32 hOffset = (wnd.getScreenWidth() - winWidth) / 2;
	const s32 vOffset = (wnd.getScreenHeight() - winHeight) / 2 + wnd.getHeightOffset();
	const s32 areaWidth = winWidth * 3 / 8;

	float tW, tH;
	g_textDrawer.getTextSize("W_0'", tW, tH);
	const f32 yShift = tH * 1.6f;

	const f32 scaleX = 1.0f / winWidth;
	const f32 scaleY = 1.0f / winHeight;
	const f32 ulx = (f32)(winWidth - areaWidth) * (2.0f * scaleX) - 1.0f;
	const f32 uly = 1.0f - yShift;
	const f32 lrx = (f32)(winWidth) * (2.0f * scaleX) - 1.0f;
	const f32 lry = -((f32)(winHeight * 5 / 8)* (2.0f * scaleY) - 1.0f);

	while (!osal_is_key_pressed(KEY_Insert, 0x0001)) {
		osal_keys_update_state();
		_debugKeys();
		_drawFrameBuffer(frameBufferList().findBuffer(*m_curFBAddr));
		_drawTextureCache();

		if (osal_is_key_pressed(MB_Left, 0x0001))
			_findSelected();
		_drawTriangleFrame();
		_drawMouseCursor();

		gfxContext.setViewport(hOffset,
			vOffset,
			winWidth, winHeight);

		switch (m_curPage) {
		case Page::general:
			_drawGeneral(ulx, uly, yShift);
			break;
		case Page::tex1:
		case Page::tex2:
			_drawTex(ulx, uly, yShift);
			break;
		case Page::colors:
			_drawColors(ulx, uly, yShift);
			break;
		case Page::blender:
			_drawBlender(ulx, uly, yShift);
			break;
		case Page::othermode_l:
			_drawOthermodeL(ulx, uly, yShift);
			break;
		case Page::othermode_h:
			_drawOthermodeH(ulx, uly, yShift);
			break;
		case Page::texcoords:
			_drawTexCoords(ulx, uly, yShift);
			break;
		case Page::coords:
			_drawVertexCoords(ulx, uly, yShift);
			break;
		case Page::texinfo:
			_drawTexture(ulx, uly, lrx, lry, yShift);
			break;
		}

		wnd.swapBuffers();
	}

	m_triangles.clear();
	m_triSel = m_triangles.end();
	g_textDrawer.setTextColor(config.font.colorf);
	m_bCapture = false;
}

u32 Debugger::_performSceneRip()
{
	wchar_t txDumpPath[MAX_PATH] = {0};
	wchar_t * pTexDumpPath = config.textureFilter.txDumpPath;
	if (wcslen(config.textureFilter.txDumpPath) == 0 ||
		osal_is_absolute_path(config.textureFilter.txDumpPath) == 0) {
		api().GetUserCachePath(txDumpPath);
		wcscat(txDumpPath, wst("/texture_dump"));
		pTexDumpPath = txDumpPath;
	}

	wchar_t wRomName[21] = {0};
	mbstowcs(wRomName, RSP.romname, 21);

	wchar_t wSrPath[MAX_PATH] = {0};
	wcscpy(wSrPath, txDumpPath);
	wcscat(wSrPath, wst("/"));
	wcscat(wSrPath, wRomName);
	wcscat(wSrPath, wst("/GLideNHQ/scene_rips"));

	FILE *srFile = nullptr;

	if (!osal_path_existsW(wSrPath) && osal_mkdirp(wSrPath) != 0)
		return -1;

	wchar_t wSrFile[MAX_PATH] = {0};
	wcscpy(wSrFile, wSrPath);
	wcscat(wSrFile, wst("/n64_scene"));

	wchar_t wSrOnlyFile[MAX_PATH] = {0};
	wcscpy(wSrOnlyFile, wSrFile);
	wcscat(wSrOnlyFile, wst(".glr"));

	if(osal_path_existsW(wSrOnlyFile))
	{
		namespace fs = std::filesystem;

		int duplicate_num = -1;

		// !HACK ALERT!
		// Couldn't get unicode filename scanning working.
		// May cause problems on machines using certain locales and absolute path configs
		for(const fs::directory_entry & file : fs::directory_iterator(wSrPath)) {
			if(file.is_directory())
				continue;

			wstring wsCurFilename;

			try {
				wsCurFilename = file.path().filename().wstring();
			} catch (std::filesystem::__cxx11::filesystem_error e) {
				// skips unicode file/dir instead of crashing
				continue;
			}

			if(wsCurFilename.length() != 18)
				continue;

			int start_idx = wsCurFilename.find(wst("n64_scene."));
			int ext_start_idx = wsCurFilename.find(wst(".glr"));
			
			if(start_idx != 0 && ext_start_idx != 14)
				continue;

			wstring wsCurFilenameNums(wsCurFilename.substr(10,4));

			bool isValid = true;

			for(u32 i = 0; i < 4; i++) {
        		if(!isdigit(wsCurFilenameNums[i]))
            		isValid = false;
			}

			if(wsCurFilename.substr(14,4)!= wst(".glr"))
				isValid = false;

			if(isValid)
				duplicate_num++;
		}

		wchar_t numBuf[MAX_PATH] = {0};

		if(duplicate_num == -1) {
			swprintf(numBuf, MAX_PATH, wst(".%04d%s"), 0, ".glr");
		} else {
			swprintf(numBuf, MAX_PATH, wst(".%04d%s"), (duplicate_num + 1), ".glr");
		}

		wcscat(wSrFile, numBuf);
	}
	else
	{
		wcscpy(wSrFile, wSrOnlyFile);
	}

#ifdef OS_WINDOWS
	if ((srFile = _wfopen(wSrFile, wst("wb"))) != nullptr) {
#else
	char srFilebuf[MAX_PATH];
	wcstombs(srFilebuf, wSrFile, MAX_PATH);
	if ((srFile = fopen(srFilebuf, "wb")) != nullptr) {
#endif
		typedef struct
		{
			f32 scene_x, scene_y, scene_z;
			f32 r, g, b, a;
			f32 t0_s, t0_t;
			f32 t1_s, t1_t;
		} RipVertex;

		typedef struct
		{
			RipVertex vertices[3];
			u32 __PAD0;
			f32 prim_r, prim_g, prim_b, prim_a;
			f32 env_r, env_g, env_b, env_a;
			f32 blend_r, blend_g, blend_b, blend_a;
			u64 t0_riceCrc;
			u64 t1_riceCrc;
			u8 t0_wrapmode, t1_wrapmode;
			u8 t0_fmt, t0_size;
			u8 t1_fmt, t1_size;
			u16 __PAD1;
		} RipTriangle;

		typedef struct
		{
			const u8 MAGIC[6] = { 'G', 'L', '6', '4', 'R', '\0'};
			const u16 VERSION = 1;
			char romName[20] = {0}; // no null terminator
			u32 num_triangles = 0;
			f32 fog_r, fog_g, fog_b;
		} RipHeader;

		const u32 ALL_NUM_TRIANGLES = static_cast<u32>(m_triangles.size());

		u32 num_triangles = 0;

		m_triSel = m_triangles.cbegin();

		for(u32 i = 0; i < ALL_NUM_TRIANGLES; i++) {
			if((u32)(m_triSel->type) == 0)
				++num_triangles;
			++m_triSel;
		}

		if(num_triangles == 0)
			return -2;

		RipHeader header;
		header.num_triangles = num_triangles;
		strncpy(header.romName, RSP.romname, 20); // omitting null-terminator

		RipTriangle triangles[num_triangles];

		memset(&triangles, 0, sizeof(triangles));

		u32 tri_counter = 0;
		m_triSel = m_triangles.cbegin();

		for(u32 i = 0; i < ALL_NUM_TRIANGLES; i++) {
			if((u32)(m_triSel->type) != 0)
			{
				++m_triSel;
				continue;
			}

			if(tri_counter == 0)
			{
				header.fog_r = m_triSel->fog_color.r;
				header.fog_g = m_triSel->fog_color.g;
				header.fog_b = m_triSel->fog_color.b;
			}

			const Vertex & v0 = m_triSel->vertices[0];
			const Vertex & v1 = m_triSel->vertices[1];
			const Vertex & v2 = m_triSel->vertices[2];

			const int tmpA0 = m_triSel->combine.saRGB0;
			const int tmpB0 = m_triSel->combine.sbRGB0;

			f32 t0_sS = 1.0f;
			f32 t0_tS = 1.0f;
			f32 t0_offsetS = 0.0f;
			f32 t0_offsetT = 0.0f;
			u8 t0_sM = 4;
			u8 t0_tM = 4;
			u8 t0_wM = 16;
			u64 t0_ricecrc = 0;
			u8 t0_format = 0;
			u8 t0_size = 0;

			f32 t1_sS = 1.0f;
			f32 t1_tS = 1.0f;
			f32 t1_offsetS = 0.0f;
			f32 t1_offsetT = 0.0f;
			u8 t1_sM = 4;
			u8 t1_tM = 4;
			u8 t1_wM = 16;
			u64 t1_ricecrc = 0;
			u8 t1_format = 0;
			u8 t1_size = 0;

			if(tmpA0 == 1 || tmpB0 == 1)
			{
				if(m_triSel->tex_info[0]) {
					const CachedTexture *t0 = m_triSel->tex_info[0]->texture;
					t0_sS = m_triSel->tex_info[0]->scales * t0->scaleS;
					t0_tS = m_triSel->tex_info[0]->scalet * t0->scaleT;
					t0_offsetS = t0->offsetS;
					t0_offsetT = t0->offsetT;
					t0_sM = ((t0->clampS << 1) | t0->mirrorS) & 3;
					t0_tM = ((t0->clampT << 1) | t0->mirrorT) & 3;
					t0_wM = ((t0_sM << 2) | t0_tM) & 15;
					t0_format = t0->format;
					t0_size = t0->size;
					t0_ricecrc = t0->riceCrc;
				}
			}

			if(tmpA0 == 2) {
				if(m_triSel->tex_info[1]) {
					const CachedTexture *t1 = m_triSel->tex_info[1]->texture;
					t1_sS = m_triSel->tex_info[1]->scales * t1->scaleS;
					t1_tS = m_triSel->tex_info[1]->scalet * t1->scaleT;
					t1_offsetS = t1->offsetS;
					t1_offsetT = t1->offsetT;
					t1_sM = ((t1->clampS << 1) | t1->mirrorS) & 3;
					t1_tM = ((t1->clampT << 1) | t1->mirrorT) & 3;
					t1_wM = ((t0_sM << 2) | t0_tM) & 15;
					t1_format = t1->format;
					t1_size = t1->size;
					t1_ricecrc = t1->riceCrc;
				}
			}

			f32 v0_s0s = (v0.s0 * t0_sS) + t0_offsetS;
			f32 v0_t0s = ((-v0.t0 * t0_tS) + 1.0f) + t0_offsetT;
			f32 v0_s1s = (v0.s1 * t1_sS) + t1_offsetS;
			f32 v0_t1s = ((-v0.t1 * t1_tS) + 1.0f) + t1_offsetT;

			f32 v1_s0s = (v1.s0 * t0_sS) + t0_offsetS;
			f32 v1_t0s = ((-v1.t0 * t0_tS) + 1.0f) + t0_offsetT;
			f32 v1_s1s = (v1.s1 * t1_sS) + t1_offsetS;
			f32 v1_t1s = ((-v1.t1 * t1_tS) + 1.0f) + t1_offsetT;

			f32 v2_s0s = (v2.s0 * t0_sS) + t0_offsetS;
			f32 v2_t0s = ((-v2.t0 * t0_tS) + 1.0f) + t0_offsetT;
			f32 v2_s1s = (v2.s1 * t1_sS) + t1_offsetS;
			f32 v2_t1s = ((-v2.t1 * t1_tS) + 1.0f) + t1_offsetT;

			triangles[tri_counter].vertices[0].scene_x = v0.sx;
			triangles[tri_counter].vertices[0].scene_y = v0.sy;
			triangles[tri_counter].vertices[0].scene_z = v0.sz;
			triangles[tri_counter].vertices[0].r = v0.r;
			triangles[tri_counter].vertices[0].g = v0.g;
			triangles[tri_counter].vertices[0].b = v0.b;
			triangles[tri_counter].vertices[0].a = v0.a;
			triangles[tri_counter].vertices[0].t0_s = v0_s0s;
			triangles[tri_counter].vertices[0].t0_t = v0_t0s;
			triangles[tri_counter].vertices[0].t1_s = v0_s1s;
			triangles[tri_counter].vertices[0].t1_t = v0_t1s;

			triangles[tri_counter].vertices[1].scene_x = v1.sx;
			triangles[tri_counter].vertices[1].scene_y = v1.sy;
			triangles[tri_counter].vertices[1].scene_z = v1.sz;
			triangles[tri_counter].vertices[1].r = v1.r;
			triangles[tri_counter].vertices[1].g = v1.g;
			triangles[tri_counter].vertices[1].b = v1.b;
			triangles[tri_counter].vertices[1].a = v1.a;
			triangles[tri_counter].vertices[1].t0_s = v1_s0s;
			triangles[tri_counter].vertices[1].t0_t = v1_t0s;
			triangles[tri_counter].vertices[1].t1_s = v1_s1s;
			triangles[tri_counter].vertices[1].t1_t = v1_t1s;

			triangles[tri_counter].vertices[2].scene_x = v2.sx;
			triangles[tri_counter].vertices[2].scene_y = v2.sy;
			triangles[tri_counter].vertices[2].scene_z = v2.sz;
			triangles[tri_counter].vertices[2].r = v2.r;
			triangles[tri_counter].vertices[2].g = v2.g;
			triangles[tri_counter].vertices[2].b = v2.b;
			triangles[tri_counter].vertices[2].a = v2.a;
			triangles[tri_counter].vertices[2].t0_s = v2_s0s;
			triangles[tri_counter].vertices[2].t0_t = v2_t0s;
			triangles[tri_counter].vertices[2].t1_s = v2_s1s;
			triangles[tri_counter].vertices[2].t1_t = v2_t1s;

			triangles[tri_counter].prim_r = m_triSel->prim_color.r;
			triangles[tri_counter].prim_g = m_triSel->prim_color.g;
			triangles[tri_counter].prim_b = m_triSel->prim_color.b;
			triangles[tri_counter].prim_a = m_triSel->prim_color.a;

			triangles[tri_counter].env_r = m_triSel->env_color.r;
			triangles[tri_counter].env_g = m_triSel->env_color.g;
			triangles[tri_counter].env_b = m_triSel->env_color.b;
			triangles[tri_counter].env_a = m_triSel->env_color.a;

			triangles[tri_counter].blend_r = m_triSel->blend_color.r;
			triangles[tri_counter].blend_g = m_triSel->blend_color.g;
			triangles[tri_counter].blend_b = m_triSel->blend_color.b;
			triangles[tri_counter].blend_a = m_triSel->blend_color.a;

			triangles[tri_counter].t0_riceCrc = t0_ricecrc;
			triangles[tri_counter].t1_riceCrc = t1_ricecrc;

			triangles[tri_counter].t0_wrapmode = t0_wM;
			triangles[tri_counter].t1_wrapmode = t1_wM;

			triangles[tri_counter].t0_fmt = t0_format;
			triangles[tri_counter].t0_size = t0_size;
			triangles[tri_counter].t1_fmt = t1_format;
			triangles[tri_counter].t1_size = t1_size;

			++tri_counter;
			++m_triSel;
		}

		m_triSel = m_triangles.cbegin();

		if (!srFile) {
			srFile = nullptr;
			return -3;
		}

		fwrite(&header, sizeof(header), 1, srFile);
		fwrite(&triangles, sizeof(triangles), 1, srFile);

		if(config.sceneRipper.CSVExport) {
			FILE *srCsvFile = nullptr;

			wchar_t wCsvFile[MAX_PATH];
			wcscpy(wCsvFile, wSrFile);
			int curSrfpp = wcslen(wCsvFile);
			curSrfpp -= 4;
			wCsvFile[curSrfpp] = '\0';
			wcscat(wCsvFile, wst(".csv"));

#ifdef OS_WINDOWS
			if ((srCsvFile = _wfopen(wCsvFile, wst("wb"))) != nullptr) {
#else
			char srCsvFilebuf[MAX_PATH];
			wcstombs(srCsvFilebuf, wCsvFile, MAX_PATH);
			if ((srCsvFile = fopen(srCsvFilebuf, "wb")) != nullptr) {
#endif
				char **pCSVTriangles = new char*[num_triangles];

				const char *CSVFileHeader =
				"v0_x,v0_y,v0_z,"
				"v0_r,v0_g,v0_b,v0_a,"
				"v0_s0,v0_t0,v0_s1,v0_t1,"
				"v1_x,v1_y,v1_z,"
				"v1_r,v1_g,v1_b,v1_a,"
				"v1_s0,v1_t0,v1_s1,v1_t1,"
				"v2_x,v2_y,v2_z,"
				"v2_r,v2_g,v2_b,v2_a,"
				"v2_s0,v2_t0,v2_s1,v2_t1,"
				"prim_r,prim_g,prim_b,prim_a,"
				"env_r,env_g,env_b,env_a,"
				"blend_r,blend_g,blend_b,blend_a,"
				"t0_wrapmode,t1_wrapmode,"
				"t0_fmt,t0_size,"
				"t1_fmt,t1_size,"
				"t0_riceCrc, t1_riceCrc,"
				"t0_riceFilename, t1_riceFilename,"
				"fog_r,fog_g,fog_b\n";

				if (!srCsvFile) {
					srCsvFile = nullptr;
					return -4;
				}

				fwrite(CSVFileHeader, strlen(CSVFileHeader), 1, srCsvFile);

				for(u32 i = 0; i < num_triangles; i++)
				{
					char t0_combined_name[128];
					char t1_combined_name[128];

					char t0_strWm[16];
					char t1_strWm[16];

					const u8 tmpt0wm = triangles[i].t0_wrapmode;
					const u8 tmpt1wm = triangles[i].t1_wrapmode;
					const u8 tmpt0fmt = triangles[i].t0_fmt;
					const u8 tmpt0size = triangles[i].t0_size;
					const u8 tmpt1fmt = triangles[i].t1_fmt;
					const u8 tmpt1size = triangles[i].t1_size;
					const u64 tmpt0rcrc = triangles[i].t0_riceCrc;
					const u64 tmpt1rcrc = triangles[i].t1_riceCrc;

					sprintf(t0_strWm, "%s (%hhu)", combined_wrapmode_type[tmpt0wm], tmpt0wm);
					sprintf(t1_strWm, "%s (%hhu)", combined_wrapmode_type[tmpt1wm], tmpt1wm);

					wstring t0wsb;
					wstring t1wsb;
					wchar_t t0wcb[64];
					wchar_t t1wcb[64];
					
					t0wsb.append(txfilter_getFormattedDmpTxFilename(t0wcb, N64FormatSize(tmpt0fmt, tmpt0size), tmpt0rcrc));
					t1wsb.append(txfilter_getFormattedDmpTxFilename(t1wcb, N64FormatSize(tmpt1fmt, tmpt1size), tmpt1rcrc));

					wcstombs(t0_combined_name, t0wsb.c_str(), 128);
					wcstombs(t1_combined_name, t1wsb.c_str(), 128);

					if(tmpt0rcrc == 0)
						t0_combined_name[0] = '\0';

					if(tmpt1rcrc == 0)
						t1_combined_name[0] = '\0';

					pCSVTriangles[i] = new char[1024];

					sprintf(pCSVTriangles[i],
					"%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%f,%f,%f,%f,"
					"%s,%s,"
					"%hhu,%hhu,"
					"%hhu,%hhu,"
					"%llX,%llX,"
					"%s,%s",
					triangles[i].vertices[0].scene_x,
					triangles[i].vertices[0].scene_y,
					triangles[i].vertices[0].scene_z,
					triangles[i].vertices[0].r,
					triangles[i].vertices[0].g,
					triangles[i].vertices[0].b,
					triangles[i].vertices[0].a,
					triangles[i].vertices[0].t0_s,
					triangles[i].vertices[0].t0_t,
					triangles[i].vertices[0].t1_s,
					triangles[i].vertices[0].t1_t,
					triangles[i].vertices[1].scene_x,
					triangles[i].vertices[1].scene_y,
					triangles[i].vertices[1].scene_z,
					triangles[i].vertices[1].r,
					triangles[i].vertices[1].g,
					triangles[i].vertices[1].b,
					triangles[i].vertices[1].a,
					triangles[i].vertices[1].t0_s,
					triangles[i].vertices[1].t0_t,
					triangles[i].vertices[1].t1_s,
					triangles[i].vertices[1].t1_t,
					triangles[i].vertices[2].scene_x,
					triangles[i].vertices[2].scene_y,
					triangles[i].vertices[2].scene_z,
					triangles[i].vertices[2].r,
					triangles[i].vertices[2].g,
					triangles[i].vertices[2].b,
					triangles[i].vertices[2].a,
					triangles[i].vertices[2].t0_s,
					triangles[i].vertices[2].t0_t,
					triangles[i].vertices[2].t1_s,
					triangles[i].vertices[2].t1_t,
					triangles[i].prim_r,
					triangles[i].prim_g,
					triangles[i].prim_b,
					triangles[i].prim_a,
					triangles[i].env_r,
					triangles[i].env_g,
					triangles[i].env_b,
					triangles[i].env_a,
					triangles[i].blend_r,
					triangles[i].blend_g,
					triangles[i].blend_b,
					triangles[i].blend_a,
					t0_strWm,
					t1_strWm,
					tmpt0fmt,
					tmpt0size,
					tmpt1fmt,
					tmpt1size,
					tmpt0rcrc,
					tmpt1rcrc,
					t0_combined_name,
					t1_combined_name);

					const size_t currCSVRowLen = strlen(pCSVTriangles[i]);

					if(i != 0) {
						sprintf((pCSVTriangles[i] + currCSVRowLen), "\n");
					} else {
						sprintf((pCSVTriangles[0] + currCSVRowLen),
							",%f,%f,%f\n",
							header.fog_r,
							header.fog_g,
							header.fog_b);
					}

					fwrite(pCSVTriangles[i], strlen(pCSVTriangles[i]), 1, srCsvFile);
					delete[] pCSVTriangles[i];
				}
				
				delete[] pCSVTriangles;

				if (srCsvFile) {
					fclose(srCsvFile);
					srCsvFile = nullptr;
				}
			} else {
				return -6;
			}
		}

		if (srFile) {
			fclose(srFile);
			srFile = nullptr;
		}

		displayLoadProgress(L"Scene Dumped!\n");
		std::this_thread::sleep_for(std::chrono::seconds(1));

		return 1;
	} else {
		return -5;
	}
}

void Debugger::draw()
{
	if (m_triangles.empty()) {
		_drawFrameBuffer(frameBufferList().getCurrent());
		dwnd().swapBuffers();
	} else {
		_drawDebugInfo();
	}

	gfxContext.bindFramebuffer(bufferTarget::READ_FRAMEBUFFER, ObjectHandle::defaultFramebuffer);
	FrameBuffer *pBuffer = frameBufferList().getCurrent();
	if (pBuffer != nullptr)
		gfxContext.bindFramebuffer(bufferTarget::DRAW_FRAMEBUFFER, pBuffer->m_FBO);
	gDP.changed |= CHANGED_SCISSOR;
}

void Debugger::performSceneRip()
{
	textureCache().update(0);
	textureCache().update(1);
	currentCombiner()->update(true);
	u32 result = _performSceneRip();
}

#else // DEBUG_DUMP

Debugger::Debugger() : m_bDebugMode(false) {}
Debugger::~Debugger() {}
void Debugger::checkDebugState() {}
void Debugger::addTriangles(const graphics::Context::DrawTriangleParameters &) {}
void Debugger::addRects(const graphics::Context::DrawRectParameters &) {}
void Debugger::draw() {}
void Debugger::performSceneRip() {}

#endif // DEBUG_DUMP
