#ifndef MOD_AUDIO_STREAM_H
#define MOD_AUDIO_STREAM_H

#include <switch.h>

#ifndef MOD_AUDIO_STREAM_NO_SPEEXDSP
#include <speex/speex_resampler.h>
#endif

#define MY_BUG_NAME "audio_stream"
#define MAX_SESSION_ID (256)
#define MAX_WS_URI (4096)
#define MAX_METADATA_LEN (8192)

#define EVENT_CONNECT           "mod_audio_stream::connect"
#define EVENT_DISCONNECT        "mod_audio_stream::disconnect"
#define EVENT_ERROR             "mod_audio_stream::error"
#define EVENT_JSON              "mod_audio_stream::json"
#define EVENT_PLAY              "mod_audio_stream::play"

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, const char* json);

struct private_data {
    switch_mutex_t *mutex;
    char sessionId[MAX_SESSION_ID];
#ifndef MOD_AUDIO_STREAM_NO_SPEEXDSP
    SpeexResamplerState *resampler;
    SpeexResamplerState *in_resampler;
#else
    void *resampler;
    void *in_resampler;
#endif
    responseHandler_t responseHandler;
    void *pAudioStreamer;
    char ws_uri[MAX_WS_URI];
    int sampling;
    int channels;
    int session_sampling;
    int in_sampling;
    int audio_paused:1;
    int close_requested:1;
    int cleanup_started:1;
    char initialMetadata[8192];
    switch_buffer_t *sbuffer;
    switch_buffer_t *in_sbuffer;
         uint8_t write_tick_seen;
    int rtp_packets;
    int in_rtp_packets;

    /* Telemetry/debug counters (best-effort; not atomic) */
    uint64_t telemetry_tick;
    uint64_t out_sent_bytes;
    uint64_t in_recv_bytes;
    uint64_t in_drop_bytes;
    uint64_t in_inject_bytes;
    uint64_t in_underruns;

    /* Optional debug log file (per call). Enabled via channel var STREAM_DEBUG_LOG_FILE */
    char debug_log_file[1024];
};

typedef struct private_data private_t;

enum notifyEvent_t {
    CONNECT_SUCCESS,
    CONNECT_ERROR,
    CONNECTION_DROPPED,
    MESSAGE
};

#endif //MOD_AUDIO_STREAM_H
