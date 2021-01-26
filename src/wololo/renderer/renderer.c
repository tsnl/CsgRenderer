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

static uint32_t const DEFAULT_VK_VALIDATION_LAYER_COUNT = 1;
static char const* default_vk_validation_layer_names[DEFAULT_VK_VALIDATION_LAYER_COUNT] = {
    "VK_LAYER_KHRONOS_validation"
};

// minimum extensions are required for the app to run at all.
static uint32_t const MINIMUM_VK_DEVICE_EXTENSION_COUNT = 1;
static char const* minimum_vk_device_extension_names[MINIMUM_VK_DEVICE_EXTENSION_COUNT] = {
    "VK_KHR_swapchain"
};

// optional extensions are loaded if supported.
static uint32_t const OPTIONAL_VK_DEVICE_EXTENSION_COUNT = 0;
static char const* optional_vk_device_extension_names[OPTIONAL_VK_DEVICE_EXTENSION_COUNT] = {
    // "VK_KHR_portability_subset",
    // https://www.moltengl.com/docs/readme/moltenvk-readme-user-guide.html
    // https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VK_KHR_portability_subset.html
    // according to the Vulkan spec, if the `VK_KHR_portability_subset` extension is included [...],
    // then ppEnabledExtensions must include [it].
    // This allows the implementation to work on MoltenVK, among other platforms that support the 
    // Vulkan 1.0 Portability subset.
    // 'VK_KHR_get_physical_device_properties2' is required.
};

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

    // App reference:
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

    // the swapchain:
    VkSwapchainKHR vk_swapchain;
    uint32_t vk_swapchain_images_count;
    VkImage* vk_swapchain_images;
    VkImageView* vk_swapchain_image_views;

    // shader modules:
    bool vk_shaders_loaded_ok;
    VkShaderModule vk_vert_shader_module;
    VkShaderModule vk_frag_shader_module;

    // pipeline layout (for uniforms):
    bool vk_pipeline_layout_created_ok;
    VkPipelineLayout vk_pipeline_layout;
};


Wo_Renderer* new_renderer(Wo_App* app, char const* name, size_t max_node_count);
Wo_Renderer* allocate_renderer(char const* name, size_t max_node_count);
Wo_Renderer* vk_init_renderer(Wo_App* app, Wo_Renderer* renderer);
VkShaderModule vk_load_shader_module(char const* file_path);

void del_renderer(Wo_Renderer* renderer);
bool allocate_node(Wo_Renderer* renderer, Wo_Node* out_node);
void set_nonroot_node(Wo_Renderer* renderer, Wo_Node node);

//
// Implementation:
//

Wo_Renderer* new_renderer(Wo_App* app, char const* name, size_t max_node_count) {
    // allocating all the memory we need:
    Wo_Renderer* renderer = allocate_renderer(name, max_node_count);
    renderer = vk_init_renderer(app, renderer);
    return renderer;
}
Wo_Renderer* allocate_renderer(char const* name, size_t max_node_count) {
    size_t subslab0_renderer_size_in_bytes = sizeof(Wo_Renderer);
    size_t subslab1_type_table_size_in_bytes = sizeof(NodeType) * max_node_count;
    size_t subslab2_info_table_size_in_bytes = sizeof(NodeInfo) * max_node_count;
    size_t subslab3_root_bitset_size_in_bytes = ((max_node_count/64 + 1)*64) / 8;
    size_t subslab4_name_size_in_bytes = 0; {
        int name_length = strlen(name);
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
    renderer->are_vk_validation_layers_enabled = true;

    // we know all pointers are NULL-initialized, so if non-NULL, we know there's an error.

    // checking that GLFW supports Vulkan:
    assert(glfwVulkanSupported() == GLFW_TRUE && "GLFW should support Vulkan.");

    // creating a Vulkan instance, applying validation layers:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Instance
    {
        VkApplicationInfo app_info = {}; {
            app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app_info.pApplicationName = renderer->name;
            app_info.applicationVersion = VK_MAKE_VERSION(0, 0, 0);
            app_info.pEngineName = "Wololo Csg Renderer";
            app_info.engineVersion = VK_MAKE_VERSION(0, 0, 0);
            app_info.apiVersion = VK_API_VERSION_1_0;
        }

        VkInstanceCreateInfo create_info = {0}; 
        {
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
                renderer->vk_available_validation_layers = malloc(
                    available_validation_layer_count * sizeof(VkLayerProperties)
                );
                vkEnumerateInstanceLayerProperties(
                    &available_validation_layer_count, 
                    renderer->vk_available_validation_layers
                );
                
                // tallying each available validation layer:
                renderer->vk_enabled_validation_layer_count = 0;
                renderer->vk_enabled_validation_layer_names = malloc(
                    DEFAULT_VK_VALIDATION_LAYER_COUNT * sizeof(char const*)
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

                // setting properties:
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

    // selecting a physical device:
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
        renderer->vk_physical_devices = malloc(
            renderer->vk_physical_device_count * 
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
    
    // Creating a software surface for the window:
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

    // checking the device's queue properties:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Physical_devices_and_queue_families
    {   
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(renderer->vk_physical_device, &queue_family_count, NULL);
        renderer->vk_queue_family_count = queue_family_count;
        renderer->vk_graphics_queue_family_index = renderer->vk_queue_family_count;
        renderer->vk_present_queue_family_index = renderer->vk_queue_family_count;
        
        renderer->vk_queue_family_properties = malloc(queue_family_count * sizeof(VkQueueFamilyProperties));
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

    // setting up a logical device to interface with the physical device
    // using the queue indices:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Logical_device_and_queues
    {
        VkDeviceQueueCreateInfo queue_create_info = {};
        queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = renderer->vk_graphics_queue_family_index;
        queue_create_info.queueCount = 1;
        float queue_priority = 1.0f;
        queue_create_info.pQueuePriorities = &queue_priority;

        // don't need any special device features:
        VkPhysicalDeviceFeatures device_features = {};

        // creating the instance...
        VkDeviceCreateInfo create_info = {};
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
        renderer->vk_available_device_extensions = malloc(
            sizeof(VkExtensionProperties) *
            renderer->vk_available_device_extension_count
        );
        vkEnumerateDeviceExtensionProperties(
            renderer->vk_physical_device, NULL,
            &renderer->vk_available_device_extension_count,
            renderer->vk_available_device_extensions
        );

        bool const debug_print_all_available_exts = true;
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
            OPTIONAL_VK_DEVICE_EXTENSION_COUNT
        );
        renderer->vk_enabled_device_extension_count = 0;
        renderer->vk_enabled_device_extension_names = malloc(max_extension_count * sizeof(char const*));
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
        for (uint32_t opt_ext_index = 0; opt_ext_index < OPTIONAL_VK_DEVICE_EXTENSION_COUNT; opt_ext_index++) {
            char const* ext_name = optional_vk_device_extension_names[opt_ext_index];
            
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
                printf("[Wololo] Found [optional] Vulkan device extension \"%s\"\n", ext_name);
                uint32_t index = renderer->vk_enabled_device_extension_count++;
                renderer->vk_enabled_device_extension_names[index] = ext_name;
            } else {
                // do nothing; optional extension.
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

    // Retrieving queue handles:
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

    // Creating the swapchain:
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
                renderer->vk_available_formats = malloc(
                    sizeof(VkSurfaceFormatKHR) * 
                    renderer->vk_available_format_count
                );
                assert(renderer->vk_available_formats != NULL && "Out of memory-- malloc failed.");
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
                renderer->vk_available_present_modes = malloc(
                    sizeof(VkPresentModeKHR) *
                    renderer->vk_available_present_mode_count
                );
                assert(renderer->vk_available_present_modes != NULL && "Out of memory-- malloc failed.");
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

        // Choosing format mode:
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
        }

        // Finally creating the chain:
        {
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

        // Acquiring and storing the created swapchain images:
        {
            // getting `renderer->vk_swapchain_images_count`
            vkGetSwapchainImagesKHR(
                renderer->vk_device,
                renderer->vk_swapchain,
                &renderer->vk_swapchain_images_count,
                NULL
            );

            // allocating:
            renderer->vk_swapchain_images = malloc(
                sizeof(VkImage) *
                renderer->vk_swapchain_images_count
            );

            // storing:
            vkGetSwapchainImagesKHR(
                renderer->vk_device,
                renderer->vk_swapchain,
                &renderer->vk_swapchain_images_count,
                renderer->vk_swapchain_images
            );
        }
    }

    // creating image views (VkImageView):
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Presentation/Image_views
    // to use any VkImage, need VkImageView
    {
        // first, allocating the vk_swapchain_image_views array
        // s.t. there exists a unique image-view per image
        renderer->vk_swapchain_image_views = NULL;
        if (renderer->vk_swapchain_images_count > 0) {
            renderer->vk_swapchain_image_views = malloc(
                sizeof(VkImageView) *
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
            
            // creating the VkImageView:
            VkResult ok = vkCreateImageView(
                renderer->vk_device, 
                &create_info, 
                NULL, 
                &renderer->vk_swapchain_image_views[index]
            );
        }
    }

    // Rather than opt for multiple shaders, at this point, we just load a single
    // ubershader that will act as a fixed-function GPU client.

    // creating the shader module:
    {
        renderer->vk_vert_shader_module = vk_load_shader_module(WO_UBERSHADER_VERT_FILEPATH);
        renderer->vk_frag_shader_module = vk_load_shader_module(WO_UBERSHADER_FRAG_FILEPATH);
        renderer->vk_shaders_loaded_ok = true;
    }

    // todo: create render pass
    {

        //
        //
        //
        //
        //
        // TODO: IMPLEMENT ME!
        // https://vulkan-tutorial.com/Drawing_a_triangle/Graphics_pipeline_basics/Render_passes
        //
        //
        //
        //
        //
        //

    }

    // creating the graphics pipeline:
    {
        VkPipelineShaderStageCreateInfo vert_shader_stage_info;
        vert_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_shader_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vert_shader_stage_info.module = renderer->vk_vert_shader_module;
        vert_shader_stage_info.pName = "main";
        
        VkPipelineShaderStageCreateInfo frag_shader_stage_info;
        vert_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_shader_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        vert_shader_stage_info.module = renderer->vk_frag_shader_module;
        vert_shader_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo shader_stages[] = {
            vert_shader_stage_info,
            frag_shader_stage_info
        };

        // loading vertex shader data:
        // (currently no data to load)
        // (not entirely sure how this is different from uniforms, but it gets baked aot)
        VkPipelineVertexInputStateCreateInfo vertex_input_info;
        vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_info.vertexBindingDescriptionCount = 0;
        vertex_input_info.pVertexBindingDescriptions = NULL;
        vertex_input_info.vertexAttributeDescriptionCount = 0;
        vertex_input_info.pVertexAttributeDescriptions = NULL;

        // specifying the input assembly, incl.
        // - pipeline 'topology': what geometric primitives to draw
        VkPipelineInputAssemblyStateCreateInfo input_assembly;
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        // specifying the viewport size (the whole monitor * DPI):
        VkViewport viewport;
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)renderer->vk_frame_extent.width;
        viewport.height = (float)renderer->vk_frame_extent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        // specifying the scissor rectangle: anything outside the scissor is 
        // discarded by the rasterizer:
        VkRect2D scissor;
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent = renderer->vk_frame_extent;

        // tying it all together, setting up the viewport(s):
        // just one
        VkPipelineViewportStateCreateInfo viewport_state_create_info;
        viewport_state_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = &viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = &scissor;

        // setting up the rasterizer:
        VkPipelineRasterizationStateCreateInfo rasterizer_create_info;
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

        // setting up multisampling (disabled)
        VkPipelineMultisampleStateCreateInfo multisampling_create_info;
        multisampling_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling_create_info.sampleShadingEnable = VK_FALSE;
        multisampling_create_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling_create_info.minSampleShading = 1.0f;
        multisampling_create_info.pSampleMask = NULL;
        multisampling_create_info.alphaToCoverageEnable = VK_FALSE;
        multisampling_create_info.alphaToOneEnable = VK_FALSE;

        // disabling depth testing; will ignore
        // VkPipelineDepthStencilStateCreateInfo

        // color blending: no alpha
        // https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Fixed_functions#page_Color-blending
        VkPipelineColorBlendAttachmentState color_blend_attachment;
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

        VkPipelineColorBlendStateCreateInfo color_blending_create_info;
        color_blending_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending_create_info.logicOpEnable = VK_FALSE;
        color_blending_create_info.logicOp = VK_LOGIC_OP_COPY;
        color_blending_create_info.attachmentCount = 1;
        color_blending_create_info.pAttachments = &color_blend_attachment;
        color_blending_create_info.blendConstants[0] = 0.0f;
        color_blending_create_info.blendConstants[1] = 0.0f;
        color_blending_create_info.blendConstants[2] = 0.0f;
        color_blending_create_info.blendConstants[3] = 0.0f;

        // in order to change the above properties, we must specify which states are dynamic:
        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT
        };
        VkPipelineDynamicStateCreateInfo dynamic_state;
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 1;
        dynamic_state.pDynamicStates = dynamic_states;

        // setting up pipeline layout (for uniforms)
        VkPipelineLayoutCreateInfo pipeline_layout_create_info;
        pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 0;
        pipeline_layout_create_info.pSetLayouts = NULL;
        pipeline_layout_create_info.pushConstantRangeCount = 0;
        pipeline_layout_create_info.pPushConstantRanges = NULL;
        pipeline_layout_create_info.flags = 0;
        pipeline_layout_create_info.pNext = NULL;
        VkResult pipeline_create_ok = vkCreatePipelineLayout(
            renderer->vk_device,
            &pipeline_layout_create_info,
            NULL,
            &renderer->vk_pipeline_layout
        );
        if (pipeline_create_ok != VK_SUCCESS) {
            assert(0 && "Failed to create pipeline layout.");
        } else {
            renderer->vk_pipeline_layout_created_ok = true;
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
VkShaderModule vk_load_shader_module(char const* file_path) {
    // loading all bytecode:
    char* buffer;
    size_t code_size;
    {
        // attempting to open the shader:
        FILE* shader_file = fopen(file_path, "r");
        assert(shader_file && "Failed to open a shader.");

        // creating a buffer based on the maximum file size:
        fseek(shader_file, 0, SEEK_END);
        size_t max_buffer_size = ftell(shader_file);
        buffer = malloc(max_buffer_size);

        // (resetting the cursor back to the beginning of the file stream)
        fseek(shader_file, 0, SEEK_SET);

        // reading all the file's content into the buffer:
        size_t out_index = 0;
        while (!feof(shader_file)) {
            int ch = fgetc(shader_file);
            if (ch <= 0) {
                break;
            } else {
                buffer[out_index++] = ch;
            }
        }
        code_size = out_index;
        
        // note that SPIR-V is a binary format, so it does not need a null-terminating
        // byte.
    }
    
    VkShaderModule shader_module;
    {
        VkShaderModuleCreateInfo create_info;
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = code_size;
        create_info.pCode = (uint32_t const*)buffer;
    }
    
    // all done:
    free(buffer);
    return shader_module;
}
void del_renderer(Wo_Renderer* renderer) {
    if (renderer != NULL) {
        // see:
        // https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Fixed_functions#page_Dynamic-state
        if (renderer->vk_pipeline_layout_created_ok) {
            vkDestroyPipelineLayout(
                renderer->vk_device,
                renderer->vk_pipeline_layout,
                NULL
            );
        }

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
        if (renderer->vk_device != VK_NULL_HANDLE) {
            printf("[Wololo] Destroying Vulkan (logical) device\n");
            vkDestroyDevice(renderer->vk_device, NULL);
        }
        if (renderer->vk_present_surface != NULL) {
            printf("[Wololo] Destroying Vulkan window present surface\n");
            vkDestroySurfaceKHR(renderer->vk_instance, renderer->vk_present_surface, NULL);
        }
        if (renderer->vk_instance != VK_NULL_HANDLE) {
            printf("[Wololo] Destroying Vulkan instance\n");
            vkDestroyInstance(renderer->vk_instance, NULL);
        }
        if (renderer->vk_physical_devices != NULL) {
            free(renderer->vk_physical_devices);
            renderer->vk_physical_devices = NULL;
        }
        if (renderer->vk_available_validation_layers != NULL) {
            free(renderer->vk_available_validation_layers);
            renderer->vk_available_validation_layers = NULL;
        }
        if (renderer->vk_enabled_validation_layer_names != NULL) {
            free(renderer->vk_enabled_validation_layer_names);
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
        if (renderer->vk_enabled_device_extension_names != NULL) {
            free(renderer->vk_enabled_device_extension_names);
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
        printf("[Wololo] Destroying renderer \"%s\"\n", renderer->name);
        free(renderer);
    }
}
bool allocate_node(Wo_Renderer* renderer, Wo_Node* out_node) {
    if (renderer->current_node_count == renderer->max_node_count) {
        return false;
    } else {
        *out_node = renderer->current_node_count++;
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
    uint64_t is_nonroot = cmp_word & (1 << (node%64));
    return !is_nonroot;
}
