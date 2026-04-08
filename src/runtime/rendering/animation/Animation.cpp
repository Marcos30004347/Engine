#include "rendering/animation/Animation.hpp"

#include <cstdio>

namespace rendering
{
namespace animation
{

namespace
{

constexpr uint32_t ANIMATION_MAGIC = 0x414E494D; // ANIM
constexpr uint32_t ANIMATION_VERSION = 1u;

template <typename T> bool writeValue(FILE *file, const T &value)
{
  return std::fwrite(&value, sizeof(T), 1u, file) == 1u;
}

template <typename T> bool readValue(FILE *file, T &value)
{
  return std::fread(&value, sizeof(T), 1u, file) == 1u;
}

bool writeString(FILE *file, const std::string &value)
{
  const uint32_t length = static_cast<uint32_t>(value.size());
  if (!writeValue(file, length))
    return false;
  return length == 0u || std::fwrite(value.data(), 1u, length, file) == length;
}

bool readString(FILE *file, std::string &value)
{
  uint32_t length = 0u;
  if (!readValue(file, length))
    return false;
  value.resize(length);
  return length == 0u || std::fread(value.data(), 1u, length, file) == length;
}

} // namespace

bool Animation::save(const std::string &path) const
{
  FILE *file = std::fopen(path.c_str(), "wb");
  if (!file)
    return false;

  bool ok = true;
  ok = ok && writeValue(file, ANIMATION_MAGIC);
  ok = ok && writeValue(file, ANIMATION_VERSION);
  ok = ok && writeString(file, name);
  ok = ok && writeValue(file, durationSeconds);

  const uint32_t channelCount = static_cast<uint32_t>(channels.size());
  ok = ok && writeValue(file, channelCount);

  for (const Channel &channel : channels)
  {
    const uint32_t keyframeCount = static_cast<uint32_t>(channel.keyframeTimes.size());
    if (keyframeCount != channel.keyframeTransforms.size())
    {
      ok = false;
      break;
    }

    ok = ok && writeValue(file, channel.boneIndex);
    ok = ok && writeValue(file, keyframeCount);
    for (uint32_t i = 0u; ok && i < keyframeCount; ++i)
    {
      ok = ok && writeValue(file, channel.keyframeTimes[i]);
      ok = ok && writeValue(file, channel.keyframeTransforms[i]);
    }
  }

  std::fclose(file);
  return ok;
}

bool Animation::load(const std::string &path, Animation &outAnimation)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;

  uint32_t magic = 0u;
  uint32_t version = 0u;
  bool ok = readValue(file, magic) && readValue(file, version);
  ok = ok && magic == ANIMATION_MAGIC && version == ANIMATION_VERSION;

  Animation loaded;
  ok = ok && readString(file, loaded.name);
  ok = ok && readValue(file, loaded.durationSeconds);

  uint32_t channelCount = 0u;
  ok = ok && readValue(file, channelCount);
  loaded.channels.resize(channelCount);

  for (uint32_t channelIndex = 0u; ok && channelIndex < channelCount; ++channelIndex)
  {
    Channel &channel = loaded.channels[channelIndex];
    uint32_t keyframeCount = 0u;
    ok = ok && readValue(file, channel.boneIndex);
    ok = ok && readValue(file, keyframeCount);
    channel.keyframeTimes.resize(keyframeCount);
    channel.keyframeTransforms.resize(keyframeCount);
    for (uint32_t keyframeIndex = 0u; ok && keyframeIndex < keyframeCount; ++keyframeIndex)
    {
      ok = ok && readValue(file, channel.keyframeTimes[keyframeIndex]);
      ok = ok && readValue(file, channel.keyframeTransforms[keyframeIndex]);
    }
  }

  std::fclose(file);
  if (!ok)
    return false;

  outAnimation = std::move(loaded);
  return true;
}

} // namespace animation
} // namespace rendering
