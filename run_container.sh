#!/bin/bash

RUN_ARGS=()
BUILD_NO_CACHE=""
DOCKER_BASE_IMAGE="ubuntu:22.04"
PROXY_VARS=(http_proxy https_proxy no_proxy HTTP_PROXY HTTPS_PROXY NO_PROXY)

for ARG in "$@"; do
    case $ARG in
        --host-build-dir=*)
            BUILD_FOLDER_NAME=${ARG#*=}
            ;;
        --docker-base-image=*)
            DOCKER_BASE_IMAGE=${ARG#*=}
            ;;
        --docker-no-cache*)
            BUILD_NO_CACHE="--no-cache"
            ;;
        --docker-shm-size*)
            DOCKER_SHM_SIZE="${ARG#*=}"
            ;;
        *)
            RUN_ARGS+=("$ARG")
            ;;
    esac
done

if [ -z "$BUILD_FOLDER_NAME" ]; then
    echo -e "\e[1;33mHost build dir is not provided, using 'build_vm_image' dir\e[0m"
    BUILD_FOLDER_NAME=build_vm_image/
fi

if ! [ "$DOCKER_SHM_SIZE" ]; then
    DOCKER_SHM_SIZE="1024m";
fi

BUILD_PATH="$PWD"/"$BUILD_FOLDER_NAME"

mkdir -p "$BUILD_FOLDER_NAME"
DOCKER_BUILD_ARGS=(
    .
    --build-arg "BASE_IMAGE=$DOCKER_BASE_IMAGE"
    --build-arg "user_id=$(id -u)"
    -t yocto
)

for proxy_var in "${PROXY_VARS[@]}"; do
    if [ -n "${!proxy_var}" ]; then
        DOCKER_BUILD_ARGS+=(--build-arg "$proxy_var=${!proxy_var}")
    fi
done

if [ -n "$BUILD_NO_CACHE" ]; then
    DOCKER_BUILD_ARGS+=("$BUILD_NO_CACHE")
fi

if ! docker build "${DOCKER_BUILD_ARGS[@]}"; then
    echo "Docker image build failed." >&2
    echo "The selected base image is '$DOCKER_BASE_IMAGE'. If Docker Hub is unreachable, rerun with --docker-base-image=<registry>/<image>:<tag> or preload the base image locally." >&2
    exit 1
fi

DOCKER_RUN_ARGS=(
    -it
    --rm
    -p 7001:7001
    -p 7002:7002
    -p 8001:8001
    -p 8002:8002
    --shm-size="$DOCKER_SHM_SIZE"
    -v "$BUILD_PATH":/home/user/project/build_dir
    -v "$PWD"/qemu_src:/home/user/project/qemu_src
    -v "$PWD"/yocto_files:/home/user/project/yocto_files
)

for proxy_var in "${PROXY_VARS[@]}"; do
    if [ -n "${!proxy_var}" ]; then
        DOCKER_RUN_ARGS+=(-e "$proxy_var=${!proxy_var}")
    fi
done

docker run "${DOCKER_RUN_ARGS[@]}" yocto "${RUN_ARGS[@]}"
