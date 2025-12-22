#version 450

layout (location = 0) in vec3 v_position;
layout (location = 1) in vec3 v_normal;
layout (location = 2) in vec2 v_uv;

layout (location = 0) out vec3 f_position;
layout (location = 1) out vec3 f_normal;
layout (location = 2) out vec2 f_uv;
layout (location = 3) out vec4 f_light_space_pos;

layout (binding = 0, std140) uniform SceneUniforms {
  mat4 view_projection;
  mat4 shadow_projection;  // Матрица проекции теней для направленного света (должна быть сразу после view_projection!)
  vec3 view_position;  // Позиция камеры для расчета view direction
  float _pad0;
  
  vec3 ambient_light_intensity;  // Рассеянное освещение
  float _pad1;
  
  vec3 sun_light_direction;  // Направление направленного света
  float _pad2;
  
  vec3 sun_light_color;  // Цвет направленного света
  float _pad3;
  
  uint point_light_count;  // Количество активных точечных источников
  float time;  // Время для анимации цвета
  
  uint active_shadow_sources;  // Битовая маска активных источников теней
  float shadow_bias;  // Смещение для расчета теней
  float _pad4;
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
  
  // Для нормалей нужно использовать обратную транспонированную матрицу
  // чтобы они оставались перпендикулярными к поверхности после масштабирования
  mat3 normal_matrix = mat3(transpose(inverse(material.model)));
  vec3 normal = normal_matrix * v_normal;

  gl_Position = scene.view_projection * position;

  f_position = position.xyz;
  f_normal = normalize(normal);  // Нормализуем нормаль в мировых координатах
  f_uv = v_uv;
  f_light_space_pos = scene.shadow_projection * position;
}
