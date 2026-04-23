/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "ambient_sensing.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <nrf_edgeai/nrf_edgeai.h>

#include "models/all_models.h"

LOG_MODULE_REGISTER(ambient_sensing, LOG_LEVEL_INF);

namespace
{
/** Default history window bit-width used by every built-in model. */
constexpr uint32_t kDefaultMaxPredictionInRow = 31;

#if defined(CONFIG_AMBIENT_SENSING_MODEL_SNORING)
constexpr float kLegacyConfidenceThreshold = 0.9f;
constexpr uint32_t kLegacyPredictionNumInRow = 20;
const char *kLegacyModelName = "Snoring";
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_BABY_CRYING)
constexpr float kLegacyConfidenceThreshold = 0.996078f;
constexpr uint32_t kLegacyPredictionNumInRow = 3;
const char *kLegacyModelName = "Baby Crying";
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_DOG_BARKING)
constexpr float kLegacyConfidenceThreshold = 0.9f;
constexpr uint32_t kLegacyPredictionNumInRow = 10;
const char *kLegacyModelName = "Dog Barking";
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_CAT_MEOWING)
constexpr float kLegacyConfidenceThreshold = 0.95f;
constexpr uint32_t kLegacyPredictionNumInRow = 9;
const char *kLegacyModelName = "Cat Meowing";
#else
constexpr float kLegacyConfidenceThreshold = 0.9f;
constexpr uint32_t kLegacyPredictionNumInRow = 10;
const char *kLegacyModelName = "Ambient";
#endif

/**
 * @brief Core rolling-window detector shared by single- and multi-threaded
 *        entry points.
 *
 * @note Uses references to caller-owned state so the same logic is safe
 *       to run from multiple threads, one detector instance per thread.
 */
bool rolling_detector_step(nrf_edgeai_t *p_model, float confidence_threshold,
			   uint32_t prediction_num_in_row, uint32_t max_prediction_num_in_row,
			   uint32_t &prediction_count, uint32_t &predictions_history)
{
	/* Read confidence for the model's predicted class for this frame. */
	const uint16_t predicted_class = p_model->decoded_output.classif.predicted_class;
	const float probability =
		p_model->decoded_output.classif.probabilities.p_f32[predicted_class];

	const bool detected = probability > confidence_threshold;

	/* Bit that will fall out of the rolling window. */
	const bool oldest_entry = (bool)(predictions_history & BIT(max_prediction_num_in_row));

	/* O(1) update of the count plus left-shift history. */
	prediction_count = prediction_count + detected - oldest_entry;
	predictions_history = (predictions_history << 1) | detected;

	if (prediction_count >= prediction_num_in_row) {
		/* Enough positive frames: fire once and clear state to avoid
		 * repeated triggers from stale history. */
		prediction_count = 0;
		predictions_history = 0;
		return true;
	}
	return false;
}
} // namespace

namespace Nrf
{
namespace AmbientSensing
{

const char *getModelName()
{
	return kLegacyModelName;
}

bool process(nrf_edgeai_t *p_model)
{
	static uint32_t prediction_count;
	static uint32_t predictions_history;

	return rolling_detector_step(p_model, kLegacyConfidenceThreshold,
				     kLegacyPredictionNumInRow, kDefaultMaxPredictionInRow,
				     prediction_count, predictions_history);
}

bool process(const ModelConfig &cfg, ModelRuntimeState &state, nrf_edgeai_t *p_model)
{
	return rolling_detector_step(p_model, cfg.confidence_threshold,
				     cfg.prediction_num_in_row, cfg.max_prediction_num_in_row,
				     state.prediction_count, state.predictions_history);
}

#ifdef CONFIG_AMBIENT_SENSING_MODEL_ALL
const ModelConfig kAllModels[] = {
	{
		.get_model = nrf_edgeai_user_model_snoring,
		.name = "Snoring",
		.confidence_threshold = 0.9f,
		.prediction_num_in_row = 20,
		.max_prediction_num_in_row = kDefaultMaxPredictionInRow,
	},
	{
		.get_model = nrf_edgeai_user_model_baby_crying,
		.name = "Baby Crying",
		.confidence_threshold = 0.996078f,
		.prediction_num_in_row = 3,
		.max_prediction_num_in_row = kDefaultMaxPredictionInRow,
	},
	{
		.get_model = nrf_edgeai_user_model_dog_barking,
		.name = "Dog Barking",
		.confidence_threshold = 0.9f,
		.prediction_num_in_row = 10,
		.max_prediction_num_in_row = kDefaultMaxPredictionInRow,
	},
	{
		.get_model = nrf_edgeai_user_model_cat_meowing,
		.name = "Cat Meowing",
		.confidence_threshold = 0.95f,
		.prediction_num_in_row = 9,
		.max_prediction_num_in_row = kDefaultMaxPredictionInRow,
	},
};
const size_t kAllModelsCount = ARRAY_SIZE(kAllModels);
#endif /* CONFIG_AMBIENT_SENSING_MODEL_ALL */

} // namespace AmbientSensing
} // namespace Nrf
