#include "driver/i2s.h"
#include "knobes.h"
#include <atomic>
#include "radio_visualizer.h"

#include "AACDecoderHelix.h"
#include "MP3DecoderHelix.h"
#include <new>

static_assert(AUDIO_VOLUME_PERCENT >= 0 && AUDIO_VOLUME_PERCENT <= 100,
              "AUDIO_VOLUME_PERCENT must be between 0 and 100");
// Shared by BLE and the decoder task; app switches never reset this setting.
static std::atomic<uint8_t> audioVolumePercent{AUDIO_VOLUME_PERCENT};
// 0 silent, 1 vibration only, 2 speaker + vibration. Session setting, like volume.
static std::atomic<uint8_t> audioAlertMode{2};
static std::atomic<bool> bootWelcomeAudioActive{false};
static std::atomic<uint16_t> bootWelcomeAudioLevel{0};
static std::atomic<uint16_t> bootWelcomeRawPeak{0};
// Borrowed from the main task only until its welcome decoder is joined.
static int16_t* bootWelcomeStereo=nullptr;

// Welcome-only +6 dB speech gain with a soft knee instead of hard clipping.
// Apply the user's volume/mute afterward, exactly as for other audio.
static int16_t welcomeSpeechSample(int16_t sample) {
    int32_t magnitude=abs(int32_t(sample))*2;
    if(magnitude>24576)
        magnitude=24576+(magnitude-24576)*8191/(magnitude-24576+8191);
    return int16_t(sample<0?-magnitude:magnitude);
}

static unsigned long total_read_audio_ms = 0;
static unsigned long total_decode_audio_ms = 0;
static unsigned long total_play_audio_ms = 0;
static volatile unsigned long last_audio_output_ms = 0;
static volatile bool audio_stop_requested = false;
// Audio decoding must not outrank the ESP32 idle and WiFi housekeeping tasks.
// A high-priority decoder loop can starve IDLE0 and trigger task_wdt.
static constexpr UBaseType_t AUDIO_PLAYER_TASK_PRIORITY = 2;

static i2s_port_t _i2s_num;
static esp_err_t i2s_init(i2s_port_t i2s_num, uint32_t sample_rate,
                          int mck_io_num,   /*!< MCK in out pin. Note that ESP32 supports setting MCK on GPIO0/GPIO1/GPIO3 only*/
                          int bck_io_num,   /*!< BCK in out pin*/
                          int ws_io_num,    /*!< WS in out pin*/
                          int data_out_num, /*!< DATA out pin*/
                          int data_in_num   /*!< DATA in pin*/
)
{
    _i2s_num = i2s_num;

    esp_err_t ret_val = ESP_OK;

    i2s_config_t i2s_config;
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = sample_rate;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 8;
    i2s_config.dma_buf_len = 160;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 0;
    i2s_config.mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT;
    i2s_config.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    i2s_pin_config_t pin_config;
    pin_config.mck_io_num = mck_io_num;
    pin_config.bck_io_num = bck_io_num;
    pin_config.ws_io_num = ws_io_num;
    pin_config.data_out_num = data_out_num;
    pin_config.data_in_num = data_in_num;

    ret_val = i2s_driver_install(i2s_num, &i2s_config, 0, NULL);
    if (ret_val != ESP_OK) return ret_val;
    ret_val = i2s_set_pin(i2s_num, &pin_config);
    if (ret_val != ESP_OK) i2s_driver_uninstall(i2s_num);

    return ret_val;
}

static int _samprate = 0;
void resetSamprate()
{
    _samprate = 0;
}

static void applyAudioVolume(int16_t *samples, size_t sampleCount)
{
    const uint8_t volume = audioAlertMode.load(std::memory_order_relaxed)==2
        ? audioVolumePercent.load(std::memory_order_relaxed) : 0;
    if (volume == 100) return;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        samples[i] = static_cast<int16_t>(
            (static_cast<int32_t>(samples[i]) * volume) / 100);
    }
}

static void writeAudioFully(const int16_t* samples, size_t bytes) {
    size_t offset=0;
    uint32_t lastProgress=millis();
    while(offset<bytes && !audio_stop_requested) {
        size_t written=0;
        esp_err_t result=i2s_write(_i2s_num,reinterpret_cast<const uint8_t*>(samples)+offset,
                                   bytes-offset,&written,pdMS_TO_TICKS(20));
        offset+=written;
        if(written)last_audio_output_ms=lastProgress=millis();
        if(result!=ESP_OK && result!=ESP_ERR_TIMEOUT) {
            Serial.printf("Audio: I2S error %d after %u/%u bytes.\n",result,unsigned(offset),unsigned(bytes));
            audio_stop_requested=true;break;
        }
        if(millis()-lastProgress>=500) {
            Serial.println("Audio: I2S output stalled for 500 ms; stopping decoder.");
            audio_stop_requested=true;break;
        }
        if(offset<bytes)vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void aacAudioDataCallback(AACFrameInfo &info, int16_t *pwm_buffer, size_t len)
{
    unsigned long s = millis();
    if (_samprate != info.sampRateOut)
    {
        // log_i("bitRate: %d, nChans: %d, sampRateCore: %d, sampRateOut: %d, bitsPerSample: %d, outputSamps: %d, profile: %d, tnsUsed: %d, pnsUsed: %d",
        //       info.bitRate, info.nChans, info.sampRateCore, info.sampRateOut, info.bitsPerSample, info.outputSamps, info.profile, info.tnsUsed, info.pnsUsed);
        i2s_set_clk(_i2s_num, info.sampRateOut /* sample_rate */, info.bitsPerSample /* bits_cfg */, (info.nChans == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO /* channel */);
        _samprate = info.sampRateOut;
    }
    applyAudioVolume(pwm_buffer, len);
    radioVizAnalyze(pwm_buffer, len, info.nChans, info.sampRateOut);
    writeAudioFully(pwm_buffer, len * sizeof(int16_t));
    // log_i("len: %d, i2s_bytes_written: %d", len, i2s_bytes_written);
    total_play_audio_ms += millis() - s;
}
static uint32_t mp3DecodedAudioUs = 0;
static void mp3AudioDataCallback(MP3FrameInfo &info, int16_t *pwm_buffer, size_t len)
{
    if (info.samprate > 0 && info.nChans > 0)
        mp3DecodedAudioUs += static_cast<uint32_t>(
            uint64_t(len) * 1000000ULL / (info.samprate * info.nChans));
    unsigned long s = millis();
    if (_samprate != info.samprate)
    {
        Serial.printf("MP3: format %d bit/s, %d Hz, %d channels.\n",
                      info.bitrate, info.samprate, info.nChans);
        log_i("bitrate: %d, nChans: %d, samprate: %d, bitsPerSample: %d, outputSamps: %d, layer: %d, version: %d",
              info.bitrate, info.nChans, info.samprate, info.bitsPerSample, info.outputSamps, info.layer, info.version);
        i2s_set_clk(_i2s_num, info.samprate /* sample_rate */, info.bitsPerSample /* bits_cfg */, (info.nChans == 2 || bootWelcomeAudioActive.load()) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO /* channel */);
        _samprate = info.samprate;
    }
    const bool welcome=bootWelcomeAudioActive.load(std::memory_order_relaxed);
    if(welcome) {
        uint16_t peak=0;
        for(size_t i=0;i<len;++i){peak=max(peak,uint16_t(abs(int32_t(pwm_buffer[i]))));pwm_buffer[i]=welcomeSpeechSample(pwm_buffer[i]);}
        if(peak>bootWelcomeRawPeak.load())bootWelcomeRawPeak.store(peak);
    }
    applyAudioVolume(pwm_buffer, len);
    if(welcome&&len) {
        uint32_t magnitude=0;
        for(size_t i=0;i<len;++i)magnitude+=abs(int32_t(pwm_buffer[i]));
        bootWelcomeAudioLevel.store(min(uint64_t(32767),uint64_t(magnitude)*32767/(len*4500)),std::memory_order_release);
    }
    radioVizAnalyze(pwm_buffer, len, info.nChans, info.samprate);
    if(welcome&&info.nChans==1) {
        // Both amplifier slots receive mono speech, matching local WAV voice.
        int16_t* stereo=bootWelcomeStereo;
        if(!stereo){audio_stop_requested=true;return;}
        for(size_t offset=0;offset<len&&!audio_stop_requested;offset+=256) {
            size_t count=min(size_t(256),len-offset);
            for(size_t i=0;i<count;++i)stereo[i*2]=stereo[i*2+1]=pwm_buffer[offset+i];
            writeAudioFully(stereo,count*2*sizeof(int16_t));
        }
    } else writeAudioFully(pwm_buffer, len * sizeof(int16_t));
    // log_i("len: %d, i2s_bytes_written: %d", len, i2s_bytes_written);
    total_play_audio_ms += millis() - s;
}

static uint8_t _frame[MP3_MAX_FRAME_SIZE]; // MP3_MAX_FRAME_SIZE is smaller, so always use MP3_MAX_FRAME_SIZE

static libhelix::AACDecoderHelix _aac(aacAudioDataCallback);
TaskHandle_t aacTaskHandle = NULL;
TaskHandle_t mp3TaskHandle = NULL;

static void aac_player_task(void *pvParam)
{
    Stream *input = (Stream *)pvParam;

    int r;
    unsigned long ms = millis();
    while (!audio_stop_requested &&
           (r = input->readBytes(_frame, MP3_MAX_FRAME_SIZE)))
    {
        total_read_audio_ms += millis() - ms;
        ms = millis();

        size_t offset = 0;
        while (offset < (size_t)r && !audio_stop_requested)
        {
            size_t consumed = _aac.write(_frame + offset, (size_t)r - offset);
            if (consumed == 0)
            {
                log_e("AAC decoder made no progress; stopping audio task.");
                audio_stop_requested = true;
                break;
            }
            offset += consumed;
        }
        total_decode_audio_ms += millis() - ms;
        ms = millis();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    log_i("AAC stop.");

    aacTaskHandle = NULL;
    vTaskDelete(NULL);
}

// Helix defaults to 5,120 int16 samples (10 KB) for its PCM output buffer.
// A maximum-size stereo MP3 frame contains 1,152 samples per channel. Helix
// asserts outputSamps < maxPWMSize() (strictly less, not less-or-equal), so one
// guard sample is required above the 2,304-sample frame maximum. The smaller
// buffer lets MP3 radio decoding coexist with WiFi and runtime BLE.
class PixyToyMP3DecoderHelix : public libhelix::MP3DecoderHelix {
public:
    explicit PixyToyMP3DecoderHelix(libhelix::MP3DataCallback callback)
        : libhelix::MP3DecoderHelix(callback)
    {
        setMaxPWMSize(2305);
    }

    bool beginChecked()
    {
        // CommonHelix::begin uses throwing new[] and does not check decoder
        // allocation. WiFi/BLE reconnects can fragment or exhaust this heap.
        end();
        buffer_size = 0;
        frame_counter = 0;
        allocateDecoder();
        if (decoder == nullptr) { releaseMemory(); return false; }
        if (!frame_buffer) frame_buffer = new (std::nothrow) uint8_t[maxFrameSize()];
        if (!pwm_buffer) pwm_buffer = new (std::nothrow) short[maxPWMSize()];
        if (!frame_buffer || !pwm_buffer) { releaseMemory(); return false; }
        memset(frame_buffer, 0, maxFrameSize());
        memset(pwm_buffer, 0, maxPWMSize() * sizeof(short));
        active = true;
        return true;
    }

    void releaseMemory()
    {
        end();
        delete[] pwm_buffer;
        pwm_buffer = nullptr;
        delete[] frame_buffer;
        frame_buffer = nullptr;
    }
};

static PixyToyMP3DecoderHelix _mp3(mp3AudioDataCallback);
static void mp3_player_task(void *pvParam)
{
    Stream *input = (Stream *)pvParam;

    int r;
    unsigned long ms = millis();
    uint32_t reportAt = ms, reportAudioUs = mp3DecodedAudioUs, inputBytes = 0;
    while (!audio_stop_requested &&
           (r = input->readBytes(_frame, MP3_MAX_FRAME_SIZE)))
    {
        total_read_audio_ms += millis() - ms;
        inputBytes += r;
        ms = millis();

        size_t offset = 0;
        while (offset < (size_t)r && !audio_stop_requested)
        {
            size_t consumed = _mp3.write(_frame + offset, (size_t)r - offset);
            if (consumed == 0)
            {
                log_e("MP3 decoder made no progress; stopping audio task.");
                audio_stop_requested = true;
                break;
            }
            offset += consumed;
        }
        total_decode_audio_ms += millis() - ms;
        ms = millis();
        // Always give IDLE0 and the TCP/IP stack a chance to run. This is
        // required even when the stream continually has buffered data.
        vTaskDelay(pdMS_TO_TICKS(1));
        if (millis() - reportAt >= 10000UL) {
            Serial.printf("MP3: %u input bytes, %u ms decoded audio in %u ms wall time; heap %u.\n",
                          inputBytes, (mp3DecodedAudioUs - reportAudioUs) / 1000,
                          unsigned(millis() - reportAt), ESP.getFreeHeap());
            reportAt = millis(); reportAudioUs = mp3DecodedAudioUs; inputBytes = 0;
        }
    }
    Serial.printf("MP3: task ended (%s, produced audio: %s).\n",
                  audio_stop_requested ? "stop requested or decoder error" : "input exhausted or timed out",
                  last_audio_output_ms != 0 ? "yes" : "no");
    Serial.printf("MP3: final partial window: %u bytes, %u ms audio, %u ms wall.\n",
                  inputBytes, (mp3DecodedAudioUs-reportAudioUs)/1000,
                  unsigned(millis()-reportAt));

    mp3TaskHandle = NULL;
    vTaskDelete(NULL);
}

// Modify the aac_player_task_start and mp3_player_task_start functions to save task handles
static BaseType_t aac_player_task_start1(Stream *input, BaseType_t audioAssignCore)
{
    audio_stop_requested = false;
    _aac.setDelay(1);
    _aac.begin();

    return xTaskCreatePinnedToCore(
        aac_player_task,
        "AAC Player Task",
        2000, // Stack depth
        input,
        AUDIO_PLAYER_TASK_PRIORITY,
        &aacTaskHandle,
        audioAssignCore);
}

static BaseType_t mp3_player_task_start1(Stream *input, BaseType_t audioAssignCore)
{
    audio_stop_requested = false;
    _mp3.setDelay(1);
    if (!_mp3.beginChecked()) return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;

    return xTaskCreatePinnedToCore(
        mp3_player_task,
        "MP3 Player Task",
        2000, // Stack depth
        input,
        AUDIO_PLAYER_TASK_PRIORITY,
        &mp3TaskHandle,
        audioAssignCore);
}

static BaseType_t aac_player_task_start(Stream *input, BaseType_t audioAssignCore)
{
    audio_stop_requested = false;
    _aac.setDelay(1);
    _aac.begin();
    aacTaskHandle = NULL;

    return xTaskCreatePinnedToCore(
        (TaskFunction_t)aac_player_task,
        (const char *const)"AAC Player Task",
        (const uint32_t)2000,
        (void *const)input,
        AUDIO_PLAYER_TASK_PRIORITY,
        (TaskHandle_t *const)&aacTaskHandle,
        (const BaseType_t)audioAssignCore);
}

static BaseType_t mp3_player_task_start(Stream *input, BaseType_t audioAssignCore)
{
    audio_stop_requested = false;
    _mp3.setDelay(1);
    Serial.printf("MP3: heap before decoder: %u free, %u largest block.\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (!_mp3.beginChecked()) {
        Serial.println("MP3: insufficient decoder memory; retrying without reboot.");
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    Serial.printf("MP3: heap after decoder: %u free, %u largest block.\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    mp3TaskHandle = NULL;

    return xTaskCreatePinnedToCore(
        (TaskFunction_t)mp3_player_task,
        (const char *const)"MP3 Player Task",
        (const uint32_t)2000,
        (void *const)input,
        AUDIO_PLAYER_TASK_PRIORITY,
        (TaskHandle_t *const)&mp3TaskHandle,
        (const BaseType_t)audioAssignCore);
}

static void wait_audio_tasks_complete()
{
    while (aacTaskHandle != NULL || mp3TaskHandle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void AAC_End()
{
Serial.println("Before AAC end");
_aac.end();
    // Reset I2S Driver
    i2s_driver_uninstall(_i2s_num);

    // Reset Global or Static Variables
    _samprate = 0;
        // Reset Global Variables
    total_read_audio_ms = 0;
    total_decode_audio_ms = 0;
    total_play_audio_ms = 0;
      //  memset(buffer->data, 0, buffer->size);
    
    // Reset read and write pointers
  //  buffer->readPointer = 0;
    //buffer->writePointer = 0;
    
    // Reset any other buffer state variables as needed
    //buffer->isFull = false;
//Serial.println("After AAC end, before task delete");
//vTaskDelete(NULL);
//Serial.println("After task delete");


}
void Stop_Audio_Tasks()
{
    if (aacTaskHandle != NULL) {
        vTaskDelete(aacTaskHandle);
        aacTaskHandle = NULL;
        Serial.println("AAC Task Stopped.");
    }

    if (mp3TaskHandle != NULL) {
        vTaskDelete(mp3TaskHandle);
        mp3TaskHandle = NULL;
        Serial.println("MP3 Task Stopped.");
    }
}

void Request_Audio_Stop()
{
    audio_stop_requested = true;
}

bool Wait_Audio_Tasks_Stopped(unsigned long timeoutMs)
{
    unsigned long started = millis();
    while ((aacTaskHandle != NULL || mp3TaskHandle != NULL) &&
           millis() - started < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return aacTaskHandle == NULL && mp3TaskHandle == NULL;
}

void Reset_MP3_Decoder()
{
    _mp3.releaseMemory();
    resetSamprate();
    last_audio_output_ms = 0;
    audio_stop_requested = false;
}
void Audio_End()
{
    Serial.println("Stopping audio tasks...");
    Stop_Audio_Tasks();
    
    // The rest of the cleanup code as previously defined
}

void reset_Audio_play1() {
    // 1. Terminate audio playback tasks
    if (aacTaskHandle != NULL) {
        vTaskDelete(aacTaskHandle);
        aacTaskHandle = NULL;
        Serial.println("AAC Task Stopped.");
    }

    if (mp3TaskHandle != NULL) {
        vTaskDelete(mp3TaskHandle);
        mp3TaskHandle = NULL;
        Serial.println("MP3 Task Stopped.");
    }

    // 2. Cleanup I2S driver (no explicit queue to clean up in provided audio code)
    i2s_driver_uninstall(_i2s_num);

    // 3. Reset buffer pointers and state variables
    // Since there's no explicit buffer management shown for audio, this step would reset any
    // global/static variables related to audio processing back to their initial states.
    resetSamprate(); // Reset the sample rate adjustment flag

    // Additional state resets as necessary, e.g., reset timing measurements
    total_read_audio_ms = 0;
    total_decode_audio_ms = 0;
    total_play_audio_ms = 0;
   // _audioInputIndex = 0;
    // 4. Reallocation or re-initialization (if required)
    // If there were dynamic memory allocations specific to the audio tasks that needed to be
    // re-allocated or re-initialized, that would be done here. Based on the provided code,
    // it seems like the main setup work happens in the task start functions and I2S setup,
    // so reinitialization might involve preparing for the next audio playback session,
    // which could include re-configuring I2S with `i2s_init` or similar preparations.
}
