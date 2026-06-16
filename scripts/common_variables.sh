#!/usr/bin/env bash

set -e

ROOT_PROJECT_PATH="$PWD"
YOCTO_VER_NAME="langdale"
LAYER_NAME="meta-ntb-cxl"
NTB_PROFILING_RECIPE_DIR="recipes-benchmark/ntb-profiling"
NTB_PROFILING_RECIPE_FILES_DIR="$NTB_PROFILING_RECIPE_DIR/files"
NTB_IDT_APP_RECIPE_DIR="recipes-kernel/ntb-idt-app"
NTB_IDT_APP_RECIPE_FILES_DIR="$NTB_IDT_APP_RECIPE_DIR/files"
NTB_IDT_SISCI_APP_RECIPE_DIR="recipes-kernel/ntb-idt-sisci-app"
NTB_IDT_SISCI_APP_RECIPE_FILES_DIR="$NTB_IDT_SISCI_APP_RECIPE_DIR/files"

YOCTO_WORK_DIR=$BUILD_PATH
if [ ! -d $YOCTO_WORK_DIR ]; then
    mkdir $YOCTO_WORK_DIR
fi

FULL_LAYER_NAME="$YOCTO_WORK_DIR"/poky/"$LAYER_NAME"

sync_ntb_profiling_recipe_sources() {
    local layer_path="$1"
    local recipe_file="ntb-profiling_0.1.bb"
    local recipe_source="$ROOT_PROJECT_PATH/yocto_files/$LAYER_NAME/$NTB_PROFILING_RECIPE_DIR/$recipe_file"
    local source_dir="$ROOT_PROJECT_PATH/user_src"
    local recipe_dest_dir="$layer_path/$NTB_PROFILING_RECIPE_DIR"
    local dest_dir="$layer_path/$NTB_PROFILING_RECIPE_FILES_DIR"

    if [ ! -f "$recipe_source" ]; then
        echo "Missing ntb_profiling recipe: $recipe_source" >&2
        exit 1
    fi

    for source_file in ntb_profiling.c ntb_generic.c ntb_utils.c ntb_profiling.h ntb_utils.h ntb_sisci.c ntb_sisci.h; do
        if [ ! -f "$source_dir/$source_file" ]; then
            echo "Missing ntb_profiling source: $source_dir/$source_file" >&2
            exit 1
        fi
    done

    mkdir -p "$recipe_dest_dir"
    cp "$recipe_source" "$recipe_dest_dir/$recipe_file"
    mkdir -p "$dest_dir"
    cp "$source_dir"/ntb_profiling.c "$dest_dir/ntb_profiling.c"
    cp "$source_dir"/ntb_generic.c "$dest_dir/ntb_generic.c"
    cp "$source_dir"/ntb_utils.c "$dest_dir/ntb_utils.c"
    cp "$source_dir"/ntb_profiling.h "$dest_dir/ntb_profiling.h"
    cp "$source_dir"/ntb_utils.h "$dest_dir/ntb_utils.h"
    cp "$source_dir"/ntb_sisci.c "$dest_dir/ntb_sisci.c"
    cp "$source_dir"/ntb_sisci.h "$dest_dir/ntb_sisci.h"
}

sync_ntb_idt_app_recipe_sources() {
    local layer_path="$1"
    local recipe_file="ntb-idt-app_0.1.bb"
    local recipe_source="$ROOT_PROJECT_PATH/yocto_files/$LAYER_NAME/$NTB_IDT_APP_RECIPE_DIR/$recipe_file"
    local source_dir="$ROOT_PROJECT_PATH/kernel_src/ntb-idt-app"
    local recipe_dest_dir="$layer_path/$NTB_IDT_APP_RECIPE_DIR"
    local dest_dir="$layer_path/$NTB_IDT_APP_RECIPE_FILES_DIR"

    if [ ! -f "$recipe_source" ]; then
        echo "Missing ntb_idt_app recipe: $recipe_source" >&2
        exit 1
    fi

    for source_file in Makefile COPYING ntb-idt-app.c; do
        if [ ! -f "$source_dir/$source_file" ]; then
            echo "Missing ntb_idt_app source: $source_dir/$source_file" >&2
            exit 1
        fi
    done

    mkdir -p "$recipe_dest_dir"
    cp "$recipe_source" "$recipe_dest_dir/$recipe_file"
    mkdir -p "$dest_dir"
    cp "$source_dir"/Makefile "$dest_dir/Makefile"
    cp "$source_dir"/COPYING "$dest_dir/COPYING"
    cp "$source_dir"/ntb-idt-app.c "$dest_dir/ntb-idt-app.c"
}

sync_ntb_idt_sisci_app_recipe_sources() {
    local layer_path="$1"
    local recipe_file="ntb-idt-sisci-app_0.1.bb"
    local recipe_source="$ROOT_PROJECT_PATH/yocto_files/$LAYER_NAME/$NTB_IDT_SISCI_APP_RECIPE_DIR/$recipe_file"
    local source_dir="$ROOT_PROJECT_PATH/kernel_src/ntb-idt-sisci-app"
    local recipe_dest_dir="$layer_path/$NTB_IDT_SISCI_APP_RECIPE_DIR"
    local dest_dir="$layer_path/$NTB_IDT_SISCI_APP_RECIPE_FILES_DIR"

    if [ ! -f "$recipe_source" ]; then
        echo "Missing ntb_idt_sisci_app recipe: $recipe_source" >&2
        exit 1
    fi

    for source_file in Makefile COPYING ntb-idt-sisci-app.c; do
        if [ ! -f "$source_dir/$source_file" ]; then
            echo "Missing ntb_idt_sisci_app source: $source_dir/$source_file" >&2
            exit 1
        fi
    done

    mkdir -p "$recipe_dest_dir"
    cp "$recipe_source" "$recipe_dest_dir/$recipe_file"
    mkdir -p "$dest_dir"
    cp "$source_dir"/Makefile "$dest_dir/Makefile"
    cp "$source_dir"/COPYING "$dest_dir/COPYING"
    cp "$source_dir"/ntb-idt-sisci-app.c "$dest_dir/ntb-idt-sisci-app.c"
}
