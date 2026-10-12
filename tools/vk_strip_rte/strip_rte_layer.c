// Vulkan layer: strip RoundingModeRTE from every SPIR-V shader module.
//
// NVIDIA (615.71.09, 615.78.08) faults the GPU (Xid 31, GPCCLIENT_GCC,
// FAULT_PDE @ 0x3_F8000000) on some Xenos-translated vertex shaders that
// declare `OpExecutionMode RoundingModeRTE 32`. RTE is the hardware's default
// fp32 rounding anyway, so dropping the declaration is behaviour-neutral.
// Lets unmodified Xenia Canary (or a stock ReXGlue plugin) run on this driver.
//
// Build: tools/vk_strip_rte/build.sh   Use: tools/vk_strip_rte/run.sh <command...>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define LAYER_NAME "VK_LAYER_GEARS1_strip_rte"
#define EXPORT __attribute__((visibility("default")))

enum { kOpExecutionMode = 16, kOpCapability = 17, kModeRTE = 4462, kCapRTE = 4467 };

typedef struct {
  void* key;  // dispatch key (first pointer of the dispatchable handle)
  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkGetDeviceProcAddr gdpa;
  PFN_vkCreateShaderModule create_shader_module;
  PFN_vkDestroyInstance destroy_instance;
  PFN_vkDestroyDevice destroy_device;
} Entry;

static Entry g_entries[64];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_stripped;

static void* key_of(const void* handle) { return *(void* const*)handle; }

static Entry* find(void* key) {
  for (int i = 0; i < 64; ++i)
    if (g_entries[i].key == key) return &g_entries[i];
  return NULL;
}

static Entry* add(void* key) {
  pthread_mutex_lock(&g_lock);
  Entry* e = find(key);
  if (!e) e = find(NULL);
  if (e) { memset(e, 0, sizeof(*e)); e->key = key; }
  pthread_mutex_unlock(&g_lock);
  return e;
}

static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                          VkInstance* out) {
  VkLayerInstanceCreateInfo* link = (VkLayerInstanceCreateInfo*)ci->pNext;
  while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
    link = (VkLayerInstanceCreateInfo*)link->pNext;
  if (!link) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  PFN_vkCreateInstance next = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkResult r = next(ci, alloc, out);
  if (r != VK_SUCCESS) return r;
  Entry* e = add(key_of(*out));
  if (e) {
    e->gipa = gipa;
    e->destroy_instance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
  }
  return r;
}

static void VKAPI_CALL DestroyInstance(VkInstance inst, const VkAllocationCallbacks* alloc) {
  Entry* e = find(key_of(inst));
  PFN_vkDestroyInstance next = e ? e->destroy_instance : NULL;
  if (e) e->key = NULL;
  if (next) next(inst, alloc);
}

static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                        const VkAllocationCallbacks* alloc, VkDevice* out) {
  VkLayerDeviceCreateInfo* link = (VkLayerDeviceCreateInfo*)ci->pNext;
  while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
    link = (VkLayerDeviceCreateInfo*)link->pNext;
  if (!link) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  PFN_vkCreateDevice next = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
  VkResult r = next(pd, ci, alloc, out);
  if (r != VK_SUCCESS) return r;
  Entry* e = add(key_of(*out));
  if (e) {
    e->gdpa = gdpa;
    e->create_shader_module = (PFN_vkCreateShaderModule)gdpa(*out, "vkCreateShaderModule");
    e->destroy_device = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
  }
  return r;
}

static void VKAPI_CALL DestroyDevice(VkDevice dev, const VkAllocationCallbacks* alloc) {
  Entry* e = find(key_of(dev));
  PFN_vkDestroyDevice next = e ? e->destroy_device : NULL;
  if (e) e->key = NULL;
  if (next) next(dev, alloc);
}

static VkResult VKAPI_CALL CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci,
                                              const VkAllocationCallbacks* alloc, VkShaderModule* out) {
  Entry* e = find(key_of(dev));
  const uint32_t* in = ci->pCode;
  size_t n = ci->codeSize / 4;
  uint32_t* buf = (n > 5 && in[0] == 0x07230203) ? malloc(ci->codeSize) : NULL;
  if (!buf) return e->create_shader_module(dev, ci, alloc, out);
  size_t o = 5, removed = 0;
  memcpy(buf, in, 5 * 4);
  for (size_t i = 5; i < n;) {
    uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!words || i + words > n) { free(buf); return e->create_shader_module(dev, ci, alloc, out); }
    int drop = (op == kOpCapability && words >= 2 && in[i + 1] == kCapRTE) ||
               (op == kOpExecutionMode && words >= 3 && in[i + 2] == kModeRTE);
    if (drop) removed++;
    else { memcpy(buf + o, in + i, words * 4); o += words; }
    i += words;
  }
  VkShaderModuleCreateInfo patched = *ci;
  patched.pCode = buf;
  patched.codeSize = o * 4;
  if (removed) __atomic_add_fetch(&g_stripped, 1, __ATOMIC_RELAXED);
  VkResult r = e->create_shader_module(dev, removed ? &patched : ci, alloc, out);
  free(buf);
  return r;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL strip_rte_GetDeviceProcAddr(VkDevice dev, const char* name) {
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)strip_rte_GetDeviceProcAddr;
  if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)CreateShaderModule;
  if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
  Entry* e = find(key_of(dev));
  return e ? e->gdpa(dev, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL strip_rte_GetInstanceProcAddr(VkInstance inst, const char* name) {
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)strip_rte_GetInstanceProcAddr;
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
  if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)DestroyInstance;
  if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)CreateDevice;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)strip_rte_GetDeviceProcAddr;
  if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)CreateShaderModule;
  if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
  if (!inst) return NULL;
  Entry* e = find(key_of(inst));
  return e ? e->gipa(inst, name) : NULL;
}

__attribute__((destructor)) static void report(void) {
  const char* v = getenv("STRIP_RTE_QUIET");
  if (!v || !*v) {
    char msg[96];
    int len = snprintf(msg, sizeof msg, "[strip_rte] shader modules with RoundingModeRTE stripped: %lu\n", g_stripped);
    if (len > 0) { ssize_t w = write(2, msg, (size_t)len); (void)w; }
  }
}
