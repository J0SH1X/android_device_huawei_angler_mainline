/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <map>

#include "FpcTa.h"

using namespace angler::fpc;

namespace {

// Pretends to be the TA: dispatches on (group, cmd) and lets each test script the response.
class MockChannel : public TaChannel {
  public:
    using Handler = std::function<void(uint8_t*, size_t)>;
    int open() override { return 0; }
    void close() override {}
    int transact(uint8_t* buf, size_t len) override {
        uint32_t g, c;
        memcpy(&g, buf, 4);
        memcpy(&c, buf + 4, 4);
        lastSize = len;
        calls.push_back({g, c});
        auto it = handlers.find({g, c});
        if (it != handlers.end()) it->second(buf, len);
        return transportRc;
    }
    int getAuthTokenKey(std::vector<uint8_t>* key) override {
        *key = std::vector<uint8_t>(32, 0xaa);
        return 0;
    }
    std::map<std::pair<uint32_t, uint32_t>, Handler> handlers;
    std::vector<std::pair<uint32_t, uint32_t>> calls;
    size_t lastSize = 0;
    int transportRc = 0;
};

template <typename T>
void put(uint8_t* b, size_t off, T v) { memcpy(b + off, &v, sizeof(T)); }
template <typename T>
T get(const uint8_t* b, size_t off) { T v; memcpy(&v, b + off, sizeof(T)); return v; }

TEST(FpcTa, SetGidSendsGidAtPlus8AndReadsResultAtPlusC) {
    MockChannel ch;
    ch.handlers[{1, 0x10}] = [](uint8_t* b, size_t) {
        EXPECT_EQ(get<uint32_t>(b, 8), 10u);
        put<int32_t>(b, 0xc, 0);
    };
    FpcTa ta(ch);
    EXPECT_EQ(ta.setGid(10), 0);
    EXPECT_EQ(ch.lastSize, kSharedDefault);
}

TEST(FpcTa, IdentifyParsesFidCoverageQuality) {
    MockChannel ch;
    ch.handlers[{1, 0x03}] = [](uint8_t* b, size_t) {
        put<int32_t>(b, 8, 0);
        put<uint32_t>(b, 0xc, 7);
        put<uint32_t>(b, 0x10, 80);
        put<uint32_t>(b, 0x14, 55);
    };
    FpcTa ta(ch);
    IdentifyResult r;
    ASSERT_EQ(ta.identify(&r), 0);
    EXPECT_TRUE(r.matched());
    EXPECT_EQ(r.fid, 7u);
    EXPECT_EQ(r.coverage, 80u);
    EXPECT_EQ(r.quality, 55u);
}

TEST(FpcTa, GetFingerIdsCapsCountAndReadsIds) {
    MockChannel ch;
    ch.handlers[{1, 0x0d}] = [](uint8_t* b, size_t len) {
        EXPECT_EQ(len, kSharedDefault);          // stock reuses the 0x48 init buffer
        EXPECT_LT(get<uint32_t>(b, 0xc), 6u);  // "in" count must be < 6
        put<int32_t>(b, 8, 0);
        put<uint32_t>(b, 0xc, 99);             // hostile TA reply: more than we asked for
        for (uint32_t i = 0; i < 5; i++) put<uint32_t>(b, 0x10 + 4 * i, 100 + i);
    };
    FpcTa ta(ch);
    std::vector<uint32_t> ids;
    ASSERT_EQ(ta.getFingerIds(&ids), 0);
    ASSERT_EQ(ids.size(), kMaxFingers);
    EXPECT_EQ(ids[4], 104u);
}

TEST(FpcTa, DbLoadSizesBufferForPathAndPassesNulTerminatedPath) {
    MockChannel ch;
    const std::string path = "/data/vendor/fpc/user_0.db";
    ch.handlers[{2, 0x0b}] = [&](uint8_t* b, size_t len) {
        EXPECT_EQ(len, path.size() + 0x11);
        EXPECT_EQ(get<uint32_t>(b, 0xc), path.size() + 1);
        EXPECT_STREQ(reinterpret_cast<char*>(b + 0x10), path.c_str());
        put<int32_t>(b, 8, -6);
    };
    FpcTa ta(ch);
    int32_t res = 0;
    ASSERT_EQ(ta.dbLoad(path, &res), 0);
    EXPECT_EQ(res, kTaDbCorrupt);
}

TEST(FpcTa, AuthorizeEnrolUsesGroup3Layout) {
    MockChannel ch;
    HatBytes hat;
    for (size_t i = 0; i < hat.size(); i++) hat[i] = static_cast<uint8_t>(i);
    ch.handlers[{3, 3}] = [&](uint8_t* b, size_t len) {
        EXPECT_EQ(len, kSharedAuthToken);
        EXPECT_EQ(get<uint32_t>(b, 0xc), 0x45u);
        EXPECT_EQ(memcmp(b + 0x10, hat.data(), kHatSize), 0);
        put<int32_t>(b, 8, 0);
    };
    FpcTa ta(ch);
    EXPECT_EQ(ta.authorizeEnrol(hat), 0);
}

TEST(FpcTa, ChallengeRoundTrip) {
    MockChannel ch;
    ch.handlers[{3, 2}] = [](uint8_t* b, size_t) {
        put<uint64_t>(b, 8, 0x1122334455667788ull);
        put<int32_t>(b, 0x10, 0);
    };
    ch.handlers[{3, 1}] = [](uint8_t* b, size_t) {
        EXPECT_EQ(get<uint64_t>(b, 8), 42u);
        put<int32_t>(b, 0x10, 0);
    };
    FpcTa ta(ch);
    uint64_t c = 0;
    EXPECT_EQ(ta.getEnrolChallenge(&c), 0);
    EXPECT_EQ(c, 0x1122334455667788ull);
    EXPECT_EQ(ta.setAuthChallenge(42), 0);
}

TEST(FpcTa, InitHwAuthPushesKey) {
    MockChannel ch;
    ch.handlers[{3, 5}] = [](uint8_t* b, size_t len) {
        EXPECT_EQ(get<uint32_t>(b, 0xc), 32u);
        EXPECT_EQ(len, 0x10u + 32);
        EXPECT_EQ(b[0x10], 0xaa);
        put<int32_t>(b, 8, 0);
    };
    FpcTa ta(ch);
    EXPECT_EQ(ta.initHwAuth(std::vector<uint8_t>(32, 0xaa)), 0);
}

TEST(FpcTa, TransportErrorsPropagateAndSkipParsing) {
    MockChannel ch;
    ch.transportRc = -5;
    FpcTa ta(ch);
    IdentifyResult r;
    EXPECT_EQ(ta.identify(&r), -5);
    EXPECT_FALSE(r.matched());
}

TEST(FpcTa, ClockHookWrapsEveryCall) {
    MockChannel ch;
    int on = 0, off = 0;
    FpcTa ta(ch);
    ta.setClockHook([&] { on++; return std::function<void()>([&] { off++; }); });
    ta.sensorIdle();
    int32_t res;
    ta.capture(0, &res);
    EXPECT_EQ(on, 2);
    EXPECT_EQ(off, 2);
}

}  // namespace
