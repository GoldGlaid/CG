#include <vulkan/vulkan_core.h>
#include <veekay/veekay.hpp>
#include "veekay/input.hpp"

#include <imgui.h>
#include <lodepng.h>
#include <cstdint>
#include <climits>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>

#define _USE_MATH_DEFINES
#include <math.h>

namespace {

constexpr uint32_t max_models = 1024;
constexpr uint32_t max_point_lights = 16;

struct Vertex {
	veekay::vec3 position; // Позиция точки на объекте
	veekay::vec3 normal;
	veekay::vec2 uv; // текстура
	// NOTE: You can add more attributes
};

struct PointLight {
	veekay::vec3 position;
	float intensity;  // Для закона обратных квадратов
	veekay::vec3 color;
	float _pad0;  // Выравнивание для std140
};

struct SceneUniforms {
	veekay::mat4 view_projection;
	veekay::vec3 view_position;  // Позиция камеры для расчета view direction
	float _pad0;  // Выравнивание для std140
	
	veekay::vec3 ambient_light_intensity;  // Рассеянное освещение
	float _pad1;  // Выравнивание для std140
	
	veekay::vec3 sun_light_direction;  // Направление направленного света
	float _pad2;  // Выравнивание для std140
	
	veekay::vec3 sun_light_color;  // Цвет направленного света
	float _pad3;  // Выравнивание для std140
	
	uint32_t point_light_count;  // Количество активных точечных источников
	float time;  // Для анимации цвета через sin(time)
};

struct ModelUniforms {
	veekay::mat4 model;
	veekay::vec3 albedo_color;
	float _pad0;
	veekay::vec3 specular_color;
	float shininess;
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
	veekay::vec3 specular_color;
	veekay::vec3 shininess;

};

struct Light {
  veekay::vec3 color;
  veekay::vec3 direction;

  veekay::vec3 ambient;
  veekay::vec3 diffuse;
  veekay::vec3 specular;
};

struct Camera {
	constexpr static float default_fov = 60.0f;
	constexpr static float default_near_plane = 0.01f;
	constexpr static float default_far_plane = 100.0f;

	veekay::vec3 position = {};
	veekay::vec3 rotation = {};
	veekay::vec3 target = {0.0f, 0.0f, 0.0f};  // Куда смотрит камера
	veekay::vec3 up = {0.0f, 1.0f, 0.0f};  // Вектор вверх

	float fov = default_fov;
	float near_plane = default_near_plane;
	float far_plane = default_far_plane;

	// NOTE: View matrix of camera (inverse of a transform)
	veekay::mat4 view() const;

	// NOTE: Orthographic projection matrix
	veekay::mat4 orthographic(float aspect_ratio) const;

	// NOTE: View and projection composition
	veekay::mat4 view_projection(float aspect_ratio) const;

	// NOTE: Look-At matrix calculation
	veekay::mat4 look_at() const;
};

// NOTE: Scene objects
inline namespace {
	//===============================================
	Camera camera{
		.position = {0.0f, 0.0f, 15.0f},
		.target = {0.0f, -5.0f, 0.0f},  // Центр сцены (где находятся объекты)
		.up = {0.0f, 1.0f, 0.0f},
		.fov = 60.0f,
		.near_plane = 0.1f,
		.far_plane = 1000.0f  // Увеличено для большей дальности прорисовки
	};


	// Орбитальные параметры камеры
	float orbit_radius = 0.0f;      // Радиус орбиты (будет вычислен из начальной позиции)
	float orbit_yaw = 0.0f;         // Горизонтальный угол (в радианах)
	float orbit_pitch = 0.0f;       // Вертикальный угол (в радианах)
	//===============================================

	std::vector<Model> models;
	std::vector<PointLight> point_lights;  // Точечные источники света

	// UI variables for additional features
	bool is_animation_paused = false;
	bool is_rotation_reversed = false;
	float rotation_speed = 1.0f;      // UI: скорость вращения
	float animation_time = 0.0f;       // Накопленное время анимации
	
	// Lighting parameters
	veekay::vec3 ambient_light_intensity = {0.2f, 0.2f, 0.2f};
	veekay::vec3 sun_light_direction = {0.0f, -1.0f, -0.5f};  // Будет нормализовано при использовании
	veekay::vec3 sun_light_color = {1.0f, 1.0f, 1.0f};
}

// NOTE: Vulkan objects
inline namespace {
	VkShaderModule vertex_shader_module;
	VkShaderModule fragment_shader_module;
	VkShaderModule light_shader_module;

	VkDescriptorPool descriptor_pool;
	VkDescriptorSetLayout descriptor_set_layout;
	VkDescriptorSet descriptor_set;

	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;

	veekay::graphics::Buffer* scene_uniforms_buffer;
	veekay::graphics::Buffer* model_uniforms_buffer;

	Mesh plane_mesh;
	Mesh cube_mesh;

	// Для настройки цвета кубика в прямом эфире
	veekay::vec3 cube_color = {0.0f, 1.0f, 0.735f};
	veekay::vec3 torus_color = {0.0f, 1.0f, 0.35f};

	veekay::graphics::Texture* missing_texture;
	VkSampler missing_texture_sampler;

	veekay::graphics::Texture* texture;
	VkSampler texture_sampler;

	// Выравнивание размеров для устранения ошибки Vulkan
	uint32_t min_uniform_buffer_offset_alignment;
	uint32_t aligned_model_uniforms_size;

	Mesh torus_mesh;  // Тор
	
	veekay::graphics::Buffer* point_lights_buffer;  // Shader-storage буфер для точечных источников
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
			
			// Нормаль: вычисляем как градиент поверхности тора
			// Частные производные по u и v для тора
			// Для тора нормаль вычисляется как cross product тангенциальных векторов
			veekay::vec3 tangent_u{
				-(majorRadius + minorRadius * cos_v) * sin_u,
				(majorRadius + minorRadius * cos_v) * cos_u,
				0.0f
			};
			
			veekay::vec3 tangent_v{
				-minorRadius * sin_v * cos_u,
				-minorRadius * sin_v * sin_u,
				minorRadius * cos_v
			};
			
			// Нормаль = cross(tangent_u, tangent_v)
			veekay::vec3 normal{
				tangent_u.y * tangent_v.z - tangent_u.z * tangent_v.y,
				tangent_u.z * tangent_v.x - tangent_u.x * tangent_v.z,
				tangent_u.x * tangent_v.y - tangent_u.y * tangent_v.x
			};
			
			// Нормализуем нормаль
			float len = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
			if (len > 0.0001f) {
				normal.x /= len;
				normal.y /= len;
				normal.z /= len;
			}
			
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
			indices.push_back(current + 1);
			indices.push_back(next);
			
			// Второй треугольник
			indices.push_back(current + 1);
			indices.push_back(next + 1);
			indices.push_back(next);
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
	// Полная матрица преобразования (scaling + rotation + translation)
	
	auto s = veekay::mat4::scaling(scale);
	
	// Вращение вокруг осей X, Y, Z (в радианах)
	auto rx = veekay::mat4::rotation(veekay::vec3{1.0f, 0.0f, 0.0f}, rotation.x);
	auto ry = veekay::mat4::rotation(veekay::vec3{0.0f, 1.0f, 0.0f}, rotation.y);
	auto rz = veekay::mat4::rotation(veekay::vec3{0.0f, 0.0f, 1.0f}, rotation.z);
	
	auto r = ry * rx * rz;  // Порядок: сначала X, потом Y, потом Z
	
	auto t = veekay::mat4::translation(position);
	
	// Порядок применения: Scale -> Rotate -> Translate
	return t * r * s;
}


// [ r.x  u.x  -f.x  0 ]
// [ r.y  u.y  -f.y  0 ]
// [ r.z  u.z  -f.z  0 ]
// [  0    0     0   1 ]

veekay::mat4 Camera::look_at() const {

	// forward - направление от камеры к target
	veekay::vec3 f = veekay::vec3::normalized(target - position);
	// right - перпендикуляр к forward и world up
	veekay::vec3 r = veekay::vec3::normalized(veekay::vec3::cross(f, up));
	// up - пересчитанный up вектор (перпендикуляр к right и forward)
	veekay::vec3 u = veekay::vec3::cross(r, f);

	veekay::mat4 view_matrix{};

	// В column-major формате: result[j][i] = столбец j, строка i

	// Столбец 0: right вектор (X ось камеры)
	view_matrix[0][0] = r.x;
	view_matrix[0][1] = r.y;
	view_matrix[0][2] = r.z;
	view_matrix[0][3] = 0.0f;

	// Столбец 1: up вектор (Y ось камеры)
	view_matrix[1][0] = u.x;
	view_matrix[1][1] = u.y;
	view_matrix[1][2] = u.z;
	view_matrix[1][3] = 0.0f;

	// Столбец 2: -forward вектор т.к. смотрим назад (Z ось камеры)
	view_matrix[2][0] = -f.x;
	view_matrix[2][1] = -f.y;
	view_matrix[2][2] = -f.z;
	view_matrix[2][3] = 0.0f;

	// Столбец 3: -трансляция т.к. это view matrix
	view_matrix[3][0] = -veekay::vec3::dot(r, position);
	view_matrix[3][1] = -veekay::vec3::dot(u, position);
	view_matrix[3][2] = -veekay::vec3::dot(f, position);
	view_matrix[3][3] = 1.0f;

	return view_matrix;
}

veekay::mat4 Camera::view() const {
	return look_at();
}

veekay::mat4 Camera::view_projection(float aspect_ratio) const {
	auto proj = veekay::mat4::projection(fov, aspect_ratio, near_plane, far_plane);

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

// Обновляет позицию камеры на основе орбитальных параметров
void updateCameraPosition();

void initialize(VkCommandBuffer cmd) {
	VkDevice& device = veekay::app.vk_device; // Интерфейс для общения с видеокартой
	VkPhysicalDevice& physical_device = veekay::app.vk_physical_device; // физические параметры видюхи
	
	// Центр сцены
	camera.target = {0.0f, -5.0f, 0.0f};

	// Инициализация орбитальных параметров камеры из начальной позиции
	//===========================================
	veekay::vec3 offset = camera.position - camera.target;
	orbit_radius = veekay::vec3::length(offset);

	if (orbit_radius < 0.5f) {
		orbit_radius = 15.0f;
		orbit_yaw = 0.0f;
		orbit_pitch = 0.0f;
	} else {
		// Вычисляем углы из начальной позиции камеры
		// Нормализуем offset
		veekay::vec3 normalized_offset = veekay::vec3::normalized(offset);
		orbit_pitch = asinf(normalized_offset.y);
		orbit_yaw = atan2f(normalized_offset.x, normalized_offset.z);
	}
	//===========================================


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
					.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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
				{
					.binding = 2,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
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

	// Создаем shader-storage буфер для точечных источников света
	point_lights_buffer = new veekay::graphics::Buffer(
		max_point_lights * sizeof(PointLight),
		nullptr,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

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
			{
				.buffer = point_lights_buffer->buffer,
				.offset = 0,
				.range = max_point_lights * sizeof(PointLight),
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
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 2,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &buffer_infos[2],
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

	torus_mesh = generateTorusMesh(1.5f, 0.5f, 12, 16);

	// NOTE: Add models to scene
	models.clear();

	// Тор
	models.emplace_back(Model{
		.mesh = torus_mesh,
		.transform = Transform{
			.position = {0.0f, -9.0f, 0.0f},
			.scale = {1.0f, 1.0f, 1.0f},
			.rotation = {0.0f, 0.0f, 0.0f},
		},
		.albedo_color = torus_color,  // Специальное значение для тора (будет анимироваться в шейдере)
		.specular_color = veekay::vec3{1.0f, 1.0f, 1.0f},
		.shininess = veekay::vec3{32.0f, 32.0f, 32.0f}
	});

	// Куб
	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {0.0f, -2.0f, 0.0f},
		},
		.albedo_color = cube_color,
		.specular_color = veekay::vec3{0.8f, 0.8f, 0.8f},
		.shininess = veekay::vec3{64.0f, 64.0f, 64.0f}
	});

	// Белая плоскость (Сетка куба)
	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {0.0f, 1.0f, 0.0f},
			.scale = {50.0f, 1.0f, 15.0f},
		},
		.albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},  // Белая плоскость (не будет анимироваться)
		.specular_color = veekay::vec3{0.5f, 0.5f, 0.5f},
		.shininess = veekay::vec3{16.0f, 16.0f, 16.0f}
	});

	// МОДЕЛЬКИ
	for (float i = 0.0; i < 10.0; i += 0.05f) {
		// Тор
		float scale = i + 20;
		models.emplace_back(Model{
			.mesh = torus_mesh,
			.transform = Transform{
				.position = {-i, -9.0f + scale , scale},
				.scale = { i, i, i},
				.rotation = {-i, -9.0f + scale , scale},
			},
			.albedo_color = veekay::vec3{2.0f, 2.0f, 2.0f},  // Специальное значение для тора (будет анимироваться в шейдере)
			.specular_color = veekay::vec3{1.0f, 1.0f, 1.0f},
			.shininess = veekay::vec3{32.0f, 32.0f, 32.0f}
		});
	}

	// Инициализация 3 точечных источников света по умолчанию
	point_lights.clear();
	point_lights.emplace_back(PointLight{
		.position = {-5.0f, -5.0f, 0.0f},  // Красный слева
		.intensity = 50.0f,
		.color = {1.0f, 0.0f, 0.0f},
		._pad0 = 0.0f
	});
	point_lights.emplace_back(PointLight{
		.position = {5.0f, -5.0f, 0.0f},  // Зеленый справа
		.intensity = 50.0f,
		.color = {0.0f, 1.0f, 0.0f},
		._pad0 = 0.0f
	});
	point_lights.emplace_back(PointLight{
		.position = {0.0f, -7.0f, 0.0f},  // Синий сверху
		.intensity = 50.0f,
		.color = {0.0f, 0.0f, 1.0f},
		._pad0 = 0.0f
	});

	// Обновляем позицию камеры на основе вычисленных орбитальных параметров
	updateCameraPosition();

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

	delete point_lights_buffer;
	delete model_uniforms_buffer;
	delete scene_uniforms_buffer;

	vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyShaderModule(device, fragment_shader_module, nullptr);
	vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void updateCameraPosition() {
	// Ограничиваем pitch, чтобы камера не переворачивалась
	constexpr float min_pitch = -float(M_PI) / 2.0f + 0.1f;  // -90 градусов
	constexpr float max_pitch = float(M_PI) / 2.0f - 0.1f;   //  +90 градусов
	orbit_pitch = std::max(min_pitch, std::min(max_pitch, orbit_pitch));
	
	// Сферические координаты: x = r * cos(pitch) * cos(y	aw)
	//                        y = r * sin(pitch)
	//                        z = r * cos(pitch) * sin(yaw)

	float cos_pitch = cosf(orbit_pitch);
	camera.position.x = camera.target.x + orbit_radius * cosf(orbit_yaw) * cos_pitch;
	camera.position.y = camera.target.y + orbit_radius * sinf(orbit_pitch);
	camera.position.z = camera.target.z + orbit_radius * sinf(orbit_yaw) * cos_pitch;
}


void update(double time) {
	ImGui::Begin("Controls:");
	ImGui::ColorEdit3("Cube Color", &cube_color.x);
	ImGui::ColorEdit3("Torus Color", &torus_color.x);
	
	// Slider для FOV камеры

	if (ImGui::SliderFloat("FOV", &camera.fov, 30.0f, 120.0f)) {
	}
	
	// Slider для скорости вращения
	if (ImGui::SliderFloat("Rotation Speed", &rotation_speed, 0.0f, 100.0f)) {
	}
	ImGui::SameLine();

	// Управление анимацией
	if (ImGui::Checkbox("Pause Animation", &is_animation_paused)) {
		// Пауза/возобновление анимации
	}
	
	if (ImGui::Checkbox("Reverse Rotation", &is_rotation_reversed)) {
		// Реверс направления вращения
	}
	
	ImGui::End();

	// UI для управления точечными источниками света
	ImGui::Begin("Point Lights");
	ImGui::Text("Active lights: %zu", point_lights.size());
	
	for (size_t i = 0; i < point_lights.size(); ++i) {
		ImGui::PushID(static_cast<int>(i));
		ImGui::Text("Light %zu", i + 1);
		
		ImGui::SliderFloat3("Position", &point_lights[i].position.x, -10.0f, 10.0f);
		ImGui::ColorEdit3("Color", &point_lights[i].color.x);
		ImGui::SliderFloat("Intensity", &point_lights[i].intensity, 0.0f, 100.0f);
		
		if (ImGui::Button("Remove") && point_lights.size() > 1) {
			point_lights.erase(point_lights.begin() + i);
			ImGui::PopID();
			break;
		}
		
		ImGui::Separator();
		ImGui::PopID();
	}
	
	if (ImGui::Button("Add Light") && point_lights.size() < max_point_lights) {
		point_lights.emplace_back(PointLight{
			.position = {0.0f, 2.0f, 0.0f},
			.intensity = 50.0f,
			.color = {1.0f, 1.0f, 1.0f},
			._pad0 = 0.0f
		});
	}
	
	ImGui::End();

	// UI для управления рассеянным и направленным освещением
	ImGui::Begin("Lighting");
	ImGui::Text("Ambient Light");
	ImGui::ColorEdit3("Ambient Intensity", &ambient_light_intensity.x);
	
	ImGui::Separator();
	ImGui::Text("Directional Light (Sun)");
	ImGui::SliderFloat3("Direction", &sun_light_direction.x, -40.0f, 40.0f);
	ImGui::ColorEdit3("Sun Color", &sun_light_color.x);
	
	// Кнопка для сброса направления к значению по умолчанию
	if (ImGui::Button("Reset Direction")) {
		sun_light_direction = {0.0f, -1.0f, -0.5f};
	}
	
	ImGui::End();

	// Анимация вращения
	// Обновляем накопленное время анимации (если не на паузе)
	if (!is_animation_paused) {
		animation_time = time * rotation_speed * (is_rotation_reversed ? -1.0f : 1.0f);
	}

	//АНИМАЦИЯ
	if (!models.empty()) {
		//тор
		models[0].transform.rotation.y = animation_time;
		//куб
		models[1].transform.rotation.y = animation_time * -1.0f;
		for (int n = 3; n < models.size(); ++n) {
			models[n].transform.rotation.y = animation_time;
		}

	}

	if (!ImGui::IsWindowHovered()) {
		using namespace veekay::input;

		constexpr float rotation_speed = 0.02f;  // Скорость вращения камеры
		constexpr float zoom_speed = 0.5f;        // Скорость приближения/отдаления

		// W/S - изменение pitch (вертикальный угол)
		if (keyboard::isKeyDown(keyboard::Key::w))
			orbit_pitch += rotation_speed;

		if (keyboard::isKeyDown(keyboard::Key::s))
			orbit_pitch -= rotation_speed;

		// A/D - изменение yaw (горизонтальный угол)
		if (keyboard::isKeyDown(keyboard::Key::a))
			orbit_yaw -= rotation_speed;

		if (keyboard::isKeyDown(keyboard::Key::d))
			orbit_yaw += rotation_speed;

		// Q/Z - изменение радиуса (приближение/отдаление)
		if (keyboard::isKeyDown(keyboard::Key::q)) {
			orbit_radius -= zoom_speed;
			if (orbit_radius < 1.0f) orbit_radius = 1.0f;  // Минимальный радиус
		}

		if (keyboard::isKeyDown(keyboard::Key::z)) {
			orbit_radius += zoom_speed;
			if (orbit_radius > 500.0f) orbit_radius = 500.0f;  // Максимальный радиус увеличен
		}
	}

	// Камера обновляется каждый кадр
	updateCameraPosition();

	float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);
	
	// Обновление shader-storage буфера с данными точечных источников
	uint32_t active_light_count = static_cast<uint32_t>(point_lights.size());
	if (active_light_count > max_point_lights) {
		active_light_count = max_point_lights;
	}
	
	// Копирование данных источников в буфер
	PointLight* lights_buffer = static_cast<PointLight*>(point_lights_buffer->mapped_region);
	for (size_t i = 0; i < active_light_count; ++i) {
		lights_buffer[i] = point_lights[i];
	}
	
	// Добавление времени в uniform
	SceneUniforms scene_uniforms{
		.view_projection = camera.view_projection(aspect_ratio),
		.view_position = camera.position,
		._pad0 = 0.0f,
		.ambient_light_intensity = ambient_light_intensity,
		._pad1 = 0.0f,
		.sun_light_direction = veekay::vec3::normalized(sun_light_direction),
		._pad2 = 0.0f,
		.sun_light_color = sun_light_color,
		._pad3 = 0.0f,
		.point_light_count = active_light_count,
		.time = static_cast<float>(time)  // Передача времени для анимации цвета
	};

	std::vector<ModelUniforms> model_uniforms(models.size());
	for (size_t i = 0, n = models.size(); i < n; ++i) {
		const Model& model = models[i];
		ModelUniforms& uniforms = model_uniforms[i];

		uniforms.model = model.transform.matrix();
		// Обновление цвета куба
		if (i == 1) {
			uniforms.albedo_color = cube_color;
		} else if (i == 0) {
			uniforms.albedo_color = torus_color;
		} else {
			uniforms.albedo_color = model.albedo_color;
		}
		
		// Передача материалов
		uniforms.specular_color = model.specular_color;
		uniforms.shininess = model.shininess.x;  // Используем x компонент как shininess
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
     Light light;

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

        // veekay::vec3 lightDir = normalize(-light.direction);
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
