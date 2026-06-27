#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_DIR="${YatCC_LLVM_DIR:-${ROOT_DIR}/llvm}"
LLVM_TARBALL="${LLVM_DIR}/llvm-18.src.tar.xz"
LLVM_TBLGEN="/usr/lib/llvm-18/bin/llvm-tblgen"
LLVM_TASK5_INC_DIR="${LLVM_DIR}/task5/include"

APT_PROXY="http://12.8.3.207:3138"
APT_PROXY_CONF="/etc/apt/apt.conf.d/99task5-proxy"

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

save_proxy_env() {
  OLD_http_proxy="${http_proxy-}"
  OLD_https_proxy="${https_proxy-}"
  OLD_ftp_proxy="${ftp_proxy-}"
  OLD_all_proxy="${all_proxy-}"

  OLD_HTTP_PROXY="${HTTP_PROXY-}"
  OLD_HTTPS_PROXY="${HTTPS_PROXY-}"
  OLD_FTP_PROXY="${FTP_PROXY-}"
  OLD_ALL_PROXY="${ALL_PROXY-}"
}

set_proxy_env() {
  export http_proxy="${APT_PROXY}"
  export https_proxy="${APT_PROXY}"
  export ftp_proxy="${APT_PROXY}"
  export all_proxy="${APT_PROXY}"

  export HTTP_PROXY="${APT_PROXY}"
  export HTTPS_PROXY="${APT_PROXY}"
  export FTP_PROXY="${APT_PROXY}"
  export ALL_PROXY="${APT_PROXY}"

  cat > "${APT_PROXY_CONF}" <<EOF
Acquire::http::Proxy "${APT_PROXY}/";
Acquire::https::Proxy "${APT_PROXY}/";
Acquire::ftp::Proxy "${APT_PROXY}/";
EOF

  log "proxy configured as ${APT_PROXY}"
}

restore_var() {
  local name="$1"
  local value="$2"

  if [[ -n "${value}" ]]; then
    export "${name}=${value}"
  else
    unset "${name}" || true
  fi
}

cleanup() {
  restore_var http_proxy "${OLD_http_proxy-}"
  restore_var https_proxy "${OLD_https_proxy-}"
  restore_var ftp_proxy "${OLD_ftp_proxy-}"
  restore_var all_proxy "${OLD_all_proxy-}"

  restore_var HTTP_PROXY "${OLD_HTTP_PROXY-}"
  restore_var HTTPS_PROXY "${OLD_HTTPS_PROXY-}"
  restore_var FTP_PROXY "${OLD_FTP_PROXY-}"
  restore_var ALL_PROXY "${OLD_ALL_PROXY-}"

  rm -f "${APT_PROXY_CONF}" || true

  log "proxy environment restored"
}

configure_tsinghua_apt_source() {
  log "configuring Tsinghua apt source for Ubuntu 24.04"

  if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    source /etc/os-release
  else
    die "missing /etc/os-release"
  fi

  [[ "${ID:-}" == "ubuntu" ]] || die "this script expects Ubuntu"
  [[ "${VERSION_CODENAME:-}" == "noble" ]] || die "this script expects Ubuntu 24.04 noble, got VERSION_CODENAME=${VERSION_CODENAME:-unknown}"

  local backup_suffix
  backup_suffix="$(date +%Y%m%d-%H%M%S)"

  if [[ -f /etc/apt/sources.list ]]; then
    cp -a /etc/apt/sources.list "/etc/apt/sources.list.bak.${backup_suffix}"
  fi

  if [[ -f /etc/apt/sources.list.d/ubuntu.sources ]]; then
    cp -a /etc/apt/sources.list.d/ubuntu.sources "/etc/apt/sources.list.d/ubuntu.sources.bak.${backup_suffix}"
  fi

  cat > /etc/apt/sources.list.d/ubuntu.sources <<EOF
Types: deb
URIs: https://mirrors.tuna.tsinghua.edu.cn/ubuntu/
Suites: noble noble-updates noble-backports
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: https://mirrors.tuna.tsinghua.edu.cn/ubuntu/
Suites: noble-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

  # 避免旧的 /etc/apt/sources.list 和 ubuntu.sources 同时启用导致重复源警告。
  # 如果你的系统没有 /etc/apt/sources.list，这一步不会有影响。
  if [[ -f /etc/apt/sources.list ]]; then
    cat > /etc/apt/sources.list <<EOF
# Disabled by task5 setup script.
# Ubuntu 24.04 uses /etc/apt/sources.list.d/ubuntu.sources below.
EOF
  fi

  log "apt source configured: https://mirrors.tuna.tsinghua.edu.cn/ubuntu/"
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

  
  log "downloading llvm source archive"
  wget -O "${LLVM_TARBALL}" \
    https://gh-proxy.com/https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/llvm-18.1.8.src.tar.xz
  

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
  if [[ "${YATCC_TASK5_SETUP_SKIP_CMAKE_CONFIGURE:-0}" == "1" ]]; then
    log "skipping build directory configuration"
    return 0
  fi

  log "configuring build directory with YatCC_LLVM_DIR=${LLVM_DIR}"
  YatCC_LLVM_DIR="${LLVM_DIR}" cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build" -G Ninja
}

main() {
  [[ "${EUID}" -eq 0 ]] || die "please run this script as root"
  has_cmd apt-get || die "apt-get is required"

  save_proxy_env
  trap cleanup EXIT

  set_proxy_env
  configure_tsinghua_apt_source

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
