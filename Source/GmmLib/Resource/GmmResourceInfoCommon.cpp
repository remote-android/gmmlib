/*==============================================================================
Copyright(c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files(the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and / or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
============================================================================*/


#include "Internal/Common/GmmLibInc.h"

/////////////////////////////////////////////////////////////////////////////////////
/// Returns indication of whether resource is eligible for 64KB pages or not.
/// On Windows, UMD must call this api after GmmResCreate()
/// @return     TRUE/FALSE
/////////////////////////////////////////////////////////////////////////////////////
BOOLEAN GMM_STDCALL GmmLib::GmmResourceInfoCommon::Is64KBPageSuitable()
{
    BOOLEAN Ignore64KBPadding = FALSE;
    //!!!! DO NOT USE GetSizeSurface() as it returns the padded size and not natural size.
    GMM_GFX_SIZE_T  Size = Surf.Size + AuxSurf.Size + AuxSecSurf.Size;

    __GMM_ASSERT(Size);

    // All ESM resources and VirtuaPadding are exempt from 64KB paging
    if (Surf.Flags.Info.ExistingSysMem ||
        Surf.Flags.Info.XAdapter ||
        Surf.Flags.Gpu.CameraCapture ||
        Surf.Flags.Info.KernelModeMapped ||
        (Surf.Flags.Gpu.S3d && !Surf.Flags.Gpu.S3dDx &&
            !pGmmGlobalContext->GetSkuTable().FtrDisplayEngineS3d)
#if(LHDM)
        || (Surf.Flags.Info.AllowVirtualPadding &&
            ExistingSysMem.hParentAllocation)
#endif    
        )
    {
        Ignore64KBPadding = TRUE;
    }

    // If 64KB paging is enabled pad out the resource to 64KB alignment
    if (pGmmGlobalContext->GetSkuTable().FtrWddm2_1_64kbPages &&
        // Ignore the padding for the above VirtualPadding or ESM cases
        (!Ignore64KBPadding) &&
        // Resource must be 64KB aligned
        (GFX_IS_ALIGNED(Surf.Alignment.BaseAlignment, GMM_KBYTE(64)) ||
            // Or must be aligned to a factor of 64KB
        (Surf.Alignment.BaseAlignment == GMM_KBYTE(32)) ||
            (Surf.Alignment.BaseAlignment == GMM_KBYTE(16)) ||
            (Surf.Alignment.BaseAlignment == GMM_KBYTE(8)) ||
            (Surf.Alignment.BaseAlignment == GMM_KBYTE(4))) &&
        // The final padded size cannot be larger then a set percentage of the original size
           ( (!Surf.Flags.Info.NoOptimizationPadding && 
               ((Size* (100 + pGmmGlobalContext->GetAllowedPaddingFor64KbPagesPercentage())) / 100) >= GFX_ALIGN(Size, GMM_KBYTE(64))) ||
               (Surf.Flags.Info.NoOptimizationPadding && GFX_IS_ALIGNED(Size, GMM_KBYTE(64))))
        
        )
    {
        return TRUE;
    }

    return FALSE;
}


/////////////////////////////////////////////////////////////////////////////////////
/// Allows clients to "create" any type of resource. This function does not 
/// allocate any memory for the resource. It just calculates the various parameters
/// which are useful for the client and can be queried using other functions.
///
/// @param[in]  GmmLib Context: Reference to ::GmmLibContext
/// @param[in]  CreateParams: Flags which specify what sort of resource to create
///
/// @return     ::GMM_STATUS
/////////////////////////////////////////////////////////////////////////////////////
GMM_STATUS GMM_STDCALL GmmLib::GmmResourceInfoCommon::Create(Context &GmmLibContext, GMM_RESCREATE_PARAMS &CreateParams)
{
    const GMM_PLATFORM_INFO* pPlatform;
    GMM_STATUS  Status = GMM_ERROR;
    GMM_TEXTURE_CALC* pTextureCalc = NULL;

    GMM_DPF_ENTER;

    __GMM_ASSERTPTR(pGmmGlobalContext, GMM_ERROR);

    if (CreateParams.Flags.Info.ExistingSysMem && 
        (CreateParams.Flags.Info.TiledW ||
         CreateParams.Flags.Info.TiledX || 
         CreateParams.Flags.Info.TiledY))
    {
        GMM_ASSERTDPF(0, "Tiled System Accelerated Memory not supported.");
        Status = GMM_INVALIDPARAM;
        goto ERROR_CASE;
    }

    pGmmLibContext = reinterpret_cast<GMM_VOIDPTR64>(&GmmLibContext);
    if(!CopyClientParams(CreateParams))
    {
        Status = GMM_INVALIDPARAM;
        goto ERROR_CASE;
    }

    pPlatform = GMM_OVERRIDE_PLATFORM_INFO(&Surf);
    pTextureCalc = GMM_OVERRIDE_TEXTURE_CALC(&Surf);

#if defined(__GMM_KMD__) || !defined(_WIN32)
    if (!CreateParams.Flags.Info.ExistingSysMem)
#else
    // TiledResource uses a private gfx alloc, which doesn't receive a  WDDM CreateAllocation
    if (!CreateParams.Flags.Info.ExistingSysMem && 
        (CreateParams.NoGfxMemory || CreateParams.Flags.Gpu.TiledResource))
#endif
    {
        if (!ValidateParams())
        {
            GMM_ASSERTDPF(0, "Invalid parameter!");
            Status = GMM_INVALIDPARAM;
            goto ERROR_CASE;
        }

        if (GMM_SUCCESS != pTextureCalc->AllocateTexture(&Surf))
        {
            GMM_ASSERTDPF(0, "GmmTexAlloc failed!");
            goto ERROR_CASE;
        }

        // Fill out the texture info for each plane if they require rediscription
        if (Surf.Flags.Info.RedecribedPlanes)
        {
            if (FALSE == RedescribePlanes())
            {
                GMM_ASSERTDPF(0, "Redescribe planes failed!");
                goto ERROR_CASE;
            }
        }

        if (Surf.Flags.Gpu.UnifiedAuxSurface)
        {
            GMM_GFX_SIZE_T  TotalSize;
            uint32_t           Alignment;

            if (GMM_SUCCESS != pTextureCalc->FillTexCCS(&Surf, 
                                               (AuxSecSurf.Type != RESOURCE_INVALID ? &AuxSecSurf : &AuxSurf))
               )
            {
                GMM_ASSERTDPF(0, "GmmTexAlloc failed!");
                goto ERROR_CASE;
            }

            if (AuxSurf.Size == 0 && GMM_SUCCESS != pTextureCalc->AllocateTexture(&AuxSurf))
            {
                GMM_ASSERTDPF(0, "GmmTexAlloc failed!");
                goto ERROR_CASE;
            }

            if (AuxSurf.Flags.Info.RedecribedPlanes)
            {
                int MaxPlanes = (GmmIsUVPacked(Surf.Format) ? GMM_PLANE_U : GMM_PLANE_V);
                for (int i = GMM_PLANE_Y; i <= MaxPlanes; i++)
                {
                    if (GMM_SUCCESS != pTextureCalc->AllocateTexture(&PlaneAuxSurf[i]))
                    {
                        GMM_ASSERTDPF(0, "GmmTexAlloc failed!");
                        goto ERROR_CASE;
                    }
                }
                ReAdjustPlaneProperties(TRUE);
            }

            AuxSurf.UnpaddedSize = AuxSurf.Size;

            if (Surf.Flags.Gpu.IndirectClearColor)
            {
                AuxSurf.CCSize = PAGE_SIZE;  // 128bit Float Value + 32bit RT Native Value + Padding.
                AuxSurf.Size += PAGE_SIZE;
            }
            
            TotalSize = Surf.Size + AuxSurf.Size;    //Not including AuxSecSurf size, multi-Aux surface isn't supported for displayables
            Alignment = GFX_ULONG_CAST(Surf.Pitch * pPlatform->TileInfo[Surf.TileMode].LogicalTileHeight);

            // We need to pad the aux size to the size of the paired surface's tile row (i.e. Pitch * TileHeight) to
            // ensure the entire surface can be described with a constant pitch (for GGTT aliasing, clean FENCE'ing and
            // AcquireSwizzlingRange, even though the aux isn't intentionally part of such fencing).
            if (Surf.Flags.Gpu.FlipChain &&
                !__GMM_IS_ALIGN(TotalSize, Alignment))
            {
                AuxSurf.Size += (GFX_ALIGN_NP2(TotalSize, Alignment) - TotalSize);
            }

            if ((Surf.Size + AuxSurf.Size + AuxSecSurf.Size) 
                       > (GMM_GFX_SIZE_T)(pPlatform->SurfaceMaxSize))
            {
                GMM_ASSERTDPF(0, "Surface too large!");
                goto ERROR_CASE;
            }
        }
    }

    __GMM_ASSERT(!(Surf.Flags.Info.ExistingSysMem && CreateParams.NoGfxMemory));
    if (Surf.Flags.Info.ExistingSysMem)
    {
        Surf.ExistingSysMem.IsGmmAllocated =
            (CreateParams.pExistingSysMem &&
            CreateParams.ExistingSysMemSize) ? FALSE : TRUE;

        if (!Surf.ExistingSysMem.IsGmmAllocated)
        {
            Surf.ExistingSysMem.IsPageAligned =
                (((CreateParams.pExistingSysMem & (PAGE_SIZE - 1)) == 0) &&
                (((CreateParams.pExistingSysMem + CreateParams.ExistingSysMemSize)
                & (PAGE_SIZE - 1)) == 0)) ? TRUE : FALSE;
        }

        if (!ValidateParams())
        {
            GMM_ASSERTDPF(0, "Invalid parameter!");
            goto ERROR_CASE;
        }

        // Get surface Gfx memory size required.
        if (GMM_SUCCESS != pTextureCalc->AllocateTexture(&Surf))
        {
            GMM_ASSERTDPF(0, "GmmTexAlloc failed!");
            goto ERROR_CASE;
        }

        if (CreateParams.pExistingSysMem &&
            CreateParams.ExistingSysMemSize)
        {
            // Client provided own memory and is not assumed to be Gfx aligned 
            ExistingSysMem.IsGmmAllocated = FALSE;

            ExistingSysMem.pExistingSysMem = CreateParams.pExistingSysMem;
            ExistingSysMem.Size = CreateParams.ExistingSysMemSize;

            // An upper dword of 0xffffffff is invalid and may mean the address
            // was sign extended or came from a rogue UMD. In either case
            // we can truncate the address down to 32 bits prevent attempts
            // to access an invalid address range.
            if ((ExistingSysMem.pExistingSysMem & (0xffffffff00000000ull)) == (0xffffffff00000000ull))
            {
                ExistingSysMem.pExistingSysMem &= 0xffffffff;
            }

            //Align the base address to new ESM requirements.
            if(!Surf.ExistingSysMem.IsPageAligned)
            {
                if (GMM_SUCCESS != ApplyExistingSysMemRestrictions())
                {
                    GMM_ASSERTDPF(0, "Malloc'ed Sys Mem too small for gfx surface!");
                    goto ERROR_CASE;
                }
            }
            else
            {
                ExistingSysMem.pVirtAddress =
                    ExistingSysMem.pGfxAlignedVirtAddress = CreateParams.pExistingSysMem;
            }

            if ((ExistingSysMem.pVirtAddress + Surf.Size) >
                (CreateParams.pExistingSysMem + ExistingSysMem.Size))
            {
                GMM_ASSERTDPF(0, "Malloc'ed Sys Mem too small for gfx surface");
                goto ERROR_CASE;
            }
        }
        else
        {
            __GMM_BUFFER_TYPE Restrictions = { 0 };

            ExistingSysMem.IsGmmAllocated = TRUE;
            Surf.ExistingSysMem.IsPageAligned = TRUE;

            // Adjust memory size to compensate for Gfx alignment.
            GetRestrictions(Restrictions);
            ExistingSysMem.Size = Restrictions.Alignment + Surf.Size;

            ExistingSysMem.pVirtAddress = (GMM_VOIDPTR64) GMM_MALLOC(GFX_ULONG_CAST(ExistingSysMem.Size));
            if (!ExistingSysMem.pVirtAddress)
            {
                GMM_ASSERTDPF(0, "Failed to allocate System Accelerated Memory.");
                goto ERROR_CASE;
            }
            else
            {
                ExistingSysMem.pGfxAlignedVirtAddress = (GMM_VOIDPTR64)GFX_ALIGN(ExistingSysMem.pVirtAddress, Restrictions.Alignment);
            }
        }
    }

    GMM_DPF_EXIT;
    return GMM_SUCCESS;

ERROR_CASE:
    //Zero out all the members
    new(this) GmmResourceInfoCommon();

    GMM_DPF_EXIT;
    return Status;
}

BOOLEAN GmmLib::GmmResourceInfoCommon::RedescribePlanes()
{
    const GMM_PLATFORM_INFO* pPlatform;
    GMM_TEXTURE_CALC* pTextureCalc = NULL;
    GMM_STATUS Status = GMM_SUCCESS;
    int MaxPlanes = 1;

    pPlatform = GMM_OVERRIDE_PLATFORM_INFO(&Surf);
    pTextureCalc = GMM_OVERRIDE_TEXTURE_CALC(&Surf);

    __GMM_ASSERT(Surf.Flags.Info.RedecribedPlanes);

    GMM_TEXTURE_INFO *pYPlane = &PlaneSurf[GMM_PLANE_Y];
    GMM_TEXTURE_INFO *pUPlane = &PlaneSurf[GMM_PLANE_U];
    GMM_TEXTURE_INFO *pVPlane = &PlaneSurf[GMM_PLANE_V];
    GMM_TEXTURE_INFO *pYAuxPlane = &PlaneAuxSurf[GMM_PLANE_Y];
    GMM_TEXTURE_INFO *pUAuxPlane = &PlaneAuxSurf[GMM_PLANE_U];
    GMM_TEXTURE_INFO *pVAuxPlane = &PlaneAuxSurf[GMM_PLANE_V];

    pYPlane->Type = Surf.Type;
    pYPlane->BaseWidth = Surf.BaseWidth;
    pYPlane->BaseHeight = Surf.BaseHeight;
    pYPlane->Depth = Surf.Depth;
    pYPlane->ArraySize = Surf.ArraySize;
    pYPlane->MSAA = Surf.MSAA;
    pYPlane->Flags = Surf.Flags;
    pYPlane->BitsPerPixel = Surf.BitsPerPixel;

#if(_DEBUG || _RELEASE_INTERNAL)
    pYPlane->Platform = Surf.Platform;
#endif

    pYPlane->Flags.Info.RedecribedPlanes = FALSE;

    *pUPlane = *pVPlane = *pYPlane;

    if (GmmIsUVPacked(Surf.Format))
    {
        // UV packed resources must have two seperate
        // tiling modes per plane, due to the packed
        // UV plane having twice the bits per pixel
        // as the Y plane.

        if (Surf.BitsPerPixel == 8)
        {
            pYPlane->BitsPerPixel = 8;
            pYPlane->Format = GMM_FORMAT_R8_UINT;

            pUPlane->BitsPerPixel = 16;
            pUPlane->Format = GMM_FORMAT_R16_UINT;
        }
        else if (Surf.BitsPerPixel == 16)
        {
            pYPlane->BitsPerPixel = 16;
            pYPlane->Format = GMM_FORMAT_R16_UINT;

            pUPlane->BitsPerPixel = 32;
            pUPlane->Format = GMM_FORMAT_R32_UINT;
        }
        else
        {
            GMM_ASSERTDPF(0, "Unsupported format/pixel size combo!");
            Status = GMM_INVALIDPARAM;
            goto ERROR_CASE;
        }

        pUPlane->BaseHeight = GFX_CEIL_DIV(pYPlane->BaseHeight, 2);
        pUPlane->BaseWidth = GFX_CEIL_DIV(pYPlane->BaseWidth, 2);
        MaxPlanes = 2;
    }
    else
    {
        // Non-UV packed surfaces only require the plane descriptors
        // have proper height and width for each plane
        switch (Surf.Format)
        {
        case GMM_FORMAT_IMC1:
        case GMM_FORMAT_IMC2:
        case GMM_FORMAT_IMC3:
        case GMM_FORMAT_IMC4:
        case GMM_FORMAT_MFX_JPEG_YUV420:
        {
            pUPlane->BaseWidth = pVPlane->BaseWidth = GFX_CEIL_DIV(pYPlane->BaseWidth, 2);
        }
        case GMM_FORMAT_MFX_JPEG_YUV422V:
        {
            pUPlane->BaseHeight = pVPlane->BaseHeight = GFX_CEIL_DIV(pYPlane->BaseHeight, 2);
            break;
        }
        case GMM_FORMAT_MFX_JPEG_YUV411R_TYPE:
        {
            pUPlane->BaseHeight = pVPlane->BaseHeight = GFX_CEIL_DIV(pYPlane->BaseHeight, 4);
            break;
        }
        case GMM_FORMAT_MFX_JPEG_YUV411:
        {
            pUPlane->BaseWidth = pVPlane->BaseWidth = GFX_CEIL_DIV(pYPlane->BaseWidth, 4);
            break;
        }
        case GMM_FORMAT_MFX_JPEG_YUV422H:
        {
            pUPlane->BaseWidth = pVPlane->BaseWidth = GFX_CEIL_DIV(pYPlane->BaseWidth, 2);
            break;
        }
        default:
        {
            break;
        }
        }

        pYPlane->Format = pUPlane->Format = pVPlane->Format =
            (pYPlane->BitsPerPixel == 8) ? GMM_FORMAT_R8_UINT : GMM_FORMAT_R16_UINT;
        MaxPlanes = 3;
    }

    for (int i = GMM_PLANE_Y; i <= MaxPlanes; i++) // all 2 or 3 planes
    {
        if (Surf.Flags.Gpu.UnifiedAuxSurface)
        {
            PlaneAuxSurf[i] = PlaneSurf[i];

            if (GMM_SUCCESS != pTextureCalc->PreProcessTexSpecialCases(&PlaneAuxSurf[i]))
            {
                return FALSE;
            }
        }

        if ((GMM_SUCCESS != pTextureCalc->AllocateTexture(&PlaneSurf[i])))
        {
            GMM_ASSERTDPF(0, "GmmTexAlloc failed!");
            Status = GMM_ERROR;
            goto ERROR_CASE;
        }
    }

    Status = static_cast<GMM_STATUS>(FALSE == ReAdjustPlaneProperties(FALSE));

ERROR_CASE:
    return (Status == GMM_SUCCESS) ? TRUE: FALSE;
}

BOOLEAN GmmLib::GmmResourceInfoCommon::ReAdjustPlaneProperties(BOOLEAN IsAuxSurf)
{
    const GMM_PLATFORM_INFO* pPlatform = GMM_OVERRIDE_PLATFORM_INFO(&Surf);
    GMM_TEXTURE_INFO* pTexInfo = (IsAuxSurf) ? &AuxSurf : &Surf;
    GMM_TEXTURE_INFO* pPlaneTexInfo = (IsAuxSurf) ? PlaneAuxSurf : PlaneSurf;

    if (GmmIsUVPacked(pTexInfo->Format))
    {
        pPlaneTexInfo[GMM_PLANE_V] = pPlaneTexInfo[GMM_PLANE_U];

        // Need to adjust the returned surfaces and then copy
        // the relivent data into the parent descriptor.
        // UV plane is wider while Y plane is taller,
        // so adjust pitch and sizes to fit accordingly
        pTexInfo->Alignment = pPlaneTexInfo[GMM_PLANE_U].Alignment;
        pTexInfo->Alignment.VAlign = pPlaneTexInfo[GMM_PLANE_Y].Alignment.VAlign;

        if (pPlaneTexInfo[GMM_PLANE_Y].Pitch != pPlaneTexInfo[GMM_PLANE_U].Pitch)
        {
            pPlaneTexInfo[GMM_PLANE_Y].Size = (pPlaneTexInfo[GMM_PLANE_Y].Size / pPlaneTexInfo[GMM_PLANE_Y].Pitch) * pPlaneTexInfo[GMM_PLANE_U].Pitch;
            __GMM_ASSERT(GFX_IS_ALIGNED(pPlaneTexInfo[GMM_PLANE_Y].Size, pPlatform->TileInfo[pPlaneTexInfo[GMM_PLANE_Y].TileMode].LogicalSize));

            if (pPlaneTexInfo[GMM_PLANE_Y].ArraySize > 1)
            {
                pPlaneTexInfo[GMM_PLANE_Y].OffsetInfo.Texture2DOffsetInfo.ArrayQPitchRender =
                    pPlaneTexInfo[GMM_PLANE_Y].OffsetInfo.Texture2DOffsetInfo.ArrayQPitchLock =
                    pPlaneTexInfo[GMM_PLANE_Y].Size / pPlaneTexInfo[GMM_PLANE_Y].ArraySize;
            }

            pTexInfo->Pitch = pPlaneTexInfo[GMM_PLANE_Y].Pitch = pPlaneTexInfo[GMM_PLANE_U].Pitch;
        }

        pTexInfo->OffsetInfo.Plane.ArrayQPitch =
            pPlaneTexInfo[GMM_PLANE_Y].OffsetInfo.Texture2DOffsetInfo.ArrayQPitchRender +
            pPlaneTexInfo[GMM_PLANE_U].OffsetInfo.Texture2DOffsetInfo.ArrayQPitchRender;

        pTexInfo->Size = pPlaneTexInfo[GMM_PLANE_Y].Size + pPlaneTexInfo[GMM_PLANE_U].Size;

        if (pTexInfo->Size > (GMM_GFX_SIZE_T)(pPlatform->SurfaceMaxSize))
        {
            GMM_ASSERTDPF(0, "Surface too large!");
            return FALSE;
        }
    }
    else
    {
        // The parent resource should be the same size as all of the child planes
        __GMM_ASSERT(pTexInfo->Size == (pPlaneTexInfo[GMM_PLANE_Y].Size + 
                               pPlaneTexInfo[GMM_PLANE_U].Size + pPlaneTexInfo[GMM_PLANE_U].Size));
    }

    return TRUE;
}


/////////////////////////////////////////////////////////////////////////////////////
/// Returns width padded to HAlign. Only called for special flags. See asserts in
/// function for which surfaces are supported.
///
/// @param[in]  MipLevel Mip level for which the width is requested
/// @return     Padded Width
/////////////////////////////////////////////////////////////////////////////////////
uint32_t GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetPaddedWidth(uint32_t MipLevel)
{
    GMM_TEXTURE_CALC *pTextureCalc;
    uint32_t AlignedWidth;
    GMM_GFX_SIZE_T MipWidth;
    uint32_t HAlign;

    __GMM_ASSERT(MipLevel <= Surf.MaxLod);

    pTextureCalc = GMM_OVERRIDE_TEXTURE_CALC(&Surf);

    // This shall be called for Depth and Separate Stencil main surface resource
    // This shall be called for the Aux surfaces (MCS, CCS and Hiz) too.
    // MCS will have Surf.Flags.Gpu.CCS set
    // Hiz will have Surf.Flags.Gpu.HiZ set
    __GMM_ASSERT(Surf.Flags.Gpu.Depth || Surf.Flags.Gpu.SeparateStencil ||
                 Surf.Flags.Gpu.CCS || Surf.Flags.Gpu.HiZ ||
                 AuxSurf.Flags.Gpu.__MsaaTileMcs ||
                 AuxSurf.Flags.Gpu.CCS || AuxSurf.Flags.Gpu.__NonMsaaTileYCcs );

    MipWidth = __GmmTexGetMipWidth(&Surf, MipLevel);

    HAlign = Surf.Alignment.HAlign;
    if (AuxSurf.Flags.Gpu.CCS && AuxSurf.Flags.Gpu.__NonMsaaTileYCcs)
    {
        HAlign = AuxSurf.Alignment.HAlign;
    }

    AlignedWidth = __GMM_EXPAND_WIDTH(pTextureCalc,
                                      GFX_ULONG_CAST(MipWidth),
                                      HAlign,
                                      &Surf);

    if (Surf.Flags.Gpu.SeparateStencil)
    {
        if (Surf.Flags.Info.TiledW)
        {
            AlignedWidth *= 2;
        }

        // Reverse MSAA Expansion ////////////////////////////////////////////////
        // It might seem strange that we ExpandWidth (with consideration for MSAA) 
        // only to "reverse" the MSAA portion of the expansion...It's an order-of-
        // operations thing--The intention of the reversal isn't to have 
        // disregarded the original MSAA expansion but to produce a width, that 
        // when MSAA'ed will match the true physical width (which requires MSAA 
        // consideration to compute).
        switch(Surf.MSAA.NumSamples) 
        {
            case 1:  break;
            case 2:  // Same as 4x...
            case 4:  AlignedWidth /= 2; break;
            case 8:  // Same as 16x...
            case 16: AlignedWidth /= 4; break;
            default: __GMM_ASSERT(0);
        }
    }

    // CCS Aux surface, Aligned width needs to be scaled based on main surface bpp
    if (AuxSurf.Flags.Gpu.CCS && AuxSurf.Flags.Gpu.__NonMsaaTileYCcs)
    {
            AlignedWidth = pTextureCalc->ScaleTextureWidth(&AuxSurf, AlignedWidth);
        }

    return AlignedWidth;
}

/////////////////////////////////////////////////////////////////////////////////////
/// Returns height padded to VAlign. Only called for special flags. See asserts in
/// function for which surfaces are supported.
///
/// @param[in]  MipLevel Mip level for which the height is requested
/// @return     Padded height
/////////////////////////////////////////////////////////////////////////////////////
uint32_t GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetPaddedHeight(uint32_t MipLevel)
{
    GMM_TEXTURE_CALC *pTextureCalc;
    uint32_t AlignedHeight, MipHeight;
    uint32_t VAlign;

    __GMM_ASSERT(MipLevel <= Surf.MaxLod);

    // See note in GmmResGetPaddedWidth.
    __GMM_ASSERT(Surf.Flags.Gpu.Depth || Surf.Flags.Gpu.SeparateStencil ||
                 Surf.Flags.Gpu.CCS || Surf.Flags.Gpu.HiZ ||
                 AuxSurf.Flags.Gpu.__MsaaTileMcs ||
                 AuxSurf.Flags.Gpu.CCS || AuxSurf.Flags.Gpu.__NonMsaaTileYCcs);

    pTextureCalc = GMM_OVERRIDE_TEXTURE_CALC(&Surf);

    MipHeight = __GmmTexGetMipHeight(&Surf, MipLevel);

    VAlign = Surf.Alignment.VAlign;
    if (AuxSurf.Flags.Gpu.CCS && AuxSurf.Flags.Gpu.__NonMsaaTileYCcs)
    {
        VAlign = AuxSurf.Alignment.VAlign;
    }

    AlignedHeight = __GMM_EXPAND_HEIGHT(pTextureCalc,
                                        MipHeight,
                                        VAlign,
                                        &Surf);

    if (Surf.Flags.Gpu.SeparateStencil)
    {
        if(Surf.Flags.Info.TiledW)
        {
            AlignedHeight /= 2;
        }

        // Reverse MSAA Expansion ////////////////////////////////////////////////
        // See note in GmmResGetPaddedWidth.
        switch(Surf.MSAA.NumSamples) 
        {
            case 1:  break;
            case 2:  break; // No height adjustment for 2x...
            case 4:  // Same as 8x...
            case 8:  AlignedHeight /= 2; break;
            case 16: AlignedHeight /= 4; break;
            default: __GMM_ASSERT(0);
        }
    }

    // CCS Aux surface, AlignedHeight needs to be scaled by 16
    if (AuxSurf.Flags.Gpu.CCS && AuxSurf.Flags.Gpu.__NonMsaaTileYCcs)
    {
            AlignedHeight = pTextureCalc->ScaleTextureHeight(&AuxSurf, AlignedHeight);
        }

    return AlignedHeight;
}

/////////////////////////////////////////////////////////////////////////////////////
/// Returns pitch padded to VAlign. Only called for special flags. See asserts in
/// function for which surfaces are supported.
///
/// @param[in]  MipLevel Mip level for which the pitch is requested
/// @return     Padded pitch
/////////////////////////////////////////////////////////////////////////////////////
uint32_t GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetPaddedPitch(uint32_t MipLevel)
{
    uint32_t AlignedWidth;
    uint32_t AlignedPitch;
    uint32_t BitsPerPixel;

    __GMM_ASSERT(MipLevel <= Surf.MaxLod);

    // See note in GetPaddedWidth.
    AlignedWidth = GetPaddedWidth(MipLevel);

    BitsPerPixel = Surf.BitsPerPixel;
    if (AuxSurf.Flags.Gpu.CCS && AuxSurf.Flags.Gpu.__NonMsaaTileYCcs)
    {
        BitsPerPixel = 8; //Aux surface are 8bpp
    }

    AlignedPitch = AlignedWidth * BitsPerPixel >> 3;

    return AlignedPitch;
}

/////////////////////////////////////////////////////////////////////////////////////
/// Returns resource's QPitch.
///
/// @return     QPitch
/////////////////////////////////////////////////////////////////////////////////////
uint32_t GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetQPitch()
{
    const GMM_PLATFORM_INFO   *pPlatform;
    uint32_t                      QPitch;

    pPlatform = GMM_OVERRIDE_PLATFORM_INFO(&Surf);

    __GMM_ASSERT(GFX_GET_CURRENT_RENDERCORE(pPlatform->Platform) >= IGFX_GEN8_CORE);
    __GMM_ASSERT((Surf.Type != RESOURCE_3D) ||
                 (GFX_GET_CURRENT_RENDERCORE(pPlatform->Platform) >= IGFX_GEN9_CORE));

    // 2D/CUBE    ==> distance in rows between array slices
    // 3D         ==> distance in rows between R-slices 
    // Compressed ==> one row contains a complete compression block vertically
    // HiZ        ==> 2 * HZ_QPitch
    // Stencil    ==> logical, i.e. not halved
   
    if((GFX_GET_CURRENT_RENDERCORE(pPlatform->Platform) >= IGFX_GEN9_CORE) &&
        GmmIsCompressed(Surf.Format))
    {
        QPitch = Surf.Alignment.QPitch / GetCompressionBlockHeight();

        if((Surf.Type == RESOURCE_3D) && !Surf.Flags.Info.Linear)
        {
            const GMM_TILE_MODE TileMode = Surf.TileMode;
            __GMM_ASSERT(TileMode < GMM_TILE_MODES);
            QPitch = GFX_ALIGN(QPitch, pPlatform->TileInfo[TileMode].LogicalTileHeight);
        }
    }
    else if(Surf.Flags.Gpu.HiZ)
    {
        QPitch = Surf.Alignment.QPitch * 2;
    }
    else
    {
        QPitch = Surf.Alignment.QPitch;
    }

    return QPitch;
}

/////////////////////////////////////////////////////////////////////////////////////
/// Returns offset information to a particular mip map or plane.
///
/// @param[in][out] Has info about which offset client is requesting. Offset is also
///                 passed back to the client in this parameter.
/// @return         ::GMM_STATUS
/////////////////////////////////////////////////////////////////////////////////////
GMM_STATUS GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetOffset(GMM_REQ_OFFSET_INFO &ReqInfo)
{
    if (Surf.Flags.Info.RedecribedPlanes)
    {
        BOOLEAN RestoreReqStdLayout = ReqInfo.ReqStdLayout ? TRUE : FALSE;

        // Lock and Render offsets do not require additional handling
        if (ReqInfo.ReqLock || ReqInfo.ReqRender)
        {
            ReqInfo.ReqStdLayout = FALSE;
            GmmTexGetMipMapOffset(&Surf, &ReqInfo);
            ReqInfo.ReqStdLayout = RestoreReqStdLayout;
        }

        if (ReqInfo.ReqStdLayout)
        {
            GMM_REQ_OFFSET_INFO TempReqInfo[GMM_MAX_PLANE] = { 0 };
            uint32_t Plane, TotalPlanes = GmmLib::Utility::GmmGetNumPlanes(Surf.Format);

            // Caller must specify which plane they need the offset into if not
            // getting the whole surface size
            if (ReqInfo.Plane >= GMM_MAX_PLANE || 
                (ReqInfo.StdLayout.Offset != -1 && !ReqInfo.Plane))
            {
                __GMM_ASSERT(0);
                return GMM_ERROR;
            }

            TempReqInfo[GMM_PLANE_Y] = *&ReqInfo;
            TempReqInfo[GMM_PLANE_Y].Plane = GMM_NO_PLANE;
            TempReqInfo[GMM_PLANE_Y].ReqLock = TempReqInfo[GMM_PLANE_Y].ReqRender = FALSE;

            TempReqInfo[GMM_PLANE_V] = TempReqInfo[GMM_PLANE_U] = TempReqInfo[GMM_PLANE_Y];

            if (GMM_SUCCESS != GmmTexGetMipMapOffset(&PlaneSurf[GMM_PLANE_Y], &TempReqInfo[GMM_PLANE_Y]) ||
                GMM_SUCCESS != GmmTexGetMipMapOffset(&PlaneSurf[GMM_PLANE_U], &TempReqInfo[GMM_PLANE_U]) ||
                GMM_SUCCESS != GmmTexGetMipMapOffset(&PlaneSurf[GMM_PLANE_V], &TempReqInfo[GMM_PLANE_V]))
            {
                __GMM_ASSERT(0);
                return GMM_ERROR;
            }

            ReqInfo.StdLayout.TileDepthPitch = TempReqInfo[ReqInfo.Plane].StdLayout.TileDepthPitch;
            ReqInfo.StdLayout.TileRowPitch = TempReqInfo[ReqInfo.Plane].StdLayout.TileRowPitch;

            if (ReqInfo.StdLayout.Offset == -1)
            {
                // Special request to get the StdLayout size
                ReqInfo.StdLayout.Offset = TempReqInfo[ReqInfo.Plane].StdLayout.Offset;

                if (!ReqInfo.Plane)
                {
                    for (Plane = GMM_PLANE_Y; Plane <= TotalPlanes; Plane++)
                    {
                        ReqInfo.StdLayout.Offset += TempReqInfo[Plane].StdLayout.Offset;
                    }
                }
            }
            else
            {
                ReqInfo.StdLayout.Offset = TempReqInfo[ReqInfo.Plane].StdLayout.Offset;

                for (Plane = GMM_PLANE_Y; Plane < (uint32_t)ReqInfo.Plane; Plane++)
                {
                    // Find the size of the previous planes and add it to the offset
                    TempReqInfo[Plane].StdLayout.Offset = -1;

                    if (GMM_SUCCESS != GmmTexGetMipMapOffset(&PlaneSurf[Plane], &TempReqInfo[Plane]))
                    {
                        __GMM_ASSERT(0);
                        return GMM_ERROR;
                    }

                    ReqInfo.StdLayout.Offset += TempReqInfo[Plane].StdLayout.Offset;
                }
            }
        }

        return GMM_SUCCESS;
    }
    else
    {
        return GmmTexGetMipMapOffset(&Surf, &ReqInfo);
    }    
}

/////////////////////////////////////////////////////////////////////////////////////
/// Performs a CPU BLT between a specified GPU resource and a system memory surface, 
/// as defined by the GMM_RES_COPY_BLT descriptor.
///
/// @param[in]  pBlt: Describes the blit operation. See ::GMM_RES_COPY_BLT for more info.
/// @return     TRUE if succeeded, FALSE otherwise
/////////////////////////////////////////////////////////////////////////////////////
BOOLEAN GMM_STDCALL GmmLib::GmmResourceInfoCommon::CpuBlt(GMM_RES_COPY_BLT *pBlt)
{
    #define REQUIRE(e)          \
        if(!(e))                \
        {                       \
            __GMM_ASSERT(0);    \
            Success = FALSE;    \
            goto EXIT;          \
        }

    const GMM_PLATFORM_INFO *pPlatform;
    BOOLEAN Success = TRUE;
    GMM_TEXTURE_INFO *pTexInfo;
    GMM_TEXTURE_CALC *pTextureCalc;

    __GMM_ASSERTPTR(pBlt, FALSE);

    pPlatform = GMM_OVERRIDE_PLATFORM_INFO(&Surf);
    pTextureCalc = GMM_OVERRIDE_TEXTURE_CALC(&Surf);

    __GMM_ASSERT(
        Surf.Type == RESOURCE_1D || 
        Surf.Type == RESOURCE_2D || 
        Surf.Type == RESOURCE_PRIMARY || 
        Surf.Type == RESOURCE_CUBE || 
        Surf.Type == RESOURCE_3D);
    __GMM_ASSERT(pBlt->Gpu.MipLevel <= Surf.MaxLod);
    __GMM_ASSERT(Surf.MSAA.NumSamples <= 1); // Supported by CpuSwizzleBlt--but not yet this function.
    __GMM_ASSERT(!Surf.Flags.Gpu.Depth || Surf.MSAA.NumSamples <= 1); // MSAA depth currently ends up with a few exchange swizzles--CpuSwizzleBlt could support with expanded XOR'ing, but probably no use case.
    __GMM_ASSERT(!(
        pBlt->Blt.Upload && 
        Surf.Flags.Gpu.Depth && 
        (Surf.BitsPerPixel == 32) && 
        (pBlt->Sys.PixelPitch == 4) && 
        (pBlt->Blt.BytesPerPixel == 3))); // When uploading D24 data from D24S8 to D24X8, no harm in copying S8 to X8 and upload will then be faster.

    pTexInfo = &(Surf);

    // UV packed planar surfaces will have different tiling geometries for the
    // Y and UV planes. Blts cannot span across the tiling boundaries and we
    // must select the proper mode for each plane. Non-UV packed formats will
    // have a constant tiling mode, and so do not have the same limits
    if (Surf.Flags.Info.RedecribedPlanes && 
        GmmIsUVPacked(Surf.Format))
    {
        if (!((pBlt->Gpu.OffsetY >= pTexInfo->OffsetInfo.Plane.Y[GMM_PLANE_U]) ||
            ((pBlt->Gpu.OffsetY + pBlt->Blt.Height) <= pTexInfo->OffsetInfo.Plane.Y[GMM_PLANE_U])))
        {
            __GMM_ASSERT(0);
            return FALSE;
        }

        if (pBlt->Gpu.OffsetY < pTexInfo->OffsetInfo.Plane.Y[GMM_PLANE_U])
        {
            // Y Plane
            pTexInfo = &(PlaneSurf[GMM_PLANE_Y]);
        }
        else
        {
            // UV Plane
            pTexInfo = &(PlaneSurf[GMM_PLANE_U]);
        }
    }

    if(pBlt->Blt.Slices > 1) 
    {
        GMM_RES_COPY_BLT SliceBlt = *pBlt;
        uint32_t Slice;

        SliceBlt.Blt.Slices = 1;
        for(Slice = pBlt->Gpu.Slice; 
            Slice < (pBlt->Gpu.Slice + pBlt->Blt.Slices); 
            Slice++) 
        {
            SliceBlt.Gpu.Slice = Slice;
            SliceBlt.Sys.pData = (void *)((char *) pBlt->Sys.pData + (Slice - pBlt->Gpu.Slice) * pBlt->Sys.SlicePitch);
            SliceBlt.Sys.BufferSize = pBlt->Sys.BufferSize - GFX_ULONG_CAST((char *) SliceBlt.Sys.pData - (char *) pBlt->Sys.pData);
            CpuBlt(&SliceBlt);
        }
    } 
    else // Single Subresource...
    {
        uint32_t ResPixelPitch = pTexInfo->BitsPerPixel / CHAR_BIT;
        uint32_t BlockWidth, BlockHeight, BlockDepth;
        uint32_t __CopyWidthBytes, __CopyHeight, __OffsetXBytes, __OffsetY;
        GMM_REQ_OFFSET_INFO GetOffset = {0};

        pTextureCalc->GetCompressionBlockDimensions(pTexInfo->Format, &BlockWidth, &BlockHeight, &BlockDepth);

        #if(LHDM)
        if( pTexInfo->MsFormat == D3DDDIFMT_G8R8_G8B8 ||
            pTexInfo->MsFormat == D3DDDIFMT_R8G8_B8G8 )
        {
            BlockWidth = 2;
            ResPixelPitch = 4;
        }
        #endif

        { // __CopyWidthBytes...
            uint32_t Width;

            if(!pBlt->Blt.Width) // i.e. "Full Width"
            {
                __GMM_ASSERT(!GmmIsPlanar(pTexInfo->Format)); // Caller must set Blt.Width--GMM "auto-size on zero" not supported with planars since multiple interpretations would confuse more than help.

                Width = GFX_ULONG_CAST(__GmmTexGetMipWidth(pTexInfo, pBlt->Gpu.MipLevel));

                __GMM_ASSERT(Width >= pBlt->Gpu.OffsetX);
                Width -= pBlt->Gpu.OffsetX;
                __GMM_ASSERT(Width);
            } 
            else 
            {
                Width = pBlt->Blt.Width;
            }

            if( ((pBlt->Sys.PixelPitch == 0) || 
                 (pBlt->Sys.PixelPitch == ResPixelPitch)) && 
                ((pBlt->Blt.BytesPerPixel == 0) || 
                 (pBlt->Blt.BytesPerPixel == ResPixelPitch))) 
            {
                // Full-Pixel BLT...
                __CopyWidthBytes = 
                    GFX_CEIL_DIV(Width, BlockWidth) * ResPixelPitch;
            } 
            else // Partial-Pixel BLT...
            {
                __GMM_ASSERT(BlockWidth == 1); // No partial-pixel support for block-compressed formats.

                // When copying between surfaces with different pixel pitches, 
                // specify CopyWidthBytes in terms of unswizzled surface 
                // (convenient convention used by CpuSwizzleBlt).
                __CopyWidthBytes = 
                    Width * 
                    (pBlt->Sys.PixelPitch ? 
                        pBlt->Sys.PixelPitch : 
                        ResPixelPitch);
            }
        }

        { // __CopyHeight...
            if(!pBlt->Blt.Height) // i.e. "Full Height"
            {
                __GMM_ASSERT(!GmmIsPlanar(pTexInfo->Format)); // Caller must set Blt.Height--GMM "auto-size on zero" not supported with planars since multiple interpretations would confuse more than help.

                __CopyHeight = __GmmTexGetMipHeight(pTexInfo, pBlt->Gpu.MipLevel);
                __GMM_ASSERT(__CopyHeight >= pBlt->Gpu.OffsetY);
                __CopyHeight -= pBlt->Gpu.OffsetY;
                __GMM_ASSERT(__CopyHeight);
            } 
            else 
            {
                __CopyHeight = pBlt->Blt.Height;
            }

            __CopyHeight = GFX_CEIL_DIV(__CopyHeight, BlockHeight);
        }

        __GMM_ASSERT((pBlt->Gpu.OffsetX % BlockWidth) == 0);
        __OffsetXBytes = (pBlt->Gpu.OffsetX / BlockWidth) * ResPixelPitch + pBlt->Gpu.OffsetSubpixel;

        __GMM_ASSERT((pBlt->Gpu.OffsetY % BlockHeight) == 0);
        __OffsetY = (pBlt->Gpu.OffsetY / BlockHeight);

        { // Get pResData Offsets to this subresource...
            GetOffset.ReqLock = pTexInfo->Flags.Info.Linear;
            GetOffset.ReqStdLayout = !GetOffset.ReqLock && pTexInfo->Flags.Info.StdSwizzle;
            GetOffset.ReqRender = !GetOffset.ReqLock && !GetOffset.ReqStdLayout;
            GetOffset.MipLevel = pBlt->Gpu.MipLevel;
            switch(pTexInfo->Type) 
            {
                case RESOURCE_1D:
                case RESOURCE_2D:
                case RESOURCE_PRIMARY:
                {
                    GetOffset.ArrayIndex = pBlt->Gpu.Slice;
                    break;
                }
                case RESOURCE_CUBE:
                {
                    GetOffset.ArrayIndex = pBlt->Gpu.Slice / 6;
                    GetOffset.CubeFace = (GMM_CUBE_FACE_ENUM)(pBlt->Gpu.Slice % 6);
                    break;
                }
                case RESOURCE_3D:
                {
                    GetOffset.Slice = (pTexInfo->Flags.Info.TiledYs || pTexInfo->Flags.Info.TiledYf) ? 
                                        (pBlt->Gpu.Slice / pPlatform->TileInfo[pTexInfo->TileMode].LogicalTileDepth) : 
                                        pBlt->Gpu.Slice;
                    break;
                }
                default: 
                    __GMM_ASSERT(0);
            }

            REQUIRE(this->GetOffset(GetOffset) == GMM_SUCCESS);
        }

        if(pTexInfo->Flags.Info.Linear) 
        {
            char *pDest, *pSrc;
            uint32_t DestPitch, SrcPitch;
            uint32_t y;

            __GMM_ASSERT( // Linear-to-linear subpixel BLT unexpected--Not implemented.
                (!pBlt->Sys.PixelPitch || (pBlt->Sys.PixelPitch == ResPixelPitch)) && 
                (!pBlt->Blt.BytesPerPixel || (pBlt->Blt.BytesPerPixel == ResPixelPitch)));

            if(pBlt->Blt.Upload) 
            {
                pDest = (char *) pBlt->Gpu.pData;
                DestPitch = GFX_ULONG_CAST(pTexInfo->Pitch);

                pSrc = (char *) pBlt->Sys.pData;
                SrcPitch = pBlt->Sys.RowPitch;
            } 
            else 
            {
                pDest = (char *) pBlt->Sys.pData;
                DestPitch = pBlt->Sys.RowPitch;

                pSrc = (char *) pBlt->Gpu.pData;
                SrcPitch = GFX_ULONG_CAST(pTexInfo->Pitch);
            }

            __GMM_ASSERT(GetOffset.Lock.Offset < pTexInfo->Size);
            pDest += GetOffset.Lock.Offset + (__OffsetY * DestPitch + __OffsetXBytes);

            for(y = 0; y < __CopyHeight; y++) 
            {
                // Memcpy per row isn't optimal, but doubt this linear-to-linear path matters.

                #if _WIN32
                    #ifdef __GMM_KMD__
                        GFX_MEMCPY_S
                    #else
                        memcpy_s
                    #endif
                        (pDest, __CopyWidthBytes, pSrc, __CopyWidthBytes);
                #else
                    memcpy(pDest, pSrc, __CopyWidthBytes);
                #endif
                pDest += DestPitch;
                pSrc += SrcPitch;
            }
        } 
        else // Swizzled BLT...
        {
            CPU_SWIZZLE_BLT_SURFACE LinearSurface = {0}, SwizzledSurface;
           uint32_t ZOffset = 0;

            __GMM_ASSERT(GetOffset.Render.Offset64 < pTexInfo->Size);
            
            ZOffset = (pTexInfo->Type == RESOURCE_3D &&
                        (pTexInfo->Flags.Info.TiledYs || pTexInfo->Flags.Info.TiledYf)) ?
                      (pBlt->Gpu.Slice % pPlatform->TileInfo[pTexInfo->TileMode].LogicalTileDepth) : 0;
            
            if( pTexInfo->Flags.Info.StdSwizzle == TRUE )
            {
                SwizzledSurface.pBase = (char *)pBlt->Gpu.pData + GFX_ULONG_CAST( GetOffset.StdLayout.Offset );
                SwizzledSurface.OffsetX = __OffsetXBytes;
                SwizzledSurface.OffsetY = __OffsetY;
                SwizzledSurface.OffsetZ = ZOffset;

                uint32_t MipWidth = GFX_ULONG_CAST( __GmmTexGetMipWidth( pTexInfo, pBlt->Gpu.MipLevel ) );
                uint32_t MipHeight = __GmmTexGetMipHeight( pTexInfo, pBlt->Gpu.MipLevel );

                pTextureCalc->AlignTexHeightWidth(pTexInfo, &MipHeight, &MipWidth);
                SwizzledSurface.Height = MipHeight;
                SwizzledSurface.Pitch = MipWidth * ResPixelPitch;
            }
            else
            {
                SwizzledSurface.pBase = (char *)pBlt->Gpu.pData + GFX_ULONG_CAST( GetOffset.Render.Offset64 );
                SwizzledSurface.Pitch = GFX_ULONG_CAST( pTexInfo->Pitch );
                SwizzledSurface.OffsetX = GetOffset.Render.XOffset + __OffsetXBytes;
                SwizzledSurface.OffsetY = GetOffset.Render.YOffset + __OffsetY;
                SwizzledSurface.OffsetZ = GetOffset.Render.ZOffset + ZOffset;
                SwizzledSurface.Height = GFX_ULONG_CAST( pTexInfo->Size / pTexInfo->Pitch );
            }

            SwizzledSurface.Element.Pitch = ResPixelPitch;

            LinearSurface.pBase = pBlt->Sys.pData;
            LinearSurface.Pitch = pBlt->Sys.RowPitch;
            LinearSurface.Height = 
                pBlt->Sys.BufferSize / 
                (pBlt->Sys.RowPitch ? 
                    pBlt->Sys.RowPitch : 
                    pBlt->Sys.BufferSize);
            LinearSurface.Element.Pitch = 
                pBlt->Sys.PixelPitch ? 
                    pBlt->Sys.PixelPitch : 
                    ResPixelPitch;
            LinearSurface.Element.Size = 
                SwizzledSurface.Element.Size = 
                    pBlt->Blt.BytesPerPixel ? 
                        pBlt->Blt.BytesPerPixel : 
                        ResPixelPitch;

            SwizzledSurface.pSwizzle = NULL;

            if(     pTexInfo->Flags.Info.TiledW )
            {
                SwizzledSurface.pSwizzle = &INTEL_TILE_W;

                // Correct for GMM's 2x Pitch handling of stencil...
                // (Unlike the HW, CpuSwizzleBlt handles TileW as a natural, 
                // 64x64=4KB tile, so the pre-Gen10 "double-pitch/half-height" 
                // kludging to TileY shape must be reversed.)
                __GMM_ASSERT((SwizzledSurface.Pitch % 2) == 0);
                SwizzledSurface.Pitch /= 2;
                SwizzledSurface.Height *= 2;

                __GMM_ASSERT((GetOffset.Render.XOffset % 2) == 0);
                SwizzledSurface.OffsetX = GetOffset.Render.XOffset / 2 + __OffsetXBytes;
                SwizzledSurface.OffsetY = GetOffset.Render.YOffset * 2 + __OffsetY;
            } 
            else if( pTexInfo->Flags.Info.TiledY && 
                     !(pTexInfo->Flags.Info.TiledYs ||
                       pTexInfo->Flags.Info.TiledYf ))
            {
                SwizzledSurface.pSwizzle = &INTEL_TILE_Y;
            } 
            else if(pTexInfo->Flags.Info.TiledX) 
            {
                SwizzledSurface.pSwizzle = &INTEL_TILE_X;
            } 
            else // Yf/s...
            {
                #define NA

                #define CASE(xD, msaa, kb, bpe)                         \
                    case bpe: SwizzledSurface.pSwizzle = &ST_##xD##_##msaa##kb##_##bpe##bpp; break

                #define SWITCH_BPP(xD, msaa, kb)                        \
                    switch(pTexInfo->BitsPerPixel)             \
                    {                                                   \
                        CASE(xD, msaa, kb, 8);                          \
                        CASE(xD, msaa, kb, 16);                         \
                        CASE(xD, msaa, kb, 32);                         \
                        CASE(xD, msaa, kb, 64);                         \
                        CASE(xD, msaa, kb, 128);                        \
                    }

                #define SWITCH_MSAA(xD, kb)                             \
                    switch(pTexInfo->MSAA.NumSamples)          \
                    {                                                   \
                        case 0:  SWITCH_BPP(xD,        , kb); break;    \
                        case 1:  SWITCH_BPP(xD,        , kb); break;    \
                        case 2:  SWITCH_BPP(xD, MSAA2_ , kb); break;    \
                        case 4:  SWITCH_BPP(xD, MSAA4_ , kb); break;    \
                        case 8:  SWITCH_BPP(xD, MSAA8_ , kb); break;    \
                        case 16: SWITCH_BPP(xD, MSAA16_, kb); break;    \
                    }

                if(pTexInfo->Type == RESOURCE_3D) 
                {
                    if(pTexInfo->Flags.Info.TiledYf) 
                    {
                        SWITCH_BPP(3D, , 4KB);
                    } 
                    else if(pTexInfo->Flags.Info.TiledYs) 
                    {
                        SWITCH_BPP(3D, , 64KB);
                    } 
                } 
                else // 2D/Cube...
                {
                    if(pTexInfo->Flags.Info.TiledYf) 
                    {
                        SWITCH_MSAA(2D, 4KB);
                    } 
                    else if(pTexInfo->Flags.Info.TiledYs) 
                    {
                        SWITCH_MSAA(2D, 64KB);
                    } 
                }
            }
            __GMM_ASSERT(SwizzledSurface.pSwizzle);

            if(pBlt->Blt.Upload) 
            {
                CpuSwizzleBlt(&SwizzledSurface, &LinearSurface, __CopyWidthBytes, __CopyHeight);
            } 
            else 
            {
                CpuSwizzleBlt(&LinearSurface, &SwizzledSurface, __CopyWidthBytes, __CopyHeight);
            }
        }
    }

EXIT:

    return Success;
}

/////////////////////////////////////////////////////////////////////////////////////
/// Helper function that helps UMDs map in the surface in a layout that
/// our HW understands. Clients call this function in a loop until it
/// returns failure. Clients will get back information in pMapping->Span, 
/// which they can use to map Span.Size bytes to Span.VirtualOffset gfx 
/// address with Span.PhysicalOffset physical page.
///
/// @param[in]  pMapping: Clients call the function with initially zero'd out GMM_GET_MAPPING.
/// @return      TRUE if more span descriptors to report, FALSE if all mapping is done
/////////////////////////////////////////////////////////////////////////////////////
BOOLEAN GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetMappingSpanDesc(GMM_GET_MAPPING *pMapping)
{
    const GMM_PLATFORM_INFO *pPlatform;
    BOOLEAN WasFinalSpan = FALSE;
    GMM_TEXTURE_INFO *pTexInfo;
    GMM_TEXTURE_CALC *pTextureCalc;

    __GMM_ASSERT(Surf.Flags.Info.StdSwizzle);

    pPlatform = GMM_OVERRIDE_PLATFORM_INFO(&Surf);
    pTextureCalc = GMM_OVERRIDE_TEXTURE_CALC(&Surf);

    pTexInfo = &Surf;

    if(pMapping->Type == GMM_MAPPING_GEN9_YS_TO_STDSWIZZLE) 
    {
        const uint32_t TileSize = GMM_KBYTE(64);

        __GMM_ASSERT(Surf.Flags.Info.TiledYs);
        __GMM_ASSERT(
            (Surf.Type == RESOURCE_2D) || 
            (Surf.Type == RESOURCE_3D) || 
            (Surf.Type == RESOURCE_CUBE));
        __GMM_ASSERT(Surf.Flags.Gpu.Depth == 0); // TODO(Minor): Proper StdSwizzle exemptions?
        __GMM_ASSERT(Surf.Flags.Gpu.SeparateStencil == 0);

        __GMM_ASSERT(AuxSurf.Size == 0); // TODO(Medium): Support not yet implemented, but DX12 UMD not using yet.
        __GMM_ASSERT(Surf.Flags.Gpu.MMC == 0); // TODO(Medium): Support not yet implemented, but not yet needed for DX12.

        // For planar surfaces we need to reorder the planes into what HW expects.
        // OS will provide planes in [Y0][Y1][U0][U1][V0][V1] order while
        // HW requires them to be in [Y0][U0][V0][Y1][U1][V1] order
        if (Surf.Flags.Info.RedecribedPlanes)
        {
            if (pMapping->Scratch.Plane == GMM_NO_PLANE)
            {
                pMapping->Scratch.Plane = GMM_PLANE_Y;
                if (GmmLib::Utility::GmmGetNumPlanes(Surf.Format) == GMM_PLANE_V)
                {
                    pMapping->Scratch.LastPlane = GMM_PLANE_V;
                }
                else
                {
                    pMapping->Scratch.LastPlane = GMM_PLANE_U;
                }
            }
            else if (pMapping->Scratch.Row == pMapping->Scratch.Rows)
            {
                // If we've crossed into a new plane then need to reset
                // the current mapping info and adjust the mapping
                // params accordingly
                GMM_REQ_OFFSET_INFO ReqInfo = { 0 };
                uint32_t Plane = pMapping->Scratch.Plane + 1;
                GMM_YUV_PLANE LastPlane = pMapping->Scratch.LastPlane;

                memset(pMapping, 0, sizeof(*pMapping));

                pMapping->Type = GMM_MAPPING_GEN9_YS_TO_STDSWIZZLE;
                pMapping->Scratch.Plane = GMM_YUV_PLANE(Plane);
                pMapping->Scratch.LastPlane = LastPlane;

                ReqInfo.ReqRender = ReqInfo.ReqStdLayout = TRUE;
                ReqInfo.Plane = GMM_YUV_PLANE(Plane);

                this->GetOffset(ReqInfo);

                pMapping->__NextSpan.PhysicalOffset = ReqInfo.StdLayout.Offset;
                pMapping->__NextSpan.VirtualOffset = ReqInfo.Render.Offset64;
            }

            pTexInfo = &PlaneSurf[pMapping->Scratch.Plane];
        }
                
        // Initialization of Mapping Params...
        if(pMapping->Scratch.Element.Width == 0) // i.e. initially zero'ed struct.
        {
            uint32_t BytesPerElement = pTexInfo->BitsPerPixel / CHAR_BIT;

            pMapping->Scratch.EffectiveLodMax = GFX_MIN(pTexInfo->MaxLod, pTexInfo->Alignment.MipTailStartLod);

            pTextureCalc->GetCompressionBlockDimensions(
                pTexInfo->Format,
                &pMapping->Scratch.Element.Width, 
                &pMapping->Scratch.Element.Height, 
                &pMapping->Scratch.Element.Depth);

            { // Tile Dimensions...
                GMM_TILE_MODE TileMode = pTexInfo->TileMode;
                __GMM_ASSERT(TileMode < GMM_TILE_MODES);

                // Get Tile Logical Tile Dimensions (i.e. uncompressed pixels)...
                pMapping->Scratch.Tile.Width = 
                    (pPlatform->TileInfo[TileMode].LogicalTileWidth / BytesPerElement) * 
                    pMapping->Scratch.Element.Width;

                pMapping->Scratch.Tile.Height = 
                    pPlatform->TileInfo[TileMode].LogicalTileHeight * 
                    pMapping->Scratch.Element.Height;

                pMapping->Scratch.Tile.Depth = 
                    pPlatform->TileInfo[TileMode].LogicalTileDepth * 
                    pMapping->Scratch.Element.Depth;

                pMapping->Scratch.RowPitchVirtual = 
                    GFX_ULONG_CAST(pTexInfo->Pitch) *
                    pPlatform->TileInfo[TileMode].LogicalTileHeight * 
                    pPlatform->TileInfo[TileMode].LogicalTileDepth;
            }

            { // Slice...
                uint32_t Lod;
                uint32_t LodsPerSlice = 
                    (pTexInfo->Type != RESOURCE_3D) ?
                        pMapping->Scratch.EffectiveLodMax + 1 :
                        1; // 3D Std Swizzle traverses slices before MIP's.

                if (pMapping->Scratch.Plane)
                {
                    // If planar then we need the parent descriptors planar pitch
                    pMapping->Scratch.SlicePitch.Virtual =
                        GFX_ULONG_CAST(Surf.OffsetInfo.Plane.ArrayQPitch) *
                        (pMapping->Scratch.Tile.Depth / pMapping->Scratch.Element.Depth);
                }
                else
                {
                    pMapping->Scratch.SlicePitch.Virtual =
                        GFX_ULONG_CAST(pTexInfo->OffsetInfo.Texture2DOffsetInfo.ArrayQPitchRender) *
                        (pMapping->Scratch.Tile.Depth / pMapping->Scratch.Element.Depth);
                }

                // SlicePitch.Physical...
                __GMM_ASSERT(pMapping->Scratch.SlicePitch.Physical == 0);
                for(Lod = 0; Lod < LodsPerSlice; Lod++) 
                {
                    uint32_t MipCols, MipRows;
                    GMM_GFX_SIZE_T MipWidth;
                    uint32_t MipHeight;

                    MipWidth = __GmmTexGetMipWidth(pTexInfo, Lod);
                    MipHeight = __GmmTexGetMipHeight(pTexInfo, Lod);

                    MipCols = GFX_ULONG_CAST(
                        GFX_CEIL_DIV(
                            MipWidth,
                            pMapping->Scratch.Tile.Width));
                    MipRows = 
                        GFX_CEIL_DIV(
                            MipHeight,
                            pMapping->Scratch.Tile.Height);

                    pMapping->Scratch.SlicePitch.Physical += 
                        MipCols * MipRows * TileSize;
                }
            }

            { // Mip0...
                if (pTexInfo->Type != RESOURCE_3D)
                {
                    pMapping->Scratch.Slices = 
                        GFX_MAX(pTexInfo->ArraySize, 1) *
                        ((pTexInfo->Type == RESOURCE_CUBE) ? 6 : 1);
                } 
                else 
                {
                    pMapping->Scratch.Slices = 
                        GFX_CEIL_DIV(pTexInfo->Depth, pMapping->Scratch.Tile.Depth);
                }

                if (pTexInfo->Pitch ==
                        (GFX_ALIGN(pTexInfo->BaseWidth, pMapping->Scratch.Tile.Width) /
                         pMapping->Scratch.Element.Width * BytesPerElement)) 
                {
                    // Treat Each LOD0 MIP as Single, Large Mapping Row...
                    pMapping->Scratch.Rows = 1;

                    pMapping->__NextSpan.Size = 
                        GFX_CEIL_DIV(pTexInfo->BaseWidth, pMapping->Scratch.Tile.Width) *
                        GFX_CEIL_DIV(pTexInfo->BaseHeight, pMapping->Scratch.Tile.Height) *
                        TileSize;
                } 
                else 
                {
                    pMapping->Scratch.Rows = 
                        GFX_CEIL_DIV(pTexInfo->BaseHeight, pMapping->Scratch.Tile.Height);

                    pMapping->__NextSpan.Size = 
                        GFX_CEIL_DIV(pTexInfo->BaseWidth, pMapping->Scratch.Tile.Width) *
                        TileSize;
                }
            }           
        }

        // This iteration's span descriptor...
        pMapping->Span = pMapping->__NextSpan;

        // Prepare for Next Iteration...
        //  for(Lod = 0; Lod <= EffectiveLodMax; Lod += 1) 
        //  for(Row = 0; Row < Rows; Row += 1) 
        //  for(Slice = 0; Slice < Slices; Slice += 1) 
        if((pMapping->Scratch.Slice += 1) < pMapping->Scratch.Slices) 
        {
            pMapping->__NextSpan.PhysicalOffset += pMapping->Scratch.SlicePitch.Physical;
            pMapping->__NextSpan.VirtualOffset += pMapping->Scratch.SlicePitch.Virtual;
        } 
        else 
        {
            pMapping->Scratch.Slice = 0;

            if((pMapping->Scratch.Row += 1) < pMapping->Scratch.Rows) 
            {
                pMapping->__NextSpan.PhysicalOffset = 
                    pMapping->Scratch.Slice0MipOffset.Physical += pMapping->Span.Size;

                pMapping->__NextSpan.VirtualOffset = 
                    pMapping->Scratch.Slice0MipOffset.Virtual += pMapping->Scratch.RowPitchVirtual;
            } 
            else if((pMapping->Scratch.Lod += 1) <= pMapping->Scratch.EffectiveLodMax) 
            {
                GMM_REQ_OFFSET_INFO GetOffset = {0};
                GMM_GFX_SIZE_T MipWidth;
                uint32_t MipHeight, MipCols;

                MipWidth = __GmmTexGetMipWidth(pTexInfo, pMapping->Scratch.Lod);
                MipHeight = __GmmTexGetMipHeight(pTexInfo, pMapping->Scratch.Lod);

                MipCols = GFX_ULONG_CAST(
                    GFX_CEIL_DIV(
                        MipWidth,
                        pMapping->Scratch.Tile.Width));

                pMapping->Scratch.Row = 0;
                pMapping->Scratch.Rows = 
                    GFX_CEIL_DIV(
                        MipHeight,
                        pMapping->Scratch.Tile.Height);

                if (pTexInfo->Type != RESOURCE_3D)
                {
                    pMapping->__NextSpan.PhysicalOffset = 
                        pMapping->Scratch.Slice0MipOffset.Physical += pMapping->Span.Size;
                } 
                else 
                {
                    uint32_t MipDepth;

                    MipDepth = __GmmTexGetMipDepth(pTexInfo, pMapping->Scratch.Lod);

                    // 3D Std Swizzle traverses slices before MIP's...
                    pMapping->Scratch.Slice0MipOffset.Physical = 
                        pMapping->__NextSpan.PhysicalOffset += pMapping->Span.Size;

                    pMapping->Scratch.Slices = 
                        GFX_CEIL_DIV(
                            MipDepth,
                            pMapping->Scratch.Tile.Depth);

                    pMapping->Scratch.SlicePitch.Physical = 
                        MipCols * pMapping->Scratch.Rows * TileSize;
                }

                GetOffset.ReqRender = TRUE;
                GetOffset.MipLevel = pMapping->Scratch.Lod;
                this->GetOffset(GetOffset);

                pMapping->__NextSpan.VirtualOffset = 
                    pMapping->Scratch.Slice0MipOffset.Virtual = 
                        GFX_ALIGN_FLOOR(GetOffset.Render.Offset64, TileSize); // Truncate for packed MIP Tail.

                pMapping->__NextSpan.Size = MipCols * TileSize;
            } 
            else 
            {
                // If the resource was a planar surface then need to iterate over the remaining planes
                WasFinalSpan = pMapping->Scratch.Plane == pMapping->Scratch.LastPlane;
                //GMM_DPF( GFXDBG_CRITICAL, "\n" );   // TODO (rmadayik): Remove when done testing.
            }
        }
    } 
    else 
    {
        __GMM_ASSERT(0);
    }

    return !WasFinalSpan;
}

//=============================================================================
//
// Function: GetTiledResourceMipPacking
//
// Desc: Get number of packed mips and total #tiles for packed mips
//
// Parameters:
//      See function arguments.
//
// Returns:
//      VOID
//-----------------------------------------------------------------------------
void GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetTiledResourceMipPacking(UINT              *pNumPackedMips,
                                               UINT              *pNumTilesForPackedMips)
{
    if (GetMaxLod()== 0)
    {
        *pNumPackedMips         = 0;
        *pNumTilesForPackedMips = 0;
        return;
    }

    if (GetResFlags().Info.TiledYs ||
        GetResFlags().Info.TiledYf)
    {
        if (Surf.Alignment.MipTailStartLod == GMM_TILED_RESOURCE_NO_MIP_TAIL)
        {
            *pNumPackedMips         = 0;
            *pNumTilesForPackedMips = 0;
        }
        else
        {
            *pNumPackedMips         = GetMaxLod()-
                Surf.Alignment.MipTailStartLod + 1;
            *pNumTilesForPackedMips = 1;
        }
    }
    else
    {
        // Error, unsupported format.
        __GMM_ASSERT(FALSE);
    }
}

//=============================================================================
//
// Function: GetPackedMipTailStartLod
//
// Desc: Get Lod of first packed Mip.
//
// Parameters:
//      See function arguments.
//
// Returns:
//      Lod of first packed Mip
//-----------------------------------------------------------------------------
uint32_t GMM_STDCALL GmmLib::GmmResourceInfoCommon::GetPackedMipTailStartLod()

{
    UINT NumPackedMips = 0, NumTilesForPackedMips= 0;

    const GMM_PLATFORM_INFO* pPlatform = GMM_OVERRIDE_PLATFORM_INFO(&Surf);

    GetTiledResourceMipPacking(&NumPackedMips,
                                  &NumTilesForPackedMips);

    return  (GetMaxLod() == 0) ?
             pPlatform->MaxLod :
             GetMaxLod() - NumPackedMips + 1; //GetMaxLod srarts at index 0, while NumPackedMips is just
                                                              //the number of mips. So + 1 to bring them to same units.
}
