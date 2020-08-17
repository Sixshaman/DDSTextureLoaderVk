#pragma once

#ifdef VK_NO_PROTOTYPES

//-----------------------------------------------------------------------------------------------------------------------------
// If VK_NO_PROTOTYPES is defined, DDSTextureLoaderVk.cpp does not know about any functions used to create images.
// If that's the case, include your files that contain Vulkan function prototypes in this file.
// ----------------------------------------------------------------------------------------------------------------------------

////////////////////////////////////////////////
// 
// EXAMPLE:
// #include "MyVulkanFunctionPrototypes.h"
// using namespace MyVulkanFunctionPrototypes;
//
///////////////////////////////////////////////

#endif // VK_NO_PROTOTYPES