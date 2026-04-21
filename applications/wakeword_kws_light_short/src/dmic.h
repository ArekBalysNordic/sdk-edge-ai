/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup dmic DMIC control functions
 * @{
 */

#ifndef __DMIC_H__
#define __DMIC_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DMIC_SAMPLE_BYTES       (2)
#define DMIC_PCM_RATE           (16000)
#define SAMPLES_BLOCK_LENGTH_MS (10)

/**
 * @brief Initialize DMIC.
 *
 * @return Operation status result, 0 for success.
 */
int dmic_init(void);

/**
 * @brief Trigger DMIC start.
 *
 * @return Operation status result, 0 for success.
 */
int dmic_start(void);

/**
 * @brief Read one audio block from DMIC.
 *
 * @param[out] audio_buffer      Pointer to returned audio buffer.
 * @param[out] audio_buffer_size Size of returned audio buffer in bytes.
 * @param[in]  timeout_ms        Read timeout in milliseconds.
 *
 * @return Operation status result, 0 for success.
 */
int dmic_read_buffer(void** audio_buffer, size_t* audio_buffer_size, int32_t timeout_ms);

/**
 * @brief Free the audio buffer acquired with @c dmic_read.
 *
 * @param buffer Audio buffer.
 */
void dmic_free_buffer(void* buffer);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __DMIC_H__ */

/**
 * @}
 */
