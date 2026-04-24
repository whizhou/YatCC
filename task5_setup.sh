#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_DIR="${YatCC_LLVM_DIR:-${ROOT_DIR}/llvm}"
LLVM_TARBALL="${LLVM_DIR}/llvm-18.src.tar.xz"
LLVM_TBLGEN="/usr/lib/llvm-18/bin/llvm-tblgen"
LLVM_TASK5_INC_DIR="${LLVM_DIR}/task5/include"

log() {
  printf '[task5-setup] %s\n' "$*"
}

die() {
  printf '[task5-setup] error: %s\n' "$*" >&2
  exit 1
}

has_cmd() {
  command -v "$1" >/dev/null 2>&1
}

has_pkg() {
  dpkg -s "$1" >/dev/null 2>&1
}

install_if_missing() {
  local pkgs=("$@")
  local missing=()
  local pkg

  for pkg in "${pkgs[@]}"; do
    if ! has_pkg "${pkg}"; then
      missing+=("${pkg}")
    fi
  done

  if [[ ${#missing[@]} -eq 0 ]]; then
    return 0
  fi

  log "installing missing apt packages: ${missing[*]}"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends "${missing[@]}"
}

ensure_system_packages() {
  install_if_missing \
    build-essential \
    cmake \
    ninja-build \
    python3 \
    python3-dev \
    default-jdk \
    bison \
    flex \
    unzip \
    wget \
    lld \
    libncurses-dev \
    libzstd-dev \
    qemu-user-static \
    gcc-riscv64-linux-gnu \
    g++-riscv64-linux-gnu \
    llvm-18 \
    clang-18
}

ensure_repo_dependencies() {
  if [[ ! -d "${ROOT_DIR}/antlr/install" || ! -f "${ROOT_DIR}/antlr/antlr.jar" ]]; then
    log "bootstrapping antlr"
    (cd "${ROOT_DIR}/antlr" && ./setup.sh)
  fi

  if [[ ! -d "${ROOT_DIR}/pybind11/install" ]]; then
    log "bootstrapping pybind11"
    (cd "${ROOT_DIR}/pybind11" && ./setup.sh)
  fi
}

ensure_local_llvm_install() {
  [[ -d "${LLVM_DIR}/install" ]] || die "missing ${LLVM_DIR}/install"
}

download_llvm_source_subset() {
  mkdir -p "${LLVM_DIR}"

  if [[ ! -f "${LLVM_TARBALL}" ]]; then
    log "downloading llvm source archive"
    wget -O "${LLVM_TARBALL}" \
      https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/llvm-18.1.8.src.tar.xz
  fi

  rm -rf "${LLVM_DIR}/llvm"
  mkdir -p "${LLVM_DIR}/llvm"

  log "extracting llvm include files"
  tar -xJf "${LLVM_TARBALL}" -C "${LLVM_DIR}/llvm" --strip-components=1 \
    llvm-18.1.8.src/include

  log "extracting llvm RISCV target files"
  tar -xJf "${LLVM_TARBALL}" -C "${LLVM_DIR}/llvm" --strip-components=1 \
    llvm-18.1.8.src/lib/Target/RISCV
}

ensure_local_llvm_sources() {
  mkdir -p "${LLVM_DIR}"

  if [[ -f "${LLVM_DIR}/llvm/lib/Target/RISCV/RISCV.td" ]]; then
    return 0
  fi

  download_llvm_source_subset
}

generate_riscv_tablegen_headers() {
  local out_dir="${LLVM_TASK5_INC_DIR}"

  [[ -x "${LLVM_TBLGEN}" ]] || die "missing ${LLVM_TBLGEN}"
  mkdir -p "${out_dir}"

  if [[ ! -f "${out_dir}/RISCVGenRegisterInfo.inc" ]]; then
    log "generating RISCVGenRegisterInfo.inc"
    "${LLVM_TBLGEN}" \
      -gen-register-info \
      -I "${LLVM_DIR}/llvm/include" \
      -I "${LLVM_DIR}/install/include" \
      -I "${LLVM_DIR}/llvm/lib/Target" \
      -I "${LLVM_DIR}/llvm/lib/Target/RISCV" \
      -o "${out_dir}/RISCVGenRegisterInfo.inc" \
      "${LLVM_DIR}/llvm/lib/Target/RISCV/RISCV.td"
  fi

  if [[ ! -f "${out_dir}/RISCVGenInstrInfo.inc" ]]; then
    log "generating RISCVGenInstrInfo.inc"
    "${LLVM_TBLGEN}" \
      -gen-instr-info \
      -I "${LLVM_DIR}/llvm/include" \
      -I "${LLVM_DIR}/install/include" \
      -I "${LLVM_DIR}/llvm/lib/Target" \
      -I "${LLVM_DIR}/llvm/lib/Target/RISCV" \
      -o "${out_dir}/RISCVGenInstrInfo.inc" \
      "${LLVM_DIR}/llvm/lib/Target/RISCV/RISCV.td"
  fi
}

configure_build_dir() {
  log "configuring build directory with YatCC_LLVM_DIR=${LLVM_DIR}"
  YatCC_LLVM_DIR="${LLVM_DIR}" cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build" -G Ninja
}

main() {
  [[ "${EUID}" -eq 0 ]] || die "please run this script as root"
  has_cmd apt-get || die "apt-get is required"

  ensure_system_packages
  ensure_repo_dependencies
  ensure_local_llvm_install
  ensure_local_llvm_sources
  generate_riscv_tablegen_headers
  configure_build_dir

  log "environment is ready"
  log "run with: YatCC_LLVM_DIR=${LLVM_DIR} cmake --build build -t task5-score"
}

main "$@"
