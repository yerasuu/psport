// SPDX-License-Identifier: GPL-2.0-or-later
/* Independent backend smoke test: command submission and readback, no game graphics. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan: %s returned %d\n", #call, r); exit(1); } } while (0)

int vulkan_smoke(void) {
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="native probe", .apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app};
    VkInstance p_instance; CHECK(vkCreateInstance(&ici, NULL, &p_instance));
    uint32_t c_devices = 0; CHECK(vkEnumeratePhysicalDevices(p_instance, &c_devices, NULL));
    if (!c_devices) { fprintf(stderr, "Vulkan: no physical devices\n"); vkDestroyInstance(p_instance, NULL); return 1; }
    VkPhysicalDevice *pp_devices = calloc(c_devices, sizeof(*pp_devices));
    if (!pp_devices) exit(1);
    CHECK(vkEnumeratePhysicalDevices(p_instance, &c_devices, pp_devices));
    VkPhysicalDevice p_physical = pp_devices[0]; free(pp_devices);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(p_physical, &properties);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(p_physical, &nq, NULL);
    VkQueueFamilyProperties *p_families = calloc(nq, sizeof(*p_families));
    if (!p_families) exit(1);
    vkGetPhysicalDeviceQueueFamilyProperties(p_physical, &nq, p_families);
    uint32_t family = 0;
    while (family < nq && !(p_families[family].queueCount && (p_families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))) ++family;
    free(p_families);
    if (family == nq) { fprintf(stderr, "Vulkan: no graphics queue\n"); vkDestroyInstance(p_instance, NULL); return 1; }
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=family, .queueCount=1, .pQueuePriorities=&priority};
    VkDeviceCreateInfo dci = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount=1, .pQueueCreateInfos=&qci};
    VkDevice p_device; CHECK(vkCreateDevice(p_physical, &dci, NULL, &p_device));
    VkQueue p_queue; vkGetDeviceQueue(p_device, family, 0, &p_queue);
    VkBufferCreateInfo bci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=4096, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer p_buffer; CHECK(vkCreateBuffer(p_device, &bci, NULL, &p_buffer));
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(p_device, p_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(p_physical, &memory);
    uint32_t type = 0, wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < memory.memoryTypeCount && !((requirements.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & wanted) == wanted)) ++type;
    if (type == memory.memoryTypeCount) { fprintf(stderr, "Vulkan: no coherent host memory\n"); exit(1); }
    VkMemoryAllocateInfo mai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=requirements.size, .memoryTypeIndex=type};
    VkDeviceMemory p_allocation; CHECK(vkAllocateMemory(p_device, &mai, NULL, &p_allocation));
    CHECK(vkBindBufferMemory(p_device, p_buffer, p_allocation, 0));
    VkCommandPoolCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=family};
    VkCommandPool p_pool; CHECK(vkCreateCommandPool(p_device, &pci, NULL, &p_pool));
    VkCommandBufferAllocateInfo cai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=p_pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
    VkCommandBuffer p_command; CHECK(vkAllocateCommandBuffers(p_device, &cai, &p_command));
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(p_command, &begin));
    vkCmdFillBuffer(p_command, p_buffer, 0, 4096, 0xb100db0e);
    VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(p_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(p_command));
    VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&p_command};
    CHECK(vkQueueSubmit(p_queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(p_queue));
    uint32_t *p_pixels; CHECK(vkMapMemory(p_device, p_allocation, 0, 4096, 0, (void **)&p_pixels));
    int is_ok = 1; for (int i=0; i<1024; ++i) if (p_pixels[i] != 0xb100db0e) is_ok = 0;
    vkUnmapMemory(p_device, p_allocation);
    printf("Vulkan: %s; command submission + 4096-byte readback %s\n", properties.deviceName, is_ok ? "PASS" : "FAIL");
    vkDestroyCommandPool(p_device, p_pool, NULL); vkDestroyBuffer(p_device, p_buffer, NULL);
    vkFreeMemory(p_device, p_allocation, NULL); vkDestroyDevice(p_device, NULL); vkDestroyInstance(p_instance, NULL);
    return is_ok ? 0 : 1;
}
