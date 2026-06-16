#!/bin/bash

RUN_ARGS=()
BUILD_NO_CACHE=""
DOCKER_REBUILD=""
DOCKER_BASE_IMAGE="ubuntu:22.04"
DOCKER_IMAGE_TAG="yocto:latest"
DOCKER_CONTEXT_LABEL="qemu-ntb-switch.context-sha"
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
        --docker-rebuild*)
            DOCKER_REBUILD="1"
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

DOCKER_CONTEXT_SHA=$(
    {
        printf 'BASE_IMAGE=%s\n' "$DOCKER_BASE_IMAGE"
        printf 'USER_ID=%s\n' "$(id -u)"
        find Dockerfile scripts -type f -print0 |
            sort -z |
            xargs -0 sha256sum
    } | sha256sum | awk '{print $1}'
)

DOCKER_BUILD_ARGS=(
    .
    --pull=false
    --build-arg "BASE_IMAGE=$DOCKER_BASE_IMAGE"
    --build-arg "user_id=$(id -u)"
    --label "$DOCKER_CONTEXT_LABEL=$DOCKER_CONTEXT_SHA"
    -t "$DOCKER_IMAGE_TAG"
)

for proxy_var in "${PROXY_VARS[@]}"; do
    if [ -n "${!proxy_var}" ]; then
        DOCKER_BUILD_ARGS+=(--build-arg "$proxy_var=${!proxy_var}")
    fi
done

if [ -n "$BUILD_NO_CACHE" ]; then
    DOCKER_BUILD_ARGS+=("$BUILD_NO_CACHE")
fi

EXISTING_CONTEXT_SHA=""
PREVIOUS_DOCKER_IMAGE_ID=""
if docker image inspect "$DOCKER_IMAGE_TAG" >/dev/null 2>&1; then
    PREVIOUS_DOCKER_IMAGE_ID=$(docker image inspect "$DOCKER_IMAGE_TAG" --format '{{.Id}}')
    EXISTING_CONTEXT_SHA=$(docker image inspect "$DOCKER_IMAGE_TAG" --format "{{ index .Config.Labels \"$DOCKER_CONTEXT_LABEL\" }}" 2>/dev/null || true)
fi

if [ -z "$DOCKER_REBUILD" ] && [ -z "$BUILD_NO_CACHE" ] && [ "$EXISTING_CONTEXT_SHA" = "$DOCKER_CONTEXT_SHA" ]; then
    echo "Using existing Docker image '$DOCKER_IMAGE_TAG'. Pass --docker-rebuild or --docker-no-cache to rebuild."
else
    if ! docker build "${DOCKER_BUILD_ARGS[@]}"; then
        echo "Docker image build failed." >&2
        echo "The selected base image is '$DOCKER_BASE_IMAGE'. If Docker Hub is unreachable, rerun with --docker-base-image=<registry>/<image>:<tag> or preload the base image locally." >&2
        exit 1
    fi

    CURRENT_DOCKER_IMAGE_ID=$(docker image inspect "$DOCKER_IMAGE_TAG" --format '{{.Id}}')
    if [ -n "$PREVIOUS_DOCKER_IMAGE_ID" ] && [ "$PREVIOUS_DOCKER_IMAGE_ID" != "$CURRENT_DOCKER_IMAGE_ID" ]; then
        if ! docker image rm "$PREVIOUS_DOCKER_IMAGE_ID" >/dev/null 2>&1; then
            echo "Old Docker image '$PREVIOUS_DOCKER_IMAGE_ID' could not be removed automatically; it may still be in use." >&2
        fi
    fi
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
    -v "$PWD"/user_src:/home/user/project/user_src
    -v "$PWD"/kernel_src:/home/user/project/kernel_src
)

if [ -e /dev/kvm ]; then
    DOCKER_RUN_ARGS+=(--device /dev/kvm --group-add "$(stat -c %g /dev/kvm)")
fi

for proxy_var in "${PROXY_VARS[@]}"; do
    if [ -n "${!proxy_var}" ]; then
        DOCKER_RUN_ARGS+=(-e "$proxy_var=${!proxy_var}")
    fi
done

docker run "${DOCKER_RUN_ARGS[@]}" "$DOCKER_IMAGE_TAG" "${RUN_ARGS[@]}"
