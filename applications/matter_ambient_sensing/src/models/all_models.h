/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <nrf_edgeai/rt/nrf_edgeai_types.h>

#if defined(CONFIG_AMBIENT_SENSING_MODEL_SNORING)
#include <models/snoring/nrf_edgeai_user_model.h>
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_DOG_BARKING)
#include <models/dog_barking/nrf_edgeai_user_model.h>
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_CAT_MEOWING)
#include <models/cat_meowing/nrf_edgeai_user_model.h>
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_BABY_CRYING)
#include <models/baby_crying/nrf_edgeai_user_model.h>
#endif

#ifdef CONFIG_AMBIENT_SENSING_MODEL_ALL
/*
 * Each generated model header uses the same `_NRF_EDGEAI_USER_MODEL_H_`
 * include guard, so including them together only pulls in the first one.
 * Declare the four getters we need here directly.
 */
#ifdef __cplusplus
extern "C" {
#endif

nrf_edgeai_t *nrf_edgeai_user_model_snoring(void);
nrf_edgeai_t *nrf_edgeai_user_model_dog_barking(void);
nrf_edgeai_t *nrf_edgeai_user_model_cat_meowing(void);
nrf_edgeai_t *nrf_edgeai_user_model_baby_crying(void);

#ifdef __cplusplus
}
#endif
#endif /* CONFIG_AMBIENT_SENSING_MODEL_ALL */

#if defined(CONFIG_AMBIENT_SENSING_MODEL_SNORING)
static inline nrf_edgeai_t *get_ambient_sensing_model()
{
	return nrf_edgeai_user_model_snoring();
}
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_DOG_BARKING)
static inline nrf_edgeai_t *get_ambient_sensing_model()
{
	return nrf_edgeai_user_model_dog_barking();
}
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_CAT_MEOWING)
static inline nrf_edgeai_t *get_ambient_sensing_model()
{
	return nrf_edgeai_user_model_cat_meowing();
}
#elif defined(CONFIG_AMBIENT_SENSING_MODEL_BABY_CRYING)
static inline nrf_edgeai_t *get_ambient_sensing_model()
{
	return nrf_edgeai_user_model_baby_crying();
}
#endif
