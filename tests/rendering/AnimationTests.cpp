#include "rendering/animation/AnimationFile.hpp"
#include "rendering/animation/AnimationPlayer.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

bool nearlyEqual(float a, float b, float epsilon = 1e-4f)
{
  return std::fabs(a - b) <= epsilon;
}

bool matricesNearlyEqual(const math::Mat4f &a, const math::Mat4f &b, float epsilon = 1e-4f)
{
  for (size_t element = 0u; element < 16u; ++element)
  {
    if (!nearlyEqual(a.data[element], b.data[element], epsilon))
      return false;
  }
  return true;
}

rendering::animation::Skeleton buildFixtureSkeleton()
{
  rendering::animation::Skeleton skeleton;
  rendering::animation::Skeleton::Bone bone;
  bone.name = "joint0";
  bone.parentIndex = -1;
  bone.defaultLocalTransform = math::Mat4f::identity();
  bone.inverseBindMatrix = math::Mat4f::identity();
  skeleton.addBone(bone);
  skeleton.setMeshPartToBoneIndex({0u});
  return skeleton;
}

std::string writeAnimatedGLTFFixture()
{
  const std::string basePath = "/tmp/engine_animation_fixture";
  const std::string gltfPath = basePath + ".gltf";
  const std::string binPath = basePath + ".bin";

  const std::vector<float> positions = {
      0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
  };
  const std::vector<float> normals = {
      0.0f, 0.0f, 1.0f,
      0.0f, 0.0f, 1.0f,
      0.0f, 0.0f, 1.0f,
  };
  const std::vector<uint16_t> joints = {
      0u, 0u, 0u, 0u,
      0u, 0u, 0u, 0u,
      0u, 0u, 0u, 0u,
  };
  const std::vector<float> weights = {
      1.0f, 0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f, 0.0f,
  };
  const std::vector<uint16_t> indices = {0u, 1u, 2u};
  const std::vector<float> inverseBindMatrix = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  const std::vector<float> animationTimes = {0.0f, 1.0f};
  const std::vector<float> animationTranslations = {
      0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
  };

  std::ofstream binFile(binPath, std::ios::binary | std::ios::trunc);
  const uint32_t positionsOffset = 0u;
  binFile.write(reinterpret_cast<const char *>(positions.data()), static_cast<std::streamsize>(positions.size() * sizeof(float)));
  const uint32_t normalsOffset = static_cast<uint32_t>(binFile.tellp());
  binFile.write(reinterpret_cast<const char *>(normals.data()), static_cast<std::streamsize>(normals.size() * sizeof(float)));
  const uint32_t jointsOffset = static_cast<uint32_t>(binFile.tellp());
  binFile.write(reinterpret_cast<const char *>(joints.data()), static_cast<std::streamsize>(joints.size() * sizeof(uint16_t)));
  const uint32_t weightsOffset = static_cast<uint32_t>(binFile.tellp());
  binFile.write(reinterpret_cast<const char *>(weights.data()), static_cast<std::streamsize>(weights.size() * sizeof(float)));
  const uint32_t indicesOffset = static_cast<uint32_t>(binFile.tellp());
  binFile.write(reinterpret_cast<const char *>(indices.data()), static_cast<std::streamsize>(indices.size() * sizeof(uint16_t)));
  binFile.put('\0');
  binFile.put('\0');
  const uint32_t inverseBindOffset = static_cast<uint32_t>(binFile.tellp());
  binFile.write(reinterpret_cast<const char *>(inverseBindMatrix.data()), static_cast<std::streamsize>(inverseBindMatrix.size() * sizeof(float)));
  const uint32_t animationTimesOffset = static_cast<uint32_t>(binFile.tellp());
  binFile.write(reinterpret_cast<const char *>(animationTimes.data()), static_cast<std::streamsize>(animationTimes.size() * sizeof(float)));
  const uint32_t animationTranslationsOffset = static_cast<uint32_t>(binFile.tellp());
  binFile.write(reinterpret_cast<const char *>(animationTranslations.data()), static_cast<std::streamsize>(animationTranslations.size() * sizeof(float)));
  const uint32_t totalSize = static_cast<uint32_t>(binFile.tellp());
  binFile.close();

  std::ofstream gltfFile(gltfPath, std::ios::trunc);
  gltfFile << "{\n"
           << "  \"asset\": {\"version\": \"2.0\"},\n"
           << "  \"buffers\": [{\"byteLength\": " << totalSize << ", \"uri\": \"engine_animation_fixture.bin\"}],\n"
           << "  \"bufferViews\": [\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << positionsOffset << ", \"byteLength\": 36},\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << normalsOffset << ", \"byteLength\": 36},\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << jointsOffset << ", \"byteLength\": 24},\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << weightsOffset << ", \"byteLength\": 48},\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << indicesOffset << ", \"byteLength\": 6},\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << inverseBindOffset << ", \"byteLength\": 64},\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << animationTimesOffset << ", \"byteLength\": 8},\n"
           << "    {\"buffer\": 0, \"byteOffset\": " << animationTranslationsOffset << ", \"byteLength\": 24}\n"
           << "  ],\n"
           << "  \"accessors\": [\n"
           << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]},\n"
           << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\"},\n"
           << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 3, \"type\": \"VEC4\"},\n"
           << "    {\"bufferView\": 3, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC4\"},\n"
           << "    {\"bufferView\": 4, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\"},\n"
           << "    {\"bufferView\": 5, \"componentType\": 5126, \"count\": 1, \"type\": \"MAT4\"},\n"
           << "    {\"bufferView\": 6, \"componentType\": 5126, \"count\": 2, \"type\": \"SCALAR\", \"min\": [0], \"max\": [1]},\n"
           << "    {\"bufferView\": 7, \"componentType\": 5126, \"count\": 2, \"type\": \"VEC3\"}\n"
           << "  ],\n"
           << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"JOINTS_0\": 2, \"WEIGHTS_0\": 3}, \"indices\": 4}]}],\n"
           << "  \"nodes\": [\n"
           << "    {\"mesh\": 0, \"skin\": 0, \"name\": \"meshNode\"},\n"
           << "    {\"name\": \"joint0\"}\n"
           << "  ],\n"
           << "  \"skins\": [{\"inverseBindMatrices\": 5, \"joints\": [1], \"skeleton\": 1}],\n"
           << "  \"animations\": [{\n"
           << "    \"name\": \"move_joint\",\n"
           << "    \"samplers\": [{\"input\": 6, \"output\": 7, \"interpolation\": \"LINEAR\"}],\n"
           << "    \"channels\": [{\"sampler\": 0, \"target\": {\"node\": 1, \"path\": \"translation\"}}]\n"
           << "  }],\n"
           << "  \"scenes\": [{\"nodes\": [0, 1]}],\n"
           << "  \"scene\": 0\n"
           << "}\n";
  gltfFile.close();

  return gltfPath;
}

bool test_gltf_animation_roundtrip()
{
  std::cout << "Testing glTF animation extraction and roundtrip... ";

  const std::string gltfPath = writeAnimatedGLTFFixture();
  const rendering::animation::Skeleton skeleton = buildFixtureSkeleton();

  std::vector<rendering::animation::AnimationFile> animations;
  if (!rendering::animation::AnimationFile::createFromGLTF(gltfPath, skeleton, animations) || animations.empty())
  {
    std::cerr << "FAILED (no animations extracted)\n";
    return false;
  }

  const std::string tempPath = "/tmp/engine_animation_roundtrip.anim";
  if (!animations.front().save(tempPath))
  {
    std::cerr << "FAILED (save)\n";
    return false;
  }

  rendering::animation::AnimationFile loadedAnimation;
  if (!rendering::animation::AnimationFile::load(tempPath, loadedAnimation))
  {
    std::cerr << "FAILED (load)\n";
    std::remove(tempPath.c_str());
    return false;
  }

  std::remove(tempPath.c_str());

  if (loadedAnimation.boneAnimations.empty() || !nearlyEqual(loadedAnimation.durationSeconds, animations.front().durationSeconds))
  {
    std::cerr << "FAILED (content mismatch)\n";
    return false;
  }

  std::cout << "PASSED\n";
  return true;
}

bool test_animation_player_sampling()
{
  std::cout << "Testing animation player sampling... ";

  const std::string gltfPath = writeAnimatedGLTFFixture();
  const rendering::animation::Skeleton skeleton = buildFixtureSkeleton();

  std::vector<rendering::animation::AnimationFile> animations;
  if (!rendering::animation::AnimationFile::createFromGLTF(gltfPath, skeleton, animations) || animations.empty())
  {
    std::cerr << "FAILED (extract)\n";
    return false;
  }

  rendering::animation::AnimationPlayer player(skeleton);
  if (!player.addAnimation(animations.front()))
  {
    std::cerr << "FAILED (add animation)\n";
    return false;
  }

  const std::vector<math::Mat4f> restPalette = skeleton.buildRestPoseSkinningPalette();
  player.resetToBindPose();
  if (player.getBoneTransforms().size() != skeleton.getBoneCount())
  {
    std::cerr << "FAILED (bind pose size)\n";
    return false;
  }

  for (size_t boneIndex = 0u; boneIndex < restPalette.size(); ++boneIndex)
  {
    if (!matricesNearlyEqual(restPalette[boneIndex], player.getBoneTransforms()[boneIndex]))
    {
      std::cerr << "FAILED (bind pose mismatch)\n";
      return false;
    }
  }

  if (!player.applyAnimationFrame(animations.front(), animations.front().durationSeconds * 0.5f, true))
  {
    std::cerr << "FAILED (sample)\n";
    return false;
  }

  bool foundMotion = false;
  for (size_t boneIndex = 0u; boneIndex < restPalette.size(); ++boneIndex)
  {
    if (!matricesNearlyEqual(restPalette[boneIndex], player.getBoneTransforms()[boneIndex], 1e-3f))
    {
      foundMotion = true;
      break;
    }
  }

  if (!foundMotion)
  {
    std::cerr << "FAILED (pose did not change)\n";
    return false;
  }

  std::cout << "PASSED\n";
  return true;
}

} // namespace

int main()
{
  bool allPassed = true;
  allPassed = test_gltf_animation_roundtrip() && allPassed;
  allPassed = test_animation_player_sampling() && allPassed;

  if (!allPassed)
  {
    std::cerr << "Animation tests failed.\n";
    return 1;
  }

  std::cout << "All animation tests passed.\n";
  return 0;
}
