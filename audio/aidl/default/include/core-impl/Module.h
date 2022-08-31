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

#pragma once

#include <map>
#include <memory>
#include <set>

#include <aidl/android/hardware/audio/core/BnModule.h>

#include "core-impl/Configuration.h"
#include "core-impl/Stream.h"

namespace aidl::android::hardware::audio::core {

class Module : public BnModule {
  public:
    // This value is used for all AudioPatches and reported by all streams.
    static constexpr int32_t kLatencyMs = 10;

  private:
    ndk::ScopedAStatus setModuleDebug(
            const ::aidl::android::hardware::audio::core::ModuleDebug& in_debug) override;
    ndk::ScopedAStatus connectExternalDevice(
            const ::aidl::android::media::audio::common::AudioPort& in_templateIdAndAdditionalData,
            ::aidl::android::media::audio::common::AudioPort* _aidl_return) override;
    ndk::ScopedAStatus disconnectExternalDevice(int32_t in_portId) override;
    ndk::ScopedAStatus getAudioPatches(std::vector<AudioPatch>* _aidl_return) override;
    ndk::ScopedAStatus getAudioPort(
            int32_t in_portId,
            ::aidl::android::media::audio::common::AudioPort* _aidl_return) override;
    ndk::ScopedAStatus getAudioPortConfigs(
            std::vector<::aidl::android::media::audio::common::AudioPortConfig>* _aidl_return)
            override;
    ndk::ScopedAStatus getAudioPorts(
            std::vector<::aidl::android::media::audio::common::AudioPort>* _aidl_return) override;
    ndk::ScopedAStatus getAudioRoutes(std::vector<AudioRoute>* _aidl_return) override;
    ndk::ScopedAStatus getAudioRoutesForAudioPort(
            int32_t in_portId,
            std::vector<::aidl::android::hardware::audio::core::AudioRoute>* _aidl_return) override;
    ndk::ScopedAStatus openInputStream(
            const ::aidl::android::hardware::audio::core::IModule::OpenInputStreamArguments&
                    in_args,
            ::aidl::android::hardware::audio::core::IModule::OpenInputStreamReturn* _aidl_return)
            override;
    ndk::ScopedAStatus openOutputStream(
            const ::aidl::android::hardware::audio::core::IModule::OpenOutputStreamArguments&
                    in_args,
            ::aidl::android::hardware::audio::core::IModule::OpenOutputStreamReturn* _aidl_return)
            override;
    ndk::ScopedAStatus setAudioPatch(const AudioPatch& in_requested,
                                     AudioPatch* _aidl_return) override;
    ndk::ScopedAStatus setAudioPortConfig(
            const ::aidl::android::media::audio::common::AudioPortConfig& in_requested,
            ::aidl::android::media::audio::common::AudioPortConfig* out_suggested,
            bool* _aidl_return) override;
    ndk::ScopedAStatus resetAudioPatch(int32_t in_patchId) override;
    ndk::ScopedAStatus resetAudioPortConfig(int32_t in_portConfigId) override;

    void cleanUpPatch(int32_t patchId);
    ndk::ScopedAStatus createStreamContext(
            int32_t in_portConfigId, int64_t in_bufferSizeFrames,
            ::aidl::android::hardware::audio::core::StreamContext* out_context);
    ndk::ScopedAStatus findPortIdForNewStream(
            int32_t in_portConfigId, ::aidl::android::media::audio::common::AudioPort** port);
    internal::Configuration& getConfig();
    void registerPatch(const AudioPatch& patch);
    void updateStreamsConnectedState(const AudioPatch& oldPatch, const AudioPatch& newPatch);

    // This value is used for all AudioPatches.
    static constexpr int32_t kMinimumStreamBufferSizeFrames = 16;
    // The maximum stream buffer size is 1 GiB = 2 ** 30 bytes;
    static constexpr int32_t kMaximumStreamBufferSizeBytes = 1 << 30;

    std::unique_ptr<internal::Configuration> mConfig;
    ModuleDebug mDebug;
    // ids of ports created at runtime via 'connectExternalDevice'.
    std::set<int32_t> mConnectedDevicePorts;
    Streams mStreams;
    // Maps port ids and port config ids to patch ids.
    // Multimap because both ports and configs can be used by multiple patches.
    std::multimap<int32_t, int32_t> mPatches;
};

}  // namespace aidl::android::hardware::audio::core
