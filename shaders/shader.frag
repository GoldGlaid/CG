#version 450
// ============================================================================
// FRAGMENT SHADER с моделью Блинн-Фонга для точечных источников света
// ОСНОВНОЕ ЗАДАНИЕ: Анимация цвета через sin(time) для RGB компонент (сохранена)
// Доп. задание: используется time из SceneUniforms для расчета цвета
// ============================================================================

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;

layout (location = 0) out vec4 final_color;

layout (binding = 0, std140) uniform SceneUniforms {
  mat4 view_projection;
  float time;  // Время для анимации цвета
  uint point_light_count;  // Количество активных точечных источников
  vec3 camera_position;  // Позиция камеры для расчета view direction
  float _pad0;
} scene;

layout (binding = 1, std140) uniform ModelUniforms {
  mat4 model;
  vec3 albedo_color;
  float _pad0;
  vec3 specular_color;
  float shininess;
} material;

// Shader-storage буфер для точечных источников света
struct PointLight {
  vec3 position;
  float intensity;
  vec3 color;
  float _pad0;
};

layout(std430, binding = 2) readonly buffer PointLightsBuffer {
  PointLight lights[];
};

void main() {
  // ОТЛАДКА: Показываем только albedo цвет без освещения для проверки видимости объектов
  vec3 animated_albedo;
  bool is_torus = (material.albedo_color.r > 1.5 && material.albedo_color.g > 1.5 && material.albedo_color.b > 1.5);
  
  if (is_torus) {
    // Для тора: анимация цвета через sin(time)
    float pi = 3.14159265359;
    float r = sin(scene.time) * 0.5 + 0.5;
    float g = sin(scene.time + 2.0 * pi / 3.0) * 0.5 + 0.5;
    float b = sin(scene.time + 4.0 * pi / 3.0) * 0.5 + 0.5;
    animated_albedo = vec3(r, g, b);
  } else {
    animated_albedo = clamp(material.albedo_color, 0.0, 1.0);
  }

  // Материал
  vec3 material_albedo = animated_albedo;
  vec3 material_specular = material.specular_color;
  float material_shininess = material.shininess;

  // Направление к камере (view direction)
  vec3 view_dir = normalize(scene.camera_position - f_position);

  // Базовое ambient освещение (увеличено для видимости)
  vec3 ambient = material_albedo * 0.3;

  // Инициализируем итоговый цвет
  vec3 result = ambient;

  // Обрабатываем каждый точечный источник света
  if (scene.point_light_count > 0) {
    for (uint i = 0; i < scene.point_light_count && i < lights.length(); ++i) {
      PointLight light = lights[i];
      
      // Направление к источнику света
      vec3 light_dir = light.position - f_position;
      float distance = length(light_dir);
      light_dir = normalize(light_dir);
      
      // Закон обратных квадратов (attenuation)
      float attenuation = light.intensity / (distance * distance + 0.01);
      
      // Diffuse компонент
      float diff = max(dot(f_normal, light_dir), 0.0);
      vec3 diffuse = diff * material_albedo * light.color;
      
      // Specular компонент (Блинн-Фонг)
      vec3 half_dir = normalize(light_dir + view_dir);
      float spec = pow(max(dot(f_normal, half_dir), 0.0), material_shininess);
      vec3 specular = spec * material_specular * light.color;
      
      // Суммируем вклад источника света
      result += (diffuse + specular) * attenuation;
    }
  }

  final_color = vec4(result, 1.0f);
}
