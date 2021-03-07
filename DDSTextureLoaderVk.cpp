//--------------------------------------------------------------------------------------
// File: DDSTextureLoader12.cpp
//
// Functions for loading a DDS texture and creating a Vulkan runtime resource for it
//
// Basically a ripoff of Microsoft's DDSTextureLoader12: https://github.com/microsoft/DirectXTex/tree/master/DDSTextureLoader
// Licensed under the MIT License.
//
//--------------------------------------------------------------------------------------

#include "DDSTextureLoaderVk.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <new>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#include <debugapi.h>
#else
#include <cstdio>
#endif

#ifdef __clang__
#pragma clang diagnostic ignored "-Wtautological-type-limit-compare"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

#pragma warning(disable : 4062)

using namespace DDSTextureLoaderVk;

//--------------------------------------------------------------------------------------
// Macros
//--------------------------------------------------------------------------------------
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
                ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |       \
                ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif /* defined(MAKEFOURCC) */

//--------------------------------------------------------------------------------------
// DDS file structure definitions
//
// See DDS.h in the 'Texconv' sample and the 'DirectXTex' library
//--------------------------------------------------------------------------------------
#pragma pack(push,1)

const uint32_t DDS_MAGIC = 0x20534444; // "DDS "

struct DDS_PIXELFORMAT
{
    uint32_t    size;
    uint32_t    flags;
    uint32_t    fourCC;
    uint32_t    RGBBitCount;
    uint32_t    RBitMask;
    uint32_t    GBitMask;
    uint32_t    BBitMask;
    uint32_t    ABitMask;
};

#define DDS_FOURCC        0x00000004  // DDPF_FOURCC
#define DDS_RGB           0x00000040  // DDPF_RGB
#define DDS_LUMINANCE     0x00020000  // DDPF_LUMINANCE
#define DDS_BUMPDUDV      0x00080000  // DDPF_BUMPDUDV

#define DDS_HEADER_FLAGS_VOLUME         0x00800000  // DDSD_DEPTH

#define DDS_HEIGHT 0x00000002 // DDSD_HEIGHT

#define DDS_CUBEMAP_POSITIVEX 0x00000600 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEX
#define DDS_CUBEMAP_NEGATIVEX 0x00000a00 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEX
#define DDS_CUBEMAP_POSITIVEY 0x00001200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEY
#define DDS_CUBEMAP_NEGATIVEY 0x00002200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEY
#define DDS_CUBEMAP_POSITIVEZ 0x00004200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEZ
#define DDS_CUBEMAP_NEGATIVEZ 0x00008200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEZ

#define DDS_CUBEMAP_ALLFACES ( DDS_CUBEMAP_POSITIVEX | DDS_CUBEMAP_NEGATIVEX |\
                               DDS_CUBEMAP_POSITIVEY | DDS_CUBEMAP_NEGATIVEY |\
                               DDS_CUBEMAP_POSITIVEZ | DDS_CUBEMAP_NEGATIVEZ )

#define DDS_CUBEMAP 0x00000200 // DDSCAPS2_CUBEMAP

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (P)
#endif

enum DDS_MISC_FLAGS2
{
    DDS_MISC_FLAGS2_ALPHA_MODE_MASK = 0x7L,
};

struct DDS_HEADER
{
    uint32_t        size;
    uint32_t        flags;
    uint32_t        height;
    uint32_t        width;
    uint32_t        pitchOrLinearSize;
    uint32_t        depth; // only if DDS_HEADER_FLAGS_VOLUME is set in flags
    uint32_t        mipMapCount;
    uint32_t        reserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t        caps;
    uint32_t        caps2;
    uint32_t        caps3;
    uint32_t        caps4;
    uint32_t        reserved2;
};

struct DDS_HEADER_DXT10
{
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag; // see D3D11_RESOURCE_MISC_FLAG (probably doesn't matter for Vulkan)
    uint32_t arraySize;
    uint32_t miscFlags2;
};

#pragma pack(pop)

//--------------------------------------------------------------------------------------
namespace
{
#ifdef VK_NO_PROTOTYPES

    PFN_vkCreateImage vkCreateImage = nullptr;

    DDSTextureLoaderVk::PFN_DdsLoader_vkCreateImageUserPtr vkCreateImageWithUserPtr = nullptr;
    void*                                                  vkCreateImageUserPtr     = nullptr;

#ifdef VK_EXT_debug_utils

    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;

    DDSTextureLoaderVk::PFN_DdsLoader_vkSetDebugUtilsObjectNameUserPtr vkSetDebugUtilsObjectNameEXTWithUserPtr = nullptr;
    void*                                                              vkSetDebugUtilsObjectNameEXTUserPtr     = nullptr;

#endif //VK_EXT_debug_utils
#endif // VK_NO_PROTOTYPES

    template<uint32_t TNameLength>
    inline void SetDebugObjectName(VkDevice device, VkImage image, const char(&name)[TNameLength]) noexcept
    {
        #if defined(VK_EXT_debug_utils) && !defined(NO_D3D12_DEBUG_NAME) && ( defined(_DEBUG) || defined(PROFILE) )
            VkDebugUtilsObjectNameInfoEXT debugObjectNameInfo;   
            debugObjectNameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
            debugObjectNameInfo.pNext        = nullptr;
            debugObjectNameInfo.objectType   = VK_OBJECT_TYPE_IMAGE;
            debugObjectNameInfo.objectHandle = (uint64_t)image;
            debugObjectNameInfo.pObjectName  = name;

            if(vkSetDebugUtilsObjectNameEXT != nullptr)
            {
                vkSetDebugUtilsObjectNameEXT(device, &debugObjectNameInfo);
            }
        #else
            UNREFERENCED_PARAMETER(device);
            UNREFERENCED_PARAMETER(image);
            UNREFERENCED_PARAMETER(name);
        #endif
    }

    inline uint32_t CountMips(uint32_t width, uint32_t height) noexcept
    {
        if (width == 0 || height == 0)
            return 0;

        uint32_t count = 1;
        while (width > 1 || height > 1)
        {
            width >>= 1;
            height >>= 1;
            count++;
        }
        return count;
    }


    //--------------------------------------------------------------------------------------
    DDS_LOADER_RESULT LoadTextureDataFromMemory(
        const uint8_t* ddsData,
        size_t ddsDataSize,
        const DDS_HEADER** header,
        const uint8_t** bitData,
        size_t* bitSize) noexcept
    {
        if (!header || !bitData || !bitSize)
        {
            return DDS_LOADER_BAD_POINTER;
        }

        *bitSize = 0;

        if (ddsDataSize > UINT32_MAX)
        {
            return DDS_LOADER_FAIL;
        }

        if (ddsDataSize < (sizeof(uint32_t) + sizeof(DDS_HEADER)))
        {
            return DDS_LOADER_FAIL;
        }

        // DDS files always start with the same magic number ("DDS ")
        auto dwMagicNumber = *reinterpret_cast<const uint32_t*>(ddsData);
        if (dwMagicNumber != DDS_MAGIC)
        {
            return DDS_LOADER_FAIL;
        }

        auto hdr = reinterpret_cast<const DDS_HEADER*>(ddsData + sizeof(uint32_t));

        // Verify header to validate DDS file
        if (hdr->size != sizeof(DDS_HEADER) ||
            hdr->ddspf.size != sizeof(DDS_PIXELFORMAT))
        {
            return DDS_LOADER_FAIL;
        }

        // Check for DX10 extension
        bool bDXT10Header = false;
        if ((hdr->ddspf.flags & DDS_FOURCC) &&
            (MAKEFOURCC('D', 'X', '1', '0') == hdr->ddspf.fourCC))
        {
            // Must be long enough for both headers and magic value
            if (ddsDataSize < (sizeof(uint32_t) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10)))
            {
                return DDS_LOADER_FAIL;
            }

            bDXT10Header = true;
        }

        // setup the pointers in the process request
        *header = hdr;
        auto offset = sizeof(uint32_t)
            + sizeof(DDS_HEADER)
            + (bDXT10Header ? sizeof(DDS_HEADER_DXT10) : 0);
        *bitData = ddsData + offset;
        *bitSize = ddsDataSize - offset;

        return DDS_LOADER_SUCCESS;
    }


    //--------------------------------------------------------------------------------------
    DDS_LOADER_RESULT LoadTextureDataFromFile(
        const char_type* fileName,
        std::unique_ptr<uint8_t[]>& ddsData,
        const DDS_HEADER** header,
        const uint8_t** bitData,
        size_t* bitSize) noexcept
    {
        if (!header || !bitData || !bitSize)
        {
            return DDS_LOADER_BAD_POINTER;
        }

        *bitSize = 0;

        std::ifstream inFile(std::filesystem::path(fileName), std::ios::in | std::ios::binary | std::ios::ate);
        if (!inFile)
            return DDS_LOADER_FAIL;

        std::streampos fileLen = inFile.tellg();
        if (!inFile)
            return DDS_LOADER_FAIL;

        // Need at least enough data to fill the header and magic number to be a valid DDS
        if (fileLen < (sizeof(uint32_t) + sizeof(DDS_HEADER)))
            return DDS_LOADER_FAIL;

        ddsData.reset(new (std::nothrow) uint8_t[size_t(fileLen)]);
        if (!ddsData)
            return DDS_LOADER_FAIL;

        inFile.seekg(0, std::ios::beg);
        if (!inFile)
        {
            ddsData.reset();
            return DDS_LOADER_FAIL;
        }

       inFile.read(reinterpret_cast<char*>(ddsData.get()), fileLen);
       if (!inFile)
       {
           ddsData.reset();
           return DDS_LOADER_FAIL;
       }

       inFile.close();

       size_t len = fileLen;

        // DDS files always start with the same magic number ("DDS ")
        auto dwMagicNumber = *reinterpret_cast<const uint32_t*>(ddsData.get());
        if (dwMagicNumber != DDS_MAGIC)
        {
            ddsData.reset();
            return DDS_LOADER_FAIL;
        }

        auto hdr = reinterpret_cast<const DDS_HEADER*>(ddsData.get() + sizeof(uint32_t));

        // Verify header to validate DDS file
        if (hdr->size != sizeof(DDS_HEADER) ||
            hdr->ddspf.size != sizeof(DDS_PIXELFORMAT))
        {
            ddsData.reset();
            return DDS_LOADER_FAIL;
        }

        // Check for DX10 extension
        bool bDXT10Header = false;
        if ((hdr->ddspf.flags & DDS_FOURCC) &&
            (MAKEFOURCC('D', 'X', '1', '0') == hdr->ddspf.fourCC))
        {
            // Must be long enough for both headers and magic value
            if (len < (sizeof(uint32_t) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10)))
            {
                ddsData.reset();
                return DDS_LOADER_FAIL;
            }

            bDXT10Header = true;
        }

        // setup the pointers in the process request
        *header = hdr;
        auto offset = sizeof(uint32_t) + sizeof(DDS_HEADER)
            + (bDXT10Header ? sizeof(DDS_HEADER_DXT10) : 0);
        *bitData = ddsData.get() + offset;
        *bitSize = len - offset;

        return DDS_LOADER_SUCCESS;
    }

    //--------------------------------------------------------------------------------------
    // Returns true if dxgiFormat belongs to _TYPELESS family of formats.
    // Otherwise, returns false.
    //--------------------------------------------------------------------------------------
    bool IsTypelessFormat(uint32_t dxgiFormat)
    {
        return dxgiFormat ==  1  //DXGI_FORMAT_R32G32B32A32_TYPELESS
            || dxgiFormat ==  5  //DXGI_FORMAT_R32G32B32_TYPELESS
            || dxgiFormat ==  9  //DXGI_FORMAT_R16G16B16A16_TYPELESS
            || dxgiFormat == 15  //DXGI_FORMAT_R32G32_TYPELESS
            || dxgiFormat == 19  //DXGI_FORMAT_R32G8X24_TYPELESS
            || dxgiFormat == 21  //DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
            || dxgiFormat == 23  //DXGI_FORMAT_R10G10B10A2_TYPELESS
            || dxgiFormat == 27  //DXGI_FORMAT_R8G8B8A8_TYPELESS
            || dxgiFormat == 33  //DXGI_FORMAT_R16G16_TYPELESS
            || dxgiFormat == 39  //DXGI_FORMAT_R32_TYPELESS
            || dxgiFormat == 44  //DXGI_FORMAT_R24G8_TYPELESS
            || dxgiFormat == 46  //DXGI_FORMAT_R24_UNORM_X8_TYPELESS
            || dxgiFormat == 47  //DXGI_FORMAT_X24_TYPELESS_G8_UINT
            || dxgiFormat == 48  //DXGI_FORMAT_R8G8_TYPELESS
            || dxgiFormat == 53  //DXGI_FORMAT_R16_TYPELESSS
            || dxgiFormat == 60  //DXGI_FORMAT_R8_TYPELESS
            || dxgiFormat == 70  //DXGI_FORMAT_BC1_TYPELESS
            || dxgiFormat == 73  //DXGI_FORMAT_BC2_TYPELESS
            || dxgiFormat == 76  //DXGI_FORMAT_BC3_TYPELESS
            || dxgiFormat == 79  //DXGI_FORMAT_BC4_TYPELESS
            || dxgiFormat == 82  //DXGI_FORMAT_BC5_TYPELESS
            || dxgiFormat == 90  //DXGI_FORMAT_B8G8R8A8_TYPELESS
            || dxgiFormat == 92  //DXGI_FORMAT_B8G8R8X8_TYPELESS
            || dxgiFormat == 94  //DXGI_FORMAT_BC6H_TYPELESS
            || dxgiFormat == 97; //DXGI_FORMAT_BC7_TYPELESS
    }

    //--------------------------------------------------------------------------------------
    // Convert D3D12_RESOURCE_DIMENSION value to corresponding VkImageType
    //--------------------------------------------------------------------------------------
    VkImageType D3DResourceDimensionToImageType(uint32_t resDim)
    {
        switch (resDim)
        {
        case 2: //D3D12_RESOURCE_DIMENSION_TEXTURE1D
            return VK_IMAGE_TYPE_1D;

        case 3: //D3D12_RESOURCE_DIMENSION_TEXTURE2D
            return VK_IMAGE_TYPE_2D;

        case 4: //D3D12_RESOURCE_DIMENSION_TEXTURE3D
            return VK_IMAGE_TYPE_3D;

        default:
            return VK_IMAGE_TYPE_MAX_ENUM;
        }
    }

    //--------------------------------------------------------------------------------------
    // Convert DXGI_FORMAT value to VkFormat
    // For regular TYPELESS formats return corresponding UINT ones.
    // For BC compressed TYPELESS formats return corresponding UNORM ones.
    // For BC6H compressed TYPELESS formats return corresponding UFLOAT ones.
    //--------------------------------------------------------------------------------------
    VkFormat DXGIToVkFormat(uint32_t dxgi_format)
    {
        switch (dxgi_format)
        {
        case 0: //DXGI_FORMAT_UNKNOWN
            return VK_FORMAT_UNDEFINED;

        case 1: //DXGI_FORMAT_R32G32B32A32_TYPELESS
            return VK_FORMAT_R32G32B32A32_UINT;

        case 2: //DXGI_FORMAT_R32G32B32A32_FLOAT
            return VK_FORMAT_R32G32B32A32_SFLOAT;

        case 3: //DXGI_FORMAT_R32G32B32A32_UINT
            return VK_FORMAT_R32G32B32A32_UINT;

        case 4: //DXGI_FORMAT_R32G32B32A32_SINT
            return VK_FORMAT_R32G32B32A32_SINT;

        case 5: //DXGI_FORMAT_R32G32B32_TYPELESS
            return VK_FORMAT_R32G32B32_UINT;

        case 6: //DXGI_FORMAT_R32G32B32_FLOAT
            return VK_FORMAT_R32G32B32_SFLOAT;

        case 7: //DXGI_FORMAT_R32G32B32_UINT
            return VK_FORMAT_R32G32B32_UINT;

        case 8: //DXGI_FORMAT_R32G32B32_SINT
            return VK_FORMAT_R32G32B32_SINT;

        case 9: //DXGI_FORMAT_R16G16B16A16_TYPELESS
            return VK_FORMAT_R16G16B16A16_UINT;

        case 10: //DXGI_FORMAT_R16G16B16A16_FLOAT
            return VK_FORMAT_R16G16B16A16_SFLOAT;

        case 11: //DXGI_FORMAT_R16G16B16A16_UNORM
            return VK_FORMAT_R16G16B16A16_UNORM;

        case 12: //DXGI_FORMAT_R16G16B16A16_UINT
            return VK_FORMAT_R16G16B16A16_UINT;

        case 13: //DXGI_FORMAT_R16G16B16A16_SNORM
            return VK_FORMAT_R16G16B16A16_SNORM;

        case 14: //DXGI_FORMAT_R16G16B16A16_SINT
            return VK_FORMAT_R16G16B16A16_SINT;

        case 15: //DXGI_FORMAT_R32G32_TYPELESS
            return VK_FORMAT_R32G32_UINT;

        case 16: //DXGI_FORMAT_R32G32_FLOAT
            return VK_FORMAT_R32G32_SFLOAT;

        case 17: //DXGI_FORMAT_R32G32_UINT
            return VK_FORMAT_R32G32_UINT;

        case 18: //DXGI_FORMAT_R32G32_SINT
            return VK_FORMAT_R32G32_SINT;

        case 19: //DXGI_FORMAT_R32G8X24_TYPELESS
            //D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses (optional) 40-bit stride.
            return VK_FORMAT_UNDEFINED;

        case 20: //DXGI_FORMAT_D32_FLOAT_S8X24_UINT
            //D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses 40-bit stride.
            return VK_FORMAT_UNDEFINED;

        case 21: //DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
            //It's possible to return R32G32 here and it will work for the red component, but only with manually not using the green component. It may be not a desired use case.
            //D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses 40-bit stride.
            return VK_FORMAT_UNDEFINED;

        case 22: //DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
            //D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses 40-bit stride.
            return VK_FORMAT_UNDEFINED;

        case 23: //DXGI_FORMAT_R10G10B10A2_TYPELESS
            return VK_FORMAT_A2B10G10R10_UINT_PACK32;

        case 24: //DXGI_FORMAT_R10G10B10A2_UNORM
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;

        case 25: //DXGI_FORMAT_R10G10B10A2_UINT
            return VK_FORMAT_A2B10G10R10_UINT_PACK32;

        case 26: //DXGI_FORMAT_R11G11B10_FLOAT
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;

        case 27: //DXGI_FORMAT_R8G8B8A8_TYPELESS
 			return VK_FORMAT_R8G8B8A8_UINT;

        case 28: //DXGI_FORMAT_R8G8B8A8_UNORM
            return VK_FORMAT_R8G8B8A8_UNORM;

        case 29: //DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            return VK_FORMAT_R8G8B8A8_SRGB;

        case 30: //DXGI_FORMAT_R8G8B8A8_UINT
            return VK_FORMAT_R8G8B8A8_UINT;

        case 31: //DXGI_FORMAT_R8G8B8A8_SNORM
            return VK_FORMAT_R8G8B8A8_SNORM;

        case 32: //DXGI_FORMAT_R8G8B8A8_SINT
            return VK_FORMAT_R8G8B8A8_SINT;

        case 33: //DXGI_FORMAT_R16G16_TYPELESS
            return VK_FORMAT_R16G16_UINT;

        case 34: //DXGI_FORMAT_R16G16_FLOAT
            return VK_FORMAT_R16G16_SFLOAT;

        case 35: //DXGI_FORMAT_R16G16_UNORM
            return VK_FORMAT_R16G16_UNORM;

        case 36: //DXGI_FORMAT_R16G16_UINT
            return VK_FORMAT_R16G16_UINT;

        case 37: //DXGI_FORMAT_R16G16_SNORM
            return VK_FORMAT_R16G16_SNORM;

        case 38: //DXGI_FORMAT_R16G16_SINT
            return VK_FORMAT_R16G16_SINT;

        case 39: //DXGI_FORMAT_R32_TYPELESS
            return VK_FORMAT_R32_UINT;

        case 40: //DXGI_FORMAT_D32_FLOAT
            return VK_FORMAT_D32_SFLOAT;

        case 41: //DXGI_FORMAT_R32_FLOAT
            return VK_FORMAT_R32_SFLOAT;

        case 42: //DXGI_FORMAT_R32_UINT
            return VK_FORMAT_R32_UINT;

        case 43: //DXGI_FORMAT_R32_SINT
            return VK_FORMAT_R32_SINT;

        case 44: //DXGI_FORMAT_R24G8_TYPELESS
            return VK_FORMAT_D24_UNORM_S8_UINT;

        case 45: //DXGI_FORMAT_D24_UNORM_S8_UINT
            return VK_FORMAT_D24_UNORM_S8_UINT;

        case 46: //DXGI_FORMAT_R24_UNORM_X8_TYPELESS
            return VK_FORMAT_UNDEFINED;

        case 47: //DXGI_FORMAT_X24_TYPELESS_G8_UINT
            return VK_FORMAT_UNDEFINED;

        case 48: //DXGI_FORMAT_R8G8_TYPELESS
            return VK_FORMAT_R8G8_UINT;

        case 49: //DXGI_FORMAT_R8G8_UNORM
            return VK_FORMAT_R8G8_UNORM;

        case 50: //DXGI_FORMAT_R8G8_UINT
            return VK_FORMAT_R8G8_UINT;

        case 51: //DXGI_FORMAT_R8G8_SNORM
            return VK_FORMAT_R8G8_SNORM;

        case 52: //DXGI_FORMAT_R8G8_SINT
            return VK_FORMAT_R8G8_SINT;

        case 53: //DXGI_FORMAT_R16_TYPELESSS
            return VK_FORMAT_R16_UINT;

        case 54: //DXGI_FORMAT_R16_FLOAT
            return VK_FORMAT_R16_SFLOAT;

        case 55: //DXGI_FORMAT_D16_UNORM
            return VK_FORMAT_D16_UNORM;

        case 56: //DXGI_FORMAT_R16_UNORM
            return VK_FORMAT_R16_UNORM;

        case 57: //DXGI_FORMAT_R16_UINT
            return VK_FORMAT_R16_UINT;

        case 58: //DXGI_FORMAT_R16_SNORM
            return VK_FORMAT_R16_SNORM;

        case 59: //DXGI_FORMAT_R16_SINT
            return VK_FORMAT_R16_SINT;

        case 60: //DXGI_FORMAT_R8_TYPELESS
            return VK_FORMAT_R8_UINT;

        case 61: //DXGI_FORMAT_R8_UNORM
            return VK_FORMAT_R8_UNORM;

        case 62: //DXGI_FORMAT_R8_UINT
            return VK_FORMAT_R8_UINT;

        case 63: //DXGI_FORMAT_R8_SNORM
            return VK_FORMAT_R8_SNORM;

        case 64: //DXGI_FORMAT_R8_SINT
            return VK_FORMAT_R8_SINT;

        case 65: //DXGI_FORMAT_A8_UNORM
            //A8 format is not supported in Vulkan.
            return VK_FORMAT_UNDEFINED;

        case 66: //DXGI_FORMAT_R1_UNORM
            //R1 format is not supported by Vulkan.
            return VK_FORMAT_UNDEFINED;

        case 67: //DXGI_FORMAT_R9G9B9E5_SHAREDEXP
 			return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1

        case 68: //DXGI_FORMAT_R8G8_B8G8_UNORM
            return VK_FORMAT_G8B8G8R8_422_UNORM;

        case 69: //DXGI_FORMAT_G8R8_G8B8_UNORM
            return VK_FORMAT_B8G8R8G8_422_UNORM;

#elif defined(VK_KHR_sampler_ycbcr_conversion)

        case 68: //DXGI_FORMAT_R8G8_B8G8_UNORM
            return VK_FORMAT_G8B8G8R8_422_UNORM_KHR;

        case 69: //DXGI_FORMAT_G8R8_G8B8_UNORM
            return VK_FORMAT_B8G8R8G8_422_UNORM_KHR;

#endif

        case 70: //DXGI_FORMAT_BC1_TYPELESS
            //Imply alpha support for the BC formats
 			return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;

        case 71: //DXGI_FORMAT_BC1_UNORM
            //Imply alpha support for the BC formats
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;

        case 72: //DXGI_FORMAT_BC1_UNORM_SRGB
            //Imply alpha support for the BC formats
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;

        case 73: //DXGI_FORMAT_BC2_TYPELESS
            return VK_FORMAT_BC2_UNORM_BLOCK;

        case 74: //DXGI_FORMAT_BC2_UNORM
            return VK_FORMAT_BC2_UNORM_BLOCK;

        case 75: //DXGI_FORMAT_BC2_UNORM_SRGB
            return VK_FORMAT_BC2_SRGB_BLOCK;

        case 76: //DXGI_FORMAT_BC3_TYPELESS
            return VK_FORMAT_BC3_UNORM_BLOCK;

        case 77: //DXGI_FORMAT_BC3_UNORM
            return VK_FORMAT_BC3_UNORM_BLOCK;

        case 78: //DXGI_FORMAT_BC3_UNORM_SRGB
            return VK_FORMAT_BC3_SRGB_BLOCK;

        case 79: //DXGI_FORMAT_BC4_TYPELESS
            return VK_FORMAT_BC4_UNORM_BLOCK;

        case 80: //DXGI_FORMAT_BC4_UNORM
            return VK_FORMAT_BC4_UNORM_BLOCK;

        case 81: //DXGI_FORMAT_BC4_SNORM
            return VK_FORMAT_BC4_SNORM_BLOCK;

        case 82: //DXGI_FORMAT_BC5_TYPELESS
            return VK_FORMAT_BC5_UNORM_BLOCK;

        case 83: //DXGI_FORMAT_BC5_UNORM
            return VK_FORMAT_BC5_UNORM_BLOCK;

        case 84: //DXGI_FORMAT_BC5_SNORM
            return VK_FORMAT_BC5_SNORM_BLOCK;

        case 85: //DXGI_FORMAT_B5G6R5_UNORM
            return VK_FORMAT_R5G6B5_UNORM_PACK16;

        case 86: //DXGI_FORMAT_B5G5R5A1_UNORM
            return VK_FORMAT_A1R5G5B5_UNORM_PACK16;

        case 87: //DXGI_FORMAT_B8G8R8A8_UNORM
            return VK_FORMAT_B8G8R8A8_UNORM;

        case 88: //DXGI_FORMAT_B8G8R8X8_UNORM
            //B8G8R8 format with 32 bit stride is not supported in Vulkan.
            return VK_FORMAT_UNDEFINED;

        case 89: //DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM
            //Vulkan does not support XR biased formats.
            return VK_FORMAT_UNDEFINED;

        case 90: //DXGI_FORMAT_B8G8R8A8_TYPELESS
            return VK_FORMAT_B8G8R8A8_UNORM;

        case 91: //DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            return VK_FORMAT_B8G8R8A8_SRGB;

        case 92: //DXGI_FORMAT_B8G8R8X8_TYPELESS
            //B8G8R8 format with 32 bit stride is not supported in Vulkan.
            return VK_FORMAT_UNDEFINED;

        case 93: //DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
            //B8G8R8 format with 32 bit stride is not supported in Vulkan.
            return VK_FORMAT_UNDEFINED;

        case 94: //DXGI_FORMAT_BC6H_TYPELESS
            return VK_FORMAT_BC6H_UFLOAT_BLOCK;

        case 95: //DXGI_FORMAT_BC6H_UF16
            return VK_FORMAT_BC6H_UFLOAT_BLOCK;

        case 96: //DXGI_FORMAT_BC6H_SF16
            return VK_FORMAT_BC6H_SFLOAT_BLOCK;

        case 97: //DXGI_FORMAT_BC7_TYPELESS
            return VK_FORMAT_BC7_UNORM_BLOCK;

        case 98: //DXGI_FORMAT_BC7_UNORM
            return VK_FORMAT_BC7_UNORM_BLOCK;

        case 99: //DXGI_FORMAT_BC7_UNORM_SRGB
            return VK_FORMAT_BC7_SRGB_BLOCK;

        case 100: //DXGI_FORMAT_AYUV
            //DXGI_FORMAT_R8G8B8A8_UNORM is a valid view format for AYUV data. We use a corresponding format here
            return VK_FORMAT_R8G8B8A8_UNORM;

        case 101: //DXGI_FORMAT_Y410
            //DXGI_FORMAT_R10G10B10A2_UNORM is a valid view format for Y410 data. We use a corresponding format here
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;

        case 102: //DXGI_FORMAT_Y416
            //DXGI_FORMAT_R16G16B16A16_UNORM is a valid view format for Y416 data. We use a corresponding format here
            return VK_FORMAT_R16G16B16A16_UNORM;

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1
        case 103: //DXGI_FORMAT_NV12
            //Multi-planar 4:2:0 8-bit per channel format
            return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

        case 104: //DXGI_FORMAT_P010
            //Multi-planar 4:2:0 10X6-bit per channel format
            return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;

        case 105: //DXGI_FORMAT_P016
            //Multi-planar 4:2:0 16-bit per channel format
            return VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;

        case 106: //DXGI_FORMAT_420_OPAQUE
            //Multi-planar 4:2:0 8-bit per channel format
            return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

        case 107: //DXGI_FORMAT_YUY2
            //Packed 4:2:2 8-bit per channel format
            return VK_FORMAT_G8B8G8R8_422_UNORM;

        case 108: //DXGI_FORMAT_Y210
            //Packed 4:2:2 10X6-bit per channel format
            return VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16;

        case 109: //DXGI_FORMAT_Y216
            //Packed 4:2:2 16-bit per channel format
            return VK_FORMAT_G16B16G16R16_422_UNORM;

#elif defined(VK_KHR_sampler_ycbcr_conversion)
        case 103: //DXGI_FORMAT_NV12
            //Multi-planar 4:2:0 8-bit per channel format
            return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM_KHR;

        case 104: //DXGI_FORMAT_P010
            //Multi-planar 4:2:0 10X6-bit per channel format
            return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16_KHR;

        case 105: //DXGI_FORMAT_P016
            //Multi-planar 4:2:0 16-bit per channel format
            return VK_FORMAT_G16_B16R16_2PLANE_420_UNORM_KHR;

        case 106: //DXGI_FORMAT_420_OPAQUE
            //Multi-planar 4:2:0 8-bit per channel format
            return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM_KHR;

        case 107: //DXGI_FORMAT_YUY2
            //Packed 4:2:2 8-bit per channel format
            return VK_FORMAT_G8B8G8R8_422_UNORM_KHR;

        case 108: //DXGI_FORMAT_Y210
            //Packed 4:2:2 10X6-bit per channel format
            return VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16_KHR;

        case 109: //DXGI_FORMAT_Y216
            //Packed 4:2:2 16-bit per channel format
            return VK_FORMAT_G16B16G16R16_422_UNORM_KHR;
#endif

        case 110: //DXGI_FORMAT_NV11
            //Vulkan does not support 4:1:1 formats
            return VK_FORMAT_UNDEFINED;

        case 111: //DXGI_FORMAT_AI44
            //Vulkan does not support palletized formats
            return VK_FORMAT_UNDEFINED;

        case 112: //DXGI_FORMAT_IA44
            //Vulkan does not support palletized formats
            return VK_FORMAT_UNDEFINED;

        case 113: //DXGI_FORMAT_P8
            //Vulkan does not support palletized formats
            return VK_FORMAT_UNDEFINED;

        case 114: //DXGI_FORMAT_A8P8
            //Vulkan does not support palletized formats
            return VK_FORMAT_UNDEFINED;

#ifdef VK_EXT_4444_formats 

        case 115: //DXGI_FORMAT_B4G4R4A4_UNORM
            return VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT;

#endif 

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1
        case 130: //DXGI_FORMAT_P208
            //Multi-planar 4:2:2 8-bit per channel format
            return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;

        case 131: //DXGI_FORMAT_V208
            //Vulkan doesn't support 4:4 formats
            return VK_FORMAT_UNDEFINED;

        case 132: //DXGI_FORMAT_V408
            //4:4:4 format
            return VK_FORMAT_R8G8B8A8_UNORM;
#endif

        default:
            //Unknown format.
            return VK_FORMAT_UNDEFINED;
        }
    }

    //-------------------------------------------------------------------------------------------
    // Returns the plane count for the format. Vulkan SDK doesn't provide the function for that
    //-------------------------------------------------------------------------------------------
    uint8_t GetVkFormatPlaneCount(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_UNDEFINED:
            return 0;

        case VK_FORMAT_R4G4_UNORM_PACK8:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_UNORM:
        case VK_FORMAT_B8G8R8_SNORM:
        case VK_FORMAT_B8G8R8_USCALED:
        case VK_FORMAT_B8G8R8_SSCALED:
        case VK_FORMAT_B8G8R8_UINT:
        case VK_FORMAT_B8G8R8_SINT:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_B8G8R8A8_USCALED:
        case VK_FORMAT_B8G8R8A8_SSCALED:
        case VK_FORMAT_B8G8R8A8_UINT:
        case VK_FORMAT_B8G8R8A8_SINT:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
        case VK_FORMAT_A8B8G8R8_UINT_PACK32:
        case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
        case VK_FORMAT_A2R10G10B10_SINT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16B16_UNORM:
        case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R16G16B16_USCALED:
        case VK_FORMAT_R16G16B16_SSCALED:
        case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R64_UINT:
        case VK_FORMAT_R64_SINT:
        case VK_FORMAT_R64_SFLOAT:
        case VK_FORMAT_R64G64_UINT:
        case VK_FORMAT_R64G64_SINT:
        case VK_FORMAT_R64G64_SFLOAT:
        case VK_FORMAT_R64G64B64_UINT:
        case VK_FORMAT_R64G64B64_SINT:
        case VK_FORMAT_R64G64B64_SFLOAT:
        case VK_FORMAT_R64G64B64A64_UINT:
        case VK_FORMAT_R64G64B64A64_SINT:
        case VK_FORMAT_R64G64B64A64_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return 1;

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1
        case VK_FORMAT_G8B8G8R8_422_UNORM:
        case VK_FORMAT_B8G8R8G8_422_UNORM:
        case VK_FORMAT_R10X6_UNORM_PACK16:
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16:
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
        case VK_FORMAT_R12X4_UNORM_PACK16:
        case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
        case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
        case VK_FORMAT_G16B16G16R16_422_UNORM:
        case VK_FORMAT_B16G16R16G16_422_UNORM:
            return 1;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
            return 2;

        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
            return 3;
#endif

#ifdef VK_IMG_format_pvrtc
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
            return 1;
#endif

#ifdef VK_EXT_texture_compression_astc_hdr
        case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK_EXT:
            return 1;
#endif

#ifdef VK_EXT_4444_formats
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT:
        case VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT:
            return 1;
#endif

#if (!defined(VK_VERSION_1_1) || !VK_VERSION_1_1) && defined(VK_KHR_sampler_ycbcr_conversion)
        case VK_FORMAT_G8B8G8R8_422_UNORM_KHR:
        case VK_FORMAT_B8G8R8G8_422_UNORM_KHR:
        case VK_FORMAT_R10X6_UNORM_PACK16_KHR:
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16_KHR:
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16_KHR:
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_R12X4_UNORM_PACK16_KHR:
        case VK_FORMAT_R12X4G12X4_UNORM_2PACK16_KHR:
        case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16_KHR:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_G16B16G16R16_422_UNORM_KHR:
        case VK_FORMAT_B16G16R16G16_422_UNORM_KHR:
            return 1;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM_KHR:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM_KHR:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM_KHR:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM_KHR:
            return 2;

        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM_KHR:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM_KHR:
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM_KHR:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM_KHR:
            return 3;
#endif 
        default:
            return 0;
        }
    }

    //--------------------------------------------------------------------------------------
    // Return the (rounded up multiple of 8) BPP for a particular format
    //--------------------------------------------------------------------------------------
    size_t BitsPerPixel(VkFormat fmt) noexcept
    {
        // Some of the formats (such as VK_FORMAT_ASTC_5x5_SRGB_BLOCK) have fractional BPP. 
        // This function rounds them up to the closest power of 2.

        switch (fmt)
        {
        case VK_FORMAT_R64G64B64A64_UINT:
        case VK_FORMAT_R64G64B64A64_SINT:
        case VK_FORMAT_R64G64B64A64_SFLOAT:
            return 256;

        case VK_FORMAT_R64G64B64_UINT:
        case VK_FORMAT_R64G64B64_SINT:
        case VK_FORMAT_R64G64B64_SFLOAT:
            return 192;

        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R64G64_UINT:
        case VK_FORMAT_R64G64_SINT:
        case VK_FORMAT_R64G64_SFLOAT:
            return 128;

        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            return 96;

        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R64_UINT:
        case VK_FORMAT_R64_SINT:
        case VK_FORMAT_R64_SFLOAT:
            return 64;

        case VK_FORMAT_R16G16B16_UNORM:
        case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R16G16B16_USCALED:
        case VK_FORMAT_R16G16B16_SSCALED:
        case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16_SFLOAT:
            return 48;

        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 40;

        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_B8G8R8A8_USCALED:
        case VK_FORMAT_B8G8R8A8_SSCALED:
        case VK_FORMAT_B8G8R8A8_UINT:
        case VK_FORMAT_B8G8R8A8_SINT:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
        case VK_FORMAT_A8B8G8R8_UINT_PACK32:
        case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
        case VK_FORMAT_A2R10G10B10_SINT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return 32;

        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_UNORM:
        case VK_FORMAT_B8G8R8_SNORM:
        case VK_FORMAT_B8G8R8_USCALED:
        case VK_FORMAT_B8G8R8_SSCALED:
        case VK_FORMAT_B8G8R8_UINT:
        case VK_FORMAT_B8G8R8_SINT:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            return 24;

        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_D16_UNORM:
            return 16;

        case VK_FORMAT_R4G4_UNORM_PACK8:
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_S8_UINT:
            return 8;

        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            return 4;

        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            return 2;

        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return 1;

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16:
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
        case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
        case VK_FORMAT_G16B16G16R16_422_UNORM:
        case VK_FORMAT_B16G16R16G16_422_UNORM:
            return 64;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
            return 48;

        case VK_FORMAT_G8B8G8R8_422_UNORM:
        case VK_FORMAT_B8G8R8G8_422_UNORM:
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
        case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
            return 32;

        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
            return 24;

        case VK_FORMAT_R10X6_UNORM_PACK16:
        case VK_FORMAT_R12X4_UNORM_PACK16:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
            return 16;

        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
            return 12;
#endif

#ifdef VK_IMG_format_pvrtc
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
            return 4;

        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG:
            return 2;
#endif

#ifdef VK_EXT_texture_compression_astc_hdr
        case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK_EXT:
            return 8;

        case VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK_EXT:
            return 4;

        case VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK_EXT:
        case VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK_EXT:
            return 2;

        case VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK_EXT:
            return 1;
#endif

#ifdef VK_EXT_4444_formats
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT:
        case VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT:
            return 16;
#endif

#if (!defined(VK_VERSION_1_1) || !VK_VERSION_1_1) && defined(VK_KHR_sampler_ycbcr_conversion)
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16_KHR:
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16_KHR:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_G16B16G16R16_422_UNORM_KHR:
        case VK_FORMAT_B16G16R16G16_422_UNORM_KHR:
            return 64;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM_KHR:
            return 48;

        case VK_FORMAT_G8B8G8R8_422_UNORM_KHR:
        case VK_FORMAT_B8G8R8G8_422_UNORM_KHR:
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16_KHR:
        case VK_FORMAT_R12X4G12X4_UNORM_2PACK16_KHR:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM_KHR:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM_KHR:
            return 32;

        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM_KHR:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM_KHR:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM_KHR:
            return 24;

        case VK_FORMAT_R10X6_UNORM_PACK16_KHR:
        case VK_FORMAT_R12X4_UNORM_PACK16_KHR:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM_KHR:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM_KHR:
            return 16;

        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM_KHR:
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM_KHR:
            return 12;
#endif

        default:
            return 0;
        }
    }

    //--------------------------------------------------------------------------------------
    // Get surface information for a particular format
    //--------------------------------------------------------------------------------------
    DDS_LOADER_RESULT GetSurfaceInfo(
        size_t width,
        size_t height,
        VkFormat fmt,
        VkImageAspectFlags aspectPlane,
        size_t* outNumBytes,
        size_t* outRowBytes,
        size_t* outNumRows) noexcept
    {
        uint64_t numBytes = 0;
        uint64_t rowBytes = 0;
        uint64_t numRows  = 0;

        bool     is2PlaneFormat = false;
        bool     is3PlaneFormat = false;
        bool     isPackedFormat = false;
        bool     isBlockFormat  = false;
        uint32_t blockWidth     = 0;
        uint32_t blockHeight    = 0;
        size_t   bytesPerBlock  = 0;
        switch(fmt)
        {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
            isBlockFormat = true;
            blockWidth    = 4;
            blockHeight   = 4;
            bytesPerBlock = 8;
            break;

        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 4;
            blockHeight   = 4;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 5;
            blockHeight   = 4;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 5;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 6;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 6;
            blockHeight   = 6;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 6;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 8;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 6;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 8;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 10;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 12;
            blockHeight   = 10;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            isBlockFormat = true;
            blockWidth    = 12;
            blockHeight   = 12;
            bytesPerBlock = 16;
            break;

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1
        case VK_FORMAT_G8B8G8R8_422_UNORM:
        case VK_FORMAT_B8G8R8G8_422_UNORM:
            isPackedFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 4;
            break;

        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
        case VK_FORMAT_G16B16G16R16_422_UNORM:
        case VK_FORMAT_B16G16R16G16_422_UNORM:
            isPackedFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 8;
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
            is2PlaneFormat = true;
            blockWidth      = 2;
            blockHeight     = 2;
            bytesPerBlock   = 6;
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
            is2PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 4;
            break;

        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
            is2PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 2;
            bytesPerBlock  = 12;
            break;

        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
            is2PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 8;
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 2;
            bytesPerBlock  = 6;
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 4;
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
            is3PlaneFormat = true;
            blockWidth     = 1;
            blockHeight    = 1;
            bytesPerBlock  = 3;
            break;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 2;
            bytesPerBlock  = 12;
            break;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 8;
            break;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
            is3PlaneFormat = true;
            blockWidth     = 1;
            blockHeight    = 1;
            bytesPerBlock  = 6;
            break;
#endif

#ifdef VK_IMG_format_pvrtc
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 4;
            bytesPerBlock = 8;
            break;

        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 4;
            bytesPerBlock = 8;
            break;
#endif

#ifdef VK_EXT_texture_compression_astc_hdr
        case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 4;
            blockHeight   = 4;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 5;
            blockHeight   = 4;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 5;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 6;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 6;
            blockHeight   = 6;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 6;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 8;
            blockHeight   = 8;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 5;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 6;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 8;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 10;
            blockHeight   = 10;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 12;
            blockHeight   = 10;
            bytesPerBlock = 16;
            break;

        case VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK_EXT:
            isBlockFormat = true;
            blockWidth    = 12;
            blockHeight   = 12;
            bytesPerBlock = 16;
            break;
#endif

#if (!defined(VK_VERSION_1_1) || !VK_VERSION_1_1) && defined(VK_KHR_sampler_ycbcr_conversion)
        case VK_FORMAT_G8B8G8R8_422_UNORM_KHR:
        case VK_FORMAT_B8G8R8G8_422_UNORM_KHR:
            isPackedFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 4;
            break;

        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_G16B16G16R16_422_UNORM:
        case VK_FORMAT_B16G16R16G16_422_UNORM:
            isPackedFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 8;
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM_KHR:
            is2PlaneFormat = true;
            blockWidth      = 2;
            blockHeight     = 2;
            bytesPerBlock   = 6;
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM_KHR:
            is2PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 4;
            break;

        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM_KHR:
            is2PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 2;
            bytesPerBlock  = 12;
            break;

        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM_KHR:
            is2PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 8;
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM_KHR:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 2;
            bytesPerBlock  = 6;
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM_KHR:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 4;
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM_KHR:
            is3PlaneFormat = true;
            blockWidth     = 1;
            blockHeight    = 1;
            bytesPerBlock  = 3;
            break;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM_KHR:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 2;
            bytesPerBlock  = 12;
            break;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM_KHR:
            is3PlaneFormat = true;
            blockWidth     = 2;
            blockHeight    = 1;
            bytesPerBlock  = 8;
            break;

        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16_KHR:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16_KHR:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM_KHR:
            is3PlaneFormat = true;
            blockWidth     = 1;
            blockHeight    = 1;
            bytesPerBlock  = 6;
            break;
#endif 
        default:
            break;
        }

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1

        assert(!is2PlaneFormat || aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT || aspectPlane == VK_IMAGE_ASPECT_PLANE_1_BIT);
        assert(!is3PlaneFormat || aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT || aspectPlane == VK_IMAGE_ASPECT_PLANE_1_BIT || aspectPlane == VK_IMAGE_ASPECT_PLANE_2_BIT);

        assert(is2PlaneFormat  || (aspectPlane != VK_IMAGE_ASPECT_PLANE_0_BIT && aspectPlane != VK_IMAGE_ASPECT_PLANE_1_BIT));
        assert(is3PlaneFormat  || (aspectPlane != VK_IMAGE_ASPECT_PLANE_0_BIT && aspectPlane != VK_IMAGE_ASPECT_PLANE_1_BIT && aspectPlane != VK_IMAGE_ASPECT_PLANE_2_BIT));

#elif (!defined(VK_VERSION_1_1) || !VK_VERSION_1_1) && defined(VK_KHR_sampler_ycbcr_conversion)

        assert(!is2PlaneFormat || aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT_KHR || aspectPlane == VK_IMAGE_ASPECT_PLANE_1_BIT_KHR);
        assert(!is3PlaneFormat || aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT_KHR || aspectPlane == VK_IMAGE_ASPECT_PLANE_1_BIT_KHR || aspectPlane == VK_IMAGE_ASPECT_PLANE_2_BIT_KHR);

        assert(is2PlaneFormat  || (aspectPlane != VK_IMAGE_ASPECT_PLANE_0_BIT_KHR && aspectPlane != VK_IMAGE_ASPECT_PLANE_1_BIT_KHR));
        assert(is3PlaneFormat  || (aspectPlane != VK_IMAGE_ASPECT_PLANE_0_BIT_KHR && aspectPlane != VK_IMAGE_ASPECT_PLANE_1_BIT_KHR && aspectPlane != VK_IMAGE_ASPECT_PLANE_2_BIT_KHR));

#endif

        if (isBlockFormat)
        {
            assert(blockWidth != 0 && blockHeight != 0);

            uint64_t numBlocksWide = 0;
            if (width > 0)
            {
                numBlocksWide = std::max<uint64_t>(1u, (uint64_t(width) + (blockWidth - 1u)) / blockWidth);
            }
            uint64_t numBlocksHigh = 0;
            if (height > 0)
            {
                numBlocksHigh = std::max<uint64_t>(1u, (uint64_t(height) + (blockHeight - 1u)) / blockHeight);
            }
            rowBytes = numBlocksWide * bytesPerBlock;
            numRows = numBlocksHigh;
            numBytes = rowBytes * numBlocksHigh;
        }
        else if(isPackedFormat && blockWidth == 2 && blockHeight == 1) //4:2:2 packed
        {
            rowBytes = ((uint64_t(width) + 1u) >> 1) * bytesPerBlock;
            numRows = uint64_t(height);
            numBytes = rowBytes * height;
        }
#if defined(VK_VERSION_1_1) && VK_VERSION_1_1
        else if(is2PlaneFormat)
        {
            if(aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT)
            {
                //First plane is always of full resolution
                rowBytes = uint64_t(width) * bytesPerBlock / (blockWidth * blockHeight + 2);
                numRows  = uint64_t(height);
            }
            else
            {
                //Second plane is half resolution or full resolution on one or both axes
                const size_t bytesPerElement = bytesPerBlock / (blockWidth * blockHeight + 2);

                rowBytes = ((uint64_t(width) + blockWidth - 1) / blockWidth) * bytesPerElement * 2;
                numRows  = ((uint64_t(height) + blockHeight - 1) / blockHeight);
            }
            
            numBytes = rowBytes * numRows;
        }
        else if(is3PlaneFormat)
        {
            if(aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT)
            {
                //First plane is always of full resolution
                rowBytes = uint64_t(width) * bytesPerBlock / (blockWidth * blockHeight + 2);
                numRows  = uint64_t(height);
            }
            else
            {
                //Second and third planes is half resolution or full resolution on one or both axes
                const size_t bytesPerElement = bytesPerBlock / (blockWidth * blockHeight + 2);

                rowBytes = ((uint64_t(width) + blockWidth - 1) / blockWidth) * bytesPerElement;
                numRows  = ((uint64_t(height) + blockHeight - 1) / blockHeight);
            }

            numBytes = rowBytes * numRows;
        }
#elif (!defined(VK_VERSION_1_1) || !VK_VERSION_1_1) && defined(VK_KHR_sampler_ycbcr_conversion)
        else if(is2PlaneFormat)
        {
            if(aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT_KHR)
            {
                //First plane is always of full resolution
                rowBytes = uint64_t(width) * bytesPerBlock / (blockWidth * blockHeight + 2);
                numRows  = uint64_t(height);
            }
            else
            {
                //Second plane is half resolution or full resolution on one or both axes
                const size_t bytesPerElement = bytesPerBlock / (blockWidth * blockHeight + 2);

                rowBytes = ((uint64_t(width) + blockWidth - 1) / blockWidth) * bytesPerElement * 2;
                numRows  = ((uint64_t(height) + blockHeight - 1) / blockHeight);
            }
            
            numBytes = rowBytes * numRows;
        }
        else if(is3PlaneFormat)
        {
            if(aspectPlane == VK_IMAGE_ASPECT_PLANE_0_BIT_KHR)
            {
                //First plane is always of full resolution
                rowBytes = uint64_t(width) * bytesPerBlock / (blockWidth * blockHeight + 2);
                numRows  = uint64_t(height);
            }
            else
            {
                //Second and third planes is half resolution or full resolution on one or both axes
                const size_t bytesPerElement = bytesPerBlock / (blockWidth * blockHeight + 2);

                rowBytes = ((uint64_t(width) + blockWidth - 1) / blockWidth) * bytesPerElement;
                numRows  = ((uint64_t(height) + blockHeight - 1) / blockHeight);
            }

            numBytes = rowBytes * numRows;
        }
#endif
        else
        {
            size_t bpp = BitsPerPixel(fmt);
            if (!bpp)
                return DDS_LOADER_INVALID_ARG;

            rowBytes = (uint64_t(width) * bpp + 7u) / 8u; // round up to nearest byte
            numRows = uint64_t(height);
            numBytes = rowBytes * height;
        }

#if defined(_M_IX86) || defined(_M_ARM) || defined(_M_HYBRID_X86_ARM64)
        static_assert(sizeof(size_t) == 4, "Not a 32-bit platform!");
        if (numBytes > UINT32_MAX || rowBytes > UINT32_MAX || numRows > UINT32_MAX)
            return DDS_LOADER_ARITHMETIC_OVERFLOW;
#else
        static_assert(sizeof(size_t) == 8, "Not a 64-bit platform!");
#endif

        if (outNumBytes)
        {
            *outNumBytes = static_cast<size_t>(numBytes);
        }
        if (outRowBytes)
        {
            *outRowBytes = static_cast<size_t>(rowBytes);
        }
        if (outNumRows)
        {
            *outNumRows = static_cast<size_t>(numRows);
        }

        return DDS_LOADER_SUCCESS;
    }

    //--------------------------------------------------------------------------------------
    #define ISBITMASK( r,g,b,a ) ( ddpf.RBitMask == r && ddpf.GBitMask == g && ddpf.BBitMask == b && ddpf.ABitMask == a )

    VkFormat GetVkFormat( const DDS_PIXELFORMAT& ddpf ) noexcept
    {
        if (ddpf.flags & DDS_RGB)
        {
            // Note that sRGB formats are written using the "DX10" extended header

            switch (ddpf.RGBBitCount)
            {
            case 32:
                if (ISBITMASK(0x000000ff,0x0000ff00,0x00ff0000,0xff000000))
                {
                    return VK_FORMAT_R8G8B8A8_UNORM;
                }

                if (ISBITMASK(0x00ff0000,0x0000ff00,0x000000ff,0xff000000))
                {
                    return VK_FORMAT_B8G8R8A8_UNORM;
                }

                if (ISBITMASK(0x00ff0000,0x0000ff00,0x000000ff,0))
                {
                    //B8G8R8 format with 32 bit stride is not supported in Vulkan.
                    return VK_FORMAT_UNDEFINED;
                }

                // No VK format maps to ISBITMASK(0x000000ff,0x0000ff00,0x00ff0000,0) aka D3DFMT_X8B8G8R8

                // Note that many common DDS reader/writers (including D3DX) swap the
                // the RED/BLUE masks for 10:10:10:2 formats. We assume
                // below that the 'backwards' header mask is being used since it is most
                // likely written by D3DX

                // For 'correct' writers, this should be 0x000003ff,0x000ffc00,0x3ff00000 for RGB data
                if (ISBITMASK(0x3ff00000,0x000ffc00,0x000003ff,0xc0000000))
                {
                    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
                }

                if (ISBITMASK(0x000003ff, 0x000ffc00, 0x3ff00000, 0xc0000000))
                {
                    return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
                }

                if (ISBITMASK(0x0000ffff,0xffff0000,0,0))
                {
                    return VK_FORMAT_R16G16_UNORM;
                }

                if (ISBITMASK(0xffffffff,0,0,0))
                {
                    // Only 32-bit color channel format in D3D9 was R32F
                    return VK_FORMAT_R32_SFLOAT; // D3DX writes this out as a FourCC of 114
                }
                break;

            case 24:
                if (ISBITMASK(0xff0000,0x00ff00,0x0000ff,0))
                {
                    return VK_FORMAT_R8G8B8_UNORM;
                }
                break;

            case 16:
                if (ISBITMASK(0x7c00,0x03e0,0x001f,0x8000))
                {
                    return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
                }
                if (ISBITMASK(0xf800,0x07e0,0x001f,0))
                {
                    return VK_FORMAT_R5G6B5_UNORM_PACK16;
                }

                // No VK format maps to ISBITMASK(0x7c00,0x03e0,0x001f,0) aka D3DFMT_X1R5G5B5

#ifdef VK_EXT_4444_formats 
                if(ISBITMASK(0x0f00,0x00f0,0x000f,0xf000))
                {
                    return VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT;
                }
#endif

                // No VK format maps to ISBITMASK(0x0f00,0x00f0,0x000f,0) aka D3DFMT_X4R4G4B4

                // No 3:3:2, 3:3:2:8, or paletted DXGI formats aka D3DFMT_A8R3G3B2, D3DFMT_R3G3B2, D3DFMT_P8, D3DFMT_A8P8, etc.
                break;
            }
        }
        else if (ddpf.flags & DDS_LUMINANCE)
        {
            if (8 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0xff,0,0,0)) //D3DFMT_L8
                {
                    return VK_FORMAT_R8_UNORM; // D3DX10/11 writes this out as DX10 extension
                }

                if (ISBITMASK(0x0f, 0, 0, 0xf0)) //D3DFMT_A4L4
                {
                    return VK_FORMAT_R4G4_UNORM_PACK8;
                }

                if (ISBITMASK(0x00ff, 0, 0, 0xff00)) //D3DFMT_A8L8
                {
                    return VK_FORMAT_R8G8_UNORM; // Some DDS writers assume the bitcount should be 8 instead of 16
                }
            }

            if (16 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0xffff,0,0,0))
                {
                    return VK_FORMAT_R16_UNORM; // D3DX10/11 writes this out as DX10 extension
                }
                if (ISBITMASK(0x00ff,0,0,0xff00))
                {
                    return VK_FORMAT_R8G8_UNORM; // D3DX10/11 writes this out as DX10 extension
                }
            }

            //No VK format maps to alpha-only D3DFMT_A8
        }
        else if (ddpf.flags & DDS_BUMPDUDV)
        {
            if (16 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0x00ff, 0xff00, 0, 0))
                {
                    return VK_FORMAT_R8G8_SNORM; // D3DX10/11 writes this out as DX10 extension
                }
            }

            if (32 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))
                {
                    return VK_FORMAT_R8G8B8A8_SNORM; // D3DX10/11 writes this out as DX10 extension
                }
                if (ISBITMASK(0x0000ffff, 0xffff0000, 0, 0))
                {
                    return VK_FORMAT_R16G16_SNORM; // D3DX10/11 writes this out as DX10 extension
                }
                if (ISBITMASK(0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000))
                {
                    return VK_FORMAT_A2B10G10R10_SNORM_PACK32; //This may be wrong, alpha shouldn't be signed
                }
            }

            // No VK format maps to DDPF_BUMPLUMINANCE aka D3DFMT_L6V5U5, D3DFMT_X8L8V8U8
        }
        else if (ddpf.flags & DDS_FOURCC)
        {
            if (MAKEFOURCC( 'D', 'X', 'T', '1' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            }
            if (MAKEFOURCC( 'D', 'X', 'T', '3' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC2_UNORM_BLOCK;
            }
            if (MAKEFOURCC( 'D', 'X', 'T', '5' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC3_UNORM_BLOCK;
            }

            // While pre-multiplied alpha isn't directly supported by the VK formats,
            // they are basically the same as these BC formats so they can be mapped
            if (MAKEFOURCC( 'D', 'X', 'T', '2' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC2_UNORM_BLOCK;
            }
            if (MAKEFOURCC( 'D', 'X', 'T', '4' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC3_UNORM_BLOCK;
            }

            if (MAKEFOURCC( 'A', 'T', 'I', '1' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC4_UNORM_BLOCK;
            }
            if (MAKEFOURCC( 'B', 'C', '4', 'U' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC4_UNORM_BLOCK;
            }
            if (MAKEFOURCC( 'B', 'C', '4', 'S' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC4_SNORM_BLOCK;
            }

            if (MAKEFOURCC( 'A', 'T', 'I', '2' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC5_UNORM_BLOCK;
            }
            if (MAKEFOURCC( 'B', 'C', '5', 'U' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC5_UNORM_BLOCK;
            }
            if (MAKEFOURCC( 'B', 'C', '5', 'S' ) == ddpf.fourCC)
            {
                return VK_FORMAT_BC5_SNORM_BLOCK;
            }

            // BC6H and BC7 are written using the "DX10" extended header

            if (MAKEFOURCC( 'R', 'G', 'B', 'G' ) == ddpf.fourCC)
            {
                return VK_FORMAT_G8B8G8R8_422_UNORM;
            }
            if (MAKEFOURCC( 'G', 'R', 'G', 'B' ) == ddpf.fourCC)
            {
                return VK_FORMAT_B8G8R8G8_422_UNORM;
            }

#if defined(VK_VERSION_1_1) && VK_VERSION_1_1
            if (MAKEFOURCC('U', 'Y', 'V', 'Y') == ddpf.fourCC)
            {
                return VK_FORMAT_G8B8G8R8_422_UNORM;
            }

            if (MAKEFOURCC('Y', 'U', 'Y', '2') == ddpf.fourCC)
            {
                return VK_FORMAT_B8G8R8G8_422_UNORM;
            }
#elif defined(VK_KHR_sampler_ycbcr_conversion)
            if (MAKEFOURCC('U', 'Y', 'V', 'Y') == ddpf.fourCC)
            {
                return VK_FORMAT_G8B8G8R8_422_UNORM_KHR;
            }

            if (MAKEFOURCC('Y', 'U', 'Y', '2') == ddpf.fourCC)
            {
                return VK_FORMAT_G8B8G8R8_422_UNORM_KHR;
            }
#endif // #if defined(VK_VERSION_1_1) && VK_VERSION_1_1

            // Check for D3DFORMAT enums being set here
            switch( ddpf.fourCC )
            {
            case 36: // D3DFMT_A16B16G16R16
                return VK_FORMAT_R16G16B16A16_UNORM;

            case 110: // D3DFMT_Q16W16V16U16
                return VK_FORMAT_R16G16B16A16_SNORM;

            case 111: // D3DFMT_R16F
                return VK_FORMAT_R16_SFLOAT;

            case 112: // D3DFMT_G16R16F
                return VK_FORMAT_R16G16_SFLOAT;

            case 113: // D3DFMT_A16B16G16R16F
                return VK_FORMAT_R16G16B16A16_SFLOAT;

            case 114: // D3DFMT_R32F
                return VK_FORMAT_R32_SFLOAT;

            case 115: // D3DFMT_G32R32F
                return VK_FORMAT_R32G32_SFLOAT;

            case 116: // D3DFMT_A32B32G32R32F
                return VK_FORMAT_R32G32B32A32_SFLOAT;

            // No VK format maps to D3DFMT_CxV8U8
            }
        }

        return VK_FORMAT_UNDEFINED;
    }

    #undef ISBITMASK


    //--------------------------------------------------------------------------------------
    VkFormat MakeSRGB(VkFormat format ) noexcept
    {
        switch( format )
        {
        case VK_FORMAT_R8_UNORM:
            return VK_FORMAT_R8_SRGB;

        case VK_FORMAT_R8G8_UNORM:
            return VK_FORMAT_R8G8_SRGB;

        case VK_FORMAT_R8G8B8_UNORM:
            return VK_FORMAT_R8G8B8_SRGB;

        case VK_FORMAT_B8G8R8_UNORM:
            return VK_FORMAT_B8G8R8_SRGB;

        case VK_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_SRGB;

        case VK_FORMAT_B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_SRGB;

        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
            return VK_FORMAT_A8B8G8R8_SRGB_PACK32;

        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            return VK_FORMAT_BC1_RGB_SRGB_BLOCK;

        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;

        case VK_FORMAT_BC2_UNORM_BLOCK:
            return VK_FORMAT_BC2_SRGB_BLOCK;

        case VK_FORMAT_BC3_UNORM_BLOCK:
            return VK_FORMAT_BC3_SRGB_BLOCK;

        case VK_FORMAT_BC7_UNORM_BLOCK:
            return VK_FORMAT_BC7_SRGB_BLOCK;

        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
            return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;

        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
            return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;

        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
            return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;

        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
            return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;

        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
            return VK_FORMAT_ASTC_5x4_SRGB_BLOCK;

        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
            return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;

        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
            return VK_FORMAT_ASTC_6x5_SRGB_BLOCK;

        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
            return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;

        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
            return VK_FORMAT_ASTC_8x5_SRGB_BLOCK;

        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
            return VK_FORMAT_ASTC_8x6_SRGB_BLOCK;

        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
            return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;

        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
            return VK_FORMAT_ASTC_10x5_SRGB_BLOCK;

        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
            return VK_FORMAT_ASTC_10x6_SRGB_BLOCK;

        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
            return VK_FORMAT_ASTC_10x8_SRGB_BLOCK;

        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
            return VK_FORMAT_ASTC_10x10_SRGB_BLOCK;

        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
            return VK_FORMAT_ASTC_12x10_SRGB_BLOCK;

        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
            return VK_FORMAT_ASTC_12x12_SRGB_BLOCK;

#ifdef VK_IMG_format_pvrtc
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
            return VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG;

        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
            return VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG;

        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG:
            return VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG;

        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
            return VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG;
#endif

        default:
            return format;
        }
    }


    //--------------------------------------------------------------------------------------
    inline bool IsDepthStencil(VkFormat fmt) noexcept
    {
        switch (fmt)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;

        default:
            return false;
        }
    }

    //--------------------------------------------------------------------------------------
    DDS_LOADER_RESULT FillInitData(size_t width,
        size_t height,
        size_t depth,
        size_t mipCount,
        size_t arraySize,
        size_t numberOfPlanes,
        VkFormat format,
        size_t maxsize,
        size_t bitSize,
        const uint8_t* bitData,
        size_t& twidth,
        size_t& theight,
        size_t& tdepth,
        size_t& skipMip,
        std::vector<LoadedSubresourceData>& initData)
    {
        if (!bitData)
        {
            return DDS_LOADER_BAD_POINTER;
        }

        skipMip = 0;
        twidth = 0;
        theight = 0;
        tdepth = 0;

        size_t NumBytes = 0;
        size_t RowBytes = 0;
        const uint8_t* pEndBits = bitData + bitSize;

        initData.clear();

        for (size_t p = 0; p < numberOfPlanes; ++p)
        {
            VkImageAspectFlags aspectPlane = VK_IMAGE_ASPECT_FLAG_BITS_MAX_ENUM;
            if(numberOfPlanes == 1 && !IsDepthStencil(format))
            {
                aspectPlane = VK_IMAGE_ASPECT_COLOR_BIT;
            }
            else if(numberOfPlanes == 1)
            {
                //No separate depth/stencil
                aspectPlane = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            else if(p == 0)
            {
                aspectPlane = VK_IMAGE_ASPECT_PLANE_0_BIT;
            }
            else if(p == 1)
            {
                aspectPlane = VK_IMAGE_ASPECT_PLANE_1_BIT;
            }
            else if(p == 2)
            {
                aspectPlane = VK_IMAGE_ASPECT_PLANE_2_BIT;
            }

            assert(aspectPlane != VK_IMAGE_ASPECT_FLAG_BITS_MAX_ENUM);

            const uint8_t* pSrcBits = bitData;

            for (size_t j = 0; j < arraySize; j++)
            {
                size_t w = width;
                size_t h = height;
                size_t d = depth;
                
                for (size_t i = 0; i < mipCount; i++)
                {
                    DDS_LOADER_RESULT surfInfoRes = GetSurfaceInfo(w, h, format, aspectPlane, &NumBytes, &RowBytes, nullptr);
                    if(surfInfoRes != DDS_LOADER_SUCCESS)
                    {
                        return surfInfoRes;
                    }

                    if(NumBytes > UINT32_MAX)
                    {
                        return DDS_LOADER_ARITHMETIC_OVERFLOW;
                    }

                    size_t dataSize = NumBytes * d;

                    if ((mipCount <= 1) || !maxsize || (w <= maxsize && h <= maxsize && d <= maxsize))
                    {
                        if (!twidth)
                        {
                            twidth  = w;
                            theight = h;
                            tdepth  = d;
                        }

                        LoadedSubresourceData res;
                        res.PData                       = pSrcBits;
                        res.DataByteSize                = dataSize;
                        res.SubresourceSlice.aspectMask = aspectPlane;
                        res.SubresourceSlice.arrayLayer = (uint32_t)j;
                        res.SubresourceSlice.mipLevel   = (uint32_t)i;
                        res.Extent.width                = (uint32_t)w;
                        res.Extent.height               = (uint32_t)h;
                        res.Extent.depth                = (uint32_t)d;

                        initData.emplace_back(res);
                    }
                    else if (!j)
                    {
                        // Count number of skipped mipmaps (first item only)
                        ++skipMip;
                    }

                    if(pSrcBits + (NumBytes*d) > pEndBits)
                    {
                        return DDS_LOADER_UNEXPECTED_EOF;
                    }

                    pSrcBits += NumBytes * d;

                    w = w >> 1;
                    h = h >> 1;
                    d = d >> 1;
                    if (w == 0)
                    {
                        w = 1;
                    }
                    if (h == 0)
                    {
                        h = 1;
                    }
                    if (d == 0)
                    {
                        d = 1;
                    }
                }
            }
        }

        return initData.empty() ? DDS_LOADER_FAIL : DDS_LOADER_SUCCESS;
    }


    //--------------------------------------------------------------------------------------
    DDS_LOADER_RESULT CreateTextureResource(
        VkDevice vkDevice,
        VkImageType imgType,
        size_t width,
        size_t height,
        size_t depth,
        size_t mipCount,
        size_t arraySize,
        VkFormat format,
        VkImageUsageFlags usageFlags,
        VkImageCreateFlags createFlags,
        unsigned int loadFlags,
        const VkAllocationCallbacks* allocator,
        VkImage* texture,
        VkImageCreateInfo* outImageCreateInfo) noexcept
    {
        if (!vkDevice)
            return DDS_LOADER_BAD_POINTER;

        DDS_LOADER_RESULT result = DDS_LOADER_FAIL;

        if(loadFlags & DDS_LOADER_FORCE_SRGB)
        {
            format = MakeSRGB(format);
        }

        VkImageCreateInfo imageCreateInfo;
        imageCreateInfo.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.pNext                 = nullptr;
        imageCreateInfo.flags                 = createFlags;
        imageCreateInfo.imageType             = imgType;
        imageCreateInfo.format                = format;
        imageCreateInfo.extent.width          = static_cast<uint32_t>(width);
        imageCreateInfo.extent.height         = static_cast<uint32_t>(height);
        imageCreateInfo.extent.depth          = static_cast<uint32_t>(depth);
        imageCreateInfo.mipLevels             = static_cast<uint32_t>(mipCount);
        imageCreateInfo.arrayLayers           = static_cast<uint32_t>(arraySize);
        imageCreateInfo.samples               = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling                = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.usage                 = usageFlags;
        imageCreateInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.queueFamilyIndexCount = 0;
        imageCreateInfo.pQueueFamilyIndices   = nullptr;
        imageCreateInfo.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;

        if(outImageCreateInfo != nullptr)
        {
            *outImageCreateInfo = imageCreateInfo;
        }

        if(vkCreateImage != nullptr)
        {
            VkResult vkRes = vkCreateImage(vkDevice, &imageCreateInfo, allocator, texture);

            //This function only returns VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY
            switch(vkRes)
            {
            case VK_SUCCESS:
                result = DDS_LOADER_SUCCESS;
                break;
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                result = DDS_LOADER_NO_HOST_MEMORY;
                break;
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                result = DDS_LOADER_NO_DEVICE_MEMORY;
                break;
            default:
                break;
            }
        }
        else
        {
            result = DDS_LOADER_NO_FUNCTION;
        }

        if(result == DDS_LOADER_SUCCESS)
        {
            assert(texture != nullptr && *texture != nullptr);

            SetDebugObjectName(vkDevice, *texture, "DDSTextureLoader");
        }

        return result;
    }

    //--------------------------------------------------------------------------------------
    DDS_LOADER_RESULT CreateTextureFromDDS(VkDevice vkDevice,
        const DDS_HEADER* header,
        const uint8_t* bitData,
        size_t bitSize,
        size_t maxsize,
        const VkPhysicalDeviceLimits* deviceLimits,
        VkImageUsageFlags usageFlags,
        VkImageCreateFlags createFlags,
        unsigned int loadFlags,
        VkAllocationCallbacks* allocationCallbacks,
        VkImage* texture,
        std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
        VkImageCreateInfo* outImageCreateInfo) noexcept(false)
    {
        DDS_LOADER_RESULT errCode = DDS_LOADER_SUCCESS;

        uint32_t width  = header->width;
        uint32_t height = header->height;
        uint32_t depth  = header->depth;

        VkImageType imgType = VK_IMAGE_TYPE_2D;
        uint32_t arraySize = 1;
        VkFormat format = VK_FORMAT_UNDEFINED;

        size_t mipCount = header->mipMapCount;
        if (0 == mipCount)
        {
            mipCount = 1;
        }

        //Minimal guaranteed supported limits (refer to Table 49. Required Limits in Vulkan specification)
        uint32_t maxImageArrayLayers   = 256;
        uint32_t maxImageDimension1D   = 4096;
        uint32_t maxImageDimension2D   = 4096;
        uint32_t maxImageDimension3D   = 256;
        uint32_t maxImageDimensionCube = 4096;
        if(deviceLimits)
        {
            maxImageArrayLayers   = deviceLimits->maxImageArrayLayers;
            maxImageDimension1D   = deviceLimits->maxImageDimension1D;
            maxImageDimension2D   = deviceLimits->maxImageDimension2D;
            maxImageDimension3D   = deviceLimits->maxImageDimension3D;
            maxImageDimensionCube = deviceLimits->maxImageDimensionCube;
        }

        VkImageCreateFlags imageCreateFlags = createFlags;

        if ((header->ddspf.flags & DDS_FOURCC) &&
            (MAKEFOURCC('D', 'X', '1', '0') == header->ddspf.fourCC))
        {
            auto d3d10ext = reinterpret_cast<const DDS_HEADER_DXT10*>(reinterpret_cast<const char*>(header) + sizeof(DDS_HEADER));

            arraySize = d3d10ext->arraySize;
            if(arraySize == 0)
            {
                return DDS_LOADER_INVALID_DATA;
            }

            if(IsTypelessFormat(d3d10ext->dxgiFormat))
            {
                imageCreateFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            }

            format = DXGIToVkFormat(d3d10ext->dxgiFormat);
            if(BitsPerPixel(format) == 0)
            {
                return DDS_LOADER_UNSUPPORTED_FORMAT;
            }

            VkImageType imgTypeFromResDim = D3DResourceDimensionToImageType(d3d10ext->resourceDimension);
            switch(imgTypeFromResDim)
            {
            case VK_IMAGE_TYPE_1D:
                // D3DX writes 1D textures with a fixed Height of 1
                if ((header->flags & DDS_HEIGHT) && height != 1)
                {
                    return DDS_LOADER_INVALID_DATA;
                }
                height = depth = 1;
                break;

            case VK_IMAGE_TYPE_2D:
                if (d3d10ext->miscFlag & 0x4 /* RESOURCE_MISC_TEXTURECUBE */)
                {
                    imageCreateFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                    arraySize *= 6;
                }

                if(arraySize > 1)
                {
                    imageCreateFlags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
                }

                depth = 1;
                break;

            case VK_IMAGE_TYPE_3D:
                if (!(header->flags & DDS_HEADER_FLAGS_VOLUME))
                {
                    return DDS_LOADER_INVALID_DATA;
                }

                if (arraySize > 1)
                {
                    return DDS_LOADER_UNSUPPORTED_LAYOUT;
                }
                break;

            default:
                return DDS_LOADER_UNSUPPORTED_LAYOUT;
            }

            imgType = imgTypeFromResDim;
        }
        else
        {
            format = GetVkFormat(header->ddspf);

            if(format == VK_FORMAT_UNDEFINED)
            {
                return DDS_LOADER_UNSUPPORTED_FORMAT;
            }

            if(header->flags & DDS_HEADER_FLAGS_VOLUME)
            {
                imgType = VK_IMAGE_TYPE_3D;
            }
            else
            {
                if (header->caps2 & DDS_CUBEMAP)
                {
                    // We require all six faces to be defined
                    if ((header->caps2 & DDS_CUBEMAP_ALLFACES) != DDS_CUBEMAP_ALLFACES)
                    {
                        return DDS_LOADER_UNSUPPORTED_LAYOUT;
                    }

                    arraySize = 6;
                    imageCreateFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                }

                depth = 1;
                imgType = VK_IMAGE_TYPE_2D;

                // Note there's no way for a legacy Direct3D 9 DDS to express a '1D' texture
            }

            assert(BitsPerPixel(format) != 0);
        }

        // Bound sizes (for security purposes we don't trust DDS file metadata larger than the Direct3D hardware requirements)
        // Vulkan does not have an easy way to obtain this informtation - there's no corresponding VkPhysicalDeviceLimits entry
        constexpr uint32_t maxDirect3DMips = 15;
        if (mipCount > maxDirect3DMips /*D3D12_REQ_MIP_LEVELS*/ )
        {
            return DDS_LOADER_UNSUPPORTED_LAYOUT;
        }

        switch (imgType)
        {
        case VK_IMAGE_TYPE_1D:
            if ((arraySize > maxImageArrayLayers) ||
                (width > maxImageDimension1D))
            {
                return DDS_LOADER_BELOW_LIMITS;
            }
            break;

        case VK_IMAGE_TYPE_2D:
            if (imageCreateFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
            {
                // This is the right bound because we set arraySize to (NumCubes*6) above
                if ((arraySize > maxImageArrayLayers) ||
                    (width > maxImageDimensionCube) ||
                    (height > maxImageDimensionCube))
                {
                    return DDS_LOADER_BELOW_LIMITS;
                }
            }
            else if ((arraySize > maxImageArrayLayers) ||
                     (width > maxImageDimension2D) ||
                     (height > maxImageDimension2D))
            {
                return DDS_LOADER_BELOW_LIMITS;
            }
            break;

        case VK_IMAGE_TYPE_3D:
            if ((arraySize > 1) ||
                (width > maxImageDimension3D) ||
                (height > maxImageDimension3D) ||
                (depth > maxImageDimension3D))
            {
                return DDS_LOADER_BELOW_LIMITS;
            }
            break;

        default:
            return DDS_LOADER_UNSUPPORTED_LAYOUT;
        }

        uint32_t numberOfPlanes = GetVkFormatPlaneCount(format);
        if(numberOfPlanes == 0)
        {
            return DDS_LOADER_UNSUPPORTED_FORMAT;
        }

        if ((numberOfPlanes > 1) && IsDepthStencil(format))
        {
            // For the future (at the moment of writing this Vulkan doesn't have any multi-planar depth-stencil format support)
            return DDS_LOADER_UNSUPPORTED_FORMAT;
        }

        // Create the texture
        size_t numberOfResources = (imgType == VK_IMAGE_TYPE_3D)
                                   ? 1 : arraySize;
        numberOfResources *= mipCount;
        numberOfResources *= numberOfPlanes;

        //Vulkan doesn't have any subresource number limit
        subresources.reserve(numberOfResources);

        size_t skipMip = 0;
        size_t twidth = 0;
        size_t theight = 0;
        size_t tdepth = 0;
        errCode = FillInitData(width, height, depth, mipCount, arraySize,
            numberOfPlanes, format,
            maxsize, bitSize, bitData,
            twidth, theight, tdepth, skipMip, subresources);

        if (errCode == DDS_LOADER_SUCCESS)
        {
            size_t reservedMips = mipCount;
            if (loadFlags & DDS_LOADER_MIP_RESERVE)
            {
                reservedMips = std::min<size_t>(maxDirect3DMips,
                    CountMips(width, height));
            }

            errCode = CreateTextureResource(vkDevice, imgType, twidth, theight, tdepth, reservedMips - skipMip, arraySize,
                format, usageFlags, imageCreateFlags, loadFlags, allocationCallbacks, texture, outImageCreateInfo);

            if (errCode != DDS_LOADER_SUCCESS && !maxsize && (mipCount > 1))
            {
                subresources.clear();

                maxsize = static_cast<size_t>(
                    (imgType == VK_IMAGE_TYPE_3D)
                    ? maxImageDimension3D
                    : maxImageDimension2D);

                errCode = FillInitData(width, height, depth, mipCount, arraySize,
                    numberOfPlanes, format,
                    maxsize, bitSize, bitData,
                    twidth, theight, tdepth, skipMip, subresources);
                if (errCode == DDS_LOADER_SUCCESS)
                {
                    errCode = CreateTextureResource(vkDevice, imgType, twidth, theight, tdepth, mipCount - skipMip, arraySize,
                        format, usageFlags, imageCreateFlags, loadFlags, allocationCallbacks, texture, outImageCreateInfo);
                }
            }
        }

        if (errCode != DDS_LOADER_SUCCESS)
        {
            subresources.clear();
        }

        return errCode;
    }

    //--------------------------------------------------------------------------------------
    DDS_ALPHA_MODE GetAlphaMode( _In_ const DDS_HEADER* header ) noexcept
    {
        if ( header->ddspf.flags & DDS_FOURCC )
        {
            if ( MAKEFOURCC( 'D', 'X', '1', '0' ) == header->ddspf.fourCC )
            {
                auto d3d10ext = reinterpret_cast<const DDS_HEADER_DXT10*>(reinterpret_cast<const uint8_t*>(header) + sizeof(DDS_HEADER));
                auto mode = static_cast<DDS_ALPHA_MODE>( d3d10ext->miscFlags2 & DDS_MISC_FLAGS2_ALPHA_MODE_MASK );
                switch( mode )
                {
                case DDS_ALPHA_MODE_STRAIGHT:
                case DDS_ALPHA_MODE_PREMULTIPLIED:
                case DDS_ALPHA_MODE_OPAQUE:
                case DDS_ALPHA_MODE_CUSTOM:
                    return mode;

                case DDS_ALPHA_MODE_UNKNOWN:
                default:
                    break;
                }
            }
            else if ( ( MAKEFOURCC( 'D', 'X', 'T', '2' ) == header->ddspf.fourCC )
                      || ( MAKEFOURCC( 'D', 'X', 'T', '4' ) == header->ddspf.fourCC ) )
            {
                return DDS_ALPHA_MODE_PREMULTIPLIED;
            }
        }

        return DDS_ALPHA_MODE_UNKNOWN;
    }

    //--------------------------------------------------------------------------------------
    void SetDebugTextureInfo(
        VkDevice device,
        const char* fileName,
        VkImage image) noexcept
    {
#if defined(VK_EXT_debug_utils) && !defined(NO_VK_DEBUG_NAME) && ( defined(_DEBUG) || defined(PROFILE) )
        if(image != VK_NULL_HANDLE)
        {
            const char* pstrName = strrchr(fileName, '\\');
            if (!pstrName)
            {
                pstrName = fileName;
            }
            else
            {
                pstrName++;
            }

            VkDebugUtilsObjectNameInfoEXT debugObjectNameInfo;
            debugObjectNameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
            debugObjectNameInfo.pNext        = nullptr;
            debugObjectNameInfo.objectType   = VK_OBJECT_TYPE_IMAGE;
            debugObjectNameInfo.objectHandle = (uint64_t)image;
            debugObjectNameInfo.pObjectName  = fileName;

            if(vkSetDebugUtilsObjectNameEXT != nullptr)
            {
                vkSetDebugUtilsObjectNameEXT(device, &debugObjectNameInfo);
            }
        }
#else
        UNREFERENCED_PARAMETER(device);
        UNREFERENCED_PARAMETER(fileName);
        UNREFERENCED_PARAMETER(image);
#endif
    }
} // anonymous namespace

#ifdef VK_NO_PROTOTYPES

    void DDSTextureLoaderVk::SetVkCreateImageFuncPtr(PFN_vkCreateImage funcPtr)
    {
        vkCreateImage = funcPtr;
    }

    void DDSTextureLoaderVk::SetVkCreateImageFuncPtrWithUserPtr(DDSTextureLoaderVk::PFN_DdsLoader_vkCreateImageUserPtr funcPtr)
    {
        vkCreateImageWithUserPtr = funcPtr;

        vkCreateImage = [](VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage)
        {
            return vkCreateImageWithUserPtr(vkCreateImageUserPtr, device, pCreateInfo, pAllocator, pImage);
        };
    }

    void DDSTextureLoaderVk::SetVkCreateImageUserPtr(void* userPtr)
    {
        vkCreateImageUserPtr = userPtr;
    }

#ifdef VK_EXT_debug_utils

    void DDSTextureLoaderVk::SetVkSetDebugUtilsObjectNameFuncPtr(PFN_vkSetDebugUtilsObjectNameEXT funcPtr)
    {
        vkSetDebugUtilsObjectNameEXT = funcPtr;
    }

    void DDSTextureLoaderVk::SetVkSetDebugUtilsObjectNameFuncPtrWithUserPtr(DDSTextureLoaderVk::PFN_DdsLoader_vkSetDebugUtilsObjectNameUserPtr funcPtr)
    {
        vkSetDebugUtilsObjectNameEXTWithUserPtr = funcPtr;

        vkSetDebugUtilsObjectNameEXT = [](VkDevice device, const VkDebugUtilsObjectNameInfoEXT* pNameInfo)
        {
            return vkSetDebugUtilsObjectNameEXTWithUserPtr(vkSetDebugUtilsObjectNameEXTUserPtr, device, pNameInfo);
        };
    }

    void DDSTextureLoaderVk::SetVkSetDebugUtilsObjectNameUserPtr(void* userPtr)
    {
        vkSetDebugUtilsObjectNameEXTUserPtr = userPtr;
    }

#endif // VK_EXT_debug_utils
#endif // VK_NO_PROTOTYPES

std::string DDSLoaderResultToString(DDS_LOADER_RESULT errorCode)
{
    switch (errorCode)
    {
    case DDSTextureLoaderVk::DDS_LOADER_SUCCESS:
        return "Operation was successful.";
    case DDSTextureLoaderVk::DDS_LOADER_FAIL:
        return "Unexpected failure when reading the file.";
    case DDSTextureLoaderVk::DDS_LOADER_BAD_POINTER:
        return "Incorrect pointer has been passed to the function.";
    case DDSTextureLoaderVk::DDS_LOADER_INVALID_ARG:
        return "Incorrect argument has been passed to the function.";
    case DDSTextureLoaderVk::DDS_LOADER_INVALID_DATA:
        return "File contains invalid information.";
    case DDSTextureLoaderVk::DDS_LOADER_UNEXPECTED_EOF:
        return "Unexpected end of file.";
    case DDSTextureLoaderVk::DDS_LOADER_UNSUPPORTED_FORMAT:
        return "The image has unsupported format.";
    case DDSTextureLoaderVk::DDS_LOADER_UNSUPPORTED_LAYOUT:
        return "The image has incorrect or unsupported layout.";
    case DDSTextureLoaderVk::DDS_LOADER_BELOW_LIMITS:
        return "The image dimensions exceed the given device limits. Note that if you don't pass VkPhysicalDeviceLimits* parameter, the limits are set to minimum possible.";
    case DDSTextureLoaderVk::DDS_LOADER_NO_HOST_MEMORY:
        return "Out of system memory.";
    case DDSTextureLoaderVk::DDS_LOADER_NO_DEVICE_MEMORY:
        return "Out of video memory.";
    case DDSTextureLoaderVk::DDS_LOADER_NO_FUNCTION:
        return "The function vkCreateImage() has not been loaded. Please use SetVkCreateImageFuncPtr() or SetVkCreateImageFuncPtrWithUserPtr()+SetVkCreateImageUserPtr() to pass the function to the loader.";
    case DDSTextureLoaderVk::DDS_LOADER_ARITHMETIC_OVERFLOW:
        return "Unexpected arithmetic overflow when reading the file.";
    default:
        return "Unknown error code.";
    }
}

//--------------------------------------------------------------------------------------
DDS_LOADER_RESULT DDSTextureLoaderVk::LoadDDSTextureFromMemory(
    VkDevice vkDevice,
    const uint8_t* ddsData,
    size_t ddsDataSize,
    VkImage* texture,
    std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
    size_t maxsize,
    VkImageCreateInfo* outImageCreateInfo,
    DDS_ALPHA_MODE* alphaMode)
{
    return LoadDDSTextureFromMemoryEx(
        vkDevice,
        ddsData,
        ddsDataSize,
        maxsize,
        nullptr,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        0,
        DDS_LOADER_DEFAULT,
        nullptr,
        texture,
        subresources,
        outImageCreateInfo,
        alphaMode);
}


DDS_LOADER_RESULT DDSTextureLoaderVk::LoadDDSTextureFromMemoryEx(
    VkDevice vkDevice,
    const uint8_t* ddsData,
    size_t ddsDataSize,
    size_t maxsize,
    const VkPhysicalDeviceLimits* deviceLimits,
    VkImageUsageFlags usageFlags,
    VkImageCreateFlags createFlags,
    unsigned int loadFlags,
    VkAllocationCallbacks* allocator,
    VkImage* texture,
    std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
    VkImageCreateInfo* outImageCreateInfo,
    DDS_ALPHA_MODE* alphaMode)
{
    if (texture)
    {
        *texture = nullptr;
    }
    if (alphaMode)
    {
        *alphaMode = DDS_ALPHA_MODE_UNKNOWN;
    }
    if (outImageCreateInfo)
    {
        memset(outImageCreateInfo, 0, sizeof(VkImageCreateInfo));
    }

    if(!vkDevice || !ddsData || !texture)
    {
        return DDS_LOADER_INVALID_ARG;
    }

    // Validate DDS file in memory
    const DDS_HEADER* header = nullptr;
    const uint8_t* bitData = nullptr;
    size_t bitSize = 0;

    DDS_LOADER_RESULT errCode = LoadTextureDataFromMemory(ddsData,
        ddsDataSize,
        &header,
        &bitData,
        &bitSize
    );
    if (errCode != DDS_LOADER_SUCCESS)
    {
        return errCode;
    }

    errCode = CreateTextureFromDDS(vkDevice,
        header, bitData, bitSize, maxsize,
        deviceLimits, usageFlags, createFlags, loadFlags,
        allocator, texture, subresources, outImageCreateInfo);
    if (errCode == DDS_LOADER_SUCCESS)
    {
        if (texture && *texture)
        {
            SetDebugObjectName(vkDevice, *texture, "DDSTextureLoader");
        }

        if (alphaMode)
            *alphaMode = GetAlphaMode(header);
    }

    return errCode;
}


//--------------------------------------------------------------------------------------
DDS_LOADER_RESULT DDSTextureLoaderVk::LoadDDSTextureFromFile(
    VkDevice vkDevice,
    const char_type* fileName,
    VkImage* texture,
    std::unique_ptr<uint8_t[]>& ddsData,
    std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
    size_t maxsize,
    VkImageCreateInfo* outImageCreateInfo,
    DDS_ALPHA_MODE* outAlphaMode)
{
    return LoadDDSTextureFromFileEx(
        vkDevice,
        fileName,
        maxsize,
        nullptr,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        0,
        DDS_LOADER_DEFAULT,
        nullptr,
        texture,
        ddsData,
        subresources,
        outImageCreateInfo,
        outAlphaMode);
}

DDS_LOADER_RESULT DDSTextureLoaderVk::LoadDDSTextureFromFileEx(
    VkDevice vkDevice,
    const char_type* fileName,
    size_t maxsize,
    const VkPhysicalDeviceLimits* deviceLimits,
    VkImageUsageFlags usageFlags,
    VkImageCreateFlags createFlags,
    unsigned int loadFlags,
    VkAllocationCallbacks* allocator,
    VkImage* texture,
    std::unique_ptr<uint8_t[]>& ddsData,
    std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
    VkImageCreateInfo* outImageCreateInfo,
    DDS_ALPHA_MODE* outAlphaMode)
{
    if (texture)
    {
        *texture = nullptr;
    }
    if (outAlphaMode)
    {
        *outAlphaMode = DDS_ALPHA_MODE_UNKNOWN;
    }
    if (outImageCreateInfo)
    {
        memset(outImageCreateInfo, 0, sizeof(VkImageCreateInfo));
    }

    if (!vkDevice || !fileName || !texture)
    {
        return DDS_LOADER_INVALID_ARG;
    }

    const DDS_HEADER* header = nullptr;
    const uint8_t* bitData = nullptr;
    size_t bitSize = 0;

    DDS_LOADER_RESULT errCode = LoadTextureDataFromFile(fileName,
        ddsData,
        &header,
        &bitData,
        &bitSize
    );
    if (errCode != DDS_LOADER_SUCCESS)
    {
        return errCode;
    }

    errCode = CreateTextureFromDDS(vkDevice,
        header, bitData, bitSize, maxsize,
        deviceLimits,
        usageFlags, createFlags, loadFlags,
        allocator, texture, subresources, outImageCreateInfo);

    if (errCode == DDS_LOADER_SUCCESS)
    {
        #if defined(WIN32) && defined(DDS_LOADER_PATH_WIDE_CHAR)
            int filenameSize = WideCharToMultiByte(CP_UTF8, 0, fileName, -1, nullptr, 0, nullptr, nullptr);

            char* filenameU8 = (char*)_malloca(filenameSize + 1);
            WideCharToMultiByte(CP_UTF8, 0, fileName, -1, filenameU8, filenameSize + 1, nullptr, nullptr);

            SetDebugTextureInfo(vkDevice, filenameU8, *texture);
        #else
            SetDebugTextureInfo(vkDevice, fileName, *texture);
        #endif // _WIN32

    
        if (outAlphaMode)
            *outAlphaMode = GetAlphaMode(header);
    }

    return errCode;
}
