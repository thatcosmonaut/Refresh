/* Refresh - XNA-inspired 3D Graphics Library with modern capabilities
 *
 * Copyright (c) 2020 Evan Hemsley
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Evan "cosmonaut" Hemsley <evan@moonside.games>
 *
 */

#if REFRESH_DRIVER_VULKAN

#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"

#include "Refresh_Driver.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_vulkan.h>

/* Global Vulkan Loader Entry Points */

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = NULL;

#define VULKAN_GLOBAL_FUNCTION(name) \
	static PFN_##name name = NULL;
#include "Refresh_Driver_Vulkan_vkfuncs.h"

/* vkInstance/vkDevice function typedefs */

#define VULKAN_INSTANCE_FUNCTION(ext, ret, func, params) \
	typedef ret (VKAPI_CALL *vkfntype_##func) params;
#define VULKAN_DEVICE_FUNCTION(ext, ret, func, params) \
	typedef ret (VKAPI_CALL *vkfntype_##func) params;
#include "Refresh_Driver_Vulkan_vkfuncs.h"

/* Required extensions */
static const char* deviceExtensionNames[] =
{
	/* Globally supported */
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	/* Core since 1.1 */
	VK_KHR_MAINTENANCE1_EXTENSION_NAME,
	VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
	VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
	/* Core since 1.2 */
	VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
	/* EXT, probably not going to be Core */
	VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
};
static uint32_t deviceExtensionCount = SDL_arraysize(deviceExtensionNames);

/* Defines */

#define STARTING_ALLOCATION_SIZE 64000000 		/* 64MB */
#define MAX_ALLOCATION_SIZE 256000000 			/* 256MB */
#define TEXTURE_STAGING_SIZE 8000000 			/* 8MB */
#define UBO_BUFFER_SIZE 8000000 				/* 8MB */
#define UBO_ACTUAL_SIZE (UBO_BUFFER_SIZE * 2)
#define SAMPLER_POOL_STARTING_SIZE 128
#define UBO_POOL_SIZE 1000
#define SUB_BUFFER_COUNT 2
#define DESCRIPTOR_SET_DEACTIVATE_FRAMES 10

#define IDENTITY_SWIZZLE \
{ \
	VK_COMPONENT_SWIZZLE_IDENTITY, \
	VK_COMPONENT_SWIZZLE_IDENTITY, \
	VK_COMPONENT_SWIZZLE_IDENTITY, \
	VK_COMPONENT_SWIZZLE_IDENTITY \
}

#define NULL_DESC_LAYOUT (VkDescriptorSetLayout) 0
#define NULL_PIPELINE_LAYOUT (VkPipelineLayout) 0
#define NULL_RENDER_PASS (REFRESH_RenderPass*) 0

#define EXPAND_ELEMENTS_IF_NEEDED(arr, initialValue, type)	\
	if (arr->count == arr->capacity)		\
	{						\
		if (arr->capacity == 0)			\
		{					\
			arr->capacity = initialValue;	\
		}					\
		else					\
		{					\
			arr->capacity *= 2;		\
		}					\
		arr->elements = (type*) SDL_realloc(	\
			arr->elements,			\
			arr->capacity * sizeof(type)	\
		);					\
	}

#define EXPAND_ARRAY_IF_NEEDED(arr, elementType, newCount, capacity, newCapacity)	\
	if (newCount >= capacity)														\
	{																				\
		capacity = newCapacity;														\
		arr = (elementType*) SDL_realloc(													\
			arr,																	\
			sizeof(elementType) * capacity													\
		);																			\
	}

#define MOVE_ARRAY_CONTENTS_AND_RESET(i, dstArr, dstCount, srcArr, srcCount)	\
	for (i = 0; i < srcCount; i += 1)											\
	{																			\
		dstArr[i] = srcArr[i];													\
	}																			\
	dstCount = srcCount;														\
	srcCount = 0;

/* Enums */

typedef enum VulkanResourceAccessType
{
	/* Reads */
	RESOURCE_ACCESS_NONE, /* For initialization */
	RESOURCE_ACCESS_INDEX_BUFFER,
	RESOURCE_ACCESS_VERTEX_BUFFER,
	RESOURCE_ACCESS_VERTEX_SHADER_READ_UNIFORM_BUFFER,
	RESOURCE_ACCESS_VERTEX_SHADER_READ_SAMPLED_IMAGE,
	RESOURCE_ACCESS_FRAGMENT_SHADER_READ_UNIFORM_BUFFER,
	RESOURCE_ACCESS_FRAGMENT_SHADER_READ_SAMPLED_IMAGE,
	RESOURCE_ACCESS_FRAGMENT_SHADER_READ_COLOR_ATTACHMENT,
	RESOURCE_ACCESS_FRAGMENT_SHADER_READ_DEPTH_STENCIL_ATTACHMENT,
	RESOURCE_ACCESS_ANY_SHADER_READ_SAMPLED_IMAGE,
	RESOURCE_ACCESS_COLOR_ATTACHMENT_READ,
	RESOURCE_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ,
	RESOURCE_ACCESS_TRANSFER_READ,
	RESOURCE_ACCESS_HOST_READ,
	RESOURCE_ACCESS_PRESENT,
	RESOURCE_ACCESS_END_OF_READ,

	/* Writes */
	RESOURCE_ACCESS_VERTEX_SHADER_WRITE,
	RESOURCE_ACCESS_FRAGMENT_SHADER_WRITE,
	RESOURCE_ACCESS_COLOR_ATTACHMENT_WRITE,
	RESOURCE_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE,
	RESOURCE_ACCESS_TRANSFER_WRITE,
	RESOURCE_ACCESS_HOST_WRITE,

	/* Read-Writes */
	RESOURCE_ACCESS_COLOR_ATTACHMENT_READ_WRITE,
	RESOURCE_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_WRITE,
	RESOURCE_ACCESS_MEMORY_TRANSFER_READ_WRITE,
	RESOURCE_ACCESS_GENERAL,

	/* Count */
	RESOURCE_ACCESS_TYPES_COUNT
} VulkanResourceAccessType;

typedef enum CreateSwapchainResult
{
	CREATE_SWAPCHAIN_FAIL,
	CREATE_SWAPCHAIN_SUCCESS,
	CREATE_SWAPCHAIN_SURFACE_ZERO,
} CreateSwapchainResult;

/* Conversions */

static VkFormat RefreshToVK_SurfaceFormat[] =
{
	VK_FORMAT_R8G8B8A8_UNORM,		    /* R8G8B8A8 */
	VK_FORMAT_R5G6B5_UNORM_PACK16,		/* R5G6B5 */
	VK_FORMAT_A1R5G5B5_UNORM_PACK16,	/* A1R5G5B5 */
	VK_FORMAT_B4G4R4A4_UNORM_PACK16,	/* B4G4R4A4 */
	VK_FORMAT_BC1_RGBA_UNORM_BLOCK,		/* BC1 */
	VK_FORMAT_BC2_UNORM_BLOCK,		    /* BC3 */
	VK_FORMAT_BC3_UNORM_BLOCK,		    /* BC5 */
	VK_FORMAT_R8G8_SNORM,			    /* R8G8_SNORM */
	VK_FORMAT_R8G8B8A8_SNORM,		    /* R8G8B8A8_SNORM */
	VK_FORMAT_A2R10G10B10_UNORM_PACK32,	/* A2R10G10B10 */
	VK_FORMAT_R16G16_UNORM,			    /* R16G16 */
	VK_FORMAT_R16G16B16A16_UNORM,		/* R16G16B16A16 */
	VK_FORMAT_R8_UNORM,			        /* R8 */
	VK_FORMAT_R32_SFLOAT,			    /* R32_SFLOAT */
	VK_FORMAT_R32G32_SFLOAT,		    /* R32G32_SFLOAT */
	VK_FORMAT_R32G32B32A32_SFLOAT,		/* R32G32B32A32_SFLOAT */
	VK_FORMAT_R16_SFLOAT,			    /* R16_SFLOAT */
	VK_FORMAT_R16G16_SFLOAT,		    /* R16G16_SFLOAT */
	VK_FORMAT_R16G16B16A16_SFLOAT		/* R16G16B16A16_SFLOAT */
};

static VkFormat RefreshToVK_DepthFormat[] =
{
    VK_FORMAT_D16_UNORM,
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D16_UNORM_S8_UINT,
    VK_FORMAT_D32_SFLOAT_S8_UINT
};

static VkFormat RefreshToVK_VertexFormat[] =
{
	VK_FORMAT_R32_SFLOAT,			/* SINGLE */
	VK_FORMAT_R32G32_SFLOAT,		/* VECTOR2 */
	VK_FORMAT_R32G32B32_SFLOAT,		/* VECTOR3 */
	VK_FORMAT_R32G32B32A32_SFLOAT,	/* VECTOR4 */
	VK_FORMAT_R8G8B8A8_UNORM,		/* COLOR */
	VK_FORMAT_R8G8B8A8_USCALED,		/* BYTE4 */
	VK_FORMAT_R16G16_SSCALED,		/* SHORT2 */
	VK_FORMAT_R16G16B16A16_SSCALED,	/* SHORT4 */
	VK_FORMAT_R16G16_SNORM,			/* NORMALIZEDSHORT2 */
	VK_FORMAT_R16G16B16A16_SNORM,	/* NORMALIZEDSHORT4 */
	VK_FORMAT_R16G16_SFLOAT,		/* HALFVECTOR2 */
	VK_FORMAT_R16G16B16A16_SFLOAT	/* HALFVECTOR4 */
};

static VkIndexType RefreshToVK_IndexType[] =
{
	VK_INDEX_TYPE_UINT16,
	VK_INDEX_TYPE_UINT32
};

static VkPrimitiveTopology RefreshToVK_PrimitiveType[] =
{
	VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
	VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
};

static VkPolygonMode RefreshToVK_PolygonMode[] =
{
	VK_POLYGON_MODE_FILL,
	VK_POLYGON_MODE_LINE,
	VK_POLYGON_MODE_POINT
};

static VkCullModeFlags RefreshToVK_CullMode[] =
{
	VK_CULL_MODE_NONE,
	VK_CULL_MODE_FRONT_BIT,
	VK_CULL_MODE_BACK_BIT,
	VK_CULL_MODE_FRONT_AND_BACK
};

static VkFrontFace RefreshToVK_FrontFace[] =
{
	VK_FRONT_FACE_COUNTER_CLOCKWISE,
	VK_FRONT_FACE_CLOCKWISE
};

static VkBlendFactor RefreshToVK_BlendFactor[] =
{
	VK_BLEND_FACTOR_ZERO,
	VK_BLEND_FACTOR_ONE,
	VK_BLEND_FACTOR_SRC_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
	VK_BLEND_FACTOR_DST_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
	VK_BLEND_FACTOR_SRC_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	VK_BLEND_FACTOR_DST_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
	VK_BLEND_FACTOR_CONSTANT_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
	VK_BLEND_FACTOR_CONSTANT_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
	VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
	VK_BLEND_FACTOR_SRC1_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
	VK_BLEND_FACTOR_SRC1_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
};

static VkBlendOp RefreshToVK_BlendOp[] =
{
	VK_BLEND_OP_ADD,
	VK_BLEND_OP_SUBTRACT,
	VK_BLEND_OP_REVERSE_SUBTRACT,
	VK_BLEND_OP_MIN,
	VK_BLEND_OP_MAX
};

static VkLogicOp RefreshToVK_LogicOp[] =
{
	VK_LOGIC_OP_CLEAR,
	VK_LOGIC_OP_AND,
	VK_LOGIC_OP_AND_REVERSE,
	VK_LOGIC_OP_COPY,
	VK_LOGIC_OP_AND_INVERTED,
	VK_LOGIC_OP_NO_OP,
	VK_LOGIC_OP_XOR,
	VK_LOGIC_OP_OR,
	VK_LOGIC_OP_NOR,
	VK_LOGIC_OP_EQUIVALENT,
	VK_LOGIC_OP_INVERT,
	VK_LOGIC_OP_OR_REVERSE,
	VK_LOGIC_OP_COPY_INVERTED,
	VK_LOGIC_OP_OR_INVERTED,
	VK_LOGIC_OP_NAND,
	VK_LOGIC_OP_SET
};

static VkCompareOp RefreshToVK_CompareOp[] =
{
	VK_COMPARE_OP_NEVER,
	VK_COMPARE_OP_LESS,
	VK_COMPARE_OP_EQUAL,
	VK_COMPARE_OP_LESS_OR_EQUAL,
	VK_COMPARE_OP_GREATER,
	VK_COMPARE_OP_NOT_EQUAL,
	VK_COMPARE_OP_GREATER_OR_EQUAL,
	VK_COMPARE_OP_ALWAYS
};

static VkStencilOp RefreshToVK_StencilOp[] =
{
	VK_STENCIL_OP_KEEP,
	VK_STENCIL_OP_ZERO,
	VK_STENCIL_OP_REPLACE,
	VK_STENCIL_OP_INCREMENT_AND_CLAMP,
	VK_STENCIL_OP_DECREMENT_AND_CLAMP,
	VK_STENCIL_OP_INVERT,
	VK_STENCIL_OP_INCREMENT_AND_WRAP,
	VK_STENCIL_OP_DECREMENT_AND_WRAP
};

static VkAttachmentLoadOp RefreshToVK_LoadOp[] =
{
    VK_ATTACHMENT_LOAD_OP_LOAD,
    VK_ATTACHMENT_LOAD_OP_CLEAR,
    VK_ATTACHMENT_LOAD_OP_DONT_CARE
};

static VkAttachmentStoreOp RefreshToVK_StoreOp[] =
{
    VK_ATTACHMENT_STORE_OP_STORE,
    VK_ATTACHMENT_STORE_OP_DONT_CARE
};

static VkSampleCountFlagBits RefreshToVK_SampleCount[] =
{
    VK_SAMPLE_COUNT_1_BIT,
    VK_SAMPLE_COUNT_2_BIT,
    VK_SAMPLE_COUNT_4_BIT,
    VK_SAMPLE_COUNT_8_BIT,
    VK_SAMPLE_COUNT_16_BIT,
    VK_SAMPLE_COUNT_32_BIT,
    VK_SAMPLE_COUNT_64_BIT
};

static VkVertexInputRate RefreshToVK_VertexInputRate[] =
{
	VK_VERTEX_INPUT_RATE_VERTEX,
	VK_VERTEX_INPUT_RATE_INSTANCE
};

static VkFilter RefreshToVK_SamplerFilter[] =
{
	VK_FILTER_NEAREST,
	VK_FILTER_LINEAR
};

static VkSamplerMipmapMode RefreshToVK_SamplerMipmapMode[] =
{
	VK_SAMPLER_MIPMAP_MODE_NEAREST,
	VK_SAMPLER_MIPMAP_MODE_LINEAR
};

static VkSamplerAddressMode RefreshToVK_SamplerAddressMode[] =
{
	VK_SAMPLER_ADDRESS_MODE_REPEAT,
	VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
	VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
};

static VkBorderColor RefreshToVK_BorderColor[] =
{
	VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
	VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,
	VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	VK_BORDER_COLOR_INT_OPAQUE_BLACK,
	VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
	VK_BORDER_COLOR_INT_OPAQUE_WHITE
};

/* Structures */

/* Memory Allocation */

typedef struct VulkanMemoryAllocation VulkanMemoryAllocation;

typedef struct VulkanMemoryFreeRegion
{
	VulkanMemoryAllocation *allocation;
	VkDeviceSize offset;
	VkDeviceSize size;
	uint32_t allocationIndex;
	uint32_t sortedIndex;
} VulkanMemoryFreeRegion;

typedef struct VulkanMemorySubAllocator
{
	VkDeviceSize nextAllocationSize;
	VulkanMemoryAllocation **allocations;
	uint32_t allocationCount;
	VulkanMemoryFreeRegion **sortedFreeRegions;
	uint32_t sortedFreeRegionCount;
	uint32_t sortedFreeRegionCapacity;
} VulkanMemorySubAllocator;

struct VulkanMemoryAllocation
{
	VulkanMemorySubAllocator *allocator;
	VkDeviceMemory memory;
	VkDeviceSize size;
	VulkanMemoryFreeRegion **freeRegions;
	uint32_t freeRegionCount;
	uint32_t freeRegionCapacity;
	uint8_t dedicated;
};

typedef struct VulkanMemoryAllocator
{
	VulkanMemorySubAllocator subAllocators[VK_MAX_MEMORY_TYPES];
} VulkanMemoryAllocator;

/* Memory Barriers */

typedef struct VulkanResourceAccessInfo
{
	VkPipelineStageFlags stageMask;
	VkAccessFlags accessMask;
	VkImageLayout imageLayout;
} VulkanResourceAccessInfo;

static const VulkanResourceAccessInfo AccessMap[RESOURCE_ACCESS_TYPES_COUNT] =
{
	/* RESOURCE_ACCESS_NONE */
	{
		0,
		0,
		VK_IMAGE_LAYOUT_UNDEFINED
	},

	/* RESOURCE_ACCESS_INDEX_BUFFER */
	{
		VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
		VK_ACCESS_INDEX_READ_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED
	},

	/* RESOURCE_ACCESS_VERTEX_BUFFER */
	{
		VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
		VK_ACCESS_INDEX_READ_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED
	},

	/* RESOURCE_ACCESS_VERTEX_SHADER_READ_UNIFORM_BUFFER */
	{
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED
	},

	/* RESOURCE_ACCESS_VERTEX_SHADER_READ_SAMPLED_IMAGE */
	{
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	},

	/* RESOURCE_ACCESS_FRAGMENT_SHADER_READ_UNIFORM_BUFFER */
	{
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_UNIFORM_READ_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED
	},

	/* RESOURCE_ACCESS_FRAGMENT_SHADER_READ_SAMPLED_IMAGE */
	{
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	},

	/* RESOURCE_ACCESS_FRAGMENT_SHADER_READ_COLOR_ATTACHMENT */
	{
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	},

	/* RESOURCE_ACCESS_FRAGMENT_SHADER_READ_DEPTH_STENCIL_ATTACHMENT */
	{
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
	},

	/* RESOURCE_ACCESS_ANY_SHADER_READ_SAMPLED_IMAGE */
	{
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	},

	/* RESOURCE_ACCESS_COLOR_ATTACHMENT_READ */
	{
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	},

	/* RESOURCE_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ */
	{
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
	},

	/* RESOURCE_ACCESS_TRANSFER_READ */
	{
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	},

	/* RESOURCE_ACCESS_HOST_READ */
	{
		VK_PIPELINE_STAGE_HOST_BIT,
		VK_ACCESS_HOST_READ_BIT,
		VK_IMAGE_LAYOUT_GENERAL
	},

	/* RESOURCE_ACCESS_PRESENT */
	{
		0,
		0,
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	},

	/* RESOURCE_ACCESS_END_OF_READ */
	{
		0,
		0,
		VK_IMAGE_LAYOUT_UNDEFINED
	},

	/* RESOURCE_ACCESS_VERTEX_SHADER_WRITE */
	{
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_IMAGE_LAYOUT_GENERAL
	},

	/* RESOURCE_ACCESS_FRAGMENT_SHADER_WRITE */
	{
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_IMAGE_LAYOUT_GENERAL
	},

	/* RESOURCE_ACCESS_COLOR_ATTACHMENT_WRITE */
	{
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	},

	/* RESOURCE_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE */
	{
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	},

	/* RESOURCE_ACCESS_TRANSFER_WRITE */
	{
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	},

	/* RESOURCE_ACCESS_HOST_WRITE */
	{
		VK_PIPELINE_STAGE_HOST_BIT,
		VK_ACCESS_HOST_WRITE_BIT,
		VK_IMAGE_LAYOUT_GENERAL
	},

	/* RESOURCE_ACCESS_COLOR_ATTACHMENT_READ_WRITE */
	{
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	},

	/* RESOURCE_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_WRITE */
	{
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	},

	/* RESOURCE_ACCESS_MEMORY_TRANSFER_READ_WRITE */
	{
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED
	},

	/* RESOURCE_ACCESS_GENERAL */
	{
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		VK_IMAGE_LAYOUT_GENERAL
	}
};

/* Memory structures */

typedef struct VulkanBuffer VulkanBuffer;

typedef struct VulkanSubBuffer
{
	VulkanMemoryAllocation *allocation;
	VkBuffer buffer;
	VkDeviceSize offset;
	VkDeviceSize size;
	VulkanResourceAccessType resourceAccessType;
	int8_t bound;
} VulkanSubBuffer;

/*
 * Our VulkanBuffer is actually a series of sub-buffers
 * so we can properly support updates while a frame is in flight
 * without needing a sync point
 */
struct VulkanBuffer /* cast from FNA3D_Buffer */
{
	VkDeviceSize size;
	VulkanSubBuffer **subBuffers;
	uint32_t subBufferCount;
	uint32_t currentSubBufferIndex;
	VulkanResourceAccessType resourceAccessType;
	VkBufferUsageFlags usage;
	uint8_t bound;
	uint8_t boundSubmitted;
};

/* Renderer Structure */

typedef struct QueueFamilyIndices
{
	uint32_t graphicsFamily;
	uint32_t presentFamily;
} QueueFamilyIndices;

typedef struct SwapChainSupportDetails
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR *formats;
	uint32_t formatsLength;
	VkPresentModeKHR *presentModes;
	uint32_t presentModesLength;
} SwapChainSupportDetails;

typedef struct SamplerDescriptorSetCache SamplerDescriptorSetCache;

typedef struct VulkanGraphicsPipelineLayout
{
	VkPipelineLayout pipelineLayout;
	SamplerDescriptorSetCache *vertexSamplerDescriptorSetCache;
	SamplerDescriptorSetCache *fragmentSamplerDescriptorSetCache;
} VulkanGraphicsPipelineLayout;

typedef struct VulkanGraphicsPipeline
{
	VkPipeline pipeline;
	VulkanGraphicsPipelineLayout *pipelineLayout;
	REFRESH_PrimitiveType primitiveType;
	VkDescriptorSet vertexSamplerDescriptorSet; /* updated by SetVertexSamplers */
	VkDescriptorSet fragmentSamplerDescriptorSet; /* updated by SetFragmentSamplers */

	VkDescriptorSet vertexUBODescriptorSet; /* permanently set in Create function */
	VkDescriptorSet fragmentUBODescriptorSet; /* permanently set in Create function */
	VkDeviceSize vertexUBOBlockSize; /* permanently set in Create function */
	VkDeviceSize fragmentUBOBlockSize; /* permantenly set in Create function */
} VulkanGraphicsPipeline;

typedef struct VulkanTexture
{
	VulkanMemoryAllocation *allocation;
	VkDeviceSize offset;
	VkDeviceSize memorySize;

	VkImage image;
	VkImageView view;
	VkExtent2D dimensions;
	uint32_t depth;
	uint32_t layerCount;
	uint32_t levelCount;
	VkFormat format;
	REFRESH_SurfaceFormat refreshFormat;
	VulkanResourceAccessType resourceAccessType;
	REFRESH_TextureUsageFlags usageFlags;
	REFRESHNAMELESS union
	{
		REFRESH_SurfaceFormat colorFormat;
		REFRESH_DepthFormat depthStencilFormat;
	};
} VulkanTexture;

typedef struct VulkanColorTarget
{
	VulkanTexture *texture;
	uint32_t layer;
	VkImageView view;
	VulkanTexture *multisampleTexture;
	VkSampleCountFlags multisampleCount;
} VulkanColorTarget;

typedef struct VulkanDepthStencilTarget
{
	VulkanTexture *texture;
	VkImageView view;
} VulkanDepthStencilTarget;

typedef struct VulkanFramebuffer
{
	VkFramebuffer framebuffer;
	VulkanColorTarget *colorTargets[MAX_COLOR_TARGET_BINDINGS];
	uint32_t colorTargetCount;
	VulkanDepthStencilTarget *depthStencilTarget;
	uint32_t width;
	uint32_t height;
} VulkanFramebuffer;

/* Cache structures */

typedef struct SamplerDescriptorSetLayoutHash
{
	VkDescriptorType descriptorType;
	uint32_t samplerBindingCount;
	VkShaderStageFlagBits stageFlag;
} SamplerDescriptorSetLayoutHash;

typedef struct SamplerDescriptorSetLayoutHashMap
{
	SamplerDescriptorSetLayoutHash key;
	VkDescriptorSetLayout value;
} SamplerDescriptorSetLayoutHashMap;

typedef struct SamplerDescriptorSetLayoutHashArray
{
	SamplerDescriptorSetLayoutHashMap *elements;
	int32_t count;
	int32_t capacity;
} SamplerDescriptorSetLayoutHashArray;

#define NUM_DESCRIPTOR_SET_LAYOUT_BUCKETS 1031

typedef struct SamplerDescriptorSetLayoutHashTable
{
	SamplerDescriptorSetLayoutHashArray buckets[NUM_DESCRIPTOR_SET_LAYOUT_BUCKETS];
} SamplerDescriptorSetLayoutHashTable;

static inline uint64_t SamplerDescriptorSetLayoutHashTable_GetHashCode(SamplerDescriptorSetLayoutHash key)
{
	const uint64_t HASH_FACTOR = 97;
	uint64_t result = 1;
	result = result * HASH_FACTOR + key.descriptorType;
	result = result * HASH_FACTOR + key.samplerBindingCount;
	result = result * HASH_FACTOR + key.stageFlag;
	return result;
}

static inline VkDescriptorSetLayout SamplerDescriptorSetLayoutHashTable_Fetch(
	SamplerDescriptorSetLayoutHashTable *table,
	SamplerDescriptorSetLayoutHash key
) {
	int32_t i;
	uint64_t hashcode = SamplerDescriptorSetLayoutHashTable_GetHashCode(key);
	SamplerDescriptorSetLayoutHashArray *arr = &table->buckets[hashcode % NUM_DESCRIPTOR_SET_LAYOUT_BUCKETS];

	for (i = 0; i < arr->count; i += 1)
	{
		const SamplerDescriptorSetLayoutHash *e = &arr->elements[i].key;
		if (    key.descriptorType == e->descriptorType &&
			key.samplerBindingCount == e->samplerBindingCount &&
			key.stageFlag == e->stageFlag   )
		{
			return arr->elements[i].value;
		}
	}

	return VK_NULL_HANDLE;
}

static inline void SamplerDescriptorSetLayoutHashTable_Insert(
	SamplerDescriptorSetLayoutHashTable *table,
	SamplerDescriptorSetLayoutHash key,
	VkDescriptorSetLayout value
) {
	uint64_t hashcode = SamplerDescriptorSetLayoutHashTable_GetHashCode(key);
	SamplerDescriptorSetLayoutHashArray *arr = &table->buckets[hashcode % NUM_DESCRIPTOR_SET_LAYOUT_BUCKETS];

	SamplerDescriptorSetLayoutHashMap map;
	map.key = key;
	map.value = value;

	EXPAND_ELEMENTS_IF_NEEDED(arr, 4, SamplerDescriptorSetLayoutHashMap);

	arr->elements[arr->count] = map;
	arr->count += 1;
}

typedef struct PipelineLayoutHash
{
	VkDescriptorSetLayout vertexSamplerLayout;
	VkDescriptorSetLayout fragmentSamplerLayout;
	VkDescriptorSetLayout vertexUniformLayout;
	VkDescriptorSetLayout fragmentUniformLayout;
} PipelineLayoutHash;

typedef struct PipelineLayoutHashMap
{
	PipelineLayoutHash key;
	VulkanGraphicsPipelineLayout *value;
} PipelineLayoutHashMap;

typedef struct PipelineLayoutHashArray
{
	PipelineLayoutHashMap *elements;
	int32_t count;
	int32_t capacity;
} PipelineLayoutHashArray;

#define NUM_PIPELINE_LAYOUT_BUCKETS 1031

typedef struct PipelineLayoutHashTable
{
	PipelineLayoutHashArray buckets[NUM_PIPELINE_LAYOUT_BUCKETS];
} PipelineLayoutHashTable;

static inline uint64_t PipelineLayoutHashTable_GetHashCode(PipelineLayoutHash key)
{
	const uint64_t HASH_FACTOR = 97;
	uint64_t result = 1;
	result = result * HASH_FACTOR + (uint64_t) key.vertexSamplerLayout;
	result = result * HASH_FACTOR + (uint64_t) key.fragmentSamplerLayout;
	result = result * HASH_FACTOR + (uint64_t) key.vertexUniformLayout;
	result = result * HASH_FACTOR + (uint64_t) key.fragmentUniformLayout;
	return result;
}

static inline VulkanGraphicsPipelineLayout* PipelineLayoutHashArray_Fetch(
	PipelineLayoutHashTable *table,
	PipelineLayoutHash key
) {
	int32_t i;
	uint64_t hashcode = PipelineLayoutHashTable_GetHashCode(key);
	PipelineLayoutHashArray *arr = &table->buckets[hashcode % NUM_PIPELINE_LAYOUT_BUCKETS];

	for (i = 0; i < arr->count; i += 1)
	{
		const PipelineLayoutHash *e = &arr->elements[i].key;
		if (	key.vertexSamplerLayout == e->vertexSamplerLayout &&
			key.fragmentSamplerLayout == e->fragmentSamplerLayout &&
			key.vertexUniformLayout == e->vertexUniformLayout &&
			key.fragmentUniformLayout == e->fragmentUniformLayout	)
		{
			return arr->elements[i].value;
		}
	}

	return NULL;
}

static inline void PipelineLayoutHashArray_Insert(
	PipelineLayoutHashTable *table,
	PipelineLayoutHash key,
	VulkanGraphicsPipelineLayout *value
) {
	uint64_t hashcode = PipelineLayoutHashTable_GetHashCode(key);
	PipelineLayoutHashArray *arr = &table->buckets[hashcode % NUM_PIPELINE_LAYOUT_BUCKETS];

	PipelineLayoutHashMap map;
	map.key = key;
	map.value = value;

	EXPAND_ELEMENTS_IF_NEEDED(arr, 4, PipelineLayoutHashMap)

	arr->elements[arr->count] = map;
	arr->count += 1;
}

typedef struct SamplerDescriptorSetData
{
	VkDescriptorImageInfo descriptorImageInfo[MAX_TEXTURE_SAMPLERS]; /* used for vertex samplers as well */
} SamplerDescriptorSetData;

typedef struct SamplerDescriptorSetHashMap
{
	uint64_t key;
	SamplerDescriptorSetData descriptorSetData;
	VkDescriptorSet descriptorSet;
	uint8_t inactiveFrameCount;
} SamplerDescriptorSetHashMap;

typedef struct SamplerDescriptorSetHashArray
{
	uint32_t *elements;
	int32_t count;
	int32_t capacity;
} SamplerDescriptorSetHashArray;

#define NUM_DESCRIPTOR_SET_HASH_BUCKETS 1031

static inline uint64_t SamplerDescriptorSetHashTable_GetHashCode(
	SamplerDescriptorSetData *descriptorSetData,
	uint32_t samplerCount
) {
	const uint64_t HASH_FACTOR = 97;
	uint32_t i;
	uint64_t result = 1;

	for (i = 0; i < samplerCount; i++)
	{
		result = result * HASH_FACTOR + (uint64_t) descriptorSetData->descriptorImageInfo[i].imageView;
		result = result * HASH_FACTOR + (uint64_t) descriptorSetData->descriptorImageInfo[i].sampler;
	}

	return result;
}

struct SamplerDescriptorSetCache
{
	VkDescriptorSetLayout descriptorSetLayout;
	uint32_t samplerBindingCount;

	SamplerDescriptorSetHashArray buckets[NUM_DESCRIPTOR_SET_HASH_BUCKETS]; /* these buckets store indices */
	SamplerDescriptorSetHashMap *elements; /* where the hash map elements are stored */
	uint32_t count;
	uint32_t capacity;

	VkDescriptorPool *samplerDescriptorPools;
	uint32_t samplerDescriptorPoolCount;
	uint32_t nextPoolSize;

	VkDescriptorSet *inactiveDescriptorSets;
	uint32_t inactiveDescriptorSetCount;
	uint32_t inactiveDescriptorSetCapacity;
};

/* Context */

typedef struct VulkanRenderer
{
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceProperties2 physicalDeviceProperties;
    VkPhysicalDeviceDriverPropertiesKHR physicalDeviceDriverProperties;
    VkDevice logicalDevice;

    void* deviceWindowHandle;

    uint8_t supportsDebugUtils;
    uint8_t debugMode;
    uint8_t headless;

	VulkanMemoryAllocator *memoryAllocator;

    REFRESH_PresentMode presentMode;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapChain;
    VkFormat swapChainFormat;
    VkComponentMapping swapChainSwizzle;
    VkImage *swapChainImages;
    VkImageView *swapChainImageViews;
    VulkanResourceAccessType *swapChainResourceAccessTypes;
    uint32_t swapChainImageCount;
    VkExtent2D swapChainExtent;

	uint8_t needNewSwapChain;
	uint8_t shouldPresent;
	uint8_t swapChainImageAcquired;
	uint32_t currentSwapChainIndex;

    QueueFamilyIndices queueFamilyIndices;
	VkQueue graphicsQueue;
	VkQueue presentQueue;

	VkFence inFlightFence;
	VkSemaphore imageAvailableSemaphore;
	VkSemaphore renderFinishedSemaphore;

	VkCommandPool commandPool;
	VkCommandBuffer *inactiveCommandBuffers;
	VkCommandBuffer *activeCommandBuffers;
	VkCommandBuffer *submittedCommandBuffers;
	uint32_t inactiveCommandBufferCount;
	uint32_t activeCommandBufferCount;
	uint32_t submittedCommandBufferCount;
	uint32_t allocatedCommandBufferCount;
	uint32_t currentCommandCount;
	VkCommandBuffer currentCommandBuffer;
	uint32_t numActiveCommands;

	VulkanGraphicsPipeline *currentGraphicsPipeline;
	VulkanFramebuffer *currentFramebuffer;

	SamplerDescriptorSetLayoutHashTable samplerDescriptorSetLayoutHashTable;
	PipelineLayoutHashTable pipelineLayoutHashTable;

	/*
	 * TODO: we can get rid of this reference when we
	 * come up with a clever descriptor set reuse system
	 */
	VkDescriptorPool *descriptorPools;
	uint32_t descriptorPoolCount;

	/* initialize baseline descriptor info */
	VkDescriptorPool defaultDescriptorPool;
	VkDescriptorSetLayout emptyVertexSamplerLayout;
	VkDescriptorSetLayout emptyFragmentSamplerLayout;
	VkDescriptorSet emptyVertexSamplerDescriptorSet;
	VkDescriptorSet emptyFragmentSamplerDescriptorSet;

	VkDescriptorSetLayout vertexParamLayout;
	VkDescriptorSetLayout fragmentParamLayout;
	VulkanBuffer *dummyVertexUniformBuffer;
	VulkanBuffer *dummyFragmentUniformBuffer;

	VulkanBuffer *textureStagingBuffer;

	VulkanBuffer** buffersInUse;
	uint32_t buffersInUseCount;
	uint32_t buffersInUseCapacity;

	VulkanBuffer** submittedBuffers;
	uint32_t submittedBufferCount;
	uint32_t submittedBufferCapacity;

	VulkanBuffer *vertexUBO;
	VulkanBuffer *fragmentUBO;
	uint32_t minUBOAlignment;

	uint32_t vertexUBOOffset;
	VkDeviceSize vertexUBOBlockIncrement;

	uint32_t fragmentUBOOffset;
	VkDeviceSize fragmentUBOBlockIncrement;

	uint32_t frameIndex;

	SDL_mutex *allocatorLock;
	SDL_mutex *commandLock;
	SDL_mutex *disposeLock;

	/* Deferred destroy storage */

	VulkanColorTarget **colorTargetsToDestroy;
	uint32_t colorTargetsToDestroyCount;
	uint32_t colorTargetsToDestroyCapacity;

	VulkanColorTarget **submittedColorTargetsToDestroy;
	uint32_t submittedColorTargetsToDestroyCount;
	uint32_t submittedColorTargetsToDestroyCapacity;

	VulkanDepthStencilTarget **depthStencilTargetsToDestroy;
	uint32_t depthStencilTargetsToDestroyCount;
	uint32_t depthStencilTargetsToDestroyCapacity;

	VulkanDepthStencilTarget **submittedDepthStencilTargetsToDestroy;
	uint32_t submittedDepthStencilTargetsToDestroyCount;
	uint32_t submittedDepthStencilTargetsToDestroyCapacity;

	VulkanTexture **texturesToDestroy;
	uint32_t texturesToDestroyCount;
	uint32_t texturesToDestroyCapacity;

	VulkanTexture **submittedTexturesToDestroy;
	uint32_t submittedTexturesToDestroyCount;
	uint32_t submittedTexturesToDestroyCapacity;

	VulkanBuffer **buffersToDestroy;
	uint32_t buffersToDestroyCount;
	uint32_t buffersToDestroyCapacity;

	VulkanBuffer **submittedBuffersToDestroy;
	uint32_t submittedBuffersToDestroyCount;
	uint32_t submittedBuffersToDestroyCapacity;

	VulkanGraphicsPipeline **graphicsPipelinesToDestroy;
	uint32_t graphicsPipelinesToDestroyCount;
	uint32_t graphicsPipelinesToDestroyCapacity;

	VulkanGraphicsPipeline **submittedGraphicsPipelinesToDestroy;
	uint32_t submittedGraphicsPipelinesToDestroyCount;
	uint32_t submittedGraphicsPipelinesToDestroyCapacity;

	VkShaderModule *shaderModulesToDestroy;
	uint32_t shaderModulesToDestroyCount;
	uint32_t shaderModulesToDestroyCapacity;

	VkShaderModule *submittedShaderModulesToDestroy;
	uint32_t submittedShaderModulesToDestroyCount;
	uint32_t submittedShaderModulesToDestroyCapacity;

	VkSampler *samplersToDestroy;
	uint32_t samplersToDestroyCount;
	uint32_t samplersToDestroyCapacity;

	VkSampler *submittedSamplersToDestroy;
	uint32_t submittedSamplersToDestroyCount;
	uint32_t submittedSamplersToDestroyCapacity;

	VulkanFramebuffer **framebuffersToDestroy;
	uint32_t framebuffersToDestroyCount;
	uint32_t framebuffersToDestroyCapacity;

	VulkanFramebuffer **submittedFramebuffersToDestroy;
	uint32_t submittedFramebuffersToDestroyCount;
	uint32_t submittedFramebuffersToDestroyCapacity;

	VkRenderPass *renderPassesToDestroy;
	uint32_t renderPassesToDestroyCount;
	uint32_t renderPassesToDestroyCapacity;

	VkRenderPass *submittedRenderPassesToDestroy;
	uint32_t submittedRenderPassesToDestroyCount;
	uint32_t submittedRenderPassesToDestroyCapacity;

    #define VULKAN_INSTANCE_FUNCTION(ext, ret, func, params) \
		vkfntype_##func func;
	#define VULKAN_DEVICE_FUNCTION(ext, ret, func, params) \
		vkfntype_##func func;
	#include "Refresh_Driver_Vulkan_vkfuncs.h"
} VulkanRenderer;

/* Forward declarations */

static void VULKAN_INTERNAL_BeginCommandBuffer(VulkanRenderer *renderer);
static void VULKAN_Submit(REFRESH_Renderer* driverData);

/* Macros */

#define RECORD_CMD(cmdCall)					\
	SDL_LockMutex(renderer->commandLock);			\
	if (renderer->currentCommandBuffer == NULL)		\
	{							\
		VULKAN_INTERNAL_BeginCommandBuffer(renderer);	\
	}							\
	cmdCall;						\
	renderer->numActiveCommands += 1;			\
	SDL_UnlockMutex(renderer->commandLock);

/* Error Handling */

static inline const char* VkErrorMessages(VkResult code)
{
	#define ERR_TO_STR(e) \
		case e: return #e;
	switch (code)
	{
		ERR_TO_STR(VK_ERROR_OUT_OF_HOST_MEMORY)
		ERR_TO_STR(VK_ERROR_OUT_OF_DEVICE_MEMORY)
		ERR_TO_STR(VK_ERROR_FRAGMENTED_POOL)
		ERR_TO_STR(VK_ERROR_OUT_OF_POOL_MEMORY)
		ERR_TO_STR(VK_ERROR_INITIALIZATION_FAILED)
		ERR_TO_STR(VK_ERROR_LAYER_NOT_PRESENT)
		ERR_TO_STR(VK_ERROR_EXTENSION_NOT_PRESENT)
		ERR_TO_STR(VK_ERROR_FEATURE_NOT_PRESENT)
		ERR_TO_STR(VK_ERROR_TOO_MANY_OBJECTS)
		ERR_TO_STR(VK_ERROR_DEVICE_LOST)
		ERR_TO_STR(VK_ERROR_INCOMPATIBLE_DRIVER)
		ERR_TO_STR(VK_ERROR_OUT_OF_DATE_KHR)
		ERR_TO_STR(VK_ERROR_SURFACE_LOST_KHR)
		ERR_TO_STR(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
		ERR_TO_STR(VK_SUBOPTIMAL_KHR)
		default: return "Unhandled VkResult!";
	}
	#undef ERR_TO_STR
}

static inline void LogVulkanResult(
	const char* vulkanFunctionName,
	VkResult result
) {
	if (result != VK_SUCCESS)
	{
		REFRESH_LogError(
			"%s: %s",
			vulkanFunctionName,
			VkErrorMessages(result)
		);
	}
}

/* Utility */

static inline uint8_t DepthFormatContainsStencil(VkFormat format)
{
	switch(format)
	{
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D32_SFLOAT:
			return 0;
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return 1;
		default:
			SDL_assert(0 && "Invalid depth format");
			return 0;
	}
}

/* Memory Management */

static inline VkDeviceSize VULKAN_INTERNAL_NextHighestAlignment(
	VkDeviceSize n,
	VkDeviceSize align
) {
	return align * ((n + align - 1) / align);
}

static VulkanMemoryFreeRegion* VULKAN_INTERNAL_NewMemoryFreeRegion(
	VulkanMemoryAllocation *allocation,
	VkDeviceSize offset,
	VkDeviceSize size
) {
	VulkanMemoryFreeRegion *newFreeRegion;
	uint32_t insertionIndex = 0;
	uint32_t i;

	/* TODO: an improvement here could be to merge contiguous free regions */
	allocation->freeRegionCount += 1;
	if (allocation->freeRegionCount > allocation->freeRegionCapacity)
	{
		allocation->freeRegionCapacity *= 2;
		allocation->freeRegions = SDL_realloc(
			allocation->freeRegions,
			sizeof(VulkanMemoryFreeRegion*) * allocation->freeRegionCapacity
		);
	}

	newFreeRegion = SDL_malloc(sizeof(VulkanMemoryFreeRegion));
	newFreeRegion->offset = offset;
	newFreeRegion->size = size;
	newFreeRegion->allocation = allocation;

	allocation->freeRegions[allocation->freeRegionCount - 1] = newFreeRegion;
	newFreeRegion->allocationIndex = allocation->freeRegionCount - 1;

	for (i = 0; i < allocation->allocator->sortedFreeRegionCount; i += 1)
	{
		if (allocation->allocator->sortedFreeRegions[i]->size < size)
		{
			/* this is where the new region should go */
			break;
		}

		insertionIndex += 1;
	}

	if (allocation->allocator->sortedFreeRegionCount + 1 > allocation->allocator->sortedFreeRegionCapacity)
	{
		allocation->allocator->sortedFreeRegionCapacity *= 2;
		allocation->allocator->sortedFreeRegions = SDL_realloc(
			allocation->allocator->sortedFreeRegions,
			sizeof(VulkanMemoryFreeRegion*) * allocation->allocator->sortedFreeRegionCapacity
		);
	}

	/* perform insertion sort */
	if (allocation->allocator->sortedFreeRegionCount > 0 && insertionIndex != allocation->allocator->sortedFreeRegionCount)
	{
		for (i = allocation->allocator->sortedFreeRegionCount; i > insertionIndex && i > 0; i -= 1)
		{
			allocation->allocator->sortedFreeRegions[i] = allocation->allocator->sortedFreeRegions[i - 1];
			allocation->allocator->sortedFreeRegions[i]->sortedIndex = i;
		}
	}

	allocation->allocator->sortedFreeRegionCount += 1;
	allocation->allocator->sortedFreeRegions[insertionIndex] = newFreeRegion;
	newFreeRegion->sortedIndex = insertionIndex;

	return newFreeRegion;
}

static void VULKAN_INTERNAL_RemoveMemoryFreeRegion(
	VulkanMemoryFreeRegion *freeRegion
) {
	uint32_t i;

	/* close the gap in the sorted list */
	if (freeRegion->allocation->allocator->sortedFreeRegionCount > 1)
	{
		for (i = freeRegion->sortedIndex; i < freeRegion->allocation->allocator->sortedFreeRegionCount - 1; i += 1)
		{
			freeRegion->allocation->allocator->sortedFreeRegions[i] =
				freeRegion->allocation->allocator->sortedFreeRegions[i + 1];

			freeRegion->allocation->allocator->sortedFreeRegions[i]->sortedIndex = i;
		}
	}

	freeRegion->allocation->allocator->sortedFreeRegionCount -= 1;

	/* close the gap in the buffer list */
	if (freeRegion->allocation->freeRegionCount > 1 && freeRegion->allocationIndex != freeRegion->allocation->freeRegionCount - 1)
	{
		freeRegion->allocation->freeRegions[freeRegion->allocationIndex] =
			freeRegion->allocation->freeRegions[freeRegion->allocation->freeRegionCount - 1];

		freeRegion->allocation->freeRegions[freeRegion->allocationIndex]->allocationIndex =
			freeRegion->allocationIndex;
	}

	freeRegion->allocation->freeRegionCount -= 1;

	SDL_free(freeRegion);
}

static uint8_t VULKAN_INTERNAL_FindMemoryType(
	VulkanRenderer *renderer,
	uint32_t typeFilter,
	VkMemoryPropertyFlags properties,
	uint32_t *result
) {
	VkPhysicalDeviceMemoryProperties memoryProperties;
	uint32_t i;

	renderer->vkGetPhysicalDeviceMemoryProperties(
		renderer->physicalDevice,
		&memoryProperties
	);

	for (i = 0; i < memoryProperties.memoryTypeCount; i += 1)
	{
		if (	(typeFilter & (1 << i)) &&
			(memoryProperties.memoryTypes[i].propertyFlags & properties) == properties	)
		{
			*result = i;
			return 1;
		}
	}

	REFRESH_LogError("Failed to find memory properties %X, filter %X", properties, typeFilter);
	return 0;
}

static uint8_t VULKAN_INTERNAL_FindBufferMemoryRequirements(
	VulkanRenderer *renderer,
	VkBuffer buffer,
	VkMemoryRequirements2KHR *pMemoryRequirements,
	uint32_t *pMemoryTypeIndex
) {
	VkBufferMemoryRequirementsInfo2KHR bufferRequirementsInfo;
	bufferRequirementsInfo.sType =
		VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2_KHR;
	bufferRequirementsInfo.pNext = NULL;
	bufferRequirementsInfo.buffer = buffer;

	renderer->vkGetBufferMemoryRequirements2KHR(
		renderer->logicalDevice,
		&bufferRequirementsInfo,
		pMemoryRequirements
	);

	if (!VULKAN_INTERNAL_FindMemoryType(
		renderer,
		pMemoryRequirements->memoryRequirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		pMemoryTypeIndex
	)) {
		REFRESH_LogError(
			"Could not find valid memory type for buffer creation"
		);
		return 0;
	}

	return 1;
}

static uint8_t VULKAN_INTERNAL_FindImageMemoryRequirements(
	VulkanRenderer *renderer,
	VkImage image,
	VkMemoryRequirements2KHR *pMemoryRequirements,
	uint32_t *pMemoryTypeIndex
) {
	VkImageMemoryRequirementsInfo2KHR imageRequirementsInfo;
	imageRequirementsInfo.sType =
		VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2_KHR;
	imageRequirementsInfo.pNext = NULL;
	imageRequirementsInfo.image = image;

	renderer->vkGetImageMemoryRequirements2KHR(
		renderer->logicalDevice,
		&imageRequirementsInfo,
		pMemoryRequirements
	);

	if (!VULKAN_INTERNAL_FindMemoryType(
		renderer,
		pMemoryRequirements->memoryRequirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		pMemoryTypeIndex
	)) {
		REFRESH_LogError(
			"Could not find valid memory type for image creation"
		);
		return 0;
	}

	return 1;
}

static uint8_t VULKAN_INTERNAL_AllocateMemory(
	VulkanRenderer *renderer,
	VkBuffer buffer,
	VkImage image,
	uint32_t memoryTypeIndex,
	VkDeviceSize allocationSize,
	uint8_t dedicated,
	VulkanMemoryAllocation **pMemoryAllocation
) {
	VulkanMemoryAllocation *allocation;
	VulkanMemorySubAllocator *allocator = &renderer->memoryAllocator->subAllocators[memoryTypeIndex];
	VkMemoryAllocateInfo allocInfo;
	VkMemoryDedicatedAllocateInfoKHR dedicatedInfo;
	VkResult result;

	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.memoryTypeIndex = memoryTypeIndex;
	allocInfo.allocationSize = allocationSize;

	allocation = SDL_malloc(sizeof(VulkanMemoryAllocation));
	allocation->size = allocationSize;

	if (dedicated)
	{
		dedicatedInfo.sType =
			VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
		dedicatedInfo.pNext = NULL;
		dedicatedInfo.buffer = buffer;
		dedicatedInfo.image = image;

		allocInfo.pNext = &dedicatedInfo;

		allocation->dedicated = 1;
	}
	else
	{
		allocInfo.pNext = NULL;

		/* allocate a non-dedicated texture buffer */
		allocator->allocationCount += 1;
		allocator->allocations = SDL_realloc(
			allocator->allocations,
			sizeof(VulkanMemoryAllocation*) * allocator->allocationCount
		);

		allocator->allocations[
			allocator->allocationCount - 1
		] = allocation;

		allocation->dedicated = 0;
	}

	allocation->freeRegions = SDL_malloc(sizeof(VulkanMemoryFreeRegion*));
	allocation->freeRegionCount = 0;
	allocation->freeRegionCapacity = 1;
	allocation->allocator = allocator;

	result = renderer->vkAllocateMemory(
		renderer->logicalDevice,
		&allocInfo,
		NULL,
		&allocation->memory
	);

	if (result != VK_SUCCESS)
	{
		LogVulkanResult("vkAllocateMemory", result);
		return 0;
	}

	VULKAN_INTERNAL_NewMemoryFreeRegion(
		allocation,
		0,
		allocation->size
	);

	*pMemoryAllocation = allocation;
	return 1;
}

static uint8_t VULKAN_INTERNAL_FindAvailableMemory(
	VulkanRenderer *renderer,
	VkBuffer buffer,
	VkImage image,
	VulkanMemoryAllocation **pMemoryAllocation,
	VkDeviceSize *pOffset,
	VkDeviceSize *pSize
) {
	VkMemoryDedicatedRequirementsKHR dedicatedRequirements =
	{
		VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR,
		NULL
	};
	VkMemoryRequirements2KHR memoryRequirements =
	{
		VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2_KHR,
		&dedicatedRequirements
	};
	uint32_t memoryTypeIndex;

	VulkanMemoryAllocation *allocation;
	VulkanMemorySubAllocator *allocator;
	VulkanMemoryFreeRegion *region;

	VkDeviceSize requiredSize, allocationSize;
	VkDeviceSize alignedOffset;
	VkDeviceSize newRegionSize, newRegionOffset;
	uint8_t allocationResult;

	if (buffer != VK_NULL_HANDLE && image != VK_NULL_HANDLE)
	{
		REFRESH_LogError("Calling FindAvailableMemory with both a buffer and image handle is invalid!");
		return 0;
	}
	else if (buffer != VK_NULL_HANDLE)
	{
		if (!VULKAN_INTERNAL_FindBufferMemoryRequirements(
			renderer,
			buffer,
			&memoryRequirements,
			&memoryTypeIndex
		)) {
			REFRESH_LogError("Failed to acquire buffer memory requirements!");
			return 0;
		}
	}
	else if (image != VK_NULL_HANDLE)
	{
		if (!VULKAN_INTERNAL_FindImageMemoryRequirements(
			renderer,
			image,
			&memoryRequirements,
			&memoryTypeIndex
		)) {
			REFRESH_LogError("Failed to acquire image memory requirements!");
			return 0;
		}
	}
	else
	{
		REFRESH_LogError("Calling FindAvailableMemory with neither buffer nor image handle is invalid!");
		return 0;
	}

	allocator = &renderer->memoryAllocator->subAllocators[memoryTypeIndex];
	requiredSize = memoryRequirements.memoryRequirements.size;

	SDL_LockMutex(renderer->allocatorLock);

	/* find the largest free region and use it */
	if (allocator->sortedFreeRegionCount > 0)
	{
		region = allocator->sortedFreeRegions[0];
		allocation = region->allocation;

		alignedOffset = VULKAN_INTERNAL_NextHighestAlignment(
			region->offset,
			memoryRequirements.memoryRequirements.alignment
		);

		if (alignedOffset + requiredSize <= region->offset + region->size)
		{
			*pMemoryAllocation = allocation;

			/* not aligned - create a new free region */
			if (region->offset != alignedOffset)
			{
				VULKAN_INTERNAL_NewMemoryFreeRegion(
					allocation,
					region->offset,
					alignedOffset - region->offset
				);
			}

			*pOffset = alignedOffset;
			*pSize = requiredSize;

			newRegionSize = region->size - ((alignedOffset - region->offset) + requiredSize);
			newRegionOffset = alignedOffset + requiredSize;

			/* remove and add modified region to re-sort */
			VULKAN_INTERNAL_RemoveMemoryFreeRegion(region);

			/* if size is 0, no need to re-insert */
			if (newRegionSize != 0)
			{
				VULKAN_INTERNAL_NewMemoryFreeRegion(
					allocation,
					newRegionOffset,
					newRegionSize
				);
			}

			SDL_UnlockMutex(renderer->allocatorLock);

			return 1;
		}
	}

	/* No suitable free regions exist, allocate a new memory region */

	if (dedicatedRequirements.prefersDedicatedAllocation || dedicatedRequirements.requiresDedicatedAllocation)
	{
		allocationSize = requiredSize;
	}
	else if (requiredSize > allocator->nextAllocationSize)
	{
		/* allocate a page of required size aligned to STARTING_ALLOCATION_SIZE increments */
		allocationSize =
			VULKAN_INTERNAL_NextHighestAlignment(requiredSize, STARTING_ALLOCATION_SIZE);
	}
	else
	{
		allocationSize = allocator->nextAllocationSize;
		allocator->nextAllocationSize = SDL_min(allocator->nextAllocationSize * 2, MAX_ALLOCATION_SIZE);
	}

	allocationResult = VULKAN_INTERNAL_AllocateMemory(
		renderer,
		buffer,
		image,
		memoryTypeIndex,
		allocationSize,
		dedicatedRequirements.prefersDedicatedAllocation || dedicatedRequirements.requiresDedicatedAllocation,
		&allocation
	);

	/* Uh oh, we're out of memory */
	if (allocationResult == 0)
	{
		/* Responsibility of the caller to handle being out of memory */
		REFRESH_LogWarn("Failed to allocate memory!");
		SDL_UnlockMutex(renderer->allocatorLock);

		return 2;
	}

	*pMemoryAllocation = allocation;
	*pOffset = 0;
	*pSize = requiredSize;

	region = allocation->freeRegions[0];

	newRegionOffset = region->offset + requiredSize;
	newRegionSize = region->size - requiredSize;

	VULKAN_INTERNAL_RemoveMemoryFreeRegion(region);

	if (newRegionSize != 0)
	{
		VULKAN_INTERNAL_NewMemoryFreeRegion(
			allocation,
			newRegionOffset,
			newRegionSize
		);
	}

	SDL_UnlockMutex(renderer->allocatorLock);

	return 1;
}

/* Memory Barriers */

static void VULKAN_INTERNAL_BufferMemoryBarrier(
	VulkanRenderer *renderer,
	VulkanResourceAccessType nextResourceAccessType,
	VulkanBuffer *buffer,
	VulkanSubBuffer *subBuffer
) {
	VkPipelineStageFlags srcStages = 0;
	VkPipelineStageFlags dstStages = 0;
	VkBufferMemoryBarrier memoryBarrier;
	VulkanResourceAccessType prevAccess, nextAccess;
	const VulkanResourceAccessInfo *prevAccessInfo, *nextAccessInfo;

	if (buffer->resourceAccessType == nextResourceAccessType)
	{
		return;
	}

	memoryBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	memoryBarrier.pNext = NULL;
	memoryBarrier.srcAccessMask = 0;
	memoryBarrier.dstAccessMask = 0;
	memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	memoryBarrier.buffer = subBuffer->buffer;
	memoryBarrier.offset = 0;
	memoryBarrier.size = buffer->size;

	prevAccess = buffer->resourceAccessType;
	prevAccessInfo = &AccessMap[prevAccess];

	srcStages |= prevAccessInfo->stageMask;

	if (prevAccess > RESOURCE_ACCESS_END_OF_READ)
	{
		memoryBarrier.srcAccessMask |= prevAccessInfo->accessMask;
	}

	nextAccess = nextResourceAccessType;
	nextAccessInfo = &AccessMap[nextAccess];

	dstStages |= nextAccessInfo->stageMask;

	if (memoryBarrier.srcAccessMask != 0)
	{
		memoryBarrier.dstAccessMask |= nextAccessInfo->accessMask;
	}

	if (srcStages == 0)
	{
		srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	}
	if (dstStages == 0)
	{
		dstStages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	}

	RECORD_CMD(renderer->vkCmdPipelineBarrier(
		renderer->currentCommandBuffer,
		srcStages,
		dstStages,
		0,
		0,
		NULL,
		1,
		&memoryBarrier,
		0,
		NULL
	));

	buffer->resourceAccessType = nextResourceAccessType;
}

static void VULKAN_INTERNAL_ImageMemoryBarrier(
	VulkanRenderer *renderer,
	VulkanResourceAccessType nextAccess,
	VkImageAspectFlags aspectMask,
	uint32_t baseLayer,
	uint32_t layerCount,
	uint32_t baseLevel,
	uint32_t levelCount,
	uint8_t discardContents,
	VkImage image,
	VulkanResourceAccessType *resourceAccessType
) {
	VkPipelineStageFlags srcStages = 0;
	VkPipelineStageFlags dstStages = 0;
	VkImageMemoryBarrier memoryBarrier;
	VulkanResourceAccessType prevAccess;
	const VulkanResourceAccessInfo *pPrevAccessInfo, *pNextAccessInfo;

	if (*resourceAccessType == nextAccess)
	{
		return;
	}

	memoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	memoryBarrier.pNext = NULL;
	memoryBarrier.srcAccessMask = 0;
	memoryBarrier.dstAccessMask = 0;
	memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	memoryBarrier.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	memoryBarrier.image = image;
	memoryBarrier.subresourceRange.aspectMask = aspectMask;
	memoryBarrier.subresourceRange.baseArrayLayer = baseLayer;
	memoryBarrier.subresourceRange.layerCount = layerCount;
	memoryBarrier.subresourceRange.baseMipLevel = baseLevel;
	memoryBarrier.subresourceRange.levelCount = levelCount;

	prevAccess = *resourceAccessType;
	pPrevAccessInfo = &AccessMap[prevAccess];

	srcStages |= pPrevAccessInfo->stageMask;

	if (prevAccess > RESOURCE_ACCESS_END_OF_READ)
	{
		memoryBarrier.srcAccessMask |= pPrevAccessInfo->accessMask;
	}

	if (discardContents)
	{
		memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}
	else
	{
		memoryBarrier.oldLayout = pPrevAccessInfo->imageLayout;
	}

	pNextAccessInfo = &AccessMap[nextAccess];

	dstStages |= pNextAccessInfo->stageMask;

	memoryBarrier.dstAccessMask |= pNextAccessInfo->accessMask;
	memoryBarrier.newLayout = pNextAccessInfo->imageLayout;

	if (srcStages == 0)
	{
		srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	}
	if (dstStages == 0)
	{
		dstStages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	}

	RECORD_CMD(renderer->vkCmdPipelineBarrier(
		renderer->currentCommandBuffer,
		srcStages,
		dstStages,
		0,
		0,
		NULL,
		0,
		NULL,
		1,
		&memoryBarrier
	));

	*resourceAccessType = nextAccess;
}

/* Resource Disposal */

static void VULKAN_INTERNAL_RemoveBuffer(
	REFRESH_Renderer *driverData,
	REFRESH_Buffer *buffer
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanBuffer *vulkanBuffer = (VulkanBuffer*) buffer;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->buffersToDestroy,
		VulkanBuffer*,
		renderer->buffersToDestroyCount + 1,
		renderer->buffersToDestroyCapacity,
		renderer->buffersToDestroyCapacity * 2
	)

	renderer->buffersToDestroy[
		renderer->buffersToDestroyCount
	] = vulkanBuffer;
	renderer->buffersToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_INTERNAL_DestroyTexture(
	VulkanRenderer* renderer,
	VulkanTexture* texture
) {
	if (texture->allocation->dedicated)
	{
		renderer->vkFreeMemory(
			renderer->logicalDevice,
			texture->allocation->memory,
			NULL
		);

		SDL_free(texture->allocation->freeRegions);
		SDL_free(texture->allocation);
	}
	else
	{
		SDL_LockMutex(renderer->allocatorLock);

		VULKAN_INTERNAL_NewMemoryFreeRegion(
			texture->allocation,
			texture->offset,
			texture->memorySize
		);

		SDL_UnlockMutex(renderer->allocatorLock);
	}

	renderer->vkDestroyImageView(
		renderer->logicalDevice,
		texture->view,
		NULL
	);

	renderer->vkDestroyImage(
		renderer->logicalDevice,
		texture->image,
		NULL
	);

	SDL_free(texture);
}

static void VULKAN_INTERNAL_DestroyColorTarget(
	VulkanRenderer *renderer,
	VulkanColorTarget *colorTarget
) {
	renderer->vkDestroyImageView(
		renderer->logicalDevice,
		colorTarget->view,
		NULL
	);

	/* The texture is not owned by the ColorTarget
	 * so we don't free it here
	 * But the multisampleTexture is!
	 */
	if (colorTarget->multisampleTexture != NULL)
	{
		VULKAN_INTERNAL_DestroyTexture(
			renderer,
			colorTarget->multisampleTexture
		);
	}

	SDL_free(colorTarget);
}

static void VULKAN_INTERNAL_DestroyDepthStencilTarget(
	VulkanRenderer *renderer,
	VulkanDepthStencilTarget *depthStencilTarget
) {
	VULKAN_INTERNAL_DestroyTexture(renderer, depthStencilTarget->texture);
	SDL_free(depthStencilTarget);
}

static void VULKAN_INTERNAL_DestroyBuffer(
	VulkanRenderer* renderer,
	VulkanBuffer* buffer
) {
	uint32_t i;

	if (buffer->bound || buffer->boundSubmitted)
	{
		REFRESH_LogError("Cannot destroy a bound buffer!");
		return;
	}

	for (i = 0; i < buffer->subBufferCount; i += 1)
	{
		if (buffer->subBuffers[i]->allocation->dedicated)
		{
			renderer->vkFreeMemory(
				renderer->logicalDevice,
				buffer->subBuffers[i]->allocation->memory,
				NULL
			);

			SDL_free(buffer->subBuffers[i]->allocation->freeRegions);
			SDL_free(buffer->subBuffers[i]->allocation);
		}
		else
		{
			SDL_LockMutex(renderer->allocatorLock);

			VULKAN_INTERNAL_NewMemoryFreeRegion(
				buffer->subBuffers[i]->allocation,
				buffer->subBuffers[i]->offset,
				buffer->subBuffers[i]->size
			);

			SDL_UnlockMutex(renderer->allocatorLock);
		}

		renderer->vkDestroyBuffer(
			renderer->logicalDevice,
			buffer->subBuffers[i]->buffer,
			NULL
		);

		SDL_free(buffer->subBuffers[i]);
	}

	SDL_free(buffer->subBuffers);
	buffer->subBuffers = NULL;

	SDL_free(buffer);
}

static void VULKAN_INTERNAL_DestroyGraphicsPipeline(
	VulkanRenderer *renderer,
	VulkanGraphicsPipeline *graphicsPipeline
) {
	VkDescriptorSet descriptorSets[2];
	descriptorSets[0] = graphicsPipeline->vertexUBODescriptorSet;
	descriptorSets[1] = graphicsPipeline->fragmentUBODescriptorSet;

	renderer->vkFreeDescriptorSets(
		renderer->logicalDevice,
		renderer->defaultDescriptorPool,
		2,
		descriptorSets
	);

	renderer->vkDestroyPipeline(
		renderer->logicalDevice,
		graphicsPipeline->pipeline,
		NULL
	);

	SDL_free(graphicsPipeline);
}

static void VULKAN_INTERNAL_DestroyShaderModule(
	VulkanRenderer *renderer,
	VkShaderModule shaderModule
) {
	renderer->vkDestroyShaderModule(
		renderer->logicalDevice,
		shaderModule,
		NULL
	);
}

static void VULKAN_INTERNAL_DestroySampler(
	VulkanRenderer *renderer,
	VkSampler sampler
) {
	renderer->vkDestroySampler(
		renderer->logicalDevice,
		sampler,
		NULL
	);
}

/* The framebuffer doesn't own any targets so we don't have to do much. */
static void VULKAN_INTERNAL_DestroyFramebuffer(
	VulkanRenderer *renderer,
	VulkanFramebuffer *framebuffer
) {
	renderer->vkDestroyFramebuffer(
		renderer->logicalDevice,
		framebuffer->framebuffer,
		NULL
	);
}

static void VULKAN_INTERNAL_DestroyRenderPass(
	VulkanRenderer *renderer,
	VkRenderPass renderPass
) {
	renderer->vkDestroyRenderPass(
		renderer->logicalDevice,
		renderPass,
		NULL
	);
}

static void VULKAN_INTERNAL_DestroySwapchain(VulkanRenderer* renderer)
{
	uint32_t i;

	for (i = 0; i < renderer->swapChainImageCount; i += 1)
	{
		renderer->vkDestroyImageView(
			renderer->logicalDevice,
			renderer->swapChainImageViews[i],
			NULL
		);
	}

	SDL_free(renderer->swapChainImages);
	renderer->swapChainImages = NULL;
	SDL_free(renderer->swapChainImageViews);
	renderer->swapChainImageViews = NULL;
	SDL_free(renderer->swapChainResourceAccessTypes);
	renderer->swapChainResourceAccessTypes = NULL;

	renderer->vkDestroySwapchainKHR(
		renderer->logicalDevice,
		renderer->swapChain,
		NULL
	);
}

static void VULKAN_INTERNAL_DestroyTextureStagingBuffer(
	VulkanRenderer* renderer
) {
	VULKAN_INTERNAL_DestroyBuffer(
		renderer,
		renderer->textureStagingBuffer
	);
}

static void VULKAN_INTERNAL_DestroySamplerDescriptorSetCache(
	VulkanRenderer* renderer,
	SamplerDescriptorSetCache* cache
) {
	uint32_t i;

	if (cache == NULL)
	{
		return;
	}

	for (i = 0; i < cache->samplerDescriptorPoolCount; i += 1)
	{
		renderer->vkDestroyDescriptorPool(
			renderer->logicalDevice,
			cache->samplerDescriptorPools[i],
			NULL
		);
	}

	SDL_free(cache->samplerDescriptorPools);
	SDL_free(cache->inactiveDescriptorSets);
	SDL_free(cache->elements);

	for (i = 0; i < NUM_DESCRIPTOR_SET_HASH_BUCKETS; i += 1)
	{
		SDL_free(cache->buckets[i].elements);
	}

	SDL_free(cache);
}

static void VULKAN_INTERNAL_PostWorkCleanup(VulkanRenderer* renderer)
{
	uint32_t i, j;

	/* Destroy submitted resources */

	SDL_LockMutex(renderer->disposeLock);

	for (i = 0; i < renderer->submittedColorTargetsToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyColorTarget(
			renderer,
			renderer->submittedColorTargetsToDestroy[i]
		);
	}
	renderer->submittedColorTargetsToDestroyCount = 0;

	for (i = 0; i < renderer->submittedDepthStencilTargetsToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyDepthStencilTarget(
			renderer,
			renderer->submittedDepthStencilTargetsToDestroy[i]
		);
	}
	renderer->submittedDepthStencilTargetsToDestroyCount = 0;

	for (i = 0; i < renderer->submittedTexturesToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyTexture(
			renderer,
			renderer->submittedTexturesToDestroy[i]
		);
	}
	renderer->submittedTexturesToDestroyCount = 0;

	for (i = 0; i < renderer->submittedBuffersToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyBuffer(
			renderer,
			renderer->submittedBuffersToDestroy[i]
		);
	}
	renderer->submittedBuffersToDestroyCount = 0;

	for (i = 0; i < renderer->submittedGraphicsPipelinesToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyGraphicsPipeline(
			renderer,
			renderer->submittedGraphicsPipelinesToDestroy[i]
		);
	}
	renderer->submittedGraphicsPipelinesToDestroyCount = 0;

	for (i = 0; i < renderer->submittedShaderModulesToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyShaderModule(
			renderer,
			renderer->submittedShaderModulesToDestroy[i]
		);
	}
	renderer->submittedShaderModulesToDestroyCount = 0;

	for (i = 0; i < renderer->submittedSamplersToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroySampler(
			renderer,
			renderer->submittedSamplersToDestroy[i]
		);
	}
	renderer->submittedSamplersToDestroyCount = 0;

	for (i = 0; i < renderer->submittedFramebuffersToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyFramebuffer(
			renderer,
			renderer->submittedFramebuffersToDestroy[i]
		);
	}
	renderer->submittedFramebuffersToDestroyCount = 0;

	for (i = 0; i < renderer->submittedRenderPassesToDestroyCount; i += 1)
	{
		VULKAN_INTERNAL_DestroyRenderPass(
			renderer,
			renderer->submittedRenderPassesToDestroy[i]
		);
	}
	renderer->submittedRenderPassesToDestroyCount = 0;

	/* Re-size submitted destroy lists */

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedColorTargetsToDestroy,
		VulkanColorTarget*,
		renderer->colorTargetsToDestroyCount,
		renderer->submittedColorTargetsToDestroyCapacity,
		renderer->colorTargetsToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedDepthStencilTargetsToDestroy,
		VulkanDepthStencilTarget*,
		renderer->depthStencilTargetsToDestroyCount,
		renderer->submittedDepthStencilTargetsToDestroyCapacity,
		renderer->depthStencilTargetsToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedTexturesToDestroy,
		VulkanTexture*,
		renderer->texturesToDestroyCount,
		renderer->submittedTexturesToDestroyCapacity,
		renderer->texturesToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedBuffersToDestroy,
		VulkanBuffer*,
		renderer->buffersToDestroyCount,
		renderer->submittedBuffersToDestroyCapacity,
		renderer->buffersToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedGraphicsPipelinesToDestroy,
		VulkanGraphicsPipeline*,
		renderer->graphicsPipelinesToDestroyCount,
		renderer->submittedGraphicsPipelinesToDestroyCapacity,
		renderer->graphicsPipelinesToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedShaderModulesToDestroy,
		VkShaderModule,
		renderer->shaderModulesToDestroyCount,
		renderer->submittedShaderModulesToDestroyCapacity,
		renderer->shaderModulesToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedSamplersToDestroy,
		VkSampler,
		renderer->samplersToDestroyCount,
		renderer->submittedSamplersToDestroyCapacity,
		renderer->samplersToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedFramebuffersToDestroy,
		VulkanFramebuffer*,
		renderer->framebuffersToDestroyCount,
		renderer->submittedFramebuffersToDestroyCapacity,
		renderer->framebuffersToDestroyCount
	)

	EXPAND_ARRAY_IF_NEEDED(
		renderer->submittedRenderPassesToDestroy,
		VkRenderPass,
		renderer->renderPassesToDestroyCount,
		renderer->submittedRenderPassesToDestroyCapacity,
		renderer->renderPassesToDestroyCount
	)

	/* Rotate destroy lists */

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedColorTargetsToDestroy,
		renderer->submittedColorTargetsToDestroyCount,
		renderer->colorTargetsToDestroy,
		renderer->colorTargetsToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedDepthStencilTargetsToDestroy,
		renderer->submittedDepthStencilTargetsToDestroyCount,
		renderer->depthStencilTargetsToDestroy,
		renderer->depthStencilTargetsToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedTexturesToDestroy,
		renderer->submittedTexturesToDestroyCount,
		renderer->texturesToDestroy,
		renderer->texturesToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedBuffersToDestroy,
		renderer->submittedBuffersToDestroyCount,
		renderer->buffersToDestroy,
		renderer->buffersToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedGraphicsPipelinesToDestroy,
		renderer->submittedGraphicsPipelinesToDestroyCount,
		renderer->graphicsPipelinesToDestroy,
		renderer->graphicsPipelinesToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedShaderModulesToDestroy,
		renderer->submittedShaderModulesToDestroyCount,
		renderer->shaderModulesToDestroy,
		renderer->shaderModulesToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedSamplersToDestroy,
		renderer->submittedSamplersToDestroyCount,
		renderer->samplersToDestroy,
		renderer->samplersToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedFramebuffersToDestroy,
		renderer->submittedFramebuffersToDestroyCount,
		renderer->framebuffersToDestroy,
		renderer->framebuffersToDestroyCount
	)

	MOVE_ARRAY_CONTENTS_AND_RESET(
		i,
		renderer->submittedRenderPassesToDestroy,
		renderer->submittedRenderPassesToDestroyCount,
		renderer->renderPassesToDestroy,
		renderer->renderPassesToDestroyCount
	)

	SDL_UnlockMutex(renderer->disposeLock);

	/* Increment the frame index */
	/* FIXME: need a better name, and to get rid of the magic value % 2 */
	renderer->frameIndex = (renderer->frameIndex + 1) % 2;

	/* Mark sub buffers of previously submitted buffers as unbound */
	for (i = 0; i < renderer->submittedBufferCount; i += 1)
	{
		if (renderer->submittedBuffers[i] != NULL)
		{
			renderer->submittedBuffers[i]->boundSubmitted = 0;

			for (j = 0; j < renderer->submittedBuffers[i]->subBufferCount; j += 1)
			{
				if (renderer->submittedBuffers[i]->subBuffers[j]->bound == renderer->frameIndex)
				{
					renderer->submittedBuffers[i]->subBuffers[j]->bound = -1;
				}
			}

			renderer->submittedBuffers[i] = NULL;
		}
	}

	renderer->submittedBufferCount = 0;

	/* Mark currently bound buffers as submitted buffers */
	if (renderer->buffersInUseCount > renderer->submittedBufferCapacity)
	{
		renderer->submittedBuffers = SDL_realloc(
			renderer->submittedBuffers,
			sizeof(VulkanBuffer*) * renderer->buffersInUseCount
		);

		renderer->submittedBufferCapacity = renderer->buffersInUseCount;
	}

	for (i = 0; i < renderer->buffersInUseCount; i += 1)
	{
		if (renderer->buffersInUse[i] != NULL)
		{
			renderer->buffersInUse[i]->bound = 0;
			renderer->buffersInUse[i]->boundSubmitted = 1;

			renderer->submittedBuffers[i] = renderer->buffersInUse[i];
			renderer->buffersInUse[i] = NULL;
		}
	}

	renderer->submittedBufferCount = renderer->buffersInUseCount;
	renderer->buffersInUseCount = 0;
}

/* Swapchain */

static inline VkExtent2D VULKAN_INTERNAL_ChooseSwapExtent(
	void* windowHandle,
	const VkSurfaceCapabilitiesKHR capabilities
) {
	VkExtent2D actualExtent;
	int32_t drawableWidth, drawableHeight;

	if (capabilities.currentExtent.width != UINT32_MAX)
	{
		return capabilities.currentExtent;
	}
	else
	{
		SDL_Vulkan_GetDrawableSize(
			(SDL_Window*) windowHandle,
			&drawableWidth,
			&drawableHeight
		);

		actualExtent.width = drawableWidth;
		actualExtent.height = drawableHeight;

		return actualExtent;
	}
}

static uint8_t VULKAN_INTERNAL_QuerySwapChainSupport(
	VulkanRenderer *renderer,
	VkPhysicalDevice physicalDevice,
	VkSurfaceKHR surface,
	SwapChainSupportDetails *outputDetails
) {
	VkResult result;
	uint32_t formatCount;
	uint32_t presentModeCount;

	result = renderer->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
		physicalDevice,
		surface,
		&outputDetails->capabilities
	);
	if (result != VK_SUCCESS)
	{
		REFRESH_LogError(
			"vkGetPhysicalDeviceSurfaceCapabilitiesKHR: %s",
			VkErrorMessages(result)
		);

		return 0;
	}

	renderer->vkGetPhysicalDeviceSurfaceFormatsKHR(
		physicalDevice,
		surface,
		&formatCount,
		NULL
	);

	if (formatCount != 0)
	{
		outputDetails->formats = (VkSurfaceFormatKHR*) SDL_malloc(
			sizeof(VkSurfaceFormatKHR) * formatCount
		);
		outputDetails->formatsLength = formatCount;

		if (!outputDetails->formats)
		{
			SDL_OutOfMemory();
			return 0;
		}

		result = renderer->vkGetPhysicalDeviceSurfaceFormatsKHR(
			physicalDevice,
			surface,
			&formatCount,
			outputDetails->formats
		);
		if (result != VK_SUCCESS)
		{
			REFRESH_LogError(
				"vkGetPhysicalDeviceSurfaceFormatsKHR: %s",
				VkErrorMessages(result)
			);

			SDL_free(outputDetails->formats);
			return 0;
		}
	}

	renderer->vkGetPhysicalDeviceSurfacePresentModesKHR(
		physicalDevice,
		surface,
		&presentModeCount,
		NULL
	);

	if (presentModeCount != 0)
	{
		outputDetails->presentModes = (VkPresentModeKHR*) SDL_malloc(
			sizeof(VkPresentModeKHR) * presentModeCount
		);
		outputDetails->presentModesLength = presentModeCount;

		if (!outputDetails->presentModes)
		{
			SDL_OutOfMemory();
			return 0;
		}

		result = renderer->vkGetPhysicalDeviceSurfacePresentModesKHR(
			physicalDevice,
			surface,
			&presentModeCount,
			outputDetails->presentModes
		);
		if (result != VK_SUCCESS)
		{
			REFRESH_LogError(
				"vkGetPhysicalDeviceSurfacePresentModesKHR: %s",
				VkErrorMessages(result)
			);

			SDL_free(outputDetails->formats);
			SDL_free(outputDetails->presentModes);
			return 0;
		}
	}

	return 1;
}

static uint8_t VULKAN_INTERNAL_ChooseSwapSurfaceFormat(
	VkFormat desiredFormat,
	VkSurfaceFormatKHR *availableFormats,
	uint32_t availableFormatsLength,
	VkSurfaceFormatKHR *outputFormat
) {
	uint32_t i;
	for (i = 0; i < availableFormatsLength; i += 1)
	{
		if (	availableFormats[i].format == desiredFormat &&
			availableFormats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR	)
		{
			*outputFormat = availableFormats[i];
			return 1;
		}
	}

	REFRESH_LogError("Desired surface format is unavailable.");
	return 0;
}

static uint8_t VULKAN_INTERNAL_ChooseSwapPresentMode(
	REFRESH_PresentMode desiredPresentInterval,
	VkPresentModeKHR *availablePresentModes,
	uint32_t availablePresentModesLength,
	VkPresentModeKHR *outputPresentMode
) {
	#define CHECK_MODE(m) \
		for (i = 0; i < availablePresentModesLength; i += 1) \
		{ \
			if (availablePresentModes[i] == m) \
			{ \
				*outputPresentMode = m; \
				REFRESH_LogInfo("Using " #m "!"); \
				return 1; \
			} \
		} \
		REFRESH_LogInfo(#m " unsupported.");

	uint32_t i;
    if (desiredPresentInterval == REFRESH_PRESENTMODE_IMMEDIATE)
	{
		CHECK_MODE(VK_PRESENT_MODE_IMMEDIATE_KHR)
	}
    else if (desiredPresentInterval == REFRESH_PRESENTMODE_MAILBOX)
    {
        CHECK_MODE(VK_PRESENT_MODE_MAILBOX_KHR)
    }
    else if (desiredPresentInterval == REFRESH_PRESENTMODE_FIFO)
    {
        CHECK_MODE(VK_PRESENT_MODE_FIFO_KHR)
    }
    else if (desiredPresentInterval == REFRESH_PRESENTMODE_FIFO_RELAXED)
    {
        CHECK_MODE(VK_PRESENT_MODE_FIFO_RELAXED_KHR)
    }
	else
	{
		REFRESH_LogError(
			"Unrecognized PresentInterval: %d",
			desiredPresentInterval
		);
		return 0;
	}

	#undef CHECK_MODE

	REFRESH_LogInfo("Fall back to VK_PRESENT_MODE_FIFO_KHR.");
	*outputPresentMode = VK_PRESENT_MODE_FIFO_KHR;
	return 1;
}

static CreateSwapchainResult VULKAN_INTERNAL_CreateSwapchain(
	VulkanRenderer *renderer
) {
	VkResult vulkanResult;
	SwapChainSupportDetails swapChainSupportDetails;
	VkSurfaceFormatKHR surfaceFormat;
	VkPresentModeKHR presentMode;
	VkExtent2D extent;
	uint32_t imageCount, swapChainImageCount, i;
	VkSwapchainCreateInfoKHR swapChainCreateInfo;
	VkImage *swapChainImages;
	VkImageViewCreateInfo createInfo;
	VkImageView swapChainImageView;

	if (!VULKAN_INTERNAL_QuerySwapChainSupport(
		renderer,
		renderer->physicalDevice,
		renderer->surface,
		&swapChainSupportDetails
	)) {
		REFRESH_LogError("Device does not support swap chain creation");
		return CREATE_SWAPCHAIN_FAIL;
	}

	renderer->swapChainFormat = VK_FORMAT_B8G8R8A8_UNORM;
	renderer->swapChainSwizzle.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	renderer->swapChainSwizzle.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	renderer->swapChainSwizzle.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	renderer->swapChainSwizzle.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	if (!VULKAN_INTERNAL_ChooseSwapSurfaceFormat(
		renderer->swapChainFormat,
		swapChainSupportDetails.formats,
		swapChainSupportDetails.formatsLength,
		&surfaceFormat
	)) {
		SDL_free(swapChainSupportDetails.formats);
		SDL_free(swapChainSupportDetails.presentModes);
		REFRESH_LogError("Device does not support swap chain format");
		return CREATE_SWAPCHAIN_FAIL;
	}

	if (!VULKAN_INTERNAL_ChooseSwapPresentMode(
		renderer->presentMode,
		swapChainSupportDetails.presentModes,
		swapChainSupportDetails.presentModesLength,
		&presentMode
	)) {
		SDL_free(swapChainSupportDetails.formats);
		SDL_free(swapChainSupportDetails.presentModes);
		REFRESH_LogError("Device does not support swap chain present mode");
		return CREATE_SWAPCHAIN_FAIL;
	}

	extent = VULKAN_INTERNAL_ChooseSwapExtent(
		renderer->deviceWindowHandle,
		swapChainSupportDetails.capabilities
	);

	if (extent.width == 0 || extent.height == 0)
	{
		return CREATE_SWAPCHAIN_SURFACE_ZERO;
	}

	imageCount = swapChainSupportDetails.capabilities.minImageCount + 1;

	if (	swapChainSupportDetails.capabilities.maxImageCount > 0 &&
		imageCount > swapChainSupportDetails.capabilities.maxImageCount	)
	{
		imageCount = swapChainSupportDetails.capabilities.maxImageCount;
	}

	if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
	{
		/* Required for proper triple-buffering.
		 * Note that this is below the above maxImageCount check!
		 * If the driver advertises MAILBOX but does not support 3 swap
		 * images, it's not real mailbox support, so let it fail hard.
		 * -flibit
		 */
		imageCount = SDL_max(imageCount, 3);
	}

	swapChainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapChainCreateInfo.pNext = NULL;
	swapChainCreateInfo.flags = 0;
	swapChainCreateInfo.surface = renderer->surface;
	swapChainCreateInfo.minImageCount = imageCount;
	swapChainCreateInfo.imageFormat = surfaceFormat.format;
	swapChainCreateInfo.imageColorSpace = surfaceFormat.colorSpace;
	swapChainCreateInfo.imageExtent = extent;
	swapChainCreateInfo.imageArrayLayers = 1;
	swapChainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swapChainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapChainCreateInfo.queueFamilyIndexCount = 0;
	swapChainCreateInfo.pQueueFamilyIndices = NULL;
	swapChainCreateInfo.preTransform = swapChainSupportDetails.capabilities.currentTransform;
	swapChainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapChainCreateInfo.presentMode = presentMode;
	swapChainCreateInfo.clipped = VK_TRUE;
	swapChainCreateInfo.oldSwapchain = VK_NULL_HANDLE;

	vulkanResult = renderer->vkCreateSwapchainKHR(
		renderer->logicalDevice,
		&swapChainCreateInfo,
		NULL,
		&renderer->swapChain
	);

	SDL_free(swapChainSupportDetails.formats);
	SDL_free(swapChainSupportDetails.presentModes);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateSwapchainKHR", vulkanResult);

		return CREATE_SWAPCHAIN_FAIL;
	}

	renderer->vkGetSwapchainImagesKHR(
		renderer->logicalDevice,
		renderer->swapChain,
		&swapChainImageCount,
		NULL
	);

	renderer->swapChainImages = (VkImage*) SDL_malloc(
		sizeof(VkImage) * swapChainImageCount
	);
	if (!renderer->swapChainImages)
	{
		SDL_OutOfMemory();
		return CREATE_SWAPCHAIN_FAIL;
	}

	renderer->swapChainImageViews = (VkImageView*) SDL_malloc(
		sizeof(VkImageView) * swapChainImageCount
	);
	if (!renderer->swapChainImageViews)
	{
		SDL_OutOfMemory();
		return CREATE_SWAPCHAIN_FAIL;
	}

	renderer->swapChainResourceAccessTypes = (VulkanResourceAccessType*) SDL_malloc(
		sizeof(VulkanResourceAccessType) * swapChainImageCount
	);
	if (!renderer->swapChainResourceAccessTypes)
	{
		SDL_OutOfMemory();
		return CREATE_SWAPCHAIN_FAIL;
	}

	swapChainImages = SDL_stack_alloc(VkImage, swapChainImageCount);
	renderer->vkGetSwapchainImagesKHR(
		renderer->logicalDevice,
		renderer->swapChain,
		&swapChainImageCount,
		swapChainImages
	);
	renderer->swapChainImageCount = swapChainImageCount;
	renderer->swapChainExtent = extent;

	createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	createInfo.pNext = NULL;
	createInfo.flags = 0;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createInfo.format = surfaceFormat.format;
	createInfo.components = renderer->swapChainSwizzle;
	createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	createInfo.subresourceRange.baseMipLevel = 0;
	createInfo.subresourceRange.levelCount = 1;
	createInfo.subresourceRange.baseArrayLayer = 0;
	createInfo.subresourceRange.layerCount = 1;
	for (i = 0; i < swapChainImageCount; i += 1)
	{
		createInfo.image = swapChainImages[i];

		vulkanResult = renderer->vkCreateImageView(
			renderer->logicalDevice,
			&createInfo,
			NULL,
			&swapChainImageView
		);

		if (vulkanResult != VK_SUCCESS)
		{
			LogVulkanResult("vkCreateImageView", vulkanResult);
			SDL_stack_free(swapChainImages);
			return CREATE_SWAPCHAIN_FAIL;
		}

		renderer->swapChainImages[i] = swapChainImages[i];
		renderer->swapChainImageViews[i] = swapChainImageView;
		renderer->swapChainResourceAccessTypes[i] = RESOURCE_ACCESS_NONE;
	}

	SDL_stack_free(swapChainImages);
	return CREATE_SWAPCHAIN_SUCCESS;
}

static void VULKAN_INTERNAL_RecreateSwapchain(VulkanRenderer* renderer)
{
	CreateSwapchainResult createSwapchainResult;
	SwapChainSupportDetails swapChainSupportDetails;
	VkExtent2D extent;

	renderer->vkDeviceWaitIdle(renderer->logicalDevice);

	VULKAN_INTERNAL_QuerySwapChainSupport(
		renderer,
		renderer->physicalDevice,
		renderer->surface,
		&swapChainSupportDetails
	);

	extent = VULKAN_INTERNAL_ChooseSwapExtent(
		renderer->deviceWindowHandle,
		swapChainSupportDetails.capabilities
	);

	if (extent.width == 0 || extent.height == 0)
	{
		return;
	}

	VULKAN_INTERNAL_DestroySwapchain(renderer);
	createSwapchainResult = VULKAN_INTERNAL_CreateSwapchain(renderer);

	if (createSwapchainResult == CREATE_SWAPCHAIN_FAIL)
	{
		REFRESH_LogError("Failed to recreate swapchain");
		return;
	}

	renderer->vkDeviceWaitIdle(renderer->logicalDevice);
}

/* Data Buffer */

/* buffer should be an alloc'd but uninitialized VulkanTexture */
static uint8_t VULKAN_INTERNAL_CreateBuffer(
	VulkanRenderer *renderer,
	VkDeviceSize size,
	VulkanResourceAccessType resourceAccessType,
	VkBufferUsageFlags usage,
	uint32_t subBufferCount,
	VulkanBuffer *buffer
) {
	VkResult vulkanResult;
	VkBufferCreateInfo bufferCreateInfo;
	uint8_t findMemoryResult;
	uint32_t i;

	buffer->size = size;
	buffer->currentSubBufferIndex = 0;
	buffer->bound = 0;
	buffer->boundSubmitted = 0;
	buffer->resourceAccessType = resourceAccessType;
	buffer->usage = usage;
	buffer->subBufferCount = subBufferCount;
	buffer->subBuffers = SDL_malloc(
		sizeof(VulkanSubBuffer*) * buffer->subBufferCount
	);

	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.pNext = NULL;
	bufferCreateInfo.flags = 0;
	bufferCreateInfo.size = size;
	bufferCreateInfo.usage = usage;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bufferCreateInfo.queueFamilyIndexCount = 1;
	bufferCreateInfo.pQueueFamilyIndices = &renderer->queueFamilyIndices.graphicsFamily;

	for (i = 0; i < subBufferCount; i += 1)
	{
		buffer->subBuffers[i] = SDL_malloc(
			sizeof(VulkanSubBuffer) * buffer->subBufferCount
		);

		vulkanResult = renderer->vkCreateBuffer(
			renderer->logicalDevice,
			&bufferCreateInfo,
			NULL,
			&buffer->subBuffers[i]->buffer
		);

		if (vulkanResult != VK_SUCCESS)
		{
			LogVulkanResult("vkCreateBuffer", vulkanResult);
			REFRESH_LogError("Failed to create VkBuffer");
			return 0;
		}

		findMemoryResult = VULKAN_INTERNAL_FindAvailableMemory(
			renderer,
			buffer->subBuffers[i]->buffer,
			VK_NULL_HANDLE,
			&buffer->subBuffers[i]->allocation,
			&buffer->subBuffers[i]->offset,
			&buffer->subBuffers[i]->size
		);

		/* We're out of available memory */
		if (findMemoryResult == 2)
		{
			REFRESH_LogWarn("Out of buffer memory!");
			return 2;
		}
		else if (findMemoryResult == 0)
		{
			REFRESH_LogError("Failed to find buffer memory!");
			return 0;
		}

		vulkanResult = renderer->vkBindBufferMemory(
			renderer->logicalDevice,
			buffer->subBuffers[i]->buffer,
			buffer->subBuffers[i]->allocation->memory,
			buffer->subBuffers[i]->offset
		);

		if (vulkanResult != VK_SUCCESS)
		{
			REFRESH_LogError("Failed to bind buffer memory!");
			return 0;
		}

		buffer->subBuffers[i]->resourceAccessType = resourceAccessType;
		buffer->subBuffers[i]->bound = -1;

		VULKAN_INTERNAL_BufferMemoryBarrier(
			renderer,
			buffer->resourceAccessType,
			buffer,
			buffer->subBuffers[i]
		);
	}

	return 1;
}

/* Command Buffers */

static void VULKAN_INTERNAL_BeginCommandBuffer(VulkanRenderer *renderer)
{
	VkCommandBufferAllocateInfo allocateInfo;
	VkCommandBufferBeginInfo beginInfo;
	VkResult result;

	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.pNext = NULL;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	beginInfo.pInheritanceInfo = NULL;

	/* If we are out of unused command buffers, allocate some more */
	if (renderer->inactiveCommandBufferCount == 0)
	{
		renderer->activeCommandBuffers = SDL_realloc(
			renderer->activeCommandBuffers,
			sizeof(VkCommandBuffer) * renderer->allocatedCommandBufferCount * 2
		);

		renderer->inactiveCommandBuffers = SDL_realloc(
			renderer->inactiveCommandBuffers,
			sizeof(VkCommandBuffer) * renderer->allocatedCommandBufferCount * 2
		);

		renderer->submittedCommandBuffers = SDL_realloc(
			renderer->submittedCommandBuffers,
			sizeof(VkCommandBuffer) * renderer->allocatedCommandBufferCount * 2
		);

		allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocateInfo.pNext = NULL;
		allocateInfo.commandPool = renderer->commandPool;
		allocateInfo.commandBufferCount = renderer->allocatedCommandBufferCount;
		allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

		result = renderer->vkAllocateCommandBuffers(
			renderer->logicalDevice,
			&allocateInfo,
			renderer->inactiveCommandBuffers
		);

		if (result != VK_SUCCESS)
		{
			LogVulkanResult("vkAllocateCommandBuffers", result);
			return;
		}

		renderer->inactiveCommandBufferCount = renderer->allocatedCommandBufferCount;
		renderer->allocatedCommandBufferCount *= 2;
	}

	renderer->currentCommandBuffer =
		renderer->inactiveCommandBuffers[renderer->inactiveCommandBufferCount - 1];

	renderer->activeCommandBuffers[renderer->activeCommandBufferCount] = renderer->currentCommandBuffer;

	renderer->activeCommandBufferCount += 1;
	renderer->inactiveCommandBufferCount -= 1;

	result = renderer->vkBeginCommandBuffer(
		renderer->currentCommandBuffer,
		&beginInfo
	);

	if (result != VK_SUCCESS)
	{
		LogVulkanResult("vkBeginCommandBuffer", result);
	}
}

static void VULKAN_INTERNAL_EndCommandBuffer(
	VulkanRenderer* renderer
) {
	VkResult result;

	result = renderer->vkEndCommandBuffer(
		renderer->currentCommandBuffer
	);

	if (result != VK_SUCCESS)
	{
		LogVulkanResult("vkEndCommandBuffer", result);
	}

	renderer->currentCommandBuffer = NULL;
	renderer->numActiveCommands = 0;
}

/* Public API */

static void VULKAN_DestroyDevice(
    REFRESH_Device *device
) {
	VulkanRenderer* renderer = (VulkanRenderer*) device->driverData;
	VkResult waitResult;
	PipelineLayoutHashArray pipelineLayoutHashArray;
	VulkanMemorySubAllocator *allocator;
	uint32_t i, j, k;

	VULKAN_Submit(device->driverData);

	waitResult = renderer->vkDeviceWaitIdle(renderer->logicalDevice);

	if (waitResult != VK_SUCCESS)
	{
		LogVulkanResult("vkDeviceWaitIdle", waitResult);
	}

	VULKAN_INTERNAL_DestroyBuffer(renderer, renderer->dummyVertexUniformBuffer);
	VULKAN_INTERNAL_DestroyBuffer(renderer, renderer->dummyFragmentUniformBuffer);
	VULKAN_INTERNAL_DestroyBuffer(renderer, renderer->vertexUBO);
	VULKAN_INTERNAL_DestroyBuffer(renderer, renderer->fragmentUBO);

	/* We have to do this twice so the rotation happens correctly */
	VULKAN_INTERNAL_PostWorkCleanup(renderer);
	VULKAN_INTERNAL_PostWorkCleanup(renderer);

	VULKAN_INTERNAL_DestroyTextureStagingBuffer(renderer);

	renderer->vkDestroySemaphore(
		renderer->logicalDevice,
		renderer->imageAvailableSemaphore,
		NULL
	);

	renderer->vkDestroySemaphore(
		renderer->logicalDevice,
		renderer->renderFinishedSemaphore,
		NULL
	);

	renderer->vkDestroyFence(
		renderer->logicalDevice,
		renderer->inFlightFence,
		NULL
	);

	renderer->vkDestroyCommandPool(
		renderer->logicalDevice,
		renderer->commandPool,
		NULL
	);

	for (i = 0; i < NUM_PIPELINE_LAYOUT_BUCKETS; i += 1)
	{
		pipelineLayoutHashArray = renderer->pipelineLayoutHashTable.buckets[i];
		for (j = 0; j < pipelineLayoutHashArray.count; j += 1)
		{
			VULKAN_INTERNAL_DestroySamplerDescriptorSetCache(
				renderer,
				pipelineLayoutHashArray.elements[j].value->vertexSamplerDescriptorSetCache
			);

			VULKAN_INTERNAL_DestroySamplerDescriptorSetCache(
				renderer,
				pipelineLayoutHashArray.elements[j].value->fragmentSamplerDescriptorSetCache
			);

			renderer->vkDestroyPipelineLayout(
				renderer->logicalDevice,
				pipelineLayoutHashArray.elements[j].value->pipelineLayout,
				NULL
			);
		}

		if (pipelineLayoutHashArray.elements != NULL)
		{
			SDL_free(pipelineLayoutHashArray.elements);
		}
	}

	renderer->vkDestroyDescriptorPool(
		renderer->logicalDevice,
		renderer->defaultDescriptorPool,
		NULL
	);

	for (i = 0; i < NUM_DESCRIPTOR_SET_LAYOUT_BUCKETS; i += 1)
	{
		for (j = 0; j < renderer->samplerDescriptorSetLayoutHashTable.buckets[i].count; j += 1)
		{
			renderer->vkDestroyDescriptorSetLayout(
				renderer->logicalDevice,
				renderer->samplerDescriptorSetLayoutHashTable.buckets[i].elements[j].value,
				NULL
			);
		}

		SDL_free(renderer->samplerDescriptorSetLayoutHashTable.buckets[i].elements);
	}

	renderer->vkDestroyDescriptorSetLayout(
		renderer->logicalDevice,
		renderer->emptyVertexSamplerLayout,
		NULL
	);

	renderer->vkDestroyDescriptorSetLayout(
		renderer->logicalDevice,
		renderer->emptyFragmentSamplerLayout,
		NULL
	);

	renderer->vkDestroyDescriptorSetLayout(
		renderer->logicalDevice,
		renderer->vertexParamLayout,
		NULL
	);

	renderer->vkDestroyDescriptorSetLayout(
		renderer->logicalDevice,
		renderer->fragmentParamLayout,
		NULL
	);

	VULKAN_INTERNAL_DestroySwapchain(renderer);

	if (!renderer->headless)
	{
		renderer->vkDestroySurfaceKHR(
			renderer->instance,
			renderer->surface,
			NULL
		);
	}

	for (i = 0; i < VK_MAX_MEMORY_TYPES; i += 1)
	{
		allocator = &renderer->memoryAllocator->subAllocators[i];

		for (j = 0; j < allocator->allocationCount; j += 1)
		{
			for (k = 0; k < allocator->allocations[j]->freeRegionCount; k += 1)
			{
				SDL_free(allocator->allocations[j]->freeRegions[k]);
			}

			SDL_free(allocator->allocations[j]->freeRegions);

			renderer->vkFreeMemory(
				renderer->logicalDevice,
				allocator->allocations[j]->memory,
				NULL
			);

			SDL_free(allocator->allocations[j]);
		}

		SDL_free(allocator->allocations);
		SDL_free(allocator->sortedFreeRegions);
	}

	SDL_free(renderer->memoryAllocator);

	SDL_DestroyMutex(renderer->commandLock);
	SDL_DestroyMutex(renderer->allocatorLock);
	SDL_DestroyMutex(renderer->disposeLock);

	SDL_free(renderer->buffersInUse);

	SDL_free(renderer->inactiveCommandBuffers);
	SDL_free(renderer->activeCommandBuffers);
	SDL_free(renderer->submittedCommandBuffers);

	renderer->vkDestroyDevice(renderer->logicalDevice, NULL);
	renderer->vkDestroyInstance(renderer->instance, NULL);

	SDL_free(renderer);
	SDL_free(device);
}

static void VULKAN_Clear(
	REFRESH_Renderer *driverData,
	REFRESH_Rect *clearRect,
	REFRESH_ClearOptions options,
	REFRESH_Color *colors,
	uint32_t colorCount,
	float depth,
	int32_t stencil
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	uint32_t attachmentCount, i;
	VkClearAttachment clearAttachments[MAX_COLOR_TARGET_BINDINGS + 1];
	VkClearRect vulkanClearRect;
	VkClearValue clearValues[4];

	uint8_t shouldClearColor = options & REFRESH_CLEAROPTIONS_COLOR;
	uint8_t shouldClearDepth = options & REFRESH_CLEAROPTIONS_DEPTH;
	uint8_t shouldClearStencil = options & REFRESH_CLEAROPTIONS_STENCIL;

	uint8_t shouldClearDepthStencil = (
		(shouldClearDepth || shouldClearStencil) &&
		renderer->currentFramebuffer->depthStencilTarget != NULL
	);

	if (!shouldClearColor && !shouldClearDepthStencil)
	{
		return;
	}

	vulkanClearRect.baseArrayLayer = 0;
	vulkanClearRect.layerCount = 1;
	vulkanClearRect.rect.offset.x = clearRect->x;
	vulkanClearRect.rect.offset.y = clearRect->y;
	vulkanClearRect.rect.extent.width = clearRect->w;
	vulkanClearRect.rect.extent.height = clearRect->h;

	attachmentCount = 0;

	if (shouldClearColor)
	{
		for (i = 0; i < colorCount; i += 1)
		{
			clearValues[i].color.float32[0] = colors[i].r / 255.0f;
			clearValues[i].color.float32[1] = colors[i].g / 255.0f;
			clearValues[i].color.float32[2] = colors[i].b / 255.0f;
			clearValues[i].color.float32[3] = colors[i].a / 255.0f;
		}

		for (i = 0; i < colorCount; i += 1)
		{
			clearAttachments[attachmentCount].aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT;
			clearAttachments[attachmentCount].colorAttachment =
				attachmentCount;
			clearAttachments[attachmentCount].clearValue =
				clearValues[attachmentCount];
			attachmentCount += 1;

			/* Do NOT clear the multisample image here!
			 * Vulkan treats them both as the same color attachment.
			 * Vulkan is a very good and not confusing at all API.
			 */
		}
	}

	if (shouldClearDepthStencil)
	{
		clearAttachments[attachmentCount].aspectMask = 0;
		clearAttachments[attachmentCount].colorAttachment = 0;

		if (shouldClearDepth)
		{
			if (depth < 0.0f)
			{
				depth = 0.0f;
			}
			else if (depth > 1.0f)
			{
				depth = 1.0f;
			}
			clearAttachments[attachmentCount].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
			clearAttachments[attachmentCount].clearValue.depthStencil.depth = depth;
		}
		else
		{
			clearAttachments[attachmentCount].clearValue.depthStencil.depth = 0.0f;
		}

		if (shouldClearStencil)
		{
			clearAttachments[attachmentCount].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			clearAttachments[attachmentCount].clearValue.depthStencil.stencil = stencil;
		}
		else
		{
			clearAttachments[attachmentCount].clearValue.depthStencil.stencil = 0;
		}

		attachmentCount += 1;
	}

	RECORD_CMD(renderer->vkCmdClearAttachments(
		renderer->currentCommandBuffer,
		attachmentCount,
		clearAttachments,
		1,
		&vulkanClearRect
	));
}

static void VULKAN_DrawInstancedPrimitives(
	REFRESH_Renderer *driverData,
	uint32_t baseVertex,
	uint32_t minVertexIndex,
	uint32_t numVertices,
	uint32_t startIndex,
	uint32_t primitiveCount,
	uint32_t instanceCount,
	REFRESH_Buffer *indices,
	REFRESH_IndexElementSize indexElementSize,
	uint32_t vertexParamOffset,
	uint32_t fragmentParamOffset
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VkDescriptorSet descriptorSets[4];
	uint32_t dynamicOffsets[2];

	descriptorSets[0] = renderer->currentGraphicsPipeline->vertexSamplerDescriptorSet;
	descriptorSets[1] = renderer->currentGraphicsPipeline->fragmentSamplerDescriptorSet;
	descriptorSets[2] = renderer->currentGraphicsPipeline->vertexUBODescriptorSet;
	descriptorSets[3] = renderer->currentGraphicsPipeline->fragmentUBODescriptorSet;

	dynamicOffsets[0] = vertexParamOffset;
	dynamicOffsets[1] = fragmentParamOffset;

	RECORD_CMD(renderer->vkCmdBindDescriptorSets(
		renderer->currentCommandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->currentGraphicsPipeline->pipelineLayout->pipelineLayout,
		0,
		4,
		descriptorSets,
		2,
		dynamicOffsets
	));

	RECORD_CMD(renderer->vkCmdDrawIndexed(
		renderer->currentCommandBuffer,
		PrimitiveVerts(
			renderer->currentGraphicsPipeline->primitiveType,
			primitiveCount
		),
		instanceCount,
		startIndex,
		baseVertex,
		0
	));
}

static void VULKAN_DrawIndexedPrimitives(
	REFRESH_Renderer *driverData,
	uint32_t baseVertex,
	uint32_t minVertexIndex,
	uint32_t numVertices,
	uint32_t startIndex,
	uint32_t primitiveCount,
	REFRESH_Buffer *indices,
	REFRESH_IndexElementSize indexElementSize,
	uint32_t vertexParamOffset,
	uint32_t fragmentParamOffset
) {
	VULKAN_DrawInstancedPrimitives(
		driverData,
		baseVertex,
		minVertexIndex,
		numVertices,
		startIndex,
		primitiveCount,
		1,
		indices,
		indexElementSize,
		vertexParamOffset,
		fragmentParamOffset
	);
}

static void VULKAN_DrawPrimitives(
	REFRESH_Renderer *driverData,
	uint32_t vertexStart,
	uint32_t primitiveCount,
	uint32_t vertexUniformBufferOffset,
	uint32_t fragmentUniformBufferOffset
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VkDescriptorSet descriptorSets[4];
	uint32_t dynamicOffsets[2];

	descriptorSets[0] = renderer->currentGraphicsPipeline->vertexSamplerDescriptorSet;
	descriptorSets[1] = renderer->currentGraphicsPipeline->fragmentSamplerDescriptorSet;
	descriptorSets[2] = renderer->currentGraphicsPipeline->vertexUBODescriptorSet;
	descriptorSets[3] = renderer->currentGraphicsPipeline->fragmentUBODescriptorSet;

	dynamicOffsets[0] = vertexUniformBufferOffset;
	dynamicOffsets[1] = fragmentUniformBufferOffset;

	RECORD_CMD(renderer->vkCmdBindDescriptorSets(
		renderer->currentCommandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->currentGraphicsPipeline->pipelineLayout->pipelineLayout,
		0,
		4,
		descriptorSets,
		2,
		dynamicOffsets
	));

	RECORD_CMD(renderer->vkCmdDraw(
		renderer->currentCommandBuffer,
		PrimitiveVerts(
			renderer->currentGraphicsPipeline->primitiveType,
			primitiveCount
		),
		1,
		vertexStart,
		0
	));
}

static REFRESH_RenderPass* VULKAN_CreateRenderPass(
	REFRESH_Renderer *driverData,
	REFRESH_RenderPassCreateInfo *renderPassCreateInfo
) {
    VulkanRenderer *renderer = (VulkanRenderer*) driverData;

    VkResult vulkanResult;
    VkAttachmentDescription attachmentDescriptions[2 * MAX_COLOR_TARGET_BINDINGS + 1];
    VkAttachmentReference colorAttachmentReferences[MAX_COLOR_TARGET_BINDINGS];
    VkAttachmentReference resolveReferences[MAX_COLOR_TARGET_BINDINGS + 1];
    VkAttachmentReference depthStencilAttachmentReference;
	VkRenderPassCreateInfo vkRenderPassCreateInfo;
    VkSubpassDescription subpass;
    VkRenderPass renderPass;
    uint32_t i;
	uint8_t multisampling = 0;

    uint32_t attachmentDescriptionCount = 0;
    uint32_t colorAttachmentReferenceCount = 0;
    uint32_t resolveReferenceCount = 0;

    for (i = 0; i < renderPassCreateInfo->colorTargetCount; i += 1)
    {
        if (renderPassCreateInfo->colorTargetDescriptions[attachmentDescriptionCount].multisampleCount > REFRESH_SAMPLECOUNT_1)
        {
			multisampling = 1;

            /* Resolve attachment and multisample attachment */

            attachmentDescriptions[attachmentDescriptionCount].flags = 0;
            attachmentDescriptions[attachmentDescriptionCount].format = RefreshToVK_SurfaceFormat[
                renderPassCreateInfo->colorTargetDescriptions[i].format
            ];
            attachmentDescriptions[attachmentDescriptionCount].samples =
                VK_SAMPLE_COUNT_1_BIT;
            attachmentDescriptions[attachmentDescriptionCount].loadOp = RefreshToVK_LoadOp[
                renderPassCreateInfo->colorTargetDescriptions[i].loadOp
            ];
            attachmentDescriptions[attachmentDescriptionCount].storeOp = RefreshToVK_StoreOp[
                renderPassCreateInfo->colorTargetDescriptions[i].storeOp
            ];
            attachmentDescriptions[attachmentDescriptionCount].stencilLoadOp =
                VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachmentDescriptions[attachmentDescriptionCount].stencilStoreOp =
                VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachmentDescriptions[attachmentDescriptionCount].initialLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachmentDescriptions[attachmentDescriptionCount].finalLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            resolveReferences[resolveReferenceCount].attachment =
                attachmentDescriptionCount;
            resolveReferences[resolveReferenceCount].layout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachmentDescriptionCount += 1;
            resolveReferenceCount += 1;

            attachmentDescriptions[attachmentDescriptionCount].flags = 0;
            attachmentDescriptions[attachmentDescriptionCount].format = RefreshToVK_SurfaceFormat[
                renderPassCreateInfo->colorTargetDescriptions[i].format
            ];
            attachmentDescriptions[attachmentDescriptionCount].samples = RefreshToVK_SampleCount[
                renderPassCreateInfo->colorTargetDescriptions[i].multisampleCount
            ];
            attachmentDescriptions[attachmentDescriptionCount].loadOp = RefreshToVK_LoadOp[
                renderPassCreateInfo->colorTargetDescriptions[i].loadOp
            ];
            attachmentDescriptions[attachmentDescriptionCount].storeOp = RefreshToVK_StoreOp[
                renderPassCreateInfo->colorTargetDescriptions[i].storeOp
            ];
            attachmentDescriptions[attachmentDescriptionCount].stencilLoadOp =
                VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachmentDescriptions[attachmentDescriptionCount].stencilStoreOp =
                VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachmentDescriptions[attachmentDescriptionCount].initialLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachmentDescriptions[attachmentDescriptionCount].finalLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            colorAttachmentReferences[colorAttachmentReferenceCount].attachment =
                attachmentDescriptionCount;
            colorAttachmentReferences[colorAttachmentReferenceCount].layout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachmentDescriptionCount += 1;
            colorAttachmentReferenceCount += 1;
        }
        else
        {
            attachmentDescriptions[attachmentDescriptionCount].flags = 0;
            attachmentDescriptions[attachmentDescriptionCount].format = RefreshToVK_SurfaceFormat[
                renderPassCreateInfo->colorTargetDescriptions[i].format
            ];
            attachmentDescriptions[attachmentDescriptionCount].samples =
                VK_SAMPLE_COUNT_1_BIT;
            attachmentDescriptions[attachmentDescriptionCount].loadOp = RefreshToVK_LoadOp[
                renderPassCreateInfo->colorTargetDescriptions[i].loadOp
            ];
            attachmentDescriptions[attachmentDescriptionCount].storeOp = RefreshToVK_StoreOp[
                renderPassCreateInfo->colorTargetDescriptions[i].storeOp
            ];
            attachmentDescriptions[attachmentDescriptionCount].stencilLoadOp =
                VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachmentDescriptions[attachmentDescriptionCount].stencilStoreOp =
                VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachmentDescriptions[attachmentDescriptionCount].initialLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachmentDescriptions[attachmentDescriptionCount].finalLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachmentDescriptionCount += 1;

            colorAttachmentReferences[colorAttachmentReferenceCount].attachment = i;
            colorAttachmentReferences[colorAttachmentReferenceCount].layout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            colorAttachmentReferenceCount += 1;
        }
    }

    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.flags = 0;
    subpass.inputAttachmentCount = 0;
    subpass.pInputAttachments = NULL;
    subpass.colorAttachmentCount = renderPassCreateInfo->colorTargetCount;
    subpass.pColorAttachments = colorAttachmentReferences;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = NULL;

    if (renderPassCreateInfo->depthTargetDescription == NULL)
    {
        subpass.pDepthStencilAttachment = NULL;
    }
    else
    {
        attachmentDescriptions[attachmentDescriptionCount].flags = 0;
        attachmentDescriptions[attachmentDescriptionCount].format = RefreshToVK_DepthFormat[
            renderPassCreateInfo->depthTargetDescription->depthFormat
        ];
        attachmentDescriptions[attachmentDescriptionCount].samples =
            VK_SAMPLE_COUNT_1_BIT; /* FIXME: do these take multisamples? */
        attachmentDescriptions[attachmentDescriptionCount].loadOp = RefreshToVK_LoadOp[
            renderPassCreateInfo->depthTargetDescription->loadOp
        ];
        attachmentDescriptions[attachmentDescriptionCount].storeOp = RefreshToVK_StoreOp[
            renderPassCreateInfo->depthTargetDescription->storeOp
        ];
        attachmentDescriptions[attachmentDescriptionCount].stencilLoadOp = RefreshToVK_LoadOp[
            renderPassCreateInfo->depthTargetDescription->stencilLoadOp
        ];
        attachmentDescriptions[attachmentDescriptionCount].stencilStoreOp = RefreshToVK_StoreOp[
            renderPassCreateInfo->depthTargetDescription->stencilStoreOp
        ];
        attachmentDescriptions[attachmentDescriptionCount].initialLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachmentDescriptions[attachmentDescriptionCount].finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depthStencilAttachmentReference.attachment =
            attachmentDescriptionCount;
        depthStencilAttachmentReference.layout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        subpass.pDepthStencilAttachment =
            &depthStencilAttachmentReference;

        attachmentDescriptionCount += 1;
    }

	if (multisampling)
	{
		subpass.pResolveAttachments = resolveReferences;
	}
	else
	{
		subpass.pResolveAttachments = NULL;
	}

    vkRenderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    vkRenderPassCreateInfo.pNext = NULL;
    vkRenderPassCreateInfo.flags = 0;
    vkRenderPassCreateInfo.pAttachments = attachmentDescriptions;
    vkRenderPassCreateInfo.attachmentCount = attachmentDescriptionCount;
    vkRenderPassCreateInfo.subpassCount = 1;
    vkRenderPassCreateInfo.pSubpasses = &subpass;
    vkRenderPassCreateInfo.dependencyCount = 0;
    vkRenderPassCreateInfo.pDependencies = NULL;

    vulkanResult = renderer->vkCreateRenderPass(
        renderer->logicalDevice,
        &vkRenderPassCreateInfo,
        NULL,
        &renderPass
    );

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateRenderPass", vulkanResult);
		return NULL_RENDER_PASS;
	}

    return (REFRESH_RenderPass*) renderPass;
}

static uint8_t VULKAN_INTERNAL_CreateSamplerDescriptorPool(
	VulkanRenderer *renderer,
	VkDescriptorType descriptorType,
	uint32_t descriptorSetCount,
	uint32_t descriptorCount,
	VkDescriptorPool *pDescriptorPool
) {
	VkResult vulkanResult;

	VkDescriptorPoolSize descriptorPoolSize;
	VkDescriptorPoolCreateInfo descriptorPoolInfo;

	descriptorPoolSize.type = descriptorType;
	descriptorPoolSize.descriptorCount = descriptorCount;

	descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptorPoolInfo.pNext = NULL;
	descriptorPoolInfo.flags = 0;
	descriptorPoolInfo.maxSets = descriptorSetCount;
	descriptorPoolInfo.poolSizeCount = 1;
	descriptorPoolInfo.pPoolSizes = &descriptorPoolSize;

	vulkanResult = renderer->vkCreateDescriptorPool(
		renderer->logicalDevice,
		&descriptorPoolInfo,
		NULL,
		pDescriptorPool
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateDescriptorPool", vulkanResult);
		return 0;
	}

	return 1;
}

static uint8_t VULKAN_INTERNAL_AllocateSamplerDescriptorSets(
	VulkanRenderer *renderer,
	VkDescriptorPool descriptorPool,
	VkDescriptorSetLayout descriptorSetLayout,
	uint32_t descriptorSetCount,
	VkDescriptorSet *descriptorSetArray
) {
	VkResult vulkanResult;
	uint32_t i;
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo;
	VkDescriptorSetLayout *descriptorSetLayouts = SDL_stack_alloc(VkDescriptorSetLayout, descriptorSetCount);

	for (i = 0; i < descriptorSetCount; i += 1)
	{
		descriptorSetLayouts[i] = descriptorSetLayout;
	}

	descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetAllocateInfo.pNext = NULL;
	descriptorSetAllocateInfo.descriptorPool = descriptorPool;
	descriptorSetAllocateInfo.descriptorSetCount = descriptorSetCount;
	descriptorSetAllocateInfo.pSetLayouts = descriptorSetLayouts;

	vulkanResult = renderer->vkAllocateDescriptorSets(
		renderer->logicalDevice,
		&descriptorSetAllocateInfo,
		descriptorSetArray
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkAllocateDescriptorSets", vulkanResult);
		SDL_stack_free(descriptorSetLayouts);
		return 0;
	}

	SDL_stack_free(descriptorSetLayouts);
	return 1;
}

static SamplerDescriptorSetCache* VULKAN_INTERNAL_CreateSamplerDescriptorSetCache(
	VulkanRenderer *renderer,
	VkDescriptorSetLayout descriptorSetLayout,
	uint32_t samplerBindingCount
) {
	uint32_t i;
	SamplerDescriptorSetCache *samplerDescriptorSetCache = SDL_malloc(sizeof(SamplerDescriptorSetCache));

	samplerDescriptorSetCache->elements = SDL_malloc(sizeof(SamplerDescriptorSetHashMap) * 16);
	samplerDescriptorSetCache->count = 0;
	samplerDescriptorSetCache->capacity = 16;

	for (i = 0; i < NUM_DESCRIPTOR_SET_HASH_BUCKETS; i += 1)
	{
		samplerDescriptorSetCache->buckets[i].elements = NULL;
		samplerDescriptorSetCache->buckets[i].count = 0;
		samplerDescriptorSetCache->buckets[i].capacity = 0;
	}

	samplerDescriptorSetCache->descriptorSetLayout = descriptorSetLayout;
	samplerDescriptorSetCache->samplerBindingCount = samplerBindingCount;

	samplerDescriptorSetCache->samplerDescriptorPools = SDL_malloc(sizeof(VkDescriptorPool));
	samplerDescriptorSetCache->samplerDescriptorPoolCount = 1;

	VULKAN_INTERNAL_CreateSamplerDescriptorPool(
		renderer,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		SAMPLER_POOL_STARTING_SIZE,
		SAMPLER_POOL_STARTING_SIZE * samplerBindingCount,
		&samplerDescriptorSetCache->samplerDescriptorPools[0]
	);

	samplerDescriptorSetCache->samplerDescriptorPoolCount = 1;
	samplerDescriptorSetCache->nextPoolSize = SAMPLER_POOL_STARTING_SIZE * 2;

	samplerDescriptorSetCache->inactiveDescriptorSetCapacity = SAMPLER_POOL_STARTING_SIZE;
	samplerDescriptorSetCache->inactiveDescriptorSetCount = SAMPLER_POOL_STARTING_SIZE;
	samplerDescriptorSetCache->inactiveDescriptorSets = SDL_malloc(
		sizeof(VkDescriptorSet) * SAMPLER_POOL_STARTING_SIZE
	);

	VULKAN_INTERNAL_AllocateSamplerDescriptorSets(
		renderer,
		samplerDescriptorSetCache->samplerDescriptorPools[0],
		samplerDescriptorSetCache->descriptorSetLayout,
		SAMPLER_POOL_STARTING_SIZE,
		samplerDescriptorSetCache->inactiveDescriptorSets
	);

	return samplerDescriptorSetCache;
}

static VkDescriptorSetLayout VULKAN_INTERNAL_FetchSamplerDescriptorSetLayout(
	VulkanRenderer *renderer,
	VkShaderStageFlagBits shaderStageFlagBit,
	uint32_t samplerBindingCount
) {
	SamplerDescriptorSetLayoutHash descriptorSetLayoutHash;
	VkDescriptorSetLayout descriptorSetLayout;

	VkDescriptorSetLayoutBinding setLayoutBindings[MAX_TEXTURE_SAMPLERS];
	VkDescriptorSetLayoutCreateInfo setLayoutCreateInfo;

	VkResult vulkanResult;
	uint32_t i;

	if (samplerBindingCount == 0)
	{
		if (shaderStageFlagBit == VK_SHADER_STAGE_VERTEX_BIT)
		{
			return renderer->emptyVertexSamplerLayout;
		}
		else if (shaderStageFlagBit == VK_SHADER_STAGE_FRAGMENT_BIT)
		{
			return renderer->emptyFragmentSamplerLayout;
		}
		else
		{
			REFRESH_LogError("Invalid shader stage flag bit: ", shaderStageFlagBit);
			return NULL_DESC_LAYOUT;
		}
	}

	descriptorSetLayoutHash.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorSetLayoutHash.stageFlag = shaderStageFlagBit;
	descriptorSetLayoutHash.samplerBindingCount = samplerBindingCount;

	descriptorSetLayout = SamplerDescriptorSetLayoutHashTable_Fetch(
		&renderer->samplerDescriptorSetLayoutHashTable,
		descriptorSetLayoutHash
	);

	if (descriptorSetLayout != VK_NULL_HANDLE)
	{
		return descriptorSetLayout;
	}

	for (i = 0; i < samplerBindingCount; i += 1)
	{
		setLayoutBindings[i].binding = i;
		setLayoutBindings[i].descriptorCount = 1;
		setLayoutBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		setLayoutBindings[i].stageFlags = shaderStageFlagBit;
		setLayoutBindings[i].pImmutableSamplers = NULL;
	}

	setLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutCreateInfo.pNext = NULL;
	setLayoutCreateInfo.flags = 0;
	setLayoutCreateInfo.bindingCount = samplerBindingCount;
	setLayoutCreateInfo.pBindings = setLayoutBindings;

	vulkanResult = renderer->vkCreateDescriptorSetLayout(
		renderer->logicalDevice,
		&setLayoutCreateInfo,
		NULL,
		&descriptorSetLayout
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateDescriptorSetLayout", vulkanResult);
		return NULL_DESC_LAYOUT;
	}

	SamplerDescriptorSetLayoutHashTable_Insert(
		&renderer->samplerDescriptorSetLayoutHashTable,
		descriptorSetLayoutHash,
		descriptorSetLayout
	);

	return descriptorSetLayout;
}

static VulkanGraphicsPipelineLayout* VULKAN_INTERNAL_FetchGraphicsPipelineLayout(
	VulkanRenderer *renderer,
	uint32_t vertexSamplerBindingCount,
	uint32_t fragmentSamplerBindingCount
) {
	VkDescriptorSetLayout setLayouts[4];

	PipelineLayoutHash pipelineLayoutHash;
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo;
	VkResult vulkanResult;

	VulkanGraphicsPipelineLayout *vulkanGraphicsPipelineLayout;

	pipelineLayoutHash.vertexSamplerLayout = VULKAN_INTERNAL_FetchSamplerDescriptorSetLayout(
		renderer,
		VK_SHADER_STAGE_VERTEX_BIT,
		vertexSamplerBindingCount
	);

	pipelineLayoutHash.fragmentSamplerLayout = VULKAN_INTERNAL_FetchSamplerDescriptorSetLayout(
		renderer,
		VK_SHADER_STAGE_FRAGMENT_BIT,
		fragmentSamplerBindingCount
	);

	pipelineLayoutHash.vertexUniformLayout = renderer->vertexParamLayout;
	pipelineLayoutHash.fragmentUniformLayout = renderer->fragmentParamLayout;

	vulkanGraphicsPipelineLayout = PipelineLayoutHashArray_Fetch(
		&renderer->pipelineLayoutHashTable,
		pipelineLayoutHash
	);

	if (vulkanGraphicsPipelineLayout != VK_NULL_HANDLE)
	{
		return vulkanGraphicsPipelineLayout;
	}

	vulkanGraphicsPipelineLayout = SDL_malloc(sizeof(VulkanGraphicsPipelineLayout));

	setLayouts[0] = pipelineLayoutHash.vertexSamplerLayout;
	setLayouts[1] = pipelineLayoutHash.fragmentSamplerLayout;
	setLayouts[2] = renderer->vertexParamLayout;
	setLayouts[3] = renderer->fragmentParamLayout;

	pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutCreateInfo.pNext = NULL;
	pipelineLayoutCreateInfo.flags = 0;
	pipelineLayoutCreateInfo.setLayoutCount = 4;
	pipelineLayoutCreateInfo.pSetLayouts = setLayouts;
	pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
	pipelineLayoutCreateInfo.pPushConstantRanges = NULL;

	vulkanResult = renderer->vkCreatePipelineLayout(
		renderer->logicalDevice,
		&pipelineLayoutCreateInfo,
		NULL,
		&vulkanGraphicsPipelineLayout->pipelineLayout
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreatePipelineLayout", vulkanResult);
		return NULL;
	}

	PipelineLayoutHashArray_Insert(
		&renderer->pipelineLayoutHashTable,
		pipelineLayoutHash,
		vulkanGraphicsPipelineLayout
	);

	/* If the binding count is 0
	 * we can just bind the same descriptor set
	 * so no cache is needed
	 */

	if (vertexSamplerBindingCount == 0)
	{
		vulkanGraphicsPipelineLayout->vertexSamplerDescriptorSetCache = NULL;
	}
	else
	{
		vulkanGraphicsPipelineLayout->vertexSamplerDescriptorSetCache =
			VULKAN_INTERNAL_CreateSamplerDescriptorSetCache(
				renderer,
				pipelineLayoutHash.vertexSamplerLayout,
				vertexSamplerBindingCount
			);
	}

	if (fragmentSamplerBindingCount == 0)
	{
		vulkanGraphicsPipelineLayout->fragmentSamplerDescriptorSetCache = NULL;
	}
	else
	{
		vulkanGraphicsPipelineLayout->fragmentSamplerDescriptorSetCache =
			VULKAN_INTERNAL_CreateSamplerDescriptorSetCache(
				renderer,
				pipelineLayoutHash.fragmentSamplerLayout,
				fragmentSamplerBindingCount
			);
	}

	return vulkanGraphicsPipelineLayout;
}

static REFRESH_GraphicsPipeline* VULKAN_CreateGraphicsPipeline(
	REFRESH_Renderer *driverData,
	REFRESH_GraphicsPipelineCreateInfo *pipelineCreateInfo
) {
	VkResult vulkanResult;
	uint32_t i;

	VulkanGraphicsPipeline *graphicsPipeline = (VulkanGraphicsPipeline*) SDL_malloc(sizeof(VulkanGraphicsPipeline));
	VkGraphicsPipelineCreateInfo vkPipelineCreateInfo;

	VkPipelineShaderStageCreateInfo shaderStageCreateInfos[2];

	VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo;
	VkVertexInputBindingDescription *vertexInputBindingDescriptions = SDL_stack_alloc(VkVertexInputBindingDescription, pipelineCreateInfo->vertexInputState.vertexBindingCount);
	VkVertexInputAttributeDescription *vertexInputAttributeDescriptions = SDL_stack_alloc(VkVertexInputAttributeDescription, pipelineCreateInfo->vertexInputState.vertexAttributeCount);

	VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo;

	VkPipelineViewportStateCreateInfo viewportStateCreateInfo;
	VkViewport *viewports = SDL_stack_alloc(VkViewport, pipelineCreateInfo->viewportState.viewportCount);
	VkRect2D *scissors = SDL_stack_alloc(VkRect2D, pipelineCreateInfo->viewportState.scissorCount);

	VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo;

	VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo;

	VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo;
	VkStencilOpState frontStencilState;
	VkStencilOpState backStencilState;

	VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo;
	VkPipelineColorBlendAttachmentState *colorBlendAttachmentStates = SDL_stack_alloc(
		VkPipelineColorBlendAttachmentState,
		pipelineCreateInfo->colorBlendState.blendStateCount
	);

	VkDescriptorSetAllocateInfo vertexUBODescriptorAllocateInfo;
	VkDescriptorSetAllocateInfo fragmentUBODescriptorAllocateInfo;

	VkWriteDescriptorSet uboWriteDescriptorSets[2];
	VkDescriptorBufferInfo vertexUniformBufferInfo;
	VkDescriptorBufferInfo fragmentUniformBufferInfo;

	VulkanRenderer *renderer = (VulkanRenderer*) driverData;

	/* Shader stages */

	shaderStageCreateInfos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStageCreateInfos[0].pNext = NULL;
	shaderStageCreateInfos[0].flags = 0;
	shaderStageCreateInfos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStageCreateInfos[0].module = (VkShaderModule) pipelineCreateInfo->vertexShaderState.shaderModule;
	shaderStageCreateInfos[0].pName = pipelineCreateInfo->vertexShaderState.entryPointName;
	shaderStageCreateInfos[0].pSpecializationInfo = NULL;

	graphicsPipeline->vertexUBOBlockSize =
		VULKAN_INTERNAL_NextHighestAlignment(
			pipelineCreateInfo->vertexShaderState.uniformBufferSize,
			renderer->minUBOAlignment
		);

	shaderStageCreateInfos[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStageCreateInfos[1].pNext = NULL;
	shaderStageCreateInfos[1].flags = 0;
	shaderStageCreateInfos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStageCreateInfos[1].module = (VkShaderModule) pipelineCreateInfo->fragmentShaderState.shaderModule;
	shaderStageCreateInfos[1].pName = pipelineCreateInfo->fragmentShaderState.entryPointName;
	shaderStageCreateInfos[1].pSpecializationInfo = NULL;

	graphicsPipeline->fragmentUBOBlockSize =
		VULKAN_INTERNAL_NextHighestAlignment(
			pipelineCreateInfo->fragmentShaderState.uniformBufferSize,
			renderer->minUBOAlignment
		);

	/* Vertex input */

	for (i = 0; i < pipelineCreateInfo->vertexInputState.vertexBindingCount; i += 1)
	{
		vertexInputBindingDescriptions[i].binding = pipelineCreateInfo->vertexInputState.vertexBindings[i].binding;
		vertexInputBindingDescriptions[i].inputRate = RefreshToVK_VertexInputRate[
			pipelineCreateInfo->vertexInputState.vertexBindings[i].inputRate
		];
		vertexInputBindingDescriptions[i].stride = pipelineCreateInfo->vertexInputState.vertexBindings[i].stride;
	}

	for (i = 0; i < pipelineCreateInfo->vertexInputState.vertexAttributeCount; i += 1)
	{
		vertexInputAttributeDescriptions[i].binding = pipelineCreateInfo->vertexInputState.vertexAttributes[i].binding;
		vertexInputAttributeDescriptions[i].format = RefreshToVK_VertexFormat[
			pipelineCreateInfo->vertexInputState.vertexAttributes[i].format
		];
		vertexInputAttributeDescriptions[i].location = pipelineCreateInfo->vertexInputState.vertexAttributes[i].location;
		vertexInputAttributeDescriptions[i].offset = pipelineCreateInfo->vertexInputState.vertexAttributes[i].offset;
	}

	vertexInputStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputStateCreateInfo.pNext = NULL;
	vertexInputStateCreateInfo.flags = 0;
	vertexInputStateCreateInfo.vertexBindingDescriptionCount = pipelineCreateInfo->vertexInputState.vertexBindingCount;
	vertexInputStateCreateInfo.pVertexBindingDescriptions = vertexInputBindingDescriptions;
	vertexInputStateCreateInfo.vertexAttributeDescriptionCount = pipelineCreateInfo->vertexInputState.vertexAttributeCount;
	vertexInputStateCreateInfo.pVertexAttributeDescriptions = vertexInputAttributeDescriptions;

	/* Topology */

	inputAssemblyStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssemblyStateCreateInfo.pNext = NULL;
	inputAssemblyStateCreateInfo.flags = 0;
	inputAssemblyStateCreateInfo.primitiveRestartEnable = VK_FALSE;
	inputAssemblyStateCreateInfo.topology = RefreshToVK_PrimitiveType[
		pipelineCreateInfo->topologyState.topology
	];

	graphicsPipeline->primitiveType = pipelineCreateInfo->topologyState.topology;

	/* Viewport */

	for (i = 0; i < pipelineCreateInfo->viewportState.viewportCount; i += 1)
	{
		viewports[i].x = pipelineCreateInfo->viewportState.viewports[i].x;
		viewports[i].y = pipelineCreateInfo->viewportState.viewports[i].y;
		viewports[i].width = pipelineCreateInfo->viewportState.viewports[i].w;
		viewports[i].height = pipelineCreateInfo->viewportState.viewports[i].h;
		viewports[i].minDepth = pipelineCreateInfo->viewportState.viewports[i].minDepth;
		viewports[i].maxDepth = pipelineCreateInfo->viewportState.viewports[i].maxDepth;
	}

	for (i = 0; i < pipelineCreateInfo->viewportState.scissorCount; i += 1)
	{
		scissors[i].offset.x = pipelineCreateInfo->viewportState.scissors[i].x;
		scissors[i].offset.y = pipelineCreateInfo->viewportState.scissors[i].y;
		scissors[i].extent.width = pipelineCreateInfo->viewportState.scissors[i].w;
		scissors[i].extent.height = pipelineCreateInfo->viewportState.scissors[i].h;
	}

	viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportStateCreateInfo.pNext = NULL;
	viewportStateCreateInfo.flags = 0;
	viewportStateCreateInfo.viewportCount = pipelineCreateInfo->viewportState.viewportCount;
	viewportStateCreateInfo.pViewports = viewports;
	viewportStateCreateInfo.scissorCount = pipelineCreateInfo->viewportState.scissorCount;
	viewportStateCreateInfo.pScissors = scissors;

	/* Rasterization */

	rasterizationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizationStateCreateInfo.pNext = NULL;
	rasterizationStateCreateInfo.flags = 0;
	rasterizationStateCreateInfo.depthClampEnable = pipelineCreateInfo->rasterizerState.depthClampEnable;
	rasterizationStateCreateInfo.rasterizerDiscardEnable = VK_FALSE;
	rasterizationStateCreateInfo.polygonMode = RefreshToVK_PolygonMode[
		pipelineCreateInfo->rasterizerState.fillMode
	];
	rasterizationStateCreateInfo.cullMode = RefreshToVK_CullMode[
		pipelineCreateInfo->rasterizerState.cullMode
	];
	rasterizationStateCreateInfo.frontFace = RefreshToVK_FrontFace[
		pipelineCreateInfo->rasterizerState.frontFace
	];
	rasterizationStateCreateInfo.depthBiasEnable =
		pipelineCreateInfo->rasterizerState.depthBiasEnable;
	rasterizationStateCreateInfo.depthBiasConstantFactor =
		pipelineCreateInfo->rasterizerState.depthBiasConstantFactor;
	rasterizationStateCreateInfo.depthBiasClamp =
		pipelineCreateInfo->rasterizerState.depthBiasClamp;
	rasterizationStateCreateInfo.depthBiasSlopeFactor =
		pipelineCreateInfo->rasterizerState.depthBiasSlopeFactor;
	rasterizationStateCreateInfo.lineWidth =
		pipelineCreateInfo->rasterizerState.lineWidth;

	/* Multisample */

	multisampleStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampleStateCreateInfo.pNext = NULL;
	multisampleStateCreateInfo.flags = 0;
	multisampleStateCreateInfo.rasterizationSamples = RefreshToVK_SampleCount[
		pipelineCreateInfo->multisampleState.multisampleCount
	];
	multisampleStateCreateInfo.sampleShadingEnable = VK_FALSE;
	multisampleStateCreateInfo.minSampleShading = 1.0f;
	multisampleStateCreateInfo.pSampleMask =
		pipelineCreateInfo->multisampleState.sampleMask;
	multisampleStateCreateInfo.alphaToCoverageEnable = VK_FALSE;
	multisampleStateCreateInfo.alphaToOneEnable = VK_FALSE;

	/* Depth Stencil State */

	frontStencilState.failOp = RefreshToVK_StencilOp[
		pipelineCreateInfo->depthStencilState.frontStencilState.failOp
	];
	frontStencilState.passOp = RefreshToVK_StencilOp[
		pipelineCreateInfo->depthStencilState.frontStencilState.passOp
	];
	frontStencilState.depthFailOp = RefreshToVK_StencilOp[
		pipelineCreateInfo->depthStencilState.frontStencilState.depthFailOp
	];
	frontStencilState.compareOp = RefreshToVK_CompareOp[
		pipelineCreateInfo->depthStencilState.frontStencilState.compareOp
	];
	frontStencilState.compareMask =
		pipelineCreateInfo->depthStencilState.frontStencilState.compareMask;
	frontStencilState.writeMask =
		pipelineCreateInfo->depthStencilState.frontStencilState.writeMask;
	frontStencilState.reference =
		pipelineCreateInfo->depthStencilState.frontStencilState.reference;

	backStencilState.failOp = RefreshToVK_StencilOp[
		pipelineCreateInfo->depthStencilState.backStencilState.failOp
	];
	backStencilState.passOp = RefreshToVK_StencilOp[
		pipelineCreateInfo->depthStencilState.backStencilState.passOp
	];
	backStencilState.depthFailOp = RefreshToVK_StencilOp[
		pipelineCreateInfo->depthStencilState.backStencilState.depthFailOp
	];
	backStencilState.compareOp = RefreshToVK_CompareOp[
		pipelineCreateInfo->depthStencilState.backStencilState.compareOp
	];
	backStencilState.compareMask =
		pipelineCreateInfo->depthStencilState.backStencilState.compareMask;
	backStencilState.writeMask =
		pipelineCreateInfo->depthStencilState.backStencilState.writeMask;
	backStencilState.reference =
		pipelineCreateInfo->depthStencilState.backStencilState.reference;


	depthStencilStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilStateCreateInfo.pNext = NULL;
	depthStencilStateCreateInfo.flags = 0;
	depthStencilStateCreateInfo.depthTestEnable =
		pipelineCreateInfo->depthStencilState.depthTestEnable;
	depthStencilStateCreateInfo.depthWriteEnable =
		pipelineCreateInfo->depthStencilState.depthWriteEnable;
	depthStencilStateCreateInfo.depthCompareOp = RefreshToVK_CompareOp[
		pipelineCreateInfo->depthStencilState.compareOp
	];
	depthStencilStateCreateInfo.depthBoundsTestEnable =
		pipelineCreateInfo->depthStencilState.depthBoundsTestEnable;
	depthStencilStateCreateInfo.stencilTestEnable =
		pipelineCreateInfo->depthStencilState.stencilTestEnable;
	depthStencilStateCreateInfo.front = frontStencilState;
	depthStencilStateCreateInfo.back = backStencilState;
	depthStencilStateCreateInfo.minDepthBounds =
		pipelineCreateInfo->depthStencilState.minDepthBounds;
	depthStencilStateCreateInfo.maxDepthBounds =
		pipelineCreateInfo->depthStencilState.maxDepthBounds;

	/* Color Blend */

	for (i = 0; i < pipelineCreateInfo->colorBlendState.blendStateCount; i += 1)
	{
		colorBlendAttachmentStates[i].blendEnable =
			pipelineCreateInfo->colorBlendState.blendStates[i].blendEnable;
		colorBlendAttachmentStates[i].srcColorBlendFactor = RefreshToVK_BlendFactor[
			pipelineCreateInfo->colorBlendState.blendStates[i].srcColorBlendFactor
		];
		colorBlendAttachmentStates[i].dstColorBlendFactor = RefreshToVK_BlendFactor[
			pipelineCreateInfo->colorBlendState.blendStates[i].dstColorBlendFactor
		];
		colorBlendAttachmentStates[i].colorBlendOp = RefreshToVK_BlendOp[
			pipelineCreateInfo->colorBlendState.blendStates[i].colorBlendOp
		];
		colorBlendAttachmentStates[i].srcAlphaBlendFactor = RefreshToVK_BlendFactor[
			pipelineCreateInfo->colorBlendState.blendStates[i].srcAlphaBlendFactor
		];
		colorBlendAttachmentStates[i].dstAlphaBlendFactor = RefreshToVK_BlendFactor[
			pipelineCreateInfo->colorBlendState.blendStates[i].dstAlphaBlendFactor
		];
		colorBlendAttachmentStates[i].alphaBlendOp = RefreshToVK_BlendOp[
			pipelineCreateInfo->colorBlendState.blendStates[i].alphaBlendOp
		];
		colorBlendAttachmentStates[i].colorWriteMask =
			pipelineCreateInfo->colorBlendState.blendStates[i].colorWriteMask;
	}

	colorBlendStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlendStateCreateInfo.pNext = NULL;
	colorBlendStateCreateInfo.flags = 0;
	colorBlendStateCreateInfo.logicOpEnable =
		pipelineCreateInfo->colorBlendState.blendOpEnable;
	colorBlendStateCreateInfo.logicOp = RefreshToVK_LogicOp[
		pipelineCreateInfo->colorBlendState.logicOp
	];
	colorBlendStateCreateInfo.attachmentCount =
		pipelineCreateInfo->colorBlendState.blendStateCount;
	colorBlendStateCreateInfo.pAttachments =
		colorBlendAttachmentStates;
	colorBlendStateCreateInfo.blendConstants[0] =
		pipelineCreateInfo->colorBlendState.blendConstants[0];
	colorBlendStateCreateInfo.blendConstants[1] =
		pipelineCreateInfo->colorBlendState.blendConstants[1];
	colorBlendStateCreateInfo.blendConstants[2] =
		pipelineCreateInfo->colorBlendState.blendConstants[2];
	colorBlendStateCreateInfo.blendConstants[3] =
		pipelineCreateInfo->colorBlendState.blendConstants[3];

	/* Pipeline Layout */

	graphicsPipeline->pipelineLayout = VULKAN_INTERNAL_FetchGraphicsPipelineLayout(
		renderer,
		pipelineCreateInfo->pipelineLayoutCreateInfo.vertexSamplerBindingCount,
		pipelineCreateInfo->pipelineLayoutCreateInfo.fragmentSamplerBindingCount
	);

	/* Pipeline */

	vkPipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	vkPipelineCreateInfo.pNext = NULL;
	vkPipelineCreateInfo.flags = 0;
	vkPipelineCreateInfo.stageCount = 2;
	vkPipelineCreateInfo.pStages = shaderStageCreateInfos;
	vkPipelineCreateInfo.pVertexInputState = &vertexInputStateCreateInfo;
	vkPipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo;
	vkPipelineCreateInfo.pTessellationState = VK_NULL_HANDLE;
	vkPipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
	vkPipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo;
	vkPipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo;
	vkPipelineCreateInfo.pDepthStencilState = &depthStencilStateCreateInfo;
	vkPipelineCreateInfo.pColorBlendState = &colorBlendStateCreateInfo;
	vkPipelineCreateInfo.pDynamicState = VK_NULL_HANDLE;
	vkPipelineCreateInfo.layout = graphicsPipeline->pipelineLayout->pipelineLayout;
	vkPipelineCreateInfo.renderPass = (VkRenderPass) pipelineCreateInfo->renderPass;
	vkPipelineCreateInfo.subpass = 0;
	vkPipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
	vkPipelineCreateInfo.basePipelineIndex = 0;

	/* TODO: enable pipeline caching */
	vulkanResult = renderer->vkCreateGraphicsPipelines(
		renderer->logicalDevice,
		VK_NULL_HANDLE,
		1,
		&vkPipelineCreateInfo,
		NULL,
		&graphicsPipeline->pipeline
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateGraphicsPipelines", vulkanResult);
		REFRESH_LogError("Failed to create graphics pipeline!");

		SDL_stack_free(vertexInputBindingDescriptions);
		SDL_stack_free(vertexInputAttributeDescriptions);
		SDL_stack_free(viewports);
		SDL_stack_free(scissors);
		SDL_stack_free(colorBlendAttachmentStates);
		return NULL;
	}

	SDL_stack_free(vertexInputBindingDescriptions);
	SDL_stack_free(vertexInputAttributeDescriptions);
	SDL_stack_free(viewports);
	SDL_stack_free(scissors);
	SDL_stack_free(colorBlendAttachmentStates);

	/* Allocate uniform buffer descriptors */

	vertexUBODescriptorAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	vertexUBODescriptorAllocateInfo.pNext = NULL;
	vertexUBODescriptorAllocateInfo.descriptorPool = renderer->defaultDescriptorPool;
	vertexUBODescriptorAllocateInfo.descriptorSetCount = 1;
	vertexUBODescriptorAllocateInfo.pSetLayouts = &renderer->vertexParamLayout;

	renderer->vkAllocateDescriptorSets(
		renderer->logicalDevice,
		&vertexUBODescriptorAllocateInfo,
		&graphicsPipeline->vertexUBODescriptorSet
	);

	fragmentUBODescriptorAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	fragmentUBODescriptorAllocateInfo.pNext = NULL;
	fragmentUBODescriptorAllocateInfo.descriptorPool = renderer->defaultDescriptorPool;
	fragmentUBODescriptorAllocateInfo.descriptorSetCount = 1;
	fragmentUBODescriptorAllocateInfo.pSetLayouts = &renderer->fragmentParamLayout;

	renderer->vkAllocateDescriptorSets(
		renderer->logicalDevice,
		&fragmentUBODescriptorAllocateInfo,
		&graphicsPipeline->fragmentUBODescriptorSet
	);

	if (graphicsPipeline->vertexUBOBlockSize == 0)
	{
		vertexUniformBufferInfo.buffer = renderer->dummyVertexUniformBuffer->subBuffers[0]->buffer;
		vertexUniformBufferInfo.offset = 0;
		vertexUniformBufferInfo.range = renderer->dummyVertexUniformBuffer->subBuffers[0]->size;
	}
	else
	{
		vertexUniformBufferInfo.buffer = renderer->vertexUBO->subBuffers[0]->buffer;
		vertexUniformBufferInfo.offset = 0;
		vertexUniformBufferInfo.range = graphicsPipeline->vertexUBOBlockSize;
	}

	if (graphicsPipeline->fragmentUBOBlockSize == 0)
	{
		fragmentUniformBufferInfo.buffer = renderer->dummyFragmentUniformBuffer->subBuffers[0]->buffer;
		fragmentUniformBufferInfo.offset = 0;
		fragmentUniformBufferInfo.range = renderer->dummyFragmentUniformBuffer->subBuffers[0]->size;
	}
	else
	{
		fragmentUniformBufferInfo.buffer = renderer->fragmentUBO->subBuffers[0]->buffer;
		fragmentUniformBufferInfo.offset = 0;
		fragmentUniformBufferInfo.range = graphicsPipeline->fragmentUBOBlockSize;
	}

	uboWriteDescriptorSets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	uboWriteDescriptorSets[0].pNext = NULL;
	uboWriteDescriptorSets[0].descriptorCount = 1;
	uboWriteDescriptorSets[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	uboWriteDescriptorSets[0].dstArrayElement = 0;
	uboWriteDescriptorSets[0].dstBinding = 0;
	uboWriteDescriptorSets[0].dstSet = graphicsPipeline->vertexUBODescriptorSet;
	uboWriteDescriptorSets[0].pBufferInfo = &vertexUniformBufferInfo;
	uboWriteDescriptorSets[0].pImageInfo = NULL;
	uboWriteDescriptorSets[0].pTexelBufferView = NULL;

	uboWriteDescriptorSets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	uboWriteDescriptorSets[1].pNext = NULL;
	uboWriteDescriptorSets[1].descriptorCount = 1;
	uboWriteDescriptorSets[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	uboWriteDescriptorSets[1].dstArrayElement = 0;
	uboWriteDescriptorSets[1].dstBinding = 0;
	uboWriteDescriptorSets[1].dstSet = graphicsPipeline->fragmentUBODescriptorSet;
	uboWriteDescriptorSets[1].pBufferInfo = &fragmentUniformBufferInfo;
	uboWriteDescriptorSets[1].pImageInfo = NULL;
	uboWriteDescriptorSets[1].pTexelBufferView = NULL;

	renderer->vkUpdateDescriptorSets(
		renderer->logicalDevice,
		2,
		uboWriteDescriptorSets,
		0,
		NULL
	);

	return (REFRESH_GraphicsPipeline*) graphicsPipeline;
}

static REFRESH_Sampler* VULKAN_CreateSampler(
	REFRESH_Renderer *driverData,
	REFRESH_SamplerStateCreateInfo *samplerStateCreateInfo
) {
	VkResult vulkanResult;
	VkSampler sampler;

	VulkanRenderer* renderer = (VulkanRenderer*)driverData;

	VkSamplerCreateInfo vkSamplerCreateInfo;
	vkSamplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	vkSamplerCreateInfo.pNext = NULL;
	vkSamplerCreateInfo.flags = 0;
	vkSamplerCreateInfo.magFilter = RefreshToVK_SamplerFilter[
		samplerStateCreateInfo->magFilter
	];
	vkSamplerCreateInfo.minFilter = RefreshToVK_SamplerFilter[
		samplerStateCreateInfo->minFilter
	];
	vkSamplerCreateInfo.mipmapMode = RefreshToVK_SamplerMipmapMode[
		samplerStateCreateInfo->mipmapMode
	];
	vkSamplerCreateInfo.addressModeU = RefreshToVK_SamplerAddressMode[
		samplerStateCreateInfo->addressModeU
	];
	vkSamplerCreateInfo.addressModeV = RefreshToVK_SamplerAddressMode[
		samplerStateCreateInfo->addressModeV
	];
	vkSamplerCreateInfo.addressModeW = RefreshToVK_SamplerAddressMode[
		samplerStateCreateInfo->addressModeW
	];
	vkSamplerCreateInfo.mipLodBias = samplerStateCreateInfo->mipLodBias;
	vkSamplerCreateInfo.anisotropyEnable = samplerStateCreateInfo->anisotropyEnable;
	vkSamplerCreateInfo.maxAnisotropy = samplerStateCreateInfo->maxAnisotropy;
	vkSamplerCreateInfo.compareEnable = samplerStateCreateInfo->compareEnable;
	vkSamplerCreateInfo.compareOp = RefreshToVK_CompareOp[
		samplerStateCreateInfo->compareOp
	];
	vkSamplerCreateInfo.minLod = samplerStateCreateInfo->minLod;
	vkSamplerCreateInfo.maxLod = samplerStateCreateInfo->maxLod;
	vkSamplerCreateInfo.borderColor = RefreshToVK_BorderColor[
		samplerStateCreateInfo->borderColor
	];
	vkSamplerCreateInfo.unnormalizedCoordinates = VK_FALSE;

	vulkanResult = renderer->vkCreateSampler(
		renderer->logicalDevice,
		&vkSamplerCreateInfo,
		NULL,
		&sampler
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateSampler", vulkanResult);
		return NULL;
	}

	return (REFRESH_Sampler*) sampler;
}

static REFRESH_Framebuffer* VULKAN_CreateFramebuffer(
	REFRESH_Renderer *driverData,
	REFRESH_FramebufferCreateInfo *framebufferCreateInfo
) {
	VkResult vulkanResult;
	VkFramebufferCreateInfo vkFramebufferCreateInfo;

	VkImageView *imageViews;
	uint32_t colorAttachmentCount = framebufferCreateInfo->colorTargetCount;
	uint32_t attachmentCount = colorAttachmentCount;
	uint32_t i;

	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanFramebuffer *vulkanFramebuffer = (VulkanFramebuffer*) SDL_malloc(sizeof(VulkanFramebuffer));

	if (framebufferCreateInfo->pDepthStencilTarget != NULL)
	{
		attachmentCount += 1;
	}

	imageViews = SDL_stack_alloc(VkImageView, attachmentCount);

	for (i = 0; i < colorAttachmentCount; i += 1)
	{
		imageViews[i] = ((VulkanColorTarget*)framebufferCreateInfo->pColorTargets[i])->view;
	}

	if (framebufferCreateInfo->pDepthStencilTarget != NULL)
	{
		imageViews[colorAttachmentCount] = ((VulkanDepthStencilTarget*)framebufferCreateInfo->pDepthStencilTarget)->view;
	}

	vkFramebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	vkFramebufferCreateInfo.pNext = NULL;
	vkFramebufferCreateInfo.flags = 0;
	vkFramebufferCreateInfo.renderPass = (VkRenderPass) framebufferCreateInfo->renderPass;
	vkFramebufferCreateInfo.attachmentCount = attachmentCount;
	vkFramebufferCreateInfo.pAttachments = imageViews;
	vkFramebufferCreateInfo.width = framebufferCreateInfo->width;
	vkFramebufferCreateInfo.height = framebufferCreateInfo->height;
	vkFramebufferCreateInfo.layers = 1;

	vulkanResult = renderer->vkCreateFramebuffer(
		renderer->logicalDevice,
		&vkFramebufferCreateInfo,
		NULL,
		&vulkanFramebuffer->framebuffer
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateFramebuffer", vulkanResult);
		SDL_stack_free(imageViews);
		return NULL;
	}

	for (i = 0; i < colorAttachmentCount; i += 1)
	{
		vulkanFramebuffer->colorTargets[i] =
			(VulkanColorTarget*) framebufferCreateInfo->pColorTargets[i];
	}

	vulkanFramebuffer->colorTargetCount = colorAttachmentCount;
	vulkanFramebuffer->depthStencilTarget =
		(VulkanDepthStencilTarget*) framebufferCreateInfo->pDepthStencilTarget;

	vulkanFramebuffer->width = framebufferCreateInfo->width;
	vulkanFramebuffer->height = framebufferCreateInfo->height;

	SDL_stack_free(imageViews);
	return (REFRESH_Framebuffer*) vulkanFramebuffer;
}

static REFRESH_ShaderModule* VULKAN_CreateShaderModule(
	REFRESH_Renderer *driverData,
	REFRESH_ShaderModuleCreateInfo *shaderModuleCreateInfo
) {
	VkResult vulkanResult;
	VkShaderModule shaderModule;
	VkShaderModuleCreateInfo vkShaderModuleCreateInfo;
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;

	vkShaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vkShaderModuleCreateInfo.pNext = NULL;
	vkShaderModuleCreateInfo.flags = 0;
	vkShaderModuleCreateInfo.codeSize = shaderModuleCreateInfo->codeSize;
	vkShaderModuleCreateInfo.pCode = (uint32_t*) shaderModuleCreateInfo->byteCode;

	vulkanResult = renderer->vkCreateShaderModule(
		renderer->logicalDevice,
		&vkShaderModuleCreateInfo,
		NULL,
		&shaderModule
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateShaderModule", vulkanResult);
		REFRESH_LogError("Failed to create shader module!");
		return NULL;
	}

	return (REFRESH_ShaderModule*) shaderModule;
}

/* texture should be an alloc'd but uninitialized VulkanTexture */
static uint8_t VULKAN_INTERNAL_CreateTexture(
	VulkanRenderer *renderer,
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	uint32_t isCube,
	VkSampleCountFlagBits samples,
	uint32_t levelCount,
	VkFormat format,
	VkImageAspectFlags aspectMask,
	VkImageTiling tiling,
	VkImageType imageType,
	VkImageUsageFlags imageUsageFlags,
	REFRESH_TextureUsageFlags textureUsageFlags,
	VulkanTexture *texture
) {
	VkResult vulkanResult;
	VkImageCreateInfo imageCreateInfo;
	VkImageCreateFlags imageCreateFlags = 0;
	VkImageViewCreateInfo imageViewCreateInfo;
	uint8_t findMemoryResult;
	uint8_t is3D = depth > 1 ? 1 : 0;
	uint8_t layerCount = isCube ? 6 : 1;
	VkComponentMapping swizzle = IDENTITY_SWIZZLE;

	if (isCube)
	{
		imageCreateFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}
	else if (is3D)
	{
		imageCreateFlags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
	}

	imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageCreateInfo.pNext = NULL;
	imageCreateInfo.flags = imageCreateFlags;
	imageCreateInfo.imageType = imageType;
	imageCreateInfo.format = format;
	imageCreateInfo.extent.width = width;
	imageCreateInfo.extent.height = height;
	imageCreateInfo.extent.depth = depth;
	imageCreateInfo.mipLevels = levelCount;
	imageCreateInfo.arrayLayers = layerCount;
	imageCreateInfo.samples = samples;
	imageCreateInfo.tiling = tiling;
	imageCreateInfo.usage = imageUsageFlags;
	// FIXME: would this interfere with pixel data sharing?
	imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageCreateInfo.queueFamilyIndexCount = 0;
	imageCreateInfo.pQueueFamilyIndices = NULL;
	imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	vulkanResult = renderer->vkCreateImage(
		renderer->logicalDevice,
		&imageCreateInfo,
		NULL,
		&texture->image
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateImage", vulkanResult);
		REFRESH_LogError("Failed to create texture!");
	}

	findMemoryResult = VULKAN_INTERNAL_FindAvailableMemory(
		renderer,
		VK_NULL_HANDLE,
		texture->image,
		&texture->allocation,
		&texture->offset,
		&texture->memorySize
	);

	/* No device memory available, time to die */
	if (findMemoryResult == 0 || findMemoryResult == 2)
	{
		REFRESH_LogError("Failed to find texture memory!");
		return 0;
	}

	vulkanResult = renderer->vkBindImageMemory(
		renderer->logicalDevice,
		texture->image,
		texture->allocation->memory,
		texture->offset
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkBindImageMemory", vulkanResult);
		REFRESH_LogError("Failed to bind texture memory!");
		return 0;
	}

	imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	imageViewCreateInfo.pNext = NULL;
	imageViewCreateInfo.flags = 0;
	imageViewCreateInfo.image = texture->image;
	imageViewCreateInfo.format = format;
	imageViewCreateInfo.components = swizzle;
	imageViewCreateInfo.subresourceRange.aspectMask = aspectMask;
	imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
	imageViewCreateInfo.subresourceRange.levelCount = levelCount;
	imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
	imageViewCreateInfo.subresourceRange.layerCount = layerCount;

	if (isCube)
	{
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	}
	else if (imageType == VK_IMAGE_TYPE_2D)
	{
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	}
	else if (imageType == VK_IMAGE_TYPE_3D)
	{
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
	}
	else
	{
		REFRESH_LogError("invalid image type: %u", imageType);
	}

	vulkanResult = renderer->vkCreateImageView(
		renderer->logicalDevice,
		&imageViewCreateInfo,
		NULL,
		&texture->view
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateImageView", vulkanResult);
		REFRESH_LogError("Failed to create texture image view");
		return 0;
	}

	texture->dimensions.width = width;
	texture->dimensions.height = height;
	texture->depth = depth;
	texture->format = format;
	texture->levelCount = levelCount;
	texture->layerCount = layerCount;
	texture->resourceAccessType = RESOURCE_ACCESS_NONE;
	texture->usageFlags = textureUsageFlags;

	return 1;
}

static REFRESH_Texture* VULKAN_CreateTexture2D(
	REFRESH_Renderer *driverData,
	REFRESH_SurfaceFormat format,
	uint32_t width,
	uint32_t height,
	uint32_t levelCount,
	REFRESH_TextureUsageFlags usageFlags
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *result;
	VkImageUsageFlags imageUsageFlags = (
		VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	);

	if (usageFlags & REFRESH_TEXTUREUSAGE_COLOR_TARGET_BIT)
	{
		imageUsageFlags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	result = (VulkanTexture*) SDL_malloc(sizeof(VulkanTexture));

	VULKAN_INTERNAL_CreateTexture(
		renderer,
		width,
		height,
		1,
		0,
		VK_SAMPLE_COUNT_1_BIT,
		levelCount,
		RefreshToVK_SurfaceFormat[format],
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_TYPE_2D,
		imageUsageFlags,
		usageFlags,
		result
	);
	result->colorFormat = format;

	return (REFRESH_Texture*) result;
}

static REFRESH_Texture* VULKAN_CreateTexture3D(
	REFRESH_Renderer *driverData,
	REFRESH_SurfaceFormat format,
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	uint32_t levelCount,
	REFRESH_TextureUsageFlags usageFlags
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *result;
	VkImageUsageFlags imageUsageFlags = (
		VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	);

	if (usageFlags & REFRESH_TEXTUREUSAGE_COLOR_TARGET_BIT)
	{
		imageUsageFlags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	result = (VulkanTexture*) SDL_malloc(sizeof(VulkanTexture));

	VULKAN_INTERNAL_CreateTexture(
		renderer,
		width,
		height,
		depth,
		0,
		VK_SAMPLE_COUNT_1_BIT,
		levelCount,
		RefreshToVK_SurfaceFormat[format],
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_TYPE_3D,
		imageUsageFlags,
		usageFlags,
		result
	);
	result->colorFormat = format;

	return (REFRESH_Texture*) result;
}

static REFRESH_Texture* VULKAN_CreateTextureCube(
	REFRESH_Renderer *driverData,
	REFRESH_SurfaceFormat format,
	uint32_t size,
	uint32_t levelCount,
	REFRESH_TextureUsageFlags usageFlags
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *result;
	VkImageUsageFlags imageUsageFlags = (
		VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	);

	if (usageFlags & REFRESH_TEXTUREUSAGE_COLOR_TARGET_BIT)
	{
		imageUsageFlags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	result = (VulkanTexture*) SDL_malloc(sizeof(VulkanTexture));

	VULKAN_INTERNAL_CreateTexture(
		renderer,
		size,
		size,
		1,
		1,
		VK_SAMPLE_COUNT_1_BIT,
		levelCount,
		RefreshToVK_SurfaceFormat[format],
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_TYPE_2D,
		imageUsageFlags,
		usageFlags,
		result
	);
	result->colorFormat = format;

	return (REFRESH_Texture*) result;
}

static REFRESH_ColorTarget* VULKAN_CreateColorTarget(
	REFRESH_Renderer *driverData,
	REFRESH_SampleCount multisampleCount,
	REFRESH_TextureSlice *textureSlice
) {
	VkResult vulkanResult;
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanColorTarget *colorTarget = (VulkanColorTarget*) SDL_malloc(sizeof(VulkanColorTarget));
	VkImageViewCreateInfo imageViewCreateInfo;
	VkComponentMapping swizzle = IDENTITY_SWIZZLE;

	colorTarget->texture = (VulkanTexture*) textureSlice->texture;
	colorTarget->layer = textureSlice->layer;
	colorTarget->multisampleTexture = NULL;
	colorTarget->multisampleCount = 1;

	/* create resolve target for multisample */
	if (multisampleCount > REFRESH_SAMPLECOUNT_1)
	{
		colorTarget->multisampleTexture =
			(VulkanTexture*) SDL_malloc(sizeof(VulkanTexture));

		VULKAN_INTERNAL_CreateTexture(
			renderer,
			colorTarget->texture->dimensions.width,
			colorTarget->texture->dimensions.height,
			1,
			0,
			RefreshToVK_SampleCount[multisampleCount],
			1,
			colorTarget->texture->format,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_TYPE_2D,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			REFRESH_TEXTUREUSAGE_COLOR_TARGET_BIT,
			colorTarget->multisampleTexture
		);
		colorTarget->multisampleTexture->colorFormat = colorTarget->texture->colorFormat;
		colorTarget->multisampleCount = multisampleCount;

		VULKAN_INTERNAL_ImageMemoryBarrier(
			renderer,
			RESOURCE_ACCESS_COLOR_ATTACHMENT_READ_WRITE,
			VK_IMAGE_ASPECT_COLOR_BIT,
			0,
			colorTarget->multisampleTexture->layerCount,
			0,
			colorTarget->multisampleTexture->levelCount,
			0,
			colorTarget->multisampleTexture->image,
			&colorTarget->multisampleTexture->resourceAccessType
		);
	}

	/* create framebuffer compatible views for RenderTarget */
	imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	imageViewCreateInfo.pNext = NULL;
	imageViewCreateInfo.flags = 0;
	imageViewCreateInfo.image = colorTarget->texture->image;
	imageViewCreateInfo.format = colorTarget->texture->format;
	imageViewCreateInfo.components = swizzle;
	imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
	imageViewCreateInfo.subresourceRange.levelCount = 1;
	imageViewCreateInfo.subresourceRange.baseArrayLayer = textureSlice->layer;
	imageViewCreateInfo.subresourceRange.layerCount = 1;
	imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

	vulkanResult = renderer->vkCreateImageView(
		renderer->logicalDevice,
		&imageViewCreateInfo,
		NULL,
		&colorTarget->view
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult(
			"vkCreateImageView",
			vulkanResult
		);
		REFRESH_LogError("Failed to create color attachment image view");
		return NULL;
	}

	return (REFRESH_ColorTarget*) colorTarget;
}

static REFRESH_DepthStencilTarget* VULKAN_CreateDepthStencilTarget(
	REFRESH_Renderer *driverData,
	uint32_t width,
	uint32_t height,
	REFRESH_DepthFormat format
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanDepthStencilTarget *depthStencilTarget =
		(VulkanDepthStencilTarget*) SDL_malloc(
			sizeof(VulkanDepthStencilTarget)
		);

	VulkanTexture *texture =
		(VulkanTexture*) SDL_malloc(sizeof(VulkanTexture));

	VkImageAspectFlags imageAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;

	VkImageUsageFlags imageUsageFlags =
		VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	if (DepthFormatContainsStencil(RefreshToVK_DepthFormat[format]))
	{
		imageAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	VULKAN_INTERNAL_CreateTexture(
		renderer,
		width,
		height,
		1,
		0,
		VK_SAMPLE_COUNT_1_BIT,
		1,
		RefreshToVK_DepthFormat[format],
		imageAspectFlags,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_TYPE_2D,
		imageUsageFlags,
		0,
		texture
	);
	texture->depthStencilFormat = format;

	depthStencilTarget->texture = texture;
	depthStencilTarget->view = texture->view;

    return (REFRESH_DepthStencilTarget*) depthStencilTarget;
}

static REFRESH_Buffer* VULKAN_CreateVertexBuffer(
	REFRESH_Renderer *driverData,
	uint32_t sizeInBytes
) {
	VulkanBuffer *buffer = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if(!VULKAN_INTERNAL_CreateBuffer(
		(VulkanRenderer*) driverData,
		sizeInBytes,
		RESOURCE_ACCESS_VERTEX_BUFFER,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		SUB_BUFFER_COUNT,
		buffer
	)) {
		REFRESH_LogError("Failed to create vertex buffer!");
		return NULL;
	}

	return (REFRESH_Buffer*) buffer;
}

static REFRESH_Buffer* VULKAN_CreateIndexBuffer(
	REFRESH_Renderer *driverData,
	uint32_t sizeInBytes
) {
	VulkanBuffer *buffer = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if (!VULKAN_INTERNAL_CreateBuffer(
		(VulkanRenderer*) driverData,
		sizeInBytes,
		RESOURCE_ACCESS_INDEX_BUFFER,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		SUB_BUFFER_COUNT,
		buffer
	)) {
		REFRESH_LogError("Failed to create index buffer!");
		return NULL;
	}

	return (REFRESH_Buffer*) buffer;
}

/* Setters */

static void VULKAN_INTERNAL_MaybeExpandStagingBuffer(
	VulkanRenderer *renderer,
	VkDeviceSize size
) {
	if (size <= renderer->textureStagingBuffer->size)
	{
		return;
	}

	VULKAN_INTERNAL_DestroyTextureStagingBuffer(renderer);

	renderer->textureStagingBuffer = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if (!VULKAN_INTERNAL_CreateBuffer(
		renderer,
		size,
		RESOURCE_ACCESS_MEMORY_TRANSFER_READ_WRITE,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		1,
		renderer->textureStagingBuffer
	)) {
		REFRESH_LogError("Failed to expand texture staging buffer!");
		return;
	}
}

static void VULKAN_SetTextureData2D(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *texture,
	uint32_t x,
	uint32_t y,
	uint32_t w,
	uint32_t h,
	uint32_t level,
	void *data,
	uint32_t dataLengthInBytes
) {
	VkResult vulkanResult;
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *vulkanTexture = (VulkanTexture*) texture;
	VkBufferImageCopy imageCopy;
	uint8_t *mapPointer;

	VULKAN_INTERNAL_MaybeExpandStagingBuffer(renderer, dataLengthInBytes);

	vulkanResult = renderer->vkMapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory,
		renderer->textureStagingBuffer->subBuffers[0]->offset,
		renderer->textureStagingBuffer->subBuffers[0]->size,
		0,
		(void**) &mapPointer
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to map buffer memory!");
		return;
	}

	SDL_memcpy(mapPointer, data, dataLengthInBytes);

	renderer->vkUnmapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		vulkanTexture->layerCount,
		0,
		vulkanTexture->levelCount,
		0,
		vulkanTexture->image,
		&vulkanTexture->resourceAccessType
	);

	imageCopy.imageExtent.width = w;
	imageCopy.imageExtent.height = h;
	imageCopy.imageExtent.depth = 1;
	imageCopy.imageOffset.x = x;
	imageCopy.imageOffset.y = y;
	imageCopy.imageOffset.z = 0;
	imageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageCopy.imageSubresource.baseArrayLayer = 0;
	imageCopy.imageSubresource.layerCount = 1;
	imageCopy.imageSubresource.mipLevel = level;
	imageCopy.bufferOffset = 0;
	imageCopy.bufferRowLength = 0;
	imageCopy.bufferImageHeight = 0;

	RECORD_CMD(renderer->vkCmdCopyBufferToImage(
		renderer->currentCommandBuffer,
		renderer->textureStagingBuffer->subBuffers[0]->buffer,
		vulkanTexture->image,
		AccessMap[vulkanTexture->resourceAccessType].imageLayout,
		1,
		&imageCopy
	));

	if (vulkanTexture->usageFlags & REFRESH_TEXTUREUSAGE_SAMPLER_BIT)
	{
		VULKAN_INTERNAL_ImageMemoryBarrier(
			renderer,
			RESOURCE_ACCESS_ANY_SHADER_READ_SAMPLED_IMAGE,
			VK_IMAGE_ASPECT_COLOR_BIT,
			0,
			vulkanTexture->layerCount,
			0,
			vulkanTexture->levelCount,
			0,
			vulkanTexture->image,
			&vulkanTexture->resourceAccessType
		);
	}

	/* Sync point */
	VULKAN_Submit(driverData);
}

static void VULKAN_SetTextureData3D(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *texture,
	uint32_t x,
	uint32_t y,
	uint32_t z,
	uint32_t w,
	uint32_t h,
	uint32_t d,
	uint32_t level,
	void* data,
	uint32_t dataLength
) {
	VkResult vulkanResult;
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *vulkanTexture = (VulkanTexture*) texture;
	VkBufferImageCopy imageCopy;
	uint8_t *mapPointer;

	VULKAN_INTERNAL_MaybeExpandStagingBuffer(renderer, dataLength);

	vulkanResult = renderer->vkMapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory,
		renderer->textureStagingBuffer->subBuffers[0]->offset,
		renderer->textureStagingBuffer->subBuffers[0]->size,
		0,
		(void**) &mapPointer
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to map buffer memory!");
		return;
	}

	SDL_memcpy(mapPointer, data, dataLength);

	renderer->vkUnmapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		vulkanTexture->layerCount,
		0,
		vulkanTexture->levelCount,
		0,
		vulkanTexture->image,
		&vulkanTexture->resourceAccessType
	);

	imageCopy.imageExtent.width = w;
	imageCopy.imageExtent.height = h;
	imageCopy.imageExtent.depth = d;
	imageCopy.imageOffset.x = x;
	imageCopy.imageOffset.y = y;
	imageCopy.imageOffset.z = z;
	imageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageCopy.imageSubresource.baseArrayLayer = 0;
	imageCopy.imageSubresource.layerCount = 1;
	imageCopy.imageSubresource.mipLevel = level;
	imageCopy.bufferOffset = 0;
	imageCopy.bufferRowLength = 0;
	imageCopy.bufferImageHeight = 0;

	RECORD_CMD(renderer->vkCmdCopyBufferToImage(
		renderer->currentCommandBuffer,
		renderer->textureStagingBuffer->subBuffers[0]->buffer,
		vulkanTexture->image,
		AccessMap[vulkanTexture->resourceAccessType].imageLayout,
		1,
		&imageCopy
	));

	if (vulkanTexture->usageFlags & REFRESH_TEXTUREUSAGE_SAMPLER_BIT)
	{
		VULKAN_INTERNAL_ImageMemoryBarrier(
			renderer,
			RESOURCE_ACCESS_ANY_SHADER_READ_SAMPLED_IMAGE,
			VK_IMAGE_ASPECT_COLOR_BIT,
			0,
			vulkanTexture->layerCount,
			0,
			vulkanTexture->levelCount,
			0,
			vulkanTexture->image,
			&vulkanTexture->resourceAccessType
		);
	}

	/* Sync point */
	VULKAN_Submit(driverData);
}

static void VULKAN_SetTextureDataCube(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *texture,
	uint32_t x,
	uint32_t y,
	uint32_t w,
	uint32_t h,
	REFRESH_CubeMapFace cubeMapFace,
	uint32_t level,
	void* data,
	uint32_t dataLength
) {
	VkResult vulkanResult;
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *vulkanTexture = (VulkanTexture*) texture;
	VkBufferImageCopy imageCopy;
	uint8_t *mapPointer;

	VULKAN_INTERNAL_MaybeExpandStagingBuffer(renderer, dataLength);

	vulkanResult = renderer->vkMapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory,
		renderer->textureStagingBuffer->subBuffers[0]->offset,
		renderer->textureStagingBuffer->subBuffers[0]->size,
		0,
		(void**) &mapPointer
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to map buffer memory!");
		return;
	}

	SDL_memcpy(mapPointer, data, dataLength);

	renderer->vkUnmapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		vulkanTexture->layerCount,
		0,
		vulkanTexture->levelCount,
		0,
		vulkanTexture->image,
		&vulkanTexture->resourceAccessType
	);

	imageCopy.imageExtent.width = w;
	imageCopy.imageExtent.height = h;
	imageCopy.imageExtent.depth = 1;
	imageCopy.imageOffset.x = x;
	imageCopy.imageOffset.y = y;
	imageCopy.imageOffset.z = 0;
	imageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageCopy.imageSubresource.baseArrayLayer = cubeMapFace;
	imageCopy.imageSubresource.layerCount = 1;
	imageCopy.imageSubresource.mipLevel = level;
	imageCopy.bufferOffset = 0;
	imageCopy.bufferRowLength = 0; /* assumes tightly packed data */
	imageCopy.bufferImageHeight = 0; /* assumes tightly packed data */

	RECORD_CMD(renderer->vkCmdCopyBufferToImage(
		renderer->currentCommandBuffer,
		renderer->textureStagingBuffer->subBuffers[0]->buffer,
		vulkanTexture->image,
		AccessMap[vulkanTexture->resourceAccessType].imageLayout,
		1,
		&imageCopy
	));

	if (vulkanTexture->usageFlags & REFRESH_TEXTUREUSAGE_SAMPLER_BIT)
	{
		VULKAN_INTERNAL_ImageMemoryBarrier(
			renderer,
			RESOURCE_ACCESS_ANY_SHADER_READ_SAMPLED_IMAGE,
			VK_IMAGE_ASPECT_COLOR_BIT,
			0,
			vulkanTexture->layerCount,
			0,
			vulkanTexture->levelCount,
			0,
			vulkanTexture->image,
			&vulkanTexture->resourceAccessType
		);
	}

	/* Sync point */
	VULKAN_Submit(driverData);
}

static void VULKAN_SetTextureDataYUV(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *y,
	REFRESH_Texture *u,
	REFRESH_Texture *v,
	uint32_t yWidth,
	uint32_t yHeight,
	uint32_t uvWidth,
	uint32_t uvHeight,
	void* data,
	uint32_t dataLength
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *tex;
	uint8_t *dataPtr = (uint8_t*) data;
	int32_t yDataLength = BytesPerImage(yWidth, yHeight, REFRESH_SURFACEFORMAT_R8);
	int32_t uvDataLength = BytesPerImage(uvWidth, uvHeight, REFRESH_SURFACEFORMAT_R8);
	VkBufferImageCopy imageCopy;
	uint8_t *mapPointer;
	VkResult vulkanResult;

	VULKAN_INTERNAL_MaybeExpandStagingBuffer(renderer, dataLength);

	/* Initialize values that are the same for Y, U, and V */

	imageCopy.imageExtent.depth = 1;
	imageCopy.imageOffset.x = 0;
	imageCopy.imageOffset.y = 0;
	imageCopy.imageOffset.z = 0;
	imageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageCopy.imageSubresource.baseArrayLayer = 0;
	imageCopy.imageSubresource.layerCount = 1;
	imageCopy.imageSubresource.mipLevel = 0;
	imageCopy.bufferOffset = 0;

	/* Y */

	tex = (VulkanTexture*) y;

	vulkanResult = renderer->vkMapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory,
		renderer->textureStagingBuffer->subBuffers[0]->offset,
		renderer->textureStagingBuffer->subBuffers[0]->size,
		0,
		(void**) &mapPointer
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to map buffer memory!");
		return;
	}

	SDL_memcpy(
		mapPointer,
		dataPtr,
		yDataLength
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		tex->layerCount,
		0,
		tex->levelCount,
		0,
		tex->image,
		&tex->resourceAccessType
	);

	imageCopy.imageExtent.width = yWidth;
	imageCopy.imageExtent.height = yHeight;
	imageCopy.bufferRowLength = yWidth;
	imageCopy.bufferImageHeight = yHeight;

	RECORD_CMD(renderer->vkCmdCopyBufferToImage(
		renderer->currentCommandBuffer,
		renderer->textureStagingBuffer->subBuffers[0]->buffer,
		tex->image,
		AccessMap[tex->resourceAccessType].imageLayout,
		1,
		&imageCopy
	));

	/* These apply to both U and V */

	imageCopy.imageExtent.width = uvWidth;
	imageCopy.imageExtent.height = uvHeight;
	imageCopy.bufferRowLength = uvWidth;
	imageCopy.bufferImageHeight = uvHeight;

	/* U */

	imageCopy.bufferOffset = yDataLength;

	tex = (VulkanTexture*) u;

	SDL_memcpy(
		mapPointer + yDataLength,
		dataPtr + yDataLength,
		uvDataLength
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		tex->layerCount,
		0,
		tex->levelCount,
		0,
		tex->image,
		&tex->resourceAccessType
	);

	RECORD_CMD(renderer->vkCmdCopyBufferToImage(
		renderer->currentCommandBuffer,
		renderer->textureStagingBuffer->subBuffers[0]->buffer,
		tex->image,
		AccessMap[tex->resourceAccessType].imageLayout,
		1,
		&imageCopy
	));

	/* V */

	imageCopy.bufferOffset = yDataLength + uvDataLength;

	tex = (VulkanTexture*) v;

	SDL_memcpy(
		mapPointer + yDataLength + uvDataLength,
		dataPtr + yDataLength + uvDataLength,
		uvDataLength
	);

	renderer->vkUnmapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		tex->layerCount,
		0,
		tex->levelCount,
		0,
		tex->image,
		&tex->resourceAccessType
	);

	RECORD_CMD(renderer->vkCmdCopyBufferToImage(
		renderer->currentCommandBuffer,
		renderer->textureStagingBuffer->subBuffers[0]->buffer,
		tex->image,
		AccessMap[tex->resourceAccessType].imageLayout,
		1,
		&imageCopy
	));

	if (tex->usageFlags & REFRESH_TEXTUREUSAGE_SAMPLER_BIT)
	{
		VULKAN_INTERNAL_ImageMemoryBarrier(
			renderer,
			RESOURCE_ACCESS_ANY_SHADER_READ_SAMPLED_IMAGE,
			VK_IMAGE_ASPECT_COLOR_BIT,
			0,
			tex->layerCount,
			0,
			tex->levelCount,
			0,
			tex->image,
			&tex->resourceAccessType
		);
	}

	/* Sync point */
	VULKAN_Submit(driverData);
}

static void VULKAN_INTERNAL_SetBufferData(
	REFRESH_Renderer* driverData,
	REFRESH_Buffer* buffer,
	uint32_t offsetInBytes,
	void* data,
	uint32_t dataLength
) {
	VulkanRenderer* renderer = (VulkanRenderer*)driverData;
	VulkanBuffer* vulkanBuffer = (VulkanBuffer*)buffer;
	uint8_t* mapPointer;
	VkResult vulkanResult;
	uint32_t i;

	#define CURIDX vulkanBuffer->currentSubBufferIndex
	#define SUBBUF vulkanBuffer->subBuffers[CURIDX]

	/* If buffer has not been bound this frame, set the first unbound index */
	if (!vulkanBuffer->bound)
	{
		for (i = 0; i < vulkanBuffer->subBufferCount; i += 1)
		{
			if (vulkanBuffer->subBuffers[i]->bound == -1)
			{
				break;
			}
		}
		CURIDX = i;
	}
	else
	{
		REFRESH_LogError("Buffer already bound. It is an error to set vertex data after binding but before submitting.");
		return;
	}


	/* Map the memory and perform the copy */
	vulkanResult = renderer->vkMapMemory(
		renderer->logicalDevice,
		SUBBUF->allocation->memory,
		SUBBUF->offset,
		SUBBUF->size,
		0,
		(void**)&mapPointer
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to map buffer memory!");
		return;
	}

	SDL_memcpy(
		mapPointer + offsetInBytes,
		data,
		dataLength
	);

	renderer->vkUnmapMemory(
		renderer->logicalDevice,
		SUBBUF->allocation->memory
	);

	#undef CURIDX
	#undef SUBBUF
}

static void VULKAN_SetVertexBufferData(
	REFRESH_Renderer *driverData,
	REFRESH_Buffer *buffer,
	uint32_t offsetInBytes,
	void* data,
	uint32_t elementCount,
	uint32_t vertexStride
) {
	VULKAN_INTERNAL_SetBufferData(
		driverData,
		buffer,
		offsetInBytes,
		data,
		elementCount * vertexStride
	);
}

static void VULKAN_SetIndexBufferData(
	REFRESH_Renderer *driverData,
	REFRESH_Buffer *buffer,
	uint32_t offsetInBytes,
	void* data,
	uint32_t dataLength
) {
	VULKAN_INTERNAL_SetBufferData(
		driverData,
		buffer,
		offsetInBytes,
		data,
		dataLength
	);
}

static uint32_t VULKAN_PushVertexShaderParams(
	REFRESH_Renderer *driverData,
	void *data,
	uint32_t elementCount
) {
	VulkanRenderer* renderer = (VulkanRenderer*)driverData;

	renderer->vertexUBOOffset += renderer->vertexUBOBlockIncrement;
	renderer->vertexUBOBlockIncrement = renderer->currentGraphicsPipeline->vertexUBOBlockSize;

	if (
		renderer->vertexUBOOffset +
		renderer->currentGraphicsPipeline->vertexUBOBlockSize >=
		UBO_BUFFER_SIZE * (renderer->frameIndex + 1)
	) {
		REFRESH_LogError("Vertex UBO overflow!");
		return 0;
	}

	VULKAN_INTERNAL_SetBufferData(
		driverData,
		(REFRESH_Buffer*) renderer->vertexUBO,
		renderer->vertexUBOOffset,
		data,
		elementCount * renderer->currentGraphicsPipeline->vertexUBOBlockSize
	);

	return renderer->vertexUBOOffset;
}

static uint32_t VULKAN_PushFragmentShaderParams(
	REFRESH_Renderer *driverData,
	void *data,
	uint32_t elementCount
) {
	VulkanRenderer* renderer = (VulkanRenderer*)driverData;

	renderer->fragmentUBOOffset += renderer->fragmentUBOBlockIncrement;
	renderer->fragmentUBOBlockIncrement = renderer->currentGraphicsPipeline->fragmentUBOBlockSize;

	if (
		renderer->fragmentUBOOffset +
		renderer->currentGraphicsPipeline->fragmentUBOBlockSize >=
		UBO_BUFFER_SIZE * (renderer->frameIndex + 1)
	) {
		REFRESH_LogError("Fragment UBO overflow!");
		return 0;
	}

	VULKAN_INTERNAL_SetBufferData(
		driverData,
		(REFRESH_Buffer*) renderer->fragmentUBO,
		renderer->fragmentUBOOffset,
		data,
		elementCount * renderer->currentGraphicsPipeline->fragmentUBOBlockSize
	);

	return renderer->fragmentUBOOffset;
}

static inline uint8_t SamplerDescriptorSetDataEqual(
	SamplerDescriptorSetData *a,
	SamplerDescriptorSetData *b,
	uint8_t samplerCount
) {
	uint32_t i;

	for (i = 0; i < samplerCount; i += 1)
	{
		if (	a->descriptorImageInfo[i].imageLayout != b->descriptorImageInfo[i].imageLayout ||
			a->descriptorImageInfo[i].imageView != b->descriptorImageInfo[i].imageView ||
			a->descriptorImageInfo[i].sampler != b->descriptorImageInfo[i].sampler	)
		{
			return 0;
		}
	}

	return 1;
}

static VkDescriptorSet VULKAN_INTERNAL_FetchSamplerDescriptorSet(
	VulkanRenderer *renderer,
	SamplerDescriptorSetCache *samplerDescriptorSetCache,
	SamplerDescriptorSetData *samplerDescriptorSetData
) {
	uint32_t i;
	uint64_t hashcode;
	SamplerDescriptorSetHashArray *arr;
	VkDescriptorSet newDescriptorSet;
	VkWriteDescriptorSet writeDescriptorSets[MAX_TEXTURE_SAMPLERS];
	SamplerDescriptorSetHashMap *map;

	hashcode = SamplerDescriptorSetHashTable_GetHashCode(
		samplerDescriptorSetData,
		samplerDescriptorSetCache->samplerBindingCount
	);
	arr = &samplerDescriptorSetCache->buckets[hashcode % NUM_DESCRIPTOR_SET_HASH_BUCKETS];

	for (i = 0; i < arr->count; i += 1)
	{
		SamplerDescriptorSetHashMap *e = &samplerDescriptorSetCache->elements[arr->elements[i]];
		if (SamplerDescriptorSetDataEqual(
			samplerDescriptorSetData,
			&e->descriptorSetData,
			samplerDescriptorSetCache->samplerBindingCount
		)) {
			e->inactiveFrameCount = 0;
			return e->descriptorSet;
		}
	}

	/* If no match exists, assign a new descriptor set and prepare it for update */
	/* If no inactive descriptor sets remain, create a new pool and allocate new inactive sets */

	if (samplerDescriptorSetCache->inactiveDescriptorSetCount == 0)
	{
		samplerDescriptorSetCache->samplerDescriptorPoolCount += 1;
		samplerDescriptorSetCache->samplerDescriptorPools = SDL_realloc(
			samplerDescriptorSetCache->samplerDescriptorPools,
			sizeof(VkDescriptorPool) * samplerDescriptorSetCache->samplerDescriptorPoolCount
		);

		VULKAN_INTERNAL_CreateSamplerDescriptorPool(
			renderer,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			samplerDescriptorSetCache->nextPoolSize,
			samplerDescriptorSetCache->nextPoolSize * samplerDescriptorSetCache->samplerBindingCount,
			&samplerDescriptorSetCache->samplerDescriptorPools[samplerDescriptorSetCache->samplerDescriptorPoolCount - 1]
		);

		samplerDescriptorSetCache->inactiveDescriptorSetCapacity += samplerDescriptorSetCache->nextPoolSize;

		samplerDescriptorSetCache->inactiveDescriptorSets = SDL_realloc(
			samplerDescriptorSetCache->inactiveDescriptorSets,
			sizeof(VkDescriptorSet) * samplerDescriptorSetCache->inactiveDescriptorSetCapacity
		);

		VULKAN_INTERNAL_AllocateSamplerDescriptorSets(
			renderer,
			samplerDescriptorSetCache->samplerDescriptorPools[samplerDescriptorSetCache->samplerDescriptorPoolCount - 1],
			samplerDescriptorSetCache->descriptorSetLayout,
			samplerDescriptorSetCache->nextPoolSize,
			samplerDescriptorSetCache->inactiveDescriptorSets
		);

		samplerDescriptorSetCache->inactiveDescriptorSetCount = samplerDescriptorSetCache->nextPoolSize;

		samplerDescriptorSetCache->nextPoolSize *= 2;
	}

	newDescriptorSet = samplerDescriptorSetCache->inactiveDescriptorSets[samplerDescriptorSetCache->inactiveDescriptorSetCount - 1];
	samplerDescriptorSetCache->inactiveDescriptorSetCount -= 1;

	for (i = 0; i < samplerDescriptorSetCache->samplerBindingCount; i += 1)
	{
		writeDescriptorSets[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[i].pNext = NULL;
		writeDescriptorSets[i].descriptorCount = 1;
		writeDescriptorSets[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writeDescriptorSets[i].dstArrayElement = 0;
		writeDescriptorSets[i].dstBinding = i;
		writeDescriptorSets[i].dstSet = newDescriptorSet;
		writeDescriptorSets[i].pBufferInfo = NULL;
		writeDescriptorSets[i].pImageInfo = &samplerDescriptorSetData->descriptorImageInfo[i];
	}

	renderer->vkUpdateDescriptorSets(
		renderer->logicalDevice,
		samplerDescriptorSetCache->samplerBindingCount,
		writeDescriptorSets,
		0,
		NULL
	);

	EXPAND_ELEMENTS_IF_NEEDED(arr, 2, uint32_t)
	arr->elements[arr->count] = samplerDescriptorSetCache->count;
	arr->count += 1;

	if (samplerDescriptorSetCache->count == samplerDescriptorSetCache->capacity)
	{
		samplerDescriptorSetCache->capacity *= 2;

		samplerDescriptorSetCache->elements = SDL_realloc(
			samplerDescriptorSetCache->elements,
			sizeof(SamplerDescriptorSetHashMap) * samplerDescriptorSetCache->capacity
		);
	}

	map = &samplerDescriptorSetCache->elements[samplerDescriptorSetCache->count];
	map->key = hashcode;

	for (i = 0; i < samplerDescriptorSetCache->samplerBindingCount; i += 1)
	{
		map->descriptorSetData.descriptorImageInfo[i].imageLayout =
			samplerDescriptorSetData->descriptorImageInfo[i].imageLayout;
		map->descriptorSetData.descriptorImageInfo[i].imageView =
			samplerDescriptorSetData->descriptorImageInfo[i].imageView;
		map->descriptorSetData.descriptorImageInfo[i].sampler =
			samplerDescriptorSetData->descriptorImageInfo[i].sampler;
	}

	map->descriptorSet = newDescriptorSet;
	map->inactiveFrameCount = 0;
	samplerDescriptorSetCache->count += 1;

	return newDescriptorSet;
}

static void VULKAN_SetVertexSamplers(
	REFRESH_Renderer *driverData,
	REFRESH_Texture **pTextures,
	REFRESH_Sampler **pSamplers
) {
	VulkanTexture *currentTexture;
	uint32_t i, samplerCount;

	VulkanRenderer* renderer = (VulkanRenderer*) driverData;
	VulkanGraphicsPipeline *graphicsPipeline = renderer->currentGraphicsPipeline;
	SamplerDescriptorSetData vertexSamplerDescriptorSetData;

	if (graphicsPipeline->pipelineLayout->vertexSamplerDescriptorSetCache == NULL)
	{
		return;
	}

	samplerCount = graphicsPipeline->pipelineLayout->vertexSamplerDescriptorSetCache->samplerBindingCount;

	for (i = 0; i < samplerCount; i += 1)
	{
		currentTexture = (VulkanTexture*) pTextures[i];
		vertexSamplerDescriptorSetData.descriptorImageInfo[i].imageView = currentTexture->view;
		vertexSamplerDescriptorSetData.descriptorImageInfo[i].sampler = (VkSampler) pSamplers[i];
		vertexSamplerDescriptorSetData.descriptorImageInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	graphicsPipeline->vertexSamplerDescriptorSet = VULKAN_INTERNAL_FetchSamplerDescriptorSet(
		renderer,
		graphicsPipeline->pipelineLayout->vertexSamplerDescriptorSetCache,
		&vertexSamplerDescriptorSetData
	);
}

static void VULKAN_SetFragmentSamplers(
	REFRESH_Renderer *driverData,
	REFRESH_Texture **pTextures,
	REFRESH_Sampler **pSamplers
) {
	VulkanTexture *currentTexture;
	uint32_t i, samplerCount;

	VulkanRenderer* renderer = (VulkanRenderer*) driverData;
	VulkanGraphicsPipeline *graphicsPipeline = renderer->currentGraphicsPipeline;
	SamplerDescriptorSetData fragmentSamplerDescriptorSetData;

	if (graphicsPipeline->pipelineLayout->fragmentSamplerDescriptorSetCache == NULL)
	{
		return;
	}

	samplerCount = graphicsPipeline->pipelineLayout->fragmentSamplerDescriptorSetCache->samplerBindingCount;

	for (i = 0; i < samplerCount; i += 1)
	{
		currentTexture = (VulkanTexture*) pTextures[i];
		fragmentSamplerDescriptorSetData.descriptorImageInfo[i].imageView = currentTexture->view;
		fragmentSamplerDescriptorSetData.descriptorImageInfo[i].sampler = (VkSampler) pSamplers[i];
		fragmentSamplerDescriptorSetData.descriptorImageInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	graphicsPipeline->fragmentSamplerDescriptorSet = VULKAN_INTERNAL_FetchSamplerDescriptorSet(
		renderer,
		graphicsPipeline->pipelineLayout->fragmentSamplerDescriptorSetCache,
		&fragmentSamplerDescriptorSetData
	);
}

static void VULKAN_INTERNAL_GetTextureData(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *texture,
	int32_t x,
	int32_t y,
	int32_t w,
	int32_t h,
	int32_t level,
	int32_t layer,
	void* data
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *vulkanTexture = (VulkanTexture*) texture;
	VulkanResourceAccessType prevResourceAccess;
	VkBufferImageCopy imageCopy;
	uint8_t *dataPtr = (uint8_t*) data;
	uint8_t *mapPointer;
	VkResult vulkanResult;
	uint32_t dataLength = BytesPerImage(w, h, vulkanTexture->colorFormat);

	VULKAN_INTERNAL_MaybeExpandStagingBuffer(renderer, dataLength);

	/* Cache this so we can restore it later */
	prevResourceAccess = vulkanTexture->resourceAccessType;

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_READ,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		vulkanTexture->layerCount,
		0,
		vulkanTexture->levelCount,
		0,
		vulkanTexture->image,
		&vulkanTexture->resourceAccessType
	);

	/* Save texture data to staging buffer */

	imageCopy.imageExtent.width = w;
	imageCopy.imageExtent.height = h;
	imageCopy.imageExtent.depth = 1;
	imageCopy.bufferRowLength = w;
	imageCopy.bufferImageHeight = h;
	imageCopy.imageOffset.x = x;
	imageCopy.imageOffset.y = y;
	imageCopy.imageOffset.z = 0;
	imageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageCopy.imageSubresource.baseArrayLayer = layer;
	imageCopy.imageSubresource.layerCount = 1;
	imageCopy.imageSubresource.mipLevel = level;
	imageCopy.bufferOffset = 0;

	RECORD_CMD(renderer->vkCmdCopyImageToBuffer(
		renderer->currentCommandBuffer,
		vulkanTexture->image,
		AccessMap[vulkanTexture->resourceAccessType].imageLayout,
		renderer->textureStagingBuffer->subBuffers[0]->buffer,
		1,
		&imageCopy
	));

	/* Restore the image layout and wait for completion of the render pass */

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		prevResourceAccess,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		vulkanTexture->layerCount,
		0,
		vulkanTexture->levelCount,
		0,
		vulkanTexture->image,
		&vulkanTexture->resourceAccessType
	);

	/* hard sync point */
	VULKAN_Submit(driverData);

	renderer->vkWaitForFences(
		renderer->logicalDevice,
		1,
		&renderer->inFlightFence,
		VK_TRUE,
		UINT64_MAX
	);

	/* Read from staging buffer */

	vulkanResult = renderer->vkMapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory,
		renderer->textureStagingBuffer->subBuffers[0]->offset,
		renderer->textureStagingBuffer->subBuffers[0]->size,
		0,
		(void**) &mapPointer
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to map buffer memory!");
		return;
	}

	SDL_memcpy(
		dataPtr,
		mapPointer,
		dataLength
	);

	renderer->vkUnmapMemory(
		renderer->logicalDevice,
		renderer->textureStagingBuffer->subBuffers[0]->allocation->memory
	);
}

static void VULKAN_GetTextureData2D(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *texture,
	uint32_t x,
	uint32_t y,
	uint32_t w,
	uint32_t h,
	uint32_t level,
	void* data
) {
    VULKAN_INTERNAL_GetTextureData(
		driverData,
		texture,
		x,
		y,
		w,
		h,
		level,
		0,
		data
	);
}

static void VULKAN_GetTextureDataCube(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *texture,
	uint32_t x,
	uint32_t y,
	uint32_t w,
	uint32_t h,
	REFRESH_CubeMapFace cubeMapFace,
	uint32_t level,
	void* data
) {
    VULKAN_INTERNAL_GetTextureData(
		driverData,
		texture,
		x,
		y,
		w,
		h,
		level,
		cubeMapFace,
		data
	);
}

static void VULKAN_AddDisposeTexture(
	REFRESH_Renderer *driverData,
	REFRESH_Texture *texture
) {
	VulkanRenderer* renderer = (VulkanRenderer*)driverData;
	VulkanTexture* vulkanTexture = (VulkanTexture*)texture;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->texturesToDestroy,
		VulkanTexture*,
		renderer->texturesToDestroyCount + 1,
		renderer->texturesToDestroyCapacity,
		renderer->texturesToDestroyCapacity * 2
	)

	renderer->texturesToDestroy[renderer->texturesToDestroyCount] = vulkanTexture;
	renderer->texturesToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_AddDisposeSampler(
	REFRESH_Renderer *driverData,
	REFRESH_Sampler *sampler
) {
	VulkanRenderer* renderer = (VulkanRenderer*)driverData;
	VkSampler vulkanSampler = (VkSampler) sampler;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->samplersToDestroy,
		VkSampler,
		renderer->samplersToDestroyCount + 1,
		renderer->samplersToDestroyCapacity,
		renderer->samplersToDestroyCapacity * 2
	)

	renderer->samplersToDestroy[renderer->samplersToDestroyCount] = vulkanSampler;
	renderer->samplersToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_AddDisposeVertexBuffer(
	REFRESH_Renderer *driverData,
	REFRESH_Buffer *buffer
) {
	VULKAN_INTERNAL_RemoveBuffer(driverData, buffer);
}

static void VULKAN_AddDisposeIndexBuffer(
	REFRESH_Renderer *driverData,
	REFRESH_Buffer *buffer
) {
	VULKAN_INTERNAL_RemoveBuffer(driverData, buffer);
}

static void VULKAN_AddDisposeColorTarget(
	REFRESH_Renderer *driverData,
	REFRESH_ColorTarget *colorTarget
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanColorTarget *vulkanColorTarget = (VulkanColorTarget*) colorTarget;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->colorTargetsToDestroy,
		VulkanColorTarget*,
		renderer->colorTargetsToDestroyCount + 1,
		renderer->colorTargetsToDestroyCapacity,
		renderer->colorTargetsToDestroyCapacity * 2
	)

	renderer->colorTargetsToDestroy[renderer->colorTargetsToDestroyCount] = vulkanColorTarget;
	renderer->colorTargetsToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_AddDisposeDepthStencilTarget(
	REFRESH_Renderer *driverData,
	REFRESH_DepthStencilTarget *depthStencilTarget
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanDepthStencilTarget *vulkanDepthStencilTarget = (VulkanDepthStencilTarget*) depthStencilTarget;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->depthStencilTargetsToDestroy,
		VulkanDepthStencilTarget*,
		renderer->depthStencilTargetsToDestroyCount + 1,
		renderer->depthStencilTargetsToDestroyCapacity,
		renderer->depthStencilTargetsToDestroyCapacity * 2
	)

	renderer->depthStencilTargetsToDestroy[renderer->depthStencilTargetsToDestroyCount] = vulkanDepthStencilTarget;
	renderer->depthStencilTargetsToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_AddDisposeFramebuffer(
	REFRESH_Renderer *driverData,
	REFRESH_Framebuffer *framebuffer
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanFramebuffer *vulkanFramebuffer = (VulkanFramebuffer*) framebuffer;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->framebuffersToDestroy,
		VulkanFramebuffer*,
		renderer->framebuffersToDestroyCount + 1,
		renderer->framebuffersToDestroyCapacity,
		renderer->framebuffersToDestroyCapacity *= 2
	)

	renderer->framebuffersToDestroy[renderer->framebuffersToDestroyCount] = vulkanFramebuffer;
	renderer->framebuffersToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_AddDisposeShaderModule(
	REFRESH_Renderer *driverData,
	REFRESH_ShaderModule *shaderModule
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VkShaderModule vulkanShaderModule = (VkShaderModule) shaderModule;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->shaderModulesToDestroy,
		VkShaderModule,
		renderer->shaderModulesToDestroyCount + 1,
		renderer->shaderModulesToDestroyCapacity,
		renderer->shaderModulesToDestroyCapacity * 2
	)

	renderer->shaderModulesToDestroy[renderer->shaderModulesToDestroyCount] = vulkanShaderModule;
	renderer->shaderModulesToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_AddDisposeRenderPass(
	REFRESH_Renderer *driverData,
	REFRESH_RenderPass *renderPass
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VkRenderPass vulkanRenderPass = (VkRenderPass) renderPass;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->renderPassesToDestroy,
		VkRenderPass,
		renderer->renderPassesToDestroyCount + 1,
		renderer->renderPassesToDestroyCapacity,
		renderer->renderPassesToDestroyCapacity * 2
	)

	renderer->renderPassesToDestroy[renderer->renderPassesToDestroyCount] = vulkanRenderPass;
	renderer->renderPassesToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_AddDisposeGraphicsPipeline(
	REFRESH_Renderer *driverData,
	REFRESH_GraphicsPipeline *graphicsPipeline
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanGraphicsPipeline *vulkanGraphicsPipeline = (VulkanGraphicsPipeline*) graphicsPipeline;

	SDL_LockMutex(renderer->disposeLock);

	EXPAND_ARRAY_IF_NEEDED(
		renderer->graphicsPipelinesToDestroy,
		VulkanGraphicsPipeline*,
		renderer->graphicsPipelinesToDestroyCount + 1,
		renderer->graphicsPipelinesToDestroyCapacity,
		renderer->graphicsPipelinesToDestroyCapacity * 2
	)

	renderer->graphicsPipelinesToDestroy[renderer->graphicsPipelinesToDestroyCount] = vulkanGraphicsPipeline;
	renderer->graphicsPipelinesToDestroyCount += 1;

	SDL_UnlockMutex(renderer->disposeLock);
}

static void VULKAN_BeginRenderPass(
	REFRESH_Renderer *driverData,
	REFRESH_RenderPass *renderPass,
	REFRESH_Framebuffer *framebuffer,
	REFRESH_Rect renderArea,
	REFRESH_Color *pColorClearValues,
	uint32_t colorClearCount,
	REFRESH_DepthStencilValue *depthStencilClearValue
) {
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanFramebuffer *vulkanFramebuffer = (VulkanFramebuffer*) framebuffer;
	VkClearValue *clearValues;
	uint32_t i;
	uint32_t clearCount = colorClearCount;
	VkImageAspectFlags depthAspectFlags;

	/* Layout transitions */

	for (i = 0; i < vulkanFramebuffer->colorTargetCount; i += 1)
	{
		VULKAN_INTERNAL_ImageMemoryBarrier(
			renderer,
			RESOURCE_ACCESS_COLOR_ATTACHMENT_READ_WRITE,
			VK_IMAGE_ASPECT_COLOR_BIT,
			vulkanFramebuffer->colorTargets[i]->layer,
			1,
			0,
			1,
			0,
			vulkanFramebuffer->colorTargets[i]->texture->image,
			&vulkanFramebuffer->colorTargets[i]->texture->resourceAccessType
		);
	}

	if (depthStencilClearValue != NULL)
	{
		depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (DepthFormatContainsStencil(
			vulkanFramebuffer->depthStencilTarget->texture->format
		)) {
			depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}

		VULKAN_INTERNAL_ImageMemoryBarrier(
			renderer,
			RESOURCE_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_WRITE,
			depthAspectFlags,
			0,
			1,
			0,
			1,
			0,
			vulkanFramebuffer->depthStencilTarget->texture->image,
			&vulkanFramebuffer->depthStencilTarget->texture->resourceAccessType
		);

		clearCount += 1;
	}

	/* Set clear values */

	clearValues = SDL_stack_alloc(VkClearValue, clearCount);

	for (i = 0; i < colorClearCount; i += 1)
	{
		clearValues[i].color.float32[0] = pColorClearValues[i].r / 255.0f;
		clearValues[i].color.float32[1] = pColorClearValues[i].g / 255.0f;
		clearValues[i].color.float32[2] = pColorClearValues[i].b / 255.0f;
		clearValues[i].color.float32[3] = pColorClearValues[i].a / 255.0f;
	}

	if (depthStencilClearValue != NULL)
	{
		clearValues[colorClearCount].depthStencil.depth =
			depthStencilClearValue->depth;
		clearValues[colorClearCount].depthStencil.stencil =
			depthStencilClearValue->stencil;
	}

	VkRenderPassBeginInfo renderPassBeginInfo;
	renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBeginInfo.pNext = NULL;
	renderPassBeginInfo.renderPass = (VkRenderPass) renderPass;
	renderPassBeginInfo.framebuffer = vulkanFramebuffer->framebuffer;
	renderPassBeginInfo.renderArea.extent.width = renderArea.w;
	renderPassBeginInfo.renderArea.extent.height = renderArea.h;
	renderPassBeginInfo.renderArea.offset.x = renderArea.x;
	renderPassBeginInfo.renderArea.offset.y = renderArea.y;
	renderPassBeginInfo.pClearValues = clearValues;
	renderPassBeginInfo.clearValueCount = clearCount;

	RECORD_CMD(renderer->vkCmdBeginRenderPass(
		renderer->currentCommandBuffer,
		&renderPassBeginInfo,
		VK_SUBPASS_CONTENTS_INLINE
	));

	renderer->currentFramebuffer = vulkanFramebuffer;

	SDL_stack_free(clearValues);
}

static void VULKAN_EndRenderPass(
	REFRESH_Renderer *driverData
) {
	uint32_t i;
	VulkanRenderer *renderer = (VulkanRenderer*) driverData;
	VulkanTexture *currentTexture;

	RECORD_CMD(renderer->vkCmdEndRenderPass(
		renderer->currentCommandBuffer
	));

	for (i = 0; i < renderer->currentFramebuffer->colorTargetCount; i += 1)
	{
		currentTexture = renderer->currentFramebuffer->colorTargets[i]->texture;
		if (currentTexture->usageFlags & REFRESH_TEXTUREUSAGE_SAMPLER_BIT)
		{
			VULKAN_INTERNAL_ImageMemoryBarrier(
				renderer,
				RESOURCE_ACCESS_ANY_SHADER_READ_SAMPLED_IMAGE,
				VK_IMAGE_ASPECT_COLOR_BIT,
				0,
				currentTexture->layerCount,
				0,
				currentTexture->levelCount,
				0,
				currentTexture->image,
				&currentTexture->resourceAccessType
			);
		}
	}

	renderer->currentGraphicsPipeline = NULL;
	renderer->currentFramebuffer = NULL;
}

static void VULKAN_BindGraphicsPipeline(
	REFRESH_Renderer *driverData,
	REFRESH_GraphicsPipeline *graphicsPipeline
) {
	VulkanRenderer* renderer = (VulkanRenderer*) driverData;
	VulkanGraphicsPipeline* pipeline = (VulkanGraphicsPipeline*) graphicsPipeline;

	/* bind dummy samplers */
	if (pipeline->pipelineLayout->vertexSamplerDescriptorSetCache == NULL)
	{
		pipeline->vertexSamplerDescriptorSet = renderer->emptyVertexSamplerDescriptorSet;
	}

	if (pipeline->pipelineLayout->fragmentSamplerDescriptorSetCache == NULL)
	{
		pipeline->fragmentSamplerDescriptorSet = renderer->emptyFragmentSamplerDescriptorSet;
	}

	RECORD_CMD(renderer->vkCmdBindPipeline(
		renderer->currentCommandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline->pipeline
	));

	renderer->currentGraphicsPipeline = pipeline;
}

static void VULKAN_INTERNAL_MarkAsBound(
	VulkanRenderer* renderer,
	VulkanBuffer* buf
) {
	VulkanSubBuffer *subbuf = buf->subBuffers[buf->currentSubBufferIndex];
	subbuf->bound = renderer->frameIndex;

	/* Don't rebind a bound buffer */
	if (buf->bound) return;

	buf->bound = 1;

	if (renderer->buffersInUseCount == renderer->buffersInUseCapacity)
	{
		renderer->buffersInUseCapacity *= 2;
		renderer->buffersInUse = SDL_realloc(
			renderer->buffersInUse,
			sizeof(VulkanBuffer*) * renderer->buffersInUseCapacity
		);
	}

	renderer->buffersInUse[renderer->buffersInUseCount] = buf;
	renderer->buffersInUseCount += 1;
}

static void VULKAN_BindVertexBuffers(
	REFRESH_Renderer *driverData,
	uint32_t firstBinding,
	uint32_t bindingCount,
	REFRESH_Buffer **pBuffers,
	uint64_t *pOffsets
) {
	VkBuffer *buffers = SDL_stack_alloc(VkBuffer, bindingCount);
	VulkanBuffer* currentBuffer;
	VulkanRenderer* renderer = (VulkanRenderer*) driverData;
	uint32_t i;

	for (i = 0; i < bindingCount; i += 1)
	{
		currentBuffer = (VulkanBuffer*) pBuffers[i];
		buffers[i] = currentBuffer->subBuffers[currentBuffer->currentSubBufferIndex]->buffer;
		VULKAN_INTERNAL_MarkAsBound(renderer, currentBuffer);
	}

	RECORD_CMD(renderer->vkCmdBindVertexBuffers(
		renderer->currentCommandBuffer,
		firstBinding,
		bindingCount,
		buffers,
		pOffsets
	));

	SDL_stack_free(buffers);
}

static void VULKAN_BindIndexBuffer(
	REFRESH_Renderer *driverData,
	REFRESH_Buffer *buffer,
	uint64_t offset,
	REFRESH_IndexElementSize indexElementSize
) {
	VulkanRenderer* renderer = (VulkanRenderer*) driverData;
	VulkanBuffer* vulkanBuffer = (VulkanBuffer*) buffer;

	VULKAN_INTERNAL_MarkAsBound(renderer, vulkanBuffer);

	RECORD_CMD(renderer->vkCmdBindIndexBuffer(
		renderer->currentCommandBuffer,
		vulkanBuffer->subBuffers[renderer->frameIndex]->buffer,
		offset,
		RefreshToVK_IndexType[indexElementSize]
	));
}

static void VULKAN_QueuePresent(
	REFRESH_Renderer* driverData,
	REFRESH_TextureSlice* textureSlice,
	REFRESH_Rect* sourceRectangle,
	REFRESH_Rect* destinationRectangle
) {
	VkResult acquireResult;
	uint32_t swapChainImageIndex;

	REFRESH_Rect srcRect;
	REFRESH_Rect dstRect;
	VkImageBlit blit;

	VulkanRenderer* renderer = (VulkanRenderer*) driverData;
	VulkanTexture* vulkanTexture = (VulkanTexture*) textureSlice->texture;

	if (renderer->headless)
	{
		REFRESH_LogError("Cannot call QueuePresent in headless mode!");
		return;
	}

	acquireResult = renderer->vkAcquireNextImageKHR(
		renderer->logicalDevice,
		renderer->swapChain,
		UINT64_MAX,
		renderer->imageAvailableSemaphore,
		VK_NULL_HANDLE,
		&swapChainImageIndex
	);

	if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
	{
		/* Failed to acquire swapchain image, mark that we need a new one */
		renderer->needNewSwapChain = 1;
		return;
	}

	renderer->shouldPresent = 1;
	renderer->swapChainImageAcquired = 1;
	renderer->currentSwapChainIndex = swapChainImageIndex;

	if (sourceRectangle != NULL)
	{
		srcRect = *sourceRectangle;
	}
	else
	{
		srcRect.x = 0;
		srcRect.y = 0;
		srcRect.w = vulkanTexture->dimensions.width;
		srcRect.h = vulkanTexture->dimensions.height;
	}

	if (destinationRectangle != NULL)
	{
		dstRect = *destinationRectangle;
	}
	else
	{
		dstRect.x = 0;
		dstRect.y = 0;
		dstRect.w = renderer->swapChainExtent.width;
		dstRect.h = renderer->swapChainExtent.height;
	}

	/* Blit the framebuffer! */

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_READ,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		1,
		0,
		1,
		0,
		vulkanTexture->image,
		&vulkanTexture->resourceAccessType
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_TRANSFER_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		1,
		0,
		1,
		0,
		renderer->swapChainImages[swapChainImageIndex],
		&renderer->swapChainResourceAccessTypes[swapChainImageIndex]
	);

	blit.srcOffsets[0].x = srcRect.x;
	blit.srcOffsets[0].y = srcRect.y;
	blit.srcOffsets[0].z = 0;
	blit.srcOffsets[1].x = srcRect.x + srcRect.w;
	blit.srcOffsets[1].y = srcRect.y + srcRect.h;
	blit.srcOffsets[1].z = 1;

	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.baseArrayLayer = 0;
	blit.srcSubresource.layerCount = 1;
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	blit.dstOffsets[0].x = dstRect.x;
	blit.dstOffsets[0].y = dstRect.y;
	blit.dstOffsets[0].z = 0;
	blit.dstOffsets[1].x = dstRect.x + dstRect.w;
	blit.dstOffsets[1].y = dstRect.y + dstRect.h;
	blit.dstOffsets[1].z = 1;

	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.baseArrayLayer = textureSlice->layer;
	blit.dstSubresource.layerCount = 1;
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	RECORD_CMD(renderer->vkCmdBlitImage(
		renderer->currentCommandBuffer,
		vulkanTexture->image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		renderer->swapChainImages[swapChainImageIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&blit,
		VK_FILTER_LINEAR
	));

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_PRESENT,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		1,
		0,
		1,
		0,
		renderer->swapChainImages[swapChainImageIndex],
		&renderer->swapChainResourceAccessTypes[swapChainImageIndex]
	);

	VULKAN_INTERNAL_ImageMemoryBarrier(
		renderer,
		RESOURCE_ACCESS_COLOR_ATTACHMENT_READ_WRITE,
		VK_IMAGE_ASPECT_COLOR_BIT,
		0,
		1,
		0,
		1,
		0,
		vulkanTexture->image,
		&vulkanTexture->resourceAccessType
	);
}

static void VULKAN_INTERNAL_DeactivateUnusedDescriptorSets(
	SamplerDescriptorSetCache *samplerDescriptorSetCache
) {
	int32_t i, j;
	SamplerDescriptorSetHashArray *arr;

	for (i = samplerDescriptorSetCache->count - 1; i >= 0; i -= 1)
	{
		samplerDescriptorSetCache->elements[i].inactiveFrameCount += 1;

		if (samplerDescriptorSetCache->elements[i].inactiveFrameCount + 1 > DESCRIPTOR_SET_DEACTIVATE_FRAMES)
		{
			arr = &samplerDescriptorSetCache->buckets[samplerDescriptorSetCache->elements[i].key % NUM_DESCRIPTOR_SET_HASH_BUCKETS];

			/* remove index from bucket */
			for (j = 0; j < arr->count; j += 1)
			{
				if (arr->elements[j] == i)
				{
					if (j < arr->count - 1)
					{
						arr->elements[j] = arr->elements[arr->count - 1];
					}

					arr->count -= 1;
					break;
				}
			}

			/* remove element from table and place in inactive sets */

			samplerDescriptorSetCache->inactiveDescriptorSets[samplerDescriptorSetCache->inactiveDescriptorSetCount] = samplerDescriptorSetCache->elements[i].descriptorSet;
			samplerDescriptorSetCache->inactiveDescriptorSetCount += 1;

			/* move another descriptor set to fill the hole */
			if (i < samplerDescriptorSetCache->count - 1)
			{
				samplerDescriptorSetCache->elements[i] = samplerDescriptorSetCache->elements[samplerDescriptorSetCache->count - 1];

				/* update index in bucket */
				arr = &samplerDescriptorSetCache->buckets[samplerDescriptorSetCache->elements[i].key % NUM_DESCRIPTOR_SET_HASH_BUCKETS];

				for (j = 0; j < arr->count; j += 1)
				{
					if (arr->elements[j] == samplerDescriptorSetCache->count - 1)
					{
						arr->elements[j] = i;
						break;
					}
				}
			}

			samplerDescriptorSetCache->count -= 1;
		}
	}
}

static void VULKAN_INTERNAL_ResetDescriptorSetData(VulkanRenderer *renderer)
{
	uint32_t i, j;
	VulkanGraphicsPipelineLayout *pipelineLayout;

	for (i = 0; i < NUM_PIPELINE_LAYOUT_BUCKETS; i += 1)
	{
		for (j = 0; j < renderer->pipelineLayoutHashTable.buckets[i].count; j += 1)
		{
			pipelineLayout = renderer->pipelineLayoutHashTable.buckets[i].elements[j].value;

			if (pipelineLayout->vertexSamplerDescriptorSetCache != NULL)
			{
				VULKAN_INTERNAL_DeactivateUnusedDescriptorSets(
					pipelineLayout->vertexSamplerDescriptorSetCache
				);
			}

			if (pipelineLayout->fragmentSamplerDescriptorSetCache != NULL)
			{
				VULKAN_INTERNAL_DeactivateUnusedDescriptorSets(
					pipelineLayout->fragmentSamplerDescriptorSetCache
				);
			}
		}
	}
}

static void VULKAN_Submit(
    REFRESH_Renderer *driverData
) {
	VulkanRenderer* renderer = (VulkanRenderer*)driverData;
	VkSubmitInfo submitInfo;
	VkResult vulkanResult, presentResult = VK_SUCCESS;
	uint32_t i;
	uint8_t present;

	VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkPresentInfoKHR presentInfo;

	present = !renderer->headless && renderer->shouldPresent;

	if (renderer->activeCommandBufferCount <= 1 && renderer->numActiveCommands == 0)
	{
		/* No commands recorded, bailing out */
		return;
	}

	if (renderer->currentCommandBuffer != NULL)
	{
		VULKAN_INTERNAL_EndCommandBuffer(renderer);
	}

	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pNext = NULL;
	submitInfo.commandBufferCount = renderer->activeCommandBufferCount;
	submitInfo.pCommandBuffers = renderer->activeCommandBuffers;

	if (present)
	{
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &renderer->imageAvailableSemaphore;
		submitInfo.pWaitDstStageMask = &waitStages;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &renderer->renderFinishedSemaphore;
	}
	else
	{
		submitInfo.waitSemaphoreCount = 0;
		submitInfo.pWaitSemaphores = NULL;
		submitInfo.pWaitDstStageMask = NULL;
		submitInfo.signalSemaphoreCount = 0;
		submitInfo.pSignalSemaphores = NULL;
	}

	/* Wait for the previous submission to complete */
	vulkanResult = renderer->vkWaitForFences(
		renderer->logicalDevice,
		1,
		&renderer->inFlightFence,
		VK_TRUE,
		UINT64_MAX
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkWaitForFences", vulkanResult);
		return;
	}

	VULKAN_INTERNAL_PostWorkCleanup(renderer);

	/* Reset the previously submitted command buffers */
	for (i = 0; i < renderer->submittedCommandBufferCount; i += 1)
	{
		vulkanResult = renderer->vkResetCommandBuffer(
			renderer->submittedCommandBuffers[i],
			VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT
		);

		if (vulkanResult != VK_SUCCESS)
		{
			LogVulkanResult("vkResetCommandBuffer", vulkanResult);
		}
	}

	/* Mark the previously submitted command buffers as inactive */
	for (i = 0; i < renderer->submittedCommandBufferCount; i += 1)
	{
		renderer->inactiveCommandBuffers[renderer->inactiveCommandBufferCount] = renderer->submittedCommandBuffers[i];
		renderer->inactiveCommandBufferCount += 1;
	}

	renderer->submittedCommandBufferCount = 0;

	/* Prepare the command buffer fence for submission */
	renderer->vkResetFences(
		renderer->logicalDevice,
		1,
		&renderer->inFlightFence
	);

	/* Submit the commands, finally. */
	vulkanResult = renderer->vkQueueSubmit(
		renderer->graphicsQueue,
		1,
		&submitInfo,
		renderer->inFlightFence
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkQueueSubmit", vulkanResult);
		return;
	}

	/* Mark active command buffers as submitted */
	for (i = 0; i < renderer->activeCommandBufferCount; i += 1)
	{
		renderer->submittedCommandBuffers[renderer->submittedCommandBufferCount] = renderer->activeCommandBuffers[i];
		renderer->submittedCommandBufferCount += 1;
	}

	renderer->activeCommandBufferCount = 0;

	/* Reset UBOs */

	renderer->vertexUBOOffset = UBO_BUFFER_SIZE * renderer->frameIndex;
	renderer->vertexUBOBlockIncrement = 0;
	renderer->fragmentUBOOffset = UBO_BUFFER_SIZE * renderer->frameIndex;
	renderer->fragmentUBOBlockIncrement = 0;

	/* Reset descriptor set data */
	VULKAN_INTERNAL_ResetDescriptorSetData(renderer);

	/* Present, if applicable */

	if (present)
	{
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.pNext = NULL;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &renderer->renderFinishedSemaphore;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &renderer->swapChain;
		presentInfo.pImageIndices = &renderer->currentSwapChainIndex;
		presentInfo.pResults = NULL;

		presentResult = renderer->vkQueuePresentKHR(
			renderer->presentQueue,
			&presentInfo
		);

		if (renderer->needNewSwapChain)
		{
			VULKAN_INTERNAL_RecreateSwapchain(renderer);
		}
	}

	renderer->swapChainImageAcquired = 0;
	renderer->shouldPresent = 0;

	VULKAN_INTERNAL_BeginCommandBuffer(renderer);
}

/* Device instantiation */

static inline uint8_t VULKAN_INTERNAL_SupportsExtension(
	const char *ext,
	VkExtensionProperties *availableExtensions,
	uint32_t numAvailableExtensions
) {
	uint32_t i;
	for (i = 0; i < numAvailableExtensions; i += 1)
	{
		if (SDL_strcmp(ext, availableExtensions[i].extensionName) == 0)
		{
			return 1;
		}
	}
	return 0;
}

static uint8_t VULKAN_INTERNAL_CheckInstanceExtensions(
	const char **requiredExtensions,
	uint32_t requiredExtensionsLength,
	uint8_t *supportsDebugUtils
) {
	uint32_t extensionCount, i;
	VkExtensionProperties *availableExtensions;
	uint8_t allExtensionsSupported = 1;

	vkEnumerateInstanceExtensionProperties(
		NULL,
		&extensionCount,
		NULL
	);
	availableExtensions = SDL_stack_alloc(
		VkExtensionProperties,
		extensionCount
	);
	vkEnumerateInstanceExtensionProperties(
		NULL,
		&extensionCount,
		availableExtensions
	);

	for (i = 0; i < requiredExtensionsLength; i += 1)
	{
		if (!VULKAN_INTERNAL_SupportsExtension(
			requiredExtensions[i],
			availableExtensions,
			extensionCount
		)) {
			allExtensionsSupported = 0;
			break;
		}
	}

	/* This is optional, but nice to have! */
	*supportsDebugUtils = VULKAN_INTERNAL_SupportsExtension(
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		availableExtensions,
		extensionCount
	);

	SDL_stack_free(availableExtensions);
	return allExtensionsSupported;
}

static uint8_t VULKAN_INTERNAL_CheckValidationLayers(
	const char** validationLayers,
	uint32_t validationLayersLength
) {
	uint32_t layerCount;
	VkLayerProperties *availableLayers;
	uint32_t i, j;
	uint8_t layerFound = 0;

	vkEnumerateInstanceLayerProperties(&layerCount, NULL);
	availableLayers = SDL_stack_alloc(VkLayerProperties, layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers);

	for (i = 0; i < validationLayersLength; i += 1)
	{
		layerFound = 0;

		for (j = 0; j < layerCount; j += 1)
		{
			if (SDL_strcmp(validationLayers[i], availableLayers[j].layerName) == 0)
			{
				layerFound = 1;
				break;
			}
		}

		if (!layerFound)
		{
			break;
		}
	}

	SDL_stack_free(availableLayers);
	return layerFound;
}

static uint8_t VULKAN_INTERNAL_CreateInstance(
    VulkanRenderer *renderer,
    void *deviceWindowHandle
) {
	VkResult vulkanResult;
	VkApplicationInfo appInfo;
	const char **instanceExtensionNames;
	uint32_t instanceExtensionCount;
	VkInstanceCreateInfo createInfo;
	static const char *layerNames[] = { "VK_LAYER_KHRONOS_validation" };

	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pNext = NULL;
	appInfo.pApplicationName = NULL;
	appInfo.applicationVersion = 0;
	appInfo.pEngineName = "REFRESH";
	appInfo.engineVersion = REFRESH_COMPILED_VERSION;
	appInfo.apiVersion = VK_MAKE_VERSION(1, 0, 0);

    if (!SDL_Vulkan_GetInstanceExtensions(
        (SDL_Window*) deviceWindowHandle,
        &instanceExtensionCount,
        NULL
    )) {
        REFRESH_LogError(
            "SDL_Vulkan_GetInstanceExtensions(): getExtensionCount: %s",
            SDL_GetError()
        );

        return 0;
    }

	/* Extra space for the following extensions:
	 * VK_KHR_get_physical_device_properties2
	 * VK_EXT_debug_utils
	 */
	instanceExtensionNames = SDL_stack_alloc(
		const char*,
		instanceExtensionCount + 2
	);

	if (!SDL_Vulkan_GetInstanceExtensions(
		(SDL_Window*) deviceWindowHandle,
		&instanceExtensionCount,
		instanceExtensionNames
	)) {
		REFRESH_LogError(
			"SDL_Vulkan_GetInstanceExtensions(): %s",
			SDL_GetError()
		);

        SDL_stack_free((char*) instanceExtensionNames);
        return 0;
	}

	/* Core since 1.1 */
	instanceExtensionNames[instanceExtensionCount++] =
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;

	if (!VULKAN_INTERNAL_CheckInstanceExtensions(
		instanceExtensionNames,
		instanceExtensionCount,
		&renderer->supportsDebugUtils
	)) {
		REFRESH_LogError(
			"Required Vulkan instance extensions not supported"
		);

        SDL_stack_free((char*) instanceExtensionNames);
        return 0;
	}

	if (renderer->supportsDebugUtils)
	{
		/* Append the debug extension to the end */
		instanceExtensionNames[instanceExtensionCount++] =
			VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	}
	else
	{
		REFRESH_LogWarn(
			"%s is not supported!",
			VK_EXT_DEBUG_UTILS_EXTENSION_NAME
		);
	}

    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pNext = NULL;
	createInfo.flags = 0;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.ppEnabledLayerNames = layerNames;
	createInfo.enabledExtensionCount = instanceExtensionCount;
	createInfo.ppEnabledExtensionNames = instanceExtensionNames;
	if (renderer->debugMode)
	{
		createInfo.enabledLayerCount = SDL_arraysize(layerNames);
		if (!VULKAN_INTERNAL_CheckValidationLayers(
			layerNames,
			createInfo.enabledLayerCount
		)) {
			REFRESH_LogWarn("Validation layers not found, continuing without validation");
			createInfo.enabledLayerCount = 0;
		}
	}
	else
	{
		createInfo.enabledLayerCount = 0;
	}

    vulkanResult = vkCreateInstance(&createInfo, NULL, &renderer->instance);
	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError(
			"vkCreateInstance failed: %s",
			VkErrorMessages(vulkanResult)
		);

        SDL_stack_free((char*) instanceExtensionNames);
        return 0;
	}

	SDL_stack_free((char*) instanceExtensionNames);
	return 1;
}

static uint8_t VULKAN_INTERNAL_CheckDeviceExtensions(
	VulkanRenderer *renderer,
	VkPhysicalDevice physicalDevice,
	const char** requiredExtensions,
	uint32_t requiredExtensionsLength
) {
	uint32_t extensionCount, i;
	VkExtensionProperties *availableExtensions;
	uint8_t allExtensionsSupported = 1;

	renderer->vkEnumerateDeviceExtensionProperties(
		physicalDevice,
		NULL,
		&extensionCount,
		NULL
	);
	availableExtensions = SDL_stack_alloc(
		VkExtensionProperties,
		extensionCount
	);
	renderer->vkEnumerateDeviceExtensionProperties(
		physicalDevice,
		NULL,
		&extensionCount,
		availableExtensions
	);

	for (i = 0; i < requiredExtensionsLength; i += 1)
	{
		if (!VULKAN_INTERNAL_SupportsExtension(
			requiredExtensions[i],
			availableExtensions,
			extensionCount
		)) {
			allExtensionsSupported = 0;
			break;
		}
	}

	SDL_stack_free(availableExtensions);
	return allExtensionsSupported;
}

static uint8_t VULKAN_INTERNAL_IsDeviceSuitable(
	VulkanRenderer *renderer,
	VkPhysicalDevice physicalDevice,
	const char** requiredExtensionNames,
	uint32_t requiredExtensionNamesLength,
	VkSurfaceKHR surface,
	QueueFamilyIndices *queueFamilyIndices,
	uint8_t *isIdeal
) {
	uint32_t queueFamilyCount, i;
	SwapChainSupportDetails swapChainSupportDetails;
	VkQueueFamilyProperties *queueProps;
	VkBool32 supportsPresent;
	uint8_t querySuccess, foundSuitableDevice = 0;
	VkPhysicalDeviceProperties deviceProperties;

	queueFamilyIndices->graphicsFamily = UINT32_MAX;
	queueFamilyIndices->presentFamily = UINT32_MAX;
	*isIdeal = 0;

	/* Note: If no dedicated device exists,
	 * one that supports our features would be fine
	 */

	if (!VULKAN_INTERNAL_CheckDeviceExtensions(
		renderer,
		physicalDevice,
		requiredExtensionNames,
		requiredExtensionNamesLength
	)) {
		return 0;
	}

	renderer->vkGetPhysicalDeviceQueueFamilyProperties(
		physicalDevice,
		&queueFamilyCount,
		NULL
	);

	/* FIXME: Need better structure for checking vs storing support details */
	querySuccess = VULKAN_INTERNAL_QuerySwapChainSupport(
		renderer,
		physicalDevice,
		surface,
		&swapChainSupportDetails
	);
	SDL_free(swapChainSupportDetails.formats);
	SDL_free(swapChainSupportDetails.presentModes);
	if (	querySuccess == 0 ||
		swapChainSupportDetails.formatsLength == 0 ||
		swapChainSupportDetails.presentModesLength == 0	)
	{
		return 0;
	}

	queueProps = (VkQueueFamilyProperties*) SDL_stack_alloc(
		VkQueueFamilyProperties,
		queueFamilyCount
	);
	renderer->vkGetPhysicalDeviceQueueFamilyProperties(
		physicalDevice,
		&queueFamilyCount,
		queueProps
	);

	for (i = 0; i < queueFamilyCount; i += 1)
	{
		renderer->vkGetPhysicalDeviceSurfaceSupportKHR(
			physicalDevice,
			i,
			surface,
			&supportsPresent
		);
		if (	supportsPresent &&
			(queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0	)
		{
			queueFamilyIndices->graphicsFamily = i;
			queueFamilyIndices->presentFamily = i;
			foundSuitableDevice = 1;
			break;
		}
	}

	SDL_stack_free(queueProps);

	if (foundSuitableDevice)
	{
		/* We'd really like a discrete GPU, but it's OK either way! */
		renderer->vkGetPhysicalDeviceProperties(
			physicalDevice,
			&deviceProperties
		);
		if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			*isIdeal = 1;
		}
		return 1;
	}

	/* This device is useless for us, next! */
	return 0;
}

static uint8_t VULKAN_INTERNAL_DeterminePhysicalDevice(
	VulkanRenderer *renderer,
	const char **deviceExtensionNames,
	uint32_t deviceExtensionCount
) {
	VkResult vulkanResult;
	VkPhysicalDevice *physicalDevices;
	uint32_t physicalDeviceCount, i, suitableIndex;
	VkPhysicalDevice physicalDevice;
	QueueFamilyIndices queueFamilyIndices;
	uint8_t isIdeal;

	vulkanResult = renderer->vkEnumeratePhysicalDevices(
		renderer->instance,
		&physicalDeviceCount,
		NULL
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError(
			"vkEnumeratePhysicalDevices failed: %s",
			VkErrorMessages(vulkanResult)
		);
		return 0;
	}

	if (physicalDeviceCount == 0)
	{
		REFRESH_LogError("Failed to find any GPUs with Vulkan support");
		return 0;
	}

	physicalDevices = SDL_stack_alloc(VkPhysicalDevice, physicalDeviceCount);

	vulkanResult = renderer->vkEnumeratePhysicalDevices(
		renderer->instance,
		&physicalDeviceCount,
		physicalDevices
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError(
			"vkEnumeratePhysicalDevices failed: %s",
			VkErrorMessages(vulkanResult)
		);
		SDL_stack_free(physicalDevices);
		return 0;
	}

	/* Any suitable device will do, but we'd like the best */
	suitableIndex = -1;
	for (i = 0; i < physicalDeviceCount; i += 1)
	{
		if (VULKAN_INTERNAL_IsDeviceSuitable(
			renderer,
			physicalDevices[i],
			deviceExtensionNames,
			deviceExtensionCount,
			renderer->surface,
			&queueFamilyIndices,
			&isIdeal
		)) {
			suitableIndex = i;
			if (isIdeal)
			{
				/* This is the one we want! */
				break;
			}
		}
	}

	if (suitableIndex != -1)
	{
		physicalDevice = physicalDevices[suitableIndex];
	}
	else
	{
		REFRESH_LogError("No suitable physical devices found");
		SDL_stack_free(physicalDevices);
		return 0;
	}

	renderer->physicalDevice = physicalDevice;
	renderer->queueFamilyIndices = queueFamilyIndices;

	renderer->physicalDeviceDriverProperties.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR;
	renderer->physicalDeviceDriverProperties.pNext = NULL;

	renderer->physicalDeviceProperties.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	renderer->physicalDeviceProperties.pNext =
		&renderer->physicalDeviceDriverProperties;

	renderer->vkGetPhysicalDeviceProperties2KHR(
		renderer->physicalDevice,
		&renderer->physicalDeviceProperties
	);

	SDL_stack_free(physicalDevices);
	return 1;
}

static uint8_t VULKAN_INTERNAL_CreateLogicalDevice(
	VulkanRenderer *renderer,
	const char **deviceExtensionNames,
	uint32_t deviceExtensionCount
) {
	VkResult vulkanResult;

	VkDeviceCreateInfo deviceCreateInfo;
	VkPhysicalDeviceFeatures deviceFeatures;

	VkDeviceQueueCreateInfo *queueCreateInfos = SDL_stack_alloc(
		VkDeviceQueueCreateInfo,
		2
	);
	VkDeviceQueueCreateInfo queueCreateInfoGraphics;
	VkDeviceQueueCreateInfo queueCreateInfoPresent;

	int32_t queueInfoCount = 1;
	float queuePriority = 1.0f;

	queueCreateInfoGraphics.sType =
		VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfoGraphics.pNext = NULL;
	queueCreateInfoGraphics.flags = 0;
	queueCreateInfoGraphics.queueFamilyIndex =
		renderer->queueFamilyIndices.graphicsFamily;
	queueCreateInfoGraphics.queueCount = 1;
	queueCreateInfoGraphics.pQueuePriorities = &queuePriority;

	queueCreateInfos[0] = queueCreateInfoGraphics;

	if (renderer->queueFamilyIndices.presentFamily != renderer->queueFamilyIndices.graphicsFamily)
	{
		queueCreateInfoPresent.sType =
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfoPresent.pNext = NULL;
		queueCreateInfoPresent.flags = 0;
		queueCreateInfoPresent.queueFamilyIndex =
			renderer->queueFamilyIndices.presentFamily;
		queueCreateInfoPresent.queueCount = 1;
		queueCreateInfoPresent.pQueuePriorities = &queuePriority;

		queueCreateInfos[1] = queueCreateInfoPresent;
		queueInfoCount += 1;
	}

	/* specifying used device features */

	SDL_zero(deviceFeatures);
	deviceFeatures.occlusionQueryPrecise = VK_TRUE;
	deviceFeatures.fillModeNonSolid = VK_TRUE;

	/* creating the logical device */

	deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceCreateInfo.pNext = NULL;
	deviceCreateInfo.flags = 0;
	deviceCreateInfo.queueCreateInfoCount = queueInfoCount;
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos;
	deviceCreateInfo.enabledLayerCount = 0;
	deviceCreateInfo.ppEnabledLayerNames = NULL;
	deviceCreateInfo.enabledExtensionCount = deviceExtensionCount;
	deviceCreateInfo.ppEnabledExtensionNames = deviceExtensionNames;
	deviceCreateInfo.pEnabledFeatures = &deviceFeatures;

	vulkanResult = renderer->vkCreateDevice(
		renderer->physicalDevice,
		&deviceCreateInfo,
		NULL,
		&renderer->logicalDevice
	);
	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError(
			"vkCreateDevice failed: %s",
			VkErrorMessages(vulkanResult)
		);
		return 0;
	}

	/* Load vkDevice entry points */

	#define VULKAN_DEVICE_FUNCTION(ext, ret, func, params) \
		renderer->func = (vkfntype_##func) \
			renderer->vkGetDeviceProcAddr( \
				renderer->logicalDevice, \
				#func \
			);
	#include "Refresh_Driver_Vulkan_vkfuncs.h"

	renderer->vkGetDeviceQueue(
		renderer->logicalDevice,
		renderer->queueFamilyIndices.graphicsFamily,
		0,
		&renderer->graphicsQueue
	);

	renderer->vkGetDeviceQueue(
		renderer->logicalDevice,
		renderer->queueFamilyIndices.presentFamily,
		0,
		&renderer->presentQueue
	);

	SDL_stack_free(queueCreateInfos);
	return 1;
}

static REFRESH_Device* VULKAN_CreateDevice(
	REFRESH_PresentationParameters *presentationParameters,
    uint8_t debugMode
) {
    REFRESH_Device *result;
    VulkanRenderer *renderer;

    VkResult vulkanResult;
	uint32_t i;

    /* Variables: Create fence and semaphores */
	VkFenceCreateInfo fenceInfo;
	VkSemaphoreCreateInfo semaphoreInfo;

	/* Variables: Create command pool and command buffer */
	VkCommandPoolCreateInfo commandPoolCreateInfo;
	VkCommandBufferAllocateInfo commandBufferAllocateInfo;

	/* Variables: Shader param layouts */
	VkDescriptorSetLayoutCreateInfo setLayoutCreateInfo;
	VkDescriptorSetLayoutBinding emptyVertexSamplerLayoutBinding;
	VkDescriptorSetLayoutBinding emptyFragmentSamplerLayoutBinding;
	VkDescriptorSetLayoutBinding vertexParamLayoutBinding;
	VkDescriptorSetLayoutBinding fragmentParamLayoutBinding;

	/* Variables: UBO Creation */
	VkDescriptorPoolCreateInfo defaultDescriptorPoolInfo;
	VkDescriptorPoolSize poolSizes[2];
	VkDescriptorSetAllocateInfo descriptorAllocateInfo;

    result = (REFRESH_Device*) SDL_malloc(sizeof(REFRESH_Device));
    ASSIGN_DRIVER(VULKAN)

    renderer = (VulkanRenderer*) SDL_malloc(sizeof(VulkanRenderer));

	/* Load Vulkan entry points */
	if (SDL_Vulkan_LoadLibrary(NULL) < 0)
	{
		REFRESH_LogWarn("Vulkan: SDL_Vulkan_LoadLibrary failed!");
		return 0;
	}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) SDL_Vulkan_GetVkGetInstanceProcAddr();
#pragma GCC diagnostic pop
	if (vkGetInstanceProcAddr == NULL)
	{
		REFRESH_LogWarn(
			"SDL_Vulkan_GetVkGetInstanceProcAddr(): %s",
			SDL_GetError()
		);
		return 0;
	}

	#define VULKAN_GLOBAL_FUNCTION(name)								\
		name = (PFN_##name) vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);			\
		if (name == NULL)									\
		{											\
			REFRESH_LogWarn("vkGetInstanceProcAddr(VK_NULL_HANDLE, \"" #name "\") failed");	\
			return 0;									\
		}
	#include "Refresh_Driver_Vulkan_vkfuncs.h"

    result->driverData = (REFRESH_Renderer*) renderer;
    renderer->debugMode = debugMode;
    renderer->headless = presentationParameters->deviceWindowHandle == NULL;

    /* Create the VkInstance */
	if (!VULKAN_INTERNAL_CreateInstance(renderer, presentationParameters->deviceWindowHandle))
	{
		REFRESH_LogError("Error creating vulkan instance");
		return NULL;
	}

    renderer->deviceWindowHandle = presentationParameters->deviceWindowHandle;
	renderer->presentMode = presentationParameters->presentMode;

	/*
	 * Create the WSI vkSurface
	 */

	if (!SDL_Vulkan_CreateSurface(
		(SDL_Window*) renderer->deviceWindowHandle,
		renderer->instance,
		&renderer->surface
	)) {
		REFRESH_LogError(
			"SDL_Vulkan_CreateSurface failed: %s",
			SDL_GetError()
		);
		return NULL;
	}

	/*
	 * Get vkInstance entry points
	 */

	#define VULKAN_INSTANCE_FUNCTION(ext, ret, func, params) \
		renderer->func = (vkfntype_##func) vkGetInstanceProcAddr(renderer->instance, #func);
	#include "Refresh_Driver_Vulkan_vkfuncs.h"

	/*
	 * Choose/Create vkDevice
	 */

	if (SDL_strcmp(SDL_GetPlatform(), "Stadia") != 0)
	{
		deviceExtensionCount -= 1;
	}
	if (!VULKAN_INTERNAL_DeterminePhysicalDevice(
		renderer,
		deviceExtensionNames,
		deviceExtensionCount
	)) {
		REFRESH_LogError("Failed to determine a suitable physical device");
		return NULL;
	}

	REFRESH_LogInfo("Refresh Driver: Vulkan");
	REFRESH_LogInfo(
		"Vulkan Device: %s",
		renderer->physicalDeviceProperties.properties.deviceName
	);
	REFRESH_LogInfo(
		"Vulkan Driver: %s %s",
		renderer->physicalDeviceDriverProperties.driverName,
		renderer->physicalDeviceDriverProperties.driverInfo
	);
	REFRESH_LogInfo(
		"Vulkan Conformance: %u.%u.%u",
		renderer->physicalDeviceDriverProperties.conformanceVersion.major,
		renderer->physicalDeviceDriverProperties.conformanceVersion.minor,
		renderer->physicalDeviceDriverProperties.conformanceVersion.patch
	);
	REFRESH_LogWarn(
		"\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
		"! Refresh Vulkan is still in development!    !\n"
        "! The API is unstable and subject to change! !\n"
        "! You have been warned!                      !\n"
		"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
	);

	if (!VULKAN_INTERNAL_CreateLogicalDevice(
		renderer,
		deviceExtensionNames,
		deviceExtensionCount
	)) {
		REFRESH_LogError("Failed to create logical device");
		return NULL;
	}

	/*
	 * Create initial swapchain
	 */

    if (!renderer->headless)
    {
        if (VULKAN_INTERNAL_CreateSwapchain(renderer) != CREATE_SWAPCHAIN_SUCCESS)
        {
            REFRESH_LogError("Failed to create swap chain");
            return NULL;
        }
    }

	renderer->needNewSwapChain = 0;
	renderer->shouldPresent = 0;
	renderer->swapChainImageAcquired = 0;

	/*
	 * Create fence and semaphores
	 */

	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.pNext = NULL;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphoreInfo.pNext = NULL;
	semaphoreInfo.flags = 0;

	vulkanResult = renderer->vkCreateSemaphore(
		renderer->logicalDevice,
		&semaphoreInfo,
		NULL,
		&renderer->imageAvailableSemaphore
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateFence", vulkanResult);
		return NULL;
	}

	vulkanResult = renderer->vkCreateSemaphore(
		renderer->logicalDevice,
		&semaphoreInfo,
		NULL,
		&renderer->renderFinishedSemaphore
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateSemaphore", vulkanResult);
		return NULL;
	}

	vulkanResult = renderer->vkCreateFence(
		renderer->logicalDevice,
		&fenceInfo,
		NULL,
		&renderer->inFlightFence
	);

	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateSemaphore", vulkanResult);
		return NULL;
	}

	/* Threading */

	renderer->allocatorLock = SDL_CreateMutex();
	renderer->commandLock = SDL_CreateMutex();
	renderer->disposeLock = SDL_CreateMutex();

	/*
	 * Create command pool and buffers
	 */

	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.pNext = NULL;
	commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPoolCreateInfo.queueFamilyIndex = renderer->queueFamilyIndices.graphicsFamily;
	vulkanResult = renderer->vkCreateCommandPool(
		renderer->logicalDevice,
		&commandPoolCreateInfo,
		NULL,
		&renderer->commandPool
	);
	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkCreateCommandPool", vulkanResult);
	}

	renderer->allocatedCommandBufferCount = 4;
	renderer->inactiveCommandBuffers = SDL_malloc(sizeof(VkCommandBuffer) * renderer->allocatedCommandBufferCount);
	renderer->activeCommandBuffers = SDL_malloc(sizeof(VkCommandBuffer) * renderer->allocatedCommandBufferCount);
	renderer->submittedCommandBuffers = SDL_malloc(sizeof(VkCommandBuffer) * renderer->allocatedCommandBufferCount);
	renderer->inactiveCommandBufferCount = renderer->allocatedCommandBufferCount;
	renderer->activeCommandBufferCount = 0;
	renderer->submittedCommandBufferCount = 0;

	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.pNext = NULL;
	commandBufferAllocateInfo.commandPool = renderer->commandPool;
	commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocateInfo.commandBufferCount = renderer->allocatedCommandBufferCount;
	vulkanResult = renderer->vkAllocateCommandBuffers(
		renderer->logicalDevice,
		&commandBufferAllocateInfo,
		renderer->inactiveCommandBuffers
	);
	if (vulkanResult != VK_SUCCESS)
	{
		LogVulkanResult("vkAllocateCommandBuffers", vulkanResult);
	}

	renderer->currentCommandCount = 0;

	VULKAN_INTERNAL_BeginCommandBuffer(renderer);

	/* Memory Allocator */

		renderer->memoryAllocator = (VulkanMemoryAllocator*) SDL_malloc(
		sizeof(VulkanMemoryAllocator)
	);

	for (i = 0; i < VK_MAX_MEMORY_TYPES; i += 1)
	{
		renderer->memoryAllocator->subAllocators[i].nextAllocationSize = STARTING_ALLOCATION_SIZE;
		renderer->memoryAllocator->subAllocators[i].allocations = NULL;
		renderer->memoryAllocator->subAllocators[i].allocationCount = 0;
		renderer->memoryAllocator->subAllocators[i].sortedFreeRegions = SDL_malloc(
			sizeof(VulkanMemoryFreeRegion*) * 4
		);
		renderer->memoryAllocator->subAllocators[i].sortedFreeRegionCount = 0;
		renderer->memoryAllocator->subAllocators[i].sortedFreeRegionCapacity = 4;
	}

	/* UBO Data */

	renderer->vertexUBO = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if (!VULKAN_INTERNAL_CreateBuffer(
		renderer,
		UBO_ACTUAL_SIZE,
		RESOURCE_ACCESS_VERTEX_SHADER_READ_UNIFORM_BUFFER,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		1,
		renderer->vertexUBO
	)) {
		REFRESH_LogError("Failed to create vertex UBO!");
		return NULL;
	}

	renderer->fragmentUBO = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if (!VULKAN_INTERNAL_CreateBuffer(
		renderer,
		UBO_ACTUAL_SIZE,
		RESOURCE_ACCESS_FRAGMENT_SHADER_READ_UNIFORM_BUFFER,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		1,
		renderer->fragmentUBO
	)) {
		REFRESH_LogError("Failed to create fragment UBO!");
		return NULL;
	}

	renderer->minUBOAlignment = renderer->physicalDeviceProperties.properties.limits.minUniformBufferOffsetAlignment;
	renderer->vertexUBOOffset = 0;
	renderer->vertexUBOBlockIncrement = 0;
	renderer->fragmentUBOOffset = 0;
	renderer->fragmentUBOBlockIncrement = 0;

	/* Set up UBO layouts */

	emptyVertexSamplerLayoutBinding.binding = 0;
	emptyVertexSamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	emptyVertexSamplerLayoutBinding.descriptorCount = 0;
	emptyVertexSamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	emptyVertexSamplerLayoutBinding.pImmutableSamplers = NULL;

	setLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutCreateInfo.pNext = NULL;
	setLayoutCreateInfo.flags = 0;
	setLayoutCreateInfo.bindingCount = 1;
	setLayoutCreateInfo.pBindings = &emptyVertexSamplerLayoutBinding;

	vulkanResult = renderer->vkCreateDescriptorSetLayout(
		renderer->logicalDevice,
		&setLayoutCreateInfo,
		NULL,
		&renderer->emptyVertexSamplerLayout
	);

	emptyFragmentSamplerLayoutBinding.binding = 0;
	emptyFragmentSamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	emptyFragmentSamplerLayoutBinding.descriptorCount = 0;
	emptyFragmentSamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	emptyFragmentSamplerLayoutBinding.pImmutableSamplers = NULL;

	setLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutCreateInfo.pNext = NULL;
	setLayoutCreateInfo.flags = 0;
	setLayoutCreateInfo.bindingCount = 1;
	setLayoutCreateInfo.pBindings = &emptyFragmentSamplerLayoutBinding;

	vulkanResult = renderer->vkCreateDescriptorSetLayout(
		renderer->logicalDevice,
		&setLayoutCreateInfo,
		NULL,
		&renderer->emptyFragmentSamplerLayout
	);

	vertexParamLayoutBinding.binding = 0;
	vertexParamLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	vertexParamLayoutBinding.descriptorCount = 1;
	vertexParamLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	vertexParamLayoutBinding.pImmutableSamplers = NULL;

	setLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutCreateInfo.pNext = NULL;
	setLayoutCreateInfo.flags = 0;
	setLayoutCreateInfo.bindingCount = 1;
	setLayoutCreateInfo.pBindings = &vertexParamLayoutBinding;

	vulkanResult = renderer->vkCreateDescriptorSetLayout(
		renderer->logicalDevice,
		&setLayoutCreateInfo,
		NULL,
		&renderer->vertexParamLayout
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to create vertex UBO layout!");
		return NULL;
	}

	fragmentParamLayoutBinding.binding = 0;
	fragmentParamLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	fragmentParamLayoutBinding.descriptorCount = 1;
	fragmentParamLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragmentParamLayoutBinding.pImmutableSamplers = NULL;

	setLayoutCreateInfo.bindingCount = 1;
	setLayoutCreateInfo.pBindings = &fragmentParamLayoutBinding;

	vulkanResult = renderer->vkCreateDescriptorSetLayout(
		renderer->logicalDevice,
		&setLayoutCreateInfo,
		NULL,
		&renderer->fragmentParamLayout
	);

	if (vulkanResult != VK_SUCCESS)
	{
		REFRESH_LogError("Failed to create fragment UBO layout!");
		return NULL;
	}

	/* Default Descriptors */

	/* default empty sampler descriptor sets */
	poolSizes[0].descriptorCount = 2;
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	/* UBO descriptor sets */
	poolSizes[1].descriptorCount = UBO_POOL_SIZE;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;

	defaultDescriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	defaultDescriptorPoolInfo.pNext = NULL;
	defaultDescriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	defaultDescriptorPoolInfo.maxSets = UBO_POOL_SIZE + 2;
	defaultDescriptorPoolInfo.poolSizeCount = 2;
	defaultDescriptorPoolInfo.pPoolSizes = poolSizes;

	renderer->vkCreateDescriptorPool(
		renderer->logicalDevice,
		&defaultDescriptorPoolInfo,
		NULL,
		&renderer->defaultDescriptorPool
	);

	descriptorAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorAllocateInfo.pNext = NULL;
	descriptorAllocateInfo.descriptorPool = renderer->defaultDescriptorPool;
	descriptorAllocateInfo.descriptorSetCount = 1;
	descriptorAllocateInfo.pSetLayouts = &renderer->emptyVertexSamplerLayout;

	renderer->vkAllocateDescriptorSets(
		renderer->logicalDevice,
		&descriptorAllocateInfo,
		&renderer->emptyVertexSamplerDescriptorSet
	);

	descriptorAllocateInfo.pSetLayouts = &renderer->emptyFragmentSamplerLayout;

	renderer->vkAllocateDescriptorSets(
		renderer->logicalDevice,
		&descriptorAllocateInfo,
		&renderer->emptyFragmentSamplerDescriptorSet
	);

	/* Initialize buffer space */

	renderer->buffersInUseCapacity = 32;
	renderer->buffersInUseCount = 0;
	renderer->buffersInUse = (VulkanBuffer**)SDL_malloc(
		sizeof(VulkanBuffer*) * renderer->buffersInUseCapacity
	);

	renderer->submittedBufferCapacity = 32;
	renderer->submittedBufferCount = 0;
	renderer->submittedBuffers = (VulkanBuffer**)SDL_malloc(
		sizeof(VulkanBuffer*) * renderer->submittedBufferCapacity
	);

	/* Staging Buffer */

	renderer->textureStagingBuffer = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if (!VULKAN_INTERNAL_CreateBuffer(
		renderer,
		TEXTURE_STAGING_SIZE,
		RESOURCE_ACCESS_MEMORY_TRANSFER_READ_WRITE,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		1,
		renderer->textureStagingBuffer
	)) {
		REFRESH_LogError("Failed to create texture staging buffer!");
		return NULL;
	}

	/* Dummy Uniform Buffers */

	renderer->dummyVertexUniformBuffer = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if (!VULKAN_INTERNAL_CreateBuffer(
		renderer,
		16,
		RESOURCE_ACCESS_VERTEX_SHADER_READ_UNIFORM_BUFFER,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		1,
		renderer->dummyVertexUniformBuffer
	)) {
		REFRESH_LogError("Failed to create dummy vertex uniform buffer!");
		return NULL;
	}

	renderer->dummyFragmentUniformBuffer = (VulkanBuffer*) SDL_malloc(sizeof(VulkanBuffer));

	if (!VULKAN_INTERNAL_CreateBuffer(
		renderer,
		16,
		RESOURCE_ACCESS_FRAGMENT_SHADER_READ_UNIFORM_BUFFER,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		1,
		renderer->dummyFragmentUniformBuffer
	)) {
		REFRESH_LogError("Failed to create dummy fragment uniform buffer!");
		return NULL;
	}

	/* Initialize caches */

	for (i = 0; i < NUM_PIPELINE_LAYOUT_BUCKETS; i += 1)
	{
		renderer->pipelineLayoutHashTable.buckets[i].elements = NULL;
		renderer->pipelineLayoutHashTable.buckets[i].count = 0;
		renderer->pipelineLayoutHashTable.buckets[i].capacity = 0;
	}

	for (i = 0; i < NUM_DESCRIPTOR_SET_LAYOUT_BUCKETS; i += 1)
	{
		renderer->samplerDescriptorSetLayoutHashTable.buckets[i].elements = NULL;
		renderer->samplerDescriptorSetLayoutHashTable.buckets[i].count = 0;
		renderer->samplerDescriptorSetLayoutHashTable.buckets[i].capacity = 0;
	}

	/* Descriptor Pools */

	renderer->descriptorPools = NULL;
	renderer->descriptorPoolCount = 0;

	/* State tracking */

	renderer->currentGraphicsPipeline = NULL;
	renderer->currentFramebuffer = NULL;

	/* Deferred destroy storage */

	renderer->colorTargetsToDestroyCapacity = 16;
	renderer->colorTargetsToDestroyCount = 0;

	renderer->colorTargetsToDestroy = (VulkanColorTarget**) SDL_malloc(
		sizeof(VulkanColorTarget*) *
		renderer->colorTargetsToDestroyCapacity
	);

	renderer->submittedColorTargetsToDestroyCapacity = 16;
	renderer->submittedColorTargetsToDestroyCount = 0;

	renderer->submittedColorTargetsToDestroy = (VulkanColorTarget**) SDL_malloc(
		sizeof(VulkanColorTarget*) *
		renderer->submittedColorTargetsToDestroyCapacity
	);

	renderer->depthStencilTargetsToDestroyCapacity = 16;
	renderer->depthStencilTargetsToDestroyCount = 0;

	renderer->depthStencilTargetsToDestroy = (VulkanDepthStencilTarget**) SDL_malloc(
		sizeof(VulkanDepthStencilTarget*) *
		renderer->depthStencilTargetsToDestroyCapacity
	);

	renderer->submittedDepthStencilTargetsToDestroyCapacity = 16;
	renderer->submittedDepthStencilTargetsToDestroyCount = 0;

	renderer->submittedDepthStencilTargetsToDestroy = (VulkanDepthStencilTarget**) SDL_malloc(
		sizeof(VulkanDepthStencilTarget*) *
		renderer->submittedDepthStencilTargetsToDestroyCapacity
	);

	renderer->texturesToDestroyCapacity = 16;
	renderer->texturesToDestroyCount = 0;

	renderer->texturesToDestroy = (VulkanTexture**)SDL_malloc(
		sizeof(VulkanTexture*) *
		renderer->texturesToDestroyCapacity
	);

	renderer->submittedTexturesToDestroyCapacity = 16;
	renderer->submittedTexturesToDestroyCount = 0;

	renderer->submittedTexturesToDestroy = (VulkanTexture**)SDL_malloc(
		sizeof(VulkanTexture*) *
		renderer->submittedTexturesToDestroyCapacity
	);

	renderer->buffersToDestroyCapacity = 16;
	renderer->buffersToDestroyCount = 0;

	renderer->buffersToDestroy = (VulkanBuffer**) SDL_malloc(
		sizeof(VulkanBuffer*) *
		renderer->buffersToDestroyCapacity
	);

	renderer->submittedBuffersToDestroyCapacity = 16;
	renderer->submittedBuffersToDestroyCount = 0;

	renderer->submittedBuffersToDestroy = (VulkanBuffer**) SDL_malloc(
		sizeof(VulkanBuffer*) *
		renderer->submittedBuffersToDestroyCapacity
	);

	renderer->graphicsPipelinesToDestroyCapacity = 16;
	renderer->graphicsPipelinesToDestroyCount = 0;

	renderer->graphicsPipelinesToDestroy = (VulkanGraphicsPipeline**) SDL_malloc(
		sizeof(VulkanGraphicsPipeline*) *
		renderer->graphicsPipelinesToDestroyCapacity
	);

	renderer->submittedGraphicsPipelinesToDestroyCapacity = 16;
	renderer->submittedGraphicsPipelinesToDestroyCount = 0;

	renderer->submittedGraphicsPipelinesToDestroy = (VulkanGraphicsPipeline**) SDL_malloc(
		sizeof(VulkanGraphicsPipeline*) *
		renderer->submittedGraphicsPipelinesToDestroyCapacity
	);

	renderer->shaderModulesToDestroyCapacity = 16;
	renderer->shaderModulesToDestroyCount = 0;

	renderer->shaderModulesToDestroy = (VkShaderModule*) SDL_malloc(
		sizeof(VkShaderModule) *
		renderer->shaderModulesToDestroyCapacity
	);

	renderer->submittedShaderModulesToDestroyCapacity = 16;
	renderer->submittedShaderModulesToDestroyCount = 0;

	renderer->submittedShaderModulesToDestroy = (VkShaderModule*) SDL_malloc(
		sizeof(VkShaderModule) *
		renderer->submittedShaderModulesToDestroyCapacity
	);

	renderer->samplersToDestroyCapacity = 16;
	renderer->samplersToDestroyCount = 0;

	renderer->samplersToDestroy = (VkSampler*) SDL_malloc(
		sizeof(VkSampler) *
		renderer->samplersToDestroyCapacity
	);

	renderer->submittedSamplersToDestroyCapacity = 16;
	renderer->submittedSamplersToDestroyCount = 0;

	renderer->submittedSamplersToDestroy = (VkSampler*) SDL_malloc(
		sizeof(VkSampler) *
		renderer->submittedSamplersToDestroyCapacity
	);

	renderer->framebuffersToDestroyCapacity = 16;
	renderer->framebuffersToDestroyCount = 0;

	renderer->framebuffersToDestroy = (VulkanFramebuffer**) SDL_malloc(
		sizeof(VulkanFramebuffer*) *
		renderer->framebuffersToDestroyCapacity
	);

	renderer->submittedFramebuffersToDestroyCapacity = 16;
	renderer->submittedFramebuffersToDestroyCount = 0;

	renderer->submittedFramebuffersToDestroy = (VulkanFramebuffer**) SDL_malloc(
		sizeof(VulkanFramebuffer*) *
		renderer->submittedFramebuffersToDestroyCapacity
	);

	renderer->renderPassesToDestroyCapacity = 16;
	renderer->renderPassesToDestroyCount = 0;

	renderer->renderPassesToDestroy = (VkRenderPass*) SDL_malloc(
		sizeof(VkRenderPass) *
		renderer->renderPassesToDestroyCapacity
	);

	renderer->submittedRenderPassesToDestroyCapacity = 16;
	renderer->submittedRenderPassesToDestroyCount = 0;

	renderer->submittedRenderPassesToDestroy = (VkRenderPass*) SDL_malloc(
		sizeof(VkRenderPass) *
		renderer->submittedRenderPassesToDestroyCapacity
	);

    return result;
}

REFRESH_Driver VulkanDriver = {
    "Vulkan",
    VULKAN_CreateDevice
};

#endif //REFRESH_DRIVER_VULKAN
