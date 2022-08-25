/*
 * Copyright (C) 2022 The Android Open Source Project
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

#define LOG_TAG "AHAL_Stream"
#define LOG_NDEBUG 0
#include <android-base/logging.h>

#include "core-impl/Stream.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioOffloadInfo;

namespace aidl::android::hardware::audio::core {

StreamIn::StreamIn(const SinkMetadata& sinkMetadata) : mMetadata(sinkMetadata) {}

ndk::ScopedAStatus StreamIn::close() {
    LOG(DEBUG) << __func__;
    if (!mIsClosed) {
        mIsClosed = true;
        return ndk::ScopedAStatus::ok();
    } else {
        LOG(ERROR) << __func__ << ": stream was already closed";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
}

ndk::ScopedAStatus StreamIn::updateMetadata(const SinkMetadata& in_sinkMetadata) {
    LOG(DEBUG) << __func__;
    if (!mIsClosed) {
        mMetadata = in_sinkMetadata;
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": stream was closed";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

StreamOut::StreamOut(const SourceMetadata& sourceMetadata,
                     const std::optional<AudioOffloadInfo>& offloadInfo)
    : mMetadata(sourceMetadata), mOffloadInfo(offloadInfo) {}

ndk::ScopedAStatus StreamOut::close() {
    LOG(DEBUG) << __func__;
    if (!mIsClosed) {
        mIsClosed = true;
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": stream was already closed";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

ndk::ScopedAStatus StreamOut::updateMetadata(const SourceMetadata& in_sourceMetadata) {
    LOG(DEBUG) << __func__;
    if (!mIsClosed) {
        mMetadata = in_sourceMetadata;
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": stream was closed";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

}  // namespace aidl::android::hardware::audio::core
