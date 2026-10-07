/*
 * 土壌水分センサー ＆ LINE通知システム
 *
 * 変更点(旧版からの改善):
 *  - sysfs GPIO(/sys/class/gpio) のビットバンギングSPIを廃止し、
 *    カーネルのSPIドライバ(spidev)経由でMCP3008と通信する
 *  - センサーへの電源供給(オン/オフ)のみ libgpiod で制御する
 *    (常時通電による電気分解・センサー劣化を防ぐため)
 *  - LINEトークン・ユーザーIDは環境変数から読み込む(ソース直書き禁止)
 *  - curlにタイムアウトを設定し、送信失敗時はリトライ+ログ保存する
 *  - SIGINT/SIGTERMで安全に終了できるようにする
 *  - `--calibrate` 起動オプションで閾値キャリブレーション用の生値を表示する
 *
 * ビルド・設定・キャリブレーション手順は README.md を参照してください。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>

#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#include <curl/curl.h>

/* ================= 設定(環境変数で上書き可能) ================= */

/* SPIデバイスパス。Pi4/Pi5どちらでも基本的に /dev/spidev0.0 で変わらない。
 * (spidevはカーネルのSPIコントローラドライバが吸収するため、
 *  旧版のようなsysfs GPIO番号のPi4/Pi5差異問題がそもそも発生しない) */
#define DEFAULT_SPI_DEVICE   "/dev/spidev0.0"
#define DEFAULT_SPI_SPEED_HZ 1350000  /* MCP3008の推奨上限(3.3V駆動時) */

/* gpiochip名はPi4とPi5で異なる。
 *   Raspberry Pi 4        : "gpiochip0"
 *   Raspberry Pi 5 (RP1)  : "gpiochip4" が一般的(カーネル/OS版により変動)
 * 実機で `gpioinfo` コマンドを実行し、目的のBCMピンがどのチップに
 * 属しているか必ず確認すること。README.mdにも手順を記載。 */
#define DEFAULT_GPIO_CHIP     "gpiochip0"
#define DEFAULT_SENSOR_POWER_LINE 17  /* BCM17: センサー電源制御用(任意のGPIO) */

#define DEFAULT_THRESHOLD 40   /* 閾値の根拠はREADME.md「キャリブレーション」参照 */
#define DEFAULT_LOG_PATH  "./soil_sensor.log"

#define POLL_INTERVAL_USEC 500000   /* 0.5秒間隔で監視 */
#define NOTIFY_COOLDOWN_SEC 60      /* 同一状態変化の連続通知を抑制する間隔 */
#define SEND_MAX_RETRY 3            /* LINE送信リトライ回数 */

/* ================= グローバル状態 ================= */

static volatile sig_atomic_t g_running = 1;
static FILE *g_log_fp = NULL;
static char g_line_token[256];
static char g_line_user_id[128];
static char g_spi_device[128];
static char g_gpio_chip[64];
static int  g_sensor_power_line;
static int  g_threshold;
static long g_spi_speed_hz;

/* ================= シグナルハンドラ ================= */

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0; /* メインcd ~/ODSループを安全に抜けるためのフラグのみ変更する */
}

/* ================= ログ ================= */

static void log_write(const char *level, const char *fmt, ...) {
    time_t now = time(NULL);
    char timebuf[32];
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);

    va_list args;
    va_start(args, fmt);

    if (g_log_fp != NULL) {
        fprintf(g_log_fp, "[%s] [%s] ", timebuf, level);
        vfprintf(g_log_fp, fmt, args);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp); /* すぐディスクに反映し、クラッシュ時もログを失わない */
    }
    va_end(args);

    /* 標準出力にも出す(systemdのjournalにも残せるように) */
    va_start(args, fmt);
    printf("[%s] [%s] ", timebuf, level);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

/* ================= 設定読み込み ================= */

static int load_config(void) {
    const char *token   = getenv("LINE_TOKEN");
    const char *user_id = getenv("LINE_USER_ID");

    if (token == NULL || user_id == NULL || strlen(token) == 0 || strlen(user_id) == 0) {
        fprintf(stderr,
            "エラー: 環境変数 LINE_TOKEN / LINE_USER_ID が設定されていません。\n"
            "README.md の「設定手順」を参照して .env を作成し、\n"
            "  export $(grep -v '^#' .env | xargs)\n"
            "などで環境変数を読み込んでから起動してください。\n");
        return -1;
    }
    strncpy(g_line_token, token, sizeof(g_line_token) - 1);
    strncpy(g_line_user_id, user_id, sizeof(g_line_user_id) - 1);

    const char *spi_dev = getenv("SPI_DEVICE");
    strncpy(g_spi_device, spi_dev ? spi_dev : DEFAULT_SPI_DEVICE, sizeof(g_spi_device) - 1);

    const char *gpio_chip = getenv("GPIO_CHIP");
    strncpy(g_gpio_chip, gpio_chip ? gpio_chip : DEFAULT_GPIO_CHIP, sizeof(g_gpio_chip) - 1);

    const char *power_line = getenv("SENSOR_POWER_LINE");
    g_sensor_power_line = power_line ? atoi(power_line) : DEFAULT_SENSOR_POWER_LINE;

    const char *threshold = getenv("MOISTURE_THRESHOLD");
    g_threshold = threshold ? atoi(threshold) : DEFAULT_THRESHOLD;

    const char *speed = getenv("SPI_SPEED_HZ");
    g_spi_speed_hz = speed ? atol(speed) : DEFAULT_SPI_SPEED_HZ;

    return 0;
}

/* ================= SPI (spidev) ================= */

static int spi_open(const char *device, long speed_hz) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        log_write("ERROR", "SPIデバイス %s のオープンに失敗: %s", device, strerror(errno));
        return -1;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = (uint32_t)speed_hz;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        log_write("ERROR", "SPIパラメータ設定に失敗: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* MCP3008のシングルエンド読み取り(channel 0-7) */
static int mcp3008_read(int spi_fd, int channel) {
    if (channel < 0 || channel > 7) return -1;

    uint8_t tx[3] = { 0x01, (uint8_t)((0x08 + channel) << 4), 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };

    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = 3;
    tr.speed_hz = (uint32_t)g_spi_speed_hz;
    tr.bits_per_word = 8;

    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 1) {
        log_write("ERROR", "SPI転送に失敗: %s", strerror(errno));
        return -1;
    }
    return ((rx[1] & 0x03) << 8) | rx[2]; /* 10bit ADC値 (0-1023) */
}

/* ================= GPIO (libgpiod): センサー電源制御 =================
 *
 * 常時センサーに通電すると、特に抵抗式センサーで電気分解による腐食が
 * 進みやすいため、測定時のみ電源を入れる設計にしている。
 * (静電容量式センサーでも、長期の常時通電より測定時のみ通電する方が
 *  一般的に寿命面で有利とされる)
 */

static struct gpiod_chip *g_chip = NULL;
static struct gpiod_line *g_power_line = NULL;

static int gpio_power_setup(void) {
    g_chip = gpiod_chip_open_by_name(g_gpio_chip);
    if (!g_chip) {
        log_write("ERROR", "GPIOチップ %s のオープンに失敗: %s", g_gpio_chip, strerror(errno));
        return -1;
    }
    g_power_line = gpiod_chip_get_line(g_chip, g_sensor_power_line);
    if (!g_power_line) {
        log_write("ERROR", "GPIOライン %d の取得に失敗: %s", g_sensor_power_line, strerror(errno));
        return -1;
    }
    if (gpiod_line_request_output(g_power_line, "soil-monitor", 0) < 0) {
        log_write("ERROR", "GPIOライン %d の出力設定に失敗: %s", g_sensor_power_line, strerror(errno));
        return -1;
    }
    return 0;
}

static void gpio_power_cleanup(void) {
    if (g_power_line) gpiod_line_release(g_power_line);
    if (g_chip) gpiod_chip_close(g_chip);
}

static void sensor_power(int on) {
    if (g_power_line) gpiod_line_set_value(g_power_line, on ? 1 : 0);
}

/* ================= LINE通知 ================= */

static int send_line_message_once(const char *message) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        log_write("ERROR", "CURLの初期化に失敗しました。");
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.line.me/v2/bot/message/push");

    struct curl_slist *headers = NULL;
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", g_line_token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /* メッセージ本体はプログラム内固定文字列のみを想定しているため、
     * JSONエスケープは簡易実装。ユーザー入力等の動的文字列を組み込む
     * 場合は cJSON 等のライブラリで正しくエスケープすること。 */
    char json_data[1024];
    snprintf(json_data, sizeof(json_data),
             "{\"to\":\"%s\",\"messages\":[{\"type\":\"text\",\"text\":\"%s\"}]}",
             g_line_user_id, message);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    /* ★ タイムアウト設定: ネットワーク不調時にメインループごと
     *   ハングするのを防ぐ */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    int ok = (res == CURLE_OK && http_code == 200);
    if (!ok) {
        log_write("WARN", "LINE送信失敗 (curl=%s, http=%ld)",
                  curl_easy_strerror(res), http_code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

/* ★ リトライ付き送信: 指数バックオフ(1s, 2s, 4s)で最大SEND_MAX_RETRY回試行し、
 *   結果を必ずログに残す */
static int send_line_message(const char *message) {
    int delay = 1;
    for (int attempt = 1; attempt <= SEND_MAX_RETRY; attempt++) {
        if (send_line_message_once(message)) {
            log_write("INFO", "LINE通知送信完了 (試行%d回目): %s", attempt, message);
            return 1;
        }
        log_write("WARN", "LINE通知送信リトライ予定 (試行%d/%d)", attempt, SEND_MAX_RETRY);
        if (attempt < SEND_MAX_RETRY) {
            sleep(delay);
            delay *= 2;
        }
    }
    log_write("ERROR", "LINE通知送信を%d回試みましたが失敗しました: %s",
              SEND_MAX_RETRY, message);
    return 0;
}

/* ================= キャリブレーションモード =================
 * `--calibrate` で起動すると、閾値判定を行わず生のADC値を出し続ける。
 * 乾燥状態・湿潤状態それぞれで実行し、境界値を決めるために使う。
 * 詳しい手順は README.md の「閾値キャリブレーション」を参照。 */
static void run_calibration_mode(int spi_fd) {
    printf("=== キャリブレーションモード ===\n");
    printf("Ctrl+Cで終了します。センサーを乾燥/湿潤状態にして値の変化を確認してください。\n\n");
    while (g_running) {
        sensor_power(1);
        usleep(10000); /* センサー電源投入後の安定待ち */
        int value = mcp3008_read(spi_fd, 1);
        sensor_power(0);

        printf("ADC生値: %d\n", value);
        usleep(POLL_INTERVAL_USEC);
    }
}

/* ================= main ================= */

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (load_config() != 0) {
        return EXIT_FAILURE;
    }

    const char *log_path = getenv("LOG_FILE_PATH");
    g_log_fp = fopen(log_path ? log_path : DEFAULT_LOG_PATH, "a");
    if (!g_log_fp) {
        fprintf(stderr, "警告: ログファイルを開けませんでした。標準出力のみに出力します。\n");
    }

    curl_global_init(CURL_GLOBAL_ALL);

    int spi_fd = spi_open(g_spi_device, g_spi_speed_hz);
    if (spi_fd < 0) {
        log_write("ERROR", "SPI初期化に失敗したため終了します。");
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    if (gpio_power_setup() != 0) {
        log_write("ERROR", "GPIO初期化に失敗したため終了します。");
        close(spi_fd);
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    log_write("INFO", "初期設定完了 (SPI: %s, GPIOチップ: %s, 電源制御ライン: %d, 閾値: %d)",
               g_spi_device, g_gpio_chip, g_sensor_power_line, g_threshold);

    if (argc > 1 && strcmp(argv[1], "--calibrate") == 0) {
        run_calibration_mode(spi_fd);
        goto cleanup;
    }

    log_write("INFO", "土壌水分センサー ＆ LINE通知システム 開始");

    {
        int last_status = -1;
        time_t last_send_time = 0;

        while (g_running) {
            sensor_power(1);
            usleep(10000); /* センサー電源投入後の安定待ち */
            int value = mcp3008_read(spi_fd, 1);
            sensor_power(0);

            if (value >= 0) {
                int current_status = (value < g_threshold && value > 10) ? 1 : 0;
                time_t now = time(NULL);

                if (current_status != last_status) {
                    if (now - last_send_time >= NOTIFY_COOLDOWN_SEC) {
                        const char *msg = (current_status == 1)
                            ? "【ラズパイ通知】水を感知しました！"
                            : "【ラズパイ通知】センサーが乾きました。";

                        log_write("INFO", "状態変化を検知 (ADC: %d) -> %s", value, msg);

                        if (send_line_message(msg)) {
                            last_status = current_status;
                            last_send_time = now;
                        } else {
                            /* 送信に失敗した場合は状態を更新しない。
                             * 次のループでも差分が残るため、次に許可される
                             * タイミングで自動的に再送が試みられる。 */
                            log_write("ERROR", "通知の送信に失敗したため状態更新を保留します。");
                        }
                    } else {
                        log_write("DEBUG",
                            "状態変化を検知しましたが%d秒以内のため送信をスキップ (ADC: %d, 残り: %ld秒)",
                            NOTIFY_COOLDOWN_SEC, value, NOTIFY_COOLDOWN_SEC - (now - last_send_time));
                    }
                }
            }

            usleep(POLL_INTERVAL_USEC);
        }
    }

    log_write("INFO", "終了シグナルを受信しました。安全に終了処理を行います。");

cleanup:
    gpio_power_cleanup();
    close(spi_fd);
    curl_global_cleanup();
    if (g_log_fp) fclose(g_log_fp);
    return EXIT_SUCCESS;
}