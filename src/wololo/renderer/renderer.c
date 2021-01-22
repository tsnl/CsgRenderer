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
#include <vulkan/vulkan.h>

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
    "VK_KHR_swapchain",
    // required to draw to a window.
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
    size_t max_node_count;
    size_t current_node_count;
    NodeType* node_type_table;
    NodeInfo* node_info_table;
    uint64_t* node_is_nonroot_bitset;
    char* name;

    VkInstance vk_instance;
    VkPhysicalDevice vk_physical_device;
    uint32_t vk_physical_device_count;
    VkPhysicalDevice* vk_physical_devices;
    
    bool vk_validation_layers_enabled;
    VkLayerProperties* vk_available_validation_layers;
    uint32_t vk_enabled_validation_layer_count;
    char const** vk_enabled_validation_layer_names;
    
    uint32_t vk_queue_family_count;
    VkQueueFamilyProperties* vk_queue_family_properties;
    
    uint32_t vk_graphics_queue_family_index;
    VkQueue vk_graphics_queue;

    VkDevice vk_device;
    uint32_t vk_available_device_extension_count;
    VkExtensionProperties* vk_available_device_extensions;
    uint32_t vk_enabled_device_extension_count;
    char const** vk_enabled_device_extension_names;
};


Wo_Renderer* new_renderer(char const* name, size_t max_node_count);
Wo_Renderer* allocate_renderer(char const* name, size_t max_node_count);
Wo_Renderer* vk_init_renderer(Wo_Renderer* renderer);
void del_renderer(Wo_Renderer* renderer);
bool allocate_node(Wo_Renderer* renderer, Wo_Node* out_node);
void set_nonroot_node(Wo_Renderer* renderer, Wo_Node node);

//
// Implementation:
//

Wo_Renderer* new_renderer(char const* name, size_t max_node_count) {
    // allocating all the memory we need:
    Wo_Renderer* renderer = allocate_renderer(name, max_node_count);
    renderer = vk_init_renderer(renderer);
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
Wo_Renderer* vk_init_renderer(Wo_Renderer* renderer) {

    // setting ambient state variables:
    renderer->vk_validation_layers_enabled = true;

    // we know all pointers are NULL-initialized, so if non-NULL, we know there's an error.

    // creating a Vulkan instance:
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
            if (renderer->vk_validation_layers_enabled) {
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
    
    // checking the device's queue properties:
    // https://vulkan-tutorial.com/en/Drawing_a_triangle/Setup/Physical_devices_and_queue_families
    {   
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(renderer->vk_physical_device, &queue_family_count, NULL);
        renderer->vk_queue_family_count = queue_family_count;
        renderer->vk_graphics_queue_family_index = renderer->vk_queue_family_count;
        
        renderer->vk_queue_family_properties = malloc(queue_family_count * sizeof(VkQueueFamilyProperties));
        vkGetPhysicalDeviceQueueFamilyProperties(renderer->vk_physical_device, &queue_family_count, renderer->vk_queue_family_properties);

        for (uint32_t index = 0; index < renderer->vk_queue_family_count; index++) {
            VkQueueFamilyProperties family_properties = renderer->vk_queue_family_properties[index];
            if (family_properties.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                renderer->vk_graphics_queue_family_index = index;
            }
            
            bool indices_are_complete = (
                (renderer->vk_graphics_queue_family_index != renderer->vk_queue_family_count) &&
                (true)
            );
            if (indices_are_complete) {
                break;
            }
        }
        if (renderer->vk_graphics_queue_family_index == renderer->vk_queue_family_count) {
            printf("[Wololo] No queue family with VK_QUEUE_GRAPHICS_BIT was found.\n");
            goto fatal_error;
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

        // loading extensions (like the Swap-chain extension):
        // https://vulkan-tutorial.com/Drawing_a_triangle/Presentation/Swap_chain
        {
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

            bool const debug_print_all_available_exts = false;
            if (debug_print_all_available_exts) {
                uint32_t ext_count = renderer->vk_available_device_extension_count;
                for (uint32_t i = 0; i < ext_count; i++) {
                    printf("\t[Vulkan] Extension supported: \"%s\" [%d/%d]\n", 
                        renderer->vk_available_device_extensions[i].extensionName,
                        i+1, ext_count
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
                    printf("[Wololo] Found Vulkan device extension \"%s\"\n", ext_name);
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
        }

        // setting up validation layers:
        if (renderer->vk_validation_layers_enabled) {
            create_info.enabledLayerCount = DEFAULT_VK_VALIDATION_LAYER_COUNT;
            create_info.ppEnabledLayerNames = default_vk_validation_layer_names;
        } else {
            create_info.enabledLayerCount = 0;
        }

        // at last, creating `renderer->vk_device`:
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
    }

    return renderer;

  fatal_error:
    del_renderer(renderer);
    return NULL;
}
void del_renderer(Wo_Renderer* renderer) {
    if (renderer != NULL) {
        if (renderer->vk_device != VK_NULL_HANDLE) {
            vkDestroyDevice(renderer->vk_device, NULL);
        }
        if (renderer->vk_instance != VK_NULL_HANDLE) {
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

Wo_Renderer* wo_renderer_new(char const* name, size_t max_renderer_count) {
    return new_renderer(name, max_renderer_count);
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
