/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUPERMAN_CODEC_H
#define SUPERMAN_CODEC_H
bool superman_wcd9302_reset_ready(void);
bool superman_slim_pio_ready(void);
struct device;
int superman_wcd9302_rx_start(struct device *dev, unsigned int port, unsigned int channel);
int superman_wcd9302_rx_stop(struct device *dev);
int superman_speaker_amp_enable(bool enable);
int superman_speaker_amp_is_off(void);
int superman_wcd9302_lineout_start(struct device *dev);
int superman_wcd9302_lineout_stop(struct device *dev);
int superman_audio_board_power_enable(bool enable);
bool superman_audio_board_power_ready(void);
#endif
