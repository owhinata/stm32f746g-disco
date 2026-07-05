#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Generate an X-CUBE-AI relocatable network binary (network_rel.bin) for the
# stedgeai_reloc backend (Epic #80 P5, issue #92).
#
# The output network_rel.bin is a single, position-independent blob (PIC code +
# embedded weights) that the firmware loads from the microSD card at runtime and
# runs XIP -- no reflash to swap models.  It is NOT committed (ST-SLA / model-zoo
# licensed; see .gitignore).  Copy the produced .bin to the SD card root, then on
# the board:  ai model load network_rel.bin
#
# `stedgeai generate --relocatable` invokes arm-none-eabi-gcc to compile the PIC
# network, so this script puts the project's own toolchain on PATH first.
#
# Usage:
#   scripts/gen-reloc-model.sh [MODEL.tflite] [OUTPUT_DIR]
# Defaults:
#   MODEL      = the ST Model Zoo BlazeFace-front 128 int8 model under _ref/
#   OUTPUT_DIR = port/nn/reloc-out  (gitignored)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STEDGEAI_ROOT="${STEDGEAI_ROOT:-/opt/ST/STEdgeAI/4.0}"
TC_BIN="${REPO_ROOT}/tools/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin"

MODEL="${1:-${REPO_ROOT}/_ref/stm32ai-modelzoo/face_detection/facedetect_front/Public_pretrainedmodel_public_dataset/widerface/blazeface_front_128/blazeface_front_128_int8.tflite}"
OUT="${2:-${REPO_ROOT}/port/nn/reloc-out}"

STEDGEAI="${STEDGEAI_ROOT}/Utilities/linux/stedgeai"
[ -x "${STEDGEAI}" ] || { echo "error: stedgeai not found: ${STEDGEAI} (set STEDGEAI_ROOT)"; exit 1; }
[ -f "${MODEL}" ]    || { echo "error: model not found: ${MODEL}"; exit 1; }
[ -x "${TC_BIN}/arm-none-eabi-gcc" ] || {
	echo "error: project toolchain not found under ${TC_BIN}"
	echo "       run a CMake configure once to auto-download it."
	exit 1
}

mkdir -p "${OUT}"
echo "model : ${MODEL}"
echo "output: ${OUT}"
echo "gcc   : $(PATH="${TC_BIN}:${PATH}" command -v arm-none-eabi-gcc)"

PATH="${TC_BIN}:${PATH}" "${STEDGEAI}" generate \
	--model "${MODEL}" --target stm32f7 --type tflite \
	--name network --relocatable \
	--output "${OUT}" --workspace "${OUT}/ws"

BIN="${OUT}/network_rel.bin"
[ -f "${BIN}" ] || { echo "error: ${BIN} was not produced"; exit 1; }
echo
echo "OK: $(ls -l "${BIN}" | awk '{print $5, $9}')"
echo "Copy it to the microSD card root, then on the board:  ai model load network_rel.bin"
