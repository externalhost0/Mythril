#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <volk.h>

#include <cstdio>

namespace {
	constexpr int kSkip = 77;

	bool HasVulkanDevice() {
		if (volkInitialize() != VK_SUCCESS)
			return false;
		const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
		VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
#ifdef __APPLE__
		const char* portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
		info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		info.enabledExtensionCount = 1;
		info.ppEnabledExtensionNames = &portability;
#endif
		VkInstance instance = VK_NULL_HANDLE;
		if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
			return false;
		volkLoadInstanceOnly(instance);
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		vkDestroyInstance(instance, nullptr);
		return count > 0;
	}
} // namespace

int main(const int argc, char** argv) {
	if (!HasVulkanDevice()) {
		std::puts("mythril_gpu_tests: no Vulkan device, skipping");
		return kSkip;
	}
	doctest::Context context(argc, argv);
	return context.run();
}
