#!/usr/bin/env bash
# Сборка и тесты ядра внутри Docker (на хосте может не быть C++20-компилятора).
#
#   scripts/dev.sh image    собрать образ pac-core-dev (один раз и при смене зависимостей)
#   scripts/dev.sh build    конфигурация и сборка
#   scripts/dev.sh test     сборка и ctest
#   scripts/dev.sh all      image + test (по умолчанию)
#   scripts/dev.sh shell    интерактивная оболочка в контейнере
#   scripts/dev.sh clean    удалить том со сборкой
#
# Переменные: PRESET (debug), JOBS (2), IMAGE (pac-core-dev).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

IMAGE="${IMAGE:-pac-core-dev}"
PRESET="${PRESET:-debug}"
JOBS="${JOBS:-2}"
BUILD_VOLUME="${BUILD_VOLUME:-pac-core-build}"

# Git Bash на Windows: не переписывать пути контейнера, а путь хоста взять в виде C:/...
export MSYS_NO_PATHCONV=1
if SRC_DIR="$(pwd -W 2>/dev/null)"; then :; else SRC_DIR="$(pwd)"; fi

# Каталог build — в именованном томе: быстрее, чем bind mount, и не засоряет рабочую копию.
# Использование: docker_run [опции docker] -- команда...
docker_run() {
  local opts=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do opts+=("$1"); shift; done
  shift
  docker run --rm "${opts[@]}" \
    -v "${SRC_DIR}:/src" \
    -v "${BUILD_VOLUME}:/src/build" \
    -w /src \
    "${IMAGE}" "$@"
}

cmd_image() {
  docker build -f Dockerfile.dev -t "${IMAGE}" .
}

cmd_build() {
  docker_run -- bash -euc "cmake --preset ${PRESET} && cmake --build --preset ${PRESET} -j${JOBS}"
}

cmd_test() {
  docker_run -- bash -euc "cmake --preset ${PRESET} && cmake --build --preset ${PRESET} -j${JOBS} && ctest --preset ${PRESET} -j${JOBS}"
}

cmd_shell() {
  docker_run -it -- bash
}

cmd_clean() {
  docker volume rm "${BUILD_VOLUME}"
}

case "${1:-all}" in
  image) cmd_image ;;
  build) cmd_build ;;
  test) cmd_test ;;
  shell) cmd_shell ;;
  clean) cmd_clean ;;
  all) cmd_image && cmd_test ;;
  *) sed -n '2,12p' "$0"; exit 2 ;;
esac
