/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <nrf_edgeai/nrf_edgeai.h>

namespace Nrf
{
namespace AmbientSensing
{

/**
 * @brief Legacy single-model post-processing (compile-time selected model).
 *
 * Uses internal static state; safe to call from a single thread only.
 */
bool process(nrf_edgeai_t *p_model);

/**
 * @brief Name of the compile-time selected model (legacy path).
 */
const char *getModelName();

/**
 * @brief Post-processing configuration for a single ambient sensing model.
 */
struct ModelConfig {
	/** Function returning the per-model @ref nrf_edgeai_t instance. */
	nrf_edgeai_t *(*get_model)(void);

	/** Human readable model name, used for logging. */
	const char *name;

	/** Confidence threshold above which the current frame is counted as a
	 * detection. */
	float confidence_threshold;

	/** Minimum number of positive detections accumulated within the
	 * history window to report a detection event. */
	uint32_t prediction_num_in_row;

	/** Size of the rolling history window in frames (bit-width used in
	 * the @ref predictions_history mask). */
	uint32_t max_prediction_num_in_row;
};

/**
 * @brief Per-thread rolling detector state.
 */
struct ModelRuntimeState {
	uint32_t prediction_count;
	uint32_t predictions_history;
};

/**
 * @brief Re-entrant post-processing used by multi-threaded mode.
 *
 * @return true when the rolling detector decides an event should be emitted.
 */
bool process(const ModelConfig &cfg, ModelRuntimeState &state, nrf_edgeai_t *p_model);

#ifdef CONFIG_AMBIENT_SENSING_MODEL_ALL
/**
 * @brief Compile-time upper bound on the number of models in @ref kAllModels.
 *        Used by consumers that need to size stack-allocated arrays.
 */
constexpr size_t kMaxAllModels = 8;

/**
 * @brief Table of all built-in ambient sensing models used when
 *        @c CONFIG_AMBIENT_SENSING_MODEL_ALL is enabled.
 */
extern const ModelConfig kAllModels[];
extern const size_t kAllModelsCount;
#endif

} // namespace AmbientSensing
} // namespace Nrf
