/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_superman"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <cutils/str_parms.h>
#include <limits.h>
#include <stdatomic.h>
#include <tinyalsa/asoundlib.h>

#include <log/log.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <system/audio.h>

#define SUPERMAN_DEFAULT_SAMPLE_RATE   48000
#define SUPERMAN_DEFAULT_AUDIO_FORMAT  AUDIO_FORMAT_PCM_16_BIT

#define SUPERMAN_INPUT_BUFFER_MILLISECONDS  20
#define SUPERMAN_INPUT_DEFAULT_CHANNEL_MASK AUDIO_CHANNEL_IN_MONO

#define SUPERMAN_OUTPUT_BUFFER_MILLISECONDS  10
#define SUPERMAN_OUTPUT_DEFAULT_CHANNEL_MASK AUDIO_CHANNEL_OUT_STEREO

struct superman_audio_device {
    struct audio_hw_device device;
    atomic_bool mic_muted;
};

struct superman_stream_out {
    struct audio_stream_out stream;
    struct pcm *pcm;
    pthread_mutex_t lock;
    uint32_t sample_rate;
    audio_channel_mask_t channel_mask;
    audio_format_t format;
    size_t frame_count;
};

struct superman_stream_in {
    struct audio_stream_in stream;
    struct pcm *pcm;
    pthread_mutex_t lock;
    struct superman_audio_device *adev;
    uint32_t sample_rate;
    audio_channel_mask_t channel_mask;
    audio_format_t format;
    size_t frame_count;
};

#ifndef SUPERMAN_PCM_CARD_ID
#define SUPERMAN_PCM_CARD_ID "SupermanAudio"
#endif
#ifndef SUPERMAN_CAPTURE_DEVICE
#define SUPERMAN_CAPTURE_DEVICE 0
#endif

/* Bind only the machine card we own, never the first USB/HDMI/loopback card. */
static int find_card(void)
{
    int found = -ENODEV;
    for (unsigned i = 0; i < 32; ++i) {
        char path[64], id[64];
        snprintf(path, sizeof(path), "/proc/asound/card%u/id", i);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        bool match = fgets(id, sizeof(id), f) &&
                     (id[strcspn(id, "\r\n")] = 0,
                      strcmp(id, SUPERMAN_PCM_CARD_ID) == 0);
        fclose(f);
        if (match) {
            if (found >= 0) return -EEXIST;
            found = (int)i;
        }
    }
    return found;
}

static int open_pcm(struct pcm **pcm, bool input, unsigned rate, unsigned channels)
{
    int card = find_card();
    if (card < 0) return card;
    struct pcm_config cfg = {
        .channels = input ? channels : 1, .rate = rate, .format = PCM_FORMAT_S16_LE,
        .period_size = rate / 100, .period_count = input ? 4 : 8,
        .start_threshold = input ? rate / 100 : 6 * (rate / 100),
    };
    *pcm = pcm_open((unsigned)card, input ? SUPERMAN_CAPTURE_DEVICE : 0,
                    (input ? PCM_IN : PCM_OUT) | PCM_MONOTONIC, &cfg);
    if (!*pcm || !pcm_is_ready(*pcm)) {
        ALOGE("PCM %s open failed: %s", input ? "capture" : "playback",
              *pcm ? pcm_get_error(*pcm) : "allocation failure");
        if (*pcm) pcm_close(*pcm);
        *pcm = NULL;
        return -ENODEV;
    }
    return 0;
}

static int negotiate(struct audio_config *cfg, bool input)
{
    bool valid = (!cfg->sample_rate || cfg->sample_rate == 48000) &&
                 (cfg->format == AUDIO_FORMAT_DEFAULT || cfg->format == AUDIO_FORMAT_PCM_16_BIT) &&
                 (cfg->channel_mask == AUDIO_CHANNEL_NONE ||
                  cfg->channel_mask == (input ? AUDIO_CHANNEL_IN_MONO : AUDIO_CHANNEL_OUT_STEREO) ||
                  (input && cfg->channel_mask == AUDIO_CHANNEL_IN_STEREO));
    if (!valid) {
        cfg->sample_rate = 48000; cfg->format = AUDIO_FORMAT_PCM_16_BIT;
        cfg->channel_mask = input ? AUDIO_CHANNEL_IN_MONO : AUDIO_CHANNEL_OUT_STEREO;
        return -EINVAL;
    }
    cfg->sample_rate = 48000; cfg->format = AUDIO_FORMAT_PCM_16_BIT;
    if (cfg->channel_mask == AUDIO_CHANNEL_NONE)
        cfg->channel_mask = input ? AUDIO_CHANNEL_IN_MONO : AUDIO_CHANNEL_OUT_STEREO;
    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    const struct superman_stream_out *out = (const struct superman_stream_out *)stream;

    return out->sample_rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return rate == ((struct superman_stream_out *)stream)->sample_rate ? 0 : -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    const struct superman_stream_out *out = (const struct superman_stream_out *)stream;
    size_t buffer_size = out->frame_count *
                         audio_stream_out_frame_size(&out->stream);

    return buffer_size;
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    const struct superman_stream_out *out = (const struct superman_stream_out *)stream;

    return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    const struct superman_stream_out *out = (const struct superman_stream_out *)stream;

    return out->format;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return format == AUDIO_FORMAT_PCM_16_BIT ? 0 : -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct superman_stream_out *out = (void *)stream;
    pthread_mutex_lock(&out->lock);
    if (out->pcm) pcm_close(out->pcm);
    out->pcm = NULL;
    pthread_mutex_unlock(&out->lock);
    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct str_parms *p = str_parms_create_str(kvpairs);
    if (!p) return -ENOMEM;
    int route = 0;
    int rc = str_parms_get_int(p, AUDIO_PARAMETER_STREAM_ROUTING, &route);
    str_parms_destroy(p);
    if (rc == -ENOENT) return 0;
    if (rc < 0) return -EINVAL;
    if (!route) return out_standby(stream);
    return (audio_devices_t)route == AUDIO_DEVICE_OUT_SPEAKER ? 0 : -ENOSYS;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    return 80;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static void downmix_stereo(const int16_t *input, int16_t *output, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
        output[i] = ((int32_t)input[2*i] + input[2*i+1]) / 2;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    struct superman_stream_out *out = (void *)stream;
    if (!buffer || bytes > UINT_MAX || bytes % audio_stream_out_frame_size(stream))
        return -EINVAL;
    if (!bytes) return 0;
    pthread_mutex_lock(&out->lock);
    int rc = out->pcm ? 0 : open_pcm(&out->pcm, false, out->sample_rate,
                                  audio_channel_count_from_out_mask(out->channel_mask));
    const int16_t *input = buffer;
    size_t remaining = bytes / 4;
    int16_t mono[480];
    while (!rc && remaining) {
        size_t frames = remaining < 480 ? remaining : 480;
        downmix_stereo(input, mono, frames);
        if (pcm_write(out->pcm, mono, (unsigned)(frames * sizeof(*mono)))) rc = -EIO;
        input += frames * 2;
        remaining -= frames;
    }
    if (rc && out->pcm) { pcm_close(out->pcm); out->pcm = NULL; }
    pthread_mutex_unlock(&out->lock);
    return rc ? rc : (ssize_t)bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    *dsp_frames = 0;
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    *timestamp = 0;
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    const struct superman_stream_in *in = (const struct superman_stream_in *)stream;

    return in->sample_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return rate == ((struct superman_stream_in *)stream)->sample_rate ? 0 : -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    const struct superman_stream_in *in = (const struct superman_stream_in *)stream;
    size_t buffer_size = in->frame_count *
                         audio_stream_in_frame_size(&in->stream);

    return buffer_size;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    const struct superman_stream_in *in = (const struct superman_stream_in *)stream;

    return in->channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    const struct superman_stream_in *in = (const struct superman_stream_in *)stream;

    return in->format;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return format == AUDIO_FORMAT_PCM_16_BIT ? 0 : -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct superman_stream_in *in = (void *)stream;
    pthread_mutex_lock(&in->lock);
    if (in->pcm) pcm_close(in->pcm);
    in->pcm = NULL;
    pthread_mutex_unlock(&in->lock);
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct str_parms *p = str_parms_create_str(kvpairs);
    if (!p) return -ENOMEM;
    int route = 0;
    int rc = str_parms_get_int(p, AUDIO_PARAMETER_STREAM_ROUTING, &route);
    str_parms_destroy(p);
    if (rc == -ENOENT) return 0;
    if (rc < 0) return -EINVAL;
    if (!route) return in_standby(stream);
    return (audio_devices_t)route == AUDIO_DEVICE_IN_BUILTIN_MIC ? 0 : -ENOSYS;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return -ENOSYS;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    struct superman_stream_in *in = (void *)stream;
    if (!buffer || bytes > UINT_MAX || bytes % audio_stream_in_frame_size(stream))
        return -EINVAL;
    if (!bytes) return 0;
    pthread_mutex_lock(&in->lock);
    int rc = in->pcm ? 0 : open_pcm(&in->pcm, true, in->sample_rate,
                                 audio_channel_count_from_in_mask(in->channel_mask));
    if (!rc && pcm_read(in->pcm, buffer, (unsigned)bytes)) rc = -EIO;
    if (rc && in->pcm) { pcm_close(in->pcm); in->pcm = NULL; }
    if (rc || atomic_load(&in->adev->mic_muted)) memset(buffer, 0, bytes);
    pthread_mutex_unlock(&in->lock);
    return rc ? rc : (ssize_t)bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static size_t samples_per_milliseconds(size_t milliseconds,
                                       uint32_t sample_rate,
                                       size_t channel_count)
{
    return milliseconds * sample_rate * channel_count / 1000;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{

    *stream_out = NULL;
    if (devices != AUDIO_DEVICE_OUT_SPEAKER || (flags & AUDIO_OUTPUT_FLAG_DIRECT)) return -ENOSYS;
    int rc = negotiate(config, false);
    if (rc) return rc;
    struct superman_stream_out *out =
            (struct superman_stream_out *)calloc(1, sizeof(struct superman_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->sample_rate = config->sample_rate;
    if (out->sample_rate == 0)
        out->sample_rate = SUPERMAN_DEFAULT_SAMPLE_RATE;
    out->channel_mask = config->channel_mask;
    if (out->channel_mask == AUDIO_CHANNEL_NONE)
        out->channel_mask = SUPERMAN_OUTPUT_DEFAULT_CHANNEL_MASK;
    out->format = config->format;
    if (out->format == AUDIO_FORMAT_DEFAULT)
        out->format = SUPERMAN_DEFAULT_AUDIO_FORMAT;
    out->frame_count = samples_per_milliseconds(
                           SUPERMAN_OUTPUT_BUFFER_MILLISECONDS,
                           out->sample_rate, 1);

    rc = open_pcm(&out->pcm, false, out->sample_rate,
                  audio_channel_count_from_out_mask(out->channel_mask));
    if (rc) { free(out); return rc; }
    rc = pthread_mutex_init(&out->lock, NULL);
    if (rc) { pcm_close(out->pcm); free(out); return -rc; }
    *stream_out = &out->stream;
    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    out_standby(&stream->common);
    pthread_mutex_destroy(&((struct superman_stream_out *)stream)->lock);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    return -ENOSYS;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    int card = find_card();
    return card < 0 ? card : 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return mode == AUDIO_MODE_NORMAL ? 0 : -ENOSYS;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    atomic_store(&((struct superman_audio_device *)dev)->mic_muted, state);
    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    *state = atomic_load(&((const struct superman_audio_device *)dev)->mic_muted);
    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    struct audio_config supported = *config;
    if (negotiate(&supported, true)) return 0;
    size_t buffer_size = supported.sample_rate / 50 *
                        audio_channel_count_from_in_mask(supported.channel_mask) * 2;
    return buffer_size;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused,
                                  const char *address __unused,
                                  audio_source_t source __unused)
{

    *stream_in = NULL;
    if (devices != AUDIO_DEVICE_IN_BUILTIN_MIC) return -ENOSYS;
    int rc = negotiate(config, true);
    if (rc) return rc;
    struct superman_stream_in *in = (struct superman_stream_in *)calloc(1, sizeof(struct superman_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;
    in->sample_rate = config->sample_rate;
    if (in->sample_rate == 0)
        in->sample_rate = SUPERMAN_DEFAULT_SAMPLE_RATE;
    in->channel_mask = config->channel_mask;
    if (in->channel_mask == AUDIO_CHANNEL_NONE)
        in->channel_mask = SUPERMAN_INPUT_DEFAULT_CHANNEL_MASK;
    in->format = config->format;
    if (in->format == AUDIO_FORMAT_DEFAULT)
        in->format = SUPERMAN_DEFAULT_AUDIO_FORMAT;
    in->frame_count = samples_per_milliseconds(
                          SUPERMAN_INPUT_BUFFER_MILLISECONDS, in->sample_rate, 1);

    in->adev = (void *)dev;
    rc = open_pcm(&in->pcm, true, in->sample_rate,
                  audio_channel_count_from_in_mask(in->channel_mask));
    if (rc) { free(in); return rc; }
    rc = pthread_mutex_init(&in->lock, NULL);
    if (rc) { pcm_close(in->pcm); free(in); return -rc; }
    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *in)
{
    in_standby(&in->common);
    pthread_mutex_destroy(&((struct superman_stream_in *)in)->lock);
    free(in);
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{

    struct superman_audio_device *adev;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct superman_audio_device));
    if (!adev)
        return -ENOMEM;

    atomic_init(&adev->mic_muted, false);
    adev->device.common.tag = HARDWARE_DEVICE_TAG;
    adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->device.common.module = (struct hw_module_t *) module;
    adev->device.common.close = adev_close;

    adev->device.init_check = adev_init_check;
    adev->device.set_voice_volume = adev_set_voice_volume;
    adev->device.set_master_volume = adev_set_master_volume;
    adev->device.get_master_volume = adev_get_master_volume;
    adev->device.set_master_mute = adev_set_master_mute;
    adev->device.get_master_mute = adev_get_master_mute;
    adev->device.set_mode = adev_set_mode;
    adev->device.set_mic_mute = adev_set_mic_mute;
    adev->device.get_mic_mute = adev_get_mic_mute;
    adev->device.set_parameters = adev_set_parameters;
    adev->device.get_parameters = adev_get_parameters;
    adev->device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->device.open_output_stream = adev_open_output_stream;
    adev->device.close_output_stream = adev_close_output_stream;
    adev->device.open_input_stream = adev_open_input_stream;
    adev->device.close_input_stream = adev_close_input_stream;
    adev->device.dump = adev_dump;

    *device = &adev->device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Superman PCM audio HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
