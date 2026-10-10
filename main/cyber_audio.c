// main/cyber_audio.c —— 见 cyber_audio.h 的分工与线程约束。
//
// 只有一个 I/O 执行者（worker），因此这里不需要为 PCM 读写加锁：
//   - s_phase 由外部（LVGL 任务）写、worker 读，单字节枚举，只表达"想去哪"；
//   - s_downlink 与统计计数用 s_cfg_lock 保护（几个字节，临界区内不做 I/O）；
//   - 播放帧队列是 SPSC 无锁队列：ws 事件回调写，worker 读（见 cyber_audio_util.h）。
#include "cyber_audio.h"

#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_audio_types.h"
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"

static const char *TAG = "cyber_audio";

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------
// worker 栈。PCM 与编码缓冲都放在静态区而不是栈上，所以这里主要是 opus 编解码调用
// 的深度 + I2S 读写。
//
// 历史教训（已解决，记录在案防止回退）：这里曾用 78/esp-opus，它用 -DUSE_ALLOCA
// 编译，opus/SILK 内部所有临时大数组都 alloca 到任务栈上——4KB 栈直接压穿
// （SP 越界 724B，addr2line 定位 silk_Encode），升到 16384 仍压穿（两次崩溃栈边界
// 都是 ~16KB，SP 越下边界 416B，定位到 silk_find_pitch_lags_FIX）。16KB 对 opus
// 一帧编码异常大，正是"临时数组全压栈"的症状。
//
// 现改用官方 espressif/esp_audio_codec（esp_opus_* API，C3 为官方预编译库）：它把
// opus 的 scratch 缓冲在 open() 时从【堆】分配，运行时编解码不再压任务栈。这与官方
// xiaozhi-esp32 参考实现一致（其 opus_codec 任务给 2048*12 = 24576B）。但本任务同时
// 承担 I2S 读写与编解码（参考拆成三个任务），故给 28672 留余量；上线后由
// cyber_audio_get_stats() 的栈高水位实测值（stack_high_water）往回收，不在这里猜。
#define CYBER_AUDIO_TASK_STACK   28672
// 优先级高于 cyber_ws(5)：上行每 60 ms 一帧是硬节奏，被网络任务长期压着会让
// 采集缓冲溢出，表现为说话断断续续。
#define CYBER_AUDIO_TASK_PRIO    6
// 编码码率。16 kHz 单声道语音 24 kbps 已经够清楚，再高只是多占带宽与排队时间。
#define CYBER_AUDIO_BITRATE      24000
// 复杂度 0。单核 160 MHz 上，libopus 的复杂度换来的是音乐质量，语音对话用不上，
// 而它直接决定每帧编码占用的 CPU 时间。
#define CYBER_AUDIO_COMPLEXITY   0
// 播放音量。
#define CYBER_AUDIO_VOLUME       80
// IDLE 阶段的轮询周期。这个值决定"按下说话键到真正开始采集"的启动延迟上限，
// 也决定空闲时的唤醒次数，20 ms 是两者的平衡点。
#define CYBER_AUDIO_IDLE_TICK_MS 20
// 播放阶段没有帧可播时的等待周期。服务端可能隔几十毫秒才推下一帧。
#define CYBER_AUDIO_IDLE_FRAME_MS 5

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_cfg_lock;              // 保护 s_downlink 与统计计数
static cyber_audio_format_t s_downlink;           // 当前生效的下行格式
// 外部（LVGL 任务）写、worker 读。volatile 是必须的：worker 里的采集/播放循环
// 靠它来判断"该不该继续"，若被编译器提到循环外，一次采集就会永不退出。
static volatile cyber_audio_phase_t s_phase;
static cyber_audio_queue_t s_rx_q;                // 播放帧（SPSC）
static cyber_audio_tx_fn s_tx_sink;
static TaskHandle_t s_task;
static bool s_inited;                             // init 已被调用过（幂等用）
// 模块是否【完全可用】：编码器/解码器可有可无，但锁与 worker 必须已经就绪。
// 与 s_inited 分开是有意的——s_inited 在函数入口就置位（那是幂等判据），而 s_ready
// 只在所有资源都拿到之后才置位，是给外部调用方看的。两者若合并，init 执行到一半
// 被 LVGL 任务插进来就会看到一个"以为能用了"的模块。
static bool s_ready;

// worker 私有：编解码器句柄与 PCM 缓冲。PCM 缓冲刻意放静态区——只有一个 worker，
// 放栈上等于让每个 60 ms 周期都多背 4.7 KB 的栈开销。
//
// 编解码器用官方 esp_audio_codec 的句柄（void*）。与旧版 78/esp-opus 的"手动共享
// 工作区"不同：官方句柄在 open() 时从堆分配自己的 scratch，close() 时归还。我们仍
// 保留半双工按需开关（进采集 open 编码器、离开时 close 归还；进播放 open 解码器、
// 离开时 close 归还），让空闲期把内存还给系统——这是 OTA 复查与 WS 会话共存的前提。
static void *s_enc;
static uint32_t s_enc_rate;
static void *s_dec;
static uint32_t s_dec_rate;
static int16_t s_pcm_in[CYBER_WS_FRAME_SAMPLES];                 // 1920 字节
static uint8_t s_enc_out[CYBER_WS_MAX_OPUS_FRAME];               // 512 字节
static int16_t s_pcm_out[CYBER_AUDIO_MAX_PCM_BYTES / 2u];        // 2880 字节

// release_* 在 ensure_* 里被用到（半双工换挡兜底），而定义在其后，这里前向声明。
static void release_encoder(void);
static void release_decoder(void);

static uint32_t s_tx_frames, s_tx_dropped, s_encode_errors;
static uint32_t s_rx_frames, s_decode_errors, s_write_errors, s_read_errors;
// 编解码器【分配】失败次数。与 encode/decode_errors 分开：那两个是"编解码算错了"，
// 这个是"内存不够，压根没建起来"。混在一起会让一次内存事故伪装成编解码器故障。
static uint32_t s_alloc_failures;

// 当前 codec 采样率，-1 表示还没配置过。
static int32_t s_codec_hz = -1;

// ---------------------------------------------------------------------------
// 格式与编解码器
// ---------------------------------------------------------------------------
// 把 codec 切到指定采样率。只能在 worker 里调用：BSP 的格式切换要求串行化，
// 且切换前必须已经停止所有 PCM 读写。
static bool ensure_codec(uint32_t hz)
{
    if (s_codec_hz == (int32_t)hz) return true;
    const esp_err_t err = bsp_audio_set_format(hz, 16, 1);
    if (err != ESP_OK) {
        // 不缓存失败：下次进入这个阶段还会再试一次。采样率不受支持是硬故障，
        // 但把它变成"永久不可用"会让一次偶发的 codec 错误变成不可恢复。
        ESP_LOGE(TAG, "codec 切到 %u Hz 失败：%s", (unsigned)hz, esp_err_to_name(err));
        return false;
    }
    s_codec_hz = (int32_t)hz;
    return true;
}

static bool ensure_encoder(void)
{
    if (s_enc != NULL) return true;

    // 半双工换挡兜底：挂上编码器前确保解码器已退场。正常流程两者本就不重叠，这句是
    // 给"播放中被按键打断"这类换挡兜底，避免两个句柄同时占堆（各占 ~20KB）。
    release_decoder();

    esp_opus_enc_config_t cfg = {
        .sample_rate    = (int)CYBER_WS_SAMPLE_RATE,
        .channel        = (int)CYBER_WS_CHANNELS,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate        = CYBER_AUDIO_BITRATE,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity     = CYBER_AUDIO_COMPLEXITY,
        // 丢包补偿：单帧丢失时解码端能靠 FEC 续上一点，而不是直接断一下。
        .enable_fec     = true,
        // 关掉 DTX：省下的那点带宽换不来"忽有忽无"的上行节奏给服务端 VAD 带来的麻烦。
        .enable_dtx     = false,
        .enable_vbr     = true,
    };
    void *enc = NULL;
    // 官方句柄在 open() 时从堆分配自己的 scratch（不再是 78/esp-opus 的压栈模式）。
    const esp_audio_err_t err = esp_opus_enc_open(&cfg, sizeof(cfg), &enc);
    if (err != ESP_AUDIO_ERR_OK || enc == NULL) {
        // 走到这里就是"内存不够"（句柄内存由 open() 现分配），所以日志直接报堆状态。
        s_alloc_failures++;
        ESP_LOGE(TAG, "Opus 编码器 open 失败：%d（第 %u 次，free=%u largest=%u）",
                 (int)err, (unsigned)s_alloc_failures,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    s_enc = enc;
    s_enc_rate = CYBER_WS_SAMPLE_RATE;
    ESP_LOGI(TAG, "Opus 编码器就绪：%u Hz %u 声道，%d bps，复杂度 %d（free=%u largest=%u）",
             (unsigned)s_enc_rate, (unsigned)CYBER_WS_CHANNELS, CYBER_AUDIO_BITRATE,
             CYBER_AUDIO_COMPLEXITY,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_alloc_failures = 0;
    return true;
}

// 解码器必须按服务端的下行采样率实例化：libopus 的解码器是按采样率实例化的，
// 拿 16 kHz 的解码器去解 24 kHz 的流不会报错，只会把音调和时长一起弄错。
//
// 与编码器互斥（半双工，同一时刻只有一个角色）。句柄内存由 open() 从堆分配。
static bool ensure_decoder(uint32_t rate)
{
    if (s_dec != NULL && s_dec_rate == rate) return true;

    // 换角色：挂上解码器就意味着编码器退场，把编码器占的堆还给系统。
    release_encoder();

    esp_opus_dec_cfg_t cfg = {
        .sample_rate    = rate,
        .channel        = (uint8_t)CYBER_WS_CHANNELS,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    void *dec = NULL;
    const esp_audio_err_t err = esp_opus_dec_open(&cfg, sizeof(cfg), &dec);
    if (err != ESP_AUDIO_ERR_OK || dec == NULL) {
        s_alloc_failures++;
        ESP_LOGE(TAG, "Opus 解码器 open 失败（%u Hz）：%d（第 %u 次，free=%u largest=%u）",
                 (unsigned)rate, (int)err, (unsigned)s_alloc_failures,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    s_dec = dec;
    s_dec_rate = rate;
    ESP_LOGI(TAG, "Opus 解码器就绪：%u Hz %u 声道（free=%u largest=%u）",
             (unsigned)rate, (unsigned)CYBER_WS_CHANNELS,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

static cyber_audio_format_t get_downlink(void)
{
    cyber_audio_format_t f;
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    f = s_downlink;
    xSemaphoreGive(s_cfg_lock);
    return f;
}

// ---------------------------------------------------------------------------
// 编解码器的取用与归还（半双工内存策略的核心，见 audio_task 处的说明）
// ---------------------------------------------------------------------------
// release_* 调用 close()，把句柄 open() 时占的堆真正还给系统。这是"按需"策略的
// 另一半：进采集 open 编码器、离开时 close 归还；进播放 open 解码器、离开时 close
// 归还。半双工保证两者永不同时存在，所以峰值内存只是两者中较大的那个句柄，空闲期
// 完全归还——这是 OTA 复查（~40 KB）与 WS 会话（~36 KB）能并存的前提。
static void release_encoder(void)
{
    if (s_enc == NULL) return;
    esp_opus_enc_close(s_enc);
    s_enc = NULL;
    ESP_LOGI(TAG, "编码器退场（堆已归还，free=%u largest=%u）",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void release_decoder(void)
{
    if (s_dec == NULL) return;
    esp_opus_dec_close(s_dec);
    s_dec = NULL;
    ESP_LOGI(TAG, "解码器退场（堆已归还，free=%u largest=%u）",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// ---------------------------------------------------------------------------
// 采集 / 播放（只在 worker 里调用）
// ---------------------------------------------------------------------------
static void do_capture(void)
{
    // 一帧 60 ms。这里刻意不自己凑帧长：bsp_audio_read 会阻塞到读满，
    // 于是循环的节奏天然就是一帧的时间。
    while (s_phase == CYBER_AUDIO_CAPTURE) {
        if (bsp_audio_read(s_pcm_in, sizeof(s_pcm_in)) != ESP_OK) {
            s_read_errors++;
            vTaskDelay(pdMS_TO_TICKS(CYBER_AUDIO_IDLE_FRAME_MS));
            continue;
        }
        if (s_enc == NULL) continue;

        esp_audio_enc_in_frame_t in = {
            .buffer = (uint8_t *)s_pcm_in,
            .len    = sizeof(s_pcm_in),
        };
        esp_audio_enc_out_frame_t out = {
            .buffer        = s_enc_out,
            .len           = sizeof(s_enc_out),
            .encoded_bytes = 0,
        };
        const esp_audio_err_t err = esp_opus_enc_process(s_enc, &in, &out);
        if (err != ESP_AUDIO_ERR_OK) {
            s_encode_errors++;
            ESP_LOGW(TAG, "编码失败：%d", (int)err);
            continue;
        }
        const uint32_t n = out.encoded_bytes;
        // 0 表示这一帧没有可发的内容（DTX 静音帧）。发一个空包会浪费服务端一次解码。
        if (n == 0) continue;

        if (s_tx_sink != NULL) {
            if (s_tx_sink(s_enc_out, (size_t)n)) {
                s_tx_frames++;
            } else {
                s_tx_dropped++;
            }
        }
    }
}

static void do_playback(void)
{
    const cyber_audio_format_t fmt = get_downlink();
    if (!ensure_decoder(fmt.rate)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }
    if (!ensure_codec(fmt.rate)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    while (s_phase == CYBER_AUDIO_PLAYBACK) {
        size_t len = 0;
        const uint8_t *frame = cyber_audio_queue_front(&s_rx_q, &len);
        if (frame == NULL) {
            vTaskDelay(pdMS_TO_TICKS(CYBER_AUDIO_IDLE_FRAME_MS));
            continue;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer      = (uint8_t *)frame,
            .len         = (uint32_t)len,
            .consumed    = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t out = {
            .buffer       = (uint8_t *)s_pcm_out,
            .len          = sizeof(s_pcm_out),
            .needed_size  = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t dec_info = {0};
        const esp_audio_err_t derr = esp_opus_dec_decode(s_dec, &raw, &out, &dec_info);
        cyber_audio_queue_pop(&s_rx_q);
        if (derr != ESP_AUDIO_ERR_OK) {
            s_decode_errors++;
            ESP_LOGW(TAG, "解码失败：%d（%u 字节）", (int)derr, (unsigned)len);
            continue;
        }
        const int samples = (int)(out.decoded_size / sizeof(int16_t));
        if (samples == 0) continue;

        if (bsp_audio_write(s_pcm_out, (size_t)out.decoded_size) != ESP_OK) {
            s_write_errors++;
            vTaskDelay(pdMS_TO_TICKS(CYBER_AUDIO_IDLE_FRAME_MS));
            continue;
        }
        s_rx_frames++;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    cyber_audio_phase_t prev = CYBER_AUDIO_IDLE;

    for (;;) {
        const cyber_audio_phase_t phase = s_phase;

        if (prev != phase) {
            // 离开一个阶段就把该阶段占着的编解码器还回去。这不是"顺手清理"，而是
            // 让下一个阶段【拿得到内存的必要条件】，也是不能被"常驻更省事"取代的
            // 设计选择——两个方向都有实测代价，只有按需才是可行的：
            //   - 不归还：实测握手后堆里最大连续块只剩 7680 B，而解码器要 18136 B
            //     连续内存，播放端会直接分配失败；
            //   - 常驻：两者合计 42852 B，开机 free 从 88 KB 掉到 32 KB，而一次 OTA
            //     复查要 ~40 KB，激活检查失败（实测 ota -> state=5）；
            //   - 按需：半双工保证编码器与解码器永不同时需要，所以峰值只是两者中较
            //     大的那块（24716 B），空闲期则完全归还给系统。
            if (prev == CYBER_AUDIO_CAPTURE) {
                release_encoder();
            } else if (prev == CYBER_AUDIO_PLAYBACK) {
                // 这些帧属于上一轮回复，留着只会在下一次播放时先吐出旧内容，
                // 听起来像"先重放上一句"。
                cyber_audio_queue_drain(&s_rx_q);
                release_decoder();
            }
            ESP_LOGI(TAG, "阶段 %d -> %d，free=%u largest=%u",
                     (int)prev, (int)phase,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            prev = phase;
        }

        switch (phase) {
        case CYBER_AUDIO_CAPTURE:
            if (!ensure_encoder() || !ensure_codec(CYBER_WS_SAMPLE_RATE)) {
                vTaskDelay(pdMS_TO_TICKS(CYBER_AUDIO_IDLE_TICK_MS));
                break;
            }
            do_capture();
            break;

        case CYBER_AUDIO_PLAYBACK:
            do_playback();
            break;

        case CYBER_AUDIO_IDLE:
        default:
            vTaskDelay(pdMS_TO_TICKS(CYBER_AUDIO_IDLE_TICK_MS));
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
// 全部先查 s_ready。这不是防御性编程，而是必需的：tick_cb 从【建屏那一刻】就开始
// 跳，而 cyber_audio_init() 排在启动序列更后面，所以这些函数一定会在模块就绪之前
// 被调到。而 xSemaphoreTake(NULL) 在 ESP-IDF 里是直接 assert 崩溃（不是返回错误），
// 所以"没就绪就返回"是唯一安全的做法，不能指望调用方自己看时序。
//
// 被提前丢掉的调用不会丢状态：调用方每拍都会重发，下一拍就补上。
static bool audio_ready(void)
{
    return s_ready;
}

void cyber_audio_set_tx_sink(cyber_audio_tx_fn fn)
{
    if (!audio_ready()) return;
    s_tx_sink = fn;
}

void cyber_audio_set_phase(cyber_audio_phase_t phase)
{
    if (!audio_ready()) return;
    s_phase = phase;
}

void cyber_audio_set_downlink(const cyber_audio_format_t *fmt)
{
    if (!audio_ready()) return;
    cyber_audio_format_t f;
    if (fmt != NULL) {
        f = *fmt;
    } else if (!cyber_audio_format_from_downlink(&f, CYBER_WS_SAMPLE_RATE,
                                                CYBER_AUDIO_FRAME_MS)) {
        return;
    }
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    s_downlink = f;
    xSemaphoreGive(s_cfg_lock);
}

void cyber_audio_push_opus(const void *data, size_t len)
{
    if (!audio_ready()) return;
    // 事件循环任务上跑，只能是一次定长拷贝。队满时 cyber_audio_queue_push 会丢弃
    // 这一帧并计数，绝不阻塞、绝不覆盖未播的帧。
    (void)cyber_audio_queue_push(&s_rx_q, data, len);
}

void cyber_audio_flush(void)
{
    if (!audio_ready()) return;
    cyber_audio_queue_drain(&s_rx_q);
}

void cyber_audio_get_stats(cyber_audio_stats_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (!audio_ready()) return;
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    out->tx_frames = s_tx_frames;
    out->tx_dropped = s_tx_dropped;
    out->encode_errors = s_encode_errors;
    out->alloc_failures = s_alloc_failures;
    out->rx_frames = s_rx_frames;
    out->decode_errors = s_decode_errors;
    out->write_errors = s_write_errors;
    out->read_errors = s_read_errors;
    out->queue_dropped_full = s_rx_q.dropped_full;
    out->queue_dropped_oversize = s_rx_q.dropped_oversize;
    xSemaphoreGive(s_cfg_lock);
    if (s_task != NULL) {
        // uxTaskGetStackHighWaterMark 以 StackType_t 为单位，换算成字节便于定容。
        out->stack_high_water =
            (uint32_t)uxTaskGetStackHighWaterMark(s_task) * (uint32_t)sizeof(StackType_t);
    }
}

esp_err_t cyber_audio_init(void)
{
    if (s_inited) return ESP_OK;

    s_cfg_lock = xSemaphoreCreateMutex();
    if (s_cfg_lock == NULL) return ESP_ERR_NO_MEM;

    cyber_audio_queue_reset(&s_rx_q);
    if (!cyber_audio_format_from_downlink(&s_downlink, CYBER_WS_SAMPLE_RATE,
                                          CYBER_AUDIO_FRAME_MS)) {
        vSemaphoreDelete(s_cfg_lock);
        s_cfg_lock = NULL;
        return ESP_ERR_INVALID_ARG;
    }
    s_phase = CYBER_AUDIO_IDLE;

    const esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频硬件初始化失败：%s", esp_err_to_name(err));
        vSemaphoreDelete(s_cfg_lock);
        s_cfg_lock = NULL;
        return err;
    }
    bsp_audio_set_volume(CYBER_AUDIO_VOLUME);

    // 编解码器不在这里预分配。官方 esp_audio_codec 的句柄在 open() 时各自从堆分配
    // scratch，close() 时归还；本模块按半双工在"进采集/进播放"时才 open，"离开阶段"
    // 时 close 归还（见 ensure_*/release_*）。所以这里没有常驻内存要占——空闲期堆
    // 完全归系统，OTA 复查与 WS 会话都能拿到各自需要的连续内存。
    //
    // 代价是每次换挡要重新 open（毫秒级），换来的是内存按需。这正是本板无 PSRAM、
    // 且 OTA/WS 会话都要大连续块的前提。

    if (xTaskCreate(audio_task, "cyber_audio", CYBER_AUDIO_TASK_STACK, NULL,
                    CYBER_AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        vSemaphoreDelete(s_cfg_lock);
        s_cfg_lock = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    // 放在这里而不是函数开头：此刻 s_cfg_lock 与 worker 都已就位，外部调用方
    // 从这一行起才可能真的调到它们。
    s_ready = true;
    ESP_LOGI(TAG, "音频通路就绪：上行 %u Hz / 下行默认 %u Hz，单帧 %u ms，"
                  "编解码器按需开关（半双工），free=%u largest=%u",
             (unsigned)CYBER_WS_SAMPLE_RATE, (unsigned)s_downlink.rate,
             (unsigned)CYBER_AUDIO_FRAME_MS,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}
