# SPDX-License-Identifier: Apache-2.0
# Nokia Lumia 730 (Superman) bring-up configuration.
# The bootloader currently under test is LK2ND's legacy Android image parser.

TARGET_ARCH := arm
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_CPU_VARIANT := krait
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi

TARGET_NO_BOOTLOADER := true
TARGET_NO_RECOVERY := false
TARGET_PREBUILT_KERNEL := device/nokia/superman/prebuilt/zImage-dtb

# LK2ND overrides these addresses at load time, but keeping the image header
# aligned with the verified LK contract makes the artifact self-describing.
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_OFFSET := 0x00008000
BOARD_RAMDISK_OFFSET := 0x02000000
BOARD_KERNEL_TAGS_OFFSET := 0x01e00000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_BOOTIMG_HEADER_VERSION := 0
BOARD_MKBOOTIMG_ARGS := --header_version 0
BOARD_MKBOOTIMG_ARGS += --kernel_offset $(BOARD_KERNEL_OFFSET)
BOARD_MKBOOTIMG_ARGS += --ramdisk_offset $(BOARD_RAMDISK_OFFSET)
BOARD_MKBOOTIMG_ARGS += --tags_offset $(BOARD_KERNEL_TAGS_OFFSET)

BOARD_KERNEL_CMDLINE := init=/init androidboot.hardware=superman
BOARD_KERNEL_CMDLINE += androidboot.selinux=permissive printk.devkmsg=on

BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 67108864
BOARD_FLASH_BLOCK_SIZE := 2048
TARGET_RECOVERY_FSTAB := device/nokia/superman/recovery.fstab

# Legacy, non-dynamic Android layout carried entirely by removable microSD.
# Vendor, product, and system_ext stay folded into system so this first normal
# boot milestone needs only one read-only OS partition and one data partition.
BOARD_BUILD_SYSTEM_ROOT_IMAGE := false
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 1610612736
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_PARTITION_SIZE := 1073741824
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_SPARSE_EXT_DISABLED := true

# The pre-init logger mounts LUMIA_BOOT here. Android first-stage SwitchRoot
# requires the same mount-point directory to exist in the new system root.
BOARD_ROOT_EXTRA_FOLDERS += lumia_sd

# Use AOSP's software/reference hardware path until device-specific HALs exist.
BOARD_USES_GENERIC_AUDIO := true
USE_CAMERA_STUB := true
USE_OPENGL_RENDERER := true
BOARD_USE_LEGACY_UI := true
TARGET_USE_PAN_DISPLAY := true
BOARD_SEPOLICY_DIRS += build/target/board/generic/sepolicy

# Keep authentication disabled for the headless recovery diagnostic image.
ALLOW_ADBD_NO_AUTH := 1
