// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_SIM_IO_H
#define SUPERMAN_RIL_SIM_IO_H
#include <stddef.h>
#include <stdint.h>
/* Read-only UIM file access. Returns payload bytes, or a negative errno. */
int uim_file_request(uint8_t *out, size_t capacity, unsigned subscription, const uint8_t *aid,
                     size_t aid_bytes, int command, int file, const char *path, int p1, int p2,
                     int p3);
int uim_file_response(const uint8_t *body, size_t bytes, int command, int file,
                      const uint8_t **contents, size_t *length, int *sw1, int *sw2);
int imsi_decode(const uint8_t *data, size_t bytes, char out[16]);
#endif
