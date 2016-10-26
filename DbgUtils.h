/*
 * Copyright (C) 2015-2016 Antmicro
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DBGUTILS_H
#define DBGUTILS_H

#include <utils/Log.h>
#include <utils/Timers.h>
#include <utils/Vector.h>
#include <log/log.h>

#ifndef NDEBUG
#define NDEBUG 0
#endif

#if !NDEBUG
# define DEBUG_CODE(x) x
#else
# define DEBUG_CODE(x)
#endif

/******************************************************************************\
                                  AutoLogCall
\******************************************************************************/

namespace android {
namespace DbgUtils {

class AutoLogCall {
public:
    AutoLogCall(const char *name): mName(name) {
        static __thread unsigned level;
        mLevel = &level;
        ALOGV("%*s+ %s", *mLevel * 4, "", name);
        ++(*mLevel);
    }
    ~AutoLogCall() {
        --(*mLevel);
        ALOGV("%*s- %s", *mLevel * 4, "", mName);
    }

private:
    const char *mName;
    unsigned *mLevel;
};

}; /* namespace DbgUtils */
}; /* namespace android */

#if !NDEBUG
# define DBGUTILS_AUTOLOGCALL(name) android::DbgUtils::AutoLogCall _autoLogCall(name)
#else
# define DBGUTILS_AUTOLOGCALL(name)
#endif

/******************************************************************************\
                                   FpsCounter
\******************************************************************************/

#define FPSCOUNTER_VARIABLE_NAME _fpsCounterState
#define FPSCOUNTER_CLASS_WITH_NS android::DbgUtils::FpsCounter

#include <utils/Timers.h>

namespace android {
namespace DbgUtils {

template<int SAMPLES>
class FpsCounter {
public:
    FpsCounter(): mTimeId(SAMPLES - 1), mSamplesCount(0) {
        for(size_t i = 0; i < SAMPLES; ++i)
            mTime[i] = 0;
    }

    double fps(int samples = SAMPLES - 1) {
        if(samples >= mSamplesCount)
            samples = mSamplesCount - 1;
        if(samples < 1)
            samples = 1;

        unsigned pastTime;
        pastTime = (mTimeId + SAMPLES - samples) % SAMPLES;

        return samples * 1000000000.0f / (mTime[mTimeId] - mTime[pastTime]);
    }

    void tick() {
        mTimeId = (mTimeId + 1) % SAMPLES;
        mTime[mTimeId] = systemTime();
        if(mSamplesCount < SAMPLES)
            ++mSamplesCount;
    }

    nsecs_t mTime[SAMPLES];
    unsigned mTimeId;
    unsigned mSamplesCount;
};

}; /* namespace DbgUtils */
}; /* namespace android */

#if !NDEBUG
# define FPSCOUNTER_HERE(samples) \
    static FPSCOUNTER_CLASS_WITH_NS<samples> FPSCOUNTER_VARIABLE_NAME; \
    FPSCOUNTER_VARIABLE_NAME.tick();

# define FPSCOUNTER_VALUE(samples) \
    (FPSCOUNTER_VARIABLE_NAME.fps(samples))

#else
# define FPSCOUNTER_HERE(samples)
# define FPSCOUNTER_VALUE(samples) (0.0f)
#endif

/******************************************************************************\
                                   Benchmark
\******************************************************************************/

#define BENCHMARK_VARIABLE_NAME _benchmarkState
#define BENCHMARK_CLASS_WITH_NS android::DbgUtils::Benchmark

#include <cstdio>
#include <cstring>
#include <stdint.h>
#include <time.h>

#ifdef HAVE_ANDROID_OS
# include <utils/Vector.h>
# define BENCHMARK_VECTOR android::Vector
/* Android vector's operator [] is const */
# define BENCHMARK_VECTOR_ITEM_EDIT(vec, id) (vec).editItemAt(id)
#else
# include <vector>
# define BENCHMARK_VECTOR std::vector
# define BENCHMARK_VECTOR_ITEM_EDIT(vec, id) (vec)[id]
#endif

namespace android {
namespace DbgUtils {

template <int SAMPLES>
class Benchmark {
public:
    Benchmark() {}
    ~Benchmark() {}

    int begin(const char *sectionName) {
        int id = mSections.size();
        for(int i = 0; i < mSections.size(); ++i) {
            if(!strcmp(mSections[i].name, sectionName)) {
                id = i;
                break;
            }
        }
        if(id == mSections.size()) {
            Section newSection;
            newSection.name = sectionName;
            for(unsigned i = 0; i < SAMPLES; ++i) {
                newSection.time[i] = 0;
            }
            newSection.timeId = SAMPLES - 1;
            newSection.samplesCount = 0;
            newSection.count = 0;
            mSections.push_back(newSection);
        }
        Section &sec = BENCHMARK_VECTOR_ITEM_EDIT(mSections, id);
        if(sec.count == 0) {
            sec.timeId = (sec.timeId + 1) % SAMPLES;
            sec.time[sec.timeId] = 0;
            if(sec.samplesCount < SAMPLES)
                ++sec.samplesCount;
        }
        ++sec.count;
        sec.time[sec.timeId] -= currentTimeNs();
        return id;
    }

    void end(int id) {
        Section &sec = BENCHMARK_VECTOR_ITEM_EDIT(mSections, id);
        sec.time[sec.timeId] += currentTimeNs();
    }

    bool formatString(char *out, size_t len, int precision) {
        for(int i = 0; i < mSections.size() && len > 0; ++i) {
            Section &sec = BENCHMARK_VECTOR_ITEM_EDIT(mSections, i);
            double t = (double)sec.time[sec.timeId] / 1000000000.0f;
            if(sec.count == 0)
                t = 0.0f;
            double avg = 0.0f;
            for(unsigned j = 0; j < sec.samplesCount; ++j) {
                const unsigned jj = (SAMPLES + sec.timeId - j) % SAMPLES;
                avg += (double)sec.time[jj];
            }
            avg = avg / sec.samplesCount / 1000000000.0f;
            size_t printedNum;
            printedNum = snprintf(out, len, "%s%s[%u]: %.*f (%.*f)",
                                  i != 0 ? "  " : "",
                                  sec.name, sec.count,
                                  precision, t, precision, avg);
            len -= printedNum;
            out += printedNum;
        }
        return (len > 0);
    }

    void newCycle() {
        for(int i = 0; i < mSections.size(); ++i) {
            Section &sec = BENCHMARK_VECTOR_ITEM_EDIT(mSections, i);
            sec.count = 0;
        }
    }

private:
    int64_t currentTimeNs() {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return t.tv_sec * 1000000000LL + t.tv_nsec;
    }

    struct Section {
        const char *name;
        int64_t     time[SAMPLES];
        unsigned    timeId;
        unsigned    samplesCount;
        unsigned    count;
    };
    BENCHMARK_VECTOR<Section> mSections;
};

}; /* namespace DbgUtils */
}; /* namespace android */

#if !NDEBUG
# define BENCHMARK_HERE(samples) \
    static BENCHMARK_CLASS_WITH_NS<samples> BENCHMARK_VARIABLE_NAME; \
    BENCHMARK_VARIABLE_NAME.newCycle();

# define BENCHMARK_SECTION(name) \
    for(int _benchmarkId = BENCHMARK_VARIABLE_NAME.begin(name); \
        _benchmarkId >= 0; \
        BENCHMARK_VARIABLE_NAME.end(_benchmarkId), _benchmarkId = -1)

# define BENCHMARK_STRING(str, len, prec) \
    BENCHMARK_VARIABLE_NAME.formatString(str, len, prec);
#else
# define BENCHMARK_HERE(samples)
# define BENCHMARK_SECTION(name) if(true)
# define BENCHMARK_STRING(str, len, prec) (*str = '\0')
#endif

#undef BENCHMARK_VECTOR

#endif // DBGUTILS_H
