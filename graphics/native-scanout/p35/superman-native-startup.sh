#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# P35 headless boot profile; CC10 adds the native msm KMS phase (contract phase kms).
PATH=/system/bin:/vendor/bin
export PATH
LOG=/dev/superman-native-startup.log
CONTRACT=/vendor/etc/superman-native-boot.contract
MANIFEST=/vendor/etc/superman-native-components.sha256
log() { echo "SUPERMAN_NATIVE_STARTUP $*" >> "$LOG"; echo "SUPERMAN_NATIVE_STARTUP $*" > /dev/kmsg; }
fail() { log "FAILED stage=$1 headless_hold=1 no_unload_no_fallback"; setprop vendor.superman.native_result failed; return 1; }
empty_consumers() {
    for n in surfaceflinger zygote zygote_secondary vendor.hwcomposer-2-1 vendor.gralloc-2-0 bootanim; do
        state=$(getprop "init.svc.$n")
        case "$state" in running|restarting) return 1;; esac
    done
    for n in surfaceflinger app_process app_process32 bootanimation android.hardware.graphics.composer@2.1-service android.hardware.graphics.allocator@2.0-service; do
        pidof "$n" >/dev/null 2>&1 && return 1
    done
    return 0
}
healthy() {
    grep -q '^faulted=0 events=[0-9]* runtime_active=[01] non_secure=1 max_asid=1$' /sys/bus/platform/devices/fdb10000.iommu/superman_diag_state
}
select_stack() {
    setprop hwc.drm.device "$devices" || { fail drm_property; return 1; }
    for pair in ro.hardware.gralloc=superman_native ro.hardware.hwcomposer=superman_native ro.hardware.egl=mesa ro.superman.native_scanout=true ro.superman.gpu_display=true ro.surface_flinger.default_composition_pixel_format=5; do
        key=${pair%%=*}; value=${pair#*=}
        setprop "$key" "$value" && [ "$(getprop "$key")" = "$value" ] || { fail selection; return 1; }
    done
}
run_profile() {
    [ "$(getprop ro.superman.native_profile)" = p35 ] &&
    [ "$(getprop ro.superman.native_candidate)" = p34 ] || { fail profile; return 1; }
    # Exactly two tab/space-separated fields: boot SHA and probe/flip/gpu phase.
    [ -r "$CONTRACT" ] || { fail unsealed; return 1; }
    read -r boot_sha phase extra < "$CONTRACT"
    [ -z "$extra" ] && [ "${#boot_sha}" = 64 ] || { fail contract; return 1; }
    case "$boot_sha" in *[!0-9a-f]*) fail contract; return 1;; esac
    case "$phase" in probe|flip|gpu|hwc|kms) ;; *) fail phase; return 1;; esac
    # CC10: the kms phase keeps the CC4b command line (LK repeats its cached
    # copy first, so a changed stage token always conflicts). The sealed
    # contract alone selects native msm KMS instead of the firmware-KMS lab.
    stage=$phase
    [ "$phase" = kms ] && stage=hwc
    # The P36 phone capture repeats the complete command line. Repeated exact
    # values are equivalent; any conflicting or malformed native value fails.
    flag=0; phase_flag=0
    for token in $(cat /proc/cmdline); do
        case "$token" in
            androidboot.superman_native|androidboot.superman_native=*)
                [ "$token" = androidboot.superman_native=p34 ] || { fail native_flag_mismatch; return 1; }
                flag=$((flag+1));;
            androidboot.superman_native_stage|androidboot.superman_native_stage=*)
                [ "$token" = "androidboot.superman_native_stage=$stage" ] || { fail phase_mismatch; return 1; }
                phase_flag=$((phase_flag+1));;
        esac
    done
    [ "$flag" -ge 1 ] && [ "$phase_flag" -ge 1 ] || { fail boot_flags; return 1; }
    log "boot_flags candidate_count=$flag stage_count=$phase_flag exact_values=1"
    [ "$(getprop ro.superman.native_stage)" = "$stage" ] || { fail stage_property; return 1; }
    [ "$(od -An -tx1 /proc/device-tree/compatible | tr -d ' \n')" = 6e6f6b69612c73757065726d616e0071636f6d2c6d736d3832323600 ] || { fail board; return 1; }
    empty_consumers || { fail consumers; return 1; }
    # Immutable stack choices must still be unset. Partial selection cannot retry.
    for p in ro.hardware.gralloc ro.hardware.hwcomposer ro.hardware.egl ro.superman.native_scanout ro.superman.gpu_display ro.surface_flinger.default_composition_pixel_format; do
        [ -z "$(getprop "$p")" ] || { fail properties_already_set; return 1; }
    done
    [ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_sha" ] || { fail boot_hash; return 1; }
    [ "$(zcat /proc/config.gz | sha256sum | cut -d ' ' -f 1)" = 76cf12182ed0bea29aa7eb0916766b0b4f5715e9fb41dfed53c962d86142af73 ] || { fail config_hash; return 1; }
    sha256sum -c "$MANIFEST" >> "$LOG" 2>&1 || { fail component_hashes; return 1; }
    grep -Eq '^(msm|superman_fw_kms|superman_acquire_gate) ' /proc/modules && { fail preloaded_owner; return 1; }
    grep -Eq '^(drm_client_lib|drm_exec|gpu_sched) ' /proc/modules && { fail preloaded_dependency; return 1; }
    [ "$(cat /sys/module/firmware_class/parameters/path)" = /vendor/firmware ] && healthy || { fail firmware_iommu; return 1; }
    for file in drm_client_lib drm_exec gpu-sched; do
        name=$(echo "$file" | tr '-' '_')
        grep -q "^$name " /proc/modules && { fail preloaded_dependency; return 1; }
        log "module=$file"
        insmod "/vendor/lib/modules/native-p34/$file.ko" || { fail dependency_load; return 1; }
    done
    if [ "$phase" = kms ]; then kms_profile; return $?; fi
    log "module=msm render_only=1 vram=16m"
    insmod /vendor/lib/modules/native-p34/msm.ko render_only=1 superman_diag=1 vram=16m || { fail msm_load; return 1; }
    [ "$(cat /sys/module/msm/parameters/render_only)" = Y ] && healthy || { fail msm_state; return 1; }
    n=0
    while [ ! -c /dev/dri/renderD128 ] && [ "$n" -lt 5 ]; do sleep 1; n=$((n+1)); done
    [ -c /dev/dri/renderD128 ] && empty_consumers || { fail render_node; return 1; }
    log "module=superman-fw-kms warm_handoff_irq_writes=1"
    insmod /vendor/lib/modules/native-p34/superman-fw-kms.ko allow_handoff=1 || { fail kms_load; return 1; }
    # No timeout-kill of a process inside a blocking KMS commit. Kernel helper
    # timeouts may take seconds; failures retain target backing until reboot.
    /vendor/bin/superman-native-kms-check --probe >> "$LOG" 2>&1
    probe_status=$?
    log "probe_status exit=$probe_status"
    [ "$probe_status" -eq 0 ] || { fail probe; return 1; }
    devices=$(sed -n 's/^NATIVE_KMS device=\(\/dev\/dri\/card[0-7]\) .*/\1/p' "$LOG" | sort -u)
    case "$devices" in /dev/dri/card[0-7]) ;; *) fail discovered_device; return 1;; esac
    if [ "$phase" = flip ]; then
        log "CPU_flip_test_begin GPU_producer_test=false"
        /vendor/bin/superman-native-kms-check --flip-test >> "$LOG" 2>&1
        flip_status=$?
        log "flip_status exit=$flip_status"
        [ "$flip_status" -eq 0 ] || { fail flip_restore; return 1; }
        grep -q '^NATIVE_TEST result=0 original_restore=confirmed producer=CPU GPU_producer_test=false$' "$LOG" || { fail restore_record; return 1; }
    fi
    if [ "$phase" = gpu ] || [ "$phase" = hwc ]; then
        log "module=superman-acquire-gate software_fence_only=1 one_shot_delay_ms=500"
        insmod /vendor/lib/modules/native-p34/superman-acquire-gate.ko allow_gate=1 || { fail acquire_gate_load; return 1; }
        # Android mounts /dev as tmpfs; ueventd creates this misc node after
        # module registration. Wait without creating a node or reloading it.
        gate_wait=0
        while [ ! -c /dev/superman-acquire-gate ] && [ "$gate_wait" -lt 5 ]; do
            empty_consumers || { fail acquire_gate_consumers; return 1; }
            healthy || { fail acquire_gate_iommu; return 1; }
            sleep 1
            gate_wait=$((gate_wait+1))
        done
        [ -c /dev/superman-acquire-gate ] || { fail acquire_gate_node; return 1; }
        empty_consumers || { fail acquire_gate_consumers; return 1; }
        healthy || { fail acquire_gate_iommu; return 1; }
        log "acquire_gate_ready waited_seconds=$gate_wait"
        if [ "$phase" = hwc ]; then
            select_stack || return 1
            log "HWC_test_begin gralloc_client_targets=1 explicit_acquire=1 UI_held=1"
            /vendor/bin/superman-native-kms-check --hwc-test >> "$LOG" 2>&1
            hwc_status=$?
            log "hwc_status exit=$hwc_status"
            [ "$hwc_status" -eq 0 ] || { fail hwc_client_target_restore; return 1; }
            grep -q '^NATIVE_HWC_TEST result=0 presents=6 retired=6 original_restore=confirmed delayed_gate=passed UI_held=true$' "$LOG" || { fail hwc_restore_record; return 1; }
            [ "$(getprop ro.superman.native_ui_candidate)" = p69 ] || { fail ui_candidate; return 1; }
            healthy && empty_consumers || { fail churn_initial_state; return 1; }
            log "HWC_churn_begin distinct_targets=6 physical_pin_bound=3 apps_held=1"
            /vendor/bin/superman-native-kms-check --hwc-churn-test >> "$LOG" 2>&1
            churn_status=$?
            log "HWC_churn_status exit=$churn_status"
            [ "$churn_status" -eq 0 ] &&
            grep -q '^NATIVE_HWC_CHURN result=0 targets=6 presents=12 retired=12 original_restore=confirmed explicit_fences=true UI_held=true$' "$LOG" || { fail churn_restore; return 1; }
            grep -q '^NATIVE_HWC_CHURN_LIFETIME distinct_backings=6 peak_resident=3 retired_releases=3 presents=12$' "$LOG" || { fail churn_lifetime; return 1; }
            healthy && empty_consumers || { fail churn_final_state; return 1; }
            setprop vendor.superman.native_churn_result passed || { fail churn_property; return 1; }
            # Restoration leaves firmware scanout running. Validate actual DRM
            # vblank again before the new HAL/SF consumers can start; this probe
            # allocates no targets and does not rerun the six-frame test.
            /vendor/bin/superman-native-kms-check --probe > /dev/superman-native-post-restore.log 2>&1
            post_restore_status=$?
            cat /dev/superman-native-post-restore.log >> "$LOG"
            log "post_restore_probe_status exit=$post_restore_status"
            [ "$post_restore_status" -eq 0 ] || { fail post_restore_vblank; return 1; }
            post_restore_devices=$(sed -n 's/^NATIVE_KMS device=\([^ ]*\) .*/\1/p' /dev/superman-native-post-restore.log)
            [ "$post_restore_devices" = "$devices" ] || { fail post_restore_device; return 1; }
            log "post_restore_vblank_passed device=$devices original_restore=confirmed"
        else
        log "GPU_test_begin explicit_acquire=1 software_delay_is_not_GPU_latency=1"
        /vendor/bin/superman-native-kms-check --gpu-test >> "$LOG" 2>&1
        gpu_status=$?
        log "gpu_status exit=$gpu_status"
        [ "$gpu_status" -eq 0 ] || { fail gpu_acquire_restore; return 1; }
        grep -q '^NATIVE_GPU_TEST result=0 original_restore=confirmed producer=GPU explicit_acquire=true delayed_gate=passed UI_held=true$' "$LOG" || { fail gpu_restore_record; return 1; }
        fi
    fi
    healthy && empty_consumers || { fail final_state; return 1; }
    if [ "$phase" != hwc ]; then select_stack || return 1; fi
    setprop vendor.superman.native_result "$phase-pass" || return 1
    log "PASS phase=$phase device=$devices headless_hold=1 SF_UI_validation=false"
}
# CC10 native display: mainline msm owns MDP5/DSI (Burton panel, PHY/PLL quirk)
# and drm_hwcomposer scans out GPU-composed client targets from VRAM. No
# firmware-KMS handoff and no CPU frame copy.
kms_profile() {
    log "module=msm render_only=0 vram=32m kms=1"
    insmod /vendor/lib/modules/native-kms/msm.ko render_only=0 superman_diag=1 vram=32m || { fail kms_msm_load; return 1; }
    [ "$(cat /sys/module/msm/parameters/render_only)" = N ] && healthy || { fail kms_msm_state; return 1; }
    log "module=panel-nokia-burton"
    insmod /vendor/lib/modules/native-kms/panel-nokia-burton.ko || { fail kms_panel_load; return 1; }
    n=0
    while [ "$(cat /sys/class/drm/card0-DSI-1/status 2>/dev/null)" != connected ] && [ "$n" -lt 15 ]; do sleep 1; n=$((n+1)); done
    [ -c /dev/dri/card0 ] && [ -c /dev/dri/renderD128 ] &&
    [ "$(cat /sys/class/drm/card0-DSI-1/status 2>/dev/null)" = connected ] || { fail kms_connector; return 1; }
    log "kms_connector connected waited_seconds=$n"
    healthy && empty_consumers || { fail kms_state; return 1; }
    setprop hwc.drm.device /dev/dri/card0 || { fail kms_drm_property; return 1; }
    for pair in ro.hardware.gralloc=superman_native ro.hardware.hwcomposer=drm ro.hardware.egl=mesa ro.superman.native_scanout=true ro.superman.gpu_display=true ro.surface_flinger.default_composition_pixel_format=5 hwc.drm.exclude_non_hwfb_imports=1 hwc.drm.use_overlay_planes=0; do
        key=${pair%%=*}; value=${pair#*=}
        setprop "$key" "$value" && [ "$(getprop "$key")" = "$value" ] || { fail kms_selection; return 1; }
    done
    setprop vendor.superman.native_result kms-pass || return 1
    log "PASS phase=kms device=/dev/dri/card0 hwcomposer=drm cpu_copy=0"
}
# CC10: start graphics HALs and SF on the native KMS stack, then release apps.
kms_ui() {
    LOG=/dev/superman-native-kms-ui.log
    : > "$LOG"
    [ "$(getprop vendor.superman.native_result)" = kms-pass ] &&
    [ "$(getprop ro.hardware.hwcomposer)" = drm ] &&
    [ -z "$(getprop vendor.superman.native_kms_release)" ] || { fail kms_ui_gate; return 1; }
    read -r boot_sha phase extra < "$CONTRACT"
    [ -z "$extra" ] && [ "$phase" = kms ] &&
    [ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_sha" ] || { fail kms_ui_boot; return 1; }
    start vendor.gralloc-2-0 && start vendor.hwcomposer-2-1 || { fail kms_hal_start; return 1; }
    n=0
    while [ "$n" -lt 15 ]; do
        lshal -i --neat --types=binderized > /dev/superman-native-hal-list.txt 2>> "$LOG"
        if grep -qx 'android.hardware.graphics.allocator@2.0::IAllocator/default' /dev/superman-native-hal-list.txt &&
           grep -qx 'android.hardware.graphics.composer@2.1::IComposer/default' /dev/superman-native-hal-list.txt; then break; fi
        sleep 1; n=$((n+1))
    done
    [ "$n" -lt 15 ] || { fail kms_hal_registration; return 1; }
    log "HAL_registered polls=$n"
    start surfaceflinger || { fail kms_sf_start; return 1; }
    n=0
    while [ "$(getprop init.svc.surfaceflinger)" != running ] && [ "$n" -lt 10 ]; do sleep 1; n=$((n+1)); done
    [ "$(getprop init.svc.surfaceflinger)" = running ] || { fail kms_sf_running; return 1; }
    sleep 2
    setprop vendor.superman.native_kms_release 1 || return 1
    log "UI_release_requested sf_running=1 hwcomposer=drm"
}
# Separate fresh-boot service gate. Never repeat the one-shot hardware test or
# reload modules here. Failures keep app processes held and do not kill a task
# that may be inside a blocking KMS commit.
sf_services() {
    LOG=/dev/superman-native-sf.log
    : > "$LOG"
    [ "$(getprop ro.superman.native_sf_candidate)" = p48 ] &&
    [ "$(getprop vendor.superman.native_sf_boot_ready)" = 1 ] &&
    [ "$(getprop vendor.superman.native_result)" = hwc-pass ] &&
    [ "$(getprop ro.superman.native_stage)" = hwc ] || { fail sf_gate; return 1; }
    count=0
    for token in $(cat /proc/cmdline); do
        case "$token" in
            androidboot.superman_native_sf|androidboot.superman_native_sf=*)
                [ "$token" = androidboot.superman_native_sf=p48 ] || { fail sf_ack_mismatch; return 1; }
                count=$((count+1));;
        esac
    done
    [ "$count" -ge 1 ] || { fail sf_ack_missing; return 1; }
    read -r boot_sha phase extra < "$CONTRACT"
    [ -z "$extra" ] && [ "$phase" = hwc ] &&
    [ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_sha" ] || { fail sf_boot_hash; return 1; }
    sha256sum -c "$MANIFEST" >> "$LOG" 2>&1 || { fail sf_components; return 1; }
    healthy && empty_consumers || { fail sf_initial_state; return 1; }
    [ "$(getprop ro.hardware.gralloc)" = superman_native ] &&
    [ "$(getprop ro.hardware.hwcomposer)" = superman_native ] &&
    [ "$(getprop ro.hardware.egl)" = mesa ] &&
    [ "$(getprop ro.surface_flinger.default_composition_pixel_format)" = 5 ] || { fail sf_stack; return 1; }
    start vendor.gralloc-2-0 && start vendor.hwcomposer-2-1 || { fail sf_hal_start; return 1; }
    n=0
    while [ "$n" -lt 10 ]; do
        healthy || { fail sf_hal_iommu; return 1; }
        # Binderized table only: a passthrough reference or VINTF declaration is
        # not sufficient evidence that the actual service registered.
        lshal -i --neat --types=binderized > /dev/superman-native-hal-list.txt 2>> "$LOG"
        if grep -qx 'android.hardware.graphics.allocator@2.0::IAllocator/default' /dev/superman-native-hal-list.txt &&
           grep -qx 'android.hardware.graphics.composer@2.1::IComposer/default' /dev/superman-native-hal-list.txt &&
           [ "$(getprop init.svc.vendor.gralloc-2-0)" = running ] &&
           [ "$(getprop init.svc.vendor.hwcomposer-2-1)" = running ]; then
            log "HAL_registered polls=$n"
            start surfaceflinger || { fail sf_start; return 1; }
            setprop vendor.superman.native_sf_result started
            log "SF_start_requested apps_held=1 bootanim_held=1 hardware_validation=pending"
            return 0
        fi
        sleep 1
        n=$((n+1))
    done
    setprop vendor.superman.native_sf_result hal-timeout
    fail sf_hal_registration
}
# P67 one-shot SF/BufferQueue laboratory. It never starts ordinary apps or
# repeats the lower HWC/acquire-gate test. Failure leaves the owning HAL alive.
framework_held() {
    for sf_app_service in zygote zygote_secondary; do
        case "$(getprop "init.svc.$sf_app_service")" in running|restarting) return 1;; esac
    done
    for sf_app_process in app_process app_process32 bootanimation; do pidof "$sf_app_process" >/dev/null 2>&1 && return 1; done
    return 0
}
apps_held() {
    framework_held || return 1
    case "$(getprop init.svc.bootanim)" in running|restarting) return 1;; esac
    return 0
}
sf_layer_test() {
    LOG=/dev/superman-native-sf-layer.log
    : > "$LOG"
    [ "$(getprop ro.superman.native_sf_layer_candidate)" = p69 ] &&
    [ "$(getprop ro.superman.native_sf_client_target)" = p69 ] &&
    [ "$(getprop vendor.superman.native_sf_result)" = started ] &&
    [ "$(getprop vendor.superman.native_result)" = hwc-pass ] &&
    [ -z "$(getprop vendor.superman.native_sf_layer_result)" ] || { fail sf_layer_gate; return 1; }
    case "$(getprop ro.build.description)" in *v70-graphicsdiag-r37-smp-p78-gpu-sync-20261021*) ;; *) fail sf_layer_identity; return 1;; esac
    read -r boot_sha phase extra < "$CONTRACT"
    [ -z "$extra" ] && [ "$phase" = hwc ] &&
    [ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_sha" ] || { fail sf_layer_boot; return 1; }
    sha256sum -c "$MANIFEST" >> "$LOG" 2>&1 || { fail sf_layer_components; return 1; }
    healthy && framework_held || { fail sf_layer_initial_state; return 1; }
    n=0
    while [ "$n" -lt 20 ]; do
        healthy && framework_held || { fail sf_layer_wait_state; return 1; }
        # SF can briefly start the sealed /system/bin/true bootanim service.
        # Wait for that no-op to exit within the existing readiness budget.
        # A real bootanimation or app process still fails framework_held.
        if apps_held && [ "$(getprop init.svc.surfaceflinger)" = running ] &&
           [ "$(getprop init.svc.vendor.hwcomposer-2-1)" = running ] &&
           [ "$(getprop init.svc.vendor.gralloc-2-0)" = running ]; then
            dumpsys -t 5 SurfaceFlinger > /dev/superman-native-sf-layer-before.txt 2>> "$LOG"
            if grep -q 'GLES: freedreno, FD305' /dev/superman-native-sf-layer-before.txt &&
               grep -q 'mMoreSamplesNeeded=0 mPeriodConfirmationInProgress=0' /dev/superman-native-sf-layer-before.txt &&
               grep -q 'Visible layers (count = 0)' /dev/superman-native-sf-layer-before.txt &&
               grep -q '^NATIVE_HWC_OWNER live=0 latched=0 retired=0 active=0 restore=0$' /dev/superman-native-sf-layer-before.txt; then break; fi
        fi
        sleep 1; n=$((n+1))
    done
    [ "$n" -lt 20 ] || { fail sf_layer_ready_timeout; return 1; }
    setprop vendor.superman.native_sf_layer_result running || return 1
    log "SF_layer_begin one_shot=1 base_frames=20 overlay_frames=12 overlay_cycles=2 apps_held=1 no_forced_process_stop=1"
    /system/bin/superman-native-sf-layer-test >> "$LOG" 2>&1
    test_status=$?
    log "SF_layer_test_status exit=$test_status"
    # A last-layer DestroyLayer performs restoration on the HAL's existing DRM
    # owner. Poll its read-only state; do not open another master or unload it.
    n=0; restored=0
    while [ "$n" -lt 10 ]; do
        dumpsys -t 5 SurfaceFlinger > /dev/superman-native-sf-layer-after.txt 2>> "$LOG"
        line=$(grep '^NATIVE_HWC_OWNER live=[0-9]* latched=[0-9]* retired=[0-9]* active=0 restore=1$' /dev/superman-native-sf-layer-after.txt)
        if [ -n "$line" ]; then
            live=$(echo "$line" | sed 's/.*live=\([0-9]*\).*/\1/')
            latched=$(echo "$line" | sed 's/.*latched=\([0-9]*\).*/\1/')
            retired=$(echo "$line" | sed 's/.*retired=\([0-9]*\).*/\1/')
            if [ "$live" = "$latched" ] && [ "$latched" = "$retired" ] && [ "$latched" -ge 20 ] &&
               grep -q 'Visible layers (count = 0)' /dev/superman-native-sf-layer-after.txt; then restored=1; break; fi
        fi
        sleep 1; n=$((n+1))
    done
    healthy && apps_held || { fail sf_layer_final_state; return 1; }
    [ "$test_status" -eq 0 ] &&
    grep -q '^NATIVE_SF_TEST result=0 presents=20 prior_producer_releases=19 overlay_presents=12 overlay_prior_releases=10 overlay_cycles=2 overlay_cleanup_ok=1 final_producer_release=unobserved removal_status=0 original_restore=must_verify_owner UI_held=true throughput_claim=false$' "$LOG" &&
    [ "$restored" -eq 1 ] || { setprop vendor.superman.native_sf_layer_result failed; fail sf_layer_presentation_restore; return 1; }
    log "SF_layer_owner_restore_passed live=$live latched=$latched retired=$retired active=0 original_restore=confirmed"
    setprop vendor.superman.native_sf_layer_result passed
    log "PASS phase=sf-layer presents=20 prior_producer_releases=19 overlay_presents=12 overlay_cycles=2 apps_held=1 native_UI_validation=false throughput_claim=false"
}
# P67 fresh-boot UI gate. A failed gate never releases zygote. The observation
# stage can stop app processes but never kills the display owner or a KMS task.
ui_fail() {
    log "UI_FAILED stage=$1 display_owner_retained=1 no_unload_no_fallback=1"
    setprop vendor.superman.native_ui_result failed
    if [ "$ui_released" = 1 ]; then
        stop zygote
        stop zygote_secondary
        log "UI_app_stop_requested owner_not_stopped=1 restoration_must_be_observed=1"
    fi
    return 1
}
ui_services_alive() {
    [ "$(getprop init.svc.surfaceflinger)" = running ] &&
    [ "$(getprop init.svc.vendor.hwcomposer-2-1)" = running ] &&
    [ "$(getprop init.svc.vendor.gralloc-2-0)" = running ]
}
native_ui() {
    LOG=/dev/superman-native-ui.log
    : > "$LOG"
    ui_released=0
    ui_boot=$(cat /proc/sys/kernel/random/boot_id)
    [ "$(getprop ro.superman.native_ui_candidate)" = p69 ] &&
    [ "$(getprop ro.superman.native_sf_layer_candidate)" = p69 ] &&
    [ "$(getprop ro.superman.native_sf_client_target)" = p69 ] &&
    [ "$(getprop vendor.superman.native_churn_result)" = passed ] &&
    [ "$(getprop vendor.superman.native_sf_layer_result)" = passed ] &&
    [ "$(getprop vendor.superman.native_sf_result)" = started ] &&
    [ "$(getprop vendor.superman.native_result)" = hwc-pass ] &&
    [ -z "$(getprop vendor.superman.native_ui_result)" ] || { ui_fail gate; return 1; }
    case "$(getprop ro.build.description)" in *v70-graphicsdiag-r37-smp-p78-gpu-sync-20261021*) ;; *) ui_fail identity; return 1;; esac
    [ "$(od -An -tx1 /proc/device-tree/compatible | tr -d ' \n')" = 6e6f6b69612c73757065726d616e0071636f6d2c6d736d3832323600 ] || { ui_fail board; return 1; }
    read -r boot_sha phase extra < "$CONTRACT"
    [ -z "$extra" ] && [ "${#boot_sha}" = 64 ] && [ "$phase" = hwc ] &&
    [ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_sha" ] || { ui_fail boot; return 1; }
    [ "$(zcat /proc/config.gz | sha256sum | cut -d ' ' -f 1)" = 76cf12182ed0bea29aa7eb0916766b0b4f5715e9fb41dfed53c962d86142af73 ] || { ui_fail config; return 1; }
    sha256sum -c "$MANIFEST" >> "$LOG" 2>&1 || { ui_fail components; return 1; }
    healthy && apps_held && ui_services_alive || { ui_fail initial_state; return 1; }
    [ "$(getprop ro.hardware.egl)" = mesa ] &&
    [ "$(getprop ro.hardware.gralloc)" = superman_native ] &&
    [ "$(getprop ro.hardware.hwcomposer)" = superman_native ] || { ui_fail stack; return 1; }
    grep -q '^NATIVE_HWC_CHURN result=0 targets=6 presents=12 retired=12 original_restore=confirmed explicit_fences=true UI_held=true$' /dev/superman-native-startup.log || { ui_fail churn_record; return 1; }
    grep -q '^NATIVE_HWC_CHURN_LIFETIME distinct_backings=6 peak_resident=3 retired_releases=3 presents=12$' /dev/superman-native-startup.log || { ui_fail churn_lifetime; return 1; }
    grep -q '^NATIVE_SF_TEST result=0 presents=20 prior_producer_releases=19 overlay_presents=12 overlay_prior_releases=10 overlay_cycles=2 overlay_cleanup_ok=1 final_producer_release=unobserved removal_status=0 original_restore=must_verify_owner UI_held=true throughput_claim=false$' /dev/superman-native-sf-layer.log || { ui_fail layer_record; return 1; }
    dumpsys -t 5 SurfaceFlinger > /dev/superman-native-ui-before.txt 2>> "$LOG" || { ui_fail owner_dump; return 1; }
    ui_line=$(grep '^NATIVE_HWC_OWNER live=[0-9]* latched=[0-9]* retired=[0-9]* active=0 restore=1$' /dev/superman-native-ui-before.txt)
    [ "$(echo "$ui_line" | wc -l)" -eq 1 ] && [ -n "$ui_line" ] || { ui_fail owner_record; return 1; }
    ui_live=$(echo "$ui_line" | sed 's/.*live=\([0-9]*\).*/\1/')
    ui_latched=$(echo "$ui_line" | sed 's/.*latched=\([0-9]*\).*/\1/')
    ui_retired=$(echo "$ui_line" | sed 's/.*retired=\([0-9]*\).*/\1/')
    [ "$ui_live" = "$ui_latched" ] && [ "$ui_latched" = "$ui_retired" ] && [ "$ui_latched" -ge 20 ] &&
    grep -q 'Visible layers (count = 0)' /dev/superman-native-ui-before.txt &&
    grep -q 'GLES: freedreno, FD305' /dev/superman-native-ui-before.txt || { ui_fail owner_restore; return 1; }
    [ "$(cat /proc/sys/kernel/random/boot_id)" = "$ui_boot" ] || { ui_fail boot_changed; return 1; }
    log "UI_GATES_PASSED boot_id=$ui_boot churn_targets=6 SF_base=20 SF_overlay=12 restored=$ui_latched native_UI_validation=pending"
    setprop vendor.superman.native_ui_result running || { ui_fail admission; return 1; }
    ui_released=1
    setprop vendor.superman.native_ui_release 1 || { ui_fail release; return 1; }
    log "UI_release_requested bootanim_held=1 all_layers_GPU_CLIENT=1 physical_targets=1 physical_pin_bound=3"
    ui_sample=0; ui_completed=0; ui_previous_latch=$ui_latched
    while [ "$ui_sample" -lt 12 ]; do
        sleep 10
        healthy && ui_services_alive && [ "$(cat /proc/sys/kernel/random/boot_id)" = "$ui_boot" ] || { ui_fail observation_health; return 1; }
        dumpsys -t 5 SurfaceFlinger > "/dev/superman-native-ui-sample-$ui_sample.txt" 2>> "$LOG" || { ui_fail observation_dump; return 1; }
        ui_state=$(grep '^NATIVE_HWC_OWNER live=[0-9]* latched=[0-9]* retired=[0-9]* active=[01] restore=[01]$' "/dev/superman-native-ui-sample-$ui_sample.txt")
        [ -n "$ui_state" ] && [ "$(echo "$ui_state" | wc -l)" -eq 1 ] || { ui_fail observation_owner; return 1; }
        ui_current_live=$(echo "$ui_state" | sed 's/.*live=\([0-9]*\).*/\1/')
        ui_current_latched=$(echo "$ui_state" | sed 's/.*latched=\([0-9]*\).*/\1/')
        ui_current_retired=$(echo "$ui_state" | sed 's/.*retired=\([0-9]*\).*/\1/')
        ui_current_active=$(echo "$ui_state" | sed 's/.*active=\([01]\).*/\1/')
        [ "$ui_current_latched" -ge "$ui_previous_latch" ] &&
        [ "$ui_current_live" -ge "$ui_current_latched" ] &&
        [ "$ui_current_live" -le "$((ui_current_latched+1))" ] &&
        [ "$((ui_current_retired+ui_current_active))" -eq "$ui_current_latched" ] || { ui_fail observation_retirement; return 1; }
        ui_previous_latch=$ui_current_latched
        ui_zygote=$(getprop init.svc.zygote)
        [ "$ui_zygote" = running ] || { ui_fail zygote_state; return 1; }
        ui_complete=$(getprop sys.boot_completed)
        if [ "$ui_complete" = 1 ] && pidof system_server >/dev/null 2>&1; then ui_completed=1; fi
        log "UI_SAMPLE index=$ui_sample boot_id=$ui_boot uptime=$(cat /proc/uptime) framework_boot_completed=$ui_complete zygote=$ui_zygote owner=$ui_state"
        ui_sample=$((ui_sample+1))
    done
    [ "$ui_completed" = 1 ] || { ui_fail framework_timeout; return 1; }
    ui_final_live_limit=$ui_current_latched
    if [ "$(getprop ro.superman.async_present)" = true ]; then
        ui_final_live_limit=$((ui_current_latched+1))
    fi
    [ "$ui_current_latched" -gt "$ui_latched" ] &&
    [ "$ui_current_live" -ge "$ui_current_latched" ] &&
    [ "$ui_current_live" -le "$ui_final_live_limit" ] && [ "$ui_current_active" = 1 ] &&
    grep -q 'Visible layers (count = [1-9][0-9]*)' /dev/superman-native-ui-sample-11.txt &&
    grep -q 'GLES: freedreno, FD305' /dev/superman-native-ui-sample-11.txt || { ui_fail UI_presentation_absent; return 1; }
    setprop vendor.superman.native_ui_observation complete
    log "UI_OBSERVATION_COMPLETE samples=12 framework_boot_completed=1 app_UI_pixels_must_be_verified=true rendered_FPS_unmeasured=true charging_gain_unmeasured=true"
}
case "$#:$1" in
    1:--sf-services) sf_services; exit $?;;
    1:--sf-layer) sf_layer_test; exit $?;;
    1:--native-ui) native_ui; exit $?;;
    1:--kms-ui) kms_ui; exit $?;;
    0:) ;;
    *) exit 64;;
esac
: > "$LOG"
log begin
run_profile
status=$?
if [ -d /data/vendor ]; then
    mkdir -p /data/vendor/superman-native-diag
    cp "$LOG" "/data/vendor/superman-native-diag/startup-$(cat /proc/sys/kernel/random/boot_id).txt"
fi
exit "$status"
