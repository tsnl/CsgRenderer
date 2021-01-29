// RENDERER uses the Vulkan API to turn a bag of nodes with attributes
// into images on screen.
// 
// Most of this code is based on a tutorial at
//  vulkan-tutorial.com/en
//
// Pending features:
// 1.   Swap chain recreation (resize, sub-optimal e.g. monitor change)
//      https://vulkan-tutorial.com/en/Drawing_a_triangle/Swap_chain_recreation

#include "renderer.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "wololo/wmath.h"
#include "wololo/platform.h"
#include "wololo/app.h"

#include "wololo/config.h"

#include <vulkan/vulkan.h>

#define MIN(A,B) (A < B ? A : B)
#define MAX(A,B) (A > B ? A : B)

//
//
// Local implementation:
//
//

#define DEFAULT_VK_VALIDATION_LAYER_COUNT (1)
static char const* default_vk_validation_layer_names[DEFAULT_VK_VALIDATION_LAYER_COUNT] = {
    "VK_LAYER_KHRONOS_validation"
};

// minimum extensions are required for the app to run at all.
#define MINIMUM_VK_DEVICE_EXTENSION_COUNT (1)
static char const* minimum_vk_device_extension_names[MINIMUM_VK_DEVICE_EXTENSION_COUNT] = {
    "VK_KHR_swapchain"
};

// 'frames in flight' refer to the number of swapchain images we can render to simultaneously:
// see: https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Rendering_and_presentation#page_Submitting-the-command-buffer
#define MAX_FRAMES_IN_FLIGHT (2)

// creating a Vulkan error callback:
// see: https://vulkan-tutorial.com/Drawing_a_triangle/Setup/Validation_layers#page_Message-callback
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    VkDebugUtilsMessageTypeFlagsEXT message_type,
    VkDebugUtilsMessengerCallbackDataEXT const* callback_data_ref,
    void* user_data
) {
    if (message_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        printf(
            "[Wololo][Vulkan-Validation] %s\n",
            callback_data_ref->pMessage
        );
    }
    return VK_TRUE;
}

// Fragment-UBO (Uniform Buffer Object) shared with shaders and constant
// after renderer initialization:
typedef struct FragmentUniformBufferObject FragmentUniformBufferObject;
struct FragmentUniformBufferObject {
    // field 1: float time_since_start_sec
    float time_since_start_sec;

    // field 2: vec2 resolution
    float resolution_x;
    float resolution_y;
};

// Vulkan buffer creation:
uint32_t help_find_vk_buffer_memory_type(
    VkPhysicalDevice physical_device,
    uint32_t type_filter,
    VkMemoryPropertyFlags properties
) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        bool bit_supported = type_filter & (1 << i);
        bool props_supported = properties == (mem_properties.memoryTypes[i].propertyFlags & properties);
        if (bit_supported && props_supported) {
            return i;
        }
    }

    assert(0 && "Failed to find suitable Vulkan buffer memory type.");
    return -1;
}
void new_vk_buffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer* buffer_p,
    VkDeviceMemory* buffer_memory_p
) {
    VkBufferCreateInfo buffer_info; {
        memset(&buffer_info, 0, sizeof(buffer_info));
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VkResult buffer_ok = vkCreateBuffer(
        device,
        &buffer_info,
        NULL,
        buffer_p
    );
    if (buffer_ok != VK_SUCCESS) {
        printf("[Wololo] Failed to create Vulkan buffer.\n");
        assert(0 && "Vulkan buffer creation failed.");
    }

    VkMemoryRequirements mem_requirements;
    memset(&mem_requirements, 0, sizeof(mem_requirements));
    vkGetBufferMemoryRequirements(device, *buffer_p, &mem_requirements);

    VkMemoryAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = help_find_vk_buffer_memory_type(
        physical_device,
        mem_requirements.memoryTypeBits,
        properties
    );
    VkResult alloc_ok = vkAllocateMemory(
        device, &alloc_info, NULL,
        buffer_memory_p
    );
    if (alloc_ok != VK_SUCCESS) {
        printf("[Wololo] Failed to allocate Vulkan buffer.\n");
        assert(0 && "Vulkan buffer allocation failed.");
    }

    // finally, associating the buffer's ID with the memory:
    vkBindBufferMemory(
        device,
        *buffer_p,
        *buffer_memory_p,
        0
    );
}
void new_vk_uniform_buffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBuffer* buffer_p,
    VkDeviceMemory* buffer_memory_p
) {
    new_vk_buffer(
        physical_device,
        device,
        size,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        buffer_p,
        buffer_memory_p
    );
}

//
// Nodes: representation of an algebraic expression
//

typedef enum NodeType NodeType;
typedef union NodeInfo NodeInfo;
enum NodeType {
    WO_LEAF_SPHERE,
    WO_LEAF_INFINITE_PLANAR_PARTITION,
    WO_NODE_BINOP_UNION_OF,
    WO_NODE_BINOP_INTERSECTION_OF,
    WO_NODE_BINOP_DIFFERENCE_OF
};
union NodeInfo {
    struct {
        Wo_Scalar radius;
    } sphere;

    struct {
        Wo_Vec3 normal;
    } infinite_planar_partition;

    struct {
        Wo_Node_Argument left;
        Wo_Node_Argument right;
    } binop_of;
};

//
// Scenes: contain all nodes and their components
// 

typedef enum RequiredVkQueueFamily RequiredVkQueueFamily;

typedef struct Wo_Renderer Wo_Renderer;
struct Wo_Renderer {
    // Own properties:
    size_t max_node_count;
    size_t current_node_count;
    NodeType* node_type_table;
    NodeInfo* node_info_table;
    uint64_t* node_is_nonroot_bitset;
    char* name;
    Wo_App* app;

    // Vulkan instances & devices:
    VkInstance vk_instance;
    VkPhysicalDevice vk_physical_device;
    uint32_t vk_physical_device_count;
    VkPhysicalDevice* vk_physical_devices;
    
    // Vulkan validation layers:
    bool are_vk_validation_layers_enabled;
    VkLayerProperties* vk_available_validation_layers;
    uint32_t vk_enabled_validation_layer_count;
    char const** vk_enabled_validation_layer_names;
    
    // Vulkan queue families:
    uint32_t vk_queue_family_count;
    VkQueueFamilyProperties* vk_queue_family_properties;
    
    // Vulkan queues:
    uint32_t vk_graphics_queue_family_index;
    uint32_t vk_present_queue_family_index;
    VkQueue vk_graphics_queue;
    VkQueue vk_present_queue;

    // Vulkan devices and extensions:
    VkDevice vk_device;
    uint32_t vk_available_device_extension_count;
    VkExtensionProperties* vk_available_device_extensions;
    uint32_t vk_enabled_device_extension_count;
    char const** vk_enabled_device_extension_names;

    // surface to present to
    // chosen format + modes from after surface creation:
    VkSurfaceKHR vk_present_surface;
    VkSurfaceFormatKHR vk_chosen_present_surface_format;
    VkPresentModeKHR vk_chosen_present_surface_mode;

    // available surface present-mode and format:
    VkSurfaceCapabilitiesKHR vk_present_surface_capabilities;
    uint32_t vk_available_format_count;
    VkSurfaceFormatKHR* vk_available_formats;
    uint32_t vk_available_present_mode_count;
    VkPresentModeKHR* vk_available_present_modes;
    
    // chosen swap-extent/aka framebuffer size:
    VkExtent2D vk_frame_extent;
    VkViewport vk_viewport;

    // the swapchain:
    VkSwapchainKHR vk_swapchain;
    uint32_t vk_swapchain_images_count;
    VkImage* vk_swapchain_images;
    VkImageView* vk_swapchain_image_views;
    // the swapchain's framebuffers:
    VkFramebuffer* vk_swapchain_framebuffers;
    uint32_t vk_swapchain_fbs_ok_count;

    // shader modules:
    bool vk_shaders_loaded_ok;
    VkShaderModule vk_vert_shader_module;
    VkShaderModule vk_frag_shader_module;

    // pipeline layout (for uniforms):
    bool vk_pipeline_layout_created_ok;
    VkDescriptorSetLayout vk_descriptor_set_layout;
    VkPipelineLayout vk_pipeline_layout;
    VkRenderPass vk_render_pass;
    bool vk_render_pass_ok;
    bool vk_descriptor_set_layout_ok;

    // the graphics pipeline
    VkPipeline vk_graphics_pipeline;
    bool vk_graphics_pipeline_ok;
    
    // command pools:
    VkCommandPool vk_command_buffer_pool;
    bool vk_command_buffer_pool_ok;
    VkCommandBuffer* vk_command_buffers;
    bool vk_command_buffers_ok;

    // uniform buffers, per-swapchain image:
    VkBuffer* uniform_buffers;
    VkDeviceMemory* uniform_buffers_memory;
    // descriptor queues, used to bind uniform buffers:
    VkDescriptorPool vk_descriptor_pool;
    VkDescriptorSet* vk_descriptor_sets;
    bool vk_descriptor_pool_ok;

    // drawing routine semaphores:
    VkSemaphore vk_image_available_semaphores[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore vk_render_finished_semaphores[MAX_FRAMES_IN_FLIGHT];
    VkFence vk_inflight_fences[MAX_FRAMES_IN_FLIGHT];
    VkFence* vk_images_inflight_fences;
    size_t current_frame_index;
    bool vk_semaphores_oks[MAX_FRAMES_IN_FLIGHT];
};


Wo_Renderer* new_renderer(Wo_App* app, char const* name, size_t max_node_count);
Wo_Renderer* allocate_renderer(char const* name, size_t max_node_count);
Wo_Renderer* vk_init_renderer(Wo_App* app, Wo_Renderer* renderer);
VkShaderModule vk_load_shader_module(Wo_Renderer* renderer, char const* file_path);

void del_renderer(Wo_Renderer* renderer);
void draw_frame_with_renderer(Wo_Renderer* renderer);
bool allocate_node(Wo_Renderer* renderer, Wo_Node* out_node);
void set_nonroot_node(Wo_Renderer* renderer, Wo_Node node);

//
// Implementation:
//

Wo_Renderer* new_renderer(Wo_App* app, char const* name, size_t max_node_count) {
    // allocating all the memory we need:
    Wo_Renderer* renderer = allocate_renderer(name, max_node_count);
    renderer = vk_init_renderer(app, renderer);
    renderer->current_frame_index = 0;
    return renderer;
}
Wo_Renderer* allocate_renderer(char const* name, size_t max_node_count) {
    size_t subslab0_renderer_size_in_bytes = sizeof(Wo_Renderer);
    size_t subslab1_type_table_size_in_bytes = sizeof(NodeType) * max_node_count;
    size_t subslab2_info_table_size_in_bytes = sizeof(NodeInfo) * max_node_count;
    size_t subslab3_root_bitset_size_in_bytes = ((max_node_count/64 + 1)*64) / 8;
    size_t subslab4_name_size_in_bytes = 0; {
        size_t name_length = strlen(name);
        if (name_length > 0) {
            subslab4_name_size_in_bytes = 1+name_length;
        }
    }
    if (subslab4_name_size_in_bytes == 1) {
        subslab4_name_size_in_bytes = 0;
    }
    size_t slab_size_in_bytes = (
        subslab0_renderer_size_in_bytes +
        subslab1_type_table_size_in_bytes + 
        subslab2_info_table_size_in_bytes +
        subslab3_root_bitset_size_in_bytes +
        subslab4_name_size_in_bytes
    );

    // 0-initializing the Renderer and all its slab data:
    uint8_t* mem_slab = calloc(slab_size_in_bytes, 1);
    if (mem_slab == NULL) {
        fprintf(stderr, "[Wololo] Failed to allocate %zu bytes for Wo_Renderer.\n", slab_size_in_bytes);
        return NULL;
    }

    Wo_Renderer* renderer = (void*)(mem_slab + 0);
    renderer->max_node_count = max_node_count;
    renderer->current_node_count = 0;
    renderer->node_type_table = (void*)&mem_slab[
        subslab0_renderer_size_in_bytes
    ];
    renderer->node_info_table = (void*)&mem_slab[
        subslab0_renderer_size_in_bytes + 
        subslab1_type_table_size_in_bytes
    ];
    renderer->node_is_nonroot_bitset = (void*)&mem_slab[
        subslab0_renderer_size_in_bytes + 
        subslab1_type_table_size_in_bytes + 
        subslab2_info_table_size_in_bytes
    ];
    renderer->name = NULL;
    if (subslab4_name_size_in_bytes > 0) {
        renderer->name = (void*)&mem_slab[
            subslab0_renderer_size_in_bytes + 
            subslab1_type_table_size_in_bytes + 
            subslab2_info_table_size_in_bytes +
            subslab3_root_bitset_size_in_bytes
        ];
        renderer->name = strncpy(renderer->name, name, subslab4_name_size_in_bytes);
    }
    return renderer;
}
Wo_Renderer* vk_init_renderer(Wo_App* app, Wo_Renderer* renderer) {
    // setting ambient state variables:
    renderer->app = app;
    renderer->are_vk_validation_layers_enabled = WO_DEBUG;

    // we know all pointers are NULL-initialized, so if non-NULL, we know there's an error.

    // checking that GLFW supports Vulkan:
    assert(glfwVulkanSupported() == GLFW_TRUE && "GLFW should support Vulkan.");

    // creating a Vulkan instance, applying validation layers:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Instance
    {
        VkApplicationInfo app_info; {
            memset(&app_info, 0, sizeof(app_info));
            app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app_info.pApplicationName = renderer->name;
            app_info.applicationVersion = VK_MAKE_VERSION(0, 0, 0);
            app_info.pEngineName = "Wololo Csg Renderer";
            app_info.engineVersion = VK_MAKE_VERSION(0, 0, 0);
            app_info.apiVersion = VK_API_VERSION_1_0;
        }

        VkInstanceCreateInfo create_info; {
            memset(&create_info, 0, sizeof(create_info));
            create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            create_info.pApplicationInfo = &app_info;
            create_info.enabledLayerCount = 0;
            
            uint32_t glfw_extension_count = 0;
            char const** glfw_extensions = NULL;
            glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
            create_info.enabledExtensionCount = glfw_extension_count;
            create_info.ppEnabledExtensionNames = glfw_extensions;
            
            // setting up validation layers:
            // https://vulkan-tutorial.com/Drawing_a_triangle/Setup/Validation_layers
            if (renderer->are_vk_validation_layers_enabled) {
                // querying available:
                uint32_t available_validation_layer_count = 0;
                vkEnumerateInstanceLayerProperties(
                    &available_validation_layer_count, 
                    NULL
                );
                renderer->vk_available_validation_layers = calloc(
                    available_validation_layer_count * sizeof(VkLayerProperties),
                    1
                );
                vkEnumerateInstanceLayerProperties(
                    &available_validation_layer_count, 
                    renderer->vk_available_validation_layers
                );
                
                // tallying each available validation layer:
                renderer->vk_enabled_validation_layer_count = 0;
                renderer->vk_enabled_validation_layer_names = calloc(
                    DEFAULT_VK_VALIDATION_LAYER_COUNT * sizeof(char const*),
                    1
                );
                for (uint32_t index = 0; index < DEFAULT_VK_VALIDATION_LAYER_COUNT; index++) {
                    char const* req_name = default_vk_validation_layer_names[index];

                    // checking if desired name is available:
                    bool found = false;
                    for (uint32_t available_index = 0; available_index < available_validation_layer_count; available_index++) {
                        char const* available_validation_layer_name = renderer->vk_available_validation_layers[available_index].layerName;
                        if (0 == strcmp(req_name, available_validation_layer_name)) {
                            found = true;
                            break;
                        }
                    }
                    
                    // and updating the 'enabled' table and count:
                    if (found) {
                        printf("[Wololo] Found Vulkan validation layer \"%s\".\n", req_name);
                        uint32_t index = renderer->vk_enabled_validation_layer_count++;
                        renderer->vk_enabled_validation_layer_names[index] = req_name;
                    } else {
                        printf("[Wololo] Could not find support for validation layer: \"%s\"\n", req_name);
                    }
                }

                // setting create_info properties to give validation layers to Vulkan:
                create_info.enabledLayerCount = renderer->vk_enabled_validation_layer_count;
                create_info.ppEnabledLayerNames = renderer->vk_enabled_validation_layer_names;
            } else {
                renderer->vk_enabled_validation_layer_count = create_info.enabledLayerCount = 0;
                renderer->vk_enabled_validation_layer_names = NULL;
            }
        }

        VkResult result = vkCreateInstance(&create_info, NULL, &renderer->vk_instance);
        if (result != VK_SUCCESS) {
            printf("[Wololo] Failed to create a Vulkan instance!\n");
            goto fatal_error;
        }
    }

    // skipping 'setupDebugMessenger'; we're stdout heathen here. :)

    // picking a physical device:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Physical_devices_and_queue_families
    {
        // querying all physical devices:
        vkEnumeratePhysicalDevices(
            renderer->vk_instance, 
            &renderer->vk_physical_device_count, 
            NULL
        );
        if (renderer->vk_physical_device_count == 0) {
            printf("[Wololo] Could not find any physical devices supporting Vulkan.\n");
            goto fatal_error;
        }
        
        // enumerating all physical devices:
        renderer->vk_physical_devices = calloc(
            renderer->vk_physical_device_count,
            sizeof(VkPhysicalDevice)
        );
        vkEnumeratePhysicalDevices(
            renderer->vk_instance, 
            &renderer->vk_physical_device_count, 
            renderer->vk_physical_devices
        );
    
        // just picking the first device:
        renderer->vk_physical_device = renderer->vk_physical_devices[0];

        // reporting:
        {
            VkPhysicalDeviceProperties physical_device_properties;
            vkGetPhysicalDeviceProperties(renderer->vk_physical_device, &physical_device_properties);

            VkPhysicalDeviceFeatures physical_device_features;
            vkGetPhysicalDeviceFeatures(renderer->vk_physical_device, &physical_device_features);
            
            printf(
                "[Wololo] Initializing with Physical Device \"%s\"\n",
                physical_device_properties.deviceName
            );
        }
    }
    
    // creating a logical device, setting command queues + queue indices:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Logical_device_and_queues
    {
        VkDeviceQueueCreateInfo queue_create_info;
        memset(&queue_create_info, 0, sizeof(queue_create_info));
        queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = renderer->vk_graphics_queue_family_index;
        queue_create_info.queueCount = 1;
        float const queue_priority = 1.0f;
        queue_create_info.pQueuePriorities = &queue_priority;

        // don't need any special device features:
        VkPhysicalDeviceFeatures device_features;
        memset(&device_features, 0, sizeof(device_features));

        // creating the instance...
        VkDeviceCreateInfo create_info;
        memset(&create_info, 0, sizeof(create_info));
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

        // setting up queues:
        create_info.pQueueCreateInfos = &queue_create_info;
        create_info.queueCreateInfoCount = 1;
        create_info.pEnabledFeatures = &device_features;
    
        // loading extensions (like the Swap-chain extension) into create_info:
        // https://vulkan-tutorial.com/Drawing_a_triangle/Presentation/Swap_chain
        
        // setting 'renderer->vk_available_device_extension_count'
        vkEnumerateDeviceExtensionProperties(
            renderer->vk_physical_device, NULL,
            &renderer->vk_available_device_extension_count,
            NULL
        );
        
        // allocating + filling 'renderer->vk_available_device_extensions':
        renderer->vk_available_device_extensions = calloc(
            sizeof(VkExtensionProperties),
            renderer->vk_available_device_extension_count
        );
        vkEnumerateDeviceExtensionProperties(
            renderer->vk_physical_device, NULL,
            &renderer->vk_available_device_extension_count,
            renderer->vk_available_device_extensions
        );

        bool const debug_print_all_available_exts = false;
        if (debug_print_all_available_exts) {
            uint32_t ext_count = renderer->vk_available_device_extension_count;
            printf("[Vulkan] %d extensions found:\n", ext_count);
            for (uint32_t i = 0; i < ext_count; i++) {
                printf("\t[%d/%d] %s\n", 
                    i+1, ext_count,
                    renderer->vk_available_device_extensions[i].extensionName
                );
            }
        }

        // storing all enabled extensions' names:
        uint32_t max_extension_count = (
            MINIMUM_VK_DEVICE_EXTENSION_COUNT + 
            0
        );
        renderer->vk_enabled_device_extension_count = 0;
        renderer->vk_enabled_device_extension_names = calloc(max_extension_count, sizeof(char const*));
        for (uint32_t min_ext_index = 0; min_ext_index < MINIMUM_VK_DEVICE_EXTENSION_COUNT; min_ext_index++) {
            char const* ext_name = minimum_vk_device_extension_names[min_ext_index];
            
            // searching for this extension:
            bool ext_found = false;
            uint32_t const available_count = renderer->vk_available_device_extension_count;
            for (uint32_t available_ext_index = 0; available_ext_index < available_count; available_ext_index++) {
                VkExtensionProperties available_ext_props = renderer->vk_available_device_extensions[available_ext_index];
                char const* available_ext_name = available_ext_props.extensionName;

                if (0 == strcmp(available_ext_name, ext_name)) {
                    ext_found = true;
                    break;
                }
            }

            if (ext_found) {
                printf("[Wololo] Initializing Vulkan device extension \"%s\"\n", ext_name);
                uint32_t index = renderer->vk_enabled_device_extension_count++;
                renderer->vk_enabled_device_extension_names[index] = ext_name;
            } else {
                printf("[Wololo] Could not find support for Vulkan device extension \"%s\"\n", ext_name);
                goto fatal_error;
            }
        }
        
        // setting extension request args:
        create_info.enabledExtensionCount = renderer->vk_enabled_device_extension_count;
        create_info.ppEnabledExtensionNames = renderer->vk_enabled_device_extension_names;

        // setting up validation layers:
        if (renderer->are_vk_validation_layers_enabled) {
            create_info.enabledLayerCount = DEFAULT_VK_VALIDATION_LAYER_COUNT;
            create_info.ppEnabledLayerNames = default_vk_validation_layer_names;
        } else {
            create_info.enabledLayerCount = 0;
        }
    
        // creating logical `renderer->vk_device` using 'create_info'
        VkResult result = vkCreateDevice(
            renderer->vk_physical_device, 
            &create_info, 
            NULL, 
            &renderer->vk_device
        );
        if (result != VK_SUCCESS) {
            printf("[Wololo] Failed to create a Vulkan logical device.\n");
            goto fatal_error;
        }
    }

    // creating a window surface to present to:
    // https://vulkan-tutorial.com/Drawing_a_triangle/Presentation/Window_surface#page_Querying-for-presentation-support
    {
        // renderer->vk_present_surface:
        GLFWwindow* glfw_window = wo_app_glfw_window(renderer->app);
        VkResult result = glfwCreateWindowSurface(
            renderer->vk_instance,
            glfw_window,
            NULL,
            &renderer->vk_present_surface
        );
        if (result != VK_SUCCESS) {
            printf("[Wololo] Failed to create Vulkan surface using GLFW.\n");
            goto fatal_error;
        }
    }

    // assigning device queues using the new surface:
    {
        // checking the device's queue properties:
        // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Physical_devices_and_queue_families
        {   
            uint32_t queue_family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(renderer->vk_physical_device, &queue_family_count, NULL);
            renderer->vk_queue_family_count = queue_family_count;
            renderer->vk_graphics_queue_family_index = renderer->vk_queue_family_count;
            renderer->vk_present_queue_family_index = renderer->vk_queue_family_count;
            
            renderer->vk_queue_family_properties = calloc(queue_family_count, sizeof(VkQueueFamilyProperties));
            vkGetPhysicalDeviceQueueFamilyProperties(renderer->vk_physical_device, &queue_family_count, renderer->vk_queue_family_properties);

            for (uint32_t index = 0; index < renderer->vk_queue_family_count; index++) {
                VkQueueFamilyProperties family_properties = renderer->vk_queue_family_properties[index];
                if (family_properties.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    renderer->vk_graphics_queue_family_index = index;
                }

                VkBool32 present_supported = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(
                    renderer->vk_physical_device,
                    index,
                    renderer->vk_present_surface,
                    &present_supported
                );
                if (present_supported) {
                    renderer->vk_present_queue_family_index = index;
                }

                // if all indices have been assigned, we needn't search further:
                bool indices_are_complete = (
                    (renderer->vk_graphics_queue_family_index != renderer->vk_queue_family_count) &&
                    (renderer->vk_present_queue_family_index != renderer->vk_queue_family_count) &&
                    (true)
                );
                if (indices_are_complete) {
                    break;
                }
            }
            if (renderer->vk_graphics_queue_family_index == renderer->vk_queue_family_count) {
                printf("[Wololo] No queue family with VK_QUEUE_GRAPHICS_BIT was found.\n");
                goto fatal_error;
            } else {
                printf("[Wololo] Vulkan graphics queue loaded.\n");
            }
            if (renderer->vk_present_queue_family_index == renderer->vk_queue_family_count) {
                printf("[Wololo] No queue family with VK_QUEUE_PRESENT_BIT was found.\n");
                goto fatal_error;
            } else {
                printf("[Wololo] Vulkan present queue loaded.\n");
            }
        }

        // getting the device queue:
        {
            // renderer->vk_graphics_queue:
            vkGetDeviceQueue(
                renderer->vk_device,
                renderer->vk_graphics_queue_family_index,
                0,
                &renderer->vk_graphics_queue
            );

            // renderer->vk_present_queue:
            vkGetDeviceQueue(
                renderer->vk_device,
                renderer->vk_present_queue_family_index,
                0,
                &renderer->vk_present_queue
            );
        }
    }

    // creating the swapchain:
    {
        // Querying GPU swap chain capabilities:
        bool swap_chain_adequate = false;
        {
            // renderer->vk_present_surface_capabilities
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                renderer->vk_physical_device,
                renderer->vk_present_surface, 
                &renderer->vk_present_surface_capabilities
            );

            // counting, allocating, setting available formats:
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                renderer->vk_physical_device,
                renderer->vk_present_surface,
                &renderer->vk_available_format_count,
                NULL
            );
            if (renderer->vk_available_format_count > 0) {
                renderer->vk_available_formats = calloc(
                    sizeof(VkSurfaceFormatKHR),
                    renderer->vk_available_format_count
                );
                assert(renderer->vk_available_formats != NULL && "Out of memory-- calloc failed.");
            } else {
                renderer->vk_available_formats = NULL;
            }
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                renderer->vk_physical_device,
                renderer->vk_present_surface,
                &renderer->vk_available_format_count,
                renderer->vk_available_formats
            );

            // counting, allocating, setting available present modes:
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                renderer->vk_physical_device,
                renderer->vk_present_surface,
                &renderer->vk_available_present_mode_count,
                NULL
            );
            if (renderer->vk_available_present_mode_count > 0) {
                renderer->vk_available_present_modes = calloc(
                    sizeof(VkPresentModeKHR),
                    renderer->vk_available_present_mode_count
                );
                assert(renderer->vk_available_present_modes != NULL && "Out of memory-- calloc failed.");
            } else {
                renderer->vk_available_present_modes = NULL;
            }
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                renderer->vk_physical_device,
                renderer->vk_present_surface,
                &renderer->vk_available_present_mode_count,
                renderer->vk_available_present_modes
            );

            // checking that at least 1 format and present-mode exists:
            swap_chain_adequate = (
                (renderer->vk_available_format_count > 0) &&
                (renderer->vk_available_present_mode_count > 0)
            );
            if (!swap_chain_adequate) {
                goto fatal_error;
            }
        }

        // Choosing present format:
        {
            // if no ideal format can be found, default to the first available.
            uint32_t ideal_index = 0;

            // scanning for an ideal format, setting `ideal_index` if found:
            for (uint32_t i = 0; i < renderer->vk_available_format_count; i++) {
                bool ideal = (
                    (renderer->vk_available_formats[i].format == VK_FORMAT_B8G8R8A8_SRGB) &&
                    (renderer->vk_available_formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                );
                if (ideal) {
                    ideal_index = i;
                    break;
                }
            }

            // storing found ideal format on renderer:
            renderer->vk_chosen_present_surface_format = renderer->vk_available_formats[ideal_index];
        }

        // Choosing present mode:
        {
            uint32_t ideal_index = renderer->vk_available_present_mode_count;
            for (uint32_t i = 0; i < renderer->vk_available_present_mode_count; i++) {
                bool ideal = (
                    // triple buffering ideally
                    renderer->vk_available_present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR
                );
                if (ideal) {
                    ideal_index = i;
                    break;
                }
            }
            if (ideal_index == renderer->vk_available_present_mode_count) {
                // no ideal present mode found. 
                // Default to VK_PRESENT_MODE_FIFO_KHR, the only guaranteed available.
                renderer->vk_chosen_present_surface_mode = VK_PRESENT_MODE_FIFO_KHR;
            } else {
                renderer->vk_chosen_present_surface_mode = renderer->vk_available_present_modes[ideal_index];
            }
        }

        // Choosing swap extent:
        {
            if (renderer->vk_present_surface_capabilities.currentExtent.width != UINT32_MAX) {
                // special case; pick best resolution from minImageExtent to maxImageExtent bounds.
                renderer->vk_frame_extent = renderer->vk_present_surface_capabilities.currentExtent;
            } else {
                int width, height;
                glfwGetFramebufferSize(
                    wo_app_glfw_window(renderer->app), 
                    &width, &height
                );

                // initializing width and height to the frame-buffer size:
                renderer->vk_frame_extent.width = (uint32_t)width;
                renderer->vk_frame_extent.height = (uint32_t)height;
                
                // clamping width and height against minimum/maximum allowed:
                renderer->vk_frame_extent.width = MAX(
                    renderer->vk_present_surface_capabilities.minImageExtent.width,
                    renderer->vk_frame_extent.width
                );
                renderer->vk_frame_extent.width = MIN(
                    renderer->vk_present_surface_capabilities.maxImageExtent.width,
                    renderer->vk_frame_extent.width
                );
                renderer->vk_frame_extent.height = MAX(
                    renderer->vk_present_surface_capabilities.minImageExtent.height,
                    renderer->vk_frame_extent.height
                );
                renderer->vk_frame_extent.height = MIN(
                    renderer->vk_present_surface_capabilities.maxImageExtent.height,
                    renderer->vk_frame_extent.height
                );
            }

            // Finally creating the swapchain:
        
            // requesting number of images in the swap chain.
            // always requesting 1 extra so the GPU is never starved for frames while we render (double-buffer or better)
            // note '0' is a special value meaning no maximum.
            uint32_t image_count = MIN(
                1 + renderer->vk_present_surface_capabilities.minImageCount,
                renderer->vk_present_surface_capabilities.maxImageCount
            );

            VkSwapchainCreateInfoKHR swapchain_create_info;
            uint32_t queue_family_indices[2] = {
                renderer->vk_graphics_queue_family_index,
                renderer->vk_present_queue_family_index
            };
            {
                memset(&swapchain_create_info, 0, sizeof(swapchain_create_info));

                swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
                swapchain_create_info.surface = renderer->vk_present_surface;
                swapchain_create_info.minImageCount = image_count;
                swapchain_create_info.imageFormat = renderer->vk_chosen_present_surface_format.format;
                swapchain_create_info.imageColorSpace = renderer->vk_chosen_present_surface_format.colorSpace;
                swapchain_create_info.imageExtent = renderer->vk_frame_extent;
                swapchain_create_info.imageArrayLayers = 1;

                // note: for postprocessing, use 'VK_IMAGE_USAGE_TRANSFER_DST_BIT' instead and use
                //       a memory operation to transfer the rendered image to a swap chain image.
                swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            
                if (renderer->vk_graphics_queue_family_index != renderer->vk_present_queue_family_index) {
                    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
                    swapchain_create_info.queueFamilyIndexCount = 2;
                    swapchain_create_info.pQueueFamilyIndices = queue_family_indices;
                } else {
                    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                }

                swapchain_create_info.preTransform = renderer->vk_present_surface_capabilities.currentTransform;
                
                // no transparent windows, thank you:
                swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

                swapchain_create_info.presentMode = renderer->vk_chosen_present_surface_mode;
                
                swapchain_create_info.clipped = VK_TRUE;

                // do not handle resizing, i.e. only ever create one swapchain:
                swapchain_create_info.oldSwapchain = VK_NULL_HANDLE;
            }

            VkResult result = vkCreateSwapchainKHR(
                renderer->vk_device,
                &swapchain_create_info, NULL, 
                &renderer->vk_swapchain
            );
            if (result != VK_SUCCESS) {
                printf("[Wololo] Failed to create a Vulkan swapchain.\n");
                goto fatal_error;
            } else {
                printf(
                    "[Wololo] Successfully create a Vulkan swapchain with extent [%u x %u]\n",
                    renderer->vk_frame_extent.width,
                    renderer->vk_frame_extent.height
                );
            }
        }

        // Retrieving the created swapchain's images:
        {
            // setting `renderer->vk_swapchain_images_count`
            vkGetSwapchainImagesKHR(
                renderer->vk_device,
                renderer->vk_swapchain,
                &renderer->vk_swapchain_images_count,
                NULL
            );

            // allocating:
            renderer->vk_swapchain_images = calloc(
                sizeof(VkImage),
                renderer->vk_swapchain_images_count
            );

            // setting vk_swapchain_images:
            VkResult swapchain_images_ok = vkGetSwapchainImagesKHR(
                renderer->vk_device,
                renderer->vk_swapchain,
                &renderer->vk_swapchain_images_count,
                renderer->vk_swapchain_images
            );
            if (swapchain_images_ok != VK_SUCCESS) {
                printf("[Wololo] Failed to create Vulkan swapchain images.\n");
                goto fatal_error;
            } else {
                printf("[Wololo] Vulkan swapchain images created successfully.\n");
            }
        }
    }

    //
    // Presentation
    //

    // creating image views:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Presentation/Image_views
    // to use any VkImage, need VkImageView
    {
        // first, allocating the vk_swapchain_image_views array
        // s.t. there exists a unique image-view per image
        renderer->vk_swapchain_image_views = NULL;
        if (renderer->vk_swapchain_images_count > 0) {
            renderer->vk_swapchain_image_views = calloc(
                sizeof(VkImageView),
                renderer->vk_swapchain_images_count
            );
            assert(
                renderer->vk_swapchain_image_views &&
                "Allocation failed: renderer->vk_swapchain_image_views"
            );
        }

        // then, calling on Vulkan to create, thereby populating the array:
        for (size_t index = 0; index < renderer->vk_swapchain_images_count; index++) {
            VkImageViewCreateInfo create_info;
            {
                memset(&create_info, 0, sizeof(create_info));
                create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                create_info.image = renderer->vk_swapchain_images[index];
                create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                create_info.format = renderer->vk_chosen_present_surface_format.format;

                // each pixel is composed of one or more values corresponding to channels
                // in the output image, e.g. red (R), green (G), blue (B), and alpha (A).
                // 'swizzle' describes a linear transformation on a bit-vector, and just tells
                // Vulkan how to access each component.
                create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
                create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
                create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
                create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

                // setting up mipmapping levels, or multiple layers (for stereographic 3D)
                create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                create_info.subresourceRange.baseMipLevel = 0;
                create_info.subresourceRange.levelCount = 1;
                create_info.subresourceRange.baseArrayLayer = 0;
                create_info.subresourceRange.layerCount = 1;
            }

            // creating the VkImageView:
            VkResult ok = vkCreateImageView(
                renderer->vk_device, 
                &create_info, 
                NULL, 
                &renderer->vk_swapchain_image_views[index]
            );
            if (ok != VK_SUCCESS) {
                printf("[Wololo] Failed to create Vulkan image view %zu/%u.\n", index+1, renderer->vk_swapchain_images_count);
                goto fatal_error;
            } else {
                printf("[Wololo] Vulkan image view %zu/%u created successfully.\n", index+1, renderer->vk_swapchain_images_count);
            }
        }
    }

    // create render pass
    {
        // add a single color bufer attachment, represented by an image from the swapchain:
        VkAttachmentDescription color_attachment_desc;
        {
            memset(&color_attachment_desc, 0, sizeof(color_attachment_desc));
            color_attachment_desc.format = renderer->vk_chosen_present_surface_format.format;
            color_attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
            // loadOp and storeOp used for color and depth
            color_attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color_attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            // forsake the stencil buffer:
            color_attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color_attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // setting the final layout to a 'present' image layout:
            color_attachment_desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            color_attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            // not mentioned in tutorial:
            color_attachment_desc.flags = 0;
        }

        // creating one sub-pass:
        VkAttachmentReference color_attachment_ref;
        {
            memset(&color_attachment_ref, 0, sizeof(color_attachment_ref));
            color_attachment_ref.attachment = 0;
            color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkSubpassDescription subpass_desc;
        {
            // zero-ing out and filling relevant fields:
            memset(&subpass_desc, 0, sizeof(subpass_desc));
            subpass_desc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass_desc.colorAttachmentCount = 1;
            subpass_desc.pColorAttachments = &color_attachment_ref;
        }

        VkRenderPassCreateInfo render_pass_create_info;
        {
            memset(&render_pass_create_info, 0, sizeof(render_pass_create_info));
            render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            render_pass_create_info.attachmentCount = 1;
            render_pass_create_info.pAttachments = &color_attachment_desc;
            render_pass_create_info.subpassCount = 1;
            render_pass_create_info.pSubpasses = &subpass_desc;
            
            // adding a subpass dependency to acquire the image at the top of the pipeline:
            // see: https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Rendering_and_presentation#page_Submitting-the-command-buffer
            VkSubpassDependency dependency; {
                memset(&dependency, 0, sizeof(dependency));
                dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
                dependency.dstSubpass = 0;
                dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                dependency.srcAccessMask = 0;
                dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            }
            render_pass_create_info.dependencyCount = 1;
            render_pass_create_info.pDependencies = &dependency;

            VkResult render_pass_ok = vkCreateRenderPass(
                renderer->vk_device,
                &render_pass_create_info,
                NULL,
                &renderer->vk_render_pass
            );
            if (render_pass_ok != VK_SUCCESS) {
                printf("[Wololo] Failed to create a Vulkan Render Pass\n");
                goto fatal_error;
            } else {
                printf("[Wololo] Vulkan render pass created successfully.\n");
                renderer->vk_render_pass_ok = true;
            }
        }
    }

    // creating a descriptor set for the UBO:
    // see:
    // https://vulkan-tutorial.com/Uniform_buffers/Descriptor_layout_and_buffer
    VkDescriptorSetLayoutBinding fubo_descriptor_set_layout_binding;
    {
        memset(&fubo_descriptor_set_layout_binding, 0, sizeof(fubo_descriptor_set_layout_binding));
        fubo_descriptor_set_layout_binding.binding = 0;
        fubo_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        fubo_descriptor_set_layout_binding.descriptorCount = 1;
    
        fubo_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        fubo_descriptor_set_layout_binding.pImmutableSamplers = NULL;
    }

    // creating the descriptor set layouts:
    VkDescriptorSetLayoutCreateInfo layout_info;
    {
        memset(&layout_info, 0, sizeof(layout_info));

        renderer->vk_descriptor_set_layout_ok = false;

        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &fubo_descriptor_set_layout_binding;

        VkResult ok = vkCreateDescriptorSetLayout(
            renderer->vk_device,
            &layout_info,
            NULL,
            &renderer->vk_descriptor_set_layout
        );
        if (ok != VK_SUCCESS) {
            printf(
                "[Wololo] Failed to create Vulkan descriptor set layout "
                "(for fragment uniform).\n"
            );
            goto fatal_error;
        } else {
            printf(
                "[Wololo] Successfully created a Vulkan descriptor set "
                "layout (for fragment uniform).\n"
            );
            renderer->vk_descriptor_set_layout_ok = true;
        }
    }

    // 
    // Creating + Initializing the Graphics Pipeline:
    //

    // Rather than opt for multiple shaders, at this point, we just load a single
    // ubershader that will act as a fixed-function GPU client.

    // creating the graphics pipeline:
    {
        // loading shaders:
        renderer->vk_shaders_loaded_ok = false;
        renderer->vk_vert_shader_module = vk_load_shader_module(renderer, WO_UBERSHADER_VERT_FILEPATH);
        renderer->vk_frag_shader_module = vk_load_shader_module(renderer, WO_UBERSHADER_FRAG_FILEPATH);
        if (renderer->vk_vert_shader_module == VK_NULL_HANDLE) {
            printf("[Wololo] Failed to load Vulkan vertex uber-shader.\n");
            goto fatal_error;
        } else if (renderer->vk_frag_shader_module == VK_NULL_HANDLE) {
            printf("[Wololo] Failed to load Vulkan fragment uber-shader.\n");
            goto fatal_error;
        } else {
            renderer->vk_shaders_loaded_ok = true;
        }

        VkPipelineShaderStageCreateInfo vert_shader_stage_info;
        memset(&vert_shader_stage_info, 0, sizeof(vert_shader_stage_info));
        vert_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_shader_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vert_shader_stage_info.module = renderer->vk_vert_shader_module;
        vert_shader_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo frag_shader_stage_info;
        memset(&frag_shader_stage_info, 0, sizeof(frag_shader_stage_info));
        frag_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_shader_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        frag_shader_stage_info.module = renderer->vk_frag_shader_module;
        frag_shader_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo shader_stages[2] = {
            vert_shader_stage_info,
            frag_shader_stage_info
        };

        // loading vertex shader data:
        // (currently no data to load)
        VkPipelineVertexInputStateCreateInfo vertex_input_info; {
            memset(&vertex_input_info, 0, sizeof(vertex_input_info));
            vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            
            vertex_input_info.vertexBindingDescriptionCount = 0;
            vertex_input_info.pVertexBindingDescriptions = NULL;
            
            vertex_input_info.vertexAttributeDescriptionCount = 0;
            vertex_input_info.pVertexAttributeDescriptions = NULL;
        }

        // specifying the input assembly, incl.
        // - pipeline 'topology': what geometric primitives to draw
        VkPipelineInputAssemblyStateCreateInfo input_assembly; {
            memset(&input_assembly, 0, sizeof(input_assembly));
            input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            input_assembly.primitiveRestartEnable = VK_FALSE;
        }

        // specifying the viewport size (the whole monitor * DPI):
        {
            renderer->vk_viewport.x = 0.0f;
            renderer->vk_viewport.y = 0.0f;
            renderer->vk_viewport.width = (float)renderer->vk_frame_extent.width;
            renderer->vk_viewport.height = (float)renderer->vk_frame_extent.height;
            renderer->vk_viewport.minDepth = 0.0f;
            renderer->vk_viewport.maxDepth = 1.0f;
        }

        // specifying the scissor rectangle: anything outside the scissor is 
        // discarded by the rasterizer:
        VkRect2D scissor; {
            scissor.offset.x = 0;
            scissor.offset.y = 0;
            scissor.extent = renderer->vk_frame_extent;
        }

        // tying it all together, setting up the viewport(s):
        // just one
        VkPipelineViewportStateCreateInfo viewport_state_create_info; {
            memset(&viewport_state_create_info, 0, sizeof(viewport_state_create_info));
            viewport_state_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport_state_create_info.viewportCount = 1;
            viewport_state_create_info.pViewports = &renderer->vk_viewport;
            viewport_state_create_info.scissorCount = 1;
            viewport_state_create_info.pScissors = &scissor;
        }

        // setting up the rasterizer:
        VkPipelineRasterizationStateCreateInfo rasterizer_create_info; {
            memset(&rasterizer_create_info, 0, sizeof(rasterizer_create_info));
            rasterizer_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer_create_info.depthClampEnable = VK_FALSE;
            rasterizer_create_info.rasterizerDiscardEnable = VK_FALSE;
            rasterizer_create_info.polygonMode = VK_POLYGON_MODE_FILL;
            // note: using any fill mode other than fill requires specifying lineWidth;
            // filled anyway for safety:
            rasterizer_create_info.lineWidth = 1.0f;
            // setting cull mode:
            rasterizer_create_info.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterizer_create_info.frontFace = VK_FRONT_FACE_CLOCKWISE;
            // disabling depth bias,
            rasterizer_create_info.depthBiasEnable = VK_FALSE;
            // optional properties to config depthBias:
            rasterizer_create_info.depthBiasConstantFactor = 0.0f;
            rasterizer_create_info.depthBiasClamp = 0.0f;
            rasterizer_create_info.depthBiasSlopeFactor = 0.0f;
        }

        // setting up multisampling (disabled)
        VkPipelineMultisampleStateCreateInfo multisampling_create_info; {
            memset(&multisampling_create_info, 0, sizeof(multisampling_create_info));
            multisampling_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling_create_info.sampleShadingEnable = VK_FALSE;
            multisampling_create_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            multisampling_create_info.minSampleShading = 1.0f;
            multisampling_create_info.pSampleMask = NULL;
            multisampling_create_info.alphaToCoverageEnable = VK_FALSE;
            multisampling_create_info.alphaToOneEnable = VK_FALSE;
        }

        // disabling depth testing; will ignore
        // VkPipelineDepthStencilStateCreateInfo

        // color blending: no alpha
        // https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Fixed_functions#page_Color-blending
        VkPipelineColorBlendAttachmentState color_blend_attachment; {
            color_blend_attachment.colorWriteMask = (
                VK_COLOR_COMPONENT_R_BIT |
                VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT |
                VK_COLOR_COMPONENT_A_BIT
            );
            color_blend_attachment.blendEnable = VK_FALSE;
            color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }

        VkPipelineColorBlendStateCreateInfo color_blending_create_info; {
            memset(&color_blending_create_info, 0, sizeof(color_blending_create_info));
            color_blending_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            color_blending_create_info.logicOpEnable = VK_FALSE;
            color_blending_create_info.logicOp = VK_LOGIC_OP_COPY;
            color_blending_create_info.attachmentCount = 1;
            color_blending_create_info.pAttachments = &color_blend_attachment;
            color_blending_create_info.blendConstants[0] = 0.0f;
            color_blending_create_info.blendConstants[1] = 0.0f;
            color_blending_create_info.blendConstants[2] = 0.0f;
            color_blending_create_info.blendConstants[3] = 0.0f;
        }

        // in order to change the above properties, we must specify which states are dynamic:
        uint32_t dynamic_state_count = 1;
        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT
        };
        VkPipelineDynamicStateCreateInfo dynamic_state_create_info; {
            memset(&dynamic_state_create_info, 0, sizeof(dynamic_state_create_info));
            dynamic_state_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic_state_create_info.dynamicStateCount = dynamic_state_count;
            dynamic_state_create_info.pDynamicStates = dynamic_states;
        }

        // setting up pipeline layout (for uniforms)
        VkPipelineLayoutCreateInfo pipeline_layout_create_info; {
            memset(&pipeline_layout_create_info, 0, sizeof(pipeline_layout_create_info));
            pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeline_layout_create_info.setLayoutCount = 1;
            pipeline_layout_create_info.pSetLayouts = &renderer->vk_descriptor_set_layout;
            pipeline_layout_create_info.pushConstantRangeCount = 0;
            pipeline_layout_create_info.pPushConstantRanges = NULL;
        }
        VkResult pipeline_create_ok = vkCreatePipelineLayout(
            renderer->vk_device,
            &pipeline_layout_create_info,
            NULL,
            &renderer->vk_pipeline_layout
        );
        if (pipeline_create_ok != VK_SUCCESS) {
            printf("[Wololo] Failed to create Vulkan pipeline layout.\n");
            renderer->vk_pipeline_layout_created_ok = false;
            goto fatal_error;
        } else {
            renderer->vk_pipeline_layout_created_ok = true;
        }

        // finally, creating the graphics pipeline:
        VkGraphicsPipelineCreateInfo pipeline_create_info; {
            memset(&pipeline_create_info, 0, sizeof(pipeline_create_info));
            pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipeline_create_info.stageCount = 2;
            pipeline_create_info.pStages = shader_stages;
            pipeline_create_info.pVertexInputState = &vertex_input_info;
            pipeline_create_info.pInputAssemblyState = &input_assembly;
            pipeline_create_info.pViewportState = &viewport_state_create_info;
            pipeline_create_info.pRasterizationState = &rasterizer_create_info;
            pipeline_create_info.pMultisampleState = &multisampling_create_info;
            pipeline_create_info.pDepthStencilState = NULL;
            pipeline_create_info.pColorBlendState = &color_blending_create_info;
            pipeline_create_info.pDynamicState = &dynamic_state_create_info;
            pipeline_create_info.layout = renderer->vk_pipeline_layout;
            pipeline_create_info.renderPass = renderer->vk_render_pass;
            pipeline_create_info.subpass = 0;
            pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
            pipeline_create_info.basePipelineIndex = -1;
            pipeline_create_info.pTessellationState = NULL;
        }

        VkResult graphics_pipeline_ok = vkCreateGraphicsPipelines(
            renderer->vk_device, VK_NULL_HANDLE, 
            1, &pipeline_create_info,
            NULL,
            &renderer->vk_graphics_pipeline
        );
        if (graphics_pipeline_ok != VK_SUCCESS) {
            printf("[Wololo] Failed to create a Vulkan graphics pipeline.\n");
            renderer->vk_graphics_pipeline_ok = false;
            goto fatal_error;
        } else {
            printf("[Wololo] Vulkan graphics pipeline created successfully.\n");
            renderer->vk_graphics_pipeline_ok = true;
        }
    }

    //
    // Preparing for drawing:
    //

    // creating swapchain framebuffers:
    // see: https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Framebuffers
    {
        renderer->vk_swapchain_framebuffers = calloc(
            renderer->vk_swapchain_images_count,
            sizeof(VkFramebuffer)
        );
        for (uint32_t i = 0; i < renderer->vk_swapchain_images_count; i++) {
            VkFramebuffer* fbp = &renderer->vk_swapchain_framebuffers[i];
            
            uint32_t attachment_count = 1;
            VkImageView attachments[] = {
                renderer->vk_swapchain_image_views[i]
            };
            
            VkFramebufferCreateInfo fb_create_info;
            memset(&fb_create_info, 0, sizeof(fb_create_info));
            fb_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_create_info.renderPass = renderer->vk_render_pass;
            fb_create_info.attachmentCount = attachment_count;
            fb_create_info.pAttachments = attachments;
            fb_create_info.width = renderer->vk_frame_extent.width;
            fb_create_info.height = renderer->vk_frame_extent.height;
            fb_create_info.layers = 1;

            VkResult swapchain_framebuffer_res = vkCreateFramebuffer(
                renderer->vk_device, 
                &fb_create_info, 
                NULL, 
                fbp
            );
            if (swapchain_framebuffer_res != VK_SUCCESS) {
                printf("[Wololo] Failed to create Vulkan framebuffer %d/%d\n", i+1, renderer->vk_swapchain_images_count);
            } else {
                printf("[Wololo] Vulkan framebuffer %d/%d created successfully.\n", i+1, renderer->vk_swapchain_images_count);
                renderer->vk_swapchain_fbs_ok_count = i;
            }
        }
    }

    // creating command buffer pool:
    {
        renderer->vk_command_buffer_pool_ok = false;

        VkCommandPoolCreateInfo pool_info;
        memset(&pool_info, 0, sizeof(VkCommandPoolCreateInfo));
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = renderer->vk_graphics_queue_family_index;

        VkResult ok = vkCreateCommandPool(
            renderer->vk_device,
            &pool_info,
            NULL, &renderer->vk_command_buffer_pool
        );
        if (ok != VK_SUCCESS) {
            printf("[Wololo] Failed to create Vulkan Command-Pool for the graphics queue.\n");
            goto fatal_error;
        } else {
            printf("[Wololo] Vulkan Command-Pool for the graphics queue created successfully.\n");
            renderer->vk_command_buffer_pool_ok = true;
        }
    }

    // creating command buffers (managed by the command buffer pool)
    {
        renderer->vk_command_buffers_ok = false;
        renderer->vk_command_buffers = calloc(
            sizeof(VkCommandBuffer),
            renderer->vk_swapchain_images_count
        );
        assert(renderer->vk_command_buffers != NULL);
        
        VkCommandBufferAllocateInfo alloc_info;
        memset(&alloc_info, 0, sizeof(VkCommandBufferAllocateInfo));
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = renderer->vk_command_buffer_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = renderer->vk_swapchain_images_count;

        VkResult ok = vkAllocateCommandBuffers(
            renderer->vk_device,
            &alloc_info,
            renderer->vk_command_buffers
        );
        if (ok != VK_SUCCESS) {
            printf("[Wololo] Failed to allocate Vulkan command buffers.\n");
            goto fatal_error;
        } else {
            renderer->vk_command_buffers_ok = true;
            printf("[Wololo] Vulkan command buffers allocated successfully.\n");
        }
    }

    // Initializing buffer objects, like:
    // - uniform buffer objects
    {
        assert(renderer->vk_swapchain_images_count > 0);

        renderer->uniform_buffers = calloc(
            renderer->vk_swapchain_images_count,
            sizeof(VkBuffer)
        );
        renderer->uniform_buffers_memory = calloc(
            renderer->vk_swapchain_images_count,
            sizeof(VkDeviceMemory)
        );
        assert(renderer->uniform_buffers && renderer->uniform_buffers_memory);

        uint32_t buf_count = renderer->vk_swapchain_images_count;
        for (uint32_t i = 0; i < buf_count; i++) {
            new_vk_uniform_buffer(
                renderer->vk_physical_device,
                renderer->vk_device,
                sizeof(FragmentUniformBufferObject),
                &renderer->uniform_buffers[i],
                &renderer->uniform_buffers_memory[i]
            );
        }
    }

    // initializing a descriptor pool to bind uniforms to shader:
    // https://vulkan-tutorial.com/Uniform_buffers/Descriptor_pool_and_sets
    {
        renderer->vk_descriptor_pool_ok = false;

        // configuring maximum descriptor pool size:
        VkDescriptorPoolSize pool_size; {
            memset(&pool_size, 0, sizeof(pool_size));
            pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            pool_size.descriptorCount = renderer->vk_swapchain_images_count;
        }

        // and the maximum number of pools:
        VkDescriptorPoolCreateInfo pool_info; {
            memset(&pool_info, 0, sizeof(pool_info));
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &pool_size;
            pool_info.maxSets = renderer->vk_swapchain_images_count;
            pool_info.flags = 0;
        }

        VkResult descriptor_pool_ok = vkCreateDescriptorPool(
            renderer->vk_device,
            &pool_info,
            NULL,
            &renderer->vk_descriptor_pool
        );
        if (descriptor_pool_ok != VK_SUCCESS) {
            printf("[Wololo] Failed to create Vulkan descriptor pool for Uniform data.\n");
            goto fatal_error;
        } else {
            printf("[Wololo] Vulkan descriptor pool for Uniform data created successfully.\n");
            renderer->vk_descriptor_pool_ok = true;
        }
    }
    // Creating descriptor sets using the descriptor pool above^
    // see:
    // https://vulkan-tutorial.com/Uniform_buffers/Descriptor_pool_and_sets
    {
        VkDescriptorSetLayout* layouts = calloc(
            renderer->vk_swapchain_images_count,
            sizeof(VkDescriptorSetLayout)
        );
        for (uint32_t i = 0; i < renderer->vk_swapchain_images_count; i++) {
            layouts[i] = renderer->vk_descriptor_set_layout;
        }
        renderer->vk_descriptor_sets = calloc(
            renderer->vk_swapchain_images_count,
            sizeof(VkDescriptorSet)
        );

        VkDescriptorSetAllocateInfo alloc_info; {
            memset(&alloc_info, 0, sizeof(alloc_info));
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = renderer->vk_descriptor_pool;
            alloc_info.descriptorSetCount = renderer->vk_swapchain_images_count;
            alloc_info.pSetLayouts = layouts;
            alloc_info.pNext = NULL;
        }
        VkResult desc_sets_ok = vkAllocateDescriptorSets(
            renderer->vk_device,
            &alloc_info,
            renderer->vk_descriptor_sets
        );

        if (desc_sets_ok != VK_SUCCESS) {
            printf("[Wololo] Failed to create Vulkan descriptor sets for Uniform data.\n");
            goto fatal_error;
        }

        // if allocation was successful, configuring each descriptor:
        for (uint32_t i = 0; i < renderer->vk_swapchain_images_count; i++) {
            VkDescriptorBufferInfo buffer_info; {
                memset(&buffer_info, 0, sizeof(buffer_info));
                buffer_info.buffer = renderer->uniform_buffers[i];
                buffer_info.offset = 0;
                buffer_info.range = sizeof(FragmentUniformBufferObject);
            }
            VkWriteDescriptorSet w_desc; {
                memset(&w_desc, 0, sizeof(w_desc));

                w_desc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w_desc.dstSet = renderer->vk_descriptor_sets[i];
                w_desc.dstBinding = 0;
                w_desc.dstArrayElement = 0;
                w_desc.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w_desc.descriptorCount = 1;

                w_desc.pBufferInfo = &buffer_info;
                w_desc.pImageInfo = NULL;
                w_desc.pTexelBufferView = NULL;
            }

            vkUpdateDescriptorSets(
                renderer->vk_device,
                1, &w_desc,
                0, NULL
            );
        }

        printf("Vulkan descriptor sets for Uniform data created successfully.\n");
    }

    // Recording the render command buffer (that can be replayed per-frame):
    // (will eventually just plaster a quad to the screen)
    // https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Command_buffers#page_Starting-command-buffer-recording
    {
        // recall there is one command buffer per swapchain image.
        for (uint32_t i = 0; i < renderer->vk_swapchain_images_count; i++) {
            VkCommandBufferBeginInfo begin_info;
            memset(&begin_info, 0, sizeof(begin_info));
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = 0;
            begin_info.pInheritanceInfo = NULL;

            VkResult command_buffer_ok = vkBeginCommandBuffer(
                renderer->vk_command_buffers[i],
                &begin_info
            );
            if (command_buffer_ok != VK_SUCCESS) {
                printf("[Wololo] Failed to begin recording Vulkan command buffer %u\n", i+1);
                goto fatal_error;
            } else {
                printf("[Wololo] Vulkan command buffer %u now recording...\n", i+1);
            }

            // beginning the render pass:
            VkRenderPassBeginInfo render_pass_info;
            {
                memset(&render_pass_info, 0, sizeof(render_pass_info));
                render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                render_pass_info.renderPass = renderer->vk_render_pass;
                render_pass_info.framebuffer = renderer->vk_swapchain_framebuffers[i];
                render_pass_info.renderArea.offset.x = 0;
                render_pass_info.renderArea.offset.y = 0;
                render_pass_info.renderArea.extent = renderer->vk_frame_extent;

                // setting the clear color:
                VkClearValue clear_color;
                if (WO_DEBUG) {
                    // offensive magenta, 100% opacity
                    clear_color.color = (VkClearColorValue) {1.0f, 0.0f, 1.0f, 1.0f};
                } else {
                    // black, 100% opacity
                    clear_color.color = (VkClearColorValue) {0.0f, 0.0f, 0.0f, 1.0f};
                }
                render_pass_info.clearValueCount = 1;
                render_pass_info.pClearValues = &clear_color;
            }
            vkCmdBeginRenderPass(
                renderer->vk_command_buffers[i],
                &render_pass_info,
                VK_SUBPASS_CONTENTS_INLINE
            );
            
            // drawing:
            {
                vkCmdBindPipeline(
                    renderer->vk_command_buffers[i],
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->vk_graphics_pipeline
                );
                vkCmdSetViewport(
                    renderer->vk_command_buffers[i],
                    0, 1,
                    &renderer->vk_viewport
                );
                
                vkCmdBindDescriptorSets(
                    renderer->vk_command_buffers[i],
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->vk_pipeline_layout,
                    0, 
                    1, &renderer->vk_descriptor_sets[i],
                    0, NULL
                );
                vkCmdDraw(
                    renderer->vk_command_buffers[i],
                    6,  // vertex count: 3 x 2 for 2 tris
                    1,  // instance count: 1 means we're not using it.
                    0,  // first vertex
                    0   // first instance
                );
                vkCmdEndRenderPass(
                    renderer->vk_command_buffers[i]
                );
            }

            // ending the render pass:
            if (vkEndCommandBuffer(renderer->vk_command_buffers[i]) != VK_SUCCESS) {
                printf(
                    "[Wololo] Recording render pass to Vulkan command buffer %u/%u failed.\n", 
                    i+1,
                    renderer->vk_swapchain_images_count
                );
                goto fatal_error;
            } else {
                printf(
                    "[Wololo] Vulkan command buffer %u/%u successfully recorded with render pass.\n", 
                    i+1,
                    renderer->vk_swapchain_images_count
                );
            }
        }
    }

    // Initializing renderer sync objects, like:
    // - semaphores (GPU-GPU sync): one per frame in flight
    // - fences (CPU-GPU sync): one per frame in flight
    {
        // see: 
        // https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Rendering_and_presentation#page_Frames-in-flight
        
        // allocating images_inflight_fences, initializing to VK_NULL_HANDLE (0):
        renderer->vk_images_inflight_fences = calloc(
            sizeof(VkFence),
            renderer->vk_swapchain_images_count
        );

        // creating semaphores and fences:
        VkSemaphoreCreateInfo semaphore_info;
        memset(&semaphore_info, 0, sizeof(semaphore_info));
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        VkFenceCreateInfo fence_info;
        memset(&fence_info, 0, sizeof(fence_info));
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        // for each possible frame in flight...
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            renderer->vk_semaphores_oks[i] = false;

            VkResult sem1res = vkCreateSemaphore(
                renderer->vk_device, &semaphore_info, NULL, 
                &renderer->vk_image_available_semaphores[i]
            );
            VkResult sem2res = vkCreateSemaphore(
                renderer->vk_device, &semaphore_info, NULL,
                &renderer->vk_render_finished_semaphores[i]
            );
            VkResult fence_res = vkCreateFence(
                renderer->vk_device, &fence_info, NULL,
                &renderer->vk_inflight_fences[i]
            );

            if (sem1res != VK_SUCCESS || sem2res != VK_SUCCESS || fence_res != VK_SUCCESS) {
                printf(
                    "[Wololo] Failed to create Vulkan synchronization objects for frame %d/%d.\n", 
                    i+1,
                    MAX_FRAMES_IN_FLIGHT
                );
                goto fatal_error;
            } else {
                printf(
                    "[Wololo] Vulkan synchronization objects for frame %d/%d created successfully.\n", 
                    i+1,
                    MAX_FRAMES_IN_FLIGHT
                );
                renderer->vk_semaphores_oks[i] = true;
            }
        }
    }

  // should never jump to this label, just flow into naturally.    
  _success:
    // reporting success, returning:
    printf("[Wololo] Successfully initialized Vulkan backend.\n");
    return renderer;

  fatal_error:
    printf("[Wololo] A fatal error occurred while initializing Vulkan.\n");
    del_renderer(renderer);
    return NULL;
}
VkShaderModule vk_load_shader_module(Wo_Renderer* renderer, char const* file_path) {
    // loading all bytecode:
    char* buffer;
    size_t code_size;
    {
        // attempting to open the shader:
        FILE* shader_file = fopen(file_path, "rb");
        assert(shader_file && "Failed to open a shader.");

        // creating a buffer based on the maximum file size:
        fseek(shader_file, 0, SEEK_END);
        size_t max_buffer_size = ftell(shader_file);
        buffer = calloc(max_buffer_size, 1);

        // (resetting the cursor back to the beginning of the file stream)
        fseek(shader_file, 0, SEEK_SET);

        // reading all the file's content into the buffer:
        size_t out_index = 0;
        while (!feof(shader_file) && !ferror(shader_file)) {
            size_t increment = fread(
                &buffer[out_index],
                1, 1,
                shader_file
            );
            if (increment <= 0) {
                break;
            } else {
                out_index += increment;
            }
        }
        code_size = out_index;
        
        // note that SPIR-V is a binary format, so it does not need a null-terminating
        // byte.
    }
    
    VkShaderModule shader_module;
    {
        VkShaderModuleCreateInfo create_info;
        memset(&create_info, 0, sizeof(create_info));
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = code_size;
        create_info.pCode = (uint32_t const*)buffer;

        VkResult shader_ok = vkCreateShaderModule(
            renderer->vk_device,
            &create_info,
            NULL,
            &shader_module
        );
        if (shader_ok != VK_SUCCESS) {
            printf("[Wololo][Vulkan] Failed to create Vulkan shader module '%s'.\n", file_path);
            shader_module = VK_NULL_HANDLE;
        } else {
            printf("[Wololo] Vulkan shader '%s' created successfully.\n", file_path);
        }
    }

    // all done:
    free(buffer);
    return shader_module;
}
void del_renderer(Wo_Renderer* renderer) {
    if (renderer != NULL) {
        // Waiting for the device to idle:
        if (renderer->vk_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(renderer->vk_device);
        }

        // destroying semaphores required in 'draw_frame_from_renderer':
        // see:
        // https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Rendering_and_presentation
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (renderer->vk_semaphores_oks[i]) {
                vkDestroySemaphore(renderer->vk_device, renderer->vk_render_finished_semaphores[i], NULL);
                vkDestroySemaphore(renderer->vk_device, renderer->vk_image_available_semaphores[i], NULL);
                vkDestroyFence(renderer->vk_device, renderer->vk_inflight_fences[i], NULL);
            }
        }
    
        // destroying the command pool used to address the graphics queue:
        // see:
        // https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Command_buffers
        {
            if (renderer->vk_command_buffer_pool_ok) {
                vkDestroyCommandPool(
                    renderer->vk_device,
                    renderer->vk_command_buffer_pool,
                    NULL
                );
                renderer->vk_command_buffer_pool_ok = false;

                free(renderer->vk_command_buffers);
                renderer->vk_command_buffers = NULL;
            }
        }

        // destroying the framebuffers:
        // see:
        // https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Framebuffers
        {
            if (renderer->vk_swapchain_framebuffers) {
                for (uint32_t i_fb = 0; i_fb < renderer->vk_swapchain_fbs_ok_count; i_fb++) {
                    vkDestroyFramebuffer(
                        renderer->vk_device, 
                        renderer->vk_swapchain_framebuffers[i_fb],
                        NULL
                    );
                }
                free(renderer->vk_swapchain_framebuffers);
                renderer->vk_swapchain_framebuffers = NULL;
            }
        }

        // destroying the pipeline object:
        // see:
        // https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Conclusion
        if (renderer->vk_graphics_pipeline_ok) {
            vkDestroyPipeline(
                renderer->vk_device,
                renderer->vk_graphics_pipeline,
                NULL
            );
            renderer->vk_graphics_pipeline_ok = false;
        }

        // destroying the pipeline layout:
        // see:
        // https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Fixed_functions#page_Dynamic-state
        if (renderer->vk_pipeline_layout_created_ok) {
            vkDestroyPipelineLayout(
                renderer->vk_device,
                renderer->vk_pipeline_layout,
                NULL
            );
            renderer->vk_pipeline_layout_created_ok = false;
        }

        // destroying the render pass:
        // see:
        // https://vulkan-tutorial.com/Drawing_a_triangle/Graphics_pipeline_basics/Render_passes
        if (renderer->vk_render_pass_ok) {
            vkDestroyRenderPass(
                renderer->vk_device,
                renderer->vk_render_pass,
                NULL
            );
            renderer->vk_render_pass_ok = false;
        }

        // destroying the shader modules:
        // see:
        // https://vulkan-tutorial.com/Drawing_a_triangle/Graphics_pipeline_basics/Shader_modules
        if (renderer->vk_shaders_loaded_ok) {
            vkDestroyShaderModule(
                renderer->vk_device,
                renderer->vk_vert_shader_module,
                NULL
            );
            vkDestroyShaderModule(
                renderer->vk_device,
                renderer->vk_frag_shader_module,
                NULL
            );
            renderer->vk_shaders_loaded_ok = false;
        }

        // destroying the swapchain image views, then the swapchain:
        if (renderer->vk_descriptor_set_layout) {
            vkDestroyDescriptorSetLayout(
                renderer->vk_device,
                renderer->vk_descriptor_set_layout,
                NULL
            );
        }
        if (renderer->uniform_buffers) {
            assert(renderer->uniform_buffers_memory);
            for (uint32_t i = 0; i < renderer->vk_swapchain_images_count; i++) {
                vkDestroyBuffer(
                    renderer->vk_device,
                    renderer->uniform_buffers[i],
                    NULL
                );
                vkFreeMemory(
                    renderer->vk_device,
                    renderer->uniform_buffers_memory[i],
                    NULL
                );
            }
            free(renderer->uniform_buffers);
            free(renderer->uniform_buffers_memory);
        }
        if (renderer->vk_descriptor_pool_ok) {
            vkDestroyDescriptorPool(
                renderer->vk_device,
                renderer->vk_descriptor_pool,
                NULL
            );
            renderer->vk_descriptor_pool_ok = false;
        }
        if (renderer->vk_swapchain_image_views != NULL) {
            for (size_t index = 0; index < renderer->vk_swapchain_images_count; index++) {
                vkDestroyImageView(
                    renderer->vk_device,
                    renderer->vk_swapchain_image_views[index],
                    NULL
                );
            }
            free(renderer->vk_swapchain_image_views);
        }
        if (renderer->vk_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(renderer->vk_device, renderer->vk_swapchain, NULL);
            free(renderer->vk_swapchain_images);
            renderer->vk_swapchain_images_count = 0;
        }
        
        // destroying the Vulkan logical device:
        if (renderer->vk_device != VK_NULL_HANDLE) {
            printf("[Wololo] Destroying Vulkan (logical) device\n");
            vkDestroyDevice(renderer->vk_device, NULL);
        }
        
        // destroying the Vulkan present surface:
        if (renderer->vk_present_surface != NULL) {
            printf("[Wololo] Destroying Vulkan window present surface\n");
            vkDestroySurfaceKHR(renderer->vk_instance, renderer->vk_present_surface, NULL);
        }

        // destroying the Vulkan physical device, de-allocating buffers:
        if (renderer->vk_physical_devices != NULL) {
            free(renderer->vk_physical_devices);
            renderer->vk_physical_devices = NULL;
        }
        if (renderer->vk_available_validation_layers != NULL) {
            free(renderer->vk_available_validation_layers);
            renderer->vk_available_validation_layers = NULL;
        }
        if (renderer->vk_enabled_validation_layer_names) {
            free((void*)renderer->vk_enabled_validation_layer_names);
            renderer->vk_enabled_validation_layer_names = NULL;
        }
        if (renderer->vk_queue_family_properties != NULL) {
            free(renderer->vk_queue_family_properties);
            renderer->vk_queue_family_properties = NULL;
        }
        if (renderer->vk_available_device_extensions != NULL) {
            free(renderer->vk_available_device_extensions);
            renderer->vk_available_device_extensions = NULL;
        }
        if (renderer->vk_enabled_device_extension_names) {
            free((void*)renderer->vk_enabled_device_extension_names);
            renderer->vk_enabled_device_extension_names = NULL;
        }
        if (renderer->vk_available_present_modes != NULL) {
            free(renderer->vk_available_present_modes);
            renderer->vk_available_present_modes = NULL;
        }
        if (renderer->vk_available_formats != NULL) {
            free(renderer->vk_available_formats);
            renderer->vk_available_formats = NULL;
        }

        // destroying the Vulkan instance:
        if (renderer->vk_instance != VK_NULL_HANDLE) {
            printf("[Wololo] Destroying Vulkan instance\n");
            vkDestroyInstance(renderer->vk_instance, NULL);
        }

        // finally, destroying the renderer struct:
        printf("[Wololo] Destroying renderer \"%s\"\n", renderer->name);
        free(renderer);
    }
}
void draw_frame_with_renderer(Wo_Renderer* renderer) {
    // this function will perform 3 operations:
    // - acquire an image from the swapchain
    // - execute the command buffer with that image as attachment in the framebuffer
    // - return the image to the swap chain for the presentation
    // each operation is synchronized using semaphores.
    // - see: https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Rendering_and_presentation

    // first, waiting for CPU lock, then reseting the fence (for the next frame index)
    vkWaitForFences(
        renderer->vk_device,
        1, &renderer->vk_inflight_fences[renderer->current_frame_index],
        VK_TRUE, UINT64_MAX
    );
    
    // When using multiple swapchain images, we need to acquire the index of a swapchain image
    // that is currently not being read from by the GPU, and is therefore writable.
    // 'renderer->current_frame_index' stripes through each
    // NOTE: updated at end of func.
    uint32_t image_index = -1;
    vkAcquireNextImageKHR(
        renderer->vk_device, 
        renderer->vk_swapchain, 
        UINT64_MAX,
        renderer->vk_image_available_semaphores[renderer->current_frame_index],
        VK_NULL_HANDLE,
        &image_index
    );

    // check if a previous frame is using this image,
    // i.e. we must wait for its fence:
    if (renderer->vk_images_inflight_fences[image_index] != VK_NULL_HANDLE) {
        vkWaitForFences(
            renderer->vk_device,
            1, &renderer->vk_images_inflight_fences[image_index],
            VK_TRUE, UINT64_MAX
        );
    }
    
    // mark the images_inflight_fences frame as 'in-use'
    // by assigning the inflight-fence to the image-fence:
    renderer->vk_images_inflight_fences[image_index] = (
        renderer->vk_inflight_fences[renderer->current_frame_index]
    );
    
    // using the acquired image_index, setting up synchronous chain of ops:

    // updating Fragment Uniform Buffer Object:
    {
        FragmentUniformBufferObject fubo;
        memset(&fubo, 0, sizeof(fubo));
        fubo.time_since_start_sec = glfwGetTime();
        fubo.resolution_x = (float)renderer->vk_frame_extent.width;
        fubo.resolution_y = (float)renderer->vk_frame_extent.height;
    
        void* data = NULL;
        VkResult map_ok = vkMapMemory(
            renderer->vk_device,
            renderer->uniform_buffers_memory[image_index],
            0,
            sizeof(fubo),
            0,
            &data
        );
        assert(map_ok == VK_SUCCESS && "Failed to map Vulkan UBO.");
        memcpy(data, &fubo, sizeof(fubo));
        vkUnmapMemory(
            renderer->vk_device,
            renderer->uniform_buffers_memory[image_index]
        );
    }

    // after drawing ops, 'submit' to the semaphore:
    // "Queue submission and synchronization is configured through parameters in the "
    // "VkSubmitInfo structure"
    // see: https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Rendering_and_presentation#page_Submitting-the-command-buffer
    VkSubmitInfo submit_info;
    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore wait_semaphores[] = {
        renderer->vk_image_available_semaphores[renderer->current_frame_index]
    };
    VkPipelineStageFlags wait_stages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
    };
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;

    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &renderer->vk_command_buffers[image_index];

    VkSemaphore signal_semaphores[] = {
        renderer->vk_render_finished_semaphores[renderer->current_frame_index]
    };
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    // it is best to reset the fence in use before using it (i.e. submitting the queue):
    vkResetFences(
        renderer->vk_device,
        1, &renderer->vk_inflight_fences[renderer->current_frame_index]
    );
    VkResult submit_ok = vkQueueSubmit(
        renderer->vk_graphics_queue, 
        1, &submit_info, 
        renderer->vk_inflight_fences[renderer->current_frame_index]
    );
    if (submit_ok != VK_SUCCESS) {
        printf("Failed to submit draw command buffer (image %u/%u)\n", image_index+1, renderer->vk_swapchain_images_count);
        fflush(stdout);
        assert(0 && "Failed to submit draw command buffer");
    }

    // setting up the second pass: present
    VkPresentInfoKHR present_info;
    memset(&present_info, 0, sizeof(present_info));
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &renderer->vk_swapchain;
    present_info.pImageIndices = &image_index;
    present_info.pResults = NULL;
    vkQueuePresentKHR(renderer->vk_present_queue, &present_info);

    // sleeping while the submitted command buffer is being processed:
    vkQueueWaitIdle(renderer->vk_present_queue);

    // updating the current frame index:
    renderer->current_frame_index = (
        (renderer->current_frame_index + 1) %
        MAX_FRAMES_IN_FLIGHT
    );
}
bool allocate_node(Wo_Renderer* renderer, Wo_Node* out_node) {
    if (renderer->current_node_count == renderer->max_node_count) {
        return false;
    } else {
        *out_node = (Wo_Node)renderer->current_node_count++;
        return true;
    }
}
void set_nonroot_node(Wo_Renderer* renderer, Wo_Node node) {
    renderer->node_is_nonroot_bitset[node/64] |= ((uint64_t)1) << (node%64);
}

Wo_Node add_sphere_node(Wo_Renderer* renderer, Wo_Scalar radius) {
    Wo_Node node;
    assert(allocate_node(renderer, &node) && "[Wololo] Failed to allocate a new sphere renderer node-- out of memory.");
    renderer->node_type_table[node] = WO_LEAF_SPHERE;
    renderer->node_info_table[node].sphere.radius = radius;
    return node;
}
Wo_Node add_infinite_planar_partition_node(Wo_Renderer* renderer, Wo_Vec3 outward_facing_normal) {
    Wo_Node node;
    assert(allocate_node(renderer, &node) && "[Wololo] Failed to allocate a new infinite planar partition renderer node-- out of memory.");
    renderer->node_type_table[node] = WO_LEAF_INFINITE_PLANAR_PARTITION;
    renderer->node_info_table[node].infinite_planar_partition.normal = outward_facing_normal;
    return node;
}
Wo_Node add_union_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right) {
    Wo_Node node;
    assert(allocate_node(renderer, &node) && "[Wololo] Failed to allocate a new union-of renderer node-- out of memory.");
    renderer->node_type_table[node] = WO_NODE_BINOP_UNION_OF;
    renderer->node_info_table[node].binop_of.left = left;
    renderer->node_info_table[node].binop_of.right = right;
    set_nonroot_node(renderer, left.node);
    set_nonroot_node(renderer, right.node);
    return node;
}
Wo_Node add_intersection_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right) {
    Wo_Node node;
    assert(allocate_node(renderer, &node) && "[Wololo] Failed to allocate a new intersection-of renderer node-- out of memory.");
    renderer->node_type_table[node] = WO_NODE_BINOP_INTERSECTION_OF;
    renderer->node_info_table[node].binop_of.left = left;
    renderer->node_info_table[node].binop_of.right = right;
    set_nonroot_node(renderer, left.node);
    set_nonroot_node(renderer, right.node);
    return node;
}
Wo_Node add_difference_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right) {
    Wo_Node node;
    assert(allocate_node(renderer, &node) && "[Wololo] Failed to allocate a new union renderer node-- out of memory.");
    renderer->node_type_table[node] = WO_NODE_BINOP_DIFFERENCE_OF;
    renderer->node_info_table[node].binop_of.left = left;
    renderer->node_info_table[node].binop_of.right = right;
    set_nonroot_node(renderer, left.node);
    set_nonroot_node(renderer, right.node);
    return node;
}

//
//
// Interface:
//
//

Wo_Renderer* wo_renderer_new(Wo_App* app, char const* name, size_t max_renderer_count) {
    return new_renderer(app, name, max_renderer_count);
}
void wo_renderer_del(Wo_Renderer* renderer) {
    del_renderer(renderer);
}
void wo_renderer_draw_frame(Wo_Renderer* renderer) {
    draw_frame_with_renderer(renderer);
}

Wo_Node wo_renderer_add_sphere_node(Wo_Renderer* renderer, Wo_Scalar radius) {
    return add_sphere_node(renderer, radius);
}
Wo_Node wo_renderer_add_infinite_planar_partition_node(Wo_Renderer* renderer, Wo_Vec3 outward_facing_normal) {
    return add_infinite_planar_partition_node(renderer, outward_facing_normal);
}
Wo_Node wo_renderer_add_union_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right) {
    return add_union_of_node(renderer, left, right);
}
Wo_Node wo_renderer_add_intersection_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right) {
    return add_intersection_of_node(renderer, left, right);
}
Wo_Node wo_renderer_add_difference_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right) {
    return add_difference_of_node(renderer, left, right);
}

bool wo_renderer_isroot(Wo_Renderer* renderer, Wo_Node node) {
    uint64_t cmp_word = renderer->node_is_nonroot_bitset[node/64];
    uint64_t is_nonroot = cmp_word & ((size_t)1 << (size_t)(node%64));
    return !is_nonroot;
}
