/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "dmic.h"

//////////////////////////////////////////////////////////////////////////////

#define BLOCK_SIZE (DMIC_SAMPLE_BYTES * DMIC_PCM_RATE * SAMPLES_BLOCK_LENGTH_MS / 1000)
#define DMIC_BUFFER_BLOCKS 16

/* Extra buffering helps tolerate slow synchronous debug prints. */
K_MEM_SLAB_DEFINE_STATIC(dmic_mem_slab, BLOCK_SIZE, DMIC_BUFFER_BLOCKS, 4);
static const struct device* const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

//////////////////////////////////////////////////////////////////////////////

int dmic_init(void)
{
    int err;

    if (!device_is_ready(dmic_dev))
    {
        printk("Device is not ready\n");
        return -ENODEV;
    }

    struct pcm_stream_cfg stream = {
        .pcm_rate   = DMIC_PCM_RATE,
        .pcm_width  = DMIC_SAMPLE_BYTES * 8,
        .block_size = BLOCK_SIZE,
        .mem_slab   = &dmic_mem_slab,
    };
    struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3250000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
			.req_chan_map_hi = 0,
			.req_num_chan = 1,
			.req_num_streams = 1,
		},
	};

    err = dmic_configure(dmic_dev, &cfg);
    if (err < 0)
    {
        printk("Failed to configure (err %d)\n", err);
        return err;
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////

int dmic_start(void)
{
    return dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
}

//////////////////////////////////////////////////////////////////////////////

int dmic_read_buffer(void** audio_buffer, size_t* audio_buffer_size, int32_t timeout_ms)
{
    return dmic_read(dmic_dev, 0, audio_buffer, audio_buffer_size, timeout_ms);
}

//////////////////////////////////////////////////////////////////////////////
void dmic_free_buffer(void* buffer)
{
    k_mem_slab_free(&dmic_mem_slab, buffer);
}
