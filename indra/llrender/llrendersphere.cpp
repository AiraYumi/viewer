/**
 * @file llrendersphere.cpp
 * @brief implementation of the LLRenderSphere class.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

//  Sphere creates a set of display lists that can then be called to create
//  a lit sphere at different LOD levels.  You only need one instance of sphere
//  per viewer - then call the appropriate list.

#include "linden_common.h"

#include "llrendersphere.h"
#include "llerror.h"

#include "llglheaders.h"
#include "llvertexbuffer.h"
#include "llglslshader.h"

LLRenderSphere gSphere;

void LLRenderSphere::render()
{
    renderGGL();
    gGL.flush();
}

inline LLVector3 polar_to_cart(F32 latitude, F32 longitude)
{
    return LLVector3(sin(F_TWO_PI * latitude) * cos(F_TWO_PI * longitude),
                     sin(F_TWO_PI * latitude) * sin(F_TWO_PI * longitude),
                     cos(F_TWO_PI * latitude));
}


void LLRenderSphere::renderGGL()
{
    LL_PROFILE_ZONE_SCOPED;
    S32 const LATITUDE_SLICES = 20;
    S32 const LONGITUDE_SLICES = 30;

    if (mVertexBuffer.isNull())
    {
        mSpherePoints.resize(LATITUDE_SLICES + 1);
        mVertexBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);

        mVertexBuffer->allocateBuffer((U32)(LATITUDE_SLICES + 1) * (LONGITUDE_SLICES + 1), LATITUDE_SLICES * LONGITUDE_SLICES * 6);

        LLStrider<LLVector3> v;
        mVertexBuffer->getVertexStrider(v);

        for (S32 lat_i = 0; lat_i < LATITUDE_SLICES + 1; lat_i++)
        {
            mSpherePoints[lat_i].resize(LONGITUDE_SLICES + 1);
            for (S32 lon_i = 0; lon_i < LONGITUDE_SLICES + 1; lon_i++)
            {
                F32 lat = (F32)lat_i / LATITUDE_SLICES;
                F32 lon = (F32)lon_i / LONGITUDE_SLICES;

                mSpherePoints[lat_i][lon_i] = polar_to_cart(lat, lon);
                v[lat_i * (LONGITUDE_SLICES + 1) + lon_i] = mSpherePoints[lat_i][lon_i];
            }
        }

        LLStrider<U16> i;
        mVertexBuffer->getIndexStrider(i);

        for (S32 lat_i = 0; lat_i < LATITUDE_SLICES; lat_i++)
        {
            for (S32 lon_i = 0; lon_i < LONGITUDE_SLICES; lon_i++)
            {
                i[(lat_i * LONGITUDE_SLICES + lon_i) * 6 + 0] = lat_i * (LONGITUDE_SLICES + 1) + lon_i;
                i[(lat_i * LONGITUDE_SLICES + lon_i) * 6 + 1] = lat_i * (LONGITUDE_SLICES + 1) + lon_i + 1;
                i[(lat_i * LONGITUDE_SLICES + lon_i) * 6 + 2] = (lat_i + 1) * (LONGITUDE_SLICES + 1) + lon_i;

                i[(lat_i * LONGITUDE_SLICES + lon_i) * 6 + 3] = (lat_i + 1) * (LONGITUDE_SLICES + 1) + lon_i;
                i[(lat_i * LONGITUDE_SLICES + lon_i) * 6 + 4] = lat_i * (LONGITUDE_SLICES + 1) + lon_i + 1;
                i[(lat_i * LONGITUDE_SLICES + lon_i) * 6 + 5] = (lat_i + 1) * (LONGITUDE_SLICES + 1) + lon_i + 1;
            }
        }

        mVertexBuffer->unmapBuffer();
    }


    if (LLGLSLShader::sCurBoundShaderPtr->mAttributeMask == LLVertexBuffer::MAP_VERTEX)
    { // shader expects only vertex positions in vertex buffer, use fast path
        mVertexBuffer->setBuffer();
        mVertexBuffer->drawRange(LLRender::TRIANGLES, 0, mVertexBuffer->getNumVerts(), mVertexBuffer->getNumIndices(), 0);
    }
    else
    { //shader wants colors in the vertex stream, use slow path
        gGL.begin(LLRender::TRIANGLES);
        for (S32 lat_i = 0; lat_i < LATITUDE_SLICES; lat_i++)
        {
            for (S32 lon_i = 0; lon_i < LONGITUDE_SLICES; lon_i++)
            {
                gGL.vertex3fv(mSpherePoints[lat_i][lon_i].mV);
                gGL.vertex3fv(mSpherePoints[lat_i][lon_i + 1].mV);
                gGL.vertex3fv(mSpherePoints[lat_i + 1][lon_i].mV);

                gGL.vertex3fv(mSpherePoints[lat_i + 1][lon_i].mV);
                gGL.vertex3fv(mSpherePoints[lat_i][lon_i + 1].mV);
                gGL.vertex3fv(mSpherePoints[lat_i + 1][lon_i + 1].mV);
            }
        }
        gGL.end();
    }
}
