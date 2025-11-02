#include "veekay/input.hpp"
#include <cstdint>
#include <climits>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>

#define _USE_MATH_DEFINES
#include <math.h>

#include <veekay/veekay.hpp>

#include <imgui.h>
#include <vulkan/vulkan_core.h>

#include <lodepng.h>

namespace {
// ОСНОВНЫЕ ЗАДАНИЯ:
// 1. Генерация тора (200 вершин) - generateTorusMesh()
// 2. Анимация цвета через sin(time) - shaders/shader.frag
// 3. Вращение тора вокруг оси
// 4. UI элементы - update()
// 5. Инициализация тора - initialize()
//
// ДОПОЛНИТЕЛЬНЫЕ ЗАДАНИЯ:
// 1. Переключение проекции (перспективная/ортографическая):
//    - Camera::orthographic()
//    - Camera::view_projection()
// 2. Управление анимацией (пауза и реверс):
//    - update()


constexpr uint32_t max_models = 1024;

struct Vertex {
	veekay::vec3 position; // Позиция точки на объекте
	veekay::vec3 normal;
	veekay::vec2 uv; // текстура
	// NOTE: You can add more attributes
};

struct SceneUniforms {
	veekay::mat4 view_projection;
	float time;  // Для анимации цвета через sin(time)
	float _pad0, _pad1, _pad2;  // Выравнивание для std140
};

struct ModelUniforms {
	veekay::mat4 model;
	veekay::vec3 albedo_color; float _pad0;
};

struct Mesh {
	veekay::graphics::Buffer* vertex_buffer;
	veekay::graphics::Buffer* index_buffer;
	uint32_t indices;
};

struct Transform {
	veekay::vec3 position = {};
	veekay::vec3 scale = {1.0f, 1.0f, 1.0f};
	veekay::vec3 rotation = {};

	// NOTE: Model matrix (translation, rotation and scaling)
	veekay::mat4 matrix() const;
};

struct Model {
	Mesh mesh;
	Transform transform;
	veekay::vec3 albedo_color;
};

struct Camera {
	constexpr static float default_fov = 60.0f;
	constexpr static float default_near_plane = 0.01f;
	constexpr static float default_far_plane = 100.0f;

	veekay::vec3 position = {};
	veekay::vec3 rotation = {};

	float fov = default_fov;
	float near_plane = default_near_plane;
	float far_plane = default_far_plane;

	bool is_perspective = true;  // Доп. задание 1: флаг типа проекции
	float ortho_size = 10.0f;     // Доп. задание 1: размер видимой области ортографической проекции

	// NOTE: View matrix of camera (inverse of a transform)
	veekay::mat4 view() const;

	// NOTE: Orthographic projection matrix
	veekay::mat4 orthographic(float aspect_ratio) const;

	// NOTE: View and projection composition
	veekay::mat4 view_projection(float aspect_ratio) const;
};

// NOTE: Scene objects
inline namespace {
	Camera camera{
		.position = {0.0f, -1.0f, -10.0f}
	};

	std::vector<Model> models;

	// ДОПОЛНИТЕЛЬНЫЕ ЗАДАНИЯ
	// UI variables for additional features
	bool is_animation_paused = false;
	bool is_rotation_reversed = false;
	float rotation_speed = 1.0f;      // UI: скорость вращения
	float animation_time = 0.0f;       // Накопленное время анимации
}

// NOTE: Vulkan objects
inline namespace {
	VkShaderModule vertex_shader_module;
	VkShaderModule fragment_shader_module;

	VkDescriptorPool descriptor_pool;
	VkDescriptorSetLayout descriptor_set_layout;
	VkDescriptorSet descriptor_set;

	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;

	veekay::graphics::Buffer* scene_uniforms_buffer;
	veekay::graphics::Buffer* model_uniforms_buffer;

	Mesh plane_mesh;
	Mesh cube_mesh;

	// Настройка цвета кубика в прямом эфире
	veekay::vec3 cube_color = {0.0f, 1.0f, 0.735f};

	veekay::graphics::Texture* missing_texture;
	VkSampler missing_texture_sampler;

	veekay::graphics::Texture* texture;
	VkSampler texture_sampler;

	// Выравнивание размеров для устранения ошибки Vulkan
	uint32_t min_uniform_buffer_offset_alignment;
	uint32_t aligned_model_uniforms_size;

	Mesh torus_mesh;  // Тор
}

float toRadians(float degrees) {
	return degrees * float(M_PI) / 180.0f;
}

// Тор (200 вершин)
Mesh generateTorusMesh(float majorRadius, float minorRadius, 
                       uint32_t majorSegments, uint32_t minorSegments) {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	
	// Двойной цикл для генерации вершин
	for (uint32_t i = 0; i <= majorSegments; ++i) {
		float u = float(i) / float(majorSegments) * 2.0f * float(M_PI);
		float cos_u = cosf(u);
		float sin_u = sinf(u);
		
		for (uint32_t j = 0; j <= minorSegments; ++j) {
			float v = float(j) / float(minorSegments) * 2.0f * float(M_PI);
			float cos_v = cosf(v);
			float sin_v = sinf(v);
			
			// Параметрические уравнения тора
			// x = (R + r*cos(v))*cos(u)
			// y = (R + r*cos(v))*sin(u)
			// z = r*sin(v)
			float x = (majorRadius + minorRadius * cos_v) * cos_u;
			float y = (majorRadius + minorRadius * cos_v) * sin_u;
			float z = minorRadius * sin_v;
			
			veekay::vec3 position{x, y, z};
			
			// Нормаль: вектор от центра тора к поверхности (нормализованный)
			veekay::vec3 center_to_surface{cos_u * cos_v, sin_u * cos_v, sin_v};

			// Нормализуем вектор нормали
			float len = sqrtf(center_to_surface.x * center_to_surface.x + 
			                  center_to_surface.y * center_to_surface.y + 
			                  center_to_surface.z * center_to_surface.z);
			veekay::vec3 normal{center_to_surface.x / len, 
			                   center_to_surface.y / len, 
			                   center_to_surface.z / len};
			
			// UV координаты
			veekay::vec2 uv{float(i) / float(majorSegments), 
			                float(j) / float(minorSegments)};
			
			vertices.push_back(Vertex{position, normal, uv});
		}
	}
	
	// Генерация индексов для треугольников
	for (uint32_t i = 0; i < majorSegments; ++i) {
		for (uint32_t j = 0; j < minorSegments; ++j) {
			uint32_t current = i * (minorSegments + 1) + j;
			uint32_t next = current + minorSegments + 1;
			
			// Первый треугольник
			indices.push_back(current);
			indices.push_back(next);
			indices.push_back(current + 1);
			
			// Второй треугольник
			indices.push_back(current + 1);
			indices.push_back(next);
			indices.push_back(next + 1);
		}
	}
	
	Mesh mesh;
	mesh.vertex_buffer = new veekay::graphics::Buffer(
		vertices.size() * sizeof(Vertex), vertices.data(),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	
	mesh.index_buffer = new veekay::graphics::Buffer(
		indices.size() * sizeof(uint32_t), indices.data(),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	
	mesh.indices = uint32_t(indices.size());
	
	return mesh;
}

veekay::mat4 Transform::matrix() const {
	// Пполная матрица преобразования (scaling + rotation + translation)
	
	auto s = veekay::mat4::scaling(scale);
	
	// Вращение вокруг осей X, Y, Z (в радианах)
	auto rx = veekay::mat4::rotation(veekay::vec3{1.0f, 0.0f, 0.0f}, rotation.x);
	auto ry = veekay::mat4::rotation(veekay::vec3{0.0f, 1.0f, 0.0f}, rotation.y);
	auto rz = veekay::mat4::rotation(veekay::vec3{0.0f, 0.0f, 1.0f}, rotation.z);
	
	auto r = rz * ry * rx;  // Порядок: сначала X, потом Y, потом Z
	
	auto t = veekay::mat4::translation(position);
	
	// Порядок применения: Scale -> Rotate -> Translate
	return t * r * s;
}

veekay::mat4 Camera::view() const {
	// TODO: Rotation

	auto t = veekay::mat4::translation(-position);

	return t;
}

// доп. задание 1: Ортографическая проекция

// Ортографическая проекция работает следующим образом:
// - В отличие от перспективной проекции, ортографическая проецирует объекты параллельными лучами
// - Объекты не становятся меньше с расстоянием (нет перспективного искажения)
// - Параметры left, right, bottom, top определяют видимую область (видимый объем)
// - near и far определяют диапазон глубины
// - Матрица ортографической проекции преобразует координаты из видимого объема в NDC (Normalized Device Coordinates) [-1, 1]

veekay::mat4 Camera::orthographic(float aspect_ratio) const {
	veekay::mat4 result{};
	
	// Вычисляем размеры для ортографической проекции
	// Используем настраиваемый размер видимой области из camera.ortho_size
	float height = ortho_size;  // Размер видимой области (половина высоты)
	float width = height * aspect_ratio;  // Ширина пропорциональна высоте и aspect ratio
	
	// Ортографическая матрица проекции
	// Определяем границы видимого объема (фрустума)
	float l = -width;   // left
	float r = width;     // right
	float b = -height;   // bottom
	float t = height;    // top
	float n = near_plane; // near
	float f = far_plane;  // far
	
	// Ортографическая матрица проекции (для Vulkan с Z в [0, 1]):
	// Перспективная проекция использует: result[2][2] = far/(far-near), result[3][2] = (-near*far)/(far-near)
	// Это формула для Vulkan с Z в диапазоне [0, 1] в NDC
	
	result[0][0] = 2.0f / (r - l);
	result[0][1] = 0.0f;
	result[0][2] = 0.0f;
	result[0][3] = 0.0f;
	
	result[1][0] = 0.0f;
	result[1][1] = 2.0f / (t - b);
	result[1][2] = 0.0f;
	result[1][3] = 0.0f;

	result[2][0] = 0.0f;
	result[2][1] = 0.0f;
	result[2][2] = 1.0f / (f - n);  // Для Z в [0, 1], как в перспективной проекции
	result[2][3] = 0.0f;
	
	result[3][0] = -(r + l) / (r - l);   // Смещение по X для центрирования
	result[3][1] = -(t + b) / (t - b);   // Смещение по Y для центрирования
	result[3][2] = -n / (f - n);         // Смещение по Z для Vulkan
	result[3][3] = 1.0f;
	
	return result;
}

veekay::mat4 Camera::view_projection(float aspect_ratio) const {
	// Доп. задание 1: Выбор типа проекции
	veekay::mat4 proj = is_perspective 
		? veekay::mat4::projection(fov, aspect_ratio, near_plane, far_plane)
		: orthographic(aspect_ratio);

	return view() * proj;
}

// NOTE: Loads shader byte code from file
// NOTE: Your shaders are compiled via CMake with this code too, look it up
VkShaderModule loadShaderModule(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	size_t size = file.tellg();
	std::vector<uint32_t> buffer(size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	file.close();

	VkShaderModuleCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = buffer.data(),
	};

	VkShaderModule result;
	if (vkCreateShaderModule(veekay::app.vk_device, &
	                         info, nullptr, &result) != VK_SUCCESS) {
		return nullptr;
	}

	return result;
}

void initialize(VkCommandBuffer cmd) {
	VkDevice& device = veekay::app.vk_device; // Интерфейс для общения с видеокартой
	VkPhysicalDevice& physical_device = veekay::app.vk_physical_device; //физические параметры видюхи

	{ // NOTE: Build graphics pipeline
		vertex_shader_module = loadShaderModule("./shaders/shader.vert.spv");
		if (!vertex_shader_module) {
			std::cerr << "Failed to load Vulkan vertex shader from file\n";
			veekay::app.running = false;
			return;
		}

		fragment_shader_module = loadShaderModule("./shaders/shader.frag.spv");
		if (!fragment_shader_module) {
			std::cerr << "Failed to load Vulkan fragment shader from file\n";
			veekay::app.running = false;
			return;
		}

		VkPipelineShaderStageCreateInfo stage_infos[2];

		// NOTE: Vertex shader stage
		stage_infos[0] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_shader_module,
			.pName = "main",
		};

		// NOTE: Fragment shader stage
		stage_infos[1] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_shader_module,
			.pName = "main",
		};

		// NOTE: How many bytes does a vertex take?
		VkVertexInputBindingDescription buffer_binding{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX, // Частота подключения к буферу
		};

		// NOTE: Declare vertex attributes
		VkVertexInputAttributeDescription attributes[] = {
			{
				.location = 0, // NOTE: First attribute
				.binding = 0, // NOTE: First vertex buffer
				.format = VK_FORMAT_R32G32B32_SFLOAT, // NOTE: 3-component vector of floats
				.offset = offsetof(Vertex, position), // NOTE: Offset of "position" field in a Vertex struct
			},
			{
				.location = 1,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = offsetof(Vertex, normal),
			},
			{
				.location = 2,
				.binding = 0,
				.format = VK_FORMAT_R32G32_SFLOAT,
				.offset = offsetof(Vertex, uv),
			},
		};

		// NOTE: Describe inputs
		VkPipelineVertexInputStateCreateInfo input_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &buffer_binding,
			.vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
			.pVertexAttributeDescriptions = attributes,
		};

		// NOTE: Every three vertices make up a triangle,
		//       so our vertex buffer contains a "list of triangles"
		VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};

		// NOTE: Declare clockwise triangle order as front-facing
		//       Discard triangles that are facing away
		//       Fill triangles, don't draw lines instaed
		VkPipelineRasterizationStateCreateInfo raster_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_BACK_BIT,
			.frontFace = VK_FRONT_FACE_CLOCKWISE,
			.lineWidth = 1.0f,
		};

		// NOTE: Use 1 sample per pixel
		VkPipelineMultisampleStateCreateInfo sample_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			.sampleShadingEnable = false,
			.minSampleShading = 1.0f,
		};

		VkViewport viewport{
			.x = 0.0f,
			.y = 0.0f,
			.width = static_cast<float>(veekay::app.window_width),
			.height = static_cast<float>(veekay::app.window_height),
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};

		VkRect2D scissor{
			// Обрезка окна
			.offset = {0, 0},
			.extent = {veekay::app.window_width, veekay::app.window_height},
		};

		// NOTE: Let rasterizer draw on the entire window
		VkPipelineViewportStateCreateInfo viewport_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,

			.viewportCount = 1,
			.pViewports = &viewport,

			.scissorCount = 1,
			.pScissors = &scissor,
		};

		// NOTE: Let rasterizer perform depth-testing and overwrite depth values on condition pass
		VkPipelineDepthStencilStateCreateInfo depth_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = true,
			.depthWriteEnable = true,
			.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		};

		// NOTE: Let fragment shader write all the color channels
		VkPipelineColorBlendAttachmentState attachment_info{
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			                  VK_COLOR_COMPONENT_G_BIT |
			                  VK_COLOR_COMPONENT_B_BIT |
			                  VK_COLOR_COMPONENT_A_BIT,
		};

		// NOTE: Let rasterizer just copy resulting pixels onto a buffer, don't blend
		VkPipelineColorBlendStateCreateInfo blend_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,

			.logicOpEnable = false,
			.logicOp = VK_LOGIC_OP_COPY,

			.attachmentCount = 1,
			.pAttachments = &attachment_info
		};

		{
			VkDescriptorPoolSize pools[] = {
				{
					.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.descriptorCount = 8,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
					.descriptorCount = 8,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 8,
				}
			};
			
			VkDescriptorPoolCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = 1,
				.poolSizeCount = sizeof(pools) / sizeof(pools[0]),
				.pPoolSizes = pools,
			};

			if (vkCreateDescriptorPool(device, &info, nullptr,
			                           &descriptor_pool) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan descriptor pool\n";
				veekay::app.running = false;
				return;
			}
		}

		// NOTE: Descriptor set layout specification
		{
			VkDescriptorSetLayoutBinding bindings[] = {
				{
					.binding = 0,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				},
			};

			VkDescriptorSetLayoutCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = sizeof(bindings) / sizeof(bindings[0]),
				.pBindings = bindings,
			};

			if (vkCreateDescriptorSetLayout(device, &info, nullptr,
			                                &descriptor_set_layout) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan descriptor set layout\n";
				veekay::app.running = false;
				return;
			}
		}

		{
			VkDescriptorSetAllocateInfo info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &descriptor_set_layout,
			};

			if (vkAllocateDescriptorSets(device, &info, &descriptor_set) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan descriptor set\n";
				veekay::app.running = false;
				return;
			}
		}

		// NOTE: Declare external data sources, only push constants this time
		VkPipelineLayoutCreateInfo layout_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &descriptor_set_layout,
		};

		// NOTE: Create pipeline layout
		if (vkCreatePipelineLayout(device, &layout_info,
		                           nullptr, &pipeline_layout) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan pipeline layout\n";
			veekay::app.running = false;
			return;
		}
		
		VkGraphicsPipelineCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2,
			.pStages = stage_infos,
			.pVertexInputState = &input_state_info,
			.pInputAssemblyState = &assembly_state_info,
			.pViewportState = &viewport_info,
			.pRasterizationState = &raster_info,
			.pMultisampleState = &sample_info,
			.pDepthStencilState = &depth_info,
			.pColorBlendState = &blend_info,
			.layout = pipeline_layout,
			.renderPass = veekay::app.vk_render_pass,
		};

		// NOTE: Create graphics pipeline
		if (vkCreateGraphicsPipelines(device, nullptr,
		                              1, &info, nullptr, &pipeline) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan pipeline\n";
			veekay::app.running = false;
			return;
		}
	}

	// Исправление ошибки Vulkan о выравнивании
	// ==============================================================
	// NOTE: Get device alignment requirements
	VkPhysicalDeviceProperties physical_device_properties;
	vkGetPhysicalDeviceProperties(physical_device, &physical_device_properties);
	min_uniform_buffer_offset_alignment = static_cast<uint32_t>(
		physical_device_properties.limits.minUniformBufferOffsetAlignment);

	// NOTE: Calculate aligned size for ModelUniforms
	uint32_t model_uniforms_size = sizeof(ModelUniforms);
	aligned_model_uniforms_size = (model_uniforms_size + min_uniform_buffer_offset_alignment - 1) 
		& ~(min_uniform_buffer_offset_alignment - 1);
	// ==============================================================

	scene_uniforms_buffer = new veekay::graphics::Buffer(
		sizeof(SceneUniforms),
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	model_uniforms_buffer = new veekay::graphics::Buffer(
		max_models * aligned_model_uniforms_size,
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	// NOTE: This texture and sampler is used when texture could not be loaded
	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &missing_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		uint32_t pixels[] = {
			0xff000000, 0xffff00ff,
			0xffff00ff, 0xff000000,
		};

		missing_texture = new veekay::graphics::Texture(cmd, 2, 2,
		                                                VK_FORMAT_B8G8R8A8_UNORM,
		                                                pixels);
	}

	{
		VkDescriptorBufferInfo buffer_infos[] = {
			{
				.buffer = scene_uniforms_buffer->buffer,
				.offset = 0,
				.range = sizeof(SceneUniforms),
			},
			{
				.buffer = model_uniforms_buffer->buffer,
				.offset = 0,
				.range = aligned_model_uniforms_size,
			},
		};

		VkWriteDescriptorSet write_infos[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &buffer_infos[0],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 1,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
				.pBufferInfo = &buffer_infos[1],
			},
		};

		vkUpdateDescriptorSets(device, sizeof(write_infos) / sizeof(write_infos[0]),
		                       write_infos, 0, nullptr);
	}

	// NOTE: Plane mesh initialization
	{
		// (v0)------(v1)
		//  |  \       |
		//  |   `--,   |
		//  |       \  |
		// (v3)------(v2)
		std::vector<Vertex> vertices = {
			{{-5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
			{{5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
			{{5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
		};

		std::vector<uint32_t> indices = {
			0, 1, 2, 2, 3, 0
		};

		plane_mesh.vertex_buffer = new veekay::graphics::Buffer(
			vertices.size() * sizeof(Vertex), vertices.data(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		plane_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		plane_mesh.indices = uint32_t(indices.size());
	}

	// NOTE: Cube mesh initialization
	{
		std::vector<Vertex> vertices = {
			{{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
			{{+0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
			{{+0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
			{{-0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},

			{{+0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
			{{+0.5f, -0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
			{{+0.5f, +0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
			{{+0.5f, +0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

			{{+0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
			{{-0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
			{{-0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
			{{+0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},

			{{-0.5f, -0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
			{{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
			{{-0.5f, +0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
			{{-0.5f, +0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

			{{-0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
			{{+0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
			{{+0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},

			{{-0.5f, +0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
			{{+0.5f, +0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
			{{+0.5f, +0.5f, +0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-0.5f, +0.5f, +0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
		};

		std::vector<uint32_t> indices = {
			0, 1, 2, 2, 3, 0,
			4, 5, 6, 6, 7, 4,
			8, 9, 10, 10, 11, 8,
			12, 13, 14, 14, 15, 12,
			16, 17, 18, 18, 19, 16,
			20, 21, 22, 22, 23, 20,
		};

		cube_mesh.vertex_buffer = new veekay::graphics::Buffer(
			vertices.size() * sizeof(Vertex), vertices.data(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		cube_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		cube_mesh.indices = uint32_t(indices.size());
	}

	// Инициализация тора
	// Генерируем тор с ~200 вершинами
	// majorSegments * minorSegments должно дать примерно 200 вершин
	// Например: 12 * 16 = 192 вершины (13 * 17 = 221 вершина)
	torus_mesh = generateTorusMesh(1.5f, 0.5f, 12, 16);

	// NOTE: Add models to scene

	models.clear();

	// Тор
	models.emplace_back(Model{
		.mesh = torus_mesh,
		.transform = Transform{
			.position = {0.0f, -2.0f, 0.0f},
			.scale = {1.0f, 1.0f, 1.0f},
			.rotation = {0.0f, 0.0f, 0.0f},
		},
		.albedo_color = veekay::vec3{2.0f, 2.0f, 2.0f}  // Специальное значение для тора (будет анимироваться в шейдере)
	});

	// Белая плоскость (Сетка куба)
	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {0.0f, 1.0f, 0.0f},
			.scale = {-15.0f, 1.0f, 15.0f},
		},
		.albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f}  // Белая плоскость (не будет анимироваться)
	});

	// Куб
	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {0.0f, -2.0f, 0.0f},
		},
		.albedo_color = cube_color
	});
}

// NOTE: Destroy resources here, do not cause leaks in your program!
void shutdown() {
	VkDevice& device = veekay::app.vk_device;

	vkDestroySampler(device, missing_texture_sampler, nullptr);
	delete missing_texture;

	delete plane_mesh.vertex_buffer;
	delete plane_mesh.index_buffer;

	delete cube_mesh.index_buffer;
	delete cube_mesh.vertex_buffer;

	delete torus_mesh.index_buffer;
	delete torus_mesh.vertex_buffer;

	delete model_uniforms_buffer;
	delete scene_uniforms_buffer;

	vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyShaderModule(device, fragment_shader_module, nullptr);
	vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {

	ImGui::Begin("Controls:");
	ImGui::ColorEdit3("Cube Color", &cube_color.x);
	
	// Slider для FOV камеры
	if (camera.is_perspective) {
		if (ImGui::SliderFloat("FOV", &camera.fov, 30.0f, 120.0f)) {
		}
	}
	
	// Slider для скорости вращения
	if (ImGui::SliderFloat("Rotation Speed", &rotation_speed, 0.0f, 100.0f)) {
	}
	
	// ДОП. ЗАДАНИЕ 1: Переключение проекции
	if (ImGui::Checkbox("Perspective Projection", &camera.is_perspective)) {
	}

	ImGui::SameLine();
	if (!camera.is_perspective) {
		ImGui::Text("(Orthographic)");
	}

	// UI для настройки размера ортографической проекции
	if (!camera.is_perspective) {
		if (ImGui::SliderFloat("Orthographic Size", &camera.ortho_size, 1.0f, 50.0f)) {
			// Размер видимой области ортографической проекции обновляется напрямую

		}
		// Доп инфа
		ImGui::Text("Visible area: %.1f x %.1f (X x Y)", 
		           camera.ortho_size * 2.0f, 
		           camera.ortho_size * 2.0f);
	}
	
	// ДОП. ЗАДАНИЕ 2: Управление анимацией
	if (ImGui::Checkbox("Pause Animation", &is_animation_paused)) {
		// Пауза/возобновление анимации
	}
	
	if (ImGui::Checkbox("Reverse Rotation", &is_rotation_reversed)) {
		// Реверс направления вращения
	}
	
	ImGui::End();

	// ДОП. ЗАДАНИЕ 2: Анимация вращения
	// Обновляем накопленное время анимации (если не на паузе)
	if (!is_animation_paused) {
		animation_time = time * rotation_speed * (is_rotation_reversed ? -1.0f : 1.0f);
	}
	// Если на паузе, animation_time не меняется (сохраняется последнее значение)

	// Обновляем вращение тора вокруг своей оси (Y-axis)
	if (!models.empty()) {
		models[0].transform.rotation.y = animation_time;
	}

	if (!ImGui::IsWindowHovered()) {
		using namespace veekay::input;

		if (mouse::isButtonDown(mouse::Button::left)) {
			auto move_delta = mouse::cursorDelta();

			// TODO: Use mouse_delta to update camera rotation
			
			auto view = camera.view();

			// TODO: Calculate right, up and front from view matrix
			veekay::vec3 right = {1.0f, 0.0f, 0.0f};
			veekay::vec3 up = {0.0f, -1.0f, 0.0f};
			veekay::vec3 front = {0.0f, 0.0f, 1.0f};

			if (keyboard::isKeyDown(keyboard::Key::w))
				camera.position += front * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::s))
				camera.position -= front * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::d))
				camera.position += right * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::a))
				camera.position -= right * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::q))
				camera.position += up * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::z))
				camera.position -= up * 0.1f;
		}
	}

	float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);
	
	// Добавить время в uniform
	SceneUniforms scene_uniforms{
		.view_projection = camera.view_projection(aspect_ratio),
		.time = static_cast<float>(time),  // Передаем время для анимации цвета
	};

	std::vector<ModelUniforms> model_uniforms(models.size());
	for (size_t i = 0, n = models.size(); i < n; ++i) {
		const Model& model = models[i];
		ModelUniforms& uniforms = model_uniforms[i];

		uniforms.model = model.transform.matrix();
		// Обновляем цвет куба из переменной cube_color (которая изменяется через UI)
		if (i == 2) { // Индекс 2 - это куб (тор=0, плоскость=1, куб=2)
			uniforms.albedo_color = cube_color;
		} else {
			uniforms.albedo_color = model.albedo_color;
		}
	}

	// Исправление ошибки Vulkan о выравнивании
	*(SceneUniforms*)scene_uniforms_buffer->mapped_region = scene_uniforms;
	// NOTE: Copy model uniforms with proper alignment
	uint8_t* buffer_ptr = static_cast<uint8_t*>(model_uniforms_buffer->mapped_region);
	for (size_t i = 0; i < model_uniforms.size(); ++i) {
		std::memcpy(buffer_ptr + i * aligned_model_uniforms_size, 
		           &model_uniforms[i], sizeof(ModelUniforms));
	}
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
	vkResetCommandBuffer(cmd, 0);

	{ // NOTE: Start recording rendering commands
		VkCommandBufferBeginInfo info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		vkBeginCommandBuffer(cmd, &info);
	}

	{ // NOTE: Use current swapchain framebuffer and clear it
		VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
		VkClearValue clear_depth{.depthStencil = {1.0f, 0}};

		VkClearValue clear_values[] = {clear_color, clear_depth};

		VkRenderPassBeginInfo info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = veekay::app.vk_render_pass,
			.framebuffer = framebuffer,
			.renderArea = {
				.extent = {
					veekay::app.window_width,
					veekay::app.window_height
				},
			},
			.clearValueCount = 2,
			.pClearValues = clear_values,
		};

		vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	VkDeviceSize zero_offset = 0;

	VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
	VkBuffer current_index_buffer = VK_NULL_HANDLE;

	for (size_t i = 0, n = models.size(); i < n; ++i) {
		const Model& model = models[i];
		const Mesh& mesh = model.mesh;

		if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
			current_vertex_buffer = mesh.vertex_buffer->buffer;
			vkCmdBindVertexBuffers(cmd, 0, 1, &current_vertex_buffer, &zero_offset);
		}

		if (current_index_buffer != mesh.index_buffer->buffer) {
			current_index_buffer = mesh.index_buffer->buffer;
			vkCmdBindIndexBuffer(cmd, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
		}

		// Исправление ошибки Vulkan о выравнивании
		uint32_t offset = i * aligned_model_uniforms_size;

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
		                    0, 1, &descriptor_set, 1, &offset);

		vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

} // namespace

int main() {
	return veekay::run({
		.init = initialize,
		.shutdown = shutdown,
		.update = update,
		.render = render,
	});
}
