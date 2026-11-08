/*
 * Direct scanout test for rotated outputs: a fullscreen Wayland window with a
 * Vulkan swapchain of WxH that declares wl_surface buffer transform T and
 * scale S, clearing to a changing colour each frame. With a pre-rotated
 * buffer matching the output, mutter can scan it out directly
 * (/sys/kernel/debug/dri/0/state: fb "allocated by" the client).
 *
 *   wayland-scanner client-header xdg-shell.xml xdg-shell-client-protocol.h
 *   wayland-scanner private-code xdg-shell.xml xdg-shell-protocol.c
 *   cc -O1 -o vkrot vkrot.c xdg-shell-protocol.c -lwayland-client -lvulkan
 *   ./vkrot 1080 2400 1 2 600     (W H transform scale frames)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static int configured;

static void
wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
   xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void
xs_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
   xdg_surface_ack_configure(s, serial);
   configured = 1;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void
top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
              struct wl_array *states)
{
   fprintf(stderr, "toplevel configure %dx%d\n", w, h);
}
static void top_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close };

static void
reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface,
           uint32_t ver)
{
   if (!strcmp(iface, wl_compositor_interface.name))
      compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
   else if (!strcmp(iface, xdg_wm_base_interface.name))
      wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int
main(int argc, char **argv)
{
   uint32_t W = argc > 1 ? atoi(argv[1]) : 1080, H = argc > 2 ? atoi(argv[2]) : 2400;
   int transform = argc > 3 ? atoi(argv[3]) : 1, scale = argc > 4 ? atoi(argv[4]) : 2;
   int frames = argc > 5 ? atoi(argv[5]) : 600;

   struct wl_display *dpy = wl_display_connect(NULL);
   if (!dpy) { fprintf(stderr, "no wayland\n"); return 1; }
   struct wl_registry *reg = wl_display_get_registry(dpy);
   wl_registry_add_listener(reg, &reg_listener, NULL);
   wl_display_roundtrip(dpy);
   xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);

   struct wl_surface *surf = wl_compositor_create_surface(compositor);
   struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
   xdg_surface_add_listener(xs, &xs_listener, NULL);
   struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
   xdg_toplevel_add_listener(top, &top_listener, NULL);
   xdg_toplevel_set_fullscreen(top, NULL);
   wl_surface_set_buffer_transform(surf, transform);
   wl_surface_set_buffer_scale(surf, scale);
   wl_surface_commit(surf);
   while (!configured)
      wl_display_dispatch(dpy);

   const char *iexts[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
   VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
      .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
   VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; VkPhysicalDevice pd; CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
   VkWaylandSurfaceCreateInfoKHR wsci = { .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
      .display = dpy, .surface = surf };
   VkSurfaceKHR vs; CHECK(vkCreateWaylandSurfaceKHR(inst, &wsci, NULL, &vs));

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   const char *dexts[] = { "VK_KHR_swapchain" };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
   VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   VkSurfaceCapabilitiesKHR caps; CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vs, &caps));
   fprintf(stderr, "caps: transforms 0x%x current 0x%x images %u-%u\n", caps.supportedTransforms,
           caps.currentTransform, caps.minImageCount, caps.maxImageCount);

   VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vs,
      .minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount,
      .imageFormat = VK_FORMAT_B8G8R8A8_UNORM, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = { W, H }, .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
   VkSwapchainKHR sc; CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
   uint32_t nimg = 0; vkGetSwapchainImagesKHR(dev, sc, &nimg, NULL);
   VkImage imgs[8]; vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);

   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
   VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   VkSemaphore acq, done; vkCreateSemaphore(dev, &semci, NULL, &acq); vkCreateSemaphore(dev, &semci, NULL, &done);
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence; vkCreateFence(dev, &fci, NULL, &fence);

   for (int f = 0; f < frames; f++) {
      uint32_t idx;
      VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
      if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
         fprintf(stderr, "acquire: %d\n", ar);
         return 1;
      }
      if (ar == VK_SUBOPTIMAL_KHR && f == 0)
         fprintf(stderr, "acquire: suboptimal\n");
      VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      vkBeginCommandBuffer(cmd, &bi);
      VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = imgs[idx], .subresourceRange = rng,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, NULL, 0, NULL, 1, &b);
      VkClearColorValue c = { .float32 = { (f % 120) / 120.0f, 0.3f, 1.0f - (f % 120) / 120.0f, 1.0f } };
      vkCmdClearColorImage(cmd, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rng);
      b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                           0, NULL, 0, NULL, 1, &b);
      vkEndCommandBuffer(cmd);
      VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
         .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
         .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &done };
      CHECK(vkQueueSubmit(q, 1, &si, fence));
      VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
         .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx };
      vkQueuePresentKHR(q, &pi);
      vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
      vkResetFences(dev, 1, &fence);
      wl_display_dispatch_pending(dpy);
   }
   return 0;
}
