# DDSTextureLoaderVk
A cross-platform lightweight DDS texture loader for Vulkan. Based on Microsoft's DirectXTex DDSTextureLoader12: https://github.com/microsoft/DirectXTex/tree/master/DDSTextureLoader  

No function allocates the memory required for images, since different developers may organize their image memory layout differently. The developer is expected to call `vkBindImageMemory` to allocate the memory for the image.  

The function only creates a `VkImage` object and loads the data from the disk. To use the loaded data in the application, the developer is expected to manually upload the data into intermediate buffer and then issue a `vkCmdCopyBufferToImage`-like command.  

## Functions
### LoadDDSTextureFromMemory
Creates a `VkImage` from a data buffer. Assumes the device supports only minimal required image limits.

Parameters:
* `vkDevice`:      Vulkan logical device used to create the image.
* `ddsData`:       DDS data buffer.
* `ddsDataSize`:   The size of the buffer provided in `ddsData`.
* `texture`:       A `VkImage` instance that gets created based on DDS data.
* `subresources`:  The list of image subresources that get passed as the last parameter of `vkCmdCopyBufferToImage`.
* `maxsize`:       The maximum size of the image in a single dimension, in texels.
* `initialOffset`: Initial offset in the intermediate buffer to which the developer will pass the image data.
* `alphaMode`:     The address by which the image alpha mode gets written. May be `NULL`.
* `isCubeMap`:     The address by which, if the image is cubemap, `true` will be written. Otherwise, `false` will be written. May be `NULL`.

### LoadDDSTextureFromFile
Creates a `VkImage` from a file. Assumes the device supports only minimal required image limits.

Parameters:
* `vkDevice`:      Vulkan logical device used to create the image.
* `fileName`:      The file path of the image.
* `texture`:       A `VkImage` instance that gets created based on DDS data.
* `ddsData`:       The image file contents. The developer is expected to copy *this* data into the intermediate buffer.
* `subresources`:  The list of image subresources that get passed as the last parameter of `vkCmdCopyBufferToImage`.
* `maxsize`:       The maximum size of the image in a single dimension, in texels.
* `initialOffset`: Initial offset in the intermediate buffer to which the developer will pass the image data.
* `alphaMode`:     The address by which the image alpha mode gets written. May be `NULL`.
* `isCubeMap`:     The address by which, if the image is cubemap, `true` will be written. Otherwise, `false` will be written. May be `NULL`.

### LoadDDSTextureFromMemoryEx
Creates a `VkImage` from a data buffer. An extended version of `LoadDDSTextureFromMemory`.

Parameters:
* `vkDevice`:      Vulkan logical device used to create the image.
* `ddsData`:       DDS data buffer.
* `ddsDataSize`:   The size of the buffer provided in `ddsData`.
* `maxsize`:       The maximum size of the image in a single dimension, in texels.
* `initialOffset`: Initial offset in the intermediate buffer to which the developer will pass the image data.
* `deviceLimits`:  A pointer to a valid `VkPhysicalDeviceLimits` structure defining image limits. If `NULL`, the loader assumes the device supports only minimal required image limits.
* `usageFlags`:    Usage flags the image will be created with.
* `createFlags`:   Creation flags the image will be created with. The loader automatically handles `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` and `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT`, meaning the developer does not need to set these flags.
* `loadFlags`:     A member of `DDS_LOADER_FLAGS` describing image loading flags.
* `allocator`:     An instance of `VkAllocationCallbacks` that will be used for image creation. May be `NULL`.
* `texture`:       A `VkImage` instance that gets created based on DDS data.
* `subresources`:  The list of image subresources that get passed as the last parameter of `vkCmdCopyBufferToImage`.
* `alphaMode`:     The address by which the image alpha mode gets written. May be `NULL`.
* `isCubeMap`:     The address by which, if the image is cubemap, `true` will be written. Otherwise, `false` will be written. May be `NULL`.

### LoadDDSTextureFromFileEx
Creates a `VkImage` from a file. An extended version of `LoadDDSTextureFromFile`.

Parameters:
* `vkDevice`:      Vulkan logical device used to create the image.
* `fileName`:      The file path of the image.
* `maxsize`:       The maximum size of the image in a single dimension, in texels.
* `initialOffset`: Initial offset in the intermediate buffer to which the developer will pass the image data.
* `deviceLimits`:  A pointer to a valid `VkPhysicalDeviceLimits` structure defining image limits. If `NULL`, the loader assumes the device supports only minimal required image limits.
* `usageFlags`:    Usage flags the image will be created with.
* `createFlags`:   Creation flags the image will be created with. The loader automatically handles `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` and `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT`, meaning the developer does not need to set these flags.
* `loadFlags`:     A member of `DDS_LOADER_FLAGS` describing image loading flags.
* `allocator`:     An instance of `VkAllocationCallbacks` that will be used for image creation. May be `NULL`.
* `texture`:       A `VkImage` instance that gets created based on DDS data.
* `ddsData`:       The image file contents. The developer is expected to copy *this* data into the intermediate buffer.
* `subresources`:  The list of image subresources that get passed as the last parameter of `vkCmdCopyBufferToImage`.
* `alphaMode`:     The address by which the image alpha mode gets written. May be `NULL`.
* `isCubeMap`:     The address by which, if the image is cubemap, `true` will be written. Otherwise, `false` will be written. May be `NULL`.


## Debug object names
Similar to DDSTextureLoader, this loader may assign debug object names in the debug mode. It's only enabled if `VK_EXT_debug_utils` extension is defined and `NO_VK_DEBUG_NAME` is not defined.


## SAL
Unlike DDSTextureLoader, this loader does not use SAL. This is to make the loader cross-platform.


## Queue family ownership
This loader creates the images on the default queue family (`queueFamilyIndexCount = 0`, `pQueueFamilyIndices = nullptr`). The developer is expected to issue `vkCmdPipelineBarrier` command to transfer the queue family ownership if needed.

`VK_SHARING_MODE_CONCURRENT` is currently not supported.


## Using VK_NO_PROTOTYPES
If the developer uses `VK_NO_PROTOTYPES`, they are expected to manually set the necessary dynamically loaded functions. These functions must be called before any other function from the library. 

### SetVkCreateImageFuncPtr
Use this function to set the pointer for dynamically loaded `vkCreateImage()` function. Example:
```
DDSTextureLoaderVk::SetVkCreateImageFuncPtr(vkCreateImage); //vkCreateImage was loaded dynamically
DDSTextureLoaderVk::LoadDDSTextureFromFile(...);
```

### SetVkCreateImageFuncPtrWithUserPtr and SetVkCreateImageUserPtr
Use these functions if `vkCreateImage()` is defined as a class member (i.e. `QVulkanFunctions`, `vk::DispatchLoaderDynamic`) or requires additional data to call.  

Example:
```
DDSTextureLoaderVk::SetVkCreateImageFuncPtrWithUserPtr([](void* userPtr, VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage)
{
	return reinterpret_cast<QVulkanFunctions*>(userPtr)->vkCreateImage(device, pCreateInfo, pAllocator, pImage);
});
DDSTextureLoaderVk::SetVkCreateImageUserPtr(m_pVulkanFunctions);

DDSTextureLoaderVk::LoadDDSTextureFromFile(...);
```

### SetVkSetDebugUtilsObjectNameFuncPtr
Use this function to set the pointer for dynamically loaded `vkSetDebugUtilsObjectNameEXT()` function. Example:
```
DDSTextureLoaderVk::SetVkSetDebugUtilsObjectNameFuncPtr(vkSetDebugUtilsObjectNameEXT); //vkSetDebugUtilsObjectNameEXT was loaded dynamically
DDSTextureLoaderVk::LoadDDSTextureFromFile(...);
```

### SetVkSetDebugUtilsObjectNameFuncPtrWithUserPtr and SetVkSetDebugUtilsObjectNameUserPtr
Use these functions if `vkSetDebugUtilsObjectNameEXT()` is defined as a class member (i.e. `QVulkanFunctions`, `vk::DispatchLoaderDynamic`) or requires additional data to call.  

Example:
```
DDSTextureLoaderVk::SetVkSetDebugUtilsObjectNameFuncPtrWithUserPtr([](void* userPtr, VkDevice device, const VkDebugUtilsObjectNameInfoEXT* pNameInfo)
{
	return reinterpret_cast<QVulkanFunctions*>(userPtr)->vkSetDebugUtilsObjectNameEXT(device, pNameInfo);
});
DDSTextureLoaderVk::SetVkSetDebugUtilsObjectNameUserPtr(m_pVulkanFunctions);

DDSTextureLoaderVk::LoadDDSTextureFromFile(...);
```

If `VK_NO_PROTOTYPES` is defined, it's mandatory to call either `SetVkCreateImageFuncPtr()` or `SetVkCreateImageFuncPtrWithUserPtr()`+`SetVkCreateImageUserPtr()`. Calling debug object name function setters is only needed if the developer wants to set debug object names. If these functions are not defined, the library omits setting debug object names.


## VkResult error codes
Since VkResult lacks the diversity of `HRESULT` error codes, this loader prints warning messages to the Visual Studio console (under Windows) or to stdio console (under Linux) in debug builds, to make the developer see what's gone wrong. To disable thos behavior, define `DDS_NO_ISSUE_LOGGING`.


## Format support
* This loader does not support all packed D32S8X24 formats. The reason for this is that Direct3D uses 64-bit stride and Vulkan uses 40-bit stride for these formats.
* This loader does not support `DXGI_FORMAT_R24_UNORM_X8_TYPELESS` and `DXGI_FORMAT_X24_TYPELESS_G8_UINT` formats. Separate depth-stencil attachments are not handled in this loader.
* This loader does not support `DXGI_FORMAT_A8_UNORM` and `DXGI_FORMAT_R1_UNORM` formats. There are no equivalents to these formats in Vulkan.
* This loader does not support `DXGI_FORMAT_B8G8R8X8_UNORM`, `DXGI_FORMAT_B8G8R8X8_TYPELESS`, `DXGI_FORMAT_B8G8R8X8_UNORM_SRGB` formats. Vulkan does not support RGB formats with 32-bit stride.
* This loader does not support `DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM`. Vulkan does not provide support for XR biased images.
* This loader does not support almost all of YUV formats. This feature is in the backlog. The full list of YUV formats that are not supported:
  * `DXGI_FORMAT_AYUV`
  * `DXGI_FORMAT_Y410`
  * `DXGI_FORMAT_Y416`
  * `DXGI_FORMAT_NV12`
  * `DXGI_FORMAT_P010`
  * `DXGI_FORMAT_P016`
  * `DXGI_FORMAT_420_OPAQUE`
  * `DXGI_FORMAT_YUY2`
  * `DXGI_FORMAT_Y210`
  * `DXGI_FORMAT_Y216`
  * `DXGI_FORMAT_NV11`
  * `DXGI_FORMAT_AI44`
  * `DXGI_FORMAT_IA44`
  * `DXGI_FORMAT_P8`
  * `DXGI_FORMAT_A8P8`
  * `DXGI_FORMAT_P208`
  * `DXGI_FORMAT_V208`
  * `DXGI_FORMAT_V408`
* This loader DOES support compressed BC formats, `DXGI_FORMAT_R8G8_B8G8_UNORM` and `DXGI_FORMAT_G8R8_G8B8_UNORM`. See **padding** section for more details.
* This loader DOES support `DXGI_FORMAT_B4G4R4A4_UNORM` format, but only if `VK_EXT_4444_formats` extension is provided.


## Padding
Since Vulkan expects `bufferRowLength` and `bufferImageHeight` to be in texels instead of bytes, it's currently impossible in this loader to load 3-byte RGB images with 4-byte padding.

Also, due to ambiguity in Vulkan documentation, it's not fully clear if Vulkan considers 2x1 blocks in compressed images as 1 or 2 texels. The current implementation makes the assumption that it considers these blocks as 2 texels.
