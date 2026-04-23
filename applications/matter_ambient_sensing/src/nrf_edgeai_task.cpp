/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "board/board.h"
#include "app/task_executor.h"
#include "dimming_effect.h"

#include "dmic.h"
#include "nrf_edgeai_task.h"
#include "app_task.h"
#include "ambient_sensing.h"
#include "models/all_models.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/EventLogging.h>
#include <platform/CHIPDeviceLayer.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::app::Clusters;
using namespace ::chip::DeviceLayer;

namespace
{
const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

#ifdef CONFIG_USE_OCCUPANCY_SENSOR_INSTEAD_OF_AMBIENT_SENSING
void ApplyOccupancyValue(EndpointId endpointId, uint8_t newOccupancyValue)
{
	using chip::BitMask;
	using chip::Protocols::InteractionModel::Status;

	BitMask<Clusters::OccupancySensing::OccupancyBitmap> currentOccupancy;
	Status status = Clusters::OccupancySensing::Attributes::Occupancy::Get(endpointId,
									       &currentOccupancy);

	if (status != Status::Success) {
		LOG_ERR("Occupancy::Get failed: %u", static_cast<unsigned>(to_underlying(status)));
		return;
	}

	if (static_cast<BitMask<Clusters::OccupancySensing::OccupancyBitmap>>(newOccupancyValue) ==
	    currentOccupancy) {
		return;
	}

	status = Clusters::OccupancySensing::Attributes::Occupancy::Set(endpointId,
									newOccupancyValue);
	if (status != Status::Success) {
		LOG_ERR("Occupancy::Set failed: %u", static_cast<unsigned>(to_underlying(status)));
		return;
	}

	if (newOccupancyValue == 1) {
		LOG_INF("Occupancy is now occupied");
	} else {
		LOG_INF("Occupancy is now vacant");
	}
}

void OccupancySensingChipWorkerHandler(intptr_t arg)
{
	ApplyOccupancyValue(CONFIG_AMBIENT_SENSING_ENDPOINT_ID, static_cast<uint8_t>(arg));
}

CHIP_ERROR RequestOccupancyMatterUpdate(uint8_t occupancyRaw)
{
	return DeviceLayer::PlatformMgr().ScheduleWork(OccupancySensingChipWorkerHandler,
						       static_cast<intptr_t>(occupancyRaw));
}
#endif /* CONFIG_USE_OCCUPANCY_SENSOR_INSTEAD_OF_AMBIENT_SENSING */

/**
 * @brief Common post-detection notification (LED effect + optional Matter
 *        occupancy update). Used by both the single-model and multi-model
 *        code paths.
 */
void NotifyDetection(const char *model_name)
{
	LOG_INF("%s detected", model_name);

	Nrf::PostTask([] {
#if defined(CONFIG_PWM) && DT_HAS_ALIAS(pwm_led0) && DT_HAS_ALIAS(pwm_led1) &&                     \
	DT_HAS_ALIAS(pwm_led2) && DT_HAS_ALIAS(pwm_led3)
		Nrf::DimmingEffect::Start();
#endif
	});

#ifdef CONFIG_USE_OCCUPANCY_SENSOR_INSTEAD_OF_AMBIENT_SENSING
	if (RequestOccupancyMatterUpdate(1) != CHIP_NO_ERROR) {
		LOG_ERR("Failed to schedule occupancy update to Matter stack");
	}
#endif
}

void InitDimmingEffect()
{
#if defined(CONFIG_PWM) && DT_HAS_ALIAS(pwm_led0) && DT_HAS_ALIAS(pwm_led1) &&                     \
	DT_HAS_ALIAS(pwm_led2) && DT_HAS_ALIAS(pwm_led3)
	Nrf::DimmingEffect::Config dim_cfg;
	dim_cfg.effect_timeout_s = 5;
	dim_cfg.blink_pairs_multiplier = 3;
	(void)Nrf::DimmingEffect::Init(
		dim_cfg,
		[](void *) {
#ifdef CONFIG_USE_OCCUPANCY_SENSOR_INSTEAD_OF_AMBIENT_SENSING
			if (RequestOccupancyMatterUpdate(0) != CHIP_NO_ERROR) {
				LOG_ERR("Failed to schedule occupancy clear to Matter "
					"stack");
			}
#endif
		},
		nullptr);
#endif
}

void ai_thread_fn()
{
	void *audio_buffer;
	size_t audio_buffer_size;
	const int32_t read_timeout = 100;
	int err = 0;

	LOG_INF("Starting Ambient Sensing Application...");

	nrf_edgeai_rt_version_t libver = nrf_edgeai_runtime_version();
	LOG_INF("Nordic Edge AI Library version: %d.%d.%d", libver.field.major,
		libver.field.minor, libver.field.patch);

#ifdef CONFIG_AMBIENT_SENSING_MODEL_ALL
	/*
	 * Multi-model mode: run every built-in model from this single thread.
	 *
	 * Why single-threaded:
	 *   - All AXON models share the global `nrf_axon_interlayer_buffer`,
	 *     so `nrf_edgeai_run_inference` must be serialised. Worker-thread
	 *     parallelism therefore yields no extra throughput on this SoC.
	 *   - Using per-worker audio queues with drop-oldest semantics meant
	 *     each model lost *different* 10 ms blocks, corrupting its own
	 *     NN input window independently and delaying the rolling
	 *     detectors (visible as recognition lag in the previous design).
	 *   - Feeding every model from the same stream keeps all models in
	 *     lockstep: if the CPU ever falls behind, the DMIC driver drops
	 *     one block at the producer side and every model loses the same
	 *     block. That matches the single-model behaviour exactly, just
	 *     repeated N times per frame.
	 */
	const size_t model_count = Nrf::AmbientSensing::kAllModelsCount;
	nrf_edgeai_t *models[Nrf::AmbientSensing::kMaxAllModels];
	Nrf::AmbientSensing::ModelRuntimeState states[Nrf::AmbientSensing::kMaxAllModels]{};

	if (model_count > Nrf::AmbientSensing::kMaxAllModels) {
		LOG_ERR("Too many ambient sensing models: %u", static_cast<unsigned>(model_count));
		return;
	}

	for (size_t i = 0; i < model_count; ++i) {
		models[i] = Nrf::AmbientSensing::kAllModels[i].get_model();
		nrf_edgeai_err_t ires = nrf_edgeai_init(models[i]);
		if (ires != NRF_EDGEAI_ERR_SUCCESS) {
			LOG_ERR("Failed to initialize %s, error %d",
				Nrf::AmbientSensing::kAllModels[i].name, ires);
			return;
		}
		LOG_INF("  - %s initialised (threshold=%0.3f, N-in-row=%u)",
			Nrf::AmbientSensing::kAllModels[i].name,
			static_cast<double>(
				Nrf::AmbientSensing::kAllModels[i].confidence_threshold),
			static_cast<unsigned>(
				Nrf::AmbientSensing::kAllModels[i].prediction_num_in_row));
	}
#else
	nrf_edgeai_t *p_model = get_ambient_sensing_model();
	nrf_edgeai_err_t ires = nrf_edgeai_init(p_model);
	if (ires != NRF_EDGEAI_ERR_SUCCESS) {
		LOG_ERR("Failed to initialize Edge AI model %s, error code: %d",
			Nrf::AmbientSensing::getModelName(), ires);
		return;
	}
#endif /* CONFIG_AMBIENT_SENSING_MODEL_ALL */

	if (dmic_init()) {
		LOG_ERR("Failed to initialize DMIC");
		return;
	}

	InitDimmingEffect();

	LOG_INF("Edge AI initialization completed");

	if (dmic_trigger(dmic_dev, DMIC_TRIGGER_START) < 0) {
		LOG_ERR("Failed to start DMIC");
		return;
	}

	while (true) {
		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size, read_timeout);
		if (err != 0) {
			if (err == -EAGAIN || err == -EBUSY) {
				k_yield();
				continue;
			}
			LOG_WRN("DMIC read failed (err %d)", err);
			k_sleep(K_MSEC(1));
			continue;
		}

		if (!EdgeAITask::Instance().IsEnabled()) {
			free_dmic_buffer(audio_buffer);
			continue;
		}

		const size_t samples_num = audio_buffer_size / DMIC_SAMPLE_BYTES;

#ifdef CONFIG_AMBIENT_SENSING_MODEL_ALL
		/*
		 * Feed the same 10 ms block into every model's DSP pipeline and
		 * run inference immediately. AXON is intrinsically serialised,
		 * but because this loop is single-threaded we pay neither
		 * mutex nor queueing overhead.
		 */
#ifdef CONFIG_AMBIENT_SENSING_ALL_LOG_TIMING
		const int64_t frame_start_us = k_uptime_ticks();
#endif
		for (size_t i = 0; i < model_count; ++i) {
			const auto &cfg = Nrf::AmbientSensing::kAllModels[i];
			nrf_edgeai_t *m = models[i];

			nrf_edgeai_err_t res = nrf_edgeai_feed_inputs(m, audio_buffer, samples_num);
			if (res != NRF_EDGEAI_ERR_SUCCESS) {
				continue;
			}

#ifdef CONFIG_AMBIENT_SENSING_ALL_LOG_TIMING
			const int64_t t0 = k_uptime_ticks();
#endif
			res = nrf_edgeai_run_inference(m);
#ifdef CONFIG_AMBIENT_SENSING_ALL_LOG_TIMING
			const int64_t t1 = k_uptime_ticks();
			LOG_DBG("  [%s] inference %u us", cfg.name,
				static_cast<unsigned>(
					k_ticks_to_us_near32(static_cast<uint32_t>(t1 - t0))));
#endif
			if (res != NRF_EDGEAI_ERR_SUCCESS) {
				continue;
			}

			if (Nrf::AmbientSensing::process(cfg, states[i], m)) {
				NotifyDetection(cfg.name);
			}
		}
#ifdef CONFIG_AMBIENT_SENSING_ALL_LOG_TIMING
		const int64_t frame_end_us = k_uptime_ticks();
		LOG_DBG("Frame total %u us",
			static_cast<unsigned>(k_ticks_to_us_near32(
				static_cast<uint32_t>(frame_end_us - frame_start_us))));
#endif

		free_dmic_buffer(audio_buffer);
#else /* !CONFIG_AMBIENT_SENSING_MODEL_ALL */
		nrf_edgeai_err_t res = nrf_edgeai_feed_inputs(p_model, audio_buffer, samples_num);
		free_dmic_buffer(audio_buffer);

		if (res != NRF_EDGEAI_ERR_SUCCESS) {
			continue;
		}

		res = nrf_edgeai_run_inference(p_model);
		if (res != NRF_EDGEAI_ERR_SUCCESS) {
			continue;
		}

		if (Nrf::AmbientSensing::process(p_model)) {
			NotifyDetection(Nrf::AmbientSensing::getModelName());
		}
#endif /* CONFIG_AMBIENT_SENSING_MODEL_ALL */
	}
}

K_THREAD_DEFINE(ai_thread_id, CONFIG_AI_THREAD_STACK_SIZE, ai_thread_fn, NULL, NULL, NULL,
		CONFIG_AI_THREAD_PRIORITY, K_FP_REGS, SYS_FOREVER_MS);
} // namespace

CHIP_ERROR EdgeAITask::Start()
{
	k_thread_start(ai_thread_id);
	return CHIP_NO_ERROR;
}

void EdgeAITask::Enable()
{
#ifdef CONFIG_AMBIENT_SENSING_MODEL_ALL
	LOG_INF("\n\nWaiting for any registered ambient sound source...\n\n");
#else
	LOG_INF("\n\nWaiting for %s source...\n\n", Nrf::AmbientSensing::getModelName());
#endif
	enabled.store(true, std::memory_order_release);
}

void EdgeAITask::Disable()
{
	enabled.store(false, std::memory_order_release);
	LOG_INF("\n\nEdge AI listening disabled\n\n");
}

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &attributePath,
				       uint8_t type, uint16_t size, uint8_t *value)
{
	ClusterId clusterId = attributePath.mClusterId;
	AttributeId attributeId = attributePath.mAttributeId;

	if (clusterId == OccupancySensing::Id &&
	    attributeId == OccupancySensing::Attributes::Occupancy::Id) {
	}
}
