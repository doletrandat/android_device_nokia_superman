#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Run synchronously in post-fs, before HAL/core/zygote graphics consumers.
# Never unload/reload MSM or restart the framework after selection.
PATH=/system/bin:/vendor/bin
export PATH
LOG=/dev/superman-gpu-startup.log
log() { echo "SUPERMAN_GPU_STARTUP $*" >> "$LOG"; echo "SUPERMAN_GPU_STARTUP $*" > /dev/kmsg; }
select_stack() {
    # Properties must be unset until this decision; ro.* cannot be changed later.
    for p in ro.hardware.gralloc ro.hardware.egl ro.surface_flinger.default_composition_pixel_format ro.superman.gpu_display; do
        [ -z "$(getprop "$p")" ] || { log "selection_refused property_already_set=$p"; return 1; }
    done
    if [ "$1" = mesa ]; then allocator=superman_drm; format=5; display=true
    else allocator=default; format=1; display=false; fi
    setprop ro.hardware.gralloc "$allocator" &&
    setprop ro.surface_flinger.default_composition_pixel_format "$format" &&
    setprop ro.superman.gpu_display "$display" &&
    setprop ro.hardware.egl "$1" || { log "selection_property_failure no_runtime_retry"; return 1; }
    [ "$(getprop ro.hardware.gralloc)" = "$allocator" ] && [ "$(getprop ro.hardware.egl)" = "$1" ] || return 1
    setprop debug.superman.fb_timing "$display"
    log "selected=$1 gralloc=$allocator sf_format=$format physical_handoff=fbdev_copy"
}
healthy() {
    state=/sys/bus/platform/devices/fdb10000.iommu/superman_diag_state
    [ -r "$state" ] && grep -q '^faulted=0 events=[0-9]* runtime_active=[01] non_secure=1 max_asid=1$' "$state"
}
idle() {
    [ "$(cat /sys/bus/platform/devices/fdb00000.gpu/power/runtime_status)" = suspended ] &&
    [ "$(cat /sys/bus/platform/devices/fdb10000.iommu/power/runtime_status)" = suspended ]
}
preflight() {
    log "stage=identity_and_firmware"
    [ "$(getprop ro.superman.gpu_candidate)" = true ] || return 1
    [ "$(cat /sys/module/firmware_class/parameters/path)" = /vendor/firmware ] || return 1
    [ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = b0ae86ff7f34276d8eaf81959b42649c43cd52ae38352990680abdb4c42fb31e ] || return 1
    [ "$(zcat /proc/config.gz | sha256sum | cut -d ' ' -f 1)" = c58a41e6079dcc8bfa18657f2765a49b1a6819727c40a78debe9ce49e5250970 ] || return 1
    log "stage=component_hashes"
    sha256sum -c /vendor/etc/superman-gpu-startup.sha256 >> "$LOG" 2>&1 || return 1
    # A preloaded GPU has unknown boot-time provenance: do not replace it.
    grep -q '^msm ' /proc/modules && return 1
    healthy || return 1
    for file in drm_client_lib drm_exec gpu-sched; do
        name=$(echo "$file" | tr '-' '_')
        if ! grep -q "^$name " /proc/modules; then
            log "stage=module file=$file"
            insmod "/vendor/lib/modules/gpu-diag/$file.ko" || return 1
        fi
    done
    log "stage=module file=msm render_only=1"
    insmod /vendor/lib/modules/gpu-diag/msm.ko render_only=1 superman_diag=1 || return 1
    [ "$(cat /sys/module/msm/parameters/render_only)" = Y ] || return 1
    n=0
    while [ ! -c /dev/dri/renderD128 ] && [ "$n" -lt 5 ]; do sleep 1; n=$((n+1)); done
    [ -c /dev/dri/renderD128 ] && healthy || return 1
    log "preflight_begin BGRA720x1280 no_framebuffer_post"
    timeout -k 2 20 /vendor/bin/superman-gpu-startup-check --run-once >> "$LOG" 2>&1 || return 1
    grep -q 'stage=PASS_no_scanout_no_fps_claim' "$LOG" && healthy || return 1
    # Runtime PM completion may lag the last buffer/context destruction.
    n=0
    while ! idle; do
        healthy && [ "$n" -lt 10 ] || return 1
        sleep 0.2; n=$((n+1))
    done
    healthy || return 1
    log "preflight_pass"
}
: > "$LOG"
log "begin"
if preflight; then select_stack mesa
else log "preflight_failed software_fallback no_module_unload"; select_stack swiftshader; fi
status=$?
# RAM log survives a failed late data mount; kmsg stage markers are captured too.
if [ -d /data/vendor ]; then
    mkdir -p /data/vendor/superman-gpu-diag
    cp "$LOG" /data/vendor/superman-gpu-diag/startup-$(cat /proc/sys/kernel/random/boot_id).txt
fi
exit "$status"
