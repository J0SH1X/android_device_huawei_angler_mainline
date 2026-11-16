/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "FsListener.h"

using namespace angler::fpc;

namespace {

struct Rsp {
    uint32_t cmd;
    int32_t ret;
    std::vector<uint8_t> data;
};

class FsListenerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "/tmp/fpcfsXXXXXX";
        ASSERT_NE(mkdtemp(tmpl), nullptr);
        mRoot = tmpl;
        mFs = std::make_unique<FsListener>(mRoot);
    }
    void TearDown() override { (void)!system(("rm -rf " + mRoot).c_str()); }

    Rsp call(uint32_t cmd, const std::vector<uint8_t>& args = {}) {
        std::vector<uint8_t> req(FsListener::kReqBufSize, 0);
        memcpy(req.data(), &cmd, 4);
        memcpy(req.data() + 4, args.data(), args.size());
        std::vector<uint8_t> rsp(FsListener::kRspBufSize);
        size_t n = 0;
        mFs->handle(req.data(), req.size(), rsp.data(), &n);
        Rsp r;
        memcpy(&r.cmd, rsp.data(), 4);
        memcpy(&r.ret, rsp.data() + 4, 4);
        r.data.assign(rsp.begin() + 8, rsp.begin() + n);
        return r;
    }
    // path@4[0x100] then a u32 at 0x104 (flags / mode)
    static std::vector<uint8_t> pathArgs(const std::string& p, uint32_t second = 0) {
        std::vector<uint8_t> a(0x104, 0);
        memcpy(a.data(), p.c_str(), p.size() + 1);
        memcpy(a.data() + 0x100, &second, 4);
        return a;
    }
    static std::vector<uint8_t> u32s(std::initializer_list<uint32_t> v) {
        std::vector<uint8_t> a;
        for (uint32_t x : v) a.insert(a.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4);
        return a;
    }

    std::string mRoot;
    std::unique_ptr<FsListener> mFs;
};

TEST_F(FsListenerTest, WriteReadRoundTripCreatesParents) {
    std::string path = mRoot + "/sub/dir/a.db";
    Rsp o = call(0x206, pathArgs(path, 0600));
    ASSERT_GE(o.ret, 0);
    int fd = o.ret;

    std::vector<uint8_t> w = u32s({static_cast<uint32_t>(fd), 5});
    w.insert(w.end(), {'h', 'e', 'l', 'l', 'o'});
    EXPECT_EQ(call(0x208, w).ret, 5);
    EXPECT_EQ(call(0x216, u32s({static_cast<uint32_t>(fd)})).ret, 0);
    EXPECT_EQ(call(0x209, u32s({static_cast<uint32_t>(fd)})).ret, 0);

    Rsp ro = call(0x202, pathArgs(path, 0 /*O_RDONLY*/));
    ASSERT_GE(ro.ret, 0);
    Rsp rd = call(0x207, u32s({static_cast<uint32_t>(ro.ret), 100}));
    ASSERT_EQ(rd.ret, 5);
    EXPECT_EQ(std::string(rd.data.begin(), rd.data.begin() + 5), "hello");
    call(0x209, u32s({static_cast<uint32_t>(ro.ret)}));
}

TEST_F(FsListenerTest, RefusesPathsOutsideRootAndDotDot) {
    EXPECT_LT(call(0x206, pathArgs("/etc/evil", 0600)).ret, 0);
    EXPECT_LT(call(0x206, pathArgs(mRoot + "/../evil", 0600)).ret, 0);
    EXPECT_LT(call(0x206, pathArgs("relative", 0600)).ret, 0);
    EXPECT_LT(call(0x213, pathArgs("/")).ret, 0);
    EXPECT_LT(call(0x213, pathArgs(mRoot)).ret, 0);  // the root itself is not removable
}

TEST_F(FsListenerTest, RefusesUntrackedFds) {
    // fd 0/1/2 are not ours
    EXPECT_EQ(call(0x209, u32s({1})).ret, -EBADF);
    EXPECT_EQ(call(0x208, u32s({1, 0})).ret, -EBADF);
    EXPECT_EQ(call(0x207, u32s({0, 10})).ret, -EBADF);
}

TEST_F(FsListenerTest, RenameStatRemove) {
    std::string a = mRoot + "/a", b = mRoot + "/b";
    Rsp o = call(0x206, pathArgs(a, 0600));
    ASSERT_GE(o.ret, 0);
    call(0x209, u32s({static_cast<uint32_t>(o.ret)}));

    std::vector<uint8_t> rn(0x204, 0);
    memcpy(rn.data(), a.c_str(), a.size() + 1);
    memcpy(rn.data() + 0x100, b.c_str(), b.size() + 1);
    EXPECT_EQ(call(0x217, rn).ret, 0);

    Rsp st = call(0x20f, pathArgs(b));
    EXPECT_EQ(st.ret, 0);
    EXPECT_EQ(st.data.size(), 0x68u);
    uint32_t mode;
    memcpy(&mode, st.data.data() + 0x10, 4);
    EXPECT_TRUE(S_ISREG(mode));

    EXPECT_EQ(call(0x213, pathArgs(b)).ret, 0);
    EXPECT_LT(call(0x20f, pathArgs(b)).ret, 0);
}

TEST_F(FsListenerTest, ChmodViaSystemIsRefused) { EXPECT_EQ(call(0x214).ret, -ENOSYS); }

TEST_F(FsListenerTest, ReadClampedToMaxIo) {
    std::string path = mRoot + "/big";
    {
        std::ofstream f(path);
        f << std::string(30000, 'x');
    }
    Rsp o = call(0x202, pathArgs(path, 0));
    ASSERT_GE(o.ret, 0);
    EXPECT_EQ(call(0x207, u32s({static_cast<uint32_t>(o.ret), 30000})).ret, 20000);
}

TEST_F(FsListenerTest, GpfsWriteReadRemove) {
    std::vector<uint8_t> buf(0x1000, 0);
    auto setReq = [&](uint32_t op, const std::string& path, uint32_t off, uint32_t size) {
        std::fill(buf.begin(), buf.end(), 0);
        memcpy(buf.data(), &op, 4);
        memcpy(buf.data() + 4, path.c_str(), path.size() + 1);
        memcpy(buf.data() + 0x104, &off, 4);
        memcpy(buf.data() + 0x108, &size, 4);
    };
    const std::string path = mRoot + "/user_0.db.tmp";
    size_t n = 0;
    int32_t status;

    setReq(1, path, 0, 4);
    memcpy(buf.data() + 0x10c, "abcd", 4);
    mFs->handleGpfs(buf.data(), buf.size(), &n);
    memcpy(&status, buf.data() + 4, 4);
    EXPECT_EQ(status, 0);
    setReq(1, path, 4, 2);
    memcpy(buf.data() + 0x10c, "ef", 2);
    mFs->handleGpfs(buf.data(), buf.size(), &n);

    setReq(0, path, 2, 4);
    mFs->handleGpfs(buf.data(), buf.size(), &n);
    memcpy(&status, buf.data() + 4, 4);
    uint32_t bytes;
    memcpy(&bytes, buf.data() + 8, 4);
    EXPECT_EQ(status, 0);
    ASSERT_EQ(bytes, 4u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf.data() + 0xc), 4), "cdef");

    setReq(2, path, 0, 0);
    mFs->handleGpfs(buf.data(), buf.size(), &n);
    memcpy(&status, buf.data() + 4, 4);
    EXPECT_EQ(status, 0);
    EXPECT_NE(access(path.c_str(), F_OK), 0);

    setReq(1, "/etc/passwd", 0, 1);
    mFs->handleGpfs(buf.data(), buf.size(), &n);
    memcpy(&status, buf.data() + 4, 4);
    EXPECT_EQ(status, -EACCES);
}

}  // namespace
