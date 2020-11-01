#include "../include/visky/visky.h"

#define STR(r)                                                                                    \
    case VK_##r:                                                                                  \
        str = #r;                                                                                 \
        break
#define noop

#define VK_CHECK_RESULT(f)                                                                        \
    {                                                                                             \
        VkResult res = (f);                                                                       \
        char* str = "UNKNOWN_ERROR";                                                              \
        switch (res)                                                                              \
        {                                                                                         \
            STR(NOT_READY);                                                                       \
            STR(TIMEOUT);                                                                         \
            STR(EVENT_SET);                                                                       \
            STR(EVENT_RESET);                                                                     \
            STR(INCOMPLETE);                                                                      \
            STR(ERROR_OUT_OF_HOST_MEMORY);                                                        \
            STR(ERROR_OUT_OF_DEVICE_MEMORY);                                                      \
            STR(ERROR_INITIALIZATION_FAILED);                                                     \
            STR(ERROR_DEVICE_LOST);                                                               \
            STR(ERROR_MEMORY_MAP_FAILED);                                                         \
            STR(ERROR_LAYER_NOT_PRESENT);                                                         \
            STR(ERROR_EXTENSION_NOT_PRESENT);                                                     \
            STR(ERROR_FEATURE_NOT_PRESENT);                                                       \
            STR(ERROR_INCOMPATIBLE_DRIVER);                                                       \
            STR(ERROR_TOO_MANY_OBJECTS);                                                          \
            STR(ERROR_FORMAT_NOT_SUPPORTED);                                                      \
            STR(ERROR_SURFACE_LOST_KHR);                                                          \
            STR(ERROR_NATIVE_WINDOW_IN_USE_KHR);                                                  \
            STR(SUBOPTIMAL_KHR);                                                                  \
            STR(ERROR_OUT_OF_DATE_KHR);                                                           \
            STR(ERROR_INCOMPATIBLE_DISPLAY_KHR);                                                  \
            STR(ERROR_VALIDATION_FAILED_EXT);                                                     \
            STR(ERROR_INVALID_SHADER_NV);                                                         \
        default:                                                                                  \
            noop;                                                                                 \
        }                                                                                         \
        if (res != VK_SUCCESS)                                                                    \
        {                                                                                         \
            log_error("VkResult is %s in %s at line %s", str, __FILE__, __LINE__);                \
        }                                                                                         \
    }

static VkDeviceSize texture_size_bytes(VkyTextureParams params)
{
    return params.width * params.height * params.depth * params.format_bytes;
}

static uint64_t next_pow2(uint64_t x)
{
    uint64_t p = 1;
    while (p < x)
        p *= 2;
    return p;
}

static size_t compute_dynamic_alignment(size_t dynamic_alignment, size_t min_ubo_alignment)
{
    if (min_ubo_alignment > 0)
    {
        dynamic_alignment = (dynamic_alignment + min_ubo_alignment - 1) & ~(min_ubo_alignment - 1);
    }
    dynamic_alignment = next_pow2(dynamic_alignment);
    return dynamic_alignment;
}

static VkResult create_debug_utils_messenger_EXT(
    VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger)
{
    PFN_vkCreateDebugUtilsMessengerEXT func =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != NULL)
    {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    }
    else
    {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
    // HACK: hide harmless warning message on Ubuntu:
    // validation layer: /usr/lib/i386-linux-gnu/libvulkan_radeon.so: wrong ELF class: ELFCLASS32
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        strstr(pCallbackData->pMessage, "ELFCLASS32") == NULL)
        log_error("validation layer: %s", pCallbackData->pMessage);
    return VK_FALSE;
}

static void destroy_debug_utils_messenger_EXT(
    VkInstance instance, VkDebugUtilsMessengerEXT debug_messenger,
    const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroyDebugUtilsMessengerEXT func =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != NULL)
    {
        func(instance, debug_messenger, pAllocator);
    }
}

static bool check_validation_layer_support(
    const uint32_t validation_layers_count, const char** validation_layers)
{
    uint32_t layer_count;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);

    VkLayerProperties* available_layers = calloc(layer_count, sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers);

    for (uint32_t i = 0; i < validation_layers_count; i++)
    {
        bool layerFound = false;
        const char* layerName = validation_layers[i];
        for (uint32_t j = 0; j < layer_count; j++)
        {
            if (strcmp(layerName, available_layers[j].layerName) == 0)
            {
                layerFound = true;
                break;
            }
        }
        if (!layerFound)
        {
            FREE(available_layers);
            return false;
        }
    }
    FREE(available_layers);
    return true;
}

static uint32_t find_memory_type(
    uint32_t typeFilter, VkMemoryPropertyFlags properties,
    VkPhysicalDeviceMemoryProperties mem_properties)
{
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++)
    {
        if ((typeFilter & (uint32_t)(1 << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    log_error("could not find an appropriate memory type");
    return 0;
}

static void transition_image_layout(
    VkDevice device, VkCommandPool command_pool, VkQueue graphics_queue, VkImage image,
    VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout)
{
    log_trace("transition image layout");

    // TODO: refactor with vky_texture_barrier()

    VkCommandBuffer command_buffer = begin_single_time_commands(device, command_pool);

    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags source_stage;
    VkPipelineStageFlags destination_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (
        old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        new_layout != VK_IMAGE_LAYOUT_UNDEFINED)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        log_error("image transition failed with new layout %d", new_layout);
    }

    vkCmdPipelineBarrier(
        command_buffer, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);

    end_single_time_commands(device, command_pool, &command_buffer, graphics_queue);
}

static void create_render_pass(
    VkDevice device, VkFormat swapchain_image_format, VkImageLayout image_layout,
    VkRenderPass* render_pass, bool do_clear)
{
    log_trace("create render pass");
    VkAttachmentDescription colorAttachment = {0};
    colorAttachment.format = swapchain_image_format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp =
        do_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = image_layout;

    VkAttachmentDescription depthAttachment = {0};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp =
        do_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef = {0};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef = {0};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency = {0};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[2] = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo render_pass_info = {0};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 2;
    render_pass_info.pAttachments = attachments;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;

    VK_CHECK_RESULT(vkCreateRenderPass(device, &render_pass_info, NULL, render_pass));
}

static void copy_buffer(
    VkDevice device, VkCommandPool command_pool, VkQueue graphics_queue, VkBuffer src_buffer,
    VkBuffer dst_buffer, VkBufferCopy copy_region, VkBufferMemoryBarrier* barrier)
{
    VkCommandBuffer command_buffer = begin_single_time_commands(device, command_pool);
    vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy_region);
    // Barrier when updating a storage vertex buffer also used for compute, and when the graphics
    // and compute queue families do not match.
    if (barrier != NULL)
    {
        vkCmdPipelineBarrier(
            command_buffer, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, barrier, 0, NULL);
    }
    end_single_time_commands(device, command_pool, &command_buffer, graphics_queue);
}

static void copy_buffer_to_image(
    VkDevice device, VkCommandPool command_pool, VkQueue graphics_queue, VkBuffer buffer,
    VkImage image, uint32_t width, uint32_t height, uint32_t depth)
{
    VkCommandBuffer command_buffer = begin_single_time_commands(device, command_pool);
    VkBufferImageCopy region = {0};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset.x = 0;
    region.imageOffset.y = 0;
    region.imageOffset.z = 0;

    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(depth > 0);

    region.imageExtent.width = width;
    region.imageExtent.height = height;
    region.imageExtent.depth = depth;

    vkCmdCopyBufferToImage(
        command_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    end_single_time_commands(device, command_pool, &command_buffer, graphics_queue);
}

static void upload_data_to_buffer(
    VkDevice device, VkBufferCopy copy_region, const void* data, VkBuffer buffer,
    VkPhysicalDeviceMemoryProperties memory_properties, VkCommandPool command_pool,
    VkQueue graphics_queue, VkBufferMemoryBarrier* barrier)
{

    VkBuffer staging_buffer;
    VkDeviceMemory staging_buffer_memory;
    VkDeviceSize size = copy_region.size;

    create_buffer(
        device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_buffer, &staging_buffer_memory,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        memory_properties);

    void* cdata = NULL;
    vkMapMemory(device, staging_buffer_memory, 0, size, 0, &cdata);
    memcpy(cdata, data, size);
    vkUnmapMemory(device, staging_buffer_memory);

    copy_buffer(
        device, command_pool, graphics_queue, staging_buffer, buffer, copy_region, barrier);

    vkDestroyBuffer(device, staging_buffer, NULL);
    vkFreeMemory(device, staging_buffer_memory, NULL);
}

static void upload_uniform_data(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    const void* data)
{
    void* cdata = NULL;
    vkMapMemory(device, memory, offset, size, 0, &cdata);
    memcpy(cdata, data, size);
    vkUnmapMemory(device, memory);
}
