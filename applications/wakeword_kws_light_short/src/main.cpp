
#include <zephyr/kernel.h>

#include <string.h>

#include <nrf_edgeai/rt/nrf_edgeai_runtime.h>
#include <nrf_edgeai/rt/nrf_edgeai_runtime_aux.h>  // for utility functions nrf_edgeai_model_axon_init_persistent_vars

#include "models/wakeword/nrf_edgeai_generated/nrf_edgeai_user_model.h"
#include "models/kws/nrf_edgeai_generated/nrf_edgeai_user_model.h"

#include "dmic.h"

//////////////////////////////////////////////////////////////////////////////

#define MODEL_WAKEWORD_LABEL        "Okay Nordic"
#define KEYWORD_SPOTTING_TIMEOUT_MS 7000
#define DEMO_FINAL_DETECTIONS_ONLY  0
#define PRINT_RAW_PROBABILITY       1
#define PRINT_KEYWORD_CLASS_ACTIVITY 1

//////////////////////////////////////////////////////////////////////////////

typedef enum application_state_e
{
    APP_WAITING_FOR_WAKEWORD,
    APP_WAITING_FOR_KEYWORDS,
} application_state_t;

typedef enum keyword_labels_e
{
    KEYWORD_OFF     = 0,
    KEYWORD_ON    = 1,
    KEYWORD_OTHER     = 2,
    KEYWORD_SILENCE    = 3,
    KEYWORD_SWITCH     = 4,

    KEYWORDS_cnt
} keyword_labels_t;

typedef struct
{
    const char* name;
    size_t      count_needed;
    uint8_t     threshold_percent;
} keyword_class_cfg_t;

typedef struct
{
    bool     has_active_class;
    uint16_t predicted_class;
    size_t   count;
    size_t   non_keyword_count;
    uint16_t non_keyword_class;
    flt32_t  average_probability;
    bool     wait_for_class_change;
    uint16_t blocked_class;
} keyword_runtime_ctx_t;

typedef struct
{
    bool     has_first_keyword;
    uint16_t first_class;
    flt32_t  first_probability;
    uint32_t detected_at_ms;
} keyword_phrase_ctx_t;

//////////////////////////////////////////////////////////////////////////////

static bool is_wakeword_detected(flt32_t probability);
static void reset_keyword_detection_state(void);
static bool is_keyword_command_component(uint16_t predicted_class);
static bool is_keyword_phrase_first_word(uint16_t predicted_class);
static bool is_keyword_phrase_second_word(uint16_t predicted_class);
static bool is_valid_keyword_command_pair(uint16_t first_class, uint16_t second_class);
static const keyword_class_cfg_t* get_keyword_class_cfg(uint16_t predicted_class);
static bool is_keyword_probability_above_threshold(uint16_t predicted_class, flt32_t probability);
static bool try_detect_keyword_command(uint16_t     predicted_class,
                                       flt32_t      probability,
                                       const char** pp_first_keyword_name,
                                       const char** pp_second_keyword_name,
                                       bool*        p_has_second_keyword,
                                       uint8_t*     p_average_probability);

//////////////////////////////////////////////////////////////////////////////

static const keyword_class_cfg_t KEYWORD_CLASSES_CFG[] = {
    [KEYWORD_OFF] = { .name = "OFF", .count_needed = 2, .threshold_percent = 0 },
    [KEYWORD_ON] = { .name = "ON", .count_needed = 2, .threshold_percent = 0 },
    [KEYWORD_OTHER] = { .name = "OTHER", .count_needed = 4, .threshold_percent = 0 },
    [KEYWORD_SILENCE] = { .name = "SILENCE", .count_needed = 4, .threshold_percent = 0 },
    [KEYWORD_SWITCH] = { .name = "SWITCH", .count_needed = 2, .threshold_percent = 0 },
};

static keyword_runtime_ctx_t s_keyword_runtime_ctx;
static keyword_phrase_ctx_t  s_keyword_phrase_ctx;

//////////////////////////////////////////////////////////////////////////////

int main()
{
#if !DEMO_FINAL_DETECTIONS_ONLY
    printk("Starting Wakeword gated KWS Application...\n");
    printk("\tWakeword: %s\n", MODEL_WAKEWORD_LABEL);
    printk("\tSupported commands: MUSIC DOWN, MUSIC UP, NEXT TRACK, ");
    printk("PREVIOUS TRACK, PLAY MUSIC, STOP MUSIC\n");
    printk("\tIgnored classes: %s, %s\n",
           KEYWORD_CLASSES_CFG[KEYWORD_OTHER].name,
           KEYWORD_CLASSES_CFG[KEYWORD_SILENCE].name);

    nrf_edgeai_rt_version_t libver = nrf_edgeai_runtime_version();
    printk("Nordic Edge AI Library version: %d.%d.%d\n",
           libver.field.major,
           libver.field.minor,
           libver.field.patch);
#endif

    nrf_edgeai_t* p_wakeword_model = nrf_edgeai_user_model_92597();
    nrf_edgeai_t* p_kws_model      = nrf_edgeai_user_model_kws();

    // Initialize wakeword model
    nrf_edgeai_err_t res = nrf_edgeai_init(p_wakeword_model);
    __ASSERT(res == NRF_EDGEAI_ERR_SUCCESS,
             "Failed to initialize Wakeword Edge AI model, error code: %d\n",
             res);

    // Initialize keyword spotting model
    res = nrf_edgeai_init(p_kws_model);
    __ASSERT(res == NRF_EDGEAI_ERR_SUCCESS,
             "Failed to initialize KWS Edge AI model, error code: %d\n",
             res);

    // Initialize PDM microphone
    int err = dmic_init();
    __ASSERT(err == 0, "Failed to initialize DMIC, error code: %d\n", err);

    err = dmic_start();
    __ASSERT(err == 0, "Failed to start DMIC, error code: %d\n", err);

    void*  audio_buffer;
    size_t audio_buffer_size;

    const int32_t read_timeout   = 100;
    uint32_t      kws_start_time = 0;

    application_state_t app_state = APP_WAITING_FOR_WAKEWORD;
#if !DEMO_FINAL_DETECTIONS_ONLY
    printk("Waiting for wakeword...\n");
#endif

    while (true)
    {
        // Read audio data from DMIC,
        err = dmic_read_buffer(&audio_buffer, &audio_buffer_size, read_timeout);
        if (err < 0)
        {
            printk("Failed to read from DMIC (err %d)\n", err);
            continue;
        }

        size_t samples_num = audio_buffer_size / DMIC_SAMPLE_BYTES;

        switch (app_state)
        {
            case APP_WAITING_FOR_WAKEWORD:
            {
                res = nrf_edgeai_feed_inputs(p_wakeword_model, audio_buffer, samples_num);

                if (res != NRF_EDGEAI_ERR_SUCCESS) { break; }

                res = nrf_edgeai_run_inference(p_wakeword_model);

                if (res != NRF_EDGEAI_ERR_SUCCESS) { break; }

                uint16_t predicted_class = p_wakeword_model->decoded_output.classif.predicted_class;
                flt32_t  probability =
                    p_wakeword_model->decoded_output.classif.probabilities.p_f32[predicted_class];

                if (is_wakeword_detected(probability))
                {
#if !DEMO_FINAL_DETECTIONS_ONLY
                    printk("Wakeword detected! Starting keyword spotting...\n");
#endif
                    // Re-initialize KWS model to reset its state after wakeword detection
                    nrf_edgeai_model_axon_init_persistent_vars(p_kws_model);
                    reset_keyword_detection_state();
                    kws_start_time = k_uptime_get_32();
                    app_state      = APP_WAITING_FOR_KEYWORDS;
                }
            }
            break;
            case APP_WAITING_FOR_KEYWORDS:
            {
                if (k_uptime_get_32() - kws_start_time > KEYWORD_SPOTTING_TIMEOUT_MS)
                {
#if !DEMO_FINAL_DETECTIONS_ONLY
                    printk("Keyword spotting timeout\nWaiting for wakeword...\n");
#endif
                    // Re-initialize WW model to reset its state after keyword detection
                    nrf_edgeai_model_axon_init_persistent_vars(p_wakeword_model);
                    reset_keyword_detection_state();
                    app_state = APP_WAITING_FOR_WAKEWORD;
                    break;
                }

                res = nrf_edgeai_feed_inputs(p_kws_model, audio_buffer, samples_num);

                if (res != NRF_EDGEAI_ERR_SUCCESS) { break; }

                res = nrf_edgeai_run_inference(p_kws_model);

                if (res != NRF_EDGEAI_ERR_SUCCESS) { break; }

                uint16_t predicted_class = p_kws_model->decoded_output.classif.predicted_class;
                flt32_t  probability =
                    p_kws_model->decoded_output.classif.probabilities.p_f32[predicted_class];

                const char* first_keyword_name      = NULL;
                const char* second_keyword_name     = NULL;
                bool        has_second_keyword      = false;
                uint8_t     average_probability_pct = 0;

                if (try_detect_keyword_command(predicted_class,
                                               probability,
                                               &first_keyword_name,
                                               &second_keyword_name,
                                               &has_second_keyword,
                                               &average_probability_pct))
                {

                    printk("Keyword detected: %s, %d %%\n",
                            first_keyword_name,
                            average_probability_pct);

                    /* Return to wakeword mode immediately after a valid command. */
                    nrf_edgeai_model_axon_init_persistent_vars(p_wakeword_model);
                    reset_keyword_detection_state();
                    app_state = APP_WAITING_FOR_WAKEWORD;

#if !DEMO_FINAL_DETECTIONS_ONLY
                    printk("Waiting for wakeword...\n");
#endif
                }
            }
            break;

            default:
                break;
        }

        dmic_free_buffer(audio_buffer);
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void reset_keyword_detection_state(void)
{
    memset(&s_keyword_runtime_ctx, 0, sizeof(s_keyword_runtime_ctx));
    memset(&s_keyword_phrase_ctx, 0, sizeof(s_keyword_phrase_ctx));
}

//////////////////////////////////////////////////////////////////////////////

static bool is_keyword_command_component(uint16_t predicted_class)
{
    return (predicted_class < KEYWORDS_cnt) && (predicted_class != KEYWORD_OTHER) &&
           (predicted_class != KEYWORD_SILENCE);
}

//////////////////////////////////////////////////////////////////////////////

static bool is_keyword_phrase_first_word(uint16_t predicted_class)
{
    return (predicted_class == KEYWORD_ON) || (predicted_class == KEYWORD_OFF) ||
           (predicted_class == KEYWORD_SWITCH);
}

//////////////////////////////////////////////////////////////////////////////


static const keyword_class_cfg_t* get_keyword_class_cfg(uint16_t predicted_class)
{
    __ASSERT(predicted_class < KEYWORDS_cnt, "Invalid keyword class: %u", predicted_class);
    return &KEYWORD_CLASSES_CFG[predicted_class];
}

//////////////////////////////////////////////////////////////////////////////

static bool is_keyword_probability_above_threshold(uint16_t predicted_class, flt32_t probability)
{
    const keyword_class_cfg_t* p_class_cfg = get_keyword_class_cfg(predicted_class);

    if (p_class_cfg->threshold_percent == 0)
    {
        return true;
    }

    return (probability * 100.0f) >= (flt32_t)p_class_cfg->threshold_percent;
}

//////////////////////////////////////////////////////////////////////////////

static bool is_wakeword_detected(flt32_t probability)
{
#define THRESHOLD  0.85f
#define POS_IN_ROW 14
    static bool   result_row[POS_IN_ROW];
    static size_t filled = 0;

    /* move result by 1 position*/
    for (int i = 0; i < POS_IN_ROW - 1; i++)
    {
        result_row[i] = result_row[i + 1];
    }

    result_row[POS_IN_ROW - 1] = (probability > THRESHOLD);

    if (filled < POS_IN_ROW)
    {
        filled++;
    }

    if (filled < POS_IN_ROW)
    {
        return false;
    }

    for (int i = 0; i < POS_IN_ROW; i++)
    {
        if (!result_row[i])
        {
#if PRINT_RAW_PROBABILITY
            printk("Wakeword window fill: %u \tprob : %0.3f\n", (unsigned int)filled, probability);
#endif
            return false;
        }
    }

#if PRINT_RAW_PROBABILITY
    printk("Wakeword window fill: %u \tprob : %0.3f\n", (unsigned int)filled, probability);
#endif

    return true;
}

//////////////////////////////////////////////////////////////////////////////

static bool try_detect_keyword_command(uint16_t     predicted_class,
                                       flt32_t      probability,
                                       const char** pp_first_keyword_name,
                                       const char** pp_second_keyword_name,
                                       bool*        p_has_second_keyword,
                                       uint8_t*     p_average_probability)
{
    const keyword_class_cfg_t* p_class_cfg          = get_keyword_class_cfg(predicted_class);
    const bool                 is_command_component =
        is_keyword_command_component(predicted_class);
    const bool passes_threshold =
        is_command_component && is_keyword_probability_above_threshold(predicted_class, probability);

    *pp_first_keyword_name = NULL;
    *pp_second_keyword_name = NULL;
    *p_has_second_keyword = false;
    *p_average_probability = 0;

    const uint32_t now_ms = k_uptime_get_32();

    if (s_keyword_runtime_ctx.wait_for_class_change)
    {
        if (predicted_class == s_keyword_runtime_ctx.blocked_class)
        {
            return false;
        }

        s_keyword_runtime_ctx.wait_for_class_change = false;
    }

    if (!passes_threshold)
    {
        if (is_command_component)
        {
            /* A low-confidence keyword should break the current confirmation streak. */
            s_keyword_runtime_ctx.has_active_class    = false;
            s_keyword_runtime_ctx.count               = 0;
            s_keyword_runtime_ctx.average_probability = 0;
        }

        if ((s_keyword_runtime_ctx.non_keyword_count == 0) ||
            (s_keyword_runtime_ctx.non_keyword_class != predicted_class))
        {
            s_keyword_runtime_ctx.non_keyword_class = predicted_class;
            s_keyword_runtime_ctx.non_keyword_count = 0;
        }

        s_keyword_runtime_ctx.non_keyword_count++;

        if (s_keyword_phrase_ctx.has_first_keyword &&
            (s_keyword_runtime_ctx.non_keyword_count < p_class_cfg->count_needed))
        {
            /* Keep the first word alive across short OTHER/SILENCE gaps. */
            s_keyword_phrase_ctx.detected_at_ms = now_ms;
        }

        if (s_keyword_runtime_ctx.non_keyword_count >= p_class_cfg->count_needed)
        {
            s_keyword_runtime_ctx.has_active_class    = false;
            s_keyword_runtime_ctx.count               = 0;
            s_keyword_runtime_ctx.non_keyword_count   = 0;
            s_keyword_runtime_ctx.non_keyword_class   = 0;
            s_keyword_runtime_ctx.average_probability = 0;
        }

        return false;
    }

    s_keyword_runtime_ctx.non_keyword_count = 0;

    if (!s_keyword_runtime_ctx.has_active_class ||
        (predicted_class != s_keyword_runtime_ctx.predicted_class))
    {
        s_keyword_runtime_ctx.has_active_class    = true;
        s_keyword_runtime_ctx.predicted_class     = predicted_class;
        s_keyword_runtime_ctx.count               = 0;
        s_keyword_runtime_ctx.non_keyword_count   = 0;
        s_keyword_runtime_ctx.non_keyword_class   = 0;
        s_keyword_runtime_ctx.average_probability = 0;
    }

    s_keyword_runtime_ctx.count++;
    s_keyword_runtime_ctx.average_probability +=
        (probability - s_keyword_runtime_ctx.average_probability) / s_keyword_runtime_ctx.count;

    if (s_keyword_phrase_ctx.has_first_keyword)
    {
        /* Any ongoing keyword activity extends the pairing window. */
        s_keyword_phrase_ctx.detected_at_ms = now_ms;
    }

#if !DEMO_FINAL_DETECTIONS_ONLY && PRINT_KEYWORD_CLASS_ACTIVITY
    printk("Keyword model class %s, count: %u \tprob : %0.3f\n",
           get_keyword_class_cfg(s_keyword_runtime_ctx.predicted_class)->name,
           (unsigned int)s_keyword_runtime_ctx.count,
           s_keyword_runtime_ctx.average_probability);
#endif

    if (s_keyword_runtime_ctx.count < p_class_cfg->count_needed)
    {
        return false;
    }

    const flt32_t detected_probability = s_keyword_runtime_ctx.average_probability;

    s_keyword_runtime_ctx.has_active_class      = false;
    s_keyword_runtime_ctx.count                 = 0;
    s_keyword_runtime_ctx.non_keyword_count     = 0;
    s_keyword_runtime_ctx.non_keyword_class     = 0;
    s_keyword_runtime_ctx.average_probability   = 0;
    s_keyword_runtime_ctx.wait_for_class_change = true;
    s_keyword_runtime_ctx.blocked_class         = predicted_class;


    if (is_keyword_phrase_first_word(predicted_class))
    {
        s_keyword_phrase_ctx.first_class       = predicted_class;
        s_keyword_phrase_ctx.first_probability = detected_probability;
        s_keyword_phrase_ctx.detected_at_ms    = now_ms;
#if !DEMO_FINAL_DETECTIONS_ONLY
        printk("First keyword detected: %s\n", p_class_cfg->name);
#endif
        return false;
    }

    /* Unsupported combinations are ignored, but the confirmed first word stays active. */
    return false;
}





// 0	OFF
// 1	ON
// 2	OTHER
// 3	SILENCE
// 4	SWITCH