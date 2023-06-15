/*********************************************************
*
*  Copyright (C) 2014 by Vitaliy Vitsentiy
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
*
*********************************************************/


#ifndef __ctpl_stl_thread_pool_H__
#define __ctpl_stl_thread_pool_H__

#include <omp.h>
#include <chrono>
#include "knowhere/common/Log.h"
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <exception>
#include <future>
#include <mutex>
#include <queue>



// thread pool to run user's functors with signature
//      ret func(int id, other_params)
// where id is the index of the thread that runs the functor
// ret is some return type


namespace ctpl {

    namespace detail {
        class ValueTracker {
        public:
            ValueTracker() : previousValue(0), currentValue(0), previousTime(std::chrono::steady_clock::now()), previousPrintTime(std::chrono::steady_clock::now()), totalTime(0) {
                valueTimes.resize(64, 0);
            }

            void updateValue(int newValue) {
                std::unique_lock<std::mutex> lock(this->mu);
                if (newValue != currentValue) {
                    std::chrono::steady_clock::time_point currentTime = std::chrono::steady_clock::now();
                    std::chrono::duration<double> timeDifference = std::chrono::duration_cast<std::chrono::duration<double>>(currentTime - previousTime);
                    totalTime += timeDifference.count();

                    valueTimes[previousValue] += timeDifference.count();


                    previousValue = currentValue;
                    currentValue = newValue;
                    previousTime = currentTime;
                }
                // printTime if previousPrintTime is more than 10 seconds ago
                std::chrono::steady_clock::time_point currentTime = std::chrono::steady_clock::now();
                std::chrono::duration<double> timeDifference = std::chrono::duration_cast<std::chrono::duration<double>>(currentTime - previousPrintTime);
                if (timeDifference.count() > 10) {
                    LOG_KNOWHERE_INFO_ << "------------ Start ------ Time for each value:" << std::endl;
                    for (int i = 0; i < 64; i++) {
                        LOG_KNOWHERE_INFO_ << i << ": " << valueTimes[i] << " seconds" << std::endl;
                    }
                    LOG_KNOWHERE_INFO_ << "------------ End   ------ Total time: " << totalTime << " seconds" << std::endl;
                    valueTimes.resize(64, 0);
                    totalTime = 0;
                    previousPrintTime = currentTime;
                }
            }

        private:
            int previousValue;
            int currentValue;
            std::chrono::steady_clock::time_point previousTime;
            std::chrono::steady_clock::time_point previousPrintTime;
            double totalTime;
            std::vector<double> valueTimes;
            std::mutex mu;
        };

        template <typename T>
        class Queue {
        public:
            bool push(T const & value) {
                std::unique_lock<std::mutex> lock(this->mutex);
                if (this->q.size() >= this->limit_) {
                    cv.wait(lock);
                }
                this->q.push(value);
                return true;
            }
            // deletes the retrieved element, do not use for non integral types
            bool pop(T & v) {
                std::unique_lock<std::mutex> lock(this->mutex);
                if (this->q.empty())
                    return false;
                v = this->q.front();
                this->q.pop();
                this->cv.notify_one();
                return true;
            }
            bool empty() {
                std::unique_lock<std::mutex> lock(this->mutex);
                return this->q.empty();
            }
            void
            set_limit(uint32_t limit) {
                limit_ = limit;
            }
        private:
            std::queue<T> q;
            std::mutex mutex;
            std::condition_variable cv;
            uint32_t limit_ = 0;
        };
    }

    class thread_pool {

    public:

        thread_pool() { this->init(); }
        thread_pool(int nThreads) {
            this->init();
            this->resize(nThreads);
            LOG_KNOWHERE_INFO_ << "thread_pool queue limit: " << 1;
            this->q.set_limit(1);
        }

        // the destructor waits for all the functions in the queue to be finished
        ~thread_pool() {
            this->stop(true);
        }

        // get the number of running threads in the pool
        int size() { return static_cast<int>(this->threads.size()); }

        // number of idle threads
        int n_idle() { return this->nWaiting; }
        std::thread & get_thread(int i) { return *this->threads[i]; }

        // change the number of threads in the pool
        // should be called from one thread, otherwise be careful to not interleave, also with this->stop()
        // nThreads must be >= 0
        void resize(int nThreads) {
            if (!this->isStop && !this->isDone) {
                int oldNThreads = static_cast<int>(this->threads.size());
                if (oldNThreads <= nThreads) {  // if the number of threads is increased
                    this->threads.resize(nThreads);
                    this->flags.resize(nThreads);

                    for (int i = oldNThreads; i < nThreads; ++i) {
                        this->flags[i] = std::make_shared<std::atomic<bool>>(false);
                        this->set_thread(i);
                    }
                }
                else {  // the number of threads is decreased
                    for (int i = oldNThreads - 1; i >= nThreads; --i) {
                        *this->flags[i] = true;  // this thread will finish
                        this->threads[i]->detach();
                    }
                    {
                        // stop the detached threads that were waiting
                        std::unique_lock<std::mutex> lock(this->mutex);
                        this->cv.notify_all();
                    }
                    this->threads.resize(nThreads);  // safe to delete because the threads are detached
                    this->flags.resize(nThreads);  // safe to delete because the threads have copies of shared_ptr of the flags, not originals
                }
            }
        }

        // empty the queue
        void clear_queue() {
            std::function<void(int id)> * _f;
            while (this->q.pop(_f))
                delete _f; // empty the queue
        }

        // pops a functional wrapper to the original function
        std::function<void(int)> pop() {
            std::function<void(int id)> * _f = nullptr;
            this->q.pop(_f);
            std::unique_ptr<std::function<void(int id)>> func(_f); // at return, delete the function even if an exception occurred
            std::function<void(int)> f;
            if (_f)
                f = *_f;
            return f;
        }

        // wait for all computing threads to finish and stop all threads
        // may be called asynchronously to not pause the calling thread while waiting
        // if isWait == true, all the functions in the queue are run, otherwise the queue is cleared without running the functions
        void stop(bool isWait = false) {
            if (!isWait) {
                if (this->isStop)
                    return;
                this->isStop = true;
                for (int i = 0, n = this->size(); i < n; ++i) {
                    *this->flags[i] = true;  // command the threads to stop
                }
                this->clear_queue();  // empty the queue
            }
            else {
                if (this->isDone || this->isStop)
                    return;
                this->isDone = true;  // give the waiting threads a command to finish
            }
            {
                std::unique_lock<std::mutex> lock(this->mutex);
                this->cv.notify_all();  // stop all waiting threads
            }
            for (int i = 0; i < static_cast<int>(this->threads.size()); ++i) {  // wait for the computing threads to finish
                    if (this->threads[i]->joinable())
                        this->threads[i]->join();
            }
            // if there were no threads in the pool but some functors in the queue, the functors are not deleted by the threads
            // therefore delete them here
            this->clear_queue();
            this->threads.clear();
            this->flags.clear();
        }

        template<typename F, typename... Rest>
        auto push(F && f, Rest&&... rest) ->std::future<decltype(f(0, rest...))> {
            auto pck = std::make_shared<std::packaged_task<decltype(f(0, rest...))(int)>>(
                std::bind(std::forward<F>(f), std::placeholders::_1, std::forward<Rest>(rest)...)
                );
            auto _f = new std::function<void(int id)>([pck](int id) {
                (*pck)(id);
            });
            this->q.push(_f);
            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_one();
            return pck->get_future();
        }

        // run the user's function that excepts argument int - id of the running thread. returned value is templatized
        // operator returns std::future, where the user can get the result and rethrow the catched exceptins
        template<typename F>
        auto push(F && f) ->std::future<decltype(f(0))> {
            auto pck = std::make_shared<std::packaged_task<decltype(f(0))(int)>>(std::forward<F>(f));
            auto _f = new std::function<void(int id)>([pck](int id) {
                (*pck)(id);
            });
            this->q.push(_f);
            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_one();
            return pck->get_future();
        }


    private:

        // deleted
        thread_pool(const thread_pool &);// = delete;
        thread_pool(thread_pool &&);// = delete;
        thread_pool & operator=(const thread_pool &);// = delete;
        thread_pool & operator=(thread_pool &&);// = delete;

        void set_thread(int i) {
            std::shared_ptr<std::atomic<bool>> flag(this->flags[i]); // a copy of the shared ptr to the flag
            auto f = [this, i, flag/* a copy of the shared ptr to the flag */]() {
                omp_set_num_threads(1);
                std::atomic<bool> & _flag = *flag;
                std::function<void(int id)> * _f;
                bool isPop = this->q.pop(_f);
                while (true) {
                    while (isPop) {  // if there is anything in the queue
                        std::unique_ptr<std::function<void(int id)>> func(_f); // at return, delete the function even if an exception occurred
                        (*_f)(i);
                        if (_flag)
                            return;  // the thread is wanted to stop, return even if the queue is not empty yet
                        else
                            isPop = this->q.pop(_f);
                    }
                    // the queue is empty here, wait for the next command
                    std::unique_lock<std::mutex> lock(this->mutex);
                    ++this->nWaiting;
                    this->tracker.updateValue(this->nWaiting);
                    this->cv.wait(lock, [this, &_f, &isPop, &_flag](){ isPop = this->q.pop(_f); return isPop || this->isDone || _flag; });
                    --this->nWaiting;
                    this->tracker.updateValue(this->nWaiting);
                    if (!isPop)
                        return;  // if the queue is empty and this->isDone == true or *flag then return
                }
            };
            this->threads[i].reset(new std::thread(f)); // compiler may not support std::make_unique()
        }

        void init() {
            this->nWaiting = 0; this->isStop = false; this->isDone = false;
            LOG_KNOWHERE_INFO_ << "thread_pool in ctpl-std initiating";
        }

        std::vector<std::unique_ptr<std::thread>> threads;
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;
        detail::Queue<std::function<void(int id)> *> q;
        std::atomic<bool> isDone;
        std::atomic<bool> isStop;
        std::atomic<int> nWaiting;  // how many threads are waiting
        detail::ValueTracker tracker;


        std::mutex mutex;
        std::condition_variable cv;
    };

}

#endif // __ctpl_stl_thread_pool_H__
