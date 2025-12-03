#version 450
// ============================================================================
// Доп. задание: используется time из SceneUniforms для передачи в fragment shader
// ============================================================================

layout (location = 0) in vec3 v_position;
layout (location = 1) in vec3 v_normal;
layout (location = 2) in vec2 v_uv;

layout (location = 0) out vec3 f_position;
layout (location = 1) out vec3 f_normal;
layout (location = 2) out vec2 f_uv;

layout (binding = 0, std140) uniform SceneUniforms {
  mat4 view_projection;
  float time;  //  Время для анимации цвета
  uint point_light_count;  // Количество активных точечных источников
  vec3 camera_position;  // Позиция камеры для расчета view direction
  float _pad0;  // Выравнивание для std140
} scene;

layout (binding = 1, std140) uniform ModelUniforms {
  mat4 model;
  vec3 albedo_color;
  float _pad0;
  vec3 specular_color;
  float shininess;
} material;

void main() {
  vec4 position = material.model * vec4(v_position, 1.0f);
  
  mat3 normal_matrix = mat3(material.model);
  vec3 normal = normal_matrix * v_normal;

  gl_Position = scene.view_projection * position;

  f_position = position.xyz;
  f_normal = normalize(normal);  // Нормализуем нормаль в мировых координатах
  f_uv = v_uv;
}
