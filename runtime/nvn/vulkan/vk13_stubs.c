// [AllGenU-Android] Stubs de Vulkan 1.3 que VulkanMemoryAllocator referencia.
//
// VMA compila rutas de VK_KHR_maintenance5 (Vulkan 1.3) y con ellas referencia
// vkGetDeviceBufferMemoryRequirements / vkGetDeviceImageMemoryRequirements.
// El header de Vulkan del NDK las DECLARA, pero libvulkan.so del telefono no las
// exporta hasta Android 13, asi que el enlazado falla en un APK con minSdk 29-30.
//
// Estas definiciones existen para que el enlace cierre. En un telefono sin
// Vulkan 1.3 nunca se llaman (VMA solo las usa si el driver anuncia la
// extension). Si se llegaran a llamar, devuelven un requisito de memoria vacio
// en vez de tocar el driver.
//
// Para quitarlas: compila con un NDK/API >= 33 y borra este archivo del target.
#include <string.h>

#include <vulkan/vulkan.h>

void vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo,
                                         VkMemoryRequirements2 *pMemoryRequirements) {
    (void)device;
    (void)pInfo;
    if (pMemoryRequirements) memset(pMemoryRequirements, 0, sizeof *pMemoryRequirements);
}

void vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
                                        VkMemoryRequirements2 *pMemoryRequirements) {
    (void)device;
    (void)pInfo;
    if (pMemoryRequirements) memset(pMemoryRequirements, 0, sizeof *pMemoryRequirements);
}
