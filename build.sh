#!/usr/bin/env bash
set -e
set -o pipefail

BUILD_DIR="build"
MODERN_X86=OFF
X86_LEVEL="x86-64-v3"
BUILD_TYPE="Release"
INSTALL=0
CLEAN=0
CMAKE_FLAGS=()

print_help() {
    cat << EOF
Usage: ./build.sh [OPTIONS] [-- <additional cmake flags>]

Options:
  -i,  --install              Build and install executables, PAM module, and model
  -m,  --modern               Build for modern x86_64 (x86-64-v3) instead of generic x86-64
  -l,  --level <level>        Specify x86_64 level when modern is used (x86-64-v2, x86-64-v3, x86-64-v4)
  -c,  --clean                Wipe the build directory before configuring
  -d,  --debug                Configure build in Debug mode
  -r,  --release              Configure build in Release mode (default: -O3 + stripped symbols)
  -h,  --help                 Show this help message

Examples:
  ./build.sh                         Build binaries and PAM module (-O3, native tuning)
  sudo ./build.sh -i                 Build and install system-wide
  ./build.sh -m                      Build targeting x86-64-v3
  ./build.sh -c -r                   Clean wipe and rebuild in Release mode
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -i|--install)
            INSTALL=1
            BUILD_TYPE="Release"
            shift
            ;;
        -m|--modern)
            MODERN_X86=ON
            shift
            ;;
        -l|--level)
            X86_LEVEL="$2"
            MODERN_X86=ON
            shift 2
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -r|--release)
            BUILD_TYPE="Release"
            shift
            ;;
        -h|--help)
            print_help
            exit 0
            ;;
        --)
            shift
            CMAKE_FLAGS+=("$@")
            break
            ;;
        *)
            CMAKE_FLAGS+=("$1")
            shift
            ;;
    esac
done

if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "==> Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

CMAKE_CONFIG_ARGS=(
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DMODERN_X86_64="$MODERN_X86"
)

if [ "$MODERN_X86" = "ON" ]; then
    CMAKE_CONFIG_ARGS+=("-DX86_64_LEVEL=$X86_LEVEL")
fi

if [ ${#CMAKE_FLAGS[@]} -gt 0 ]; then
    CMAKE_CONFIG_ARGS+=("${CMAKE_FLAGS[@]}")
fi

echo "==> Configuring with: cmake ${CMAKE_CONFIG_ARGS[*]}"
cmake "${CMAKE_CONFIG_ARGS[@]}"

echo "==> Building project..."

BUILD_CMD=(cmake --build "$BUILD_DIR" --parallel)
if command -v stdbuf >/dev/null 2>&1; then
    BUILD_CMD=(stdbuf -oL -eL "${BUILD_CMD[@]}")
fi

if [ -t 1 ]; then
    "${BUILD_CMD[@]}" 2>&1 | while IFS= read -r line; do
        if [[ "$line" =~ ^\[[[:space:]]*([0-9]+(%|/[0-9]+))\][[:space:]]*(.*) ]]; then
            pct="${BASH_REMATCH[1]}"
            rest="${BASH_REMATCH[3]}"

            if [[ "$rest" =~ Building[[:space:]]C[[:space:]]object[[:space:]]+(.*) ]]; then
                file="${BASH_REMATCH[1]}"
                file="${file#CMakeFiles/*.dir/}"
                file="${file%.o}"
                printf "\r\033[K\033[1;32m[%4s]\033[0m %s" "$pct" "$file"
            elif [[ "$rest" =~ Linking[[:space:]]+(.*) ]]; then
                target="${BASH_REMATCH[1]}"
                target="${target#*executable }"
                target="${target#*library }"
                printf "\r\033[K\033[1;36m[%4s]\033[0m Linking %s" "$pct" "$target"
            elif [[ "$rest" =~ Built[[:space:]]target[[:space:]]+(.*) ]]; then
                printf "\r\033[K\033[1;34m[%4s]\033[0m Built %s" "$pct" "${BASH_REMATCH[1]}"
            fi
        else
            printf "\n%s" "$line"
        fi
    done
    printf "\r\033[K\033[1;32m==> Build complete.\033[0m\n"
else
    "${BUILD_CMD[@]}"
fi

if [ "$INSTALL" -eq 1 ]; then
    echo "==> Installing system components..."

    if [ "$(id -u)" -ne 0 ]; then
        echo -e "\033[1;31m[!] Error: Installation requires root privileges. Run with sudo.\033[0m"
        exit 1
    fi

    # Locate the compiled ONNX model
    MODEL_SRC=""
    for m in "model.onnx" "train/model.onnx"; do
        if [ -f "$m" ]; then
            MODEL_SRC="$m"
            break
        fi
    done

    # Create target directories
    install -d -m 755 /usr/bin
    install -d -m 755 /usr/lib/security
    install -d -m 755 /usr/lib/elanfp
    install -d -m 755 /usr/share/elanfp
    install -d -m 700 /var/lib/elanfp/templates

    # Install executables and PAM module
    install -m 755 "${BUILD_DIR}/elanfp-enroll" /usr/bin/
    install -m 755 "${BUILD_DIR}/elanfp-verify" /usr/bin/
    install -m 755 "${BUILD_DIR}/pam_elanfp.so" /usr/lib/security/

    # Strip symbols if release build
    if [ "$BUILD_TYPE" = "Release" ] && command -v strip >/dev/null 2>&1; then
        strip -s /usr/bin/elanfp-enroll /usr/bin/elanfp-verify /usr/lib/security/pam_elanfp.so 2>/dev/null || true
    fi

    # Install local ONNX runtime libraries if present
    if compgen -G "onnx/lib/libonnxruntime.so*" > /dev/null; then
        install -m 755 onnx/lib/libonnxruntime.so* /usr/lib/elanfp/
    fi

    # Install model and external data if present
    if [ -n "$MODEL_SRC" ]; then
        install -m 644 "$MODEL_SRC" /usr/share/elanfp/model.onnx
        echo -e "  \033[1;32m[+]\033[0m Installed model to /usr/share/elanfp/model.onnx"

        # Check for split external weights
        DATA_SRC=""
        if [ -f "${MODEL_SRC}.data" ]; then
            DATA_SRC="${MODEL_SRC}.data"
        elif [ -f "${MODEL_SRC%.onnx}.data" ]; then
            DATA_SRC="${MODEL_SRC%.onnx}.data"
        fi

        if [ -n "$DATA_SRC" ]; then
            install -m 644 "$DATA_SRC" "/usr/share/elanfp/$(basename "$DATA_SRC")"
            echo -e "  \033[1;32m[+]\033[0m Installed external weights to /usr/share/elanfp/$(basename "$DATA_SRC")"
        fi
    else
        echo -e "  \033[1;33m[!]\033[0m Warning: No model.onnx found. Train/export model to /usr/share/elanfp/model.onnx"
    fi

    echo -e "\033[1;32m==> Installation successful\033[0m"
fi