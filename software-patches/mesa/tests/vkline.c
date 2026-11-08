/* offscreen triangle: vertex buffer + varying + clear, read back and check
 * usage: vktri [indexed|indirect|indexed-indirect|count1|count2] */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef W
#define W 256
#endif
#ifndef H
#define H 256
#endif
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static uint32_t *read_file(const char *p, size_t *sz)
{
   FILE *f = fopen(p, "rb");
   if (!f) { perror(p); exit(1); }
   fseek(f, 0, SEEK_END); *sz = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *d = malloc(*sz);
   if (fread(d, 1, *sz, f) != *sz) exit(1);
   fclose(f);
   return d;
}

static VkPhysicalDevice pd;
static VkDevice dev;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type\n"); exit(1);
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
   return b;
}

static VkShaderModule load_shader(const char *path)
{
   size_t sz; uint32_t *code = read_file(path, &sz);
   VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code };
   VkShaderModule m; CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
   return m;
}

int main(int argc, char **argv)
{
   const char *mode = "";
   int indexed = !strcmp(mode, "indexed") || !strcmp(mode, "indexed-indirect");
   int indirect = !strcmp(mode, "indirect") || !strcmp(mode, "indexed-indirect");
   int count = !strcmp(mode, "count1") ? 1 : !strcmp(mode, "count2") ? 2 : 0;
   VkInstance inst;
   VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   const char *count_ext = "VK_KHR_draw_indirect_count";
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .enabledExtensionCount = count ? 1 : 0, .ppEnabledExtensionNames = &count_ext };
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   PFN_vkCmdDrawIndirectCount draw_indirect_count = (PFN_vkCmdDrawIndirectCount)
      vkGetDeviceProcAddr(dev, "vkCmdDrawIndirectCountKHR");
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   /* colour target */
   VkImage img; VkDeviceMemory imem; VkMemoryRequirements mr;
   VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
   CHECK(vkCreateImage(dev, &imci, NULL, &img));
   vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = find_mem(mr.memoryTypeBits, 0) };
   CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));
   VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView view; CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

   /* triangle: pos.xy, colour.rgb; red at top, green bottom left, blue bottom right */
   /* line list from argv: x0 y0 x1 y1 [x0 y0 x1 y1 ...], white */
   float verts[16 * 5] = { 0, 1, 1, 1, 1,   0, -1, 1, 1, 1 };
   int nverts = 2;
   if (argc >= 5) {
      nverts = 0;
      for (int a = 1; a + 1 < argc && nverts < 16; a += 2, nverts++) {
         float *v = &verts[nverts * 5];
         v[0] = atof(argv[a]); v[1] = atof(argv[a + 1]); v[2] = v[3] = v[4] = 1;
      }
   }
   void *vmap; VkBuffer vb = make_buffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vmap);
   memcpy(vmap, verts, sizeof(verts));
   uint16_t idx[] = { 0, 1, 2 };
   void *imap; VkBuffer ib = make_buffer(sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &imap);
   memcpy(imap, idx, sizeof(idx));
   void *rmap; VkBuffer rb = make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rmap);
   /* indirect commands: [0] the triangle, [1..2] for count mode: an empty
    * draw first, then the triangle, so it only shows with count >= 2 */
   void *cmap; VkBuffer cmdb = make_buffer(64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &cmap);
   uint32_t *c = cmap;
   if (indexed) {
      uint32_t ic[] = { 3, 1, 0, 0, 0 };
      memcpy(c, ic, sizeof(ic));
   } else if (count) {
      uint32_t dc[] = { 3, 0, 0, 0,   3, 1, 0, 0 };
      memcpy(c, dc, sizeof(dc));
      c[15] = count;   /* count buffer value at byte 60 */
   } else {
      uint32_t dc[] = { 3, 1, 0, 0 };
      memcpy(c, dc, sizeof(dc));
   }

   VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
   VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
   VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
      .subpassCount = 1, .pSubpasses = &sub };
   VkRenderPass rp; CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
      .pAttachments = &view, .width = W, .height = H, .layers = 1 };
   VkFramebuffer fb; CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = load_shader("vktri.vert.spv"), .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = load_shader("vktri.frag.spv"), .pName = "main" },
   };
   VkVertexInputBindingDescription vbd = { 0, 5 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription vad[2] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 } };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = vad };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST };
   int vpw = getenv("VPW") ? atoi(getenv("VPW")) : W;	/* viewport size, fb stays W x H */
   VkViewport vp = { 0, 0, vpw, vpw, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { W, H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
   VkPipeline pipe; CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cmd, &cbbi));
   VkClearValue clear = { .color = { { 0.25f, 0.25f, 0.25f, 1.0f } } };
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
      .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &clear };
   vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkDeviceSize off = 0;
   vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
   if (indexed)
      vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT16);
   if (count)
      draw_indirect_count(cmd, cmdb, 0, cmdb, 60, 2, 16);
   else if (indirect && indexed)
      vkCmdDrawIndexedIndirect(cmd, cmdb, 0, 1, 20);
   else if (indirect)
      vkCmdDrawIndirect(cmd, cmdb, 0, 1, 16);
   else if (indexed)
      vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
   else
      vkCmdDraw(cmd, nverts, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
   vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
   CHECK(vkEndCommandBuffer(cmd));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   uint8_t *px = rmap;
#define PX(x, y) (px + ((y) * W + (x)) * 4)
   for (int x = 0; x < W; x++) {
      int n = 0, y0 = -1, y1 = -1;
      for (int y = 0; y < H; y++)
         if (PX(x, y)[0] == 255 && PX(x, y)[1] == 255) { n++; if (y0 < 0) y0 = y; y1 = y; }
      if (n) printf("column %3d: %3d pixels (y %d..%d)\n", x, n, y0, y1);
   }
   FILE *f = fopen("vkline.ppm", "wb");
   fprintf(f, "P6\n%d %d\n255\n", W, H);
   for (int i = 0; i < W * H; i++) fwrite(px + i * 4, 1, 3, f);
   fclose(f);
   return 0;
}
