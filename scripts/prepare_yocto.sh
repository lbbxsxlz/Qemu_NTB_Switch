#!/usr/bin/env bash

echo "Starting prepare_yocto..."

set -x
set -e

. "$(dirname $0)"/common_variables.sh

DEFAULT_POKY_GIT_URL="https://github.com/yoctoproject/poky.git"
POKY_GIT_URL="${POKY_GIT_URL:-$DEFAULT_POKY_GIT_URL}"

cd $YOCTO_WORK_DIR
if [ -d poky ] && [ ! -d poky/.git ]; then
    rm -rf poky
fi

if [ ! -d poky ]; then
    git clone "$POKY_GIT_URL" poky
fi
cd poky

CURRENT_ORIGIN_URL="$(git remote get-url origin)"
if [ "$CURRENT_ORIGIN_URL" != "$POKY_GIT_URL" ]; then
    case "$CURRENT_ORIGIN_URL" in
        git://git.yoctoproject.org/poky|git://git.yoctoproject.org/poky.git|https://git.yoctoproject.org/poky|https://git.yoctoproject.org/poky.git)
            git remote set-url origin "$POKY_GIT_URL"
            ;;
    esac

    if [ "$POKY_GIT_URL" != "$DEFAULT_POKY_GIT_URL" ]; then
        git remote set-url origin "$POKY_GIT_URL"
    fi
fi

git fetch origin "$YOCTO_VER_NAME"

if ! git checkout my-$YOCTO_VER_NAME; then
    git checkout -t origin/$YOCTO_VER_NAME  -b my-$YOCTO_VER_NAME
fi
git pull

source oe-init-build-env build

cp $ROOT_PROJECT_PATH/yocto_files/configs/local.conf ./conf/local.conf
rm -r ../$LAYER_NAME || true # cleanup
cp -r $ROOT_PROJECT_PATH/yocto_files/$LAYER_NAME ../$LAYER_NAME

# Add layer to bitbake if it has not already been added
if [ -z "$(bitbake-layers show-layers | grep $LAYER_NAME)" ]; then
    bitbake-layers add-layer ../$LAYER_NAME
fi

set +x

echo "Finished prepare_yocto"
