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

#error "If VK_NO_PROTOTYPES is defined, please declare the function includes in DDSVulkanFunctionsInclude.h."

#endif // VK_NO_PROTOTYPES