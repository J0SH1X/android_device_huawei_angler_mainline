/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * nanohub backend for the mainline Sensors HAL (Nexus 6P / angler).
 *
 * The sensors (BMI160 accel/gyro, AK09911 mag, BMP280 baro, ALS, proximity)
 * sit behind an STM32 "nanohub" MCU. The kernel driver (drivers/staging/
 * nanohub) exposes the raw host protocol on /dev/nanohub:
 *  - configuration: write a packed ConfigCmd (evtType 0x300) per hub sensor
 *  - data: read returns event blocks, evtType = 0x200 + hub sensor type,
 *    followed by a reference time (CLOCK_BOOTTIME ns, translated by the
 *    kernel), a FirstSample header and delta-time encoded samples (SI units).
 * Protocol taken from AOSP device/google/contexthub/sensorhal (the stock HAL).
 *
 * Only the physical sensors and the BMI160 step/motion functions are listed;
 * fused sensors (gravity, rotation vectors, ...) are provided by the Android
 * framework from accel+gyro+mag.
 */

#define LOG_TAG "sensors-nanohub"

#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/SensorTypes.h>
#include <libsensors_mainline/SensorBackend.h>

#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace aidl::android::hardware::sensors::mainline {
namespace {

constexpr const char* kNanohubPath = "/dev/nanohub";

/* firmware/os/inc/eventnums.h */
constexpr uint32_t EVT_NO_FIRST_SENSOR_EVENT = 0x00000200;
constexpr uint32_t EVT_NO_SENSOR_CONFIG_EVENT = 0x00000300;
constexpr uint32_t EVT_RESET_REASON = 0x00000403;
constexpr uint32_t EVT_APP_TO_SENSOR_HAL_DATA = 0x00000404;
constexpr uint32_t OS_LOG_EVENT = 0x474F4C41; /* "ALOG" */

/* firmware/os/inc/sensType.h */
constexpr uint8_t SENS_TYPE_ACCEL = 1;
constexpr uint8_t SENS_TYPE_SIG_MOTION = 4;
constexpr uint8_t SENS_TYPE_GYRO = 6;
constexpr uint8_t SENS_TYPE_MAG = 8;
constexpr uint8_t SENS_TYPE_BARO = 10;
constexpr uint8_t SENS_TYPE_ALS = 12;
constexpr uint8_t SENS_TYPE_PROX = 13;
constexpr uint8_t SENS_TYPE_STEP_COUNT = 22;
constexpr uint8_t SENS_TYPE_STEP_DETECT = 23;
/* data-only types: the angler hub firmware reports accel and mag as raw int16 samples */
constexpr uint8_t SENS_TYPE_ACCEL_RAW = 32;
constexpr uint8_t SENS_TYPE_MAG_RAW = 59;

/* the AMS ALS/prox driver turns ON_CHANGE into 5 Hz; requesting ON_CHANGE makes the hub
 * framework re-issue the rate forever (hundreds of "sensorRateAls: 5120/0" logs per second) */
constexpr uint32_t SENSOR_RATE_5HZ = 5u << 10;

constexpr uint8_t CONFIG_CMD_DISABLE = 0;
constexpr uint8_t CONFIG_CMD_ENABLE = 1;
constexpr uint8_t CONFIG_CMD_FLUSH = 2;

constexpr uint32_t SENSOR_RATE_ONCHANGE = 0xFFFFFF01u;
constexpr uint32_t SENSOR_RATE_ONESHOT = 0xFFFFFF02u;

struct ConfigCmd {
    uint32_t evtType;
    uint64_t latency;
    uint32_t rate; /* Hz << 10, or SENSOR_RATE_* */
    uint8_t sensorType;
    uint8_t cmd;
    uint16_t flags;
} __attribute__((packed));
static_assert(sizeof(ConfigCmd) == 20);

struct FirstSample {
    uint8_t numSamples;
    uint8_t numFlushes;
    uint8_t highAccuracy : 1;
    uint8_t biasPresent : 1;
    uint8_t biasSample : 6;
    uint8_t pad;
} __attribute__((packed));

struct OneAxisSample {
    uint32_t deltaTime;
    union {
        float fdata;
        uint32_t idata;
    };
} __attribute__((packed));

struct ThreeAxisSample {
    uint32_t deltaTime;
    float x, y, z;
} __attribute__((packed));

struct RawThreeAxisSample {
    uint32_t deltaTime;
    int16_t ix, iy, iz;
} __attribute__((packed));

/* delta times: bit 0 set = value in ns, clear = value << 9 */
uint64_t DeltaNs(uint32_t delta) {
    return (delta & 1) ? delta : (static_cast<uint64_t>(delta) << 9);
}

enum class Payload { kThree, kOneFloat, kStepCount, kTrigger };

size_t SampleSize(Payload p, bool raw) {
    if (raw) return sizeof(RawThreeAxisSample);
    return p == Payload::kThree ? sizeof(ThreeAxisSample) : sizeof(OneAxisSample);
}

struct HubSensor {
    uint8_t hub_type;
    SensorType type;
    Payload payload;
    const char* name;
    const char* vendor;
    float max_range;
    float resolution;
    float power_ma;
    int32_t min_delay_us; /* 0 on-change, -1 one-shot */
    int32_t max_delay_us;
    bool wake_up;
    uint8_t raw_type = 0;  /* hub data type for raw int16 samples, 0 = none */
    float raw_scale = 0;   /* raw LSB -> SI unit */
    uint32_t fixed_rate = 0; /* rate to request instead of ON_CHANGE */
};

/* ranges/resolutions as in the stock angler sensor list (BMI160 8 g / 2000 dps, AK09911, BMP280) */
const HubSensor kSensors[] = {
        {SENS_TYPE_ACCEL, SensorType::ACCELEROMETER, Payload::kThree, "BMI160 accelerometer",
         "Bosch", 156.9064f, 0.0047884f, 0.15f, 5000, 1000000, false, SENS_TYPE_ACCEL_RAW,
         9.80665f * 16.0f / 32768.0f},
        {SENS_TYPE_GYRO, SensorType::GYROSCOPE, Payload::kThree, "BMI160 gyroscope", "Bosch",
         34.906586f, 0.00106526f, 0.85f, 5000, 1000000, false},
        {SENS_TYPE_MAG, SensorType::MAGNETIC_FIELD, Payload::kThree, "AK09911 magnetometer",
         "AKM", 4912.0f, 0.0625f, 2.4f, 20000, 1000000, false, SENS_TYPE_MAG_RAW, 1.0f / 16.0f},
        {SENS_TYPE_BARO, SensorType::PRESSURE, Payload::kOneFloat, "BMP280 barometer", "Bosch",
         1100.0f, 0.005f, 0.004f, 40000, 1000000, false},
        {SENS_TYPE_ALS, SensorType::LIGHT, Payload::kOneFloat, "Ambient light", "nanohub",
         43000.0f, 1.0f, 0.1f, 0, 1000000, false, 0, 0, SENSOR_RATE_5HZ},
        {SENS_TYPE_PROX, SensorType::PROXIMITY, Payload::kOneFloat, "Proximity", "nanohub", 5.0f,
         5.0f, 0.1f, 0, 1000000, true, 0, 0, SENSOR_RATE_5HZ},
        {SENS_TYPE_STEP_DETECT, SensorType::STEP_DETECTOR, Payload::kTrigger,
         "BMI160 step detector", "Bosch", 1.0f, 1.0f, 0.15f, 0, 1000000, false},
        {SENS_TYPE_STEP_COUNT, SensorType::STEP_COUNTER, Payload::kStepCount,
         "BMI160 step counter", "Bosch", 4294967295.0f, 1.0f, 0.15f, 0, 1000000, false},
        {SENS_TYPE_SIG_MOTION, SensorType::SIGNIFICANT_MOTION, Payload::kTrigger,
         "BMI160 significant motion", "Bosch", 1.0f, 1.0f, 0.15f, -1, 0, true},
};

class NanohubBackend : public ISensorBackend {
  public:
    std::string GetName() const override { return "nanohub"; }

    int32_t Initialize(const PostEventsCallback& callback) override {
        post_ = callback;
        fd_.reset(open(kNanohubPath, O_RDWR | O_CLOEXEC));
        if (fd_ < 0) {
            PLOG(WARNING) << "cannot open " << kNanohubPath << ", no nanohub sensors";
            return 0;
        }
        stop_fd_.reset(eventfd(0, EFD_CLOEXEC));
        if (stop_fd_ < 0) {
            PLOG(ERROR) << "eventfd";
            fd_.reset();
            return 0;
        }
        for (size_t i = 0; i < std::size(kSensors); i++) {
            const HubSensor& s = kSensors[i];
            SensorInfo info{};
            info.sensorHandle = static_cast<int32_t>(i + 1);
            info.type = s.type;
            ApplySensorTypeDefaults(&info);
            info.name = s.name;
            info.vendor = s.vendor;
            info.version = 1;
            info.maxRange = s.max_range;
            info.resolution = s.resolution;
            info.power = s.power_ma;
            info.minDelayUs = s.min_delay_us;
            info.maxDelayUs = s.max_delay_us;
            info.fifoReservedEventCount = 0;
            info.fifoMaxEventCount = 0;
            if (s.wake_up)
                info.flags |= SensorInfo::SENSOR_FLAG_BITS_WAKE_UP;
            else
                info.flags &= ~SensorInfo::SENSOR_FLAG_BITS_WAKE_UP;
            sensors_.push_back(info);
            State st{};
            st.sensor = &s;
            st.period_ns = s.min_delay_us > 0 ? int64_t{s.min_delay_us} * 4 * 1000 : 0;
            state_[info.sensorHandle] = st;
            by_hub_type_[s.hub_type] = info.sensorHandle;
            if (s.raw_type) by_hub_type_[s.raw_type] = info.sensorHandle;
            LOG(INFO) << SensorInfoToString(info);
        }
        reader_ = std::thread([this] { ReadLoop(); });
        return 0;
    }

    void Deinitialize() override {
        if (fd_ < 0) return;
        {
            std::lock_guard lk(lock_);
            for (auto& [handle, st] : state_)
                if (st.active) SendConfig(st, CONFIG_CMD_DISABLE);
        }
        uint64_t one = 1;
        (void)write(stop_fd_, &one, sizeof(one));
        if (reader_.joinable()) reader_.join();
        fd_.reset();
    }

    std::vector<SensorInfo> GetSensorsList() override { return sensors_; }

    int32_t Activate(int32_t handle, bool enabled) override {
        std::lock_guard lk(lock_);
        auto it = state_.find(handle);
        if (it == state_.end()) return -EINVAL;
        it->second.active = enabled;
        if (!enabled) it->second.flushes.clear();
        return SendConfig(it->second, enabled ? CONFIG_CMD_ENABLE : CONFIG_CMD_DISABLE);
    }

    int32_t Batch(int32_t handle, int64_t period_ns, int64_t latency_ns) override {
        std::lock_guard lk(lock_);
        auto it = state_.find(handle);
        if (it == state_.end()) return -EINVAL;
        it->second.period_ns = period_ns;
        it->second.latency_ns = latency_ns;
        return it->second.active ? SendConfig(it->second, CONFIG_CMD_ENABLE) : 0;
    }

    int32_t Flush(int32_t handle) override {
        std::lock_guard lk(lock_);
        auto it = state_.find(handle);
        if (it == state_.end()) return -EINVAL;
        int32_t ret = SendConfig(it->second, CONFIG_CMD_FLUSH);
        if (ret) return ret;
        it->second.flushes.push_back(handle);
        return 0;
    }

    int32_t SetOperationMode(OperationMode) override { return 0; }

  private:
    struct State {
        const HubSensor* sensor = nullptr;
        bool active = false;
        int64_t period_ns = 0;
        int64_t latency_ns = 0;
        std::deque<int32_t> flushes;
        int64_t step_offset = 0;
        int64_t last_steps = 0;
    };

    /* caller holds lock_ */
    int32_t SendConfig(const State& st, uint8_t cmd) {
        ConfigCmd c{};
        c.evtType = EVT_NO_SENSOR_CONFIG_EVENT;
        c.sensorType = st.sensor->hub_type;
        c.cmd = cmd;
        if (cmd == CONFIG_CMD_DISABLE) {
            c.rate = 0;
            c.latency = UINT64_MAX;
        } else {
            if (st.sensor->fixed_rate)
                c.rate = st.sensor->fixed_rate;
            else if (st.sensor->min_delay_us < 0)
                c.rate = SENSOR_RATE_ONESHOT;
            else if (st.sensor->min_delay_us == 0)
                c.rate = SENSOR_RATE_ONCHANGE;
            else
                c.rate = static_cast<uint32_t>(1024000000000ULL /
                                               static_cast<uint64_t>(std::max<int64_t>(
                                                       st.period_ns, st.sensor->min_delay_us * 1000LL)));
            c.latency = static_cast<uint64_t>(std::max<int64_t>(st.latency_ns, 0));
        }
        ssize_t n = TEMP_FAILURE_RETRY(write(fd_, &c, sizeof(c)));
        if (n != static_cast<ssize_t>(sizeof(c))) {
            PLOG(WARNING) << "config " << st.sensor->name << " cmd " << int(cmd) << " failed";
            return n < 0 ? -errno : -EIO;
        }
        LOG(DEBUG) << st.sensor->name << ": cmd " << int(cmd) << " rate " << c.rate << " latency "
                   << c.latency;
        return 0;
    }

    void ResendActive() {
        std::lock_guard lk(lock_);
        for (auto& [handle, st] : state_) {
            if (!st.active) continue;
            if (st.sensor->hub_type == SENS_TYPE_STEP_COUNT) st.step_offset = st.last_steps;
            SendConfig(st, CONFIG_CMD_ENABLE);
        }
    }

    void ReadLoop() {
        pollfd fds[2] = {{fd_, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
        uint8_t buf[256];
        for (;;) {
            int r = poll(fds, 2, -1);
            if (r < 0) {
                if (errno == EINTR) continue;
                PLOG(ERROR) << "poll";
                return;
            }
            if (fds[1].revents) return;
            if (!(fds[0].revents & POLLIN)) continue;
            ssize_t len = read(fd_, buf, sizeof(buf));
            if (len < 0) {
                if (errno != EINTR && errno != EAGAIN) PLOG(WARNING) << "read";
                continue;
            }
            for (ssize_t off = 0; off < len;) {
                ssize_t used = ProcessBlock(buf + off, len - off);
                if (used <= 0) break;
                off += used;
            }
        }
    }

    /* returns bytes consumed, <= 0 to drop the rest of the buffer */
    ssize_t ProcessBlock(const uint8_t* p, size_t len) {
        uint32_t evt;
        if (len < sizeof(evt)) return -1;
        memcpy(&evt, p, sizeof(evt));
        if (evt == OS_LOG_EVENT || evt == EVT_APP_TO_SENSOR_HAL_DATA) return 0;
        if (evt == EVT_RESET_REASON) {
            uint32_t reason = 0;
            if (len >= 8) memcpy(&reason, p + 4, 4);
            LOG(INFO) << "hub reset, reason 0x" << std::hex << reason << ", restoring sensors";
            ResendActive();
            return 0;
        }
        if (evt < EVT_NO_FIRST_SENSOR_EVENT || evt >= EVT_NO_SENSOR_CONFIG_EVENT) {
            LOG(DEBUG) << "unhandled evt 0x" << std::hex << evt << " len " << std::dec << len;
            return 0;
        }
        uint8_t hub_type = static_cast<uint8_t>(evt - EVT_NO_FIRST_SENSOR_EVENT);
        auto h = by_hub_type_.find(hub_type);

        size_t off = sizeof(evt);
        uint64_t ts;
        if (len < off + sizeof(ts) + sizeof(FirstSample)) return -1;
        memcpy(&ts, p + off, sizeof(ts));
        off += sizeof(ts);
        FirstSample first;
        memcpy(&first, p + off, sizeof(first)); /* overlays sample[0].deltaTime */

        if (h == by_hub_type_.end()) {
            LOG(DEBUG) << "no sensor for hub type " << int(hub_type);
            return 0;
        }
        const HubSensor* s = state_[h->second].sensor;
        const bool raw = s->raw_type && hub_type == s->raw_type;
        size_t sample_size = SampleSize(s->payload, raw);
        std::vector<Event> events;
        bool active = false;
        {
            std::lock_guard lk(lock_);
            active = s && state_[h->second].active;
        }
        for (int i = 0; i < first.numSamples; i++) {
            if (off + sample_size > len) return -1;
            uint32_t delta;
            memcpy(&delta, p + off, sizeof(delta));
            if (i > 0) ts += DeltaNs(delta);
            if (active && !(first.biasPresent && first.biasSample == i))
                events.push_back(MakeSample(h->second, *s, p + off, ts, first.highAccuracy, raw));
            off += sample_size;
        }
        if (!first.numSamples) off += sizeof(FirstSample);

        bool wake = s && s->wake_up;
        if (first.numFlushes && s) {
            std::lock_guard lk(lock_);
            auto& fl = state_[h->second].flushes;
            for (int i = 0; i < first.numFlushes && !fl.empty(); i++) {
                events.push_back(MakeFlushCompleteEvent(fl.front()));
                fl.pop_front();
            }
        }
        if (!events.empty()) post_(events, wake);
        return static_cast<ssize_t>(off);
    }

    Event MakeSample(int32_t handle, const HubSensor& s, const uint8_t* sample, uint64_t ts,
                     bool high_accuracy, bool raw) {
        int64_t t = static_cast<int64_t>(ts);
        if (raw) {
            RawThreeAxisSample v;
            memcpy(&v, sample, sizeof(v));
            return MakeVec3Event(handle, s.type, t, v.ix * s.raw_scale, v.iy * s.raw_scale,
                                 v.iz * s.raw_scale,
                                 high_accuracy || s.type != SensorType::MAGNETIC_FIELD
                                         ? SensorStatus::ACCURACY_HIGH
                                         : SensorStatus::ACCURACY_MEDIUM);
        }
        switch (s.payload) {
            case Payload::kThree: {
                ThreeAxisSample v;
                memcpy(&v, sample, sizeof(v));
                return MakeVec3Event(handle, s.type, t, v.x, v.y, v.z,
                                     high_accuracy || s.type != SensorType::MAGNETIC_FIELD
                                             ? SensorStatus::ACCURACY_HIGH
                                             : SensorStatus::ACCURACY_MEDIUM);
            }
            case Payload::kOneFloat: {
                OneAxisSample v;
                memcpy(&v, sample, sizeof(v));
                return MakeScalarEvent(handle, s.type, t, v.fdata);
            }
            case Payload::kStepCount: {
                OneAxisSample v;
                memcpy(&v, sample, sizeof(v));
                std::lock_guard lk(lock_);
                auto& st = state_[handle];
                st.last_steps = st.step_offset + v.idata;
                return MakeStepCountEvent(handle, t, st.last_steps);
            }
            case Payload::kTrigger:
            default:
                return MakeScalarEvent(handle, s.type, t, 1.0f);
        }
    }

    PostEventsCallback post_;
    ::android::base::unique_fd fd_;
    ::android::base::unique_fd stop_fd_;
    std::thread reader_;
    std::mutex lock_;
    std::vector<SensorInfo> sensors_;
    std::map<int32_t, State> state_;
    std::map<uint8_t, int32_t> by_hub_type_;
};

}  // namespace
}  // namespace aidl::android::hardware::sensors::mainline

DEFINE_SENSOR_BACKEND(::aidl::android::hardware::sensors::mainline::NanohubBackend, 0)
