#include <string>
#include <cstring>
#include "mod_audio_stream.h"
#include "WebSocketClient.h"
#include <switch_json.h>
#include <fstream>
#include <switch_buffer.h>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <vector>
#include <memory>
#include <inttypes.h>
#include "base64.h"
#include "g711.h"

#include <cstdarg>
#include <ctime>

// Forward declaration so class methods above can call it.
namespace {
    inline void debug_file_log(private_t* tech_pvt, const char* fmt, ...);
}

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/

class AudioStreamer {
public:
    // Factory
    static std::shared_ptr<AudioStreamer> create(
        const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat,
        bool suppressLog, const char* extra_headers, const char* tls_cafile, const char* tls_keyfile, 
        const char* tls_certfile, bool tls_disable_hostname_validation) {

        std::shared_ptr<AudioStreamer> sp(new AudioStreamer(
            uuid, wsUri, callback, deflate, heart_beat,
            suppressLog, extra_headers, tls_cafile, tls_keyfile, 
            tls_certfile, tls_disable_hostname_validation
        ));

        sp->bindCallbacks(std::weak_ptr<AudioStreamer>(sp));

        sp->client.connect();

        return sp;
    }

    ~AudioStreamer()= default;

    void disconnect() {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting...\n");
        client.disconnect();
    }

    bool isConnected() {
        return client.isConnected();
    }

    void writeBinary(uint8_t* buffer, size_t len) {
        if(!this->isConnected()) return;
        client.sendBinary(buffer, len);
    }

    void writeText(const char* text) {
        if(!this->isConnected()) return;
        client.sendMessage(text, strlen(text));
    }

    void setBinaryHandler(std::function<void(const void*, size_t)> cb) {
        m_binaryHandler = std::move(cb);
    }

    void deleteFiles() {
        if(m_playFile >0) {
            for (const auto &fileName: m_Files) {
                remove(fileName.c_str());
            }
        }
    }

    void markCleanedUp() {
        m_cleanedUp.store(true, std::memory_order_release);
        client.setMessageCallback({});
        client.setOpenCallback({});
        client.setErrorCallback({});
        client.setCloseCallback({});
    }

    bool isCleanedUp() const {
        return m_cleanedUp.load(std::memory_order_acquire);
    }

private:
    // Ctor
    AudioStreamer(
        const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat,
        bool suppressLog, const char* extra_headers, const char* tls_cafile, const char* tls_keyfile, 
        const char* tls_certfile, bool tls_disable_hostname_validation
    ) : m_sessionId(uuid), m_notify(callback), m_suppress_log(suppressLog), 
        m_extra_headers(extra_headers), m_playFile(0) {

        WebSocketHeaders hdrs;
        WebSocketTLSOptions tls;

        if (m_extra_headers) {
            cJSON *headers_json = cJSON_Parse(m_extra_headers);
            if (headers_json) {
                cJSON *iterator = headers_json->child;
                while (iterator) {
                    if (iterator->type == cJSON_String && iterator->valuestring != nullptr) {
                        hdrs.set(iterator->string, iterator->valuestring);
                    }
                    iterator = iterator->next;
                }
                cJSON_Delete(headers_json);
            }
        }

        client.setUrl(wsUri);

        // Setup TLS options
        // NONE - disables validation
        // SYSTEM - uses the system CAs bundle
        if (tls_cafile) {
            tls.caFile = tls_cafile;
        }

        if (tls_keyfile) {
            tls.keyFile = tls_keyfile;
        }

        if (tls_certfile) {
            tls.certFile = tls_certfile;
        }

        tls.disableHostnameValidation = tls_disable_hostname_validation;
        client.setTLSOptions(tls);

        // Optional heart beat, sent every xx seconds when there is not any traffic
        // to make sure that load balancers do not kill an idle connection.
        if(heart_beat)
            client.setPingInterval(heart_beat);

        // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
        if(deflate)
            client.enableCompression(false);

        // Set extra headers if any
        if(!hdrs.empty())
            client.setHeaders(hdrs);
    }

    void bindCallbacks(std::weak_ptr<AudioStreamer> wp) {
        client.setMessageCallback([wp](const std::string& message) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;
            self->eventCallback(MESSAGE, message.c_str());
        });

        client.setBinaryCallback([wp](const void* data, size_t len) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;
            if (self->m_binaryHandler) {
                self->m_binaryHandler(data, len);
            }
        });

        client.setOpenCallback([wp]() {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "connected");
            char* json_str = cJSON_PrintUnformatted(root);

            self->eventCallback(CONNECT_SUCCESS, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);
        });

        client.setErrorCallback([wp](int code, const std::string& msg) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            /* Keep a human-readable line in FreeSWITCH logs (helps debug unexpected disconnects) */
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "mod_audio_stream ws error: session=%s code=%d msg=%s\n",
                              self->m_sessionId.c_str(),
                              code,
                              msg.c_str());

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "error");
            cJSON* message = cJSON_CreateObject();
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "error", msg.c_str());
            cJSON_AddItemToObject(root, "message", message);

            char* json_str = cJSON_PrintUnformatted(root);

            self->eventCallback(CONNECT_ERROR, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);
        });

        client.setCloseCallback([wp](int code, const std::string& reason) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            /* Keep a human-readable line in FreeSWITCH logs (helps debug unexpected disconnects) */
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "mod_audio_stream ws closed: session=%s code=%d reason=%s\n",
                              self->m_sessionId.c_str(),
                              code,
                              reason.c_str());

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "disconnected");
            cJSON* message = cJSON_CreateObject();
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "reason", reason.c_str());
            cJSON_AddItemToObject(root, "message", message);

            char* json_str = cJSON_PrintUnformatted(root);

            self->eventCallback(CONNECTION_DROPPED, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);
        });
    }

    switch_media_bug_t *get_media_bug(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if(!channel) {
            return nullptr;
        }
        auto *bug = (switch_media_bug_t *) switch_channel_get_private(channel, MY_BUG_NAME);
        return bug;
    }

    inline void media_bug_close(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if(bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            tech_pvt->close_requested = 1;
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
        }
    }

    inline void send_initial_metadata(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if(bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            if(tech_pvt && strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                          "sending initial metadata %s\n", tech_pvt->initialMetadata);
                writeText(tech_pvt->initialMetadata);
            }
        }
    }

    void eventCallback(notifyEvent_t event, const char* message) {
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        std::string msg = message ? std::string(message) : std::string();

        if(psession) {
            switch (event) {
                case CONNECT_SUCCESS:
                    {
                        auto *bug = get_media_bug(psession);
                        if (bug) {
                            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
                            debug_file_log(tech_pvt, "ws CONNECT_SUCCESS payload=%s", msg.c_str());
                        }
                    }
                    send_initial_metadata(psession);
                    m_notify(psession, EVENT_CONNECT, msg.c_str());
                    break;
                case CONNECTION_DROPPED:
                    // Log full payload to capture close code/reason even if libwsc doesn't invoke close callback.
                    // message is typically a JSON string: {"status":"disconnected","message":{"code":X,"reason":"..."}}
                    // NOTE: keep the text unique so we can confirm the running module binary is updated.
                    {
                        auto *bug = get_media_bug(psession);
                        if (bug) {
                            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
                            debug_file_log(tech_pvt, "ws CONNECTION_DROPPED payload=%s", msg.c_str());
                        }
                    }
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
                                      "(%s) MOD_AUDIO_STREAM_V2 ws dropped payload=%s\n",
                                      m_sessionId.c_str(),
                                      msg.c_str());
                    m_notify(psession, EVENT_DISCONNECT, msg.c_str());
                    break;
                case CONNECT_ERROR:
                    {
                        auto *bug = get_media_bug(psession);
                        if (bug) {
                            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
                            debug_file_log(tech_pvt, "ws CONNECT_ERROR payload=%s", msg.c_str());
                        }
                    }
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                    m_notify(psession, EVENT_ERROR, msg.c_str());

                    media_bug_close(psession);

                    break;
                case MESSAGE:
                    if(processMessage(psession, msg) != SWITCH_TRUE) {
                        m_notify(psession, EVENT_JSON, msg.c_str());
                    }
                    if(!m_suppress_log)
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "response: %s\n", msg.c_str());
                    break;
            }
            switch_core_session_rwunlock(psession);
        }
    }

    switch_bool_t processMessage(switch_core_session_t* session, std::string& message) {
        cJSON* json = cJSON_Parse(message.c_str());
        switch_bool_t status = SWITCH_FALSE;
        if (!json) {
            return status;
        }
        const char* jsType = cJSON_GetObjectCstr(json, "type");
        if(jsType && strcmp(jsType, "streamAudio") == 0) {
            cJSON* jsonData = cJSON_GetObjectItem(json, "data");
            if(jsonData) {
                cJSON* jsonFile = nullptr;
                cJSON* jsonAudio = cJSON_DetachItemFromObject(jsonData, "audioData");
                const char* jsAudioDataType = cJSON_GetObjectCstr(jsonData, "audioDataType");
                std::string fileType;
                int sampleRate;
                if (0 == strcmp(jsAudioDataType, "raw")) {
                    cJSON* jsonSampleRate = cJSON_GetObjectItem(jsonData, "sampleRate");
                    sampleRate = jsonSampleRate && jsonSampleRate->valueint ? jsonSampleRate->valueint : 0;
                    std::unordered_map<int, const char*> sampleRateMap = {
                            {8000, ".r8"},
                            {16000, ".r16"},
                            {24000, ".r24"},
                            {32000, ".r32"},
                            {48000, ".r48"},
                            {64000, ".r64"}
                    };
                    auto it = sampleRateMap.find(sampleRate);
                    fileType = (it != sampleRateMap.end()) ? it->second : "";
                } else if (0 == strcmp(jsAudioDataType, "wav")) {
                    fileType = ".wav";
                } else if (0 == strcmp(jsAudioDataType, "mp3")) {
                    fileType = ".mp3";
                } else if (0 == strcmp(jsAudioDataType, "ogg")) {
                    fileType = ".ogg";
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - unsupported audio type: %s\n",
                                      m_sessionId.c_str(), jsAudioDataType);
                }

                if(jsonAudio && jsonAudio->valuestring != nullptr && !fileType.empty()) {
                    char filePath[256];
                    std::string rawAudio;
                    try {
                        rawAudio = base64_decode(jsonAudio->valuestring);
                    } catch (const std::exception& e) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - base64 decode error: %s\n",
                                          m_sessionId.c_str(), e.what());
                        cJSON_Delete(jsonAudio); cJSON_Delete(json);
                        return status;
                    }
                    switch_snprintf(filePath, 256, "%s%s%s_%d.tmp%s", SWITCH_GLOBAL_dirs.temp_dir,
                                    SWITCH_PATH_SEPARATOR, m_sessionId.c_str(), m_playFile++, fileType.c_str());
                    std::ofstream fstream(filePath, std::ofstream::binary);
                    fstream << rawAudio;
                    fstream.close();
                    m_Files.insert(filePath);
                    jsonFile = cJSON_CreateString(filePath);
                    cJSON_AddItemToObject(jsonData, "file", jsonFile);
                }

                if(jsonFile) {
                    char *jsonString = cJSON_PrintUnformatted(jsonData);
                    m_notify(session, EVENT_PLAY, jsonString);
                    message.assign(jsonString);
                    free(jsonString);
                    status = SWITCH_TRUE;
                }
                if (jsonAudio)
                    cJSON_Delete(jsonAudio);

            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - no data in streamAudio\n", m_sessionId.c_str());
            }
        }
        cJSON_Delete(json);
        return status;
    }

private:
    std::string m_sessionId;
    responseHandler_t m_notify;
    WebSocketClient client;
    bool m_suppress_log;
    const char* m_extra_headers;
    int m_playFile;
    std::unordered_set<std::string> m_Files;
    std::atomic<bool> m_cleanedUp{false};
    std::function<void(const void*, size_t)> m_binaryHandler;
};


namespace {

    inline void debug_file_log(private_t* tech_pvt, const char* fmt, ...) {
        if (!tech_pvt) return;
        if (tech_pvt->debug_log_file[0] == '\0') return;

        FILE* fp = fopen(tech_pvt->debug_log_file, "a");
        if (!fp) return;

        // Timestamp (local time)
        char ts[64];
        const time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

        fprintf(fp, "[%s] [%s] ", ts, tech_pvt->sessionId);

        va_list ap;
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);

        fputc('\n', fp);
        fclose(fp);
    }

    // Lightweight, rate-limited counters/telemetry per call.
    // Goal: make it obvious in FreeSWITCH logs whether we are:
    //  - sending outbound audio to WS
    //  - receiving inbound audio from WS
    //  - injecting inbound audio into the call (or inserting silence on underrun)
    inline void log_telemetry(private_t* tech_pvt, switch_core_session_t* session, const char* tag,
                              uint64_t value1 = 0, uint64_t value2 = 0) {
        if (!tech_pvt) return;
        // Log at most ~1/sec per session (50 callbacks/sec at 20ms, so 50 ticks ~= 1s).
        tech_pvt->telemetry_tick++;
        if ((tech_pvt->telemetry_tick % 50) != 0) return;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "(%s) %s v1=%" PRIu64 " v2=%" PRIu64 "\n",
                          tech_pvt->sessionId, tag,
                          (uint64_t)value1, (uint64_t)value2);
    }

    // G.711 encoders live in g711.h so standalone tests can validate the same implementation.

    inline void enqueue_inbound_audio(private_t* tech_pvt, const uint8_t* data, size_t len) {
        if (!tech_pvt || !data || !len) return;
        if (tech_pvt->cleanup_started) return;
        if (!tech_pvt->in_sbuffer) return;

        // Never block WS thread; if mutex is busy, drop.
        if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            return;
        }

        const switch_size_t freespace = switch_buffer_freespace(tech_pvt->in_sbuffer);
        if (freespace < (switch_size_t)len) {
            // bounded jitter buffer: drop newest inbound audio when full
            tech_pvt->in_drop_bytes += (switch_size_t)len;
            debug_file_log(tech_pvt, "ws.in drop len=%zu freespace=%u inuse=%u drop_total=%" PRIu64,
                           len,
                           (unsigned int)freespace,
                           (unsigned int)switch_buffer_inuse(tech_pvt->in_sbuffer),
                           (uint64_t)tech_pvt->in_drop_bytes);
            switch_mutex_unlock(tech_pvt->mutex);
            return;
        }

        switch_buffer_write(tech_pvt->in_sbuffer, data, len);
        tech_pvt->in_recv_bytes += (switch_size_t)len;
        // File-only debug trace (WS thread has no session pointer safely)
        debug_file_log(tech_pvt, "ws.in recv len=%zu inuse=%u recv_total=%" PRIu64,
                       len,
                       (unsigned int)switch_buffer_inuse(tech_pvt->in_sbuffer),
                       (uint64_t)tech_pvt->in_recv_bytes);
        // Can't log here easily with a session pointer (WS thread), so just count.
        switch_mutex_unlock(tech_pvt->mutex);
    }

    switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri,
                                     uint32_t sampling, int desiredSampling, int channels, char *metadata, responseHandler_t responseHandler,
                                     int deflate, int heart_beat, bool suppressLog, int rtp_packets, const char* extra_headers,
                                     const char *tls_cafile, const char *tls_keyfile, const char *tls_certfile, 
                                     bool tls_disable_hostname_validation)
    {
        int err; //speex

        switch_memory_pool_t *pool = switch_core_session_get_pool(session);

        memset(tech_pvt, 0, sizeof(private_t));

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
        strncpy(tech_pvt->ws_uri, wsUri, MAX_WS_URI);
        tech_pvt->sampling = desiredSampling;
    tech_pvt->session_sampling = (int)sampling;
    tech_pvt->in_sampling = 0;
        tech_pvt->responseHandler = responseHandler;
        tech_pvt->rtp_packets = rtp_packets;
        tech_pvt->channels = channels;
        tech_pvt->audio_paused = 0;

        // Optional file logging: STREAM_DEBUG_LOG_FILE can be a path, or a directory.
        // If it's a directory, we'll create <dir>/mod_audio_stream_<uuid>.log
        tech_pvt->debug_log_file[0] = '\0';
        {
            switch_channel_t *channel = switch_core_session_get_channel(session);
            const char *dbg = channel ? switch_channel_get_variable(channel, "STREAM_DEBUG_LOG_FILE") : nullptr;
            if (!zstr(dbg)) {
                // Heuristic: treat as directory if ends with '/'
                const size_t n = strlen(dbg);
                if (dbg[n - 1] == '/') {
                    switch_snprintf(tech_pvt->debug_log_file, sizeof(tech_pvt->debug_log_file),
                                    "%smod_audio_stream_%s.log", dbg, tech_pvt->sessionId);
                } else {
                    strncpy(tech_pvt->debug_log_file, dbg, sizeof(tech_pvt->debug_log_file) - 1);
                }
            }
        }

        debug_file_log(tech_pvt, "init ws=%s session_sampling=%d desired_sampling=%d channels=%d rtp_packets=%d",
                       wsUri ? wsUri : "(null)",
                       tech_pvt->session_sampling,
                       desiredSampling,
                       channels,
                       rtp_packets);

        if (metadata) strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN);

        tech_pvt->in_rtp_packets = (tech_pvt->in_rtp_packets > 0 ? tech_pvt->in_rtp_packets : 10);

        const size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);
        const size_t in_buflen = (FRAME_SIZE_8000 * (size_t)tech_pvt->session_sampling / 8000 * (size_t)channels * (size_t)tech_pvt->in_rtp_packets);
        
        auto sp = AudioStreamer::create(tech_pvt->sessionId, wsUri, responseHandler, deflate, heart_beat,
                                        suppressLog, extra_headers, tls_cafile, tls_keyfile,
                                        tls_certfile, tls_disable_hostname_validation);

        // Bind inbound binary audio handler (expects raw PCM16 matching session sampling by default)
        sp->setBinaryHandler([tech_pvt](const void* data, size_t len) {
            enqueue_inbound_audio(tech_pvt, (const uint8_t*)data, len);
        });

        tech_pvt->pAudioStreamer = new std::shared_ptr<AudioStreamer>(sp);

        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);
        
        if (switch_buffer_create(pool, &tech_pvt->sbuffer, buflen) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                "%s: Error creating switch buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        if (switch_buffer_create(pool, &tech_pvt->in_sbuffer, in_buflen) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                "%s: Error creating inbound switch buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

#ifndef MOD_AUDIO_STREAM_NO_SPEEXDSP
        if (desiredSampling != sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n", tech_pvt->sessionId, sampling, desiredSampling);
            tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
            if (0 != err) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
        }
        else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
        }
#else
        if (desiredSampling != sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "(%s) desired sampling %u != session sampling %u but SpeexDSP is not available; sending native sampling\n", tech_pvt->sessionId, desiredSampling, sampling);
        }
        tech_pvt->resampler = nullptr;
#endif

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init\n", tech_pvt->sessionId);

    debug_file_log(tech_pvt, "stream_data_init done");

        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t* tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s destroy_tech_pvt\n", tech_pvt->sessionId);
        debug_file_log(tech_pvt, "destroy_tech_pvt");
#ifndef MOD_AUDIO_STREAM_NO_SPEEXDSP
        if (tech_pvt->resampler) {
            speex_resampler_destroy(tech_pvt->resampler);
            tech_pvt->resampler = nullptr;
        }
        if (tech_pvt->in_resampler) {
            speex_resampler_destroy(tech_pvt->in_resampler);
            tech_pvt->in_resampler = nullptr;
        }
#else
        tech_pvt->resampler = nullptr;
        tech_pvt->in_resampler = nullptr;
#endif
        if (tech_pvt->mutex) {
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }
    }

}

extern "C" {
    int validate_ws_uri(const char* url, char* wsUri) {
        const char* scheme = nullptr;
        const char* hostStart = nullptr;
        const char* hostEnd = nullptr;
        const char* portStart = nullptr;

        // Check scheme
        if (strncmp(url, "ws://", 5) == 0) {
            scheme = "ws";
            hostStart = url + 5;
        } else if (strncmp(url, "wss://", 6) == 0) {
            scheme = "wss";
            hostStart = url + 6;
        } else {
            return 0;
        }

        // Find host end or port start
        hostEnd = hostStart;
        while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
            if (!std::isalnum(*hostEnd) && *hostEnd != '-' && *hostEnd != '.') {
                return 0;
            }
            ++hostEnd;
        }

        // Check if host is empty
        if (hostStart == hostEnd) {
            return 0;
        }

        // Check for port
        if (*hostEnd == ':') {
            portStart = hostEnd + 1;
            while (*portStart && *portStart != '/') {
                if (!std::isdigit(*portStart)) {
                    return 0;
                }
                ++portStart;
            }
        }

        // Copy valid URI to wsUri
        std::strncpy(wsUri, url, MAX_WS_URI);
        return 1;
    }

    switch_status_t is_valid_utf8(const char *str) {
        switch_status_t status = SWITCH_STATUS_FALSE;
        while (*str) {
            if ((*str & 0x80) == 0x00) {
                // 1-byte character
                str++;
            } else if ((*str & 0xE0) == 0xC0) {
                // 2-byte character
                if ((str[1] & 0xC0) != 0x80) {
                    return status;
                }
                str += 2;
            } else if ((*str & 0xF0) == 0xE0) {
                // 3-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) {
                    return status;
                }
                str += 3;
            } else if ((*str & 0xF8) == 0xF0) {
                // 4-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) {
                    return status;
                }
                str += 4;
            } else {
                // invalid character
                return status;
            }
        }
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_send_text(switch_core_session_t *session, char* text) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_text failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        std::shared_ptr<AudioStreamer> streamer;

        switch_mutex_lock(tech_pvt->mutex);

        if (tech_pvt->pAudioStreamer) {
            auto sp_wrap = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
            if (sp_wrap && *sp_wrap) {
                streamer = *sp_wrap; // copy shared_ptr
            }
        }

        switch_mutex_unlock(tech_pvt->mutex);

        if (streamer) {
            streamer->writeText(text);
            return SWITCH_STATUS_SUCCESS;
        }

        return SWITCH_STATUS_FALSE;
    }

    switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_pauseresume failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        switch_core_media_bug_flush(bug);
        tech_pvt->audio_paused = pause;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        char *wsUri,
                                        int sampling,
                                        int channels,
                                        char* metadata,
                                        void **ppUserData)
    {
        int deflate = 0, heart_beat = 0;
        bool suppressLog = false;
        const char* buffer_size;
        const char* extra_headers;
        int rtp_packets = 1; //20ms burst
        const char* tls_cafile = NULL;;
        const char* tls_keyfile = NULL;;
        const char* tls_certfile = NULL;;
        bool tls_disable_hostname_validation = false;

        switch_channel_t *channel = switch_core_session_get_channel(session);

        if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE")) {
            deflate = 1;
        }

        if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG")) {
            suppressLog = true;
        }

        tls_cafile = switch_channel_get_variable(channel, "STREAM_TLS_CA_FILE");
        tls_keyfile = switch_channel_get_variable(channel, "STREAM_TLS_KEY_FILE");
        tls_certfile = switch_channel_get_variable(channel, "STREAM_TLS_CERT_FILE");

        if (switch_channel_var_true(channel, "STREAM_TLS_DISABLE_HOSTNAME_VALIDATION")) {
            tls_disable_hostname_validation = true;
        }

        const char* heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
        if (heartBeat) {
            char *endptr;
            long value = strtol(heartBeat, &endptr, 10);
            if (*endptr == '\0' && value <= INT_MAX && value >= INT_MIN) {
                heart_beat = (int) value;
            }
        }

        if ((buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
            int bSize = atoi(buffer_size);
            if(bSize % 20 != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "%s: Buffer size of %s is not a multiple of 20ms. Using default 20ms.\n",
                                  switch_channel_get_name(channel), buffer_size);
            } else if(bSize >= 20){
                rtp_packets = bSize/20;
            }
        }

        extra_headers = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");

        // allocate per-session tech_pvt
        auto* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));

        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
            return SWITCH_STATUS_FALSE;
        }
        {
            const char *in_pkts_s = switch_channel_get_variable(channel, "audio_stream_in_rtp_packets");
            if (in_pkts_s) {
                int in_pkts = atoi(in_pkts_s);
                if (in_pkts < 1) in_pkts = 1;
                if (in_pkts > 200) in_pkts = 200;
                tech_pvt->in_rtp_packets = in_pkts;
            }
        }

        if (SWITCH_STATUS_SUCCESS != stream_data_init(tech_pvt, session, wsUri, samples_per_second, sampling, channels, 
                                                        metadata, responseHandler, deflate, heart_beat, suppressLog, rtp_packets, 
                                                        extra_headers, tls_cafile, tls_keyfile, tls_certfile, tls_disable_hostname_validation)) {
            destroy_tech_pvt(tech_pvt);
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = tech_pvt;

        return SWITCH_STATUS_SUCCESS;
    }

    switch_bool_t stream_frame(switch_media_bug_t *bug) {
        auto *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) return SWITCH_TRUE;
        if (tech_pvt->audio_paused || tech_pvt->cleanup_started) return SWITCH_TRUE;

        switch_core_session_t *session = switch_core_media_bug_get_session(bug);
        
        std::shared_ptr<AudioStreamer> streamer;
        std::vector<std::vector<uint8_t>> pending_send;

        if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            return SWITCH_TRUE;
        }

        if (!tech_pvt->pAudioStreamer) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        auto sp_ptr = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
        if (!sp_ptr || !(*sp_ptr)) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        streamer = *sp_ptr;

        auto *resampler = tech_pvt->resampler;
        const int channels = tech_pvt->channels;
        const int rtp_packets = tech_pvt->rtp_packets;

        if (nullptr == resampler) {
            
            uint8_t data_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = {};
            frame.data = data_buf;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if (!frame.datalen) {
                    continue;
                }

                if (rtp_packets == 1) {
                    pending_send.emplace_back((uint8_t*)frame.data, (uint8_t*)frame.data + frame.datalen);
                    continue;
                }

                size_t freespace = switch_buffer_freespace(tech_pvt->sbuffer);
                
                if (freespace >= frame.datalen) {
                    switch_buffer_write(tech_pvt->sbuffer, static_cast<uint8_t *>(frame.data), frame.datalen);
                }

                if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                    switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                    if (inuse > 0) {
                        std::vector<uint8_t> tmp(inuse);
                        switch_buffer_read(tech_pvt->sbuffer, tmp.data(), inuse);
                        switch_buffer_zero(tech_pvt->sbuffer);
                        pending_send.emplace_back(std::move(tmp));
                    }
                }
            }
            
        }
#ifndef MOD_AUDIO_STREAM_NO_SPEEXDSP
        else {

            uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = {};
            frame.data = data;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if(!frame.datalen) {
                    continue;
                }

                const size_t freespace = switch_buffer_freespace(tech_pvt->sbuffer);
                spx_uint32_t in_len = frame.samples;
                spx_uint32_t out_len = (freespace / (tech_pvt->channels * sizeof(spx_int16_t)));
                
                if(out_len == 0) {
                    if(freespace == 0) {
                        switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                        if (inuse > 0) {
                            std::vector<uint8_t> tmp(inuse);
                            switch_buffer_read(tech_pvt->sbuffer, tmp.data(), inuse);
                            switch_buffer_zero(tech_pvt->sbuffer);
                            pending_send.emplace_back(std::move(tmp));
                        }
                    }
                    continue;
                }

                std::vector<spx_int16_t> out;
                out.resize((size_t)out_len * (size_t)channels);

                if(channels == 1) {
                    speex_resampler_process_int(resampler,
                                    0,
                                    (const spx_int16_t *)frame.data,
                                    &in_len,
                                    out.data(),
                                    &out_len);
                } else {
                    speex_resampler_process_interleaved_int(resampler,
                                    (const spx_int16_t *)frame.data,
                                    &in_len,
                                    out.data(),
                                    &out_len);
                }

                if(out_len > 0) {
                    const size_t bytes_written = (size_t)out_len * (size_t)channels * sizeof(spx_int16_t);

                    if (rtp_packets == 1) { //20ms packet
                        const uint8_t* p = (const uint8_t*)out.data();
                        pending_send.emplace_back(p, p + bytes_written);
                        continue;
                    }

                    if (bytes_written <= switch_buffer_freespace(tech_pvt->sbuffer)) {
                        switch_buffer_write(tech_pvt->sbuffer, (const uint8_t *)out.data(), bytes_written);
                    }
                }

                if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                    switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                    if (inuse > 0) {
                        std::vector<uint8_t> tmp(inuse);
                        switch_buffer_read(tech_pvt->sbuffer, tmp.data(), inuse);
                        switch_buffer_zero(tech_pvt->sbuffer);
                        pending_send.emplace_back(std::move(tmp));
                    }
                }
            }
    }
#else
    else {
        // SpeexDSP not available; resampling path is disabled.
    }
#endif
        
        switch_mutex_unlock(tech_pvt->mutex);
    
        if (!streamer || !streamer->isConnected()) return SWITCH_TRUE;

        for (auto &chunk : pending_send) {
            if (!chunk.empty()) {
                streamer->writeBinary(chunk.data(), chunk.size());
                tech_pvt->out_sent_bytes += (uint64_t)chunk.size();
            }
        }

        // Periodic outbound telemetry (approx once/sec).
        log_telemetry(tech_pvt, session, "ws.out", (uint64_t)tech_pvt->out_sent_bytes, (uint64_t)pending_send.size());

        return SWITCH_TRUE;
    }

    switch_bool_t stream_frame_write(switch_media_bug_t *bug) {
        auto *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) return SWITCH_TRUE;
        if (tech_pvt->audio_paused || tech_pvt->cleanup_started) return SWITCH_TRUE;

        switch_core_session_t *session = switch_core_media_bug_get_session(bug);

    // Our inbound WS contract is PCM16 (little-endian) at the session sampling.
    // But the write-replace frame might be coded (e.g., PCMA/PCMU). We must match
    // whatever format/size FreeSWITCH expects in wframe.
    const int channels = tech_pvt->channels ? tech_pvt->channels : 1;
    const int rate = tech_pvt->session_sampling ? tech_pvt->session_sampling : 8000;
    const switch_size_t samples_per_20ms = (switch_size_t)(rate / 50);
    const switch_size_t pcm_bytes_needed = samples_per_20ms * (switch_size_t)channels * sizeof(int16_t);

        if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            return SWITCH_TRUE;
        }

        // FreeSWITCH 1.10.x provides a write-replace frame for media bug WRITE.
        // We must fill exactly one frame worth of samples and then set it as the replace frame.
        switch_frame_t *wframe = switch_core_media_bug_get_write_replace_frame(bug);
        if (!wframe || !wframe->data) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        // Decide injection mode based on the replace frame size.
        // Typical sizes at 8kHz/20ms/mono:
        //  - L16: 320 bytes
        //  - PCMA/PCMU: 160 bytes
        const switch_size_t out_bytes_needed = (switch_size_t)(wframe->buflen);
        if (out_bytes_needed == 0) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

    // Default inject silence.
    // For G.711, silence isn't 0x00; but using 0x00 is an acceptable placeholder.
    // Better silence values can be added later if needed.
    const switch_size_t safe_out = (out_bytes_needed <= (switch_size_t)wframe->buflen) ? out_bytes_needed : (switch_size_t)wframe->buflen;
    memset(wframe->data, 0, safe_out);
    wframe->datalen = (uint32_t)safe_out;
    wframe->samples = (uint32_t)(samples_per_20ms * (switch_size_t)channels);

        /* One-time marker: prove WRITE replace callback is executing */
        if (session && !tech_pvt->write_tick_seen) {
            tech_pvt->write_tick_seen = 1;
            const switch_codec_t *wcodec_dbg = switch_core_session_get_write_codec(session);
            const char *cname_dbg = (wcodec_dbg && wcodec_dbg->implementation && wcodec_dbg->implementation->iananame)
                ? wcodec_dbg->implementation->iananame
                : "unknown";
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "mod_audio_stream pushback: first WRITE tick codec=%s safe_out=%u pcm_need=%u inuse=%u\n",
                              cname_dbg,
                              (unsigned int)safe_out,
                              (unsigned int)pcm_bytes_needed,
                              (unsigned int)(tech_pvt->in_sbuffer ? switch_buffer_inuse(tech_pvt->in_sbuffer) : 0));

            debug_file_log(tech_pvt, "WRITE first_tick codec=%s safe_out=%u pcm_need=%u inuse=%u",
                           cname_dbg,
                           (unsigned int)safe_out,
                           (unsigned int)pcm_bytes_needed,
                           (unsigned int)(tech_pvt->in_sbuffer ? switch_buffer_inuse(tech_pvt->in_sbuffer) : 0));
        }

        bool injected = false;

        if (tech_pvt->in_sbuffer) {
            const switch_size_t inuse = switch_buffer_inuse(tech_pvt->in_sbuffer);

            // Case 1: replace frame looks like linear PCM16
            if (safe_out == pcm_bytes_needed) {
                if (inuse >= pcm_bytes_needed) {
                    switch_buffer_read(tech_pvt->in_sbuffer, (uint8_t*)wframe->data, pcm_bytes_needed);
                    wframe->datalen = (uint32_t)pcm_bytes_needed;
                    wframe->samples = (uint32_t)(samples_per_20ms * (switch_size_t)channels);
                    tech_pvt->in_inject_bytes += (uint64_t)pcm_bytes_needed;
                    injected = true;
                } else {
                    tech_pvt->in_underruns++;
                }
            }
            // Case 2: replace frame looks like G.711 (PCMA/PCMU) 8-bit samples
            else if (safe_out == samples_per_20ms * (switch_size_t)channels) {
                if (inuse >= pcm_bytes_needed) {
                    // Read PCM16, encode to 8-bit.
                    std::vector<int16_t> pcm;
                    pcm.resize(samples_per_20ms * (switch_size_t)channels);
                    switch_buffer_read(tech_pvt->in_sbuffer, (uint8_t*)pcm.data(), pcm_bytes_needed);

                    // Decide PCMA vs PCMU. Most calls in your logs negotiate PCMA.
                    // We use the channel's current write codec name when available.
                    const switch_codec_t *wcodec = switch_core_session_get_write_codec(session);
                    const char *cname = (wcodec && wcodec->implementation && wcodec->implementation->iananame)
                        ? wcodec->implementation->iananame
                        : "";
                    const bool is_pcma = !strcasecmp(cname, "PCMA");
                    const bool is_pcmu = !strcasecmp(cname, "PCMU");

                    uint8_t *out = (uint8_t*)wframe->data;
                    const size_t out_samples = (size_t)(samples_per_20ms * (switch_size_t)channels);

                    if (is_pcma || (!is_pcma && !is_pcmu)) {
                        // default to PCMA to match your negotiated codec.
                        for (size_t i = 0; i < out_samples; i++) {
                            out[i] = mod_audio_stream_linear_to_alaw(pcm[i]);
                        }
                    } else {
                        for (size_t i = 0; i < out_samples; i++) {
                            out[i] = mod_audio_stream_linear_to_ulaw(pcm[i]);
                        }
                    }

                    wframe->datalen = (uint32_t)safe_out;
                    wframe->samples = (uint32_t)(samples_per_20ms * (switch_size_t)channels);
                    tech_pvt->in_inject_bytes += (uint64_t)safe_out;
                    injected = true;
                } else {
                    tech_pvt->in_underruns++;
                }
            }
            // Unknown replace frame sizing; keep silence.
            else {
                tech_pvt->in_underruns++;
            }
        }

        switch_core_media_bug_set_write_replace_frame(bug, wframe);

        // Rate-limited once per ~second.
        if (session) {
            const switch_size_t inuse_now = tech_pvt->in_sbuffer ? switch_buffer_inuse(tech_pvt->in_sbuffer) : 0;
            // v1/v2 meaning depends on tag.
            log_telemetry(tech_pvt, session,
                          injected ? "pushback.inject" : "pushback.silence",
                          (uint64_t)safe_out, (uint64_t)inuse_now);
            // Also periodically dump totals so you can correlate with WS server logs.
            log_telemetry(tech_pvt, session,
                          "pushback.totals",
                          (uint64_t)tech_pvt->in_recv_bytes,
                          (uint64_t)tech_pvt->in_inject_bytes);

            const switch_codec_t *wcodec = switch_core_session_get_write_codec(session);
            const char *cname = (wcodec && wcodec->implementation && wcodec->implementation->iananame)
                ? wcodec->implementation->iananame
                : "unknown";
            // v1=out_bytes_needed (expected), v2=pcm_bytes_needed (our inbound requirement)
            log_telemetry(tech_pvt, session,
                          cname,
                          (uint64_t)safe_out,
                          (uint64_t)pcm_bytes_needed);

            /* Extra human-readable periodic log (every ~second) */
            if ((tech_pvt->telemetry_tick % 50) == 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                  "mod_audio_stream pushback: %s safe_out=%u pcm_need=%u inuse=%u injected=%s recv=%" PRIu64 " inject=%" PRIu64 " underruns=%" PRIu64 "\n",
                                  cname,
                                  (unsigned int)safe_out,
                                  (unsigned int)pcm_bytes_needed,
                                  (unsigned int)inuse_now,
                                  injected ? "yes" : "no",
                                  (uint64_t)tech_pvt->in_recv_bytes,
                                  (uint64_t)tech_pvt->in_inject_bytes,
                                  (uint64_t)tech_pvt->in_underruns);

                debug_file_log(tech_pvt, "WRITE tick codec=%s injected=%s safe_out=%u pcm_need=%u inuse=%u recv=%" PRIu64 " inject=%" PRIu64 " underruns=%" PRIu64,
                               cname,
                               injected ? "yes" : "no",
                               (unsigned int)safe_out,
                               (unsigned int)pcm_bytes_needed,
                               (unsigned int)inuse_now,
                               (uint64_t)tech_pvt->in_recv_bytes,
                               (uint64_t)tech_pvt->in_inject_bytes,
                               (uint64_t)tech_pvt->in_underruns);
            }
        }

        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if(bug)
        {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            char sessionId[MAX_SESSION_ID];
            strcpy(sessionId, tech_pvt->sessionId);

            debug_file_log(tech_pvt, "cleanup start channelIsClosing=%d has_text=%s", channelIsClosing, text ? "yes" : "no");

            std::shared_ptr<AudioStreamer>* sp_wrap = nullptr;
            std::shared_ptr<AudioStreamer> streamer;

            switch_mutex_lock(tech_pvt->mutex);

            if (tech_pvt->cleanup_started) {
                switch_mutex_unlock(tech_pvt->mutex);
                return SWITCH_STATUS_SUCCESS;
            }
            tech_pvt->cleanup_started = 1;

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_session_cleanup\n", sessionId);

            switch_channel_set_private(channel, MY_BUG_NAME, nullptr);

            sp_wrap = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
            tech_pvt->pAudioStreamer = nullptr;

            if (sp_wrap && *sp_wrap) {
                streamer = *sp_wrap;
            }

            switch_mutex_unlock(tech_pvt->mutex);

            if (!channelIsClosing) {
                switch_core_media_bug_remove(session, &bug);
            }

            if (sp_wrap) {
                delete sp_wrap;
                sp_wrap = nullptr;
            }

            if(streamer) {
                streamer->deleteFiles();
                if (text) streamer->writeText(text);
                
                streamer->markCleanedUp();
                streamer->disconnect();
            }

            debug_file_log(tech_pvt, "cleanup done out_sent=%" PRIu64 " in_recv=%" PRIu64 " in_inject=%" PRIu64 " underruns=%" PRIu64 " drops=%" PRIu64,
                           (uint64_t)tech_pvt->out_sent_bytes,
                           (uint64_t)tech_pvt->in_recv_bytes,
                           (uint64_t)tech_pvt->in_inject_bytes,
                           (uint64_t)tech_pvt->in_underruns,
                           (uint64_t)tech_pvt->in_drop_bytes);

            if (tech_pvt->in_sbuffer) {
                switch_buffer_zero(tech_pvt->in_sbuffer);
            }

            destroy_tech_pvt(tech_pvt);

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) stream_session_cleanup: connection closed\n", sessionId);
            return SWITCH_STATUS_SUCCESS;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "stream_session_cleanup: no bug - websocket connection already closed\n");
        return SWITCH_STATUS_FALSE;
    }
}