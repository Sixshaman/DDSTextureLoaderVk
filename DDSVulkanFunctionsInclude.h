#pragma once

#ifdef VK_NO_PROTOTYPES

#include <vulkan/vulkan_core.h>

extern PFN_vkCreateImage vkCreateImage;

#ifdef VK_EXT_debug_utils
	extern PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT;
#endif

#endif // VK_NO_PROTOTYPES