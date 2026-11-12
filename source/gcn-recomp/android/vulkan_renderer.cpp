#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include "vulkan_renderer.h"
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include "vk_basic_vs.inc"
#include "vk_basic_fs.inc"

#define VKLOG(...) __android_log_print(ANDROID_LOG_INFO, GCN_LOG_TAG, __VA_ARGS__)
#define VKFAIL(...) __android_log_print(ANDROID_LOG_ERROR, GCN_LOG_TAG, __VA_ARGS__)
#define VKINIT(...) __android_log_print(ANDROID_LOG_INFO, "TANDEM-VK-INIT", __VA_ARGS__)
#define VKDRAW(...) __android_log_print(ANDROID_LOG_INFO, "TANDEM-VK-DRAW", __VA_ARGS__)
#define VKPRESENT(...) __android_log_print(ANDROID_LOG_INFO, "TANDEM-VK-PRESENT", __VA_ARGS__)
#define VKTEV(...) __android_log_print(ANDROID_LOG_INFO, "TANDEM-TEV", __VA_ARGS__)
#define VKEFB(...) __android_log_print(ANDROID_LOG_INFO, "TANDEM-EFB", __VA_ARGS__)
#define VKFRAG(...) __android_log_print(ANDROID_LOG_INFO, "TANDEM-FRAGMENT", __VA_ARGS__)
#define VKPIX(...) __android_log_print(ANDROID_LOG_INFO, "TANDEM-PIXELS", __VA_ARGS__)

namespace gx::vk {
namespace {
struct Buffer { VkBuffer b{}; VkDeviceMemory m{}; void* mapped{}; };
struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkDescriptorSet set{}; VkFormat format{}; VkExtent2D extent{}; VkImageAspectFlags aspect{}; };
struct State {
    ANativeWindow* window{}; VkInstance instance{}; VkSurfaceKHR surface{}; VkPhysicalDevice physical{};
    VkDevice device{}; VkQueue queue{}; uint32_t queue_family{}; VkCommandPool pool{}; VkCommandBuffer cmd{};
    VkSwapchainKHR swapchain{}; VkFormat format{}; VkExtent2D extent{}; std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views; VkRenderPass pass{}; VkPipelineLayout layout{}; VkPipeline pipeline{};
    VkDescriptorSetLayout set_layout{}; VkDescriptorPool descriptors{}; VkSampler sampler{}; Image white{};
    Buffer vertices{}, indices{}, readback{}; bool readback_supported{}; VkQueryPool occlusion{}; VkFence fence{}; VkSemaphore acquire{}, render_done{}; uint32_t frame{};
    uint32_t frame_image{}; bool frame_active{}; bool frame_clear{};
    std::unordered_map<uint32_t, Image> textures;
    std::unordered_map<uint32_t, Image> efb_textures;
    Image efb_color{}, efb_depth{}, xfb{};
    VkFormat depth_format{VK_FORMAT_D32_SFLOAT};
} g;

bool check(VkResult r, const char* where) { if (r != VK_SUCCESS) { VKFAIL("%s: VkResult=%d", where, (int)r); return false; } return true; }
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(g.physical, &p);
    for (uint32_t i=0;i<p.memoryTypeCount;i++) if ((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&props)==props) return i;
    return UINT32_MAX;
}
Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer out{}; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=size; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(g.device,&bi,nullptr,&out.b),"buffer"); VkMemoryRequirements r{}; vkGetBufferMemoryRequirements(g.device,out.b,&r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=r.size; ai.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(g.device,&ai,nullptr,&out.m),"buffer memory"); check(vkBindBufferMemory(g.device,out.b,out.m,0),"bind buffer"); vkMapMemory(g.device,out.m,0,size,0,&out.mapped); return out;
}
void destroy(Buffer& b) { if(b.m) vkFreeMemory(g.device,b.m,nullptr); if(b.b) vkDestroyBuffer(g.device,b.b,nullptr); b={}; }
void destroy(Image& i) { if(i.set && g.descriptors) vkFreeDescriptorSets(g.device,g.descriptors,1,&i.set); if(i.view) vkDestroyImageView(g.device,i.view,nullptr); if(i.image) vkDestroyImage(g.device,i.image,nullptr); if(i.memory) vkFreeMemory(g.device,i.memory,nullptr); i={}; }
Image make_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image out{}; out.format=format; out.extent=extent; out.aspect=aspect;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=format; ci.extent={extent.width,extent.height,1}; ci.mipLevels=1; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=usage; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(!check(vkCreateImage(g.device,&ci,nullptr,&out.image),"image")) return {};
    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(g.device,out.image,&mr); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=mr.size; ai.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if(ai.memoryTypeIndex==UINT32_MAX) { destroy(out); return {}; }
    if(!check(vkAllocateMemory(g.device,&ai,nullptr,&out.memory),"image memory") || !check(vkBindImageMemory(g.device,out.image,out.memory,0),"bind image")) { destroy(out); return {}; }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=out.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format; vi.subresourceRange={aspect,0,1,0,1}; if(!check(vkCreateImageView(g.device,&vi,nullptr,&out.view),"image view")) { destroy(out); return {}; }
    return out;
}
void descriptor(Image& out) {
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=g.descriptors; da.descriptorSetCount=1; da.pSetLayouts=&g.set_layout; if(!check(vkAllocateDescriptorSets(g.device,&da,&out.set),"image descriptor")) return;
    VkDescriptorImageInfo di{g.sampler,out.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=out.set; w.dstBinding=0; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&di; vkUpdateDescriptorSets(g.device,1,&w,0,nullptr);
}
VkCommandBuffer one_time() { VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=g.pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1; VkCommandBuffer c{}; check(vkAllocateCommandBuffers(g.device,&ai,&c),"cmd alloc"); VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; check(vkBeginCommandBuffer(c,&bi),"cmd begin"); return c; }
void submit_one(VkCommandBuffer c) { check(vkEndCommandBuffer(c),"cmd end"); VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&c; check(vkQueueSubmit(g.queue,1,&si,VK_NULL_HANDLE),"cmd submit"); check(vkQueueWaitIdle(g.queue),"cmd wait"); vkFreeCommandBuffers(g.device,g.pool,1,&c); }
Image upload_texture(const TexData& t) {
    Image out{}; if(t.levels.empty() || t.levels[0].empty() || !t.width || !t.height) return out;
    const VkDeviceSize bytes=VkDeviceSize(t.width)*t.height*4;
    Buffer staging=buffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped,t.levels[0].data(),size_t(bytes));
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={t.width,t.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g.device,&ci,nullptr,&out.image),"texture image");VkMemoryRequirements mr{};vkGetImageMemoryRequirements(g.device,out.image,&mr);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(ai.memoryTypeIndex==UINT32_MAX)ai.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);check(vkAllocateMemory(g.device,&ai,nullptr,&out.memory),"texture memory");check(vkBindImageMemory(g.device,out.image,out.memory,0),"bind texture");
    VkCommandBuffer c=one_time();VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.image=out.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={t.width,t.height,1};vkCmdCopyBufferToImage(c,staging.b,out.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);submit_one(c);destroy(staging);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=out.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};if(!check(vkCreateImageView(g.device,&vi,nullptr,&out.view),"texture view")){destroy(out);return {};}VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=g.descriptors;da.descriptorSetCount=1;da.pSetLayouts=&g.set_layout;const VkResult descriptor_result=vkAllocateDescriptorSets(g.device,&da,&out.set);if(descriptor_result!=VK_SUCCESS){VKFAIL("texture descriptor allocation failed result=%d live=%zu",int(descriptor_result),g.textures.size());destroy(out);return {};}VkDescriptorImageInfo di{g.sampler,out.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=out.set;w.dstBinding=0;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&di;vkUpdateDescriptorSets(g.device,1,&w,0,nullptr);return out;
}
VkShaderModule shader(const uint32_t* code, size_t bytes) { VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=bytes; ci.pCode=code; VkShaderModule m{}; check(vkCreateShaderModule(g.device,&ci,nullptr,&m),"shader"); return m; }
void mat_mul(const float* a,const float* b,float* o) { for(int c=0;c<4;c++)for(int r=0;r<4;r++){o[c*4+r]=0;for(int k=0;k<4;k++)o[c*4+r]+=a[k*4+r]*b[c*4+k];} }
void projection(const PixelState& s,float* out) {
    const float* p=s.proj; float P[16]={}; bool ortho=(int)p[6]!=0;
    if(ortho){P[0]=p[0];P[12]=p[1];P[5]=p[2];P[13]=p[3];P[10]=p[4];P[14]=p[5];P[15]=1;}
    else {P[0]=p[0];P[8]=p[1];P[5]=p[2];P[9]=p[3];P[10]=p[4];P[14]=p[5];P[11]=-1;}
    // GX stores viewport as half-width, half-height, z-span, x-origin, y-origin,
    // far-z.  The Vulkan path used to treat these as x/y/width/height and then
    // flipped Y a second time, putting the 2-D menu outside the clip volume.
    const float* v=s.viewport; const uint32_t bp59=s.bp[0x59];
    const float xoff=float((bp59 & 0x3ffu)*2u), yoff=float(((bp59>>10)&0x3ffu)*2u);
    float V[16]={}; V[0]=2*v[0]/640.f; V[12]=2*(v[3]-xoff)/640.f-1;
    // Match the GX EFB used by the desktop/Dolphin-compatible frontend.  The
    // Android surface is scaled later by the Vulkan viewport; it is not the
    // source coordinate space for GX's 640x528 viewport.
    V[5]=-2*v[1]/528.f; V[13]=1-2*(v[4]-yoff)/528.f;
    // GX carries a 24-bit viewport depth range. Vulkan's depth convention is
    // [0,1], so retain the range instead of collapsing every fragment to 0.5.
    V[10]=v[2]/16777215.f; V[14]=v[5]/16777215.f; V[15]=1;
    mat_mul(V,P,out);
}
bool choose_device() {
    uint32_t n=0; vkEnumeratePhysicalDevices(g.instance,&n,nullptr); if(!n)return false; std::vector<VkPhysicalDevice> ds(n); vkEnumeratePhysicalDevices(g.instance,&n,ds.data());
    for(auto d:ds){uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,nullptr);std::vector<VkQueueFamilyProperties> q(qn);vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,q.data());for(uint32_t i=0;i<qn;i++){VkBool32 present=0;vkGetPhysicalDeviceSurfaceSupportKHR(d,i,g.surface,&present);if((q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)&&present){g.physical=d;g.queue_family=i;return true;}}} return false;
}
bool make_swapchain() {
    VkSurfaceCapabilitiesKHR caps{}; if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical,g.surface,&caps)!=VK_SUCCESS){VKFAIL("swapchain: surface capabilities failed");return false;} uint32_t n=0;if(vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical,g.surface,&n,nullptr)!=VK_SUCCESS || !n){VKFAIL("swapchain: no surface formats");return false;}std::vector<VkSurfaceFormatKHR> fs(n);if(vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical,g.surface,&n,fs.data())!=VK_SUCCESS){VKFAIL("swapchain: format query failed");return false;}
    g.format=fs[0].format; g.extent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{std::max(1u,g.extent.width),std::max(1u,g.extent.height)}:caps.currentExtent;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=g.surface;ci.minImageCount=std::max(caps.minImageCount,2u);ci.imageFormat=g.format;ci.imageColorSpace=fs[0].colorSpace;ci.imageExtent=g.extent;ci.imageArrayLayers=1;g.readback_supported=(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)!=0;ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (g.readback_supported?VK_IMAGE_USAGE_TRANSFER_SRC_BIT:0);ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=caps.currentTransform;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;if(!check(vkCreateSwapchainKHR(g.device,&ci,nullptr,&g.swapchain),"swapchain"))return false;
    vkGetSwapchainImagesKHR(g.device,g.swapchain,&n,nullptr);if(!n)return false;g.swap_images.resize(n);vkGetSwapchainImagesKHR(g.device,g.swapchain,&n,g.swap_images.data());g.swap_views.resize(n);for(uint32_t i=0;i<n;i++){VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=g.swap_images[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=g.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};if(!check(vkCreateImageView(g.device,&vi,nullptr,&g.swap_views[i]),"swap view"))return false;}VKLOG("readback supported=%d",int(g.readback_supported));VKLOG("swapchain ok images=%u extent=%ux%u format=%d",n,g.extent.width,g.extent.height,(int)g.format);return true;
}
} // namespace

void shutdown();
bool init(ANativeWindow* window,uint32_t width,uint32_t height) {
    if(g.instance || g.device || g.surface) { VKLOG("init: replacing existing Vulkan state"); shutdown(); }
    VKINIT("begin window=%p size=%ux%u",window,width,height);g.window=window;g.extent={width,height}; VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Project Tandem";app.apiVersion=VK_API_VERSION_1_0;
    const char* exts[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;ii.enabledExtensionCount=2;ii.ppEnabledExtensionNames=exts;if(!check(vkCreateInstance(&ii,nullptr,&g.instance),"instance")){shutdown();return false;}VKLOG("instance ok");
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};si.window=window;if(!check(vkCreateAndroidSurfaceKHR(g.instance,&si,nullptr,&g.surface),"surface")){shutdown();return false;}VKLOG("surface ok");if(!choose_device()){VKFAIL("no graphics/present device");shutdown();return false;}VKLOG("physical device/queue ok family=%u",g.queue_family);
    float prio=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=g.queue_family;qi.queueCount=1;qi.pQueuePriorities=&prio;VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;const char* dext=VK_KHR_SWAPCHAIN_EXTENSION_NAME;di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&dext;if(!check(vkCreateDevice(g.physical,&di,nullptr,&g.device),"device")){shutdown();return false;}vkGetDeviceQueue(g.device,g.queue_family,0,&g.queue);VKLOG("logical device/queue ok");
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=g.queue_family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;check(vkCreateCommandPool(g.device,&pi,nullptr,&g.pool),"pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=g.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;check(vkAllocateCommandBuffers(g.device,&ca,&g.cmd),"main cmd");
    make_swapchain();
    g.efb_color=make_image(VK_FORMAT_R8G8B8A8_UNORM,{640,528},VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    g.efb_depth=make_image(g.depth_format,{640,528},VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    g.xfb=make_image(VK_FORMAT_R8G8B8A8_UNORM,{608,448},VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    if(!g.efb_color.image || !g.efb_depth.image || !g.xfb.image) { VKFAIL("persistent EFB allocation failed"); shutdown(); return false; }
    { VkCommandBuffer c=one_time(); VkImageMemoryBarrier tr[2]{}; tr[0].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; tr[0].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; tr[0].newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; tr[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; tr[0].image=g.efb_color.image; tr[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; tr[1]=tr[0]; tr[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; tr[1].newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; tr[1].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; tr[1].image=g.efb_depth.image; tr[1].subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}; vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,2,tr); submit_one(c); }
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qpi.queryType=VK_QUERY_TYPE_OCCLUSION;qpi.queryCount=1;check(vkCreateQueryPool(g.device,&qpi,nullptr,&g.occlusion),"occlusion query");
    VkAttachmentDescription ad[2]{}; ad[0].format=g.efb_color.format; ad[0].samples=VK_SAMPLE_COUNT_1_BIT; ad[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; ad[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE; ad[0].initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ad[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ad[1].format=g.depth_format; ad[1].samples=VK_SAMPLE_COUNT_1_BIT; ad[1].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; ad[1].storeOp=VK_ATTACHMENT_STORE_OP_STORE; ad[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE; ad[1].initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; ad[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ar{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}; VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ar;sp.pDepthStencilAttachment=&dr;VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=2;rp.pAttachments=ad;rp.subpassCount=1;rp.pSubpasses=&sp;check(vkCreateRenderPass(g.device,&rp,nullptr,&g.pass),"render pass");
    VkDescriptorSetLayoutBinding lb{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT};VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&lb;check(vkCreateDescriptorSetLayout(g.device,&li,nullptr,&g.set_layout),"set layout");VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,128};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&g.set_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;check(vkCreatePipelineLayout(g.device,&pl,nullptr,&g.layout),"pipeline layout");
    VkShaderModule vs=shader(vk_basic_vs,sizeof(vk_basic_vs)),fsmod=shader(vk_basic_fs,sizeof(vk_basic_fs));VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main"},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fsmod,"main"}};VkVertexInputBindingDescription vb{0,sizeof(GpuVertex),VK_VERTEX_INPUT_RATE_VERTEX};VkVertexInputAttributeDescription va[3]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(GpuVertex,pos)},{1,0,VK_FORMAT_R8G8B8A8_UNORM,offsetof(GpuVertex,col[0])},{2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(GpuVertex,tex[0])}};VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&vb;vi.vertexAttributeDescriptionCount=3;vi.pVertexAttributeDescriptions=va;VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;VkViewport vp{0,0,640,528,0,1};VkRect2D sc{{0,0},{640,528}};VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vps.viewportCount=1;vps.pViewports=&vp;vps.scissorCount=1;vps.pScissors=&sc;VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.lineWidth=1;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;VkPipelineColorBlendAttachmentState cb{};cb.blendEnable=VK_FALSE;cb.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;cb.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;cb.colorBlendOp=VK_BLEND_OP_ADD;cb.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;cb.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;cb.alphaBlendOp=VK_BLEND_OP_ADD;VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cbs.attachmentCount=1;cbs.pAttachments=&cb;VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=stages;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vps;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pDepthStencilState=&ds;gp.pColorBlendState=&cbs;gp.layout=g.layout;gp.renderPass=g.pass;check(vkCreateGraphicsPipelines(g.device,VK_NULL_HANDLE,1,&gp,nullptr,&g.pipeline),"pipeline");vkDestroyShaderModule(g.device,vs,nullptr);vkDestroyShaderModule(g.device,fsmod,nullptr);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16384};VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;dpi.maxSets=16384;dpi.poolSizeCount=1;dpi.pPoolSizes=&ps;check(vkCreateDescriptorPool(g.device,&dpi,nullptr,&g.descriptors),"descriptor pool");VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};samp.magFilter=VK_FILTER_LINEAR;samp.minFilter=VK_FILTER_LINEAR;samp.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;samp.addressModeU=samp.addressModeV=samp.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;samp.maxLod=16;check(vkCreateSampler(g.device,&samp,nullptr,&g.sampler),"sampler");g.vertices=buffer(64*1024*1024,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);g.indices=buffer(16*1024*1024,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);if(g.readback_supported) g.readback=buffer(VkDeviceSize(g.extent.width)*g.extent.height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;check(vkCreateFence(g.device,&fi,nullptr,&g.fence),"fence");VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};check(vkCreateSemaphore(g.device,&sem,nullptr,&g.acquire),"acquire semaphore");check(vkCreateSemaphore(g.device,&sem,nullptr,&g.render_done),"render semaphore");VKLOG("Vulkan GX frontend initialized (%ux%u)",g.extent.width,g.extent.height);return true;
}

bool draw(Batch& b) {
    static unsigned draw_logs=0;
    if(!g.device||b.verts.empty()||b.indices.empty()){if(draw_logs++<8)VKDRAW("rejected device=%p verts=%zu indices=%zu",(void*)g.device,b.verts.size(),b.indices.size());return false;}
    if(draw_logs++<8) VKDRAW("batch=%u verts=%zu packed=%zu indices=%zu cmds=%zu",draw_logs,b.verts.size(),b.packed.size(),b.indices.size(),b.cmds.size());
    if(!g.white.set) { TexData white{}; white.id=0; white.width=1; white.height=1; white.levels.push_back({0xffffffffu}); g.white=upload_texture(white); }
    for(uint32_t id:b.dead_textures) { auto it=g.textures.find(id); if(it!=g.textures.end()) { destroy(it->second); g.textures.erase(it); } }
    for(const auto& t:b.new_textures) if(t && !g.textures.count(t->id)) { Image uploaded=upload_texture(*t); if(uploaded.image && uploaded.set) g.textures.emplace(t->id,std::move(uploaded)); else VKFAIL("texture upload skipped id=%u",t->id); }
    static unsigned texture_batches=0;
    if(texture_batches++<8) __android_log_print(ANDROID_LOG_INFO,"TANDEM-TEX",
        "batch=%u new=%zu cache=%zu states=%zu",texture_batches,b.new_textures.size(),g.textures.size(),b.states.size());
static unsigned diagnostic_batches=0;
if(diagnostic_batches++<5) {
    const auto& v=b.verts.front();
    __android_log_print(ANDROID_LOG_INFO,"TANDEM-VERT",
        "batch=%u verts=%zu indices=%zu cmds=%zu pos=(%.3f,%.3f,%.3f) color=(%u,%u,%u,%u) index0=%u",
        diagnostic_batches,b.verts.size(),b.indices.size(),b.cmds.size(),
        v.pos[0],v.pos[1],v.pos[2],
        unsigned(v.col[0][0]),unsigned(v.col[0][1]),
        unsigned(v.col[0][2]),unsigned(v.col[0][3]),
        unsigned(b.indices.front()));
    for(size_t i=0;i<b.states.size() && i<2;i++) {
        const auto& st=b.states[i];
        __android_log_print(ANDROID_LOG_INFO,"TANDEM-VERT",
            "state=%zu proj=(%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f) viewport=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f)",
            i,st.proj[0],st.proj[1],st.proj[2],st.proj[3],
            st.proj[4],st.proj[5],st.proj[6],
            st.viewport[0],st.viewport[1],st.viewport[2],
            st.viewport[3],st.viewport[4],st.viewport[5]);
    }
}

static unsigned geometry_batches=0;
if(geometry_batches++<8) {
    size_t zeros=0, invalid=0;
    float mn[3]={1e30f,1e30f,1e30f};
    float mx[3]={-1e30f,-1e30f,-1e30f};

    for(const auto& v:b.verts) {
        if(v.pos[0]==0 && v.pos[1]==0 && v.pos[2]==0)
            ++zeros;
        for(int k=0;k<3;k++) {
            if(v.pos[k]<mn[k])mn[k]=v.pos[k];
            if(v.pos[k]>mx[k])mx[k]=v.pos[k];
        }
    }

    for(uint32_t idx:b.indices)
        if(idx>=b.verts.size())++invalid;

    __android_log_print(ANDROID_LOG_INFO,"TANDEM-GEO",
        "batch=%u verts=%zu zeros=%zu invalid=%zu X=[%.2f,%.2f] Y=[%.2f,%.2f] Z=[%.2f,%.2f]",
        geometry_batches,b.verts.size(),zeros,invalid,
        mn[0],mx[0],mn[1],mx[1],mn[2],mx[2]);
}

static unsigned index_batches=0;
if(index_batches++<8) {
    size_t bad=0,draws=0,shifted=0;
    long long minimum=0,maximum=0;
    bool first=true;
    for(const Cmd& c:b.cmds) {
        if(c.type!=CmdType::Draw || c.prim!=0)continue;
        ++draws;
        if(c.base_vertex!=0)++shifted;
        if(c.first>b.indices.size() ||
           c.count>b.indices.size()-c.first) {
            ++bad;
            continue;
        }
        for(size_t j=c.first;j<size_t(c.first)+c.count;j++) {
            long long effective=
                (long long)b.indices[j]+c.base_vertex;
            if(first){minimum=maximum=effective;first=false;}
            if(effective<minimum)minimum=effective;
            if(effective>maximum)maximum=effective;
            if(effective<0 ||
               effective>=(long long)b.verts.size())++bad;
        }
    }
    __android_log_print(ANDROID_LOG_INFO,"TANDEM-IDX",
        "batch=%u verts=%zu packed=%zu draws=%zu shifted=%zu bad=%zu min=%lld max=%lld",
        index_batches,b.verts.size(),b.packed.size(),
        draws,shifted,bad,minimum,maximum);
}
    // `base_vertex` is an index into Batch::packed, whose stride depends on the
    // draw's texture-coordinate count. Vulkan's fixed vertex input cannot consume
    // that buffer as sizeof(GpuVertex). Expand each draw into a common full-stride
    // array and rewrite only its index range; this preserves the GX command order
    // while making the GPU address arithmetic unambiguous.
    std::vector<GpuVertex> gpu_vertices;
    std::vector<uint32_t> gpu_indices=b.indices;
    for(const Cmd& c:b.cmds) {
        if(c.type!=CmdType::Draw || c.state>=b.states.size() || !c.vcount) continue;
        const uint32_t ntex=draw_ntex(b.states[c.state]);
        const uint32_t stride=packed_stride(ntex);
        const size_t src_base=(size_t)(int64_t(c.base_vertex)+c.vfirst)*stride;
        const uint32_t dst_base=(uint32_t)gpu_vertices.size();
        gpu_vertices.resize(gpu_vertices.size()+c.vcount);
        for(uint32_t v=0;v<c.vcount;v++) {
            if(src_base+size_t(v)*stride+std::min<size_t>(stride,sizeof(GpuVertex))<=b.packed.size())
                std::memcpy(&gpu_vertices[dst_base+v],b.packed.data()+src_base+size_t(v)*stride,std::min<size_t>(stride,sizeof(GpuVertex)));
            else if(c.vfirst+v<b.verts.size()) gpu_vertices[dst_base+v]=b.verts[c.vfirst+v];
        }
        if(c.first<=gpu_indices.size() && c.count<=gpu_indices.size()-c.first)
            for(uint32_t j=0;j<c.count;j++) {
                const uint32_t ix=b.indices[c.first+j];
                gpu_indices[c.first+j]=dst_base+(ix>=c.vfirst && ix<c.vfirst+c.vcount ? ix-c.vfirst : 0);
            }
    }
    if(gpu_vertices.empty()) gpu_vertices=b.verts;
    if(gpu_vertices.size()*sizeof(GpuVertex)>64u*1024u*1024u || gpu_indices.size()*sizeof(uint32_t)>16u*1024u*1024u) {
        VKFAIL("batch buffers exceeded: vertices=%zu indices=%zu",gpu_vertices.size(),gpu_indices.size());
        return false;
    }
    bool has_present=false;
    for(const Cmd& c:b.cmds) if(c.type==CmdType::Present) { has_present=true; break; }
    // K107: aguardar a GPU antes de reutilizar buffers compartilhados.
    if(!check(vkWaitForFences(g.device,1,&g.fence,VK_TRUE,UINT64_MAX),"frame fence"))
        return false;
    std::memcpy(g.vertices.mapped,gpu_vertices.data(),gpu_vertices.size()*sizeof(GpuVertex));
    std::memcpy(g.indices.mapped,gpu_indices.data(),gpu_indices.size()*sizeof(uint32_t));
    check(vkResetFences(g.device,1,&g.fence),"reset frame fence");
    uint32_t image=g.frame_image; const bool wait_acquire=!g.frame_active;
    VkResult ar=VK_SUCCESS;
    if(!g.frame_active) {
        ar=vkAcquireNextImageKHR(g.device,g.swapchain,UINT64_MAX,g.acquire,VK_NULL_HANDLE,&image);
        if(ar==VK_ERROR_OUT_OF_DATE_KHR || ar==VK_SUBOPTIMAL_KHR) { VKLOG("swapchain acquire result=%d",(int)ar); }
        if(ar!=VK_SUCCESS && ar!=VK_SUBOPTIMAL_KHR) return false;
        g.frame_image=image; g.frame_active=true; g.frame_clear=true;
        VKDRAW("frame-start image=%u",image);
    }
    check(vkResetCommandBuffer(g.cmd,0),"reset command");VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(g.cmd,&bi),"begin command");vkCmdResetQueryPool(g.cmd,g.occlusion,0,1);vkCmdBeginQuery(g.cmd,g.occlusion,0,0);VkClearValue clear{{{0.02f,0.02f,0.03f,1}}};VkClearValue depth_clear{};depth_clear.depthStencil={1.0f,0};VkFramebuffer fb{};VkImageView attachments[2]={g.efb_color.view,g.efb_depth.view};VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbi.renderPass=g.pass;fbi.attachmentCount=2;fbi.pAttachments=attachments;fbi.width=640;fbi.height=528;fbi.layers=1;check(vkCreateFramebuffer(g.device,&fbi,nullptr,&fb),"framebuffer");VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rbi.renderPass=g.pass;rbi.framebuffer=fb;rbi.renderArea.extent={640,528};rbi.clearValueCount=2;VkClearValue clears[2]={clear,depth_clear};rbi.pClearValues=clears;vkCmdBeginRenderPass(g.cmd,&rbi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(g.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline);VkDeviceSize off=0;vkCmdBindVertexBuffers(g.cmd,0,1,&g.vertices.b,&off);vkCmdBindIndexBuffer(g.cmd,g.indices.b,0,VK_INDEX_TYPE_UINT32);
    if(g.frame_clear) {
        VkClearAttachment ca[2]{}; ca[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; ca[0].colorAttachment=0; ca[0].clearValue=clear; ca[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; ca[1].clearValue.depthStencil.depth=1.0f;
        VkClearRect cr{}; cr.rect={{0,0},g.extent}; cr.baseArrayLayer=0; cr.layerCount=1;
        cr.rect.extent={640,528}; vkCmdClearAttachments(g.cmd,2,ca,1,&cr); g.frame_clear=false;
    }
    static unsigned long frames=0,accepted=0,rejected_type=0,rejected_prim=0,rejected_state=0;
    ++frames;
    for(const Cmd& c:b.cmds){
        if(c.type==CmdType::Present) {
            // Present is emitted immediately before its display EFB copy. The
            // copy itself is handled below; keeping this marker in the stream
            // preserves GX ordering without presenting a half-written frame.
            ++rejected_type; continue;
        }
        if(c.type==CmdType::EfbCopy) {
            static unsigned efb_logs=0;
            if(efb_logs++<12) VKEFB("batch=%u src=%ux%u@%u,%u dst=%ux%u tex=%u xfb=%d depth=%d clear=%d",draw_logs,
                c.copy.src_w,c.copy.src_h,c.copy.src_x,c.copy.src_y,c.copy.dst_w,c.copy.dst_h,c.copy.tex_id,
                c.copy.to_xfb,c.copy.depth,c.copy.clear);
            vkCmdEndRenderPass(g.cmd);
            VkImageMemoryBarrier src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            src.oldLayout=c.copy.depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            src.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            src.srcAccessMask=c.copy.depth?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            src.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            src.image=c.copy.depth?g.efb_depth.image:g.efb_color.image;
            src.subresourceRange={c.copy.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(g.cmd,c.copy.depth?VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&src);
            Image* dst=nullptr;
            if(c.copy.to_xfb) dst=&g.xfb;
            else if(c.copy.tex_id) {
                auto it=g.efb_textures.find(c.copy.tex_id);
                if(it==g.efb_textures.end()) {
                    Image ni=make_image(c.copy.depth?g.depth_format:VK_FORMAT_R8G8B8A8_UNORM,{std::max(1u,c.copy.dst_w),std::max(1u,c.copy.dst_h)},VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,c.copy.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT);
                    if(ni.image) { descriptor(ni); it=g.efb_textures.emplace(c.copy.tex_id,std::move(ni)).first; }
                }
                if(it!=g.efb_textures.end()) dst=&it->second;
            }
            if(dst && dst->image) {
                VkImageMemoryBarrier d{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; d.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; d.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; d.srcAccessMask=0; d.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; d.image=dst->image; d.subresourceRange={dst->aspect,0,1,0,1};
                vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&d);
                VkImageCopy cp{}; cp.srcSubresource={src.subresourceRange.aspectMask,0,0,1}; cp.srcOffset={(int)c.copy.src_x,(int)c.copy.src_y,0}; cp.dstSubresource={dst->aspect,0,0,1}; cp.extent={std::min(c.copy.src_w,dst->extent.width),std::min(c.copy.src_h,dst->extent.height),1}; vkCmdCopyImage(g.cmd,src.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&cp);
                d.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; d.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; d.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; d.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&d);
            }
            src.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; src.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; src.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; src.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; if(c.copy.depth) { src.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; src.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; }
            vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,c.copy.depth?VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&src);
            VkRenderPassBeginInfo again=rbi; again.clearValueCount=2; again.pClearValues=clears; vkCmdBeginRenderPass(g.cmd,&again,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(g.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline); vkCmdBindVertexBuffers(g.cmd,0,1,&g.vertices.b,&off); vkCmdBindIndexBuffer(g.cmd,g.indices.b,0,VK_INDEX_TYPE_UINT32);
            if(c.copy.clear) { VkClearAttachment ca[2]{}; ca[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; ca[0].clearValue.color.float32[0]=((c.copy.clear_rgba>>24)&255)/255.f; ca[0].clearValue.color.float32[1]=((c.copy.clear_rgba>>16)&255)/255.f; ca[0].clearValue.color.float32[2]=((c.copy.clear_rgba>>8)&255)/255.f; ca[0].clearValue.color.float32[3]=(c.copy.clear_rgba&255)/255.f; ca[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; ca[1].clearValue.depthStencil.depth=1.f; VkClearRect cr{}; cr.rect={{0,0},{640,528}}; cr.layerCount=1; vkCmdClearAttachments(g.cmd,2,ca,1,&cr); }
            continue;
        }
        if(c.type!=CmdType::Draw){++rejected_type;continue;}
        if(c.prim!=0){++rejected_prim;continue;}
        if(c.state>=b.states.size()){++rejected_state;continue;}
        ++accepted;
        const PixelState& draw_state=b.states[c.state];
        static unsigned tev_logs=0;
        if(tev_logs++<12) {
            const uint32_t stages=((draw_state.bp[0]>>10)&15u)+1u;
            VKTEV("batch=%u state=%u gen=%08x stages=%u texgens=%u tex0=%u order0=%08x cenv0=%08x aenv0=%08x ksel0=%08x blend=%08x z=%08x",
                draw_logs,c.state,draw_state.bp[0],stages,draw_state.num_texgens,draw_state.tex_id[0],draw_state.bp[0x28],
                draw_state.bp[0xC0],draw_state.bp[0xC1],draw_state.bp[0xF6],draw_state.bp[0x41],draw_state.bp[0x40]);
        }
        uint32_t tex_map=0;
        const uint32_t tev_count=std::min<uint32_t>(((draw_state.bp[0]>>10)&15u)+1u,2u);
        for(uint32_t stage=0;stage<tev_count;stage++) { const uint32_t order=draw_state.bp[0x28+stage/2]>>((stage&1)*12); if(order&0x40u) { tex_map=order&7u; break; } }
        auto draw_texture=g.textures.find(draw_state.tex_id[tex_map]);
        auto efb_texture=g.efb_textures.find(draw_state.tex_id[tex_map]);
        if(draw_state.num_texgens && efb_texture!=g.efb_textures.end() && efb_texture->second.set)
            vkCmdBindDescriptorSets(g.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.layout,0,1,&efb_texture->second.set,0,nullptr);
        else if(draw_state.num_texgens && draw_texture!=g.textures.end() && draw_texture->second.set)
            vkCmdBindDescriptorSets(g.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.layout,0,1,&draw_texture->second.set,0,nullptr);
        else if(g.white.set)
            vkCmdBindDescriptorSets(g.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.layout,0,1,&g.white.set,0,nullptr);
struct Push{float projection[16];uint32_t tev[16];}push{};static_assert(sizeof(Push)==128);projection(b.states[c.state],push.projection);for(uint32_t stage=0;stage<2;stage++){const uint32_t env=draw_state.bp[0xC0+2*stage];push.tev[stage*4+0]=env;push.tev[stage*4+1]=draw_state.bp[0xC1+2*stage];push.tev[stage*4+2]=draw_state.bp[0x28+stage/2]>>((stage&1)*12);push.tev[stage*4+3]=draw_state.bp[0xF6+stage/2];}push.tev[8]=draw_state.tev_reg[0][0];push.tev[9]=draw_state.tev_reg[0][1];push.tev[10]=draw_state.tev_reg[1][0];push.tev[11]=draw_state.tev_reg[1][1];push.tev[12]=draw_state.tev_konst[0][0];push.tev[13]=draw_state.tev_konst[0][1];push.tev[14]=draw_state.tev_konst[1][0];push.tev[15]=draw_state.tev_konst[1][1];vkCmdPushConstants(g.cmd,g.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);vkCmdDrawIndexed(g.cmd,c.count,1,c.first,0,0);}vkCmdEndRenderPass(g.cmd);vkCmdEndQuery(g.cmd,g.occlusion,0);
    if(has_present) {
        VkImageMemoryBarrier s{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; s.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; s.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; s.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; s.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; s.image=g.efb_color.image; s.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&s);
        VkImageMemoryBarrier d{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; d.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; d.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; d.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; d.image=g.swap_images[image]; d.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&d);
        VkImageBlit bl{}; bl.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bl.srcOffsets[1]={608,448,1}; bl.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bl.dstOffsets[1]={(int)g.extent.width,(int)g.extent.height,1}; vkCmdBlitImage(g.cmd,g.efb_color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.swap_images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_LINEAR);
        d.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; d.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; d.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; d.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT; vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&d); s.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; s.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; s.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; s.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&s);
    }
    if(g.readback_supported && has_present && g.readback.b) {
        VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};to_copy.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;to_copy.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;to_copy.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;to_copy.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;to_copy.image=g.swap_images[image];to_copy.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&to_copy);
        VkBufferImageCopy rb{};rb.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};rb.imageExtent={g.extent.width,g.extent.height,1};vkCmdCopyImageToBuffer(g.cmd,g.swap_images[image],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.readback.b,1,&rb);
        VkImageMemoryBarrier to_present=to_copy;to_present.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;to_present.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;to_present.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;to_present.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(g.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&to_present);
    }
    check(vkEndCommandBuffer(g.cmd),"end command");
    VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};sub.waitSemaphoreCount=(wait_acquire?1u:0u);sub.pWaitSemaphores=&g.acquire;sub.pWaitDstStageMask=&wait_stage;sub.commandBufferCount=1;sub.pCommandBuffers=&g.cmd;if(has_present){sub.signalSemaphoreCount=1;sub.pSignalSemaphores=&g.render_done;}check(vkQueueSubmit(g.queue,1,&sub,g.fence),"submit");
    VkResult pr=VK_SUCCESS;if(has_present){VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&g.render_done;present.swapchainCount=1;present.pSwapchains=&g.swapchain;present.pImageIndices=&image;pr=vkQueuePresentKHR(g.queue,&present);if(pr!=VK_SUCCESS && pr!=VK_SUBOPTIMAL_KHR) check(pr,"present");g.frame_active=false;g.frame_clear=true;}check(vkWaitForFences(g.device,1,&g.fence,VK_TRUE,UINT64_MAX),"render fence");uint64_t samples=0;const VkResult qr=vkGetQueryPoolResults(g.device,g.occlusion,0,1,sizeof(samples),&samples,sizeof(samples),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);static unsigned pixel_logs=0;if(pixel_logs++<12) VKPIX("frame=%u image=%u occlusion_result=%d fragment_samples=%llu",g.frame+1,image,int(qr),(unsigned long long)samples);
    if(g.readback_supported && has_present && g.readback.b) { static unsigned frag_logs=0; if(frag_logs++<12) { const uint8_t* px=static_cast<const uint8_t*>(g.readback.mapped); size_t nonclear=0,bright=0; const size_t count=size_t(g.extent.width)*g.extent.height; for(size_t i=0;i<count;i++){const uint8_t* p=px+i*4; if(p[0]>12||p[1]>12||p[2]>12)++bright; if(p[0]>8||p[1]>8||p[2]>11)++nonclear;} VKFRAG("frame=%u image=%u bright=%zu nonclear=%zu/%zu",g.frame+1,image,bright,nonclear,count); } }
    vkDestroyFramebuffer(g.device,fb,nullptr);g.frame++;
    if(frames<=8) VKPRESENT("frame=%u image=%u result=%d",g.frame,image,(int)pr);
    if(frames%300==0)
        __android_log_print(ANDROID_LOG_INFO,"TANDEM-DRAW",
            "frames=%lu accepted=%lu type=%lu prim=%lu state=%lu",
            frames,accepted,rejected_type,rejected_prim,rejected_state);
    return true;
}
void resize(ANativeWindow* window,uint32_t width,uint32_t height){(void)window;(void)width;(void)height;}
void shutdown(){if(!g.device)return;vkDeviceWaitIdle(g.device);destroy(g.white);for(auto& [id,texture]:g.textures)destroy(texture);g.textures.clear();destroy(g.vertices);destroy(g.indices);if(g.occlusion)vkDestroyQueryPool(g.device,g.occlusion,nullptr);if(g.fence)vkDestroyFence(g.device,g.fence,nullptr);if(g.acquire)vkDestroySemaphore(g.device,g.acquire,nullptr);if(g.render_done)vkDestroySemaphore(g.device,g.render_done,nullptr);if(g.sampler)vkDestroySampler(g.device,g.sampler,nullptr);if(g.descriptors)vkDestroyDescriptorPool(g.device,g.descriptors,nullptr);if(g.pipeline)vkDestroyPipeline(g.device,g.pipeline,nullptr);if(g.layout)vkDestroyPipelineLayout(g.device,g.layout,nullptr);if(g.set_layout)vkDestroyDescriptorSetLayout(g.device,g.set_layout,nullptr);if(g.pass)vkDestroyRenderPass(g.device,g.pass,nullptr);for(auto v:g.swap_views)vkDestroyImageView(g.device,v,nullptr);if(g.swapchain)vkDestroySwapchainKHR(g.device,g.swapchain,nullptr);if(g.pool)vkDestroyCommandPool(g.device,g.pool,nullptr);vkDestroyDevice(g.device,nullptr);if(g.surface)vkDestroySurfaceKHR(g.instance,g.surface,nullptr);if(g.instance)vkDestroyInstance(g.instance,nullptr);g={};}
} // namespace gx::vk

// The Vulkan frontend deliberately exposes the same GX contract as the desktop
// backend.  The first Android pass renders the real transformed Batch stream; the
// VR-only controls are kept as no-ops until the Vulkan eye compositor is added.
#include "gx/render_gl.h"
#include "device_checkpoint.h"
namespace gx {
extern std::atomic<uint32_t> g_copy_writebacks_done;
extern std::atomic<bool> g_renderer_writes_back;
bool g_cull_swap=false; const char* g_dump_dir=nullptr; int g_dump_every=0;
void render_init(int) {}
void render_set_shader_cache(const char*) {}
void render_set_shader_cache_deferred(bool) {}
bool render_shader_cache_step(double,int*,int*) { return true; }
void render_set_internal_scale(int) {}
void render_set_window_size(int,int) {}
void render_set_flat_aspect(float,float) {}
void render_set_flat_overlay_hook(FlatOverlayHook,bool) {}
void render_set_output_fbo(unsigned) {}
bool render_execute(Batch& b) { return vk::draw(b); }
void render_interpolation_enable(bool) {}
void render_interpolation_reset() {}
bool render_interpolated(float) { return false; }
uint32_t present_count() { static uint32_t n=0; return n++; }
void render_set_vr_eye(const float*,const float*,const float*) {}
void render_hud_frame(float,float,float,float,float,float out[16]) { for(int i=0;i<16;i++)out[i]=(i%5)==0?1.f:0.f; }
void render_set_vr_morph(float,const float*) {}
void render_set_world_pitch(float) {}
void render_set_world_pitch_hook(WorldPitchHook) {}
void render_set_depth_layers(float,float,float,float,float) {}
void render_set_hud_push(float,float) {}
void render_set_view_space_world(bool) {}
void render_set_eye_filter(EyeFilter) {}
uint64_t render_texture_hash(uint32_t) { return 0; }
void render_set_eye_hook(EyeHook) {}
void render_set_first_person(bool) {}
bool render_first_person() { return false; }
void render_set_frame_hook(FrameHook,bool) {}
bool render_execute_eye(Batch& b,unsigned,int,int,bool) { return vk::draw(b); }
bool render_execute_stereo_pair(Batch& b,unsigned,unsigned,float,float) { return vk::draw(b); }
void render_set_panel_band(float) {}

}

DeviceFragment checkpoint_graphics_render(bool) { throw std::logic_error("Vulkan renderer checkpoint not implemented"); }
