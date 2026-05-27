/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef __KEYWORD_H__
#define __KEYWORD_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct {
	const char *name;
	size_t count_needed;
	uint8_t threshold_percent;
} keyword_class_cfg_t;

typedef struct {
	bool has_active_class;
	uint16_t predicted_class;
	size_t count;
	size_t non_keyword_count;
	uint16_t non_keyword_class;
	float average_probability;
	bool wait_for_class_change;
	uint16_t blocked_class;
} keyword_runtime_ctx_t;

typedef struct {
	bool has_first_keyword;
	uint16_t first_class;
	float first_probability;
	uint32_t detected_at_ms;
} keyword_phrase_ctx_t;

/* Indices must match the KWS model output classes (see scripts/kws_live_plot.py). */
typedef enum keyword_labels_e {
	KEYWORD_LIGHT = 0,
	KEYWORD_OFF = 1,
	KEYWORD_ON = 2,
	KEYWORD_OTHER = 3,
	KEYWORD_SCENE_FOUR = 4,
	KEYWORD_SCENE_ONE = 5,
	KEYWORD_SCENE_THREE = 6,
	KEYWORD_SCENE_TWO = 7,
	KEYWORD_SILENCE = 8,
	KEYWORD_TOGGLE_LIGHT = 9,

	KEYWORDS_cnt
} keyword_labels_t;

int kw_init(void);
void kw_reset_model(void);
/**
 * @return 0 while still listening, 1 when a full keyword command is recognized (see @a kw_class),
 *         -EBUSY if more audio is needed, or another negative errno on error.
 */
int kw_process(uint8_t *const audio_buffer, const uint16_t num_samples, uint16_t *const kw_class);

extern const keyword_class_cfg_t KEYWORD_CLASSES_CFG[];
#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
