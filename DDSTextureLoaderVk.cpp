//--------------------------------------------------------------------------------------
// File: DDSTextureLoader12.cpp
//
// Functions for loading a DDS texture and creating a Vulkan runtime resource for it
//
// Heavily based on Microsoft's DDSTextureLoader12: https://github.com/microsoft/DirectXTex/tree/master/DDSTextureLoader
// Licensed under the MIT License.
//
//--------------------------------------------------------------------------------------

#include "DDSTextureLoaderVk.h"
#include "DDSVulkanFunctionsInclude.h"

#include <assert.h>
#include <algorithm>
#include <fstream>
#include <memory>

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

using namespace Vulkan;

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

#define DDS_FOURCC      0x00000004  // DDPF_FOURCC
#define DDS_RGB         0x00000040  // DDPF_RGB
#define DDS_LUMINANCE   0x00020000  // DDPF_LUMINANCE
#define DDS_ALPHA       0x00000002  // DDPF_ALPHA
#define DDS_BUMPDUDV    0x00080000  // DDPF_BUMPDUDV

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
    inline void IssueWarning(const char* wng)
    {
        #if !defined(DDS_NO_ISSUE_LOGGING) && defined(_DEBUG) && defined(_WIN32)
            OutputDebugStringA(wng);
        #elif !defined(DDS_NO_ISSUE_LOGGING) && defined(_DEBUG)
            std::wprintf(wng);
        #endif
    }

    template<uint32_t TNameLength>
    inline void SetDebugObjectName(VkDevice device, VkImage image, const char(&name)[TNameLength]) noexcept
    {
        #if !defined(NO_VK_DEBUG_NAME) && defined(VK_EXT_debug_utils) && (defined(_DEBUG) || defined(PROFILE))
            VkDebugUtilsObjectNameInfoEXT debugObjectNameInfo;   
            debugObjectNameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
            debugObjectNameInfo.pNext        = nullptr;
            debugObjectNameInfo.objectType   = VK_OBJECT_TYPE_IMAGE;
            debugObjectNameInfo.objectHandle = (uint64_t)image;
            debugObjectNameInfo.pObjectName  = name;

            vkSetDebugUtilsObjectNameEXT(device, &debugObjectNameInfo);
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
    VkResult LoadTextureDataFromMemory(
        const uint8_t* ddsData,
        size_t ddsDataSize,
        const DDS_HEADER** header,
        const uint8_t** bitData,
        size_t* bitSize) noexcept
    {
        if (!header || !bitData || !bitSize)
        {
            return VK_ERROR_UNKNOWN;
        }

        if (ddsDataSize > UINT32_MAX)
        {
            return VK_ERROR_UNKNOWN;
        }

        if (ddsDataSize < (sizeof(uint32_t) + sizeof(DDS_HEADER)))
        {
            return VK_ERROR_UNKNOWN;
        }

        // DDS files always start with the same magic number ("DDS ")
        auto dwMagicNumber = *reinterpret_cast<const uint32_t*>(ddsData);
        if (dwMagicNumber != DDS_MAGIC)
        {
            return VK_ERROR_UNKNOWN;
        }

        auto hdr = reinterpret_cast<const DDS_HEADER*>(ddsData + sizeof(uint32_t));

        // Verify header to validate DDS file
        if (hdr->size != sizeof(DDS_HEADER) ||
            hdr->ddspf.size != sizeof(DDS_PIXELFORMAT))
        {
            return VK_ERROR_UNKNOWN;
        }

        // Check for DX10 extension
        bool bDXT10Header = false;
        if ((hdr->ddspf.flags & DDS_FOURCC) &&
            (MAKEFOURCC('D', 'X', '1', '0') == hdr->ddspf.fourCC))
        {
            // Must be long enough for both headers and magic value
            if (ddsDataSize < (sizeof(DDS_HEADER) + sizeof(uint32_t) + sizeof(DDS_HEADER_DXT10)))
            {
                return VK_ERROR_UNKNOWN;
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

        return VK_SUCCESS;
    }


    //--------------------------------------------------------------------------------------
    VkResult LoadTextureDataFromFile(
        const char_type* fileName,
        std::unique_ptr<uint8_t[]>& ddsData,
        const DDS_HEADER** header,
        const uint8_t** bitData,
        size_t* bitSize) noexcept
    {
        if (!header || !bitData || !bitSize)
        {
            return VK_ERROR_UNKNOWN;
        }

        // open the file
        std::ifstream hFile(fileName, std::ios::binary);
        if(!hFile)
        {
            return VK_ERROR_UNKNOWN;
        }

        // Get the file size
        std::streampos fileBegin = hFile.tellg();
        hFile.seekg(hFile.end);
        std::streampos fileEnd = hFile.tellg();
        hFile.seekg(hFile.beg);

        size_t fileSize = fileEnd - fileBegin;
        
        uint32_t fileSizeHighPart = (uint32_t)((fileSize && 0xffffffff00000000ull) >> 32);
        uint32_t fileSizeLowPart  = (uint32_t)((fileSize && 0x00000000ffffffffull) >>  0);

        // File is too big for 32-bit allocation, so reject read
        if(fileSizeHighPart > 0)
        {
            return VK_ERROR_UNKNOWN;
        }

        // Need at least enough data to fill the header and magic number to be a valid DDS
        if(fileSizeLowPart < (sizeof(DDS_HEADER) + sizeof(uint32_t)))
        {
            return VK_ERROR_UNKNOWN;
        }

        // create enough space for the file data
        ddsData.reset(new (std::nothrow) uint8_t[fileSizeLowPart]);
        if (!ddsData)
        {
            return VK_ERROR_UNKNOWN;
        }

        // read the data in
        uint32_t BytesRead = hFile.readsome((char*)ddsData.get(), fileSizeLowPart);
        
        if(hFile.bad())
        {
            return VK_ERROR_UNKNOWN;
        }

        if (BytesRead < fileSizeLowPart)
        {
            return VK_ERROR_UNKNOWN;
        }

        // DDS files always start with the same magic number ("DDS ")
        auto dwMagicNumber = *reinterpret_cast<const uint32_t*>(ddsData.get());
        if (dwMagicNumber != DDS_MAGIC)
        {
            return VK_ERROR_UNKNOWN;
        }

        auto hdr = reinterpret_cast<const DDS_HEADER*>(ddsData.get() + sizeof(uint32_t));

        // Verify header to validate DDS file
        if (hdr->size != sizeof(DDS_HEADER) ||
            hdr->ddspf.size != sizeof(DDS_PIXELFORMAT))
        {
            return VK_ERROR_UNKNOWN;
        }

        // Check for DX10 extension
        bool bDXT10Header = false;
        if ((hdr->ddspf.flags & DDS_FOURCC) &&
            (MAKEFOURCC('D', 'X', '1', '0') == hdr->ddspf.fourCC))
        {
            // Must be long enough for both headers and magic value
            if (fileSizeLowPart < (sizeof(DDS_HEADER) + sizeof(uint32_t) + sizeof(DDS_HEADER_DXT10)))
            {
                return VK_ERROR_UNKNOWN;
            }

            bDXT10Header = true;
        }

        // setup the pointers in the process request
        *header = hdr;
        auto offset = sizeof(uint32_t) + sizeof(DDS_HEADER)
            + (bDXT10Header ? sizeof(DDS_HEADER_DXT10) : 0);
        *bitData = ddsData.get() + offset;
        *bitSize = fileSizeLowPart - offset;

        return VK_SUCCESS;
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
            IssueWarning("D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses 40-bit stride. The current version does not support this format.\n");
            return VK_FORMAT_UNDEFINED;

        case 20: //DXGI_FORMAT_D32_FLOAT_S8X24_UINT
            IssueWarning("D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses 40-bit stride. The current version does not support this format.\n");
            return VK_FORMAT_UNDEFINED;

        case 21: //DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
            //It's possible to return R32G32 here and it will work for the red component, but only with manually not using the green component. It may be not a desired use case.
            IssueWarning("D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses 40-bit stride. The current version does not support this format.\n");
            return VK_FORMAT_UNDEFINED;

        case 22: //DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
            IssueWarning("D32S8 format has different packing rules in Direct3D and Vulkan. Direct3D uses 64-bit stride and Vulkan uses 40-bit stride. The current version does not support this format.\n");
            return VK_FORMAT_UNDEFINED;

        case 23: //DXGI_FORMAT_R10G10B10A2_TYPELESS
            return VK_FORMAT_A2B10G10R10_UINT_PACK32;

        case 24: //DXGI_FORMAT_R10G10B10A2_UNORM
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;

        case 25: //DXGI_FORMAT_R10G10B10A2_UINT
            return VK_FORMAT_A2B10G10R10_UINT_PACK32;

        case 26: //DXGI_FORMAT_R11G11B10_FLOAT
            //DXGI_FORMAT uses different ordering naming convention for some reason, but both APIs store R-component in lower bits and B-component in higher bits.
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
            IssueWarning("R24X8 format: separate depth-stencil attachments are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 47: //DXGI_FORMAT_X24_TYPELESS_G8_UINT
            IssueWarning("X24G8 format: separate depth-stencil attachments are not yet supported in this loader.\n");
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
            //It's possible to return VK_FORMAT_R8_UNORM, but it's probably not what the user wants.
            IssueWarning("A8 format: alpha-only formats are not supported in Vulkan.\n");
            return VK_FORMAT_UNDEFINED;

        case 66: //DXGI_FORMAT_R1_UNORM
            IssueWarning("R1 format: R1 format is not supported by Vulkan.\n");
            return VK_FORMAT_UNDEFINED;

        case 67: //DXGI_FORMAT_R9G9B9E5_SHAREDEXP
            //Again, DXGI_FORMAT uses different ordering naming convention, but both APIs store R-component in lower bits, B-component in higher bits and exponent in the highest bits.
 			return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;

        case 68: //DXGI_FORMAT_R8G8_B8G8_UNORM
            return VK_FORMAT_G8B8G8R8_422_UNORM;

        case 69: //DXGI_FORMAT_G8R8_G8B8_UNORM
            return VK_FORMAT_B8G8R8G8_422_UNORM;

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
            IssueWarning("B8G8R8 format with 32 bit stride is not supported in Vulkan.\n");
            return VK_FORMAT_UNDEFINED;

        case 89: //DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM
            //Vulkan does not support XR biased formats
            IssueWarning("R10F10B10A2 biased format is not supported in Vulkan.\n");
            return VK_FORMAT_UNDEFINED;

        case 90: //DXGI_FORMAT_B8G8R8A8_TYPELESS
            return VK_FORMAT_B8G8R8A8_UNORM;

        case 91: //DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            return VK_FORMAT_B8G8R8A8_SRGB;

        case 92: //DXGI_FORMAT_B8G8R8X8_TYPELESS
            IssueWarning("B8G8R8 format with 32 bit stride is not supported in Vulkan.\n");
            return VK_FORMAT_UNDEFINED;

        case 93: //DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
            IssueWarning("B8G8R8 format with 32 bit stride is not supported in Vulkan.\n");
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
            //DXGI_FORMAT_R8G8B8A8_UNORM is a valid view format for AYUV data, but the user may not want that
            IssueWarning("AYUV format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 101: //DXGI_FORMAT_Y410
            //DXGI_FORMAT_R10G10B10A2_UNORM is a valid view format for Y410 data, but the user may not want that
            IssueWarning("Y410 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 102: //DXGI_FORMAT_Y416
            //DXGI_FORMAT_R16G16B16A16_UNORM is a valid view format for Y416 data, but the user may not want that
            IssueWarning("Y416 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 103: //DXGI_FORMAT_NV12
            //DXGI_FORMAT_R8_UNORM is a valid view format for NV12 data, but the user may not want that
            IssueWarning("NV12 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 104: //DXGI_FORMAT_P010
            //DXGI_FORMAT_R16_UNORM is a valid view format for P010 data, but the user may not want that
            IssueWarning("P010 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 105: //DXGI_FORMAT_P016
            //DXGI_FORMAT_R16_UNORM is a valid view format for P016 data, but the user may not want that
            IssueWarning("P016 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 106: //DXGI_FORMAT_420_OPAQUE
            IssueWarning("420 OPAQUE format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 107: //DXGI_FORMAT_YUY2
            //DXGI_FORMAT_R8G8B8A8_UNORM is a valid view format for YUV2 data, but the user may not want that
            IssueWarning("YUV2 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 108: //DXGI_FORMAT_Y210
            //DXGI_FORMAT_R16G16B16A16_UNORM is a valid view format for Y210 data, but the user may not want that
            IssueWarning("Y210 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 109: //DXGI_FORMAT_Y216
            //DXGI_FORMAT_R16G16B16A16_UNORM is a valid view format for Y216 data, but the user may not want that
            IssueWarning("Y216 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 110: //DXGI_FORMAT_NV11
            //DXGI_FORMAT_R8_UNORM is a valid view format for NV11 data, but the user may not want that
            IssueWarning("NV11 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 111: //DXGI_FORMAT_AI44
            IssueWarning("AI44 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 112: //DXGI_FORMAT_IA44
            IssueWarning("IA44 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 113: //DXGI_FORMAT_P8
            IssueWarning("P8 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 114: //DXGI_FORMAT_A8P8
            IssueWarning("A8P8 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 115: //DXGI_FORMAT_B4G4R4A4_UNORM
            #ifdef VK_EXT_4444_formats
                return VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT;
            #else
                IssueWarning("B4G4R4A4 packed 16-bit format requires VK_EXT_4444_formats extension.\n");
                return VK_FORMAT_UNDEFINED;
            #endif

        case 130: //DXGI_FORMAT_P208
            IssueWarning("P208 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 131: //DXGI_FORMAT_V208
            IssueWarning("V208 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 132: //DXGI_FORMAT_V408
            IssueWarning("V408 format: packed YUV formats are not yet supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 189: //DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
            IssueWarning("DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE: Sampler feedback is not supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 190: //DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE
            IssueWarning("DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE: Sampler feedback is not supported in this loader.\n");
            return VK_FORMAT_UNDEFINED;

        case 0xffffffff: //DXGI_FORMAT_FORCE_UINT
            //This value is never used
            return VK_FORMAT_UNDEFINED;

        default:
            IssueWarning("Unknown format.\n");
            return VK_FORMAT_UNDEFINED;
        }
    }

    //--------------------------------------------------------------------------------------
    // Return the BPP for a particular format
    //--------------------------------------------------------------------------------------
    size_t BitsPerPixel(VkFormat fmt) noexcept
    {
        switch( fmt )
        {
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            return 128;

        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
            return 96;

        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
            return 64;

        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_G8B8G8R8_422_UNORM:
        case VK_FORMAT_B8G8R8G8_422_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return 32;

        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
            return 16;

        #ifdef VK_EXT_4444_formats
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT:
            return 16;
        #endif

        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_SINT:
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
            return 8;

        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return 4;

        default:
            IssueWarning("Unsupported/Not implemented Vulkan format.\n");
            return 0;
        }
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
                    IssueWarning("B8G8R8 format with 32 bit stride is not supported in Vulkan.\n");
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
                // No 24bpp DXGI formats aka D3DFMT_R8G8B8
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

                if(ISBITMASK(0x0f00,0x00f0,0x000f,0xf000))
                {
                    //VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT requires an extension
                }

                // No DXGI format maps to ISBITMASK(0x0f00,0x00f0,0x000f,0) aka D3DFMT_X4R4G4B4

                // No 3:3:2, 3:3:2:8, or paletted DXGI formats aka D3DFMT_A8R3G3B2, D3DFMT_R3G3B2, D3DFMT_P8, D3DFMT_A8P8, etc.
                break;
            }
        }
        else if (ddpf.flags & DDS_LUMINANCE)
        {
            if (8 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0xff,0,0,0))
                {
                    return VK_FORMAT_R8_UNORM; // D3DX10/11 writes this out as DX10 extension
                }

                // No VK format maps to ISBITMASK(0x0f,0,0,0xf0) aka D3DFMT_A4L4

                if (ISBITMASK(0x00ff, 0, 0, 0xff00))
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
        }
        else if (ddpf.flags & DDS_ALPHA)
        {
            if (8 == ddpf.RGBBitCount)
            {
                //No VK format corresponds to DXGI_FORMAT_A8_UNORM
            }
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

                // No VK format maps to ISBITMASK(0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000) aka D3DFMT_A2W10V10U10
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

            if (MAKEFOURCC('Y','U','Y','2') == ddpf.fourCC)
            {
                IssueWarning("YUV2: This loader does not support YUV formats.\n");
            }

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
        case VK_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_SRGB;

        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;

        case VK_FORMAT_BC2_UNORM_BLOCK:
            return VK_FORMAT_BC2_SRGB_BLOCK;

        case VK_FORMAT_BC3_UNORM_BLOCK:
            return VK_FORMAT_BC3_SRGB_BLOCK;

        case VK_FORMAT_B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_SRGB;

        case VK_FORMAT_BC7_UNORM_BLOCK:
            return VK_FORMAT_BC7_SRGB_BLOCK;

        default:
            return format;
        }
    }


    //--------------------------------------------------------------------------------------
    inline bool IsDepthStencil(VkFormat fmt) noexcept
    {
        switch (fmt)
        {
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D16_UNORM:
            return true;

        default:
            return false;
        }
    }

    //--------------------------------------------------------------------------------------
    VkResult FillInitData(size_t width,
        size_t height,
        size_t depth,
        size_t mipCount,
        size_t arraySize,
        size_t numberOfPlanes,
        VkFormat format,
        size_t maxsize,
        size_t bitSize,
        size_t initialOffset,
        const uint8_t* bitData,
        size_t& twidth,
        size_t& theight,
        size_t& tdepth,
        size_t& skipMip,
        std::vector<VkBufferImageCopy>& initData)
    {
        if (!bitData)
        {
            return VK_ERROR_UNKNOWN;
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
            const uint8_t* pSrcBits = bitData;

            for (size_t j = 0; j < arraySize; j++)
            {
                size_t w = width;
                size_t h = height;
                size_t d = depth;
                for (size_t i = 0; i < mipCount; i++)
                {
                    if ((mipCount <= 1) || !maxsize || (w <= maxsize && h <= maxsize && d <= maxsize))
                    {
                        if (!twidth)
                        {
                            twidth = w;
                            theight = h;
                            tdepth = d;
                        }

                        //Vulkan stores row and depth pitch in texels instead of bytes. This can lead to problems with some
                        //format packing rules (i.e. RBG24 with 4 bytes padding). Unfortunately, solving this problem is much
                        //harder than anticipated, because we can't do any modifications to the source data.

                        //For block packed formats: neither Vulkan specification nor VkFormat docs clearly state how the block compressed
                        //data gets treated for the purpose of VkBufferImageCopy::bufferRowLength and VkBufferImageCopy::bufferImageHeight.
                        //This loader assumes that block compression does not affect texel row length at all

                        VkBufferImageCopy res;
                        res.bufferOffset                    = initialOffset + (pSrcBits - bitData);
                        res.bufferRowLength                 = w;
                        res.bufferImageHeight               = h;
                        res.imageOffset.x                   = 0;
                        res.imageOffset.y                   = 0;
                        res.imageOffset.z                   = 0;
                        res.imageExtent.width               = w;
                        res.imageExtent.height              = h;
                        res.imageExtent.depth               = d;
                        res.imageSubresource.baseArrayLayer = j;
                        res.imageSubresource.layerCount     = 1;
                        res.imageSubresource.aspectMask     = IsDepthStencil(format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
                        res.imageSubresource.mipLevel       = i;

                        //We don't support plane resources yet. If we did, we'd have to call AdjustPlaneResource() right there

                        initData.emplace_back(res);
                    }
                    else if (!j)
                    {
                        // Count number of skipped mipmaps (first item only)
                        ++skipMip;
                    }

                    if (pSrcBits + (NumBytes*d) > pEndBits)
                    {
                        IssueWarning("DDS reading error: unexpected EOF!\n");
                        return VK_ERROR_UNKNOWN;
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

        return initData.empty() ? VK_ERROR_UNKNOWN : VK_SUCCESS;
    }


    //--------------------------------------------------------------------------------------
    VkResult CreateTextureResource(
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
        VkImage* texture) noexcept
    {
        if (!vkDevice)
            return VK_ERROR_UNKNOWN;

        VkResult err = VK_ERROR_UNKNOWN;

        if (loadFlags & DDS_LOADER_FORCE_SRGB)
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

        err = vkCreateImage(vkDevice, &imageCreateInfo, allocator, texture);
        if(err == VK_SUCCESS)
        {
            assert(texture != nullptr && *texture != nullptr);

            SetDebugObjectName(vkDevice, *texture, "DDSTextureLoader");
        }

        return err;
    }

    //--------------------------------------------------------------------------------------
    VkResult CreateTextureFromDDS(VkDevice vkDevice,
        const DDS_HEADER* header,
        const uint8_t* bitData,
        size_t bitSize,
        size_t maxsize,
        size_t initialDataOffset,
        const VkPhysicalDeviceLimits* deviceLimits,
        VkImageUsageFlags usageFlags,
        VkImageCreateFlags createFlags,
        unsigned int loadFlags,
        VkAllocationCallbacks* allocator,
        VkImage* texture,
        std::vector<VkBufferImageCopy>& subresources,
        bool* outIsCubeMap) noexcept(false)
    {
        VkResult errCode = VK_SUCCESS;

        uint32_t width  = header->width;
        uint32_t height = header->height;
        uint32_t depth  = header->depth;

        VkImageType imgType = VK_IMAGE_TYPE_2D;
        uint32_t arraySize = 1;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool isCubeMap = false;

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
                IssueWarning("DDS reading error: array size cannot be null\n");
                return VK_ERROR_UNKNOWN;
            }

            if(IsTypelessFormat(d3d10ext->dxgiFormat))
            {
                imageCreateFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            }

            //Vulkan also has a special flag for sub-sampled images (VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT). BC and YUV block-compressed formatted images may or may not have to be 
            //created with this flag (the person who wrote this line doesn't know if it's true)

            VkFormat vkFormatFromDxgi = DXGIToVkFormat(d3d10ext->dxgiFormat);
            if(BitsPerPixel(vkFormatFromDxgi) == 0)
            {
                IssueWarning("DDS reading error: unsupported format!\n");
                return VK_ERROR_UNKNOWN;
            }

            format = vkFormatFromDxgi;

            VkImageType imgTypeFromResDim = D3DResourceDimensionToImageType(d3d10ext->resourceDimension);
            switch(imgTypeFromResDim)
            {
            case VK_IMAGE_TYPE_1D:
                // D3DX writes 1D textures with a fixed Height of 1
                if ((header->flags & DDS_HEIGHT) && height != 1)
                {
                    IssueWarning("DDS reading error: invalid data. 1D textures should have the height of 1.\n");
                    return VK_ERROR_UNKNOWN;
                }
                height = depth = 1;
                break;

            case VK_IMAGE_TYPE_2D:
                if (d3d10ext->miscFlag & 0x4 /* RESOURCE_MISC_TEXTURECUBE */)
                {
                    imageCreateFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                    arraySize *= 6;
                    isCubeMap = true;
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
                    IssueWarning("DDS reading error: DDS files with TEXTURE3D resource dimension should have DDS_HEADER_FLAGS_VOLUME flag set!\n");
                    return VK_ERROR_UNKNOWN;
                }

                if (arraySize > 1)
                {
                    IssueWarning("DDS reading error: This loader doesn't support 3D texture arrays!\n");
                    return VK_ERROR_UNKNOWN;
                }
                break;

            default:
                IssueWarning("DDS reading error: unsupported resource dimension!\n");
                return VK_ERROR_UNKNOWN;
            }

            imgType = imgTypeFromResDim;
        }
        else
        {
            format = GetVkFormat(header->ddspf);

            if(format == VK_FORMAT_UNDEFINED)
            {
                IssueWarning("DDS reading error: unsupported format!\n");
                return VK_ERROR_UNKNOWN;
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
                        IssueWarning("DDS reading error: invalid data. All six faces of a cube map have to be defined.\n");
                        return VK_ERROR_UNKNOWN;
                    }

                    arraySize = 6;
                    isCubeMap = true;
                    imageCreateFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                }

                depth = 1;
                imgType = VK_IMAGE_TYPE_2D;

                // Note there's no way for a legacy Direct3D 9 DDS to express a '1D' texture
            }

            assert(BitsPerPixel(format) != 0);
        }

        // Bound sizes (for security purposes we don't trust DDS file metadata larger than the Direct3D hardware requirements. Vulkan does not have an easy way to obtain this informtation)
        uint32_t maxDirect3DMips = 15;
        if (mipCount > maxDirect3DMips /*D3D12_REQ_MIP_LEVELS*/ )
        {
            IssueWarning("DDS reading error: images with >15 MIPs aren't supported for security reason.\n");
            return VK_ERROR_UNKNOWN;
        }

        switch (imgType)
        {
        case VK_IMAGE_TYPE_1D:
            if ((arraySize > maxImageArrayLayers) ||
                (width > maxImageDimension1D))
            {
                IssueWarning("DDS reading error: 1D image is too big.\n");
                return VK_ERROR_UNKNOWN;
            }
            break;

        case VK_IMAGE_TYPE_2D:
            if (isCubeMap)
            {
                // This is the right bound because we set arraySize to (NumCubes*6) above
                if ((arraySize > maxImageArrayLayers) ||
                    (width > maxImageDimensionCube) ||
                    (height > maxImageDimensionCube))
                {
                    IssueWarning("DDS reading error: cube image is too big.\n");
                    return VK_ERROR_UNKNOWN;
                }
            }
            else if ((arraySize > maxImageArrayLayers) ||
                     (width > maxImageDimension2D) ||
                     (height > maxImageDimension2D))
            {
                IssueWarning("DDS reading error: 2D image is too big.\n");
                return VK_ERROR_UNKNOWN;
            }
            break;

        case VK_IMAGE_TYPE_3D:
            if ((arraySize > 1) ||
                (width > maxImageDimension3D) ||
                (height > maxImageDimension3D) ||
                (depth > maxImageDimension3D))
            {
                IssueWarning("DDS reading error: 3D image is too big.\n");
                return VK_ERROR_UNKNOWN;
            }
            break;

        default:
            IssueWarning("DDS reading error: unknown image type.\n");
            return VK_ERROR_UNKNOWN;
        }

        //It doesn't seem like there's a Vulkan alternative to D3D12GetFormatPlaneCount.
        //So we can't support planes.
        uint32_t numberOfPlanes = 1;

        if (outIsCubeMap != nullptr)
        {
            *outIsCubeMap = isCubeMap;
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
            maxsize, bitSize, initialDataOffset, bitData,
            twidth, theight, tdepth, skipMip, subresources);

        if (errCode == VK_SUCCESS)
        {
            size_t reservedMips = mipCount;
            if (loadFlags & DDS_LOADER_MIP_RESERVE)
            {
                reservedMips = std::min<size_t>(maxDirect3DMips,
                    CountMips(width, height));
            }

            errCode = CreateTextureResource(vkDevice, imgType, twidth, theight, tdepth, reservedMips - skipMip, arraySize,
                format, usageFlags, imageCreateFlags, loadFlags, allocator, texture);

            if (errCode != VK_SUCCESS && !maxsize && (mipCount > 1))
            {
                subresources.clear();

                maxsize = static_cast<size_t>(
                    (imgType == VK_IMAGE_TYPE_3D)
                    ? maxImageDimension3D
                    : maxImageDimension2D);

                errCode = FillInitData(width, height, depth, mipCount, arraySize,
                    numberOfPlanes, format,
                    maxsize, bitSize, initialDataOffset, bitData,
                    twidth, theight, tdepth, skipMip, subresources);
                if (errCode == VK_SUCCESS)
                {
                    errCode = CreateTextureResource(vkDevice, imgType, twidth, theight, tdepth, mipCount - skipMip, arraySize,
                        format, usageFlags, imageCreateFlags, loadFlags, allocator, texture);
                }
            }
        }

        if (errCode != VK_SUCCESS)
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
#if !defined(NO_D3D12_DEBUG_NAME) && defined(VK_EXT_debug_utils) && ( defined(_DEBUG) || defined(PROFILE) )
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

            vkSetDebugUtilsObjectNameEXT(device, &debugObjectNameInfo);
        }
#endif
    }
} // anonymous namespace


//--------------------------------------------------------------------------------------
VkResult Vulkan::LoadDDSTextureFromMemory(
    VkDevice vkDevice,
    const uint8_t* ddsData,
    size_t ddsDataSize,
    VkImage* texture,
    std::vector<VkBufferImageCopy>& subresources,
    size_t maxsize,
    size_t initialOffset,
    DDS_ALPHA_MODE* alphaMode,
    bool* isCubeMap)
{
    return LoadDDSTextureFromMemoryEx(
        vkDevice,
        ddsData,
        ddsDataSize,
        maxsize,
        initialOffset,
        nullptr,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        0,
        DDS_LOADER_DEFAULT,
        nullptr,
        texture,
        subresources,
        alphaMode,
        isCubeMap);
}


VkResult Vulkan::LoadDDSTextureFromMemoryEx(
    VkDevice vkDevice,
    const uint8_t* ddsData,
    size_t ddsDataSize,
    size_t maxsize,
    size_t initialOffset,
    const VkPhysicalDeviceLimits* deviceLimits,
    VkImageUsageFlags usageFlags,
    VkImageCreateFlags createFlags,
    unsigned int loadFlags,
    VkAllocationCallbacks* allocator,
    VkImage* texture,
    std::vector<VkBufferImageCopy>& subresources,
    DDS_ALPHA_MODE* alphaMode,
    bool* isCubeMap)
{
    if (texture)
    {
        *texture = nullptr;
    }
    if (alphaMode)
    {
        *alphaMode = DDS_ALPHA_MODE_UNKNOWN;
    }
    if (isCubeMap)
    {
        *isCubeMap = false;
    }

    if(!vkDevice || !ddsData || !texture)
    {
        return VK_ERROR_UNKNOWN;
    }

    // Validate DDS file in memory
    const DDS_HEADER* header = nullptr;
    const uint8_t* bitData = nullptr;
    size_t bitSize = 0;

    VkResult errCode = LoadTextureDataFromMemory(ddsData,
        ddsDataSize,
        &header,
        &bitData,
        &bitSize
    );
    if (errCode != VK_SUCCESS)
    {
        return errCode;
    }

    errCode = CreateTextureFromDDS(vkDevice,
        header, bitData, bitSize, maxsize,
        initialOffset, deviceLimits, usageFlags, createFlags, loadFlags,
        allocator, texture, subresources, isCubeMap);
    if (errCode == VK_SUCCESS)
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
VkResult Vulkan::LoadDDSTextureFromFile(
    VkDevice vkDevice,
    const char_type* fileName,
    VkImage* texture,
    std::unique_ptr<uint8_t[]>& ddsData,
    std::vector<VkBufferImageCopy>& subresources,
    size_t maxsize,
    size_t initialOffset,
    DDS_ALPHA_MODE* alphaMode,
    bool* isCubeMap)
{
    return LoadDDSTextureFromFileEx(
        vkDevice,
        fileName,
        maxsize,
        initialOffset,
        nullptr,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        0,
        DDS_LOADER_DEFAULT,
        nullptr,
        texture,
        ddsData,
        subresources,
        alphaMode,
        isCubeMap);
}

VkResult Vulkan::LoadDDSTextureFromFileEx(
    VkDevice vkDevice,
    const char_type* fileName,
    size_t maxsize,
    size_t initialOffset,
    const VkPhysicalDeviceLimits* deviceLimits,
    VkImageUsageFlags usageFlags,
    VkImageCreateFlags createFlags,
    unsigned int loadFlags,
    VkAllocationCallbacks* allocator,
    VkImage* texture,
    std::unique_ptr<uint8_t[]>& ddsData,
    std::vector<VkBufferImageCopy>& subresources,
    DDS_ALPHA_MODE* alphaMode,
    bool* isCubeMap)
{
    if (texture)
    {
        *texture = nullptr;
    }
    if (alphaMode)
    {
        *alphaMode = DDS_ALPHA_MODE_UNKNOWN;
    }
    if (isCubeMap)
    {
        *isCubeMap = false;
    }

    if (!vkDevice || !fileName || !texture)
    {
        return VK_ERROR_UNKNOWN;
    }

    const DDS_HEADER* header = nullptr;
    const uint8_t* bitData = nullptr;
    size_t bitSize = 0;

    VkResult errCode = LoadTextureDataFromFile(fileName,
        ddsData,
        &header,
        &bitData,
        &bitSize
    );
    if (errCode != VK_SUCCESS)
    {
        return errCode;
    }

    errCode = CreateTextureFromDDS(vkDevice,
        header, bitData, bitSize, maxsize,
        initialOffset, deviceLimits,
        usageFlags, createFlags, loadFlags,
        allocator, texture, subresources, isCubeMap);

    if (errCode == VK_SUCCESS)
    {
        #ifdef DDS_PATH_WIDE_CHAR
            int filenameSize = WideCharToMultiByte(CP_UTF8, 0, fileName, -1, nullptr, 0, nullptr, nullptr);

            char* filenameU8 = (char*)alloca(filenameSize + 1);
            WideCharToMultiByte(CP_UTF8, 0, fileName, -1, filenameU8, filenameSize + 1, nullptr, nullptr);

            SetDebugTextureInfo(vkDevice, filenameU8, *texture);
        #else
            SetDebugTextureInfo(vkDevice, fileName, *texture);
        #endif // _WIN32

    
        if (alphaMode)
            *alphaMode = GetAlphaMode(header);
    }

    return errCode;
}
