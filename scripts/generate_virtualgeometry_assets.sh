#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 OUTPUT_DIR" >&2
  exit 1
fi

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
OUTPUT_DIR="$1"

OBJ_IMPORTER="${BUILD_DIR}/src/cli/virtualgeometry/importer/obj/vgimport_obj"
GLTF_IMPORTER="${BUILD_DIR}/src/cli/virtualgeometry/importer/gltf/vgimport_gltf"

if [ ! -x "${OBJ_IMPORTER}" ]; then
  echo "Missing OBJ importer: ${OBJ_IMPORTER}" >&2
  echo "Build it with: cmake --build \"${BUILD_DIR}\" --target vgimport_obj" >&2
  exit 1
fi

if [ ! -x "${GLTF_IMPORTER}" ]; then
  echo "Missing glTF importer: ${GLTF_IMPORTER}" >&2
  echo "Build it with: cmake --build \"${BUILD_DIR}\" --target vgimport_gltf" >&2
  exit 1
fi

mkdir -p \
  "${OUTPUT_DIR}/meshes/obj" \
  "${OUTPUT_DIR}/meshes/gltf" \
  "${OUTPUT_DIR}/meshes/obj/armadillo.obj.virtualgeometry_materials" \
  "${OUTPUT_DIR}/meshes/obj/cow.obj.virtualgeometry_materials" \
  "${OUTPUT_DIR}/meshes/obj/horse.obj.virtualgeometry_materials" \
  "${OUTPUT_DIR}/meshes/obj/sponza.obj.virtualgeometry_materials" \
  "${OUTPUT_DIR}/meshes/obj/stanford-bunny.obj.virtualgeometry_materials" \
  "${OUTPUT_DIR}/meshes/obj/suzanne.obj.virtualgeometry_materials" \
  "${OUTPUT_DIR}/meshes/obj/teapot.obj.virtualgeometry_materials"

"${OBJ_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/obj/armadillo.obj" --output "${OUTPUT_DIR}/meshes/obj/armadillo.obj.virtualgeometry" --materials-dir "${OUTPUT_DIR}/meshes/obj/armadillo.obj.virtualgeometry_materials"
"${OBJ_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/obj/cow.obj" --output "${OUTPUT_DIR}/meshes/obj/cow.obj.virtualgeometry" --materials-dir "${OUTPUT_DIR}/meshes/obj/cow.obj.virtualgeometry_materials"
"${OBJ_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/obj/horse.obj" --output "${OUTPUT_DIR}/meshes/obj/horse.obj.virtualgeometry" --materials-dir "${OUTPUT_DIR}/meshes/obj/horse.obj.virtualgeometry_materials"
"${OBJ_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/obj/sponza.obj" --output "${OUTPUT_DIR}/meshes/obj/sponza.obj.virtualgeometry" --materials-dir "${OUTPUT_DIR}/meshes/obj/sponza.obj.virtualgeometry_materials"
"${OBJ_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/obj/stanford-bunny.obj" --output "${OUTPUT_DIR}/meshes/obj/stanford-bunny.obj.virtualgeometry" --materials-dir "${OUTPUT_DIR}/meshes/obj/stanford-bunny.obj.virtualgeometry_materials"
"${OBJ_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/obj/suzanne.obj" --output "${OUTPUT_DIR}/meshes/obj/suzanne.obj.virtualgeometry" --materials-dir "${OUTPUT_DIR}/meshes/obj/suzanne.obj.virtualgeometry_materials"
"${OBJ_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/obj/teapot.obj" --output "${OUTPUT_DIR}/meshes/obj/teapot.obj.virtualgeometry" --materials-dir "${OUTPUT_DIR}/meshes/obj/teapot.obj.virtualgeometry_materials"
"${GLTF_IMPORTER}" --input "${REPO_ROOT}/assets/meshes/gltf/CesiumMan.gltf" --output "${OUTPUT_DIR}/meshes/gltf/CesiumMan.gltf.virtualgeometry"
