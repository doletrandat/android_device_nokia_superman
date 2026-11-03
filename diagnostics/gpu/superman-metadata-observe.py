#!/usr/bin/env python3
"""Bounded P69 metadata observation. Host-arms nonpixel logging; never captures pixels."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import shlex
import shutil
import subprocess
import threading
import time

UUID = re.compile(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}')
IDENTITIES = {'v70-graphicsdiag-r37-smp-p69-metadata-20261011': 'p69'}
CONFIG_SHA = 'c58a41e6079dcc8bfa18657f2765a49b1a6819727c40a78debe9ce49e5250970'
BOARD = b'nokia,superman\0qcom,msm8226\0'
IDENTITY = ('cat /proc/sys/kernel/random/boot_id; cat /proc/uptime; '
            'pidof surfaceflinger; pidof android.hardware.graphics.composer@2.1-service; '
            'getprop ro.build.description; getprop ro.superman.native_ui_candidate; '
            'getprop ro.superman.native_sf_pair_trace; getprop ro.superman.native_target_trace; '
            'getprop debug.superman.target_trace; getprop vendor.superman.native_ui_result; '
            'getprop ro.superman.native_metadata_trace; getprop debug.superman.metadata_trace')
PROPS = ['ro.hardware.egl', 'ro.hardware.gralloc', 'ro.hardware.hwcomposer',
         'ro.superman.native_scanout', 'ro.superman.gpu_display',
         'ro.surface_flinger.default_composition_pixel_format', 'ro.superman.bgra_window_configs',
         'ro.superman.native_sf_layer_candidate', 'ro.superman.native_sf_client_target',
         'debug.hwui.renderer', 'debug.hwui.use_partial_updates', 'debug.hwui.use_buffer_age',
         'debug.hwui.use_gpu_pixel_buffers', 'debug.hwui.skip_empty_damage',
         'debug.hwui.show_dirty_regions', 'debug.hwui.show_layers_updates', 'debug.hwui.overdraw',
         'debug.hwui.render_ahead', 'debug.sf.showupdates', 'debug.sf.disable_backpressure',
         'debug.sf.enable_gl_backpressure', 'debug.sf.disable_client_composition_cache',
         'debug.superman.cpu_fb_copy', 'debug.superman.swiftshader_fast_jit',
         'ro.superman.gpu_candidate', 'ro.boot.console']
PROPERTY_QUERY = '; '.join('printf ' + shlex.quote(p + '=') + '; getprop ' + p for p in PROPS)
ENV_QUERY = ("for n in surfaceflinger android.hardware.graphics.composer@2.1-service zygote; do "
             "for p in $(pidof $n); do case $p in ''|*[!0-9]*) exit 71;; esac; "
             "printf 'PROCESS %s %s\\n' \"$n\" \"$p\"; "
             "tr '\\000' '\\n' < /proc/$p/environ | "
             "sed -n '/^FD_MESA_DEBUG=/p; /^MESA_DEBUG=/p; /^LIBGL_DEBUG=/p; "
             "/^EGL_LOG_LEVEL=/p; /^IR3_SHADER_DEBUG=/p; /^GALLIUM_DRIVER=/p; "
             "/^MESA_LOADER_DRIVER_OVERRIDE=/p; /^MESA_GL_VERSION_OVERRIDE=/p; "
             "/^MESA_GLES_VERSION_OVERRIDE=/p; /^MESA_EXTENSION_OVERRIDE=/p'; done; done")
STATE_QUERY = ("date +%s; cat /sys/bus/platform/devices/fdb10000.iommu/superman_diag_state; "
               "getprop debug.hwui.use_partial_updates; getprop debug.hwui.use_buffer_age; "
               "getprop debug.hwui.renderer; "
               "pidof system_server com.android.systemui com.android.launcher3 com.android.settings")
BUFFERS = {'main': 32 * 1024**2, 'system': 4 * 1024**2,
           'crash': 2 * 1024**2, 'kernel': 4 * 1024**2}


def linux_path(path):
    """Check actual mount before reading/writing; do not access NTFS targets."""
    resolved = path.resolve(strict=True)
    q = subprocess.run(['findmnt', '-T', str(resolved), '-n', '-o', 'FSTYPE'],
                       capture_output=True, text=True, timeout=3)
    if q.returncode or q.stdout.strip() not in ['ext2', 'ext3', 'ext4', 'btrfs', 'xfs', 'tmpfs']:
        raise ValueError('Supported Linux filesystem required; NTFS/unknown mount refused')
    return resolved


def parse_identity(data, package):
    s = data.decode('ascii').splitlines()
    kind = IDENTITIES.get(package['identity'])
    if (not kind or len(s) != 12 or not UUID.fullmatch(s[0]) or
            not re.fullmatch(r'\d+\.\d+ \d+\.\d+', s[1]) or
            not all(re.fullmatch(r'[1-9]\d*', v) for v in s[2:4]) or s[2] == s[3]):
        raise ValueError('Invalid supported identity/boot/SF/HWC/uptime')
    expected = 'aosp_superman-userdebug 11 RD2A.211001.002 ' + package['identity'] + ' test-keys'
    if (s[4] != expected or s[5] != kind or s[9] != 'running' or
            s[6:8] != ['', ''] or s[8] or s[10] != 'p69' or
            (s[11] and not re.fullmatch(r'm69-[0-9a-f]{16}', s[11]))):
        raise ValueError('Image/selector/UI mismatch or active pixel capture; baseline refused')
    return {'boot': s[0], 'uptime': s[1], 'sf_pid': s[2], 'hwc_pid': s[3],
            'description': s[4], 'candidate': kind, 'pixel_arm': s[8], 'metadata_selector': s[10], 'metadata_arm': s[11]}


def same_process(a, b):
    return all(a[k] == b[k] for k in ['boot', 'sf_pid', 'hwc_pid', 'description', 'candidate'])


def audit_properties(data, candidate):
    rows = {}
    for line in data.decode().splitlines():
        key, sep, value = line.partition('=')
        if not sep or key not in PROPS or key in rows:
            raise ValueError('Malformed/duplicate graphics property')
        rows[key] = value
    if set(rows) != set(PROPS):
        raise ValueError('Missing graphics properties')
    expected = {'ro.hardware.egl':'mesa', 'ro.hardware.gralloc':'superman_native',
        'ro.hardware.hwcomposer':'superman_native', 'ro.superman.native_scanout':'true',
        'ro.superman.gpu_display':'true', 'ro.surface_flinger.default_composition_pixel_format':'5',
        'ro.superman.bgra_window_configs':'1',
        'ro.superman.native_sf_layer_candidate':candidate, 'ro.superman.native_sf_client_target':candidate}
    mismatches = {k:{'actual':rows[k], 'expected':v} for k,v in expected.items() if rows[k] != v}
    overrides = {k:v for k,v in rows.items() if k.startswith(('debug.hwui.', 'debug.sf.')) and v}
    return {'native_pipeline_mismatches': mismatches, 'explicit_graphics_overrides': overrides,
        'values': rows, 'unset_HWUI_partial_updates_default': True,
        'unset_HWUI_buffer_age_default': True, 'overrides_are_not_automatically_wrong': True}


def stream_args(buffer):
    if buffer not in BUFFERS:
        raise ValueError('Unknown log buffer')
    return ['adb', '-s', 'superman', 'exec-out', 'su', '0', 'logcat', '-b', buffer,
            '-v', 'threadtime', '-v', 'monotonic', '-T', '1', '*:V']


def append_marker(run, kind):
    """Host timestamp only. Never calls ADB or reads/maps graphics buffers."""
    if kind not in ['missing', 'tearing', 'returned', 'normal']:
        raise ValueError('Unknown observation')
    if run.is_symlink() or not run.is_dir():
        raise ValueError('Unsafe run directory')
    linux_path(run)
    if (run / 'binding.json').is_symlink():
        raise ValueError('Unsafe binding file')
    linux_path(run / 'binding.json')
    linux_path(run / 'observations.jsonl')
    binding = json.loads((run / 'binding.json').read_text())
    record = {'kind': kind, 'host_monotonic_ns': time.monotonic_ns(),
              'host_unix_ns': time.time_ns(), 'boot': binding['boot'],
              'user_observation': True, 'physical_frame_synchronized': False,
              'phone_request': False}
    fd = os.open(run / 'observations.jsonl', os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW)
    try:
        data = (json.dumps(record) + '\n').encode()
        if os.write(fd, data) != len(data):
            raise OSError('Short marker append')
    finally:
        os.close(fd)
    return record


class Stream:
    def __init__(self, root, buffer, stop, launch=subprocess.Popen):
        self.root, self.buffer, self.stop, self.launch = root, buffer, stop, launch
        self.result = {'buffer': buffer, 'bytes': 0, 'stderr_bytes': 0, 'complete': False}

    def run(self):
        p = None; sel = None
        try:
            p = self.launch(stream_args(self.buffer), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            sel = selectors.DefaultSelector()
            sel.register(p.stdout, selectors.EVENT_READ, 'out')
            sel.register(p.stderr, selectors.EVENT_READ, 'err')
            with (self.root / ('logcat-' + self.buffer + '.txt')).open('xb') as out, \
                    (self.root / ('logcat-' + self.buffer + '.stderr')).open('xb') as err:
                while not self.stop.is_set() and sel.get_map():
                    for key, _ in sel.select(0.2):
                        data = os.read(key.fileobj.fileno(), 65536)
                        if not data:
                            sel.unregister(key.fileobj)
                            continue
                        target, field, limit = ((out, 'bytes', BUFFERS[self.buffer]) if key.data == 'out'
                                                else (err, 'stderr_bytes', 65536))
                        remaining = limit - self.result[field]
                        chunk = data[:remaining]
                        target.write(chunk); self.result[field] += len(chunk)
                        if len(data) > remaining or self.result[field] == limit:
                            self.result['cap_reached'] = field
                            raise RuntimeError('Host stream byte cap reached; original prefix preserved')
                    if shutil.disk_usage(self.root).free < 64 * 1024**2:
                        raise RuntimeError('Host reserve below 64 MiB')
                self.result['host_stop'] = self.stop.is_set()
                self.result['complete'] = self.stop.is_set()
        except Exception as e:
            self.result['error'] = str(e)
        finally:
            if sel is not None:
                sel.close()
            if p is not None:
                if p.poll() is None:
                    p.terminate()
                    try: p.wait(timeout=2)
                    except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=2)
                self.result['client_exit'] = p.returncode
                self.result['client_terminated_by_host'] = self.stop.is_set() or 'error' in self.result
                for f in [p.stdout, p.stderr]:
                    if f: f.close()
            (self.root / ('stream-' + self.buffer + '.json')).write_text(json.dumps(self.result, indent=2) + '\n')


class Observer:
    def __init__(self, output, artifact, seconds=120, execute=None):
        if not 30 <= seconds <= 180:
            raise ValueError('Duration must be30..180 seconds')
        if not output.is_dir():
            raise ValueError('Existing output directory required')
        linux_path(output)
        linux_path(artifact / 'package-inputs.json')
        linux_path(artifact / 'extra-component-manifest.json')
        self.package = json.loads((artifact / 'package-inputs.json').read_text())
        self.manifest = json.loads((artifact / 'extra-component-manifest.json').read_text())
        if self.package['identity'] not in IDENTITIES or self.manifest['identity'] != self.package['identity']:
            raise ValueError('Explicit supported exact package required')
        self.seconds, self.execute = seconds, execute or self.adb
        self.out = output / ('systemwide-' + time.strftime('%Y%m%d-%H%M%S') + '-' + secrets.token_hex(4))
        self.out.mkdir(parents=True, exist_ok=False)
        self.commands = []; self.binding = None; self.trace_token = ""; self.arm_attempted = False
        (self.out / 'observations.jsonl').write_text('')
        (self.out / 'plan.json').write_text(json.dumps({'package': str(artifact), 'identity': self.package['identity'],
            'requested_seconds': seconds, 'sample_period_seconds': 15, 'stream_caps': BUFFERS,
            'pixels': False, 'phone_properties_changed': ['debug.superman.metadata_trace'], 'tracefs_changed': False,
            'GPU_debugfs_MMIO_read': False, 'phone_storage_written': False,
            'all_applications': True, 'command_duration_measured': True,
            'baseline_context_snapshots_outside_observation': True,
            'settle_seconds_after_preflight': 5,
            'existing_logs_plus_bounded_metadata': True, 'one_shot_per_process': True, 'overhead_unmeasured_until_run': True}, indent=2) + '\n')

    @staticmethod
    def adb(cmd, timeout):
        return subprocess.run(['adb', '-s', 'superman', 'exec-out', 'su', '0', 'sh', '-c', cmd],
                              capture_output=True, timeout=timeout)

    def read(self, name, cmd, timeout=12, required=True):
        begin = time.monotonic_ns()
        try:
            q = self.execute(cmd, timeout)
        except subprocess.TimeoutExpired as e:
            q = subprocess.CompletedProcess([], 124, e.stdout or b'', (e.stderr or b'') + b'HOST_TIMEOUT\n')
        self.commands.append({'file': name, 'command': cmd, 'exit': q.returncode,
            'host_begin_ns': begin, 'host_end_ns': time.monotonic_ns(), 'bytes': len(q.stdout)})
        # Known finite metadata sources only. Preserve a bounded prefix of unexpected output.
        data = q.stdout[:8 * 1024**2]; (self.out / name).write_bytes(data)
        (self.out / (name + '.stderr')).write_bytes(q.stderr[:65536])
        (self.out / 'commands.json').write_text(json.dumps(self.commands, indent=2) + '\n')
        if len(q.stdout) > len(data):
            raise RuntimeError('Metadata cap exceeded; prefix retained')
        if required and q.returncode:
            raise RuntimeError(name + ': read failed; evidence retained')
        return data

    def identity(self, label):
        value = parse_identity(self.read(label, IDENTITY), self.package)
        if value['metadata_arm'] != self.trace_token:
            raise RuntimeError('Metadata arm differs from this collector; stop session')
        if self.binding and not same_process(self.binding, value):
            raise RuntimeError('Boot/SF/HWC/image changed; stop this session')
        return value

    def arm_metadata(self):
        self.identity('identity-before-arm.txt')
        self.trace_token = 'm69-' + secrets.token_hex(8)
        self.arm_attempted = True
        self.read('metadata-arm.txt', 'setprop debug.superman.metadata_trace ' + self.trace_token +
                  '; [ "$(getprop debug.superman.metadata_trace)" = ' + self.trace_token + ' ]')
        self.identity('identity-armed.txt')

    def disarm_metadata(self):
        # Clear only this collector's token on the originally bound boot.
        # Do not overwrite a different token or act on a new boot after reconnect.
        if not self.arm_attempted or not self.binding:
            return {'attempted': False, 'cleared': False}
        token = self.trace_token
        command = ('[ "$(cat /proc/sys/kernel/random/boot_id)" = ' + self.binding['boot'] +
                   ' ] && [ "$(getprop debug.superman.metadata_trace)" = ' + token +
                   ' ] && setprop debug.superman.metadata_trace "" && '
                   '[ -z "$(getprop debug.superman.metadata_trace)" ]')
        data = self.read('metadata-disarm.txt', command)
        self.trace_token = ''
        return {'attempted': True, 'cleared': True, 'token': token,
                'scope': 'own nonpixel diagnostic arm only; no GPU/UI/pixel settings'}

    def preflight(self):
        if shutil.disk_usage(self.out).free < 128 * 1024**2:
            raise RuntimeError('Host reserve below 128 MiB')
        self.binding = self.identity('identity-preflight.txt')
        (self.out / 'binding.json').write_text(json.dumps(self.binding, indent=2) + '\n')
        if self.read('board.bin', 'cat /proc/device-tree/compatible') != BOARD:
            raise RuntimeError('Exact Nokia730/MSM8226 board mismatch')
        files = self.manifest['files']
        if len(files) != 143 or len({x['image_path'] for x in files}) != 143:
            raise RuntimeError('143unique package components required')
        hashes = self.read('selected.txt', 'sha256sum ' + ' '.join(shlex.quote('/' + x['image_path']) for x in files), 35)
        rows = {p:h for h,p in re.findall(r'^([0-9a-f]{64})\s+(/\S+)$', hashes.decode(), re.M)}
        if len(rows) != 143 or any(rows.get('/' + x['image_path']) != x['sha256'] for x in files):
            raise RuntimeError('Installed package components mismatch')
        native = self.read('native.txt', 'sha256sum -c /vendor/etc/superman-native-components.sha256', 30).decode().splitlines()
        if len(native) != 32 or any(not v.endswith(': OK') for v in native):
            raise RuntimeError('Native manifest mismatch')
        boot = self.read('SD-boot.txt', 'sha256sum /lumia_sd/boot.img', 30).decode().split()
        if not boot or boot[0] != self.package['boot_sha256']:
            raise RuntimeError('Actual SD boot mismatch')
        config = self.read('config.gz', 'cat /proc/config.gz')
        if hashlib.sha256(gzip.decompress(config)).hexdigest() != CONFIG_SHA:
            raise RuntimeError('Captured config mismatch')
        props = self.read('properties-before.txt', PROPERTY_QUERY)
        audit = audit_properties(props, self.binding['candidate'])
        (self.out / 'graphics-config-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
        if audit['native_pipeline_mismatches']:
            raise RuntimeError('Active native pipeline differs from image startup contract; audit retained')
        self.read('GPU-env-before.txt', ENV_QUERY, required=False)
        self.read('cmdline.txt', 'cat /proc/cmdline')
        # These read-only dumps can still add load; run before the timed baseline.
        self.read('SF-before.txt', 'dumpsys -t 5 SurfaceFlinger', required=False)
        self.read('activity-before.txt', 'dumpsys -t 5 activity top', required=False)
        self.read('processes-before.txt', 'ps -A -o PID,PPID,NAME', required=False)
        self.read('crash-filenames-before.txt', 'ls -l /data/anr /data/tombstones', required=False)
        self.read('kernel-before.txt', 'dmesg', required=False)
        self.read('state-before.txt', STATE_QUERY, required=False)
        self.read('startup-gates.txt', 'cat /dev/superman-native-startup.log /dev/superman-native-sf-layer.log /dev/superman-native-ui.log', required=False)
        self.identity('identity-ready.txt')
        (self.out / 'preflight.json').write_text(json.dumps({'passed': True, 'boot': self.binding['boot'],
            'installed_package_verified': True, 'pixel_capture_unarmed': True}, indent=2) + '\n')

    def observe(self):
        stop = threading.Event(); workers = []; threads = []; result = {'passed': False}
        try:
            print('Verifying boot, installed components and graphics configuration before observation.', flush=True)
            self.preflight()
            time.sleep(5)
            self.identity('identity-settled.txt')
            for name in BUFFERS:
                stream = Stream(self.out, name, stop); workers.append(stream)
                thread = threading.Thread(target=stream.run, daemon=True); threads.append(thread); thread.start()
            started = time.monotonic_ns(); deadline = time.monotonic() + self.seconds
            (self.out / 'window.json').write_text(json.dumps({'host_begin_ns': started, 'host_unix_begin_ns': time.time_ns(),
                'boot': self.binding['boot'], 'duration_seconds': self.seconds}, indent=2) + '\n')
            self.arm_metadata()
            print('Recording bounded nonpixel metadata with pixel capture OFF. Run:', self.out, flush=True)
            index = 0
            while time.monotonic() < deadline:
                self.identity('identity-sample-%02d.txt' % index)
                self.read('state-sample-%02d.txt' % index, STATE_QUERY, required=False)
                index += 1
                if any('error' in w.result or (not t.is_alive()) for w,t in zip(workers,threads)):
                    raise RuntimeError('A stream ended or reached its cap; partial baseline retained')
                if shutil.disk_usage(self.out).free < 64 * 1024**2:
                    raise RuntimeError('Host reserve below 64 MiB')
                stop.wait(min(15, max(0, deadline - time.monotonic())))
            self.identity('identity-window-end.txt')
            result.update(passed=True, same_boot_SF_HWC=True, pixel_arm_empty_at_identity_checks=True,
                          host_window_end_ns=time.monotonic_ns(), state_samples=index)
        except Exception as e:
            result['error'] = str(e)
        finally:
            try:
                cleanup = self.disarm_metadata()
            except Exception as e:
                cleanup = {'attempted': self.arm_attempted, 'cleared': False, 'error': str(e)}
                result.update(passed=False, diagnostic_disarm_error=str(e))
            result['metadata_arm_cleanup'] = cleanup
            stop.set()
            for t in threads: t.join(timeout=4)
            result['passed'] = (result['passed'] and all(not t.is_alive() for t in threads)
                                and all(w.result['complete'] and 'error' not in w.result for w in workers))
            result.update(streams=[w.result for w in workers], collector_configuration_changes=self.arm_attempted,
                collector_changes_scope="own nonpixel metadata arm only", GPU_UI_policy_changed=False,
                collector_pixel_reads=False,
                overhead='ADB/log readers and low-duty metadata still add load; not zero-overhead or a FPS benchmark')
            # Context after the timed window. A changed boot/process gets no unbound post-dumps.
            if result['passed']:
                try:
                    self.read('kernel-after.txt', 'dmesg', required=False)
                    self.read('SF-after.txt', 'dumpsys -t 5 SurfaceFlinger', required=False)
                    self.read('activity-after.txt', 'dumpsys -t 5 activity top', required=False)
                    self.read('processes-after.txt', 'ps -A -o PID,PPID,NAME', required=False)
                    self.read('crash-filenames-after.txt', 'ls -l /data/anr /data/tombstones', required=False)
                    self.read('properties-after.txt', PROPERTY_QUERY)
                    self.read('GPU-env-after.txt', ENV_QUERY, required=False)
                    self.identity('identity-after.txt')
                except Exception as e:
                    result.update(passed=False, post_window_error=str(e))
            (self.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            (self.out / 'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n'
                for p in sorted(self.out.iterdir()) if p.is_file() and p.name not in ['SHA256SUMS', 'observations.jsonl']))
        print(json.dumps(result, indent=2), flush=True)
        return result['passed']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package-artifact', type=Path)
    p.add_argument('--output-root', type=Path)
    p.add_argument('--seconds', type=int, default=120)
    p.add_argument('--mark-run', type=Path)
    p.add_argument('--kind', choices=['missing', 'tearing', 'returned', 'normal'])
    args = p.parse_args()
    if args.mark_run:
        if not args.kind or args.package_artifact or args.output_root:
            p.error('Marker needs only --mark-run and --kind')
        print(json.dumps(append_marker(args.mark_run, args.kind)))
        return
    if not args.package_artifact or not args.output_root or args.kind:
        p.error('Use explicit --package-artifact and --output-root')
    if not Observer(args.output_root, args.package_artifact, args.seconds).observe():
        raise SystemExit(1)

if __name__ == '__main__': main()
