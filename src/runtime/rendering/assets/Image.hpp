#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rendering
{
namespace assets
{

class Image
{
public:
  enum class PixelFormat : uint32_t
  {
    RGBA8 = 0u,
  };

  static Image load(const std::string &path, bool flipVertically = false);

  bool isValid() const
  {
    return !pixels_.empty() && width_ > 0u && height_ > 0u;
  }

  uint32_t getWidth() const
  {
    return width_;
  }
  uint32_t getHeight() const
  {
    return height_;
  }
  uint32_t getChannelCount() const
  {
    return channelCount_;
  }
  PixelFormat getFormat() const
  {
    return format_;
  }
  const std::vector<uint8_t> &getPixels() const
  {
    return pixels_;
  }

private:
  uint32_t width_ = 0u;
  uint32_t height_ = 0u;
  uint32_t channelCount_ = 0u;
  PixelFormat format_ = PixelFormat::RGBA8;
  std::vector<uint8_t> pixels_;
};

} // namespace assets
} // namespace rendering
