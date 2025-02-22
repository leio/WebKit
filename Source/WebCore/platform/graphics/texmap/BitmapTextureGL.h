/*
 Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 Copyright (C) 2014 Igalia S.L.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
 */

#ifndef BitmapTextureGL_h
#define BitmapTextureGL_h

#if USE(TEXTURE_MAPPER_GL)

#include "BitmapTexture.h"
#include "ClipStack.h"
#include "FilterOperation.h"
#include "IntSize.h"
#include "TextureMapperContextAttributes.h"
#include "TextureMapperGL.h"
#include "TextureMapperGLHeaders.h"

namespace WebCore {

class TextureMapper;
class TextureMapperGL;
class FilterOperation;
class NativeImage;

#if OS(WINDOWS)
#define USE_TEXMAP_DEPTH_STENCIL_BUFFER 1
#else
#define USE_TEXMAP_DEPTH_STENCIL_BUFFER 0
#endif

class BitmapTextureGL : public BitmapTexture {
public:
    static Ref<BitmapTexture> create(const TextureMapperContextAttributes& contextAttributes, const Flags flags = NoFlag, GLint internalFormat = GL_DONT_CARE)
    {
        return adoptRef(*new BitmapTextureGL(contextAttributes, flags, internalFormat));
    }

    virtual ~BitmapTextureGL();

    IntSize size() const override;
    bool isValid() const override;
    void didReset() override;
    void bindAsSurface();
    void initializeStencil();
    void initializeDepthBuffer();
    virtual uint32_t id() const { return m_id; }
    uint32_t textureTarget() const { return GL_TEXTURE_2D; }
    IntSize textureSize() const { return m_textureSize; }
    void updateContents(NativeImage*, const IntRect&, const IntPoint&) override;
    void updateContents(const void*, const IntRect& target, const IntPoint& sourceOffset, int bytesPerLine) override;
    bool isBackedByOpenGL() const override { return true; }

    RefPtr<BitmapTexture> applyFilters(TextureMapper&, const FilterOperations&, bool defersLastFilterPass) override;
    struct FilterInfo {
        RefPtr<const FilterOperation> filter;

        FilterInfo(RefPtr<const FilterOperation>&& f = nullptr)
            : filter(WTFMove(f))
            { }
    };

    const FilterInfo* filterInfo() const { return &m_filterInfo; }
    void setFilterInfo(FilterInfo&& filterInfo) { m_filterInfo = WTFMove(filterInfo); }

    ClipStack& clipStack() { return m_clipStack; }

    GLint internalFormat() const { return m_internalFormat; }

    void copyFromExternalTexture(GLuint textureID);

    TextureMapperGL::Flags colorConvertFlags() const { return m_colorConvertFlags; }

private:
    BitmapTextureGL(const TextureMapperContextAttributes&, const Flags, GLint internalFormat);

    GLuint m_id { 0 };
    IntSize m_textureSize;
    GLuint m_fbo { 0 };
#if !USE(TEXMAP_DEPTH_STENCIL_BUFFER)
    GLuint m_rbo { 0 };
#endif
    GLuint m_depthBufferObject { 0 };
    bool m_shouldClear { true };
    ClipStack m_clipStack;
    TextureMapperContextAttributes m_contextAttributes;
    TextureMapperGL::Flags m_colorConvertFlags { TextureMapperGL::NoFlag };

    void clearIfNeeded();
    void createFboIfNeeded();

    FilterInfo m_filterInfo;

    GLint m_internalFormat;
    GLenum m_format;
    GLenum m_type {
#if OS(DARWIN)
        GL_UNSIGNED_INT_8_8_8_8_REV
#else
        GL_UNSIGNED_BYTE
#endif
    };
};

BitmapTextureGL* toBitmapTextureGL(BitmapTexture*);

}

#endif // USE(TEXTURE_MAPPER_GL)

#endif // BitmapTextureGL_h
