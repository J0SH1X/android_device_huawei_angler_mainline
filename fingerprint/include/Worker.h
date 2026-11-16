/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>

#include "FpcEngine.h"

namespace angler::fpc {

/**
 * One thread, FIFO. All ISessionCallback calls and all TA traffic happen here, never on binder
 * threads (binder calls into the framework from a binder thread can exhaust the pool).
 */
class Worker {
  public:
    Worker() : mThread([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> l(mLock);
            mStop = true;
            if (mIdleToken) mIdleToken->trigger();
        }
        mCond.notify_all();
        mThread.join();
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> l(mLock);
            mQueue.push_back(std::move(fn));
            if (mIdleToken) mIdleToken->trigger();  // preempt the idle task
        }
        mCond.notify_one();
    }

    /** Runs whenever the queue is empty; must return promptly once its token is triggered. */
    void setIdleTask(std::function<void(CancelToken&)> fn) {
        {
            std::lock_guard<std::mutex> l(mLock);
            mIdle = std::move(fn);
        }
        mCond.notify_one();
    }

  private:
    void loop() {
        for (;;) {
            std::function<void()> fn;
            std::function<void(CancelToken&)> idle;
            std::shared_ptr<CancelToken> token;
            {
                std::unique_lock<std::mutex> l(mLock);
                mCond.wait(l, [this] { return mStop || !mQueue.empty() || mIdle; });
                if (mStop && mQueue.empty()) return;
                if (!mQueue.empty()) {
                    fn = std::move(mQueue.front());
                    mQueue.pop_front();
                } else {
                    idle = mIdle;
                    token = mIdleToken = std::make_shared<CancelToken>();
                }
            }
            if (fn) {
                fn();
            } else {
                idle(*token);
                std::lock_guard<std::mutex> l(mLock);
                mIdleToken.reset();
            }
        }
    }

    std::mutex mLock;
    std::condition_variable mCond;
    std::deque<std::function<void()>> mQueue;
    bool mStop = false;
    std::function<void(CancelToken&)> mIdle;
    std::shared_ptr<CancelToken> mIdleToken;
    std::thread mThread;  // last: starts after the members above are constructed
};

}  // namespace angler::fpc
