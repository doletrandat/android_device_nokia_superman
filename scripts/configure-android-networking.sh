#!/bin/sh
# Resolve the networking correction without compiling or packaging images.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: sh $0 /path/to/kernel-tree /path/to/new-config-directory" >&2
    exit 2
fi
kernel_tree=$(CDPATH= cd -- "$1" && pwd)
fragment=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/android-networking.config
mkdir -- "$2"
config_dir=$(CDPATH= cd -- "$2" && pwd)
cd "$kernel_tree"
test -f .config
scripts/kconfig/merge_config.sh -m -O "$config_dir" .config "$fragment"
# Use the caller's real cross compiler for deployment configuration checks.
# Existing conf is required: this script never builds even host tools.
: "${CC:?Set CC to the ARM cross compiler}"
: "${LD:?Set LD to the corresponding linker}"
export CC LD
ARCH=arm SRCARCH=arm srctree=. KCONFIG_CONFIG="$config_dir/.config" \
    scripts/kconfig/conf --olddefconfig Kconfig
while IFS= read -r option; do
    case "$option" in
        CONFIG_*)
            if ! grep -qxF "$option" "$config_dir/.config"; then
                echo "Unresolved requirement: $option" >&2
                exit 1
            fi
            ;;
    esac
done < "$fragment"
echo "Resolved configuration: $config_dir/.config"
