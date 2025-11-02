#version 450
// ============================================================================
// FRAGMENT SHADER для тора с анимацией цвета
// ОСНОВНОЕ ЗАДАНИЕ: Анимация цвета через sin(time) для RGB компонент
// Доп. задание: используется time из SceneUniforms для расчета цвета
// ============================================================================

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;

layout (location = 0) out vec4 final_color;

layout (binding = 0, std140) uniform SceneUniforms {
  mat4 view_projection;
  float time;  // Время для анимации цвета
  float _pad0, _pad1, _pad2;
};

layout (binding = 1, std140) uniform ModelUniforms {
  mat4 model;
  vec3 albedo_color;
};

void main() {
  // Анимация цвета через sin(time) для RGB компонент
  // Применяем анимацию цвета только для тора (albedo_color > 1.5 - специальный маркер)
  // Для других моделей используем их исходный albedo_color

  vec3 final_albedo;

  // если albedo_color > 1.5, это тор
  if (albedo_color.r > 1.5 && albedo_color.g > 1.5 && albedo_color.b > 1.5) {
    // Для тора: анимация цвета через sin(time)
    // Используем sin(time), sin(time + 2π/3), sin(time + 4π/3) для R, G, B
    // Нормализуем значения sin (от -1 до 1) в диапазон [0, 1]
    float pi = 3.14159265359;
    float r = sin(time) * 0.5 + 0.5;
    float g = sin(time + 2.0 * pi / 3.0) * 0.5 + 0.5;
    float b = sin(time + 4.0 * pi / 3.0) * 0.5 + 0.5;

    final_albedo = vec3(r, g, b);

  } else {
    // Для других моделей: используем исходный albedo_color
    // Ограничиваем значения в диапазоне [0, 1] для корректного отображения
	
    final_albedo = clamp(albedo_color, 0.0, 1.0);
  }

  final_color = vec4(final_albedo, 1.0f);
}
