// R2PORT (R2_FOSSILIZE): offline round trip of the Fossilize recorder wrapper (VKR2Fossilize.cpp).
// Records a shader module, descriptor set layout, pipeline layout, render pass and graphics pipeline with fake
// handles, reopens the .foz and checks the entries, then checks that a second (append) session deduplicates
// and that pipelines linked from pipeline libraries are not recorded. No Vulkan device is needed.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/VK/VKR2Fossilize.h"
#include "fossilize_db.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

template <typename T>
static T fake_handle(uintptr_t v) { return reinterpret_cast<T>(v); }

static void record_session(bool with_linked)
{
	static const uint32_t spirv[] = { 0x07230203, 0x00010000, 0, 8, 0, 0x00020011, 1, 0x0006000b };
	VkShaderModuleCreateInfo sm{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	sm.codeSize = sizeof(spirv);
	sm.pCode = spirv;
	r2foz::record_shader_module(fake_handle<VkShaderModule>(0x100), sm);

	VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo dsl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dsl.bindingCount = 1;
	dsl.pBindings = &b;
	r2foz::record_descriptor_set_layout(fake_handle<VkDescriptorSetLayout>(0x200), dsl);

	VkDescriptorSetLayout set = fake_handle<VkDescriptorSetLayout>(0x200);
	VkPushConstantRange pc{ VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &set;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pc;
	r2foz::record_pipeline_layout(fake_handle<VkPipelineLayout>(0x300), pl);

	VkAttachmentDescription att{ 0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &ref;
	VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rp.attachmentCount = 1;
	rp.pAttachments = &att;
	rp.subpassCount = 1;
	rp.pSubpasses = &sub;
	r2foz::record_render_pass(fake_handle<VkRenderPass>(0x400), rp);

	VkPipelineShaderStageCreateInfo stages[2]{};
	for (auto& s : stages)
	{
		s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		s.module = fake_handle<VkShaderModule>(0x100);
		s.pName = "main";
	}
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.lineWidth = 1.f;
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;

	VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &ds;
	gp.layout = fake_handle<VkPipelineLayout>(0x300);
	gp.renderPass = fake_handle<VkRenderPass>(0x400);
	r2foz::record_graphics_pipeline(fake_handle<VkPipeline>(0x500), gp);

	if (with_linked)
	{
		// A fast-linked pipeline (pipeline libraries) must be skipped by the wrapper
		VkPipeline libs[1] = { fake_handle<VkPipeline>(0x600) };
		VkPipelineLibraryCreateInfoKHR li{ VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
		li.libraryCount = 1;
		li.pLibraries = libs;
		VkGraphicsPipelineCreateInfo linked{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		linked.pNext = &li;
		linked.layout = gp.layout;
		r2foz::record_graphics_pipeline(fake_handle<VkPipeline>(0x700), linked);

		// A second distinct pipeline (different topology)
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		r2foz::record_graphics_pipeline(fake_handle<VkPipeline>(0x800), gp);
	}
}

static size_t count(Fossilize::DatabaseInterface& db, Fossilize::ResourceTag tag)
{
	size_t n = 0;
	CHECK(db.get_hash_list_for_resource_tag(tag, &n, nullptr));
	return n;
}

int main()
{
	const char* path = "test_r2foz.foz";
	std::remove(path);

	CHECK(!r2foz::recording());
	record_session(false); // not recording: no-op
	CHECK(r2foz::record_init(path));
	CHECK(r2foz::recording());
	record_session(false);
	r2foz::record_shutdown();
	CHECK(!r2foz::recording());

	{
		std::unique_ptr<Fossilize::DatabaseInterface> db(Fossilize::create_stream_archive_database(path, Fossilize::DatabaseMode::ReadOnly));
		CHECK(db && db->prepare());
		CHECK(count(*db, Fossilize::RESOURCE_SHADER_MODULE) == 1);
		CHECK(count(*db, Fossilize::RESOURCE_DESCRIPTOR_SET_LAYOUT) == 1);
		CHECK(count(*db, Fossilize::RESOURCE_PIPELINE_LAYOUT) == 1);
		CHECK(count(*db, Fossilize::RESOURCE_RENDER_PASS) == 1);
		CHECK(count(*db, Fossilize::RESOURCE_GRAPHICS_PIPELINE) == 1);
	}

	// Second session appends: the identical pipeline deduplicates, the linked one is skipped, the new one is added
	CHECK(r2foz::record_init(path));
	record_session(true);
	r2foz::record_shutdown();
	{
		std::unique_ptr<Fossilize::DatabaseInterface> db(Fossilize::create_stream_archive_database(path, Fossilize::DatabaseMode::ReadOnly));
		CHECK(db && db->prepare());
		CHECK(count(*db, Fossilize::RESOURCE_SHADER_MODULE) == 1);
		CHECK(count(*db, Fossilize::RESOURCE_GRAPHICS_PIPELINE) == 2);
	}

	std::remove(path);
	std::printf(g_fail ? "test_r2foz: %d failure(s)\n" : "test_r2foz: PASS\n", g_fail);
	return g_fail ? 1 : 0;
}
