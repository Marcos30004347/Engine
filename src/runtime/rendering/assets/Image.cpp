#include "rendering/assets/Image.hpp"

#include <stdexcept>

#include "stb_image.h"

namespace rendering
{
namespace assets
{

Image Image::load(const std::string &path, bool flipVertically)
{
  stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc *data = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
  if (data == nullptr)
    throw std::runtime_error("Failed to load image: " + path);

  Image image;
  image.width_ = static_cast<uint32_t>(width);
  image.height_ = static_cast<uint32_t>(height);
  image.channelCount_ = 4u;
  image.format_ = PixelFormat::RGBA8;
  image.pixels_.assign(data, data + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4u));
  stbi_image_free(data);
  return image;
}

} // namespace assets
} // namespace rendering
