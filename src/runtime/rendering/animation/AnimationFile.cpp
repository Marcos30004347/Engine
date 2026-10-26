#include "rendering/animation/AnimationFile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "algorithm/BinaryIO.hpp"
#include "tiny_gltf.h"

namespace rendering
{
namespace animation
{

namespace
{

constexpr uint32_t ANIMATION_FILE_MAGIC = 0x414E4D46; // ANMF
constexpr uint32_t ANIMATION_FILE_VERSION = 1u;

constexpr uint32_t TRACK_TRANSLATION_BIT = (1u << 0u);
constexpr uint32_t TRACK_ROTATION_BIT = (1u << 1u);
constexpr uint32_t TRACK_SCALE_BIT = (1u << 2u);

bool writeString(FILE *file, const std::string &value)
{
  virtualgeometry::write_u32(file, static_cast<uint32_t>(value.size()));
  return value.empty() || std::fwrite(value.data(), 1u, value.size(), file) == value.size();
}

bool readString(FILE *file, std::string &value)
{
  const uint32_t length = virtualgeometry::read_u32(file);
  value.resize(length);
  return length == 0u || std::fread(value.data(), 1u, length, file) == length;
}

bool writeVec3Track(FILE *file, const AnimationFile::Vec3Track &track)
{
  if (track.times.size() != track.values.size())
    return false;

  virtualgeometry::write_u32(file, static_cast<uint32_t>(track.interpolation));
  virtualgeometry::write_u32(file, static_cast<uint32_t>(track.times.size()));
  for (size_t index = 0; index < track.times.size(); ++index)
  {
    virtualgeometry::write_f32(file, track.times[index]);
    virtualgeometry::write_f32(file, track.values[index].x());
    virtualgeometry::write_f32(file, track.values[index].y());
    virtualgeometry::write_f32(file, track.values[index].z());
  }
  return true;
}

bool readVec3Track(FILE *file, AnimationFile::Vec3Track &track)
{
  track.interpolation = static_cast<AnimationFile::Interpolation>(virtualgeometry::read_u32(file));
  const uint32_t keyCount = virtualgeometry::read_u32(file);
  track.times.resize(keyCount);
  track.values.resize(keyCount);
  for (uint32_t index = 0u; index < keyCount; ++index)
  {
    track.times[index] = virtualgeometry::read_f32(file);
    track.values[index].x() = virtualgeometry::read_f32(file);
    track.values[index].y() = virtualgeometry::read_f32(file);
    track.values[index].z() = virtualgeometry::read_f32(file);
  }
  return true;
}

bool writeQuatTrack(FILE *file, const AnimationFile::QuatTrack &track)
{
  if (track.times.size() != track.values.size())
    return false;

  virtualgeometry::write_u32(file, static_cast<uint32_t>(track.interpolation));
  virtualgeometry::write_u32(file, static_cast<uint32_t>(track.times.size()));
  for (size_t index = 0; index < track.times.size(); ++index)
  {
    virtualgeometry::write_f32(file, track.times[index]);
    virtualgeometry::write_f32(file, track.values[index].x);
    virtualgeometry::write_f32(file, track.values[index].y);
    virtualgeometry::write_f32(file, track.values[index].z);
    virtualgeometry::write_f32(file, track.values[index].w);
  }
  return true;
}

bool readQuatTrack(FILE *file, AnimationFile::QuatTrack &track)
{
  track.interpolation = static_cast<AnimationFile::Interpolation>(virtualgeometry::read_u32(file));
  const uint32_t keyCount = virtualgeometry::read_u32(file);
  track.times.resize(keyCount);
  track.values.resize(keyCount);
  for (uint32_t index = 0u; index < keyCount; ++index)
  {
    track.times[index] = virtualgeometry::read_f32(file);
    track.values[index].x = virtualgeometry::read_f32(file);
    track.values[index].y = virtualgeometry::read_f32(file);
    track.values[index].z = virtualgeometry::read_f32(file);
    track.values[index].w = virtualgeometry::read_f32(file);
  }
  return true;
}

math::Mat4f buildNodeLocalTransform(const tinygltf::Node &node)
{
  if (node.matrix.size() == 16u)
  {
    math::Mat4f transform{};
    for (size_t element = 0; element < 16u; ++element)
      transform.data[element] = static_cast<float>(node.matrix[element]);
    return transform;
  }

  math::Vec3f translation(0.0f, 0.0f, 0.0f);
  if (node.translation.size() == 3u)
    translation = math::Vec3f(static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2]));

  math::Quatf rotation = math::Quatf::identity();
  if (node.rotation.size() == 4u)
    rotation = math::Quatf(static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3]));

  math::Vec3f scale(1.0f, 1.0f, 1.0f);
  if (node.scale.size() == 3u)
    scale = math::Vec3f(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]));

  return math::Mat4f::translate(translation) * math::Mat4f::fromQuaternion(rotation) * math::Mat4f::scale(scale);
}

double readNumericComponent(const unsigned char *data, int componentType, bool normalized)
{
  switch (componentType)
  {
  case TINYGLTF_COMPONENT_TYPE_BYTE:
  {
    int8_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return std::max(static_cast<double>(value) / 127.0, -1.0);
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
  {
    uint8_t value = 0u;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return static_cast<double>(value) / 255.0;
  }
  case TINYGLTF_COMPONENT_TYPE_SHORT:
  {
    int16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return std::max(static_cast<double>(value) / 32767.0, -1.0);
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
  {
    uint16_t value = 0u;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return static_cast<double>(value) / 65535.0;
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
  {
    uint32_t value = 0u;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<double>(value);
  }
  case TINYGLTF_COMPONENT_TYPE_FLOAT:
  {
    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<double>(value);
  }
  default:
    return 0.0;
  }
}

bool readAccessorFloatComponents(const tinygltf::Model &model, int accessorIndex, int expectedComponents, std::vector<float> &out)
{
  if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
    return false;

  const tinygltf::Accessor &accessor = model.accessors[static_cast<size_t>(accessorIndex)];
  if (tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type)) != expectedComponents)
    return false;
  if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    return false;

  const tinygltf::BufferView &bufferView = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
  if (bufferView.buffer < 0 || bufferView.buffer >= static_cast<int>(model.buffers.size()))
    return false;

  const tinygltf::Buffer &buffer = model.buffers[static_cast<size_t>(bufferView.buffer)];
  const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
  const int stride = accessor.ByteStride(bufferView);
  if (componentSize <= 0 || stride <= 0)
    return false;

  const size_t accessorOffset = bufferView.byteOffset + accessor.byteOffset;
  if (accessorOffset >= buffer.data.size())
    return false;

  out.resize(accessor.count * static_cast<size_t>(expectedComponents));
  for (size_t element = 0; element < accessor.count; ++element)
  {
    const unsigned char *elementData = buffer.data.data() + accessorOffset + element * static_cast<size_t>(stride);
    for (int component = 0; component < expectedComponents; ++component)
    {
      const unsigned char *componentData = elementData + component * static_cast<size_t>(componentSize);
      out[element * static_cast<size_t>(expectedComponents) + static_cast<size_t>(component)] = static_cast<float>(readNumericComponent(componentData, accessor.componentType, accessor.normalized));
    }
  }

  return true;
}

AnimationFile::Interpolation parseInterpolation(const std::string &interpolation)
{
  if (interpolation == "STEP")
    return AnimationFile::Interpolation::Step;
  return AnimationFile::Interpolation::Linear;
}

AnimationFile::BoneAnimation &getOrCreateBoneAnimation(std::vector<AnimationFile::BoneAnimation> &boneAnimations, std::unordered_map<uint32_t, size_t> &boneIndexToChannel, uint32_t boneIndex)
{
  auto it = boneIndexToChannel.find(boneIndex);
  if (it != boneIndexToChannel.end())
    return boneAnimations[it->second];

  const size_t newIndex = boneAnimations.size();
  boneAnimations.push_back(AnimationFile::BoneAnimation{});
  boneAnimations.back().boneIndex = boneIndex;
  boneIndexToChannel[boneIndex] = newIndex;
  return boneAnimations.back();
}

bool loadModel(const std::string &path, tinygltf::Model &model)
{
  tinygltf::TinyGLTF loader;
  std::string warnings;
  std::string errors;

  const bool isBinary = path.size() >= 4u && path.substr(path.size() - 4u) == ".glb";
  const bool loaded = isBinary ? loader.LoadBinaryFromFile(&model, &errors, &warnings, path) : loader.LoadASCIIFromFile(&model, &errors, &warnings, path);
  if (!warnings.empty())
    std::printf("%s\n", warnings.c_str());
  if (!errors.empty())
    std::printf("%s\n", errors.c_str());
  return loaded;
}

bool loadTranslationTrack(const tinygltf::Model &model, const tinygltf::AnimationSampler &sampler, AnimationFile::Vec3Track &track)
{
  std::vector<float> values;
  if (!readAccessorFloatComponents(model, sampler.output, 3, values))
    return false;

  const size_t keyCount = track.times.size();
  if (sampler.interpolation == "CUBICSPLINE")
  {
    if (values.size() != keyCount * 9u)
      return false;
    track.interpolation = AnimationFile::Interpolation::Linear;
    track.values.resize(keyCount);
    for (size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
    {
      const size_t base = keyIndex * 9u + 3u;
      track.values[keyIndex] = math::Vec3f(values[base + 0u], values[base + 1u], values[base + 2u]);
    }
    return true;
  }

  if (values.size() != keyCount * 3u)
    return false;

  track.interpolation = parseInterpolation(sampler.interpolation);
  track.values.resize(keyCount);
  for (size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
    track.values[keyIndex] = math::Vec3f(values[keyIndex * 3u + 0u], values[keyIndex * 3u + 1u], values[keyIndex * 3u + 2u]);
  return true;
}

bool loadRotationTrack(const tinygltf::Model &model, const tinygltf::AnimationSampler &sampler, AnimationFile::QuatTrack &track)
{
  std::vector<float> values;
  if (!readAccessorFloatComponents(model, sampler.output, 4, values))
    return false;

  const size_t keyCount = track.times.size();
  if (sampler.interpolation == "CUBICSPLINE")
  {
    if (values.size() != keyCount * 12u)
      return false;
    track.interpolation = AnimationFile::Interpolation::Linear;
    track.values.resize(keyCount);
    for (size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
    {
      const size_t base = keyIndex * 12u + 4u;
      track.values[keyIndex] = math::Quatf(values[base + 0u], values[base + 1u], values[base + 2u], values[base + 3u]).normalize();
    }
    return true;
  }

  if (values.size() != keyCount * 4u)
    return false;

  track.interpolation = parseInterpolation(sampler.interpolation);
  track.values.resize(keyCount);
  for (size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
    track.values[keyIndex] = math::Quatf(values[keyIndex * 4u + 0u], values[keyIndex * 4u + 1u], values[keyIndex * 4u + 2u], values[keyIndex * 4u + 3u]).normalize();
  return true;
}

} // namespace

bool AnimationFile::save(const std::string &path) const
{
  FILE *file = std::fopen(path.c_str(), "wb");
  if (!file)
    return false;

  bool ok = true;
  virtualgeometry::write_u32(file, ANIMATION_FILE_MAGIC);
  virtualgeometry::write_u32(file, ANIMATION_FILE_VERSION);
  ok = ok && writeString(file, name);
  virtualgeometry::write_f32(file, durationSeconds);
  virtualgeometry::write_u32(file, static_cast<uint32_t>(boneAnimations.size()));

  for (const BoneAnimation &boneAnimation : boneAnimations)
  {
    uint32_t trackMask = 0u;
    if (boneAnimation.hasTranslation)
      trackMask |= TRACK_TRANSLATION_BIT;
    if (boneAnimation.hasRotation)
      trackMask |= TRACK_ROTATION_BIT;
    if (boneAnimation.hasScale)
      trackMask |= TRACK_SCALE_BIT;

    virtualgeometry::write_u32(file, boneAnimation.boneIndex);
    virtualgeometry::write_u32(file, trackMask);

    if ((trackMask & TRACK_TRANSLATION_BIT) != 0u)
      ok = ok && writeVec3Track(file, boneAnimation.translation);
    if ((trackMask & TRACK_ROTATION_BIT) != 0u)
      ok = ok && writeQuatTrack(file, boneAnimation.rotation);
    if ((trackMask & TRACK_SCALE_BIT) != 0u)
      ok = ok && writeVec3Track(file, boneAnimation.scale);

    if (!ok)
      break;
  }

  std::fclose(file);
  return ok;
}

bool AnimationFile::load(const std::string &path, AnimationFile &outAnimation)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;

  bool ok = true;
  ok = ok && virtualgeometry::read_u32(file) == ANIMATION_FILE_MAGIC;
  ok = ok && virtualgeometry::read_u32(file) == ANIMATION_FILE_VERSION;

  AnimationFile loaded;
  ok = ok && readString(file, loaded.name);
  loaded.durationSeconds = virtualgeometry::read_f32(file);
  const uint32_t boneAnimationCount = virtualgeometry::read_u32(file);
  loaded.boneAnimations.resize(boneAnimationCount);

  for (uint32_t animationIndex = 0u; ok && animationIndex < boneAnimationCount; ++animationIndex)
  {
    BoneAnimation &boneAnimation = loaded.boneAnimations[animationIndex];
    boneAnimation.boneIndex = virtualgeometry::read_u32(file);
    const uint32_t trackMask = virtualgeometry::read_u32(file);

    boneAnimation.hasTranslation = (trackMask & TRACK_TRANSLATION_BIT) != 0u;
    boneAnimation.hasRotation = (trackMask & TRACK_ROTATION_BIT) != 0u;
    boneAnimation.hasScale = (trackMask & TRACK_SCALE_BIT) != 0u;

    if (boneAnimation.hasTranslation)
      ok = ok && readVec3Track(file, boneAnimation.translation);
    if (boneAnimation.hasRotation)
      ok = ok && readQuatTrack(file, boneAnimation.rotation);
    if (boneAnimation.hasScale)
      ok = ok && readVec3Track(file, boneAnimation.scale);
  }

  std::fclose(file);
  if (!ok)
    return false;

  outAnimation = std::move(loaded);
  return true;
}

bool AnimationFile::createFromGLTF(const std::string &path, const Skeleton &skeleton, std::vector<AnimationFile> &outAnimations)
{
  tinygltf::Model model;
  if (!loadModel(path, model))
    return false;

  std::unordered_map<std::string, uint32_t> boneNameToIndex;
  boneNameToIndex.reserve(skeleton.getBoneCount());
  for (uint32_t boneIndex = 0u; boneIndex < skeleton.getBoneCount(); ++boneIndex)
    boneNameToIndex[skeleton.getBone(boneIndex).name] = boneIndex;

  std::unordered_map<int, uint32_t> nodeToBoneIndex;
  for (const tinygltf::Skin &skin : model.skins)
  {
    for (size_t jointIndex = 0u; jointIndex < skin.joints.size(); ++jointIndex)
    {
      const int nodeIndex = skin.joints[jointIndex];
      if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
        continue;

      const tinygltf::Node &jointNode = model.nodes[static_cast<size_t>(nodeIndex)];
      const std::string boneName = jointNode.name.empty() ? ("bone_" + std::to_string(jointIndex)) : jointNode.name;
      const auto boneIt = boneNameToIndex.find(boneName);
      if (boneIt != boneNameToIndex.end())
        nodeToBoneIndex[nodeIndex] = boneIt->second;
    }
  }

  for (size_t nodeIndex = 0u; nodeIndex < model.nodes.size(); ++nodeIndex)
  {
    const tinygltf::Node &node = model.nodes[nodeIndex];
    const auto boneIt = boneNameToIndex.find(node.name);
    if (boneIt != boneNameToIndex.end())
      nodeToBoneIndex[static_cast<int>(nodeIndex)] = boneIt->second;
  }

  std::vector<AnimationFile> extractedAnimations;
  extractedAnimations.reserve(model.animations.size());

  for (size_t animationIndex = 0u; animationIndex < model.animations.size(); ++animationIndex)
  {
    const tinygltf::Animation &sourceAnimation = model.animations[animationIndex];

    AnimationFile animation;
    animation.name = sourceAnimation.name.empty() ? ("animation_" + std::to_string(animationIndex)) : sourceAnimation.name;

    std::unordered_map<uint32_t, size_t> boneIndexToChannel;
    for (const tinygltf::AnimationChannel &channel : sourceAnimation.channels)
    {
      if (channel.sampler < 0 || channel.sampler >= static_cast<int>(sourceAnimation.samplers.size()))
        continue;
      if (channel.target_node < 0)
        continue;

      const auto boneIt = nodeToBoneIndex.find(channel.target_node);
      if (boneIt == nodeToBoneIndex.end())
        continue;

      const tinygltf::AnimationSampler &sampler = sourceAnimation.samplers[static_cast<size_t>(channel.sampler)];
      std::vector<float> keyframeTimes;
      if (!readAccessorFloatComponents(model, sampler.input, 1, keyframeTimes) || keyframeTimes.empty())
        continue;

      animation.durationSeconds = std::max(animation.durationSeconds, keyframeTimes.back());
      BoneAnimation &boneAnimation = getOrCreateBoneAnimation(animation.boneAnimations, boneIndexToChannel, boneIt->second);

      if (channel.target_path == "translation")
      {
        boneAnimation.hasTranslation = true;
        boneAnimation.translation.times = std::move(keyframeTimes);
        if (!loadTranslationTrack(model, sampler, boneAnimation.translation))
          return false;
      }
      else if (channel.target_path == "rotation")
      {
        boneAnimation.hasRotation = true;
        boneAnimation.rotation.times = std::move(keyframeTimes);
        if (!loadRotationTrack(model, sampler, boneAnimation.rotation))
          return false;
      }
      else if (channel.target_path == "scale")
      {
        boneAnimation.hasScale = true;
        boneAnimation.scale.times = std::move(keyframeTimes);
        if (!loadTranslationTrack(model, sampler, boneAnimation.scale))
          return false;
      }
    }

    if (!animation.boneAnimations.empty())
      extractedAnimations.push_back(std::move(animation));
  }

  outAnimations = std::move(extractedAnimations);
  return true;
}

} // namespace animation
} // namespace rendering
