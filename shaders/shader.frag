#version 450
layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;

layout (location = 0) out vec4 final_color;

layout (binding = 0, std140) uniform SceneUniforms {
  mat4 view_projection;
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
} scene;

layout (binding = 1, std140) uniform ModelUniforms {
  mat4 model;
  vec3 albedo_color;
  float disable_uv_distortion;  // 0.0 = применять искажение UV, 1.0 = не применять
  vec3 specular_color;
  float enable_color_tinting;  // 1.0 = применять цвет к текстуре, 0.0 = не применять
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

// Текстуры материалов
layout (binding = 3) uniform sampler2D albedo_texture;
layout (binding = 4) uniform sampler2D specular_texture;
layout (binding = 5) uniform sampler2D emissive_texture;

void main() {
  // Нетривиальное сэмплирование: модифицируем UV координаты
  // Добавляем волновое искажение на основе позиции и времени (только если не отключено)
  vec2 modified_uv = f_uv;
  
  if (material.disable_uv_distortion < 0.5) {
    // Волновое искажение: синусоидальная деформация (слабое для видимости текстуры)
    float wave_strength = 0.02;  // Сила искажения (уменьшено)
    float wave_frequency = 2.0;  // Частота волн
    modified_uv.x += sin(f_uv.y * wave_frequency + scene.time * 1.0) * wave_strength;
    modified_uv.y += cos(f_uv.x * wave_frequency + scene.time * 1.0) * wave_strength;
    
    // Дополнительно: слабое вращение UV координат на основе позиции
    float angle = length(f_position.xy) * 0.05 + scene.time * 0.2;
    float cos_a = cos(angle);
    float sin_a = sin(angle);
    vec2 centered_uv = modified_uv - vec2(0.5, 0.5);
    modified_uv = vec2(
      centered_uv.x * cos_a - centered_uv.y * sin_a,
      centered_uv.x * sin_a + centered_uv.y * cos_a
    ) + vec2(0.5, 0.5);
    
    // Ограничиваем UV координаты, чтобы не выходить за пределы текстуры
    modified_uv = clamp(modified_uv, vec2(0.0), vec2(1.0));
  }

  // Сэмплируем текстуры
  vec4 albedo_tex = texture(albedo_texture, modified_uv);
  vec4 specular_tex = texture(specular_texture, modified_uv);
  vec4 emissive_tex = texture(emissive_texture, modified_uv);

  // Используем цвет материала для тонирования (только если включено)
  vec3 material_albedo;
  if (material.enable_color_tinting > 0.5) {
    material_albedo = albedo_tex.rgb * material.albedo_color;  // С тонированием
  } else {
    material_albedo = albedo_tex.rgb;  // Без тонирования
  }

  // Используем specular текстуру для модуляции specular цвета
  vec3 material_specular = material.specular_color * specular_tex.rgb;
  float material_shininess = material.shininess;

  // Нормализуем нормаль
  vec3 normal = normalize(f_normal);
  
  // Направление к камере (view direction)
  vec3 view_dir = normalize(scene.view_position - f_position);

  // Рассеянное освещение из uniforms
  vec3 ambient = scene.ambient_light_intensity;

  // Направленное освещение (солнце) по модели Блинн-Фонга
  vec3 half_vector = normalize(view_dir - scene.sun_light_direction);
  float sun_shade = max(0.0, -dot(scene.sun_light_direction, normal));
  vec3 sun_diffuse = material_albedo;  // Без умножения на sun_light_color (применяется в итоговой формуле)
  vec3 sun_specular = material_specular * pow(max(0.0, dot(normal, half_vector)), material_shininess);
  vec3 sun_light_intensity = sun_shade * scene.sun_light_color * (sun_diffuse + sun_specular);

  // Итоговый цвет
  vec3 result = ambient + sun_light_intensity;

  // Каждый точечный источник света
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
      float diff = max(dot(normal, light_dir), 0.0);
      vec3 diffuse = diff * material_albedo * light.color;
      
      // Specular компонент (Блинн-Фонг)
      vec3 half_dir = normalize(light_dir + view_dir);
      float spec = pow(max(dot(normal, half_dir), 0.0), material_shininess);
      vec3 specular = spec * material_specular * light.color;
      
      // Суммируем вклад источника света
      result += (diffuse + specular) * attenuation;
    }
  }

  // Добавляем emissive вклад (не зависит от освещения)
  vec3 emissive = emissive_tex.rgb;
  result += emissive;


  if (material.disable_uv_distortion >= 0.5 && material.enable_color_tinting < 0.5 && 
      material.shininess < 0.5 && 
      material.specular_color.r < 0.1 && material.specular_color.g < 0.1 && material.specular_color.b < 0.1) {
    result = material_albedo;
  }

  final_color = vec4(result, 1.0f);
}
