// [AllGenU-Android] Stubs de Vulkan 1.3 para VulkanMemoryAllocator.
//
// El header de Vulkan del NDK (API 30, version 1.2) no declara estas funciones,
// pero VMA las referencia al compilar su implementacion. En un telefono sin
// Vulkan 1.3 nunca se llaman (VMA las usa solo en sus rutas de
// VK_KHR_maintenance5). Aqui se dan definiciones que devuelven error.
#include <vulkan/vulkan.h>
#include <string.h>

#ifndef VK_API_VERSION_1_3
#define VK_API_VERSION_1_3 VK_MAKE_API_VERSION(0, 1, 3, 0)
#endif

#if VK_HEADER_VERSION < 250 || !defined(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)
void vkGetDeviceBufferMemoryRequirements(VkDevice device, const void *pInfo,
                                         VkMemoryRequirements *pMemoryRequirements) {
    (void)device; (void)pInfo;
    if (pMemoryRequirements) memset(pMemoryRequirements, 0, sizeof *pMemoryRequirements);
}

void vkGetDeviceImageMemoryRequirements(VkDevice device, const void *pInfo,
                                        VkMemoryRequirements *pMemoryRequirements) {
    (void)device; (void)pInfo;
    if (pMemoryRequirements) memset(pMemoryRequirements, 0, sizeof *pMemoryRequirements);
}
#endif
