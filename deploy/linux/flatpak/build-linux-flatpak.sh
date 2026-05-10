#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
APP_ID="org.deskflow.deskflow"
BRANCH="${DESKFLOW_FLATPAK_BRANCH:-master}"
PLATFORM="${DESKFLOW_DOCKER_PLATFORM:-linux/amd64}"

OUT_DIR="${DESKFLOW_FLATPAK_OUT_DIR:-${ROOT_DIR}/build-flatpak}"
if [[ "${OUT_DIR}" != /* ]]; then
  OUT_DIR="${ROOT_DIR}/${OUT_DIR}"
fi

case "${PLATFORM}" in
  linux/amd64)
    FLATPAK_ARCH="x86_64"
    LOCAL_DOCKER_ARCH_TAG="amd64"
    ;;
  linux/arm64|linux/arm64/v8)
    FLATPAK_ARCH="aarch64"
    LOCAL_DOCKER_ARCH_TAG="arm64"
    ;;
  *)
    echo "Unsupported DESKFLOW_DOCKER_PLATFORM: ${PLATFORM}" >&2
    echo "Supported values: linux/amd64, linux/arm64" >&2
    exit 2
    ;;
esac

DOCKER_IMAGE="${DESKFLOW_FLATPAK_BUILDER_IMAGE:-ghcr.io/flathub-infra/flatpak-github-actions:kde-6.10}"
CONTAINER_OUT_DIR="/workspace/build-flatpak"
CONTAINER_SOURCE_DIR="${CONTAINER_OUT_DIR}/source"
CONTAINER_MANIFEST_DIR="${CONTAINER_OUT_DIR}/manifest"
CONTAINER_BUILD_DIR="${CONTAINER_OUT_DIR}/build"
CONTAINER_REPO_DIR="${CONTAINER_OUT_DIR}/repo"
CONTAINER_STATE_DIR="${CONTAINER_OUT_DIR}/builder-state"
CONTAINER_MANIFEST_PATH="${CONTAINER_MANIFEST_DIR}/${APP_ID}.yml"

SOURCE_DIR="${OUT_DIR}/source"
MANIFEST_DIR="${OUT_DIR}/manifest"
FLATPAK_HOME_DIR="${OUT_DIR}/flatpak-home"
FLATPAK_SYSTEM_DIR="${OUT_DIR}/flatpak-system"
FLATPAK_CACHE_DIR="${OUT_DIR}/flatpak-cache"

usage() {
  cat <<'EOF'
Usage: deploy/linux/flatpak/build-linux-flatpak.sh [--clean] [--build-image]

Build a Flatpak bundle for Deskflow using Docker. Defaults to an x86_64
bundle for Ubuntu 24.04 / Linux Mint 22.x, whose native Qt and libportal
packages are too old for current Deskflow.

Environment overrides:
  DESKFLOW_DOCKER_PLATFORM          Docker platform, default linux/amd64
  DESKFLOW_FLATPAK_BUILDER_IMAGE    Builder image tag, default CI Flatpak image
  DESKFLOW_FLATPAK_OUT_DIR          Output/cache directory, default build-flatpak
  DESKFLOW_FLATPAK_BRANCH           Flatpak branch, default master
  DESKFLOW_FLATPAK_VERSION          Bundle version label, default git describe
EOF
}

CLEAN=0
BUILD_IMAGE="${DESKFLOW_FLATPAK_BUILD_IMAGE:-0}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN=1
      shift
      ;;
    --build-image)
      BUILD_IMAGE=1
      shift
      ;;
    --no-build-image)
      BUILD_IMAGE=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required" >&2
  exit 1
fi

if ! command -v rsync >/dev/null 2>&1; then
  echo "rsync is required" >&2
  exit 1
fi

if [[ "${BUILD_IMAGE}" == "1" && -z "${DESKFLOW_FLATPAK_BUILDER_IMAGE:-}" ]]; then
  DOCKER_IMAGE="deskflow-flatpak-builder:ubuntu24.04-${LOCAL_DOCKER_ARCH_TAG}"
fi

if [[ "${BUILD_IMAGE}" == "1" ]]; then
  echo "Ensuring Docker builder image ${DOCKER_IMAGE}..."
  docker build --platform "${PLATFORM}" -t "${DOCKER_IMAGE}" - <<'DOCKERFILE'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates \
    elfutils \
    flatpak \
    flatpak-builder \
    git \
    patch \
    python3 \
    python3-pip \
    xz-utils \
  && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi

if [[ "${CLEAN}" == "1" ]]; then
  if ! rm -rf "${OUT_DIR}" "${ROOT_DIR}/.flatpak-builder" 2>/dev/null; then
    echo "Cleaning root-owned Flatpak build paths with Docker..."
    mkdir -p "${OUT_DIR}"
    docker run --rm \
      --platform "${PLATFORM}" \
      -v "${ROOT_DIR}:/workspace" \
      -v "${OUT_DIR}:${CONTAINER_OUT_DIR}" \
      "${DOCKER_IMAGE}" \
      bash -lc "find '${CONTAINER_OUT_DIR}' -mindepth 1 -maxdepth 1 -exec rm -rf {} +; rm -rf /workspace/.flatpak-builder"
  fi
fi

mkdir -p \
  "${SOURCE_DIR}" \
  "${MANIFEST_DIR}" \
  "${FLATPAK_HOME_DIR}" \
  "${FLATPAK_SYSTEM_DIR}" \
  "${FLATPAK_CACHE_DIR}" \
  "${OUT_DIR}/builder-state"

VERSION="${DESKFLOW_FLATPAK_VERSION:-$(
  cd "${ROOT_DIR}"
  git describe --tags --always --dirty 2>/dev/null || printf 'workspace'
)}"
VERSION="${VERSION//\//-}"
BUNDLE_NAME="deskflow-${VERSION}-linux-${FLATPAK_ARCH}.flatpak"
CONTAINER_BUNDLE_PATH="${CONTAINER_OUT_DIR}/${BUNDLE_NAME}"

echo "Staging source tree..."
RSYNC_EXCLUDES=(
  --exclude='.git/'
  --exclude='.flatpak-builder/'
  --exclude='/build/'
  --exclude='/build-*'
  --exclude='/cmake-build-*'
  --exclude='/build-flatpak/'
)
if [[ "${OUT_DIR}" == "${ROOT_DIR}/"* ]]; then
  OUT_DIR_REL="${OUT_DIR#${ROOT_DIR}/}"
  RSYNC_EXCLUDES+=(--exclude="/${OUT_DIR_REL%/}/")
fi

rsync -a --delete \
  "${RSYNC_EXCLUDES[@]}" \
  "${ROOT_DIR}/" "${SOURCE_DIR}/"

echo "Staging Flatpak manifest..."
rsync -a --delete "${ROOT_DIR}/deploy/linux/flatpak/" "${MANIFEST_DIR}/"
if ! grep -q 'path: ../../../' "${MANIFEST_DIR}/${APP_ID}.yml"; then
  echo "Expected local source path not found in ${APP_ID}.yml" >&2
  exit 1
fi
sed -e "s|path: ../../../|path: ${CONTAINER_SOURCE_DIR}|" \
  "${MANIFEST_DIR}/${APP_ID}.yml" \
  > "${MANIFEST_DIR}/${APP_ID}.yml.tmp"
mv "${MANIFEST_DIR}/${APP_ID}.yml.tmp" "${MANIFEST_DIR}/${APP_ID}.yml"

echo "Building Flatpak bundle ${BUNDLE_NAME}..."
docker run --rm \
  --platform "${PLATFORM}" \
  --privileged \
  --security-opt seccomp=unconfined \
  -v "${ROOT_DIR}:/workspace" \
  -v "${OUT_DIR}:${CONTAINER_OUT_DIR}" \
  -v "${FLATPAK_HOME_DIR}:/root" \
  -v "${FLATPAK_SYSTEM_DIR}:/var/lib/flatpak" \
  -v "${FLATPAK_CACHE_DIR}:/var/cache/flatpak" \
  -w /workspace \
  "${DOCKER_IMAGE}" \
  bash -lc "
    set -euo pipefail
    flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
    flatpak install -y --noninteractive --arch='${FLATPAK_ARCH}' flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
    flatpak-builder --force-clean --install-deps-from=flathub --arch='${FLATPAK_ARCH}' --state-dir='${CONTAINER_STATE_DIR}' --repo='${CONTAINER_REPO_DIR}' '${CONTAINER_BUILD_DIR}' '${CONTAINER_MANIFEST_PATH}'
    flatpak build-bundle --arch='${FLATPAK_ARCH}' '${CONTAINER_REPO_DIR}' '${CONTAINER_BUNDLE_PATH}' '${APP_ID}' '${BRANCH}'
    chown $(id -u):$(id -g) '${CONTAINER_BUNDLE_PATH}'
  "

echo "Built ${OUT_DIR}/${BUNDLE_NAME}"
