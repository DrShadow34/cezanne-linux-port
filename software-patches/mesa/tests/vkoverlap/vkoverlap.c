/* vkoverlap: render-pass overlap on PanVK JM (vertex/tiler/compute chains
 * running while earlier fragment chains still do). Headless, Vulkan 1.3.
 *
 * Default: race checks. Each frame a slow fragment pass writes X (or reads
 * T late), then, behind the barrier an application must use, something on
 * the vertex/tiler/compute side reads (or writes) it:
 *   vs     colour output -> vertex shader: a vertex shader reads X
 *   cs     colour output -> compute shader: a dispatch reads X
 *   copy   colour output -> copy: X copied to a buffer
 *   query  precise occlusion query around the cs test's slow pass, then
 *          vkCmdCopyQueryPoolResults (not around the vs one: EndQuery's
 *          availability write waits for the fragment jobs and would hide a
 *          missing wait there)
 *   war    fragment shader reads T at its end -> compute writes T
 *   cs2    colour output -> compute shader, then a render pass whose vertex
 *          shader reads no memory (it may skip the wait), then the
 *          dispatch reading X: the skipped barrier must stay pending
 *   vin    a fragment shader stores to V, fragment shader -> vertex
 *          attribute input, V is the next pass's vertex buffer (vertex
 *          shader without descriptors: only "fragment jobs wrote memory"
 *          makes it wait)
 * A missing wait shows as the previous frame's value.
 * BAD=1: the vs barrier only names the fragment shader stage (insufficient):
 * with overlap it should fail, the check can see races. BAD2=1: the same
 * for the vin barrier, BAD3=1 for the cs2 one (then cs2 only passes with
 * the correct barrier if the skipped wait stays pending).
 * ONE=1: all frames in one vkQueueSubmit (else one submit per frame).
 * DBG=1: the vs pass writes (vertex shader's min, frame): tells a pass that
 * didn't run from a stale read.
 * PERF=1: frames of P passes, each a heavy vertex-only draw and a slow
 * full-screen draw, barriers colour output -> fragment shader only; prints
 * ms per frame. BROAD=1: the barriers between passes name all graphics,
 * compute and transfer stages (as DXVK often does); the heavy vertex shader
 * reads no memory, so they shouldn't stop the overlap. Overlap is on by
 * default; PANVK_JM_OVERLAP=0 runs the serial reference.
 * Env: FRAMES, ITERS (fragment loop), PASSES, TRIS, VITERS (vertex loop).
 * Shaders: build.sh (glslangValidator), the .spv files next to the binary.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XW 1024
#define XH 1024
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;
static const char *dir = ".";

static unsigned env(const char *n, unsigned def)
{
   const char *v = getenv(n);
   return v ? strtoul(v, NULL, 0) : def;
}

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type\n");
   exit(1);
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
   VkBuffer b; VkDeviceMemory m; VkMemoryRequirements mr;
   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
   CHECK(vkCreateBuffer(dev, &bci, NULL, &b));
   vkGetBufferMemoryRequirements(dev, b, &mr);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   CHECK(vkAllocateMemory(dev, &mai, NULL, &m));
   CHECK(vkBindBufferMemory(dev, b, m, 0));
   CHECK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map));
   memset(*map, 0xff, size);
   return b;
}

struct image { VkImage img; VkImageView view; };

static struct image make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage)
{
   struct image r; VkDeviceMemory m; VkMemoryRequirements mr;
   VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
   CHECK(vkCreateImage(dev, &ci, NULL, &r.img));
   vkGetImageMemoryRequirements(dev, r.img, &mr);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = find_mem(mr.memoryTypeBits, 0) };
   CHECK(vkAllocateMemory(dev, &mai, NULL, &m));
   CHECK(vkBindImageMemory(dev, r.img, m, 0));
   VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   CHECK(vkCreateImageView(dev, &vci, NULL, &r.view));
   return r;
}

static VkShaderModule shader(const char *name)
{
   char p[512];
   snprintf(p, sizeof(p), "%s/%s.spv", dir, name);
   FILE *f = fopen(p, "rb");
   if (!f) { perror(p); exit(1); }
   fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(sz);
   if (fread(code, 1, sz, f) != sz) exit(1);
   fclose(f);
   VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code };
   VkShaderModule m; CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
   return m;
}

static VkPipelineLayout layout;

static VkPipeline gfx_vi(const char *vs, const char *fs, int mode, VkFormat fmt, int vattr)
{
   VkSpecializationMapEntry me = { 0, 0, sizeof(int) };
   VkSpecializationInfo si = { 1, &me, sizeof(int), &mode };
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader(vs), .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader(fs), .pName = "main", .pSpecializationInfo = &si },
   };
   /* vattr: one uint attribute per vertex at location 0, binding 0 */
   VkVertexInputBindingDescription vb = { 0, 4, VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32_UINT, 0 };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = vattr ? 1 : 0, .pVertexBindingDescriptions = &vb,
      .vertexAttributeDescriptionCount = vattr ? 1 : 0, .pVertexAttributeDescriptions = &va };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
   VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
   VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci,
      .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
      .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = layout };
   VkPipeline p; CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
   return p;
}

static VkPipeline gfx(const char *vs, const char *fs, int mode, VkFormat fmt)
{
   return gfx_vi(vs, fs, mode, fmt, 0);
}

static VkPipeline comp(const char *cs)
{
   VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader(cs), .pName = "main" },
      .layout = layout };
   VkPipeline p; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
   return p;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
   VkMemoryBarrier2 mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
      .dstStageMask = ds, .dstAccessMask = da };
   VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
   vkCmdPipelineBarrier2(cb, &di);
}

static void begin_pass(VkCommandBuffer cb, VkImageView v, uint32_t w, uint32_t h)
{
   VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
   VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, h } },
      .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
   vkCmdBeginRendering(cb, &ri);
   VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { w, h } };
   vkCmdSetViewport(cb, 0, 1, &vp);
   vkCmdSetScissor(cb, 0, 1, &sc);
}

static void push(VkCommandBuffer cb, uint32_t frame, uint32_t iters)
{
   uint32_t pc[2] = { frame, iters };
   vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), pc);
}

static void copy_px(VkCommandBuffer cb, VkImage img, VkBuffer buf, VkDeviceSize off, int32_t x, int32_t y)
{
   VkBufferImageCopy r = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageOffset = { x, y, 0 }, .imageExtent = { 1, 1, 1 } };
   vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
}

#define ALL_STAGES VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
#define ALL_ACCESS (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)
#define COLOR_OUT VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
#define COLOR_WR VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT

int main(int argc, char **argv)
{
   if (argc > 1)
      dir = argv[1];
   unsigned frames = env("FRAMES", 40), iters = env("ITERS", 1500);
   int bad = env("BAD", 0), one = env("ONE", 0), perf = env("PERF", 0), dbg = env("DBG", 0);
   int bad2 = env("BAD2", 0), bad3 = env("BAD3", 0), broad = env("BROAD", 0);

   VkInstance inst;
   VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE };
   VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13,
      .features = { .occlusionQueryPrecise = VK_TRUE } };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   struct image X = make_image(XW, XH, VK_FORMAT_R32_UINT, rt | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
   struct image X2 = make_image(XW, XH, VK_FORMAT_R32_UINT, rt);
   struct image Y = make_image(4, 4, VK_FORMAT_R32G32_UINT, rt);
   struct image T = make_image(2, 1, VK_FORMAT_R32_UINT,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

   void *m_vs, *m_cs, *m_copy, *m_q, *m_war, *m_vin, *m_v;
   VkBuffer b_vs = make_buffer(frames * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m_vs);
   /* cs, then cs2 from offset frames * 8 */
   VkBuffer b_cs = make_buffer(frames * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &m_cs);
   VkBuffer b_vin = make_buffer(frames * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m_vin);
   /* vin: written by a fragment shader, read as a vertex buffer */
   VkBuffer b_v = make_buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m_v);
   VkBuffer b_copy = make_buffer(frames * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m_copy);
   VkBuffer b_q = make_buffer(frames * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m_q);
   VkBuffer b_war = make_buffer(frames * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m_war);

   /* set 0: X sampled (0), result buffer (1), T storage (2), T sampled (3),
    * V (4) */
   VkDescriptorSetLayoutBinding bind[5] = {
      { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL },
      { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL },
      { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL },
      { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL },
      { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL },
   };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bind };
   VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_ALL, 0, 8 };
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
   VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
   VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
      .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST };
   VkSampler smp; CHECK(vkCreateSampler(dev, &sci, NULL, &smp));
   VkDescriptorImageInfo ix = { smp, X.view, VK_IMAGE_LAYOUT_GENERAL }, it = { smp, T.view, VK_IMAGE_LAYOUT_GENERAL };
   VkDescriptorBufferInfo bcs = { b_cs, 0, VK_WHOLE_SIZE }, bv = { b_v, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet w[5] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ix },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bcs },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &it },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &it },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bv },
   };
   vkUpdateDescriptorSets(dev, 5, w, 0, NULL);

   VkPipeline p_slow = gfx("fs.vert", "slow.frag", 0, VK_FORMAT_R32_UINT);
   VkPipeline p_war = gfx("fs.vert", "slow.frag", 1, VK_FORMAT_R32_UINT);
   VkPipeline p_vsread = gfx("vsread.vert", dbg ? "passdbg.frag" : "pass.frag", 0, VK_FORMAT_R32G32_UINT);
   VkPipeline p_heavy = gfx("heavy.vert", "zero.frag", 0, VK_FORMAT_R32_UINT);
   VkPipeline p_readx = comp("readx.comp"), p_writet = comp("writet.comp");
   VkPipeline p_ssbo = gfx("fs.vert", "ssbo.frag", 0, VK_FORMAT_R32_UINT);
   VkPipeline p_vattr = gfx_vi("vattr.vert", "pass.frag", 0, VK_FORMAT_R32G32_UINT, 1);

   VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = frames };
   VkQueryPool qp; CHECK(vkCreateQueryPool(dev, &qpci, NULL, &qp));

   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBuffer *cbs = calloc(frames + 1, sizeof(*cbs));
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = frames + 1 };
   CHECK(vkAllocateCommandBuffers(dev, &cbai, cbs));
   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

   /* init: everything GENERAL, X = ~0, T = 1000 */
   VkCommandBuffer cb = cbs[frames];
   CHECK(vkBeginCommandBuffer(cb, &bi));
   VkImage imgs[] = { X.img, X2.img, Y.img, T.img };
   for (unsigned i = 0; i < 4; i++) {
      VkImageMemoryBarrier2 ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = ALL_STAGES, .dstStageMask = ALL_STAGES, .dstAccessMask = ALL_ACCESS,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = imgs[i],
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
      VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib };
      vkCmdPipelineBarrier2(cb, &di);
   }
   VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkClearColorValue cx = { .uint32 = { ~0u } }, ct = { .uint32 = { 1000 } };
   vkCmdClearColorImage(cb, X.img, VK_IMAGE_LAYOUT_GENERAL, &cx, 1, &rng);
   vkCmdClearColorImage(cb, T.img, VK_IMAGE_LAYOUT_GENERAL, &ct, 1, &rng);
   vkCmdResetQueryPool(cb, qp, 0, frames);
   barrier(cb, ALL_STAGES, ALL_ACCESS, ALL_STAGES, ALL_ACCESS);
   CHECK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   unsigned passes = env("PASSES", 8), tris = env("TRIS", 100000), viters = env("VITERS", 200);
   for (unsigned k = 0; k < frames; k++) {
      cb = cbs[k];
      CHECK(vkBeginCommandBuffer(cb, &bi));
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
      if (perf) {
         for (unsigned p = 0; p < passes; p++) {
            begin_pass(cb, (p & 1) ? X2.view : X.view, XW, XH);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_heavy);
            push(cb, k, viters);
            vkCmdDraw(cb, 3 * tris, 1, 0, 0);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_slow);
            push(cb, k, iters);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            barrier(cb, COLOR_OUT, COLOR_WR,
                    broad ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                          : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
         }
         CHECK(vkEndCommandBuffer(cb));
         continue;
      }

      /* earlier reads of X / T on the vertex/tiler/compute side before
       * this frame's writes (applications need it, PanVK orders it anyway) */
      barrier(cb, ALL_STAGES, ALL_ACCESS, COLOR_OUT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, ALL_ACCESS);

      /* vs: slow pass writes X = k, a vertex shader reads it */
      begin_pass(cb, X.view, XW, XH);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_slow);
      push(cb, k, iters);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      barrier(cb, COLOR_OUT, COLOR_WR,
              bad ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      begin_pass(cb, Y.view, 4, 4);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_vsread);
      push(cb, k, 0);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      barrier(cb, COLOR_OUT, COLOR_WR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      copy_px(cb, Y.img, b_vs, k * 8, 0, 0);

      /* cs: X = k + 100000 (counted by the occlusion query), a dispatch
       * reads it */
      barrier(cb, ALL_STAGES, ALL_ACCESS, COLOR_OUT, ALL_ACCESS);
      begin_pass(cb, X.view, XW, XH);
      vkCmdBeginQuery(cb, qp, k, VK_QUERY_CONTROL_PRECISE_BIT);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_slow);
      push(cb, k + 100000, iters);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndQuery(cb, qp, k);
      vkCmdEndRendering(cb);
      /* query: copy the result (query commands run in order) */
      vkCmdCopyQueryPoolResults(cb, qp, k, 1, b_q, k * 8, 8,
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      barrier(cb, COLOR_OUT, COLOR_WR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p_readx);
      push(cb, k, 0);
      vkCmdDispatch(cb, 1, 1, 1);

      /* copy: X = k + 200000, copied out */
      barrier(cb, ALL_STAGES, ALL_ACCESS, COLOR_OUT, ALL_ACCESS);
      begin_pass(cb, X.view, XW, XH);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_slow);
      push(cb, k + 200000, iters);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      barrier(cb, COLOR_OUT, COLOR_WR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      copy_px(cb, X.img, b_copy, k * 8, 0, 0);
      copy_px(cb, X.img, b_copy, k * 8 + 4, XW - 1, XH - 1);

      /* war: the slow pass reads T (= 1000 + k) at its end into X2, then a
       * dispatch writes T = 1000 + k + 1 */
      begin_pass(cb, X2.view, XW, XH);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_war);
      push(cb, k, iters);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      barrier(cb, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p_writet);
      push(cb, k, 1000 + k + 1);
      vkCmdDispatch(cb, 1, 1, 1);
      barrier(cb, COLOR_OUT, COLOR_WR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      copy_px(cb, X2.img, b_war, k * 4, XW - 1, XH - 1);

      /* cs2: X = k + 300000; barrier to compute; a render pass that may
       * skip the wait (vertex shader without memory access); the dispatch */
      barrier(cb, ALL_STAGES, ALL_ACCESS, COLOR_OUT, ALL_ACCESS);
      begin_pass(cb, X.view, XW, XH);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_slow);
      push(cb, k + 300000, iters);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      barrier(cb, COLOR_OUT, COLOR_WR,
              bad3 ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      begin_pass(cb, X2.view, XW, XH);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_heavy);
      push(cb, k, 1);
      vkCmdDraw(cb, 300, 1, 0, 0);
      vkCmdEndRendering(cb);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p_readx);
      push(cb, frames + k, 0);
      vkCmdDispatch(cb, 1, 1, 1);

      /* vin: a fragment shader stores k + 400000 to V, V is the next
       * pass's vertex buffer. Before: the previous frame's vertex buffer
       * read of V -> this write (fragment side only, so no barrier from
       * fragment work is left pending to hide a missing wait) */
      barrier(cb, ALL_STAGES, ALL_ACCESS, COLOR_OUT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, ALL_ACCESS);
      begin_pass(cb, X2.view, XW, XH);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_ssbo);
      push(cb, k + 400000, iters);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      barrier(cb, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              bad2 ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
              bad2 ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
      begin_pass(cb, Y.view, 4, 4);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_vattr);
      VkDeviceSize zero = 0;
      vkCmdBindVertexBuffers(cb, 0, 1, &b_v, &zero);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      barrier(cb, COLOR_OUT, COLOR_WR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      copy_px(cb, Y.img, b_vin, k * 8, 0, 0);
      CHECK(vkEndCommandBuffer(cb));
   }

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   if (one) {
      si.commandBufferCount = frames;
      si.pCommandBuffers = cbs;
      CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   } else {
      for (unsigned k = 0; k < frames; k++) {
         si.commandBufferCount = 1;
         si.pCommandBuffers = &cbs[k];
         CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
      }
   }
   CHECK(vkQueueWaitIdle(q));
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

   if (perf) {
      printf("perf: %u frames x %u passes (%u tris, viters %u, iters %u): %.2f ms/frame\n",
             frames, passes, tris, viters, iters, ms / frames);
      return 0;
   }

   unsigned f_vs = 0, f_cs = 0, f_copy = 0, f_q = 0, f_war = 0, f_cs2 = 0, f_vin = 0;
   uint32_t *vs = m_vs, *cs = m_cs, *cp_ = m_copy, *war = m_war, *vin = m_vin;
   uint64_t *qr = m_q;
   for (unsigned k = 0; k < frames; k++) {
      if (vs[2 * k] != k || vs[2 * k + 1] != k) {
         if (f_vs++ < 3) printf("vs   frame %u: %u..%u\n", k, vs[2 * k], vs[2 * k + 1]);
      }
      if (cs[2 * k] != k + 100000 || cs[2 * k + 1] != k + 100000) {
         if (f_cs++ < 3) printf("cs   frame %u: %u..%u\n", k, cs[2 * k], cs[2 * k + 1]);
      }
      if (cp_[2 * k] != k + 200000 || cp_[2 * k + 1] != k + 200000) {
         if (f_copy++ < 3) printf("copy frame %u: %u %u\n", k, cp_[2 * k], cp_[2 * k + 1]);
      }
      if (qr[k] != (uint64_t)XW * XH) {
         if (f_q++ < 3) printf("query frame %u: %llu\n", k, (unsigned long long)qr[k]);
      }
      if (war[k] != 1000 + k) {
         if (f_war++ < 3) printf("war  frame %u: %u\n", k, war[k]);
      }
      uint32_t *c2 = cs + 2 * frames;
      if (c2[2 * k] != k + 300000 || c2[2 * k + 1] != k + 300000) {
         if (f_cs2++ < 3) printf("cs2  frame %u: %u..%u\n", k, c2[2 * k], c2[2 * k + 1]);
      }
      if (vin[2 * k] != k + 400000 || vin[2 * k + 1] != k + 400000) {
         if (f_vin++ < 3) printf("vin  frame %u: %u..%u\n", k, vin[2 * k], vin[2 * k + 1]);
      }
   }
   printf("%u frames%s%s%s, %.1f ms/frame: fail vs %u, cs %u, copy %u, query %u, war %u, cs2 %u, vin %u\n",
          frames, bad ? " BAD" : "", bad2 ? " BAD2" : bad3 ? " BAD3" : "", one ? " ONE" : "", ms / frames,
          f_vs, f_cs, f_copy, f_q, f_war, f_cs2, f_vin);
   return (f_vs || f_cs || f_copy || f_q || f_war || f_cs2 || f_vin) ? 1 : 0;
}
