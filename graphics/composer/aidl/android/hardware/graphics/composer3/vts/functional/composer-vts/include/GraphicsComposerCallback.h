/**
 * Copyright (c) 2021, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <aidl/android/hardware/graphics/composer3/IComposerCallback.h>

#include <android-base/thread_annotations.h>
#include <mutex>
#include <unordered_set>

namespace aidl::android::hardware::graphics::composer3::vts {

// IComposerCallback to be installed with IComposerClient::registerCallback.
class GraphicsComposerCallback : public IComposerCallback {
  public:
    void setVsyncAllowed(bool allowed);

    std::vector<int64_t> getDisplays() const;

    int32_t getInvalidHotplugCount() const;

    int32_t getInvalidRefreshCount() const;

    int32_t getInvalidVsyncCount() const;

    int32_t getInvalidVsyncPeriodChangeCount() const;

    int32_t getInvalidSeamlessPossibleCount() const;

    std::optional<VsyncPeriodChangeTimeline> takeLastVsyncPeriodChangeTimeline();

  private:
    virtual ::ndk::ScopedAStatus onHotplug(int64_t in_display, bool in_connected) override;
    virtual ::ndk::ScopedAStatus onRefresh(int64_t in_display) override;
    virtual ::ndk::ScopedAStatus onSeamlessPossible(int64_t in_display) override;
    virtual ::ndk::ScopedAStatus onVsync(int64_t in_display, int64_t in_timestamp,
                                         int32_t in_vsyncPeriodNanos) override;
    virtual ::ndk::ScopedAStatus onVsyncPeriodTimingChanged(
            int64_t in_display,
            const ::aidl::android::hardware::graphics::composer3::VsyncPeriodChangeTimeline&
                    in_updatedTimeline) override;

    ::ndk::SpAIBinder asBinder() override;
    bool isRemote() override;

    mutable std::mutex mMutex;
    // the set of all currently connected displays
    std::unordered_set<int64_t> mDisplays GUARDED_BY(mMutex);
    // true only when vsync is enabled
    bool mVsyncAllowed GUARDED_BY(mMutex) = true;

    std::optional<VsyncPeriodChangeTimeline> mTimeline GUARDED_BY(mMutex);

    // track invalid callbacks
    int32_t mInvalidHotplugCount GUARDED_BY(mMutex) = 0;
    int32_t mInvalidRefreshCount GUARDED_BY(mMutex) = 0;
    int32_t mInvalidVsyncCount GUARDED_BY(mMutex) = 0;
    int32_t mInvalidVsyncPeriodChangeCount GUARDED_BY(mMutex) = 0;
    int32_t mInvalidSeamlessPossibleCount GUARDED_BY(mMutex) = 0;
};

}  // namespace aidl::android::hardware::graphics::composer3::vts