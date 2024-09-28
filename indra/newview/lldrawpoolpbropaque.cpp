/**
 * @file lldrawpoolpbropaque.cpp
 * @brief LLDrawPoolGLTFPBR class implementation
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

#include "llviewerprecompiledheaders.h"

#include "lldrawpool.h"
#include "lldrawpoolpbropaque.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "gltfscenemanager.h"

extern LLCullResult* sCull;

LLDrawPoolGLTFPBR::LLDrawPoolGLTFPBR(U32 type) :
    LLRenderPass(type)
{
    if (type == LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK)
    {
        mRenderType = LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK;
    }
    else
    {
        mRenderType = LLPipeline::RENDER_TYPE_PASS_GLTF_PBR;
    }
}

S32 LLDrawPoolGLTFPBR::getNumDeferredPasses()
{
    return 1;
}

void LLDrawPoolGLTFPBR::renderDeferred(S32 pass)
{
    llassert(!LLPipeline::sRenderingHUDs);

    if (mRenderType == LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK)
    {
        // opaque
        LL::GLTFSceneManager::instance().render(true);
        // opaque rigged
        LL::GLTFSceneManager::instance().render(true, true);
    }

    LLGLTFMaterial::AlphaMode alpha_mode = mRenderType == LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK ? LLGLTFMaterial::ALPHA_MODE_MASK : LLGLTFMaterial::ALPHA_MODE_OPAQUE;

    for (U32 planar = 0; planar < 2; ++planar)
    {
        gGLTFPBRShaderPack.mShader[alpha_mode][0][planar].bind();
        pushGLTFBatches(sCull->mGLTFBatches.mDrawInfo[alpha_mode][0][planar], planar);

        gGLTFPBRShaderPack.mShader[alpha_mode][0][planar].bind(true);
        pushRiggedGLTFBatches(sCull->mGLTFBatches.mSkinnedDrawInfo[alpha_mode][0][planar], planar);

        { // double sided
            LLGLDisable cull(GL_CULL_FACE);
            gGLTFPBRShaderPack.mShader[alpha_mode][1][planar].bind();
            pushGLTFBatches(sCull->mGLTFBatches.mDrawInfo[alpha_mode][1][planar], planar);

            gGLTFPBRShaderPack.mShader[alpha_mode][1][planar].bind(true);
            pushRiggedGLTFBatches(sCull->mGLTFBatches.mSkinnedDrawInfo[alpha_mode][1][planar], planar);
        }
    }
}

S32 LLDrawPoolGLTFPBR::getNumPostDeferredPasses()
{
    return 1;
}

void LLDrawPoolGLTFPBR::renderPostDeferred(S32 pass)
{
    if (LLPipeline::sRenderingHUDs)
    {
        gHUDPBROpaqueProgram.bind();
        //pushGLTFBatches(mRenderType);
    }
    else if (mRenderType == LLPipeline::RENDER_TYPE_PASS_GLTF_PBR) // HACK -- don't render glow except for the non-alpha masked implementation
    {
#if 0
        gGL.setColorMask(false, true);
        gPBRGlowProgram.bind();
        pushGLTFBatches(LLRenderPass::PASS_GLTF_GLOW);

        gPBRGlowProgram.bind(true);
        pushRiggedGLTFBatches(LLRenderPass::PASS_GLTF_GLOW_RIGGED);
#endif

        gGL.setColorMask(true, false);
    }
}

