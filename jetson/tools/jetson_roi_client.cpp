#include "capture/FrameSource.hpp"
#include "capture/V4L2Capture.hpp"
#include "control/SafetyDecider.hpp"
#include "control/SafetyTransmitter.hpp"
#include "control/UartPort.hpp"
#include "metrics/LatencyStats.hpp"
#include "network/TcpRoiClient.hpp"
#include "preprocess/RoiPreprocessor.hpp"
#include "roi/RoiCropper.hpp"
#include "roi/RoiProposer.hpp"
#include "stream/MjpegStreamServer.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic_bool stop_requested{false};

void handle_signal(int) {
    stop_requested.store(true);
}

bool parse_port(const char* text, std::uint16_t& port) {
    try {
        const unsigned long value = std::stoul(text);
        if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        port = static_cast<std::uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

/*
 * ---------------------------------------------------------------------------
 * 보고서용 측정
 * ---------------------------------------------------------------------------
 *
 * "FPS"가 무엇인지부터 고정한다. ROI 분류 구조에서는 서로 다른 세 숫자가
 * 전부 FPS라고 불릴 수 있어서, 하나만 적으면 읽는 쪽이 다른 것으로 이해한다.
 *
 *   frame FPS   완료된 프레임 / 초. 화면 결과가 갱신되는 속도이며, 사람이
 *               "자연스럽다"고 느끼는 값은 이것이다.
 *   ROI/s       분류한 ROI / 초. 보드 처리량이고 프레임당 ROI 개수와 무관하다.
 *   처리분 FPS  카메라 대기를 뺀 프레임 시간의 역수. 카메라가 더 빨랐다면
 *               낼 수 있었을 상한이며, 카메라 병목과 처리 병목을 가른다.
 *
 * 루프가 완전 순차이므로 다음이 성립한다.
 *
 *   프레임 시간 = 캡처(대기 포함) + proposal + Σ(crop + TCP 왕복) + publish
 *
 * 그래서 프레임당 ROI 개수(N)별로도 나눠 보고한다 - N이 섞인 평균 FPS 하나는
 * 재현도 비교도 안 되기 때문이다.
 *
 * 환경변수:
 *   ADAS_MEASURE_WARMUP   : 통계에서 제외할 앞쪽 프레임 수 (기본 10)
 *                           TensorRT 첫 추론과 카메라 안정화 구간을 버린다.
 *   ADAS_MEASURE_QUIET=1  : ROI마다 찍던 결과 줄을 끈다. 콘솔 출력이 측정을
 *                           왜곡하므로 측정 실행에서는 켜는 것을 권한다.
 *   ADAS_MEASURE_PROGRESS : 몇 프레임마다 진행 줄을 찍을지 (기본 100, 0=끔)
 *   ADAS_MEASURE_CSV      : ROI 한 건당 한 행을 남길 CSV 경로. 히스토그램과
 *                           백분위수는 이 파일로 오프라인 계산한다.
 *   ADAS_TCP_NODELAY=1    : 분류 socket의 Nagle을 끈다.
 */

using Clock = std::chrono::steady_clock;

std::int64_t elapsed_us(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
        .count();
}

unsigned long env_ulong(const char* name, unsigned long fallback) {
    const char* const text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    try {
        return std::stoul(text);
    } catch (...) {
        return fallback;
    }
}

bool env_flag(const char* name) {
    return env_ulong(name, 0ul) != 0ul;
}

/*
 * For the judge thresholds. All of them are normalized 0..1 ratios, so a
 * value outside that range falls back - a single typo that swings a gate
 * fully open or shut is hard to trace back on the real board.
 */
float env_ratio(const char* name, float fallback) {
    const char* const text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    try {
        const float value = std::stof(text);
        if (!(value >= 0.0F) || !(value <= 1.0F)) {
            std::cerr << "warning: " << name << " out of range [0,1], using "
                      << fallback << '\n';
            return fallback;
        }
        return value;
    } catch (...) {
        std::cerr << "warning: " << name << " is not a number, using "
                  << fallback << '\n';
        return fallback;
    }
}

/*
 * ---------------------------------------------------------------------------
 * 제어 계층 (TurtleBot 안전 상태 송신)
 * ---------------------------------------------------------------------------
 *
 * 설계는 docs/JETSON_CONTROL_DESIGN.md 다. 요점만:
 *
 * 분류 루프는 프레임당 ROI 개수에 따라 주기가 흔들리고 TCP 왕복에 막힌다.
 * 그래서 송신은 20 ms 고정 스레드로 분리하고, 이 루프는 "최신 판단"만
 * 갱신한다. 판단이 아예 멈춘 것은 송신 스레드의 watchdog 이 잡는다 -
 * 멈춘 쪽은 스스로 그 사실을 보고할 수 없기 때문이다.
 *
 *   ADAS_UART_PORT   : 안전 상태를 내보낼 포트. 없으면 제어 계층을 켜지 않고
 *                      기존과 동일하게 동작한다 (/dev/ttyTHS1 또는 /dev/ttyUSB0)
 *   ADAS_UART_BAUD   : 기본 115200
 *   ADAS_EMPTY_FRAME_HEARTBEAT=0 : 검출 0건 프레임에 heartbeat ROI
 *                      보내는 것을 끈다. 기본은 켜 둔다 - 아래 heartbeat_frame
 *                      주석 참고. 순수 성능 측정만 할 때만 끈다.
 *   ADAS_HEARTBEAT_INTERVAL_MS : heartbeat 최소 간격(기본 150 ms).
 *                      PS 의 stale 판정(500 ms)보다 넉넉히 짧아야 한다.
 *   ADAS_CONTROL_ZONE_FILTER=1 : 경로 밖 후보를 분류 요청 전에 거른다.
 *                      결과는 바뀌지 않고(경로 밖은 어차피 Clear) 왕복만 준다.
 *                      기본은 꺼 둔다 - 켠 조건과 끈 조건을 각각 재고 정한다.
 */

/* 이 프로젝트의 클래스 배치. KR260 과 한 칸씩 다르다 (background 가 앞에 붙었다). */
safety::ClassMap projectClassMap() {
    safety::ClassMap classes;
    classes.background       = 0;
    classes.car              = 1;
    classes.person           = 2;
    classes.sign_warning     = 3;
    classes.sign_prohibition = 4;
    classes.sign_mandatory   = 5;
    return classes;
}

/*
 * 경로 밖(zone_x 밖) 후보는 어떤 class 여도 판단 결과가 Clear 다. 분류하지
 * 않아도 결과가 같으므로 왕복만 줄어든다 - 동작 보존 최적화다.
 */
bool insidePathZone(
    const adas::roi::RoiCandidate& candidate,
    const adas::control::AdapterConfig& adapter,
    const safety::JudgeConfig& judge
) {
    if (adapter.frame_width <= 0) {
        return true;
    }
    const auto& box = candidate.object_bbox;
    const float center_x =
        (box.x + box.width * 0.5F) / static_cast<float>(adapter.frame_width);
    return center_x >= judge.zone_x_min && center_x <= judge.zone_x_max;
}

/*
 * 응답을 못 받은 ROI 를 로그·오버레이에서 구분하기 위한 표식.
 * 프로토콜의 status 값(0..4)과 겹치지 않는 값을 쓴다.
 */
constexpr std::uint32_t kStatusNoReply = 0xffffffffu;

/* 연속 분류 실패가 이만큼이면 링크 장애로 본다. */
constexpr std::uint32_t kLinkFailureThreshold = 3u;

struct RoiRecord {
    int roi_index{0};
    std::int64_t crop_us{0};
    std::int64_t rtt_us{0};
    std::uint32_t status{0};
    std::uint32_t class_id{0};
    std::uint32_t confidence_ppm{0};
};

struct Metrics {
    adas::metrics::LatencyStats capture;
    adas::metrics::LatencyStats propose;
    adas::metrics::LatencyStats crop;
    adas::metrics::LatencyStats rtt;
    adas::metrics::LatencyStats publish;
    adas::metrics::LatencyStats frame;
    adas::metrics::LatencyStats work;  // 프레임 시간에서 카메라 대기를 뺀 값

    // ROI 개수(N)별 프레임 시간. N이 섞인 평균 FPS는 비교가 안 된다.
    std::map<std::size_t, adas::metrics::LatencyStats> frame_by_roi_count;
    std::map<std::uint32_t, std::uint64_t> status_counts;

    std::size_t frames{0};
    std::size_t rois{0};
    std::int64_t wall_us{0};
};

void print_stat_row(
    const char* name,
    adas::metrics::LatencyStats& stat
) {
    if (stat.empty()) {
        std::printf("  %-32s %6s\n", name, "-");
        return;
    }
    std::printf(
        "  %-32s %6zu  %9.3f %9.3f %9.3f %9.3f\n",
        name,
        stat.count(),
        stat.percentileMs(0.5),
        stat.meanMs(),
        stat.percentileMs(0.95),
        stat.maxMs()
    );
}

void print_summary(Metrics& metrics, unsigned long warmup) {
    std::printf("\n=== Jetson 파이프라인 측정 요약 ===\n");

    if (metrics.frames == 0) {
        std::printf("  측정된 프레임 없음 (warmup=%lu 보다 짧게 돌았다)\n\n",
                    warmup);
        return;
    }

    const double wall_s = static_cast<double>(metrics.wall_us) / 1000000.0;
    std::printf("  측정 구간      %.2f s  (앞 %lu 프레임 제외)\n",
                wall_s, warmup);
    std::printf("  완료 프레임    %zu", metrics.frames);
    if (wall_s > 0.0) {
        std::printf("   ->  %.2f FPS   (결과 갱신 속도)",
                    static_cast<double>(metrics.frames) / wall_s);
    }
    std::printf("\n");
    std::printf("  분류한 ROI     %zu", metrics.rois);
    if (wall_s > 0.0) {
        std::printf("   ->  %.2f ROI/s",
                    static_cast<double>(metrics.rois) / wall_s);
    }
    std::printf("\n");
    std::printf("  프레임당 ROI   평균 %.2f\n",
                static_cast<double>(metrics.rois)
                    / static_cast<double>(metrics.frames));

    std::printf("  응답 상태      ");
    for (const auto& entry : metrics.status_counts) {
        std::printf("status=%u:%llu  ",
                    entry.first,
                    static_cast<unsigned long long>(entry.second));
    }
    std::printf("\n");

    // 라벨을 ASCII로 두는 것은 취향이 아니다 - printf의 폭 지정은 바이트를
    // 세므로 한글 라벨을 섞으면 표가 어긋난다. 각 구간의 설명은
    // docs/FPS_MEASUREMENT_GUIDE.md 에 있다.
    std::printf("\n  stage latency (ms)                    n    median"
                "      mean       p95       max\n");
    print_stat_row("capture (incl. camera wait)", metrics.capture);
    print_stat_row("proposal inference", metrics.propose);
    print_stat_row("crop+prepare (per ROI)", metrics.crop);
    print_stat_row("TCP round-trip (per ROI)", metrics.rtt);
    print_stat_row("MJPEG publish", metrics.publish);
    print_stat_row("frame total", metrics.frame);
    print_stat_row("frame work (excl. camera wait)", metrics.work);

    if (!metrics.work.empty()) {
        const double median_work = metrics.work.percentileMs(0.5);
        if (median_work > 0.0) {
            std::printf(
                "%42s->  %.2f FPS  (카메라가 더 빨랐다면 낼 수 있는 상한)\n",
                "", 1000.0 / median_work);
        }
    }

    std::printf("\n  ROI 개수별 프레임 시간\n");
    for (auto& entry : metrics.frame_by_roi_count) {
        const double median = entry.second.percentileMs(0.5);
        std::printf("    N=%-3zu n=%-6zu median %8.3f ms",
                    entry.first, entry.second.count(), median);
        if (median > 0.0) {
            std::printf("  ->  %6.2f FPS", 1000.0 / median);
        }
        std::printf("\n");
    }
    std::printf("===================================\n\n");
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    // argv[5] (mjpeg-port)는 선택 인자다: 안 주면 스트리밍 없이 기존과
    // 동일하게 동작한다.
    if (argc != 5 && argc != 6) {
        std::cerr << "usage: " << argv[0]
                  << " <video-device> <ps-ip> <port> <engine|--full-frame>"
                     " [mjpeg-port]\n";
        return EXIT_FAILURE;
    }

    std::uint16_t port = 0;
    if (!parse_port(argv[3], port)) {
        std::cerr << "invalid TCP port\n";
        return EXIT_FAILURE;
    }
    const bool full_frame_mode = std::string(argv[4]) == "--full-frame";

    const unsigned long warmup_frames = env_ulong("ADAS_MEASURE_WARMUP", 10ul);
    const unsigned long progress_every =
        env_ulong("ADAS_MEASURE_PROGRESS", 100ul);
    const bool quiet = env_flag("ADAS_MEASURE_QUIET");
    const bool no_delay = env_flag("ADAS_TCP_NODELAY");
    const char* const csv_path = std::getenv("ADAS_MEASURE_CSV");

    std::ofstream csv;
    if (csv_path != nullptr && *csv_path != '\0') {
        csv.open(csv_path);
        if (!csv.is_open()) {
            // 측정 파일을 못 여는 것으로 파이프라인을 멈추지는 않는다.
            // 다만 조용히 넘어가지도 않는다 - 없는 줄 모르고 측정하면
            // 그 실행은 통째로 버려진다.
            std::cerr << "warning: failed to open ADAS_MEASURE_CSV: "
                      << csv_path << ", continuing without CSV\n";
        } else {
            csv << "frame_id,roi_index,roi_count,capture_us,propose_us,"
                   "crop_us,rtt_us,publish_us,frame_us,status,class_id,"
                   "confidence_ppm\n";
        }
    }

    /* --- 제어 계층 --------------------------------------------------- */
    const char* const uart_path = std::getenv("ADAS_UART_PORT");
    const unsigned uart_baud =
        static_cast<unsigned>(env_ulong("ADAS_UART_BAUD", 115200ul));
    const bool zone_filter = env_flag("ADAS_CONTROL_ZONE_FILTER");
    /* 기본 켜짐. 0 을 명시해야 꺼진다. */
    const bool empty_frame_heartbeat =
        env_ulong("ADAS_EMPTY_FRAME_HEARTBEAT", 1ul) != 0ul;
    const std::uint64_t heartbeat_interval_ms =
        static_cast<std::uint64_t>(env_ulong("ADAS_HEARTBEAT_INTERVAL_MS", 150ul));

    adas::control::PosixUartPort uart;
    adas::control::SteadySafetyClock safety_clock;
    std::unique_ptr<adas::control::SafetyTransmitter> transmitter;
    std::atomic_bool transmitter_stop{false};
    std::thread transmitter_thread;

    adas::control::SafetyDecider::Config decider_config;
    decider_config.judge.classes = projectClassMap();
    decider_config.latch.release_ms = 200u;
    decider_config.latch.release_frames = 3u;

    /*
     * The judge gates depend on how the camera is mounted. Hardcoding them
     * means a rebuild for every single value change, which makes tuning on
     * the real board impractical.
     */
    decider_config.judge.sign_slow_height =
        env_ratio("ADAS_SIGN_SLOW_HEIGHT", decider_config.judge.sign_slow_height);
    decider_config.judge.stop_height =
        env_ratio("ADAS_STOP_HEIGHT", decider_config.judge.stop_height);
    decider_config.judge.slow_height =
        env_ratio("ADAS_SLOW_HEIGHT", decider_config.judge.slow_height);
    decider_config.judge.zone_y_min =
        env_ratio("ADAS_ZONE_Y_MIN", decider_config.judge.zone_y_min);
    decider_config.judge.zone_x_min =
        env_ratio("ADAS_ZONE_X_MIN", decider_config.judge.zone_x_min);
    decider_config.judge.zone_x_max =
        env_ratio("ADAS_ZONE_X_MAX", decider_config.judge.zone_x_max);
    decider_config.judge.min_score =
        env_ratio("ADAS_MIN_SCORE", decider_config.judge.min_score);

    /*
     * If zone_x is inverted, the range check in judgeOne always fails and
     * **nothing is ever judged a hazard** - braking disappears silently,
     * so this has to be visible rather than merely logged.
     */
    if (decider_config.judge.zone_x_min > decider_config.judge.zone_x_max) {
        std::cerr << "ADAS_ZONE_X_MIN > ADAS_ZONE_X_MAX: nothing would ever "
                     "be judged as a hazard\n";
        return EXIT_FAILURE;
    }
    if (decider_config.judge.slow_height > decider_config.judge.stop_height) {
        std::cerr << "warning: ADAS_SLOW_HEIGHT > ADAS_STOP_HEIGHT, Slow "
                     "state is unreachable\n";
    }
    adas::control::SafetyDecider decider(decider_config);

    /*
     * Record which gates this run used. Once the values are tunable from the
     * environment, "what were they at the time" becomes a precondition for
     * reading any measurement taken from this run.
     */
    std::cout << "safety judge: sign_slow_height="
              << decider_config.judge.sign_slow_height
              << " stop_height=" << decider_config.judge.stop_height
              << " slow_height=" << decider_config.judge.slow_height
              << " zone_x=[" << decider_config.judge.zone_x_min << ','
              << decider_config.judge.zone_x_max << ']'
              << " zone_y_min=" << decider_config.judge.zone_y_min
              << " min_score=" << decider_config.judge.min_score << '\n';

    if (uart_path != nullptr && *uart_path != '\0') {
        if (!uart.open(uart_path, uart_baud)) {
            /*
             * 제어를 켜라고 했는데 못 켰다. 조용히 분류만 돌면 로봇이
             * 제어 없이 움직이는 상태가 되므로 여기서 멈춘다.
             */
            std::cerr << "failed to open UART: " << uart_path << '\n';
            return EXIT_FAILURE;
        }
        transmitter = std::make_unique<adas::control::SafetyTransmitter>(
            uart, safety_clock, adas::control::SafetyTransmitter::Config{});
        transmitter_thread = std::thread([&transmitter, &transmitter_stop]() {
            transmitter->run(transmitter_stop);
        });
        std::cout << "safety uart: " << uart_path << " @" << uart_baud
                  << ", zone-filter=" << (zone_filter ? "on" : "off") << '\n';
    }

    std::unique_ptr<adas::stream::MjpegStreamServer> mjpeg_server;
    if (argc == 6) {
        std::uint16_t mjpeg_port = 0;
        if (!parse_port(argv[5], mjpeg_port)) {
            std::cerr << "invalid MJPEG port\n";
            return EXIT_FAILURE;
        }
        adas::stream::MjpegServerConfig mjpeg_config;
        mjpeg_config.port = mjpeg_port;
        mjpeg_server = std::make_unique<adas::stream::MjpegStreamServer>(mjpeg_config);
        if (!mjpeg_server->start()) {
            // 스트리밍은 부가 기능이다 - 못 띄워도 분류 파이프라인은
            // 계속 돌린다.
            std::cerr << "warning: failed to start MJPEG server on port "
                      << mjpeg_port << ", continuing without streaming\n";
            mjpeg_server.reset();
        } else {
            std::cout << "MJPEG stream: http://<jetson-ip>:" << mjpeg_port << "/\n";
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    V4L2Capture capture;
    if (!capture.init(argv[1])) {
        std::cerr << "failed to open camera: " << argv[1] << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "camera: " << capture.format().describe() << '\n';

    // 캡처를 별도 스레드로 돌려 분류 처리와 겹치게 한다 (§젯슨 병목 개선,
    // capture/FrameSource.hpp 참고). 소비자는 항상 최신 프레임만 받는다.
    FrameSource frame_source(capture);
    frame_source.start();
    std::uint64_t last_frame_seq = 0;

    adas::network::TcpRoiClient client;
    if (client.connectToServer(argv[2], port)
        != adas::network::TcpClientStatus::Ok) {
        std::cerr << "failed to connect to PS server\n";
        return EXIT_FAILURE;
    }
    if (no_delay
        && client.setNoDelay(true) != adas::network::TcpClientStatus::Ok) {
        // 이 실행의 측정값이 "NODELAY 켠 조건"이라는 전제가 깨지므로
        // 반드시 눈에 보여야 한다.
        std::cerr << "warning: failed to set TCP_NODELAY on classify socket\n";
    }

    std::cout << "measurement: warmup=" << warmup_frames
              << " quiet=" << (quiet ? "on" : "off")
              << " csv=" << (csv.is_open() ? csv_path : "(off)")
              << " tcp_nodelay=" << (no_delay ? "on" : "off") << '\n';

    adas::roi::ProposerConfig proposer_config;
    if (!full_frame_mode) {
        proposer_config.engine_path = argv[4];
    }
    const adas::roi::RoiProposer proposer(proposer_config);
    const adas::roi::RoiCropper cropper;
    const adas::preprocess::RoiPreprocessor preprocessor;
    std::uint32_t frame_id = 0u;
    /* Arty PS 로 마지막 요청을 보낸 시각. heartbeat 간격 판단에 쓴다. */
    std::uint64_t last_request_ms = 0u;

    Metrics metrics;
    std::vector<RoiRecord> frame_records;
    std::vector<adas::control::RoiObservation> observations;
    std::uint32_t consecutive_classify_failures = 0u;
    Clock::time_point measure_start;
    Clock::time_point measure_end;
    bool measuring = false;
    /*
     * 분류 실패로 프로세스를 끝내지 않게 되면서, 비정상 종료로 볼 것은
     * 카메라가 멈춘 경우만 남았다.
     */
    bool capture_failed = false;

    while (!stop_requested.load()) {
        const Clock::time_point t_frame_begin = Clock::now();

        cv::Mat frame;
        const FrameSource::Status capture_status =
            frame_source.next(frame, last_frame_seq, 1000);
        if (capture_status == FrameSource::Status::Timeout) {
            continue;
        }
        if (capture_status != FrameSource::Status::Ok) {
            /* 카메라가 죽으면 판단 근거가 없다. 나가기 전에 Stop 을 남긴다. */
            std::cerr << "camera capture stopped or failed\n";
            capture_failed = true;
            decider.forceStop(safety_clock.nowMs());
            if (transmitter) {
                transmitter->publish(decider.state(), safety_clock.nowMs());
            }
            break;
        }
        const Clock::time_point t_capture = Clock::now();

        std::vector<adas::roi::RoiCandidate> candidates;
        if (full_frame_mode) {
            candidates.push_back({
                frame_id,
                0u,
                {0.0F, 0.0F,
                 static_cast<float>(frame.cols),
                 static_cast<float>(frame.rows)},
                1.0F
            });
        } else {
            candidates = proposer.propose(frame, frame_id);
        }
        if (zone_filter) {
            /*
             * 경로 밖은 어떤 class 여도 Clear 다. 분류하지 않아도 판단이
             * 같으므로 왕복만 줄어든다.
             */
            std::vector<adas::roi::RoiCandidate> inside;
            inside.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                if (insidePathZone(candidate, decider.config().adapter,
                                   decider.config().judge)) {
                    inside.push_back(candidate);
                }
            }
            candidates.swap(inside);
        }

        /*
         * 검출 0건이어도 프레임당 요청 하나는 반드시 보낸다.
         *
         * 안전 판단은 Arty PS 가 한다. PS 는 요청이 온 프레임에서만
         * 판단을 갱신하므로, ROI 가 없는 프레임을 통째로 건너뛰면
         * 갱신이 끊긴다. 그러면 PS 의 stale watchdog 이 "판단 근거가
         * 오래됐다"고 보고 Stop 을 낸다 - 빈 화면(=안전)인데 로봇이
         * 멈춘다. 실제로 2026-08-20 데모에서 이것 때문에 STOP/CLEAR 가
         * 쉴 새 없이 드나들고 바퀴가 안 도는 증상이 나왔다.
         *
         * objectness 를 0 으로 둔다. DetectionAdapter 가 min_objectness
         * (0.25) 미만을 Rejected 로 버리므로 이 ROI 는 판단에 절대
         * 끼지 않는다. 즉 위험 근거를 만들어내지 않고 "이 프레임은
         * 살아 있고 볼 것이 없다"만 전달된다. bbox 도 이중 안전장치로
         * 경로 영역 밖(화면 좌측 상단)에 둔다.
         *
         * 매 프레임 보내면 빈 화면에서 FPS 가 반토막난다(실측 29.3 ->
         * 12.3 FPS). 그래서 "마지막 요청 이후 heartbeat_interval_ms 가
         * 지났을 때만" 보낸다. 실제 ROI 요청도 같은 타이머를 갱신하므로,
         * 물체가 보이는 동안에는 heartbeat 가 아예 나가지 않는다.
         *
         * 비용은 빈 프레임당 가속기 왕복 1회다. 측정에서는
         * 이 ROI 를 metrics · overlay · CSV 에서 전부 제외한다 - 그래야
         * "검출 0건 프레임"이라는 사실이 숫자에 그대로 남는다.
         */
        const std::uint64_t frame_ms = safety_clock.nowMs();
        const bool heartbeat_frame =
            empty_frame_heartbeat
            && candidates.empty()
            && (frame_ms - last_request_ms) >= heartbeat_interval_ms;
        if (heartbeat_frame) {
            candidates.push_back({
                frame_id,
                0u,
                {0.0F, 0.0F, 32.0F, 32.0F},
                0.0F
            });
        }

        const Clock::time_point t_propose = Clock::now();

        // 앞쪽 몇 프레임은 TensorRT 첫 추론과 카메라 안정화 때문에
        // 정상 상태가 아니다. 측정 창은 여기서 시작한다.
        const bool counted = frame_id >= warmup_frames;
        if (counted && !measuring) {
            measuring = true;
            measure_start = t_frame_begin;
        }

        // 이번 프레임에서 나온 ROI 분류 결과. MJPEG 서버로 넘겨
        // 화면에 겹쳐 그릴 때만 쓴다 - 분류 루프 자체의 동작에는
        // 영향이 없다.
        std::vector<adas::stream::RoiOverlay> overlays;
        overlays.reserve(candidates.size());
        frame_records.clear();
        observations.clear();

        int roi_index = 0;
        for (const auto& candidate : candidates) {
            const Clock::time_point t_roi_begin = Clock::now();
            const auto cropped = cropper.crop(frame, candidate);
            if (!cropped) {
                continue;
            }
            const auto prepared = preprocessor.prepare(*cropped);
            if (!prepared) {
                continue;
            }
            const Clock::time_point t_crop = Clock::now();

            adas::network::ClassificationResult result;
            const adas::network::TcpClientStatus classify_status =
                client.classify(*prepared, result);
            const Clock::time_point t_rtt = Clock::now();

            /*
             * 분류 실패로 파이프라인을 끝내지 않는다. proposal 은 이미
             * "물체가 있다"고 말했으므로, class 를 못 얻은 것은 정보 부족이지
             * 안전 근거가 아니다 - 판단 계층이 Unclassified 로 받아 기하만으로
             * 본다. 연속 실패는 링크 장애로 따로 처리한다.
             */
            const bool classified =
                classify_status == adas::network::TcpClientStatus::Ok;
            if (classified) {
                consecutive_classify_failures = 0u;
            } else {
                ++consecutive_classify_failures;
                if (!quiet) {
                    std::cerr << "classification request failed ("
                              << consecutive_classify_failures << " in a row)\n";
                }
            }
            observations.push_back({candidate, result, classified});

            const std::int64_t crop_us = elapsed_us(t_roi_begin, t_crop);
            const std::int64_t rtt_us = elapsed_us(t_crop, t_rtt);

            if (counted && !heartbeat_frame) {
                metrics.crop.add(crop_us);
                metrics.rtt.add(rtt_us);
                metrics.rois += 1;
                metrics.status_counts[classified ? result.status : kStatusNoReply] += 1;
                frame_records.push_back({
                    roi_index, crop_us, rtt_us,
                    result.status, result.class_id, result.confidence_ppm
                });
            }

            if (!quiet && !heartbeat_frame) {
                std::cout << "frame=" << result.frame_id
                          << " roi=" << result.roi_id
                          << " status=" << result.status
                          << " class=" << result.class_id
                          << " confidence_ppm=" << result.confidence_ppm
                          << " rtt_us=" << rtt_us
                          << '\n';
            }

            /* heartbeat ROI 는 화면에 그리지 않는다 - 실제 검출이 아니다. */
            if (!heartbeat_frame) {
                overlays.push_back({
                    candidate.object_bbox,
                    classified ? result.status : kStatusNoReply,
                    classified ? result.class_id : UINT32_MAX,
                    classified ? result.confidence_ppm : 0u
                });
                ++roi_index;
            }
        }

        /* 실제 ROI 든 heartbeat 든, 보냈으면 타이머를 갱신한다. */
        if (!candidates.empty()) {
            last_request_ms = safety_clock.nowMs();
        }

        /*
         * 개별 ROI 실패가 아니라 연속 실패는 링크 장애다. 판단 자체가
         * 불가능하므로 래치를 거치지 않고 곧바로 Stop 이다.
         */
        const std::uint64_t now_ms = safety_clock.nowMs();
        if (consecutive_classify_failures >= kLinkFailureThreshold) {
            decider.forceStop(now_ms);
        } else {
            (void)decider.decide(observations, now_ms);
        }
        if (transmitter) {
            transmitter->publish(decider.state(), now_ms);
        }

        /* 연결이 끊겼으면 다음 프레임을 위해 한 번 붙여 본다. */
        if (!client.isConnected()) {
            (void)client.connectToServer(argv[2], port);
        }

        // 캡처/분류 루프는 이 호출로 절대 멈추지 않는다 - publish()는
        // 프레임을 clone해서 공유 슬롯에 넣고 바로 반환한다. 실제 JPEG
        // 인코딩과 브라우저로의 전송은 MjpegStreamServer 내부의 별도
        // 스레드에서 일어난다 (JETSON_MJPEG_STREAM_NOTES.md 참고).
        const Clock::time_point t_before_publish = Clock::now();
        if (mjpeg_server) {
            mjpeg_server->publish(frame, std::move(overlays));
        }
        const Clock::time_point t_frame_end = Clock::now();

        if (counted) {
            const std::int64_t capture_us = elapsed_us(t_frame_begin, t_capture);
            const std::int64_t propose_us = elapsed_us(t_capture, t_propose);
            const std::int64_t publish_us =
                elapsed_us(t_before_publish, t_frame_end);
            const std::int64_t frame_us = elapsed_us(t_frame_begin, t_frame_end);

            metrics.capture.add(capture_us);
            metrics.propose.add(propose_us);
            metrics.publish.add(publish_us);
            metrics.frame.add(frame_us);
            // 카메라가 다음 프레임을 내줄 때까지 기다린 시간을 뺀 값이
            // 실제 처리 비용이다.
            metrics.work.add(frame_us - capture_us);
            metrics.frames += 1;
            metrics.frame_by_roi_count[frame_records.size()].add(frame_us);
            measure_end = t_frame_end;

            if (csv.is_open()) {
                if (frame_records.empty()) {
                    // 검출 0건인 프레임도 결과를 갱신한 프레임이다.
                    // 이 행이 없으면 프레임 수가 조용히 과소계상된다.
                    csv << frame_id << ",-1,0," << capture_us << ','
                        << propose_us << ",,," << publish_us << ','
                        << frame_us << ",,,\n";
                } else {
                    for (const RoiRecord& record : frame_records) {
                        csv << frame_id << ',' << record.roi_index << ','
                            << frame_records.size() << ',' << capture_us << ','
                            << propose_us << ',' << record.crop_us << ','
                            << record.rtt_us << ',' << publish_us << ','
                            << frame_us << ',' << record.status << ','
                            << record.class_id << ','
                            << record.confidence_ppm << '\n';
                    }
                }
            }

            if (progress_every != 0ul
                && metrics.frames % progress_every == 0u) {
                const std::int64_t span_us =
                    elapsed_us(measure_start, measure_end);
                const double span_s =
                    static_cast<double>(span_us) / 1000000.0;
                std::printf("[%zu frames] %.2f FPS, %zu ROI, %.2f ROI/s\n",
                            metrics.frames,
                            span_s > 0.0
                                ? static_cast<double>(metrics.frames) / span_s
                                : 0.0,
                            metrics.rois,
                            span_s > 0.0
                                ? static_cast<double>(metrics.rois) / span_s
                                : 0.0);
                std::fflush(stdout);
            }
        }

        ++frame_id;
    }

    if (measuring) {
        metrics.wall_us = elapsed_us(measure_start, measure_end);
    }
    print_summary(metrics, warmup_frames);

    if (csv.is_open()) {
        csv.flush();
        csv.close();
    }

    /*
     * 나가기 전에 Stop 을 한 번 더 남긴다. 이 프로세스가 사라지면 RPi 는
     * 100 ms timeout 으로 Stop 을 유지하지만, 그 사이를 비워 둘 이유가 없다.
     */
    if (transmitter) {
        decider.forceStop(safety_clock.nowMs());
        transmitter->publish(decider.state(), safety_clock.nowMs());
        transmitter_stop.store(true);
        if (transmitter_thread.joinable()) {
            transmitter_thread.join();
        }
        const auto safety_stats = transmitter->stats();
        std::printf("safety uart: sent %llu, failures %llu, stale %llu,"
                    " immediate stops %llu\n",
                    (unsigned long long)safety_stats.frames_sent,
                    (unsigned long long)safety_stats.send_failures,
                    (unsigned long long)safety_stats.stale_events,
                    (unsigned long long)safety_stats.immediate_stops);
    }

    frame_source.stop();
    client.disconnect();
    return capture_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
