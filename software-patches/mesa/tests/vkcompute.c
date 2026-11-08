/* minimal Vulkan compute check: fill a buffer from a shader, verify on the CPU
 * usage: vkcompute [indirect] */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N (1024 * 1024)
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

static uint32_t find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type\n"); exit(1);
}

int main(int argc, char **argv)
{
   int indirect = argc > 1 && !strcmp(argv[1], "indirect");
   VkInstance inst;
   VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; VkPhysicalDevice pd;
   CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n", props.deviceName);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   /* output buffer + indirect args buffer, host visible */
   VkBuffer buf, ibuf;
   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N * 4,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   bci.size = 64; bci.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &ibuf));
   VkMemoryRequirements mr, imr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   vkGetBufferMemoryRequirements(dev, ibuf, &imr);
   VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, hv) };
   VkDeviceMemory mem, imem; CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   mai.allocationSize = imr.size; mai.memoryTypeIndex = find_mem(pd, imr.memoryTypeBits, hv);
   CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));
   CHECK(vkBindBufferMemory(dev, ibuf, imem, 0));
   uint32_t *out; CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&out));
   memset(out, 0xab, N * 4);
   uint32_t *iargs; CHECK(vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, (void **)&iargs));
   iargs[0] = N / 64; iargs[1] = 1; iargs[2] = 1;

   size_t spv_sz; uint32_t *spv = read_file(getenv("SPV") ? getenv("SPV") : "vkcompute.spv", &spv_sz);
   VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_sz, .pCode = spv };
   VkShaderModule sm; CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));

   VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
   VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
   VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
      .layout = pl };
   VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
   VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
   VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cb, &cbbi));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   uint32_t pc[2] = { 3, 7 };
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
   if (indirect)
      vkCmdDispatchIndirect(cb, ibuf, 0);
   else
      vkCmdDispatch(cb, N / 64, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   unsigned bad = 0, first = ~0u;
   for (uint32_t i = 0; i < N; i++)
      if (out[i] != i * 3 + 7) { if (first == ~0u) first = i; bad++; }
   if (bad)
      printf("%s dispatch: FAIL %u wrong, first at %u: %08x (want %08x)\n",
             indirect ? "indirect" : "direct", bad, first, out[first], first * 3 + 7);
   else
      printf("%s dispatch: OK (%u values)\n", indirect ? "indirect" : "direct", N);
   return bad != 0;
}
