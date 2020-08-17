//--------------------------------------------------------------------------------------
// File: DDSTextureLoader12.h
//
// Functions for loading a DDS texture and creating a Vulkan runtime resource for it
//
// Heavily based on Microsoft's DDSTextureLoader12: https://github.com/microsoft/DirectXTex/tree/master/DDSTextureLoader
// Licensed under the MIT License.
//
//--------------------------------------------------------------------------------------

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

#if defined(_WIN32) && defined(UNICODE)

#ifndef DDS_PATH_WIDE_CHAR
#define DDS_PATH_WIDE_CHAR
#endif

using char_type = wchar_t;

#else

#ifdef DDS_PATH_WIDE_CHAR
#undef DDS_PATH_WIDE_CHAR
#endif

using char_type = char;

#endif // _WIN32


namespace Vulkan
{
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

    // Standard version
    VkResult __cdecl LoadDDSTextureFromMemory(
        VkDevice vkDevice,
        const uint8_t* ddsData,
        size_t ddsDataSize,
        VkImage* texture,
        std::vector<VkBufferImageCopy>& subresources,
        size_t maxsize,
        size_t initialOffset,
        DDS_ALPHA_MODE* alphaMode = nullptr,
        bool* isCubeMap = nullptr);

    VkResult __cdecl LoadDDSTextureFromFile(
        VkDevice vkDevice,
        const char_type* fileName,
        VkImage* texture,
        std::unique_ptr<uint8_t[]>& ddsData,
        std::vector<VkBufferImageCopy>& subresources,
        size_t maxsize,
        size_t initialOffset,
        DDS_ALPHA_MODE* alphaMode = nullptr,
        bool* isCubeMap = nullptr);

    // Extended version
    VkResult __cdecl LoadDDSTextureFromMemoryEx(
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
        DDS_ALPHA_MODE* alphaMode = nullptr,
        bool* isCubeMap = nullptr);

    VkResult __cdecl LoadDDSTextureFromFileEx(
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
        DDS_ALPHA_MODE* alphaMode = nullptr,
        bool* isCubeMap = nullptr);
}