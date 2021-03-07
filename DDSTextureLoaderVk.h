//--------------------------------------------------------------------------------------
// File: DDSTextureLoader12.h
//
// Functions for loading a DDS texture and creating a Vulkan runtime resource for it
//
// Basically a ripoff of Microsoft's DDSTextureLoader12: https://github.com/microsoft/DirectXTex/tree/master/DDSTextureLoader
// Licensed under the MIT License.
//
//--------------------------------------------------------------------------------------

#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>

#if defined(_WIN32) && defined(UNICODE)

#ifndef DDS_LOADER_PATH_WIDE_CHAR
#define DDS_LOADER_PATH_WIDE_CHAR
#endif

    using char_type = wchar_t;
#else
    using char_type = char;
#endif // _WIN32

namespace DDSTextureLoaderVk
{

#ifdef VK_NO_PROTOTYPES

//Normal version (for when vkCreateImage() is defined as-is)
void SetVkCreateImageFuncPtr(PFN_vkCreateImage funcPtr);

//User-ptr version (for when vkCreateImage() is defined as a class member function)
typedef VkResult (*PFN_DdsLoader_vkCreateImageUserPtr)(void* userPtr, VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage);

void SetVkCreateImageFuncPtrWithUserPtr(PFN_DdsLoader_vkCreateImageUserPtr funcPtr);
void SetVkCreateImageUserPtr(void* userPtr);

#ifdef VK_EXT_debug_utils

//Normal version (for when vkSetDebugUtilsObjectNameEXT() is defined as-is)
void SetVkSetDebugUtilsObjectNameFuncPtr(PFN_vkSetDebugUtilsObjectNameEXT funcPtr);

//User-ptr version (for when vkSetDebugUtilsObjectNameEXT() is defined as a class member function)
typedef VkResult (*PFN_DdsLoader_vkSetDebugUtilsObjectNameUserPtr)(void* userPtr, VkDevice device, const VkDebugUtilsObjectNameInfoEXT* pNameInfo);

void SetVkSetDebugUtilsObjectNameFuncPtrWithUserPtr(PFN_DdsLoader_vkSetDebugUtilsObjectNameUserPtr funcPtr);
void SetVkSetDebugUtilsObjectNameUserPtr(void* userPtr);

#endif

#endif

#ifndef DDS_ALPHA_MODE_DEFINED
#define DDS_ALPHA_MODE_DEFINED
    enum DDS_ALPHA_MODE : uint32_t
    {
        DDS_ALPHA_MODE_UNKNOWN = 0,
        DDS_ALPHA_MODE_STRAIGHT = 1,
        DDS_ALPHA_MODE_PREMULTIPLIED = 2,
        DDS_ALPHA_MODE_OPAQUE = 3,
        DDS_ALPHA_MODE_CUSTOM = 4,
    };
#endif

    enum DDS_LOADER_FLAGS
    {
        DDS_LOADER_DEFAULT = 0,
        DDS_LOADER_FORCE_SRGB = 0x1,
        DDS_LOADER_MIP_RESERVE = 0x8,
    };

    enum DDS_LOADER_RESULT: uint32_t
    {
        DDS_LOADER_SUCCESS             = 0,  //Everything is fine. Corresponds to S_OK hresult value
        DDS_LOADER_FAIL                = 1,  //General error. Corresponds to E_FAIL hresult value
        DDS_LOADER_BAD_POINTER         = 2,  //Invalid pointer has been passed. Corresponds to E_POINTER hresult value
        DDS_LOADER_INVALID_ARG         = 3,  //Invalid argument has been passed. Corresponds to E_INVALIDARG hresult value
        DDS_LOADER_INVALID_DATA        = 4,  //Invalid data in the file
        DDS_LOADER_UNEXPECTED_EOF      = 5,  //Unexpected end of file
        DDS_LOADER_UNSUPPORTED_FORMAT  = 6,  //DDS file's format is unsupported by the loader
        DDS_LOADER_UNSUPPORTED_LAYOUT  = 7,  //DDS file's layout is invalid/unsupported by the loader (unknown image type, it's a 3D texture array, etc.)
        DDS_LOADER_BELOW_LIMITS        = 8,  //Image size is bigger than device limits
        DDS_LOADER_NO_HOST_MEMORY      = 9,  //Not enough system memory to create image handle
        DDS_LOADER_NO_DEVICE_MEMORY    = 10,  //Not enough video memory to create image handle
        DDS_LOADER_NO_FUNCTION         = 11, //The vkCreateImage() function was not loaded
        DDS_LOADER_ARITHMETIC_OVERFLOW = 12, //Arithmetic overflow over uint32_t capacity
    };

    std::string DDSLoaderResultToString(DDS_LOADER_RESULT errorCode);

    //Helper struct to describe an image subresource loaded by the loader
    struct LoadedSubresourceData
    {
        const uint8_t*     PData;            //Pointer to the subresource data
        size_t             DataByteSize;     //The size of the subresource data, in bytes
        VkImageSubresource SubresourceSlice; //The slice (mip level, array layer, and possibly plane) of the subresource
        VkExtent3D         Extent;           //The extent (width-height-depth) of the subresource
    };

    // Standard version
    DDS_LOADER_RESULT __cdecl LoadDDSTextureFromMemory(
        VkDevice vkDevice,
        const uint8_t* ddsData,
        size_t ddsDataSize,
        VkImage* texture,
        std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
        size_t maxsize = 0,
        VkImageCreateInfo* outImageCreateInfo = nullptr,
        DDS_ALPHA_MODE* outAlphaMode = nullptr);

    DDS_LOADER_RESULT __cdecl LoadDDSTextureFromFile(
        VkDevice vkDevice,
        const char_type* fileName,
        VkImage* texture,
        std::unique_ptr<uint8_t[]>& ddsData,
        std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
        size_t maxsize = 0,
        VkImageCreateInfo* outImageCreateInfo = nullptr,
        DDS_ALPHA_MODE* alphaMode = nullptr);

    // Extended version
    DDS_LOADER_RESULT __cdecl LoadDDSTextureFromMemoryEx(
        VkDevice vkDevice,
        const uint8_t* ddsData,
        size_t ddsDataSize,
        size_t maxsize,
        const VkPhysicalDeviceLimits* deviceLimits,
        VkImageUsageFlags usageFlags,
        VkImageCreateFlags createFlags,
        unsigned int loadFlags,
        VkAllocationCallbacks* allocationCallbacks,
        VkImage* texture,
        std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
        VkImageCreateInfo* outImageCreateInfo = nullptr,
        DDS_ALPHA_MODE* alphaMode = nullptr);

    DDS_LOADER_RESULT __cdecl LoadDDSTextureFromFileEx(
        VkDevice vkDevice,
        const char_type* fileName,
        size_t maxsize,
        const VkPhysicalDeviceLimits* deviceLimits,
        VkImageUsageFlags usageFlags,
        VkImageCreateFlags createFlags,
        unsigned int loadFlags,
        VkAllocationCallbacks* allocationCallbacks,
        VkImage* texture,
        std::unique_ptr<uint8_t[]>& ddsData,
        std::vector<DDSTextureLoaderVk::LoadedSubresourceData>& subresources,
        VkImageCreateInfo* outImageCreateInfo = nullptr,
        DDS_ALPHA_MODE* alphaMode = nullptr);
}