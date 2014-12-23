#define PCM_CARD                0
#define PCM_CARD_HDMI           1

#define PCM_CARD_DEFAULT        PCM_CARD

#define MIXER_CARD              0

/* MultiMedia1 LP */
#define PCM_DEVICE_MM_LP        0
#define PCM_DEVICE_MM           1
#define PCM_DEVICE_MM2          2
#define PCM_DEVICE_MM_UL        3
#define PCM_DEVICE_SCO_OUT      4
#define PCM_DEVICE_SCO_IN       5

#define PCM_DEVICE_DEFAULT_OUT  PCM_DEVICE_MM_LP
#define PCM_DEVICE_DEFAULT_IN   PCM_DEVICE_MM_UL

struct pcm_config pcm_config_out = {
    .channels = 2,
    .rate = 44100,
    .period_size = 960,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 960 * 4,
};

struct pcm_config pcm_config_out_lp = {
    .channels = 2,
    .rate = 44100,
    .period_size = 960,
    .period_count = 8,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 960 * 4,
};

struct pcm_config pcm_config_in = {
    .channels = 1,
    .rate = 44100,
    .period_size = 960,
    .period_count = 2,
    .start_threshold = 1,
    .stop_threshold = 960 * 2,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_sco = {
    .channels = 1,
    .rate = 8000,
    .period_size = 256,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_hdmi = {
    .channels = 2,
    .rate = 48000,
    .period_size = 960,
    .period_count = 8,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 960 * 4,
};

