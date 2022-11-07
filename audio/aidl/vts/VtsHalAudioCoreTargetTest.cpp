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

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#define LOG_TAG "VtsHalAudioCore"
#include <android-base/logging.h>

#include <StreamWorker.h>
#include <Utils.h>
#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <aidl/android/hardware/audio/core/IConfig.h>
#include <aidl/android/hardware/audio/core/IModule.h>
#include <aidl/android/media/audio/common/AudioIoFlags.h>
#include <aidl/android/media/audio/common/AudioOutputFlags.h>
#include <android-base/chrono_utils.h>
#include <android/binder_enums.h>
#include <fmq/AidlMessageQueue.h>

#include "AudioHalBinderServiceUtil.h"
#include "ModuleConfig.h"
#include "TestUtils.h"

using namespace android;
using aidl::android::hardware::audio::common::PlaybackTrackMetadata;
using aidl::android::hardware::audio::common::RecordTrackMetadata;
using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::hardware::audio::core::AudioPatch;
using aidl::android::hardware::audio::core::AudioRoute;
using aidl::android::hardware::audio::core::IModule;
using aidl::android::hardware::audio::core::IStreamIn;
using aidl::android::hardware::audio::core::IStreamOut;
using aidl::android::hardware::audio::core::ModuleDebug;
using aidl::android::hardware::audio::core::StreamDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::android::media::audio::common::AudioContentType;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortDeviceExt;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::AudioSource;
using aidl::android::media::audio::common::AudioUsage;
using android::hardware::audio::common::isBitPositionFlagSet;
using android::hardware::audio::common::StreamLogic;
using android::hardware::audio::common::StreamWorker;
using ndk::enum_range;
using ndk::ScopedAStatus;

template <typename T>
auto findById(std::vector<T>& v, int32_t id) {
    return std::find_if(v.begin(), v.end(), [&](const auto& e) { return e.id == id; });
}

template <typename C>
std::vector<int32_t> GetNonExistentIds(const C& allIds) {
    if (allIds.empty()) {
        return std::vector<int32_t>{-1, 0, 1};
    }
    std::vector<int32_t> nonExistentIds;
    nonExistentIds.push_back(*std::min_element(allIds.begin(), allIds.end()) - 1);
    nonExistentIds.push_back(*std::max_element(allIds.begin(), allIds.end()) + 1);
    return nonExistentIds;
}

AudioDeviceAddress GenerateUniqueDeviceAddress() {
    static int nextId = 1;
    // TODO: Use connection-specific ID.
    return AudioDeviceAddress::make<AudioDeviceAddress::Tag::id>(std::to_string(++nextId));
}

// All 'With*' classes are move-only because they are associated with some
// resource or state of a HAL module.
class WithDebugFlags {
  public:
    static WithDebugFlags createNested(const WithDebugFlags& parent) {
        return WithDebugFlags(parent.mFlags);
    }

    WithDebugFlags() {}
    explicit WithDebugFlags(const ModuleDebug& initial) : mInitial(initial), mFlags(initial) {}
    WithDebugFlags(const WithDebugFlags&) = delete;
    WithDebugFlags& operator=(const WithDebugFlags&) = delete;
    ~WithDebugFlags() {
        if (mModule != nullptr) {
            EXPECT_IS_OK(mModule->setModuleDebug(mInitial));
        }
    }
    void SetUp(IModule* module) { ASSERT_IS_OK(module->setModuleDebug(mFlags)); }
    ModuleDebug& flags() { return mFlags; }

  private:
    ModuleDebug mInitial;
    ModuleDebug mFlags;
    IModule* mModule = nullptr;
};

// For consistency, WithAudioPortConfig can start both with a non-existent
// port config, and with an existing one. Existence is determined by the
// id of the provided config. If it's not 0, then WithAudioPortConfig is
// essentially a no-op wrapper.
class WithAudioPortConfig {
  public:
    WithAudioPortConfig() {}
    explicit WithAudioPortConfig(const AudioPortConfig& config) : mInitialConfig(config) {}
    WithAudioPortConfig(const WithAudioPortConfig&) = delete;
    WithAudioPortConfig& operator=(const WithAudioPortConfig&) = delete;
    ~WithAudioPortConfig() {
        if (mModule != nullptr) {
            EXPECT_IS_OK(mModule->resetAudioPortConfig(getId())) << "port config id " << getId();
        }
    }
    void SetUp(IModule* module) {
        ASSERT_NE(AudioPortExt::Tag::unspecified, mInitialConfig.ext.getTag())
                << "config: " << mInitialConfig.toString();
        // Negotiation is allowed for device ports because the HAL module is
        // allowed to provide an empty profiles list for attached devices.
        ASSERT_NO_FATAL_FAILURE(
                SetUpImpl(module, mInitialConfig.ext.getTag() == AudioPortExt::Tag::device));
    }
    int32_t getId() const { return mConfig.id; }
    const AudioPortConfig& get() const { return mConfig; }

  private:
    void SetUpImpl(IModule* module, bool negotiate) {
        if (mInitialConfig.id == 0) {
            AudioPortConfig suggested;
            bool applied = false;
            ASSERT_IS_OK(module->setAudioPortConfig(mInitialConfig, &suggested, &applied))
                    << "Config: " << mInitialConfig.toString();
            if (!applied && negotiate) {
                mInitialConfig = suggested;
                ASSERT_NO_FATAL_FAILURE(SetUpImpl(module, false))
                        << " while applying suggested config: " << suggested.toString();
            } else {
                ASSERT_TRUE(applied) << "Suggested: " << suggested.toString();
                mConfig = suggested;
                mModule = module;
            }
        } else {
            mConfig = mInitialConfig;
        }
    }

    AudioPortConfig mInitialConfig;
    IModule* mModule = nullptr;
    AudioPortConfig mConfig;
};

// Can be used as a base for any test here, does not depend on the fixture GTest parameters.
class AudioCoreModuleBase {
  public:
    // The default buffer size is used mostly for negative tests.
    static constexpr int kDefaultBufferSizeFrames = 256;

    void SetUpImpl(const std::string& moduleName) {
        ASSERT_NO_FATAL_FAILURE(ConnectToService(moduleName));
        debug.flags().simulateDeviceConnections = true;
        ASSERT_NO_FATAL_FAILURE(debug.SetUp(module.get()));
    }

    void TearDownImpl() {
        if (module != nullptr) {
            EXPECT_IS_OK(module->setModuleDebug(ModuleDebug{}));
        }
    }

    void ConnectToService(const std::string& moduleName) {
        module = IModule::fromBinder(binderUtil.connectToService(moduleName));
        ASSERT_NE(module, nullptr);
    }

    void RestartService() {
        ASSERT_NE(module, nullptr);
        moduleConfig.reset();
        module = IModule::fromBinder(binderUtil.restartService());
        ASSERT_NE(module, nullptr);
    }

    void ApplyEveryConfig(const std::vector<AudioPortConfig>& configs) {
        for (const auto& config : configs) {
            ASSERT_NE(0, config.portId);
            WithAudioPortConfig portConfig(config);
            ASSERT_NO_FATAL_FAILURE(portConfig.SetUp(module.get()));  // calls setAudioPortConfig
            EXPECT_EQ(config.portId, portConfig.get().portId);
            std::vector<AudioPortConfig> retrievedPortConfigs;
            ASSERT_IS_OK(module->getAudioPortConfigs(&retrievedPortConfigs));
            const int32_t portConfigId = portConfig.getId();
            auto configIt = std::find_if(
                    retrievedPortConfigs.begin(), retrievedPortConfigs.end(),
                    [&portConfigId](const auto& retrConf) { return retrConf.id == portConfigId; });
            EXPECT_NE(configIt, retrievedPortConfigs.end())
                    << "Port config id returned by setAudioPortConfig: " << portConfigId
                    << " is not found in the list returned by getAudioPortConfigs";
            if (configIt != retrievedPortConfigs.end()) {
                EXPECT_EQ(portConfig.get(), *configIt)
                        << "Applied port config returned by setAudioPortConfig: "
                        << portConfig.get().toString()
                        << " is not the same as retrieved via getAudioPortConfigs: "
                        << configIt->toString();
            }
        }
    }

    template <typename Entity>
    void GetAllEntityIds(std::set<int32_t>* entityIds,
                         ScopedAStatus (IModule::*getter)(std::vector<Entity>*),
                         const std::string& errorMessage) {
        std::vector<Entity> entities;
        { ASSERT_IS_OK((module.get()->*getter)(&entities)); }
        std::transform(entities.begin(), entities.end(),
                       std::inserter(*entityIds, entityIds->begin()),
                       [](const auto& entity) { return entity.id; });
        EXPECT_EQ(entities.size(), entityIds->size()) << errorMessage;
    }

    void GetAllPatchIds(std::set<int32_t>* patchIds) {
        return GetAllEntityIds<AudioPatch>(
                patchIds, &IModule::getAudioPatches,
                "IDs of audio patches returned by IModule.getAudioPatches are not unique");
    }

    void GetAllPortIds(std::set<int32_t>* portIds) {
        return GetAllEntityIds<AudioPort>(
                portIds, &IModule::getAudioPorts,
                "IDs of audio ports returned by IModule.getAudioPorts are not unique");
    }

    void GetAllPortConfigIds(std::set<int32_t>* portConfigIds) {
        return GetAllEntityIds<AudioPortConfig>(
                portConfigIds, &IModule::getAudioPortConfigs,
                "IDs of audio port configs returned by IModule.getAudioPortConfigs are not unique");
    }

    void SetUpModuleConfig() {
        if (moduleConfig == nullptr) {
            moduleConfig = std::make_unique<ModuleConfig>(module.get());
            ASSERT_EQ(EX_NONE, moduleConfig->getStatus().getExceptionCode())
                    << "ModuleConfig init error: " << moduleConfig->getError();
        }
    }

    std::shared_ptr<IModule> module;
    std::unique_ptr<ModuleConfig> moduleConfig;
    AudioHalBinderServiceUtil binderUtil;
    WithDebugFlags debug;
};

class AudioCoreModule : public AudioCoreModuleBase, public testing::TestWithParam<std::string> {
  public:
    void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpImpl(GetParam())); }

    void TearDown() override { ASSERT_NO_FATAL_FAILURE(TearDownImpl()); }
};

class WithDevicePortConnectedState {
  public:
    explicit WithDevicePortConnectedState(const AudioPort& idAndData) : mIdAndData(idAndData) {}
    WithDevicePortConnectedState(const AudioPort& id, const AudioDeviceAddress& address)
        : mIdAndData(setAudioPortAddress(id, address)) {}
    WithDevicePortConnectedState(const WithDevicePortConnectedState&) = delete;
    WithDevicePortConnectedState& operator=(const WithDevicePortConnectedState&) = delete;
    ~WithDevicePortConnectedState() {
        if (mModule != nullptr) {
            EXPECT_IS_OK(mModule->disconnectExternalDevice(getId()))
                    << "when disconnecting device port ID " << getId();
        }
    }
    void SetUp(IModule* module) {
        ASSERT_IS_OK(module->connectExternalDevice(mIdAndData, &mConnectedPort))
                << "when connecting device port ID & data " << mIdAndData.toString();
        ASSERT_NE(mIdAndData.id, getId())
                << "ID of the connected port must not be the same as the ID of the template port";
        mModule = module;
    }
    int32_t getId() const { return mConnectedPort.id; }
    const AudioPort& get() { return mConnectedPort; }

  private:
    static AudioPort setAudioPortAddress(const AudioPort& id, const AudioDeviceAddress& address) {
        AudioPort result = id;
        result.ext.get<AudioPortExt::Tag::device>().device.address = address;
        return result;
    }

    const AudioPort mIdAndData;
    IModule* mModule = nullptr;
    AudioPort mConnectedPort;
};

class StreamContext {
  public:
    typedef AidlMessageQueue<StreamDescriptor::Command, SynchronizedReadWrite> CommandMQ;
    typedef AidlMessageQueue<StreamDescriptor::Reply, SynchronizedReadWrite> ReplyMQ;
    typedef AidlMessageQueue<int8_t, SynchronizedReadWrite> DataMQ;

    explicit StreamContext(const StreamDescriptor& descriptor)
        : mFrameSizeBytes(descriptor.frameSizeBytes),
          mCommandMQ(new CommandMQ(descriptor.command)),
          mReplyMQ(new ReplyMQ(descriptor.reply)),
          mBufferSizeFrames(descriptor.bufferSizeFrames),
          mDataMQ(maybeCreateDataMQ(descriptor)) {}
    void checkIsValid() const {
        EXPECT_NE(0UL, mFrameSizeBytes);
        ASSERT_NE(nullptr, mCommandMQ);
        EXPECT_TRUE(mCommandMQ->isValid());
        ASSERT_NE(nullptr, mReplyMQ);
        EXPECT_TRUE(mReplyMQ->isValid());
        if (mDataMQ != nullptr) {
            EXPECT_TRUE(mDataMQ->isValid());
            EXPECT_GE(mDataMQ->getQuantumCount() * mDataMQ->getQuantumSize(),
                      mFrameSizeBytes * mBufferSizeFrames)
                    << "Data MQ actual buffer size is "
                       "less than the buffer size as specified by the descriptor";
        }
    }
    size_t getBufferSizeBytes() const { return mFrameSizeBytes * mBufferSizeFrames; }
    size_t getBufferSizeFrames() const { return mBufferSizeFrames; }
    CommandMQ* getCommandMQ() const { return mCommandMQ.get(); }
    DataMQ* getDataMQ() const { return mDataMQ.get(); }
    ReplyMQ* getReplyMQ() const { return mReplyMQ.get(); }

  private:
    static std::unique_ptr<DataMQ> maybeCreateDataMQ(const StreamDescriptor& descriptor) {
        using Tag = StreamDescriptor::AudioBuffer::Tag;
        if (descriptor.audio.getTag() == Tag::fmq) {
            return std::make_unique<DataMQ>(descriptor.audio.get<Tag::fmq>());
        }
        return nullptr;
    }

    const size_t mFrameSizeBytes;
    std::unique_ptr<CommandMQ> mCommandMQ;
    std::unique_ptr<ReplyMQ> mReplyMQ;
    const size_t mBufferSizeFrames;
    std::unique_ptr<DataMQ> mDataMQ;
};

class StreamLogicDriver {
  public:
    virtual ~StreamLogicDriver() = default;
    // Return 'true' to stop the worker.
    virtual bool done() = 0;
    // For 'Writer' logic, if the 'actualSize' is 0, write is skipped.
    // The 'fmqByteCount' from the returned command is passed as is to the HAL.
    virtual StreamDescriptor::Command getNextCommand(int maxDataSize,
                                                     int* actualSize = nullptr) = 0;
    // Return 'true' to indicate that no further processing is needed,
    // for example, the driver is expecting a bad status to be returned.
    // The logic cycle will return with 'CONTINUE' status. Otherwise,
    // the reply will be validated and then passed to 'processValidReply'.
    virtual bool interceptRawReply(const StreamDescriptor::Reply& reply) = 0;
    // Return 'false' to indicate that the contents of the reply are unexpected.
    // Will abort the logic cycle.
    virtual bool processValidReply(const StreamDescriptor::Reply& reply) = 0;
};

class StreamCommonLogic : public StreamLogic {
  protected:
    StreamCommonLogic(const StreamContext& context, StreamLogicDriver* driver)
        : mCommandMQ(context.getCommandMQ()),
          mReplyMQ(context.getReplyMQ()),
          mDataMQ(context.getDataMQ()),
          mData(context.getBufferSizeBytes()),
          mDriver(driver) {}
    StreamContext::CommandMQ* getCommandMQ() const { return mCommandMQ; }
    StreamContext::ReplyMQ* getReplyMQ() const { return mReplyMQ; }
    StreamLogicDriver* getDriver() const { return mDriver; }

    std::string init() override { return ""; }

    StreamContext::CommandMQ* mCommandMQ;
    StreamContext::ReplyMQ* mReplyMQ;
    StreamContext::DataMQ* mDataMQ;
    std::vector<int8_t> mData;
    StreamLogicDriver* const mDriver;
};

class StreamReaderLogic : public StreamCommonLogic {
  public:
    StreamReaderLogic(const StreamContext& context, StreamLogicDriver* driver)
        : StreamCommonLogic(context, driver) {}

  protected:
    Status cycle() override {
        if (getDriver()->done()) {
            return Status::EXIT;
        }
        StreamDescriptor::Command command = getDriver()->getNextCommand(mData.size());
        if (!mCommandMQ->writeBlocking(&command, 1)) {
            LOG(ERROR) << __func__ << ": writing of command into MQ failed";
            return Status::ABORT;
        }
        StreamDescriptor::Reply reply{};
        if (!mReplyMQ->readBlocking(&reply, 1)) {
            LOG(ERROR) << __func__ << ": reading of reply from MQ failed";
            return Status::ABORT;
        }
        if (getDriver()->interceptRawReply(reply)) {
            return Status::CONTINUE;
        }
        if (reply.status != STATUS_OK) {
            LOG(ERROR) << __func__ << ": received error status: " << statusToString(reply.status);
            return Status::ABORT;
        }
        if (reply.fmqByteCount < 0 || reply.fmqByteCount > command.fmqByteCount) {
            LOG(ERROR) << __func__
                       << ": received invalid byte count in the reply: " << reply.fmqByteCount;
            return Status::ABORT;
        }
        if (static_cast<size_t>(reply.fmqByteCount) != mDataMQ->availableToRead()) {
            LOG(ERROR) << __func__
                       << ": the byte count in the reply is not the same as the amount of "
                       << "data available in the MQ: " << reply.fmqByteCount
                       << " != " << mDataMQ->availableToRead();
        }
        if (reply.latencyMs < 0 && reply.latencyMs != StreamDescriptor::LATENCY_UNKNOWN) {
            LOG(ERROR) << __func__ << ": received invalid latency value: " << reply.latencyMs;
            return Status::ABORT;
        }
        if (reply.xrunFrames < 0) {
            LOG(ERROR) << __func__ << ": received invalid xrunFrames value: " << reply.xrunFrames;
            return Status::ABORT;
        }
        if (std::find(enum_range<StreamDescriptor::State>().begin(),
                      enum_range<StreamDescriptor::State>().end(),
                      reply.state) == enum_range<StreamDescriptor::State>().end()) {
            LOG(ERROR) << __func__ << ": received invalid stream state: " << toString(reply.state);
            return Status::ABORT;
        }
        const bool acceptedReply = getDriver()->processValidReply(reply);
        if (const size_t readCount = mDataMQ->availableToRead(); readCount > 0) {
            std::vector<int8_t> data(readCount);
            if (mDataMQ->read(data.data(), readCount)) {
                memcpy(mData.data(), data.data(), std::min(mData.size(), data.size()));
                goto checkAcceptedReply;
            }
            LOG(ERROR) << __func__ << ": reading of " << readCount << " data bytes from MQ failed";
            return Status::ABORT;
        }  // readCount == 0
    checkAcceptedReply:
        if (acceptedReply) {
            return Status::CONTINUE;
        }
        LOG(ERROR) << __func__ << ": unacceptable reply: " << reply.toString();
        return Status::ABORT;
    }
};
using StreamReader = StreamWorker<StreamReaderLogic>;

class StreamWriterLogic : public StreamCommonLogic {
  public:
    StreamWriterLogic(const StreamContext& context, StreamLogicDriver* driver)
        : StreamCommonLogic(context, driver) {}

  protected:
    Status cycle() override {
        if (getDriver()->done()) {
            return Status::EXIT;
        }
        int actualSize = 0;
        StreamDescriptor::Command command = getDriver()->getNextCommand(mData.size(), &actualSize);
        if (actualSize != 0 && !mDataMQ->write(mData.data(), mData.size())) {
            LOG(ERROR) << __func__ << ": writing of " << mData.size() << " bytes to MQ failed";
            return Status::ABORT;
        }
        if (!mCommandMQ->writeBlocking(&command, 1)) {
            LOG(ERROR) << __func__ << ": writing of command into MQ failed";
            return Status::ABORT;
        }
        StreamDescriptor::Reply reply{};
        if (!mReplyMQ->readBlocking(&reply, 1)) {
            LOG(ERROR) << __func__ << ": reading of reply from MQ failed";
            return Status::ABORT;
        }
        if (getDriver()->interceptRawReply(reply)) {
            return Status::CONTINUE;
        }
        if (reply.status != STATUS_OK) {
            LOG(ERROR) << __func__ << ": received error status: " << statusToString(reply.status);
            return Status::ABORT;
        }
        if (reply.fmqByteCount < 0 || reply.fmqByteCount > command.fmqByteCount) {
            LOG(ERROR) << __func__
                       << ": received invalid byte count in the reply: " << reply.fmqByteCount;
            return Status::ABORT;
        }
        if (mDataMQ->availableToWrite() != mDataMQ->getQuantumCount()) {
            LOG(ERROR) << __func__ << ": the HAL module did not consume all data from the data MQ: "
                       << "available to write " << mDataMQ->availableToWrite()
                       << ", total size: " << mDataMQ->getQuantumCount();
            return Status::ABORT;
        }
        if (reply.latencyMs < 0 && reply.latencyMs != StreamDescriptor::LATENCY_UNKNOWN) {
            LOG(ERROR) << __func__ << ": received invalid latency value: " << reply.latencyMs;
            return Status::ABORT;
        }
        if (reply.xrunFrames < 0) {
            LOG(ERROR) << __func__ << ": received invalid xrunFrames value: " << reply.xrunFrames;
            return Status::ABORT;
        }
        if (std::find(enum_range<StreamDescriptor::State>().begin(),
                      enum_range<StreamDescriptor::State>().end(),
                      reply.state) == enum_range<StreamDescriptor::State>().end()) {
            LOG(ERROR) << __func__ << ": received invalid stream state: " << toString(reply.state);
            return Status::ABORT;
        }
        if (getDriver()->processValidReply(reply)) {
            return Status::CONTINUE;
        }
        LOG(ERROR) << __func__ << ": unacceptable reply: " << reply.toString();
        return Status::ABORT;
    }
};
using StreamWriter = StreamWorker<StreamWriterLogic>;

template <typename T>
struct IOTraits {
    static constexpr bool is_input = std::is_same_v<T, IStreamIn>;
    using Worker = std::conditional_t<is_input, StreamReader, StreamWriter>;
};

template <typename Stream>
class WithStream {
  public:
    WithStream() {}
    explicit WithStream(const AudioPortConfig& portConfig) : mPortConfig(portConfig) {}
    WithStream(const WithStream&) = delete;
    WithStream& operator=(const WithStream&) = delete;
    ~WithStream() {
        if (mStream != nullptr) {
            mContext.reset();
            EXPECT_IS_OK(mStream->close()) << "port config id " << getPortId();
        }
    }
    void SetUpPortConfig(IModule* module) { ASSERT_NO_FATAL_FAILURE(mPortConfig.SetUp(module)); }
    ScopedAStatus SetUpNoChecks(IModule* module, long bufferSizeFrames) {
        return SetUpNoChecks(module, mPortConfig.get(), bufferSizeFrames);
    }
    ScopedAStatus SetUpNoChecks(IModule* module, const AudioPortConfig& portConfig,
                                long bufferSizeFrames);
    void SetUp(IModule* module, long bufferSizeFrames) {
        ASSERT_NO_FATAL_FAILURE(SetUpPortConfig(module));
        ASSERT_IS_OK(SetUpNoChecks(module, bufferSizeFrames)) << "port config id " << getPortId();
        ASSERT_NE(nullptr, mStream) << "port config id " << getPortId();
        EXPECT_GE(mDescriptor.bufferSizeFrames, bufferSizeFrames)
                << "actual buffer size must be no less than requested";
        mContext.emplace(mDescriptor);
        ASSERT_NO_FATAL_FAILURE(mContext.value().checkIsValid());
    }
    Stream* get() const { return mStream.get(); }
    const StreamContext* getContext() const { return mContext ? &(mContext.value()) : nullptr; }
    std::shared_ptr<Stream> getSharedPointer() const { return mStream; }
    const AudioPortConfig& getPortConfig() const { return mPortConfig.get(); }
    int32_t getPortId() const { return mPortConfig.getId(); }

  private:
    WithAudioPortConfig mPortConfig;
    std::shared_ptr<Stream> mStream;
    StreamDescriptor mDescriptor;
    std::optional<StreamContext> mContext;
};

SinkMetadata GenerateSinkMetadata(const AudioPortConfig& portConfig) {
    RecordTrackMetadata trackMeta;
    trackMeta.source = AudioSource::MIC;
    trackMeta.gain = 1.0;
    trackMeta.channelMask = portConfig.channelMask.value();
    SinkMetadata metadata;
    metadata.tracks.push_back(trackMeta);
    return metadata;
}

template <>
ScopedAStatus WithStream<IStreamIn>::SetUpNoChecks(IModule* module,
                                                   const AudioPortConfig& portConfig,
                                                   long bufferSizeFrames) {
    aidl::android::hardware::audio::core::IModule::OpenInputStreamArguments args;
    args.portConfigId = portConfig.id;
    args.sinkMetadata = GenerateSinkMetadata(portConfig);
    args.bufferSizeFrames = bufferSizeFrames;
    aidl::android::hardware::audio::core::IModule::OpenInputStreamReturn ret;
    ScopedAStatus status = module->openInputStream(args, &ret);
    if (status.isOk()) {
        mStream = std::move(ret.stream);
        mDescriptor = std::move(ret.desc);
    }
    return status;
}

SourceMetadata GenerateSourceMetadata(const AudioPortConfig& portConfig) {
    PlaybackTrackMetadata trackMeta;
    trackMeta.usage = AudioUsage::MEDIA;
    trackMeta.contentType = AudioContentType::MUSIC;
    trackMeta.gain = 1.0;
    trackMeta.channelMask = portConfig.channelMask.value();
    SourceMetadata metadata;
    metadata.tracks.push_back(trackMeta);
    return metadata;
}

template <>
ScopedAStatus WithStream<IStreamOut>::SetUpNoChecks(IModule* module,
                                                    const AudioPortConfig& portConfig,
                                                    long bufferSizeFrames) {
    aidl::android::hardware::audio::core::IModule::OpenOutputStreamArguments args;
    args.portConfigId = portConfig.id;
    args.sourceMetadata = GenerateSourceMetadata(portConfig);
    args.offloadInfo = ModuleConfig::generateOffloadInfoIfNeeded(portConfig);
    args.bufferSizeFrames = bufferSizeFrames;
    aidl::android::hardware::audio::core::IModule::OpenOutputStreamReturn ret;
    ScopedAStatus status = module->openOutputStream(args, &ret);
    if (status.isOk()) {
        mStream = std::move(ret.stream);
        mDescriptor = std::move(ret.desc);
    }
    return status;
}

class WithAudioPatch {
  public:
    WithAudioPatch() {}
    WithAudioPatch(const AudioPortConfig& srcPortConfig, const AudioPortConfig& sinkPortConfig)
        : mSrcPortConfig(srcPortConfig), mSinkPortConfig(sinkPortConfig) {}
    WithAudioPatch(bool sinkIsCfg1, const AudioPortConfig& portConfig1,
                   const AudioPortConfig& portConfig2)
        : mSrcPortConfig(sinkIsCfg1 ? portConfig2 : portConfig1),
          mSinkPortConfig(sinkIsCfg1 ? portConfig1 : portConfig2) {}
    WithAudioPatch(const WithAudioPatch&) = delete;
    WithAudioPatch& operator=(const WithAudioPatch&) = delete;
    ~WithAudioPatch() {
        if (mModule != nullptr && mPatch.id != 0) {
            EXPECT_IS_OK(mModule->resetAudioPatch(mPatch.id)) << "patch id " << getId();
        }
    }
    void SetUpPortConfigs(IModule* module) {
        ASSERT_NO_FATAL_FAILURE(mSrcPortConfig.SetUp(module));
        ASSERT_NO_FATAL_FAILURE(mSinkPortConfig.SetUp(module));
    }
    ScopedAStatus SetUpNoChecks(IModule* module) {
        mModule = module;
        mPatch.sourcePortConfigIds = std::vector<int32_t>{mSrcPortConfig.getId()};
        mPatch.sinkPortConfigIds = std::vector<int32_t>{mSinkPortConfig.getId()};
        return mModule->setAudioPatch(mPatch, &mPatch);
    }
    void SetUp(IModule* module) {
        ASSERT_NO_FATAL_FAILURE(SetUpPortConfigs(module));
        ASSERT_IS_OK(SetUpNoChecks(module)) << "source port config id " << mSrcPortConfig.getId()
                                            << "; sink port config id " << mSinkPortConfig.getId();
        EXPECT_GT(mPatch.minimumStreamBufferSizeFrames, 0) << "patch id " << getId();
        for (auto latencyMs : mPatch.latenciesMs) {
            EXPECT_GT(latencyMs, 0) << "patch id " << getId();
        }
    }
    int32_t getId() const { return mPatch.id; }
    const AudioPatch& get() const { return mPatch; }
    const AudioPortConfig& getSinkPortConfig() const { return mSinkPortConfig.get(); }
    const AudioPortConfig& getSrcPortConfig() const { return mSrcPortConfig.get(); }
    const AudioPortConfig& getPortConfig(bool getSink) const {
        return getSink ? getSinkPortConfig() : getSrcPortConfig();
    }

  private:
    WithAudioPortConfig mSrcPortConfig;
    WithAudioPortConfig mSinkPortConfig;
    IModule* mModule = nullptr;
    AudioPatch mPatch;
};

TEST_P(AudioCoreModule, Published) {
    // SetUp must complete with no failures.
}

TEST_P(AudioCoreModule, CanBeRestarted) {
    ASSERT_NO_FATAL_FAILURE(RestartService());
}

TEST_P(AudioCoreModule, PortIdsAreUnique) {
    std::set<int32_t> portIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortIds(&portIds));
}

TEST_P(AudioCoreModule, GetAudioPortsIsStable) {
    std::vector<AudioPort> ports1;
    ASSERT_IS_OK(module->getAudioPorts(&ports1));
    std::vector<AudioPort> ports2;
    ASSERT_IS_OK(module->getAudioPorts(&ports2));
    ASSERT_EQ(ports1.size(), ports2.size())
            << "Sizes of audio port arrays do not match across consequent calls to getAudioPorts";
    std::sort(ports1.begin(), ports1.end());
    std::sort(ports2.begin(), ports2.end());
    EXPECT_EQ(ports1, ports2);
}

TEST_P(AudioCoreModule, GetAudioRoutesIsStable) {
    std::vector<AudioRoute> routes1;
    ASSERT_IS_OK(module->getAudioRoutes(&routes1));
    std::vector<AudioRoute> routes2;
    ASSERT_IS_OK(module->getAudioRoutes(&routes2));
    ASSERT_EQ(routes1.size(), routes2.size())
            << "Sizes of audio route arrays do not match across consequent calls to getAudioRoutes";
    std::sort(routes1.begin(), routes1.end());
    std::sort(routes2.begin(), routes2.end());
    EXPECT_EQ(routes1, routes2);
}

TEST_P(AudioCoreModule, GetAudioRoutesAreValid) {
    std::vector<AudioRoute> routes;
    ASSERT_IS_OK(module->getAudioRoutes(&routes));
    for (const auto& route : routes) {
        std::set<int32_t> sources(route.sourcePortIds.begin(), route.sourcePortIds.end());
        EXPECT_NE(0UL, sources.size())
                << "empty audio port sinks in the audio route: " << route.toString();
        EXPECT_EQ(sources.size(), route.sourcePortIds.size())
                << "IDs of audio port sinks are not unique in the audio route: "
                << route.toString();
    }
}

TEST_P(AudioCoreModule, GetAudioRoutesPortIdsAreValid) {
    std::set<int32_t> portIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortIds(&portIds));
    std::vector<AudioRoute> routes;
    ASSERT_IS_OK(module->getAudioRoutes(&routes));
    for (const auto& route : routes) {
        EXPECT_EQ(1UL, portIds.count(route.sinkPortId))
                << route.sinkPortId << " sink port id is unknown";
        for (const auto& source : route.sourcePortIds) {
            EXPECT_EQ(1UL, portIds.count(source)) << source << " source port id is unknown";
        }
    }
}

TEST_P(AudioCoreModule, GetAudioRoutesForAudioPort) {
    std::set<int32_t> portIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortIds(&portIds));
    if (portIds.empty()) {
        GTEST_SKIP() << "No ports in the module.";
    }
    for (const auto portId : portIds) {
        std::vector<AudioRoute> routes;
        EXPECT_IS_OK(module->getAudioRoutesForAudioPort(portId, &routes));
        for (const auto& r : routes) {
            if (r.sinkPortId != portId) {
                const auto& srcs = r.sourcePortIds;
                EXPECT_TRUE(std::find(srcs.begin(), srcs.end(), portId) != srcs.end())
                        << " port ID " << portId << " does not used by the route " << r.toString();
            }
        }
    }
    for (const auto portId : GetNonExistentIds(portIds)) {
        std::vector<AudioRoute> routes;
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->getAudioRoutesForAudioPort(portId, &routes))
                << "port ID " << portId;
    }
}

TEST_P(AudioCoreModule, CheckDevicePorts) {
    std::vector<AudioPort> ports;
    ASSERT_IS_OK(module->getAudioPorts(&ports));
    std::optional<int32_t> defaultOutput, defaultInput;
    std::set<AudioDevice> inputs, outputs;
    const int defaultDeviceFlag = 1 << AudioPortDeviceExt::FLAG_INDEX_DEFAULT_DEVICE;
    for (const auto& port : ports) {
        if (port.ext.getTag() != AudioPortExt::Tag::device) continue;
        const auto& devicePort = port.ext.get<AudioPortExt::Tag::device>();
        EXPECT_NE(AudioDeviceType::NONE, devicePort.device.type.type);
        EXPECT_NE(AudioDeviceType::IN_DEFAULT, devicePort.device.type.type);
        EXPECT_NE(AudioDeviceType::OUT_DEFAULT, devicePort.device.type.type);
        if (devicePort.device.type.type > AudioDeviceType::IN_DEFAULT &&
            devicePort.device.type.type < AudioDeviceType::OUT_DEFAULT) {
            EXPECT_EQ(AudioIoFlags::Tag::input, port.flags.getTag());
        } else if (devicePort.device.type.type > AudioDeviceType::OUT_DEFAULT) {
            EXPECT_EQ(AudioIoFlags::Tag::output, port.flags.getTag());
        }
        EXPECT_FALSE((devicePort.flags & defaultDeviceFlag) != 0 &&
                     !devicePort.device.type.connection.empty())
                << "Device port " << port.id
                << " must be permanently attached to be set as default";
        if ((devicePort.flags & defaultDeviceFlag) != 0) {
            if (port.flags.getTag() == AudioIoFlags::Tag::output) {
                EXPECT_FALSE(defaultOutput.has_value())
                        << "At least two output device ports are declared as default: "
                        << defaultOutput.value() << " and " << port.id;
                defaultOutput = port.id;
                EXPECT_EQ(0UL, outputs.count(devicePort.device))
                        << "Non-unique output device: " << devicePort.device.toString();
                outputs.insert(devicePort.device);
            } else if (port.flags.getTag() == AudioIoFlags::Tag::input) {
                EXPECT_FALSE(defaultInput.has_value())
                        << "At least two input device ports are declared as default: "
                        << defaultInput.value() << " and " << port.id;
                defaultInput = port.id;
                EXPECT_EQ(0UL, inputs.count(devicePort.device))
                        << "Non-unique input device: " << devicePort.device.toString();
                inputs.insert(devicePort.device);
            } else {
                FAIL() << "Invalid AudioIoFlags Tag: " << toString(port.flags.getTag());
            }
        }
    }
}

TEST_P(AudioCoreModule, CheckMixPorts) {
    std::vector<AudioPort> ports;
    ASSERT_IS_OK(module->getAudioPorts(&ports));
    std::optional<int32_t> primaryMixPort;
    for (const auto& port : ports) {
        if (port.ext.getTag() != AudioPortExt::Tag::mix) continue;
        const auto& mixPort = port.ext.get<AudioPortExt::Tag::mix>();
        if (port.flags.getTag() == AudioIoFlags::Tag::output &&
            isBitPositionFlagSet(port.flags.get<AudioIoFlags::Tag::output>(),
                                 AudioOutputFlags::PRIMARY)) {
            EXPECT_FALSE(primaryMixPort.has_value())
                    << "At least two mix ports have PRIMARY flag set: " << primaryMixPort.value()
                    << " and " << port.id;
            primaryMixPort = port.id;
            EXPECT_EQ(1, mixPort.maxOpenStreamCount)
                    << "Primary mix port " << port.id << " can not have maxOpenStreamCount "
                    << mixPort.maxOpenStreamCount;
        }
    }
}

TEST_P(AudioCoreModule, GetAudioPort) {
    std::set<int32_t> portIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortIds(&portIds));
    if (portIds.empty()) {
        GTEST_SKIP() << "No ports in the module.";
    }
    for (const auto portId : portIds) {
        AudioPort port;
        EXPECT_IS_OK(module->getAudioPort(portId, &port));
        EXPECT_EQ(portId, port.id);
    }
    for (const auto portId : GetNonExistentIds(portIds)) {
        AudioPort port;
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->getAudioPort(portId, &port))
                << "port ID " << portId;
    }
}

TEST_P(AudioCoreModule, SetUpModuleConfig) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    // Send the module config to logcat to facilitate failures investigation.
    LOG(INFO) << "SetUpModuleConfig: " << moduleConfig->toString();
}

// Verify that HAL module reports for a connected device port at least one non-dynamic profile,
// that is, a profile with actual supported configuration.
// Note: This test relies on simulation of external device connections by the HAL module.
TEST_P(AudioCoreModule, GetAudioPortWithExternalDevices) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    std::vector<AudioPort> ports = moduleConfig->getExternalDevicePorts();
    if (ports.empty()) {
        GTEST_SKIP() << "No external devices in the module.";
    }
    for (const auto& port : ports) {
        AudioPort portWithData = port;
        portWithData.ext.get<AudioPortExt::Tag::device>().device.address =
                GenerateUniqueDeviceAddress();
        WithDevicePortConnectedState portConnected(portWithData);
        ASSERT_NO_FATAL_FAILURE(portConnected.SetUp(module.get()));
        const int32_t connectedPortId = portConnected.getId();
        ASSERT_NE(portWithData.id, connectedPortId);
        ASSERT_EQ(portWithData.ext.getTag(), portConnected.get().ext.getTag());
        EXPECT_EQ(portWithData.ext.get<AudioPortExt::Tag::device>().device,
                  portConnected.get().ext.get<AudioPortExt::Tag::device>().device);
        // Verify that 'getAudioPort' and 'getAudioPorts' return the same connected port.
        AudioPort connectedPort;
        EXPECT_IS_OK(module->getAudioPort(connectedPortId, &connectedPort))
                << "port ID " << connectedPortId;
        EXPECT_EQ(portConnected.get(), connectedPort);
        const auto& portProfiles = connectedPort.profiles;
        EXPECT_NE(0UL, portProfiles.size())
                << "Connected port has no profiles: " << connectedPort.toString();
        const auto dynamicProfileIt =
                std::find_if(portProfiles.begin(), portProfiles.end(), [](const auto& profile) {
                    return profile.format.type == AudioFormatType::DEFAULT;
                });
        EXPECT_EQ(portProfiles.end(), dynamicProfileIt) << "Connected port contains dynamic "
                                                        << "profiles: " << connectedPort.toString();

        std::vector<AudioPort> allPorts;
        ASSERT_IS_OK(module->getAudioPorts(&allPorts));
        const auto allPortsIt = findById(allPorts, connectedPortId);
        EXPECT_NE(allPorts.end(), allPortsIt);
        if (allPortsIt != allPorts.end()) {
            EXPECT_EQ(portConnected.get(), *allPortsIt);
        }
    }
}

TEST_P(AudioCoreModule, OpenStreamInvalidPortConfigId) {
    std::set<int32_t> portConfigIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortConfigIds(&portConfigIds));
    for (const auto portConfigId : GetNonExistentIds(portConfigIds)) {
        {
            aidl::android::hardware::audio::core::IModule::OpenInputStreamArguments args;
            args.portConfigId = portConfigId;
            args.bufferSizeFrames = kDefaultBufferSizeFrames;
            aidl::android::hardware::audio::core::IModule::OpenInputStreamReturn ret;
            EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->openInputStream(args, &ret))
                    << "port config ID " << portConfigId;
            EXPECT_EQ(nullptr, ret.stream);
        }
        {
            aidl::android::hardware::audio::core::IModule::OpenOutputStreamArguments args;
            args.portConfigId = portConfigId;
            args.bufferSizeFrames = kDefaultBufferSizeFrames;
            aidl::android::hardware::audio::core::IModule::OpenOutputStreamReturn ret;
            EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->openOutputStream(args, &ret))
                    << "port config ID " << portConfigId;
            EXPECT_EQ(nullptr, ret.stream);
        }
    }
}

TEST_P(AudioCoreModule, PortConfigIdsAreUnique) {
    std::set<int32_t> portConfigIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortConfigIds(&portConfigIds));
}

TEST_P(AudioCoreModule, PortConfigPortIdsAreValid) {
    std::set<int32_t> portIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortIds(&portIds));
    std::vector<AudioPortConfig> portConfigs;
    ASSERT_IS_OK(module->getAudioPortConfigs(&portConfigs));
    for (const auto& config : portConfigs) {
        EXPECT_EQ(1UL, portIds.count(config.portId))
                << config.portId << " port id is unknown, config id " << config.id;
    }
}

TEST_P(AudioCoreModule, ResetAudioPortConfigInvalidId) {
    std::set<int32_t> portConfigIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortConfigIds(&portConfigIds));
    for (const auto portConfigId : GetNonExistentIds(portConfigIds)) {
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->resetAudioPortConfig(portConfigId))
                << "port config ID " << portConfigId;
    }
}

// Verify that for the audio port configs provided by the HAL after init, resetting
// the config does not delete it, but brings it back to the initial config.
TEST_P(AudioCoreModule, ResetAudioPortConfigToInitialValue) {
    std::vector<AudioPortConfig> portConfigsBefore;
    ASSERT_IS_OK(module->getAudioPortConfigs(&portConfigsBefore));
    // TODO: Change port configs according to port profiles.
    for (const auto& c : portConfigsBefore) {
        EXPECT_IS_OK(module->resetAudioPortConfig(c.id)) << "port config ID " << c.id;
    }
    std::vector<AudioPortConfig> portConfigsAfter;
    ASSERT_IS_OK(module->getAudioPortConfigs(&portConfigsAfter));
    for (const auto& c : portConfigsBefore) {
        auto afterIt = findById<AudioPortConfig>(portConfigsAfter, c.id);
        EXPECT_NE(portConfigsAfter.end(), afterIt)
                << " port config ID " << c.id << " was removed by reset";
        if (afterIt != portConfigsAfter.end()) {
            EXPECT_EQ(c, *afterIt);
        }
    }
}

TEST_P(AudioCoreModule, SetAudioPortConfigSuggestedConfig) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    auto srcMixPort = moduleConfig->getSourceMixPortForAttachedDevice();
    if (!srcMixPort.has_value()) {
        GTEST_SKIP() << "No mix port for attached output devices";
    }
    AudioPortConfig portConfig;
    AudioPortConfig suggestedConfig;
    portConfig.portId = srcMixPort.value().id;
    {
        bool applied = true;
        ASSERT_IS_OK(module->setAudioPortConfig(portConfig, &suggestedConfig, &applied))
                << "Config: " << portConfig.toString();
        EXPECT_FALSE(applied);
    }
    EXPECT_EQ(0, suggestedConfig.id);
    EXPECT_TRUE(suggestedConfig.sampleRate.has_value());
    EXPECT_TRUE(suggestedConfig.channelMask.has_value());
    EXPECT_TRUE(suggestedConfig.format.has_value());
    EXPECT_TRUE(suggestedConfig.flags.has_value());
    WithAudioPortConfig applied(suggestedConfig);
    ASSERT_NO_FATAL_FAILURE(applied.SetUp(module.get()));
    const AudioPortConfig& appliedConfig = applied.get();
    EXPECT_NE(0, appliedConfig.id);
    EXPECT_TRUE(appliedConfig.sampleRate.has_value());
    EXPECT_EQ(suggestedConfig.sampleRate.value(), appliedConfig.sampleRate.value());
    EXPECT_TRUE(appliedConfig.channelMask.has_value());
    EXPECT_EQ(suggestedConfig.channelMask.value(), appliedConfig.channelMask.value());
    EXPECT_TRUE(appliedConfig.format.has_value());
    EXPECT_EQ(suggestedConfig.format.value(), appliedConfig.format.value());
    EXPECT_TRUE(appliedConfig.flags.has_value());
    EXPECT_EQ(suggestedConfig.flags.value(), appliedConfig.flags.value());
}

TEST_P(AudioCoreModule, SetAllAttachedDevicePortConfigs) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    ASSERT_NO_FATAL_FAILURE(ApplyEveryConfig(moduleConfig->getPortConfigsForAttachedDevicePorts()));
}

// Note: This test relies on simulation of external device connections by the HAL module.
TEST_P(AudioCoreModule, SetAllExternalDevicePortConfigs) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    std::vector<AudioPort> ports = moduleConfig->getExternalDevicePorts();
    if (ports.empty()) {
        GTEST_SKIP() << "No external devices in the module.";
    }
    for (const auto& port : ports) {
        WithDevicePortConnectedState portConnected(port, GenerateUniqueDeviceAddress());
        ASSERT_NO_FATAL_FAILURE(portConnected.SetUp(module.get()));
        ASSERT_NO_FATAL_FAILURE(
                ApplyEveryConfig(moduleConfig->getPortConfigsForDevicePort(portConnected.get())));
    }
}

TEST_P(AudioCoreModule, SetAllStaticAudioPortConfigs) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    ASSERT_NO_FATAL_FAILURE(ApplyEveryConfig(moduleConfig->getPortConfigsForMixPorts()));
}

TEST_P(AudioCoreModule, SetAudioPortConfigInvalidPortId) {
    std::set<int32_t> portIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortIds(&portIds));
    for (const auto portId : GetNonExistentIds(portIds)) {
        AudioPortConfig portConfig, suggestedConfig;
        bool applied;
        portConfig.portId = portId;
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT,
                      module->setAudioPortConfig(portConfig, &suggestedConfig, &applied))
                << "port ID " << portId;
        EXPECT_FALSE(suggestedConfig.format.has_value());
        EXPECT_FALSE(suggestedConfig.channelMask.has_value());
        EXPECT_FALSE(suggestedConfig.sampleRate.has_value());
    }
}

TEST_P(AudioCoreModule, SetAudioPortConfigInvalidPortConfigId) {
    std::set<int32_t> portConfigIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortConfigIds(&portConfigIds));
    for (const auto portConfigId : GetNonExistentIds(portConfigIds)) {
        AudioPortConfig portConfig, suggestedConfig;
        bool applied;
        portConfig.id = portConfigId;
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT,
                      module->setAudioPortConfig(portConfig, &suggestedConfig, &applied))
                << "port config ID " << portConfigId;
        EXPECT_FALSE(suggestedConfig.format.has_value());
        EXPECT_FALSE(suggestedConfig.channelMask.has_value());
        EXPECT_FALSE(suggestedConfig.sampleRate.has_value());
    }
}

TEST_P(AudioCoreModule, TryConnectMissingDevice) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    std::vector<AudioPort> ports = moduleConfig->getExternalDevicePorts();
    if (ports.empty()) {
        GTEST_SKIP() << "No external devices in the module.";
    }
    AudioPort ignored;
    WithDebugFlags doNotSimulateConnections = WithDebugFlags::createNested(debug);
    doNotSimulateConnections.flags().simulateDeviceConnections = false;
    ASSERT_NO_FATAL_FAILURE(doNotSimulateConnections.SetUp(module.get()));
    for (const auto& port : ports) {
        AudioPort portWithData = port;
        portWithData.ext.get<AudioPortExt::Tag::device>().device.address =
                GenerateUniqueDeviceAddress();
        EXPECT_STATUS(EX_ILLEGAL_STATE, module->connectExternalDevice(portWithData, &ignored))
                << "static port " << portWithData.toString();
    }
}

TEST_P(AudioCoreModule, TryChangingConnectionSimulationMidway) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    std::vector<AudioPort> ports = moduleConfig->getExternalDevicePorts();
    if (ports.empty()) {
        GTEST_SKIP() << "No external devices in the module.";
    }
    WithDevicePortConnectedState portConnected(*ports.begin(), GenerateUniqueDeviceAddress());
    ASSERT_NO_FATAL_FAILURE(portConnected.SetUp(module.get()));
    ModuleDebug midwayDebugChange = debug.flags();
    midwayDebugChange.simulateDeviceConnections = false;
    EXPECT_STATUS(EX_ILLEGAL_STATE, module->setModuleDebug(midwayDebugChange))
            << "when trying to disable connections simulation while having a connected device";
}

TEST_P(AudioCoreModule, ConnectDisconnectExternalDeviceInvalidPorts) {
    AudioPort ignored;
    std::set<int32_t> portIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPortIds(&portIds));
    for (const auto portId : GetNonExistentIds(portIds)) {
        AudioPort invalidPort;
        invalidPort.id = portId;
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->connectExternalDevice(invalidPort, &ignored))
                << "port ID " << portId << ", when setting CONNECTED state";
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->disconnectExternalDevice(portId))
                << "port ID " << portId << ", when setting DISCONNECTED state";
    }

    std::vector<AudioPort> ports;
    ASSERT_IS_OK(module->getAudioPorts(&ports));
    for (const auto& port : ports) {
        if (port.ext.getTag() != AudioPortExt::Tag::device) {
            EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->connectExternalDevice(port, &ignored))
                    << "non-device port ID " << port.id << " when setting CONNECTED state";
            EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->disconnectExternalDevice(port.id))
                    << "non-device port ID " << port.id << " when setting DISCONNECTED state";
        } else {
            const auto& devicePort = port.ext.get<AudioPortExt::Tag::device>();
            if (devicePort.device.type.connection.empty()) {
                EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->connectExternalDevice(port, &ignored))
                        << "for a permanently attached device port ID " << port.id
                        << " when setting CONNECTED state";
                EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->disconnectExternalDevice(port.id))
                        << "for a permanently attached device port ID " << port.id
                        << " when setting DISCONNECTED state";
            }
        }
    }
}

// Note: This test relies on simulation of external device connections by the HAL module.
TEST_P(AudioCoreModule, ConnectDisconnectExternalDeviceTwice) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    AudioPort ignored;
    std::vector<AudioPort> ports = moduleConfig->getExternalDevicePorts();
    if (ports.empty()) {
        GTEST_SKIP() << "No external devices in the module.";
    }
    for (const auto& port : ports) {
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->disconnectExternalDevice(port.id))
                << "when disconnecting already disconnected device port ID " << port.id;
        AudioPort portWithData = port;
        portWithData.ext.get<AudioPortExt::Tag::device>().device.address =
                GenerateUniqueDeviceAddress();
        WithDevicePortConnectedState portConnected(portWithData);
        ASSERT_NO_FATAL_FAILURE(portConnected.SetUp(module.get()));
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT,
                      module->connectExternalDevice(portConnected.get(), &ignored))
                << "when trying to connect a connected device port "
                << portConnected.get().toString();
        EXPECT_STATUS(EX_ILLEGAL_STATE, module->connectExternalDevice(portWithData, &ignored))
                << "when connecting again the external device "
                << portWithData.ext.get<AudioPortExt::Tag::device>().device.toString()
                << "; Returned connected port " << ignored.toString() << " for template "
                << portWithData.toString();
    }
}

// Note: This test relies on simulation of external device connections by the HAL module.
TEST_P(AudioCoreModule, DisconnectExternalDeviceNonResetPortConfig) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    std::vector<AudioPort> ports = moduleConfig->getExternalDevicePorts();
    if (ports.empty()) {
        GTEST_SKIP() << "No external devices in the module.";
    }
    for (const auto& port : ports) {
        WithDevicePortConnectedState portConnected(port, GenerateUniqueDeviceAddress());
        ASSERT_NO_FATAL_FAILURE(portConnected.SetUp(module.get()));
        const auto portConfig = moduleConfig->getSingleConfigForDevicePort(portConnected.get());
        {
            WithAudioPortConfig config(portConfig);
            // Note: if SetUp fails, check the status of 'GetAudioPortWithExternalDevices' test.
            // Our test assumes that 'getAudioPort' returns at least one profile, and it
            // is not a dynamic profile.
            ASSERT_NO_FATAL_FAILURE(config.SetUp(module.get()));
            EXPECT_STATUS(EX_ILLEGAL_STATE, module->disconnectExternalDevice(portConnected.getId()))
                    << "when trying to disconnect device port ID " << port.id
                    << " with active configuration " << config.getId();
        }
    }
}

TEST_P(AudioCoreModule, ExternalDevicePortRoutes) {
    ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    std::vector<AudioPort> ports = moduleConfig->getExternalDevicePorts();
    if (ports.empty()) {
        GTEST_SKIP() << "No external devices in the module.";
    }
    for (const auto& port : ports) {
        std::vector<AudioRoute> routesBefore;
        ASSERT_IS_OK(module->getAudioRoutes(&routesBefore));

        int32_t connectedPortId;
        {
            WithDevicePortConnectedState portConnected(port, GenerateUniqueDeviceAddress());
            ASSERT_NO_FATAL_FAILURE(portConnected.SetUp(module.get()));
            connectedPortId = portConnected.getId();
            std::vector<AudioRoute> connectedPortRoutes;
            ASSERT_IS_OK(module->getAudioRoutesForAudioPort(connectedPortId, &connectedPortRoutes))
                    << "when retrieving routes for connected port id " << connectedPortId;
            // There must be routes for the port to be useful.
            if (connectedPortRoutes.empty()) {
                std::vector<AudioRoute> allRoutes;
                ASSERT_IS_OK(module->getAudioRoutes(&allRoutes));
                ADD_FAILURE() << " no routes returned for the connected port "
                              << portConnected.get().toString()
                              << "; all routes: " << android::internal::ToString(allRoutes);
            }
        }
        std::vector<AudioRoute> ignored;
        ASSERT_STATUS(EX_ILLEGAL_ARGUMENT,
                      module->getAudioRoutesForAudioPort(connectedPortId, &ignored))
                << "when retrieving routes for released connected port id " << connectedPortId;

        std::vector<AudioRoute> routesAfter;
        ASSERT_IS_OK(module->getAudioRoutes(&routesAfter));
        ASSERT_EQ(routesBefore.size(), routesAfter.size())
                << "Sizes of audio route arrays do not match after creating and "
                << "releasing a connected port";
        std::sort(routesBefore.begin(), routesBefore.end());
        std::sort(routesAfter.begin(), routesAfter.end());
        EXPECT_EQ(routesBefore, routesAfter);
    }
}

class StreamLogicDriverInvalidCommand : public StreamLogicDriver {
  public:
    StreamLogicDriverInvalidCommand(const std::vector<StreamDescriptor::Command>& commands)
        : mCommands(commands) {}

    std::string getUnexpectedStatuses() {
        // This method is intended to be called after the worker thread has joined,
        // thus no extra synchronization is needed.
        std::string s;
        if (!mStatuses.empty()) {
            s = std::string("Pairs of (command, actual status): ")
                        .append((android::internal::ToString(mStatuses)));
        }
        return s;
    }

    bool done() override { return mNextCommand >= mCommands.size(); }
    StreamDescriptor::Command getNextCommand(int, int* actualSize) override {
        if (actualSize != nullptr) *actualSize = 0;
        return mCommands[mNextCommand++];
    }
    bool interceptRawReply(const StreamDescriptor::Reply& reply) override {
        if (reply.status != STATUS_BAD_VALUE) {
            std::string s = mCommands[mNextCommand - 1].toString();
            s.append(", ").append(statusToString(reply.status));
            mStatuses.push_back(std::move(s));
            // If the HAL does not recognize the command as invalid,
            // retrieve the data etc.
            return reply.status != STATUS_OK;
        }
        return true;
    }
    bool processValidReply(const StreamDescriptor::Reply&) override { return true; }

  private:
    const std::vector<StreamDescriptor::Command> mCommands;
    size_t mNextCommand = 0;
    std::vector<std::string> mStatuses;
};

template <typename Stream>
class AudioStream : public AudioCoreModule {
  public:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(AudioCoreModule::SetUp());
        ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    }

    void CloseTwice() {
        const auto portConfig = moduleConfig->getSingleConfigForMixPort(IOTraits<Stream>::is_input);
        if (!portConfig.has_value()) {
            GTEST_SKIP() << "No mix port for attached devices";
        }
        std::shared_ptr<Stream> heldStream;
        {
            WithStream<Stream> stream(portConfig.value());
            ASSERT_NO_FATAL_FAILURE(stream.SetUp(module.get(), kDefaultBufferSizeFrames));
            heldStream = stream.getSharedPointer();
        }
        EXPECT_STATUS(EX_ILLEGAL_STATE, heldStream->close()) << "when closing the stream twice";
    }

    void OpenAllConfigs() {
        const auto allPortConfigs =
                moduleConfig->getPortConfigsForMixPorts(IOTraits<Stream>::is_input);
        for (const auto& portConfig : allPortConfigs) {
            WithStream<Stream> stream(portConfig);
            ASSERT_NO_FATAL_FAILURE(stream.SetUp(module.get(), kDefaultBufferSizeFrames));
        }
    }

    void OpenInvalidBufferSize() {
        const auto portConfig = moduleConfig->getSingleConfigForMixPort(IOTraits<Stream>::is_input);
        if (!portConfig.has_value()) {
            GTEST_SKIP() << "No mix port for attached devices";
        }
        WithStream<Stream> stream(portConfig.value());
        ASSERT_NO_FATAL_FAILURE(stream.SetUpPortConfig(module.get()));
        // The buffer size of 1 frame should be impractically small, and thus
        // less than any minimum buffer size suggested by any HAL.
        for (long bufferSize : std::array<long, 4>{-1, 0, 1, std::numeric_limits<long>::max()}) {
            EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, stream.SetUpNoChecks(module.get(), bufferSize))
                    << "for the buffer size " << bufferSize;
            EXPECT_EQ(nullptr, stream.get());
        }
    }

    void OpenInvalidDirection() {
        // Important! The direction of the port config must be reversed.
        const auto portConfig =
                moduleConfig->getSingleConfigForMixPort(!IOTraits<Stream>::is_input);
        if (!portConfig.has_value()) {
            GTEST_SKIP() << "No mix port for attached devices";
        }
        WithStream<Stream> stream(portConfig.value());
        ASSERT_NO_FATAL_FAILURE(stream.SetUpPortConfig(module.get()));
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT,
                      stream.SetUpNoChecks(module.get(), kDefaultBufferSizeFrames))
                << "port config ID " << stream.getPortId();
        EXPECT_EQ(nullptr, stream.get());
    }

    void OpenOverMaxCount() {
        constexpr bool isInput = IOTraits<Stream>::is_input;
        auto ports = moduleConfig->getMixPorts(isInput);
        bool hasSingleRun = false;
        for (const auto& port : ports) {
            const size_t maxStreamCount = port.ext.get<AudioPortExt::Tag::mix>().maxOpenStreamCount;
            if (maxStreamCount == 0 ||
                moduleConfig->getAttachedDevicesPortsForMixPort(isInput, port).empty()) {
                // No restrictions or no permanently attached devices.
                continue;
            }
            auto portConfigs = moduleConfig->getPortConfigsForMixPorts(isInput, port);
            if (portConfigs.size() < maxStreamCount + 1) {
                // Not able to open a sufficient number of streams for this port.
                continue;
            }
            hasSingleRun = true;
            std::optional<WithStream<Stream>> streamWraps[maxStreamCount + 1];
            for (size_t i = 0; i <= maxStreamCount; ++i) {
                streamWraps[i].emplace(portConfigs[i]);
                WithStream<Stream>& stream = streamWraps[i].value();
                if (i < maxStreamCount) {
                    ASSERT_NO_FATAL_FAILURE(stream.SetUp(module.get(), kDefaultBufferSizeFrames));
                } else {
                    ASSERT_NO_FATAL_FAILURE(stream.SetUpPortConfig(module.get()));
                    EXPECT_STATUS(EX_ILLEGAL_STATE,
                                  stream.SetUpNoChecks(module.get(), kDefaultBufferSizeFrames))
                            << "port config ID " << stream.getPortId() << ", maxOpenStreamCount is "
                            << maxStreamCount;
                }
            }
        }
        if (!hasSingleRun) {
            GTEST_SKIP() << "Not enough ports to test max open stream count";
        }
    }

    void OpenTwiceSamePortConfig() {
        const auto portConfig = moduleConfig->getSingleConfigForMixPort(IOTraits<Stream>::is_input);
        if (!portConfig.has_value()) {
            GTEST_SKIP() << "No mix port for attached devices";
        }
        EXPECT_NO_FATAL_FAILURE(OpenTwiceSamePortConfigImpl(portConfig.value()));
    }

    void ResetPortConfigWithOpenStream() {
        const auto portConfig = moduleConfig->getSingleConfigForMixPort(IOTraits<Stream>::is_input);
        if (!portConfig.has_value()) {
            GTEST_SKIP() << "No mix port for attached devices";
        }
        WithStream<Stream> stream(portConfig.value());
        ASSERT_NO_FATAL_FAILURE(stream.SetUp(module.get(), kDefaultBufferSizeFrames));
        EXPECT_STATUS(EX_ILLEGAL_STATE, module->resetAudioPortConfig(stream.getPortId()))
                << "port config ID " << stream.getPortId();
    }

    void SendInvalidCommand() {
        const auto portConfig = moduleConfig->getSingleConfigForMixPort(IOTraits<Stream>::is_input);
        if (!portConfig.has_value()) {
            GTEST_SKIP() << "No mix port for attached devices";
        }
        EXPECT_NO_FATAL_FAILURE(SendInvalidCommandImpl(portConfig.value()));
    }

    void OpenTwiceSamePortConfigImpl(const AudioPortConfig& portConfig) {
        WithStream<Stream> stream1(portConfig);
        ASSERT_NO_FATAL_FAILURE(stream1.SetUp(module.get(), kDefaultBufferSizeFrames));
        WithStream<Stream> stream2;
        EXPECT_STATUS(EX_ILLEGAL_STATE, stream2.SetUpNoChecks(module.get(), stream1.getPortConfig(),
                                                              kDefaultBufferSizeFrames))
                << "when opening a stream twice for the same port config ID "
                << stream1.getPortId();
    }

    void SendInvalidCommandImpl(const AudioPortConfig& portConfig) {
        std::vector<StreamDescriptor::Command> commands(6);
        commands[0].code = StreamDescriptor::CommandCode(-1);
        commands[1].code = StreamDescriptor::CommandCode(
                static_cast<int32_t>(StreamDescriptor::CommandCode::START) - 1);
        commands[2].code = StreamDescriptor::CommandCode(std::numeric_limits<int32_t>::min());
        commands[3].code = StreamDescriptor::CommandCode(std::numeric_limits<int32_t>::max());
        // TODO: For proper testing of input streams, need to put the stream into
        // a state which accepts BURST commands.
        commands[4].code = StreamDescriptor::CommandCode::BURST;
        commands[4].fmqByteCount = -1;
        commands[5].code = StreamDescriptor::CommandCode::BURST;
        commands[5].fmqByteCount = std::numeric_limits<int32_t>::min();
        WithStream<Stream> stream(portConfig);
        ASSERT_NO_FATAL_FAILURE(stream.SetUp(module.get(), kDefaultBufferSizeFrames));
        StreamLogicDriverInvalidCommand driver(commands);
        typename IOTraits<Stream>::Worker worker(*stream.getContext(), &driver);
        ASSERT_TRUE(worker.start());
        worker.join();
        EXPECT_EQ("", driver.getUnexpectedStatuses());
    }
};
using AudioStreamIn = AudioStream<IStreamIn>;
using AudioStreamOut = AudioStream<IStreamOut>;

#define TEST_IN_AND_OUT_STREAM(method_name)                                        \
    TEST_P(AudioStreamIn, method_name) { ASSERT_NO_FATAL_FAILURE(method_name()); } \
    TEST_P(AudioStreamOut, method_name) { ASSERT_NO_FATAL_FAILURE(method_name()); }

TEST_IN_AND_OUT_STREAM(CloseTwice);
TEST_IN_AND_OUT_STREAM(OpenAllConfigs);
TEST_IN_AND_OUT_STREAM(OpenInvalidBufferSize);
TEST_IN_AND_OUT_STREAM(OpenInvalidDirection);
TEST_IN_AND_OUT_STREAM(OpenOverMaxCount);
TEST_IN_AND_OUT_STREAM(OpenTwiceSamePortConfig);
TEST_IN_AND_OUT_STREAM(ResetPortConfigWithOpenStream);
TEST_IN_AND_OUT_STREAM(SendInvalidCommand);

TEST_P(AudioStreamOut, OpenTwicePrimary) {
    const auto mixPorts = moduleConfig->getMixPorts(false);
    auto primaryPortIt = std::find_if(mixPorts.begin(), mixPorts.end(), [](const AudioPort& port) {
        return port.flags.getTag() == AudioIoFlags::Tag::output &&
               isBitPositionFlagSet(port.flags.get<AudioIoFlags::Tag::output>(),
                                    AudioOutputFlags::PRIMARY);
    });
    if (primaryPortIt == mixPorts.end()) {
        GTEST_SKIP() << "No primary mix port";
    }
    if (moduleConfig->getAttachedSinkDevicesPortsForMixPort(*primaryPortIt).empty()) {
        GTEST_SKIP() << "Primary mix port can not be routed to any of attached devices";
    }
    const auto portConfig = moduleConfig->getSingleConfigForMixPort(false, *primaryPortIt);
    ASSERT_TRUE(portConfig.has_value()) << "No profiles specified for the primary mix port";
    EXPECT_NO_FATAL_FAILURE(OpenTwiceSamePortConfigImpl(portConfig.value()));
}

TEST_P(AudioStreamOut, RequireOffloadInfo) {
    const auto offloadMixPorts =
            moduleConfig->getOffloadMixPorts(true /*attachedOnly*/, true /*singlePort*/);
    if (offloadMixPorts.empty()) {
        GTEST_SKIP()
                << "No mix port for compressed offload that could be routed to attached devices";
    }
    const auto portConfig =
            moduleConfig->getSingleConfigForMixPort(false, *offloadMixPorts.begin());
    ASSERT_TRUE(portConfig.has_value())
            << "No profiles specified for the compressed offload mix port";
    StreamDescriptor descriptor;
    std::shared_ptr<IStreamOut> ignored;
    aidl::android::hardware::audio::core::IModule::OpenOutputStreamArguments args;
    args.portConfigId = portConfig.value().id;
    args.sourceMetadata = GenerateSourceMetadata(portConfig.value());
    args.bufferSizeFrames = kDefaultBufferSizeFrames;
    aidl::android::hardware::audio::core::IModule::OpenOutputStreamReturn ret;
    EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->openOutputStream(args, &ret))
            << "when no offload info is provided for a compressed offload mix port";
}

using CommandAndState = std::pair<StreamDescriptor::CommandCode, StreamDescriptor::State>;

class StreamLogicDefaultDriver : public StreamLogicDriver {
  public:
    explicit StreamLogicDefaultDriver(const std::vector<CommandAndState>& commands)
        : mCommands(commands) {}

    // The three methods below is intended to be called after the worker
    // thread has joined, thus no extra synchronization is needed.
    bool hasObservablePositionIncrease() const { return mObservablePositionIncrease; }
    bool hasRetrogradeObservablePosition() const { return mRetrogradeObservablePosition; }
    std::string getUnexpectedStateTransition() const { return mUnexpectedTransition; }

    bool done() override { return mNextCommand >= mCommands.size(); }
    StreamDescriptor::Command getNextCommand(int maxDataSize, int* actualSize) override {
        StreamDescriptor::Command command{};
        command.code = mCommands[mNextCommand++].first;
        const int dataSize = command.code == StreamDescriptor::CommandCode::BURST ? maxDataSize : 0;
        command.fmqByteCount = dataSize;
        if (actualSize != nullptr) {
            // In the output scenario, reduce slightly the fmqByteCount to verify
            // that the HAL module always consumes all data from the MQ.
            if (command.fmqByteCount > 1) command.fmqByteCount--;
            *actualSize = dataSize;
        }
        return command;
    }
    bool interceptRawReply(const StreamDescriptor::Reply&) override { return false; }
    bool processValidReply(const StreamDescriptor::Reply& reply) override {
        if (mPreviousFrames.has_value()) {
            if (reply.observable.frames > mPreviousFrames.value()) {
                mObservablePositionIncrease = true;
            } else if (reply.observable.frames < mPreviousFrames.value()) {
                mRetrogradeObservablePosition = true;
            }
        }
        mPreviousFrames = reply.observable.frames;

        const auto& lastCommandState = mCommands[mNextCommand - 1];
        if (lastCommandState.second != reply.state) {
            std::string s = std::string("Unexpected transition from the state ")
                                    .append(mPreviousState)
                                    .append(" to ")
                                    .append(toString(reply.state))
                                    .append(" caused by the command ")
                                    .append(toString(lastCommandState.first));
            LOG(ERROR) << __func__ << ": " << s;
            mUnexpectedTransition = std::move(s);
            return false;
        }
        return true;
    }

  protected:
    const std::vector<CommandAndState>& mCommands;
    size_t mNextCommand = 0;
    std::optional<int64_t> mPreviousFrames;
    std::string mPreviousState = "<initial state>";
    bool mObservablePositionIncrease = false;
    bool mRetrogradeObservablePosition = false;
    std::string mUnexpectedTransition;
};

using NamedCommandSequence = std::pair<std::string, std::vector<CommandAndState>>;
enum { PARAM_MODULE_NAME, PARAM_CMD_SEQ, PARAM_SETUP_SEQ };
using StreamIoTestParameters =
        std::tuple<std::string /*moduleName*/, NamedCommandSequence, bool /*useSetupSequence2*/>;
template <typename Stream>
class AudioStreamIo : public AudioCoreModuleBase,
                      public testing::TestWithParam<StreamIoTestParameters> {
  public:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(SetUpImpl(std::get<PARAM_MODULE_NAME>(GetParam())));
        ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    }

    void Run() {
        const auto allPortConfigs =
                moduleConfig->getPortConfigsForMixPorts(IOTraits<Stream>::is_input);
        if (allPortConfigs.empty()) {
            GTEST_SKIP() << "No mix ports have attached devices";
        }
        for (const auto& portConfig : allPortConfigs) {
            SCOPED_TRACE(portConfig.toString());
            const auto& commandsAndStates = std::get<PARAM_CMD_SEQ>(GetParam()).second;
            if (!std::get<PARAM_SETUP_SEQ>(GetParam())) {
                ASSERT_NO_FATAL_FAILURE(RunStreamIoCommandsImplSeq1(portConfig, commandsAndStates));
            } else {
                ASSERT_NO_FATAL_FAILURE(RunStreamIoCommandsImplSeq2(portConfig, commandsAndStates));
            }
        }
    }

    bool ValidateObservablePosition(const AudioPortConfig& /*portConfig*/) {
        // May return false based on the portConfig, e.g. for telephony ports.
        return true;
    }

    // Set up a patch first, then open a stream.
    void RunStreamIoCommandsImplSeq1(const AudioPortConfig& portConfig,
                                     const std::vector<CommandAndState>& commandsAndStates) {
        auto devicePorts = moduleConfig->getAttachedDevicesPortsForMixPort(
                IOTraits<Stream>::is_input, portConfig);
        ASSERT_FALSE(devicePorts.empty());
        auto devicePortConfig = moduleConfig->getSingleConfigForDevicePort(devicePorts[0]);
        WithAudioPatch patch(IOTraits<Stream>::is_input, portConfig, devicePortConfig);
        ASSERT_NO_FATAL_FAILURE(patch.SetUp(module.get()));

        WithStream<Stream> stream(patch.getPortConfig(IOTraits<Stream>::is_input));
        ASSERT_NO_FATAL_FAILURE(stream.SetUp(module.get(), kDefaultBufferSizeFrames));
        StreamLogicDefaultDriver driver(commandsAndStates);
        typename IOTraits<Stream>::Worker worker(*stream.getContext(), &driver);

        ASSERT_TRUE(worker.start());
        worker.join();
        EXPECT_FALSE(worker.hasError()) << worker.getError();
        EXPECT_EQ("", driver.getUnexpectedStateTransition());
        if (ValidateObservablePosition(portConfig)) {
            EXPECT_TRUE(driver.hasObservablePositionIncrease());
            EXPECT_FALSE(driver.hasRetrogradeObservablePosition());
        }
    }

    // Open a stream, then set up a patch for it.
    void RunStreamIoCommandsImplSeq2(const AudioPortConfig& portConfig,
                                     const std::vector<CommandAndState>& commandsAndStates) {
        WithStream<Stream> stream(portConfig);
        ASSERT_NO_FATAL_FAILURE(stream.SetUp(module.get(), kDefaultBufferSizeFrames));
        StreamLogicDefaultDriver driver(commandsAndStates);
        typename IOTraits<Stream>::Worker worker(*stream.getContext(), &driver);

        auto devicePorts = moduleConfig->getAttachedDevicesPortsForMixPort(
                IOTraits<Stream>::is_input, portConfig);
        ASSERT_FALSE(devicePorts.empty());
        auto devicePortConfig = moduleConfig->getSingleConfigForDevicePort(devicePorts[0]);
        WithAudioPatch patch(IOTraits<Stream>::is_input, stream.getPortConfig(), devicePortConfig);
        ASSERT_NO_FATAL_FAILURE(patch.SetUp(module.get()));

        ASSERT_TRUE(worker.start());
        worker.join();
        EXPECT_FALSE(worker.hasError()) << worker.getError();
        EXPECT_EQ("", driver.getUnexpectedStateTransition());
        if (ValidateObservablePosition(portConfig)) {
            EXPECT_TRUE(driver.hasObservablePositionIncrease());
            EXPECT_FALSE(driver.hasRetrogradeObservablePosition());
        }
    }
};
using AudioStreamIoIn = AudioStreamIo<IStreamIn>;
using AudioStreamIoOut = AudioStreamIo<IStreamOut>;

#define TEST_IN_AND_OUT_STREAM_IO(method_name)                                       \
    TEST_P(AudioStreamIoIn, method_name) { ASSERT_NO_FATAL_FAILURE(method_name()); } \
    TEST_P(AudioStreamIoOut, method_name) { ASSERT_NO_FATAL_FAILURE(method_name()); }

TEST_IN_AND_OUT_STREAM_IO(Run);

// Tests specific to audio patches. The fixure class is named 'AudioModulePatch'
// to avoid clashing with 'AudioPatch' class.
class AudioModulePatch : public AudioCoreModule {
  public:
    static std::string direction(bool isInput, bool capitalize) {
        return isInput ? (capitalize ? "Input" : "input") : (capitalize ? "Output" : "output");
    }

    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(AudioCoreModule::SetUp());
        ASSERT_NO_FATAL_FAILURE(SetUpModuleConfig());
    }

    void SetInvalidPatchHelper(int32_t expectedException, const std::vector<int32_t>& sources,
                               const std::vector<int32_t>& sinks) {
        AudioPatch patch;
        patch.sourcePortConfigIds = sources;
        patch.sinkPortConfigIds = sinks;
        ASSERT_STATUS(expectedException, module->setAudioPatch(patch, &patch))
                << "patch source ids: " << android::internal::ToString(sources)
                << "; sink ids: " << android::internal::ToString(sinks);
    }

    void ResetPortConfigUsedByPatch(bool isInput) {
        auto srcSinkGroups = moduleConfig->getRoutableSrcSinkGroups(isInput);
        if (srcSinkGroups.empty()) {
            GTEST_SKIP() << "No routes to any attached " << direction(isInput, false) << " devices";
        }
        auto srcSinkGroup = *srcSinkGroups.begin();
        auto srcSink = *srcSinkGroup.second.begin();
        WithAudioPatch patch(srcSink.first, srcSink.second);
        ASSERT_NO_FATAL_FAILURE(patch.SetUp(module.get()));
        std::vector<int32_t> sourceAndSinkPortConfigIds(patch.get().sourcePortConfigIds);
        sourceAndSinkPortConfigIds.insert(sourceAndSinkPortConfigIds.end(),
                                          patch.get().sinkPortConfigIds.begin(),
                                          patch.get().sinkPortConfigIds.end());
        for (const auto portConfigId : sourceAndSinkPortConfigIds) {
            EXPECT_STATUS(EX_ILLEGAL_STATE, module->resetAudioPortConfig(portConfigId))
                    << "port config ID " << portConfigId;
        }
    }

    void SetInvalidPatch(bool isInput) {
        auto srcSinkPair = moduleConfig->getRoutableSrcSinkPair(isInput);
        if (!srcSinkPair.has_value()) {
            GTEST_SKIP() << "No routes to any attached " << direction(isInput, false) << " devices";
        }
        WithAudioPortConfig srcPortConfig(srcSinkPair.value().first);
        ASSERT_NO_FATAL_FAILURE(srcPortConfig.SetUp(module.get()));
        WithAudioPortConfig sinkPortConfig(srcSinkPair.value().second);
        ASSERT_NO_FATAL_FAILURE(sinkPortConfig.SetUp(module.get()));
        {  // Check that the pair can actually be used for setting up a patch.
            WithAudioPatch patch(srcPortConfig.get(), sinkPortConfig.get());
            ASSERT_NO_FATAL_FAILURE(patch.SetUp(module.get()));
        }
        EXPECT_NO_FATAL_FAILURE(
                SetInvalidPatchHelper(EX_ILLEGAL_ARGUMENT, {}, {sinkPortConfig.getId()}));
        EXPECT_NO_FATAL_FAILURE(SetInvalidPatchHelper(
                EX_ILLEGAL_ARGUMENT, {srcPortConfig.getId(), srcPortConfig.getId()},
                {sinkPortConfig.getId()}));
        EXPECT_NO_FATAL_FAILURE(
                SetInvalidPatchHelper(EX_ILLEGAL_ARGUMENT, {srcPortConfig.getId()}, {}));
        EXPECT_NO_FATAL_FAILURE(
                SetInvalidPatchHelper(EX_ILLEGAL_ARGUMENT, {srcPortConfig.getId()},
                                      {sinkPortConfig.getId(), sinkPortConfig.getId()}));

        std::set<int32_t> portConfigIds;
        ASSERT_NO_FATAL_FAILURE(GetAllPortConfigIds(&portConfigIds));
        for (const auto portConfigId : GetNonExistentIds(portConfigIds)) {
            EXPECT_NO_FATAL_FAILURE(SetInvalidPatchHelper(EX_ILLEGAL_ARGUMENT, {portConfigId},
                                                          {sinkPortConfig.getId()}));
            EXPECT_NO_FATAL_FAILURE(SetInvalidPatchHelper(EX_ILLEGAL_ARGUMENT,
                                                          {srcPortConfig.getId()}, {portConfigId}));
        }
    }

    void SetNonRoutablePatch(bool isInput) {
        auto srcSinkPair = moduleConfig->getNonRoutableSrcSinkPair(isInput);
        if (!srcSinkPair.has_value()) {
            GTEST_SKIP() << "All possible source/sink pairs are routable";
        }
        WithAudioPatch patch(srcSinkPair.value().first, srcSinkPair.value().second);
        ASSERT_NO_FATAL_FAILURE(patch.SetUpPortConfigs(module.get()));
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, patch.SetUpNoChecks(module.get()))
                << "when setting up a patch from " << srcSinkPair.value().first.toString() << " to "
                << srcSinkPair.value().second.toString() << " that does not have a route";
    }

    void SetPatch(bool isInput) {
        auto srcSinkGroups = moduleConfig->getRoutableSrcSinkGroups(isInput);
        if (srcSinkGroups.empty()) {
            GTEST_SKIP() << "No routes to any attached " << direction(isInput, false) << " devices";
        }
        for (const auto& srcSinkGroup : srcSinkGroups) {
            const auto& route = srcSinkGroup.first;
            std::vector<std::unique_ptr<WithAudioPatch>> patches;
            for (const auto& srcSink : srcSinkGroup.second) {
                if (!route.isExclusive) {
                    patches.push_back(
                            std::make_unique<WithAudioPatch>(srcSink.first, srcSink.second));
                    EXPECT_NO_FATAL_FAILURE(patches[patches.size() - 1]->SetUp(module.get()));
                } else {
                    WithAudioPatch patch(srcSink.first, srcSink.second);
                    EXPECT_NO_FATAL_FAILURE(patch.SetUp(module.get()));
                }
            }
        }
    }

    void UpdatePatch(bool isInput) {
        auto srcSinkGroups = moduleConfig->getRoutableSrcSinkGroups(isInput);
        if (srcSinkGroups.empty()) {
            GTEST_SKIP() << "No routes to any attached " << direction(isInput, false) << " devices";
        }
        for (const auto& srcSinkGroup : srcSinkGroups) {
            for (const auto& srcSink : srcSinkGroup.second) {
                WithAudioPatch patch(srcSink.first, srcSink.second);
                ASSERT_NO_FATAL_FAILURE(patch.SetUp(module.get()));
                AudioPatch ignored;
                EXPECT_NO_FATAL_FAILURE(module->setAudioPatch(patch.get(), &ignored));
            }
        }
    }

    void UpdateInvalidPatchId(bool isInput) {
        auto srcSinkGroups = moduleConfig->getRoutableSrcSinkGroups(isInput);
        if (srcSinkGroups.empty()) {
            GTEST_SKIP() << "No routes to any attached " << direction(isInput, false) << " devices";
        }
        // First, set up a patch to ensure that its settings are accepted.
        auto srcSinkGroup = *srcSinkGroups.begin();
        auto srcSink = *srcSinkGroup.second.begin();
        WithAudioPatch patch(srcSink.first, srcSink.second);
        ASSERT_NO_FATAL_FAILURE(patch.SetUp(module.get()));
        // Then use the same patch setting, except for having an invalid ID.
        std::set<int32_t> patchIds;
        ASSERT_NO_FATAL_FAILURE(GetAllPatchIds(&patchIds));
        for (const auto patchId : GetNonExistentIds(patchIds)) {
            AudioPatch patchWithNonExistendId = patch.get();
            patchWithNonExistendId.id = patchId;
            EXPECT_STATUS(EX_ILLEGAL_ARGUMENT,
                          module->setAudioPatch(patchWithNonExistendId, &patchWithNonExistendId))
                    << "patch ID " << patchId;
        }
    }
};

// Not all tests require both directions, so parametrization would require
// more abstractions.
#define TEST_PATCH_BOTH_DIRECTIONS(method_name)                                                  \
    TEST_P(AudioModulePatch, method_name##Input) { ASSERT_NO_FATAL_FAILURE(method_name(true)); } \
    TEST_P(AudioModulePatch, method_name##Output) { ASSERT_NO_FATAL_FAILURE(method_name(false)); }

TEST_PATCH_BOTH_DIRECTIONS(ResetPortConfigUsedByPatch);
TEST_PATCH_BOTH_DIRECTIONS(SetInvalidPatch);
TEST_PATCH_BOTH_DIRECTIONS(SetNonRoutablePatch);
TEST_PATCH_BOTH_DIRECTIONS(SetPatch);
TEST_PATCH_BOTH_DIRECTIONS(UpdateInvalidPatchId);
TEST_PATCH_BOTH_DIRECTIONS(UpdatePatch);

TEST_P(AudioModulePatch, ResetInvalidPatchId) {
    std::set<int32_t> patchIds;
    ASSERT_NO_FATAL_FAILURE(GetAllPatchIds(&patchIds));
    for (const auto patchId : GetNonExistentIds(patchIds)) {
        EXPECT_STATUS(EX_ILLEGAL_ARGUMENT, module->resetAudioPatch(patchId))
                << "patch ID " << patchId;
    }
}

INSTANTIATE_TEST_SUITE_P(AudioCoreModuleTest, AudioCoreModule,
                         testing::ValuesIn(android::getAidlHalInstanceNames(IModule::descriptor)),
                         android::PrintInstanceNameToString);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioCoreModule);
INSTANTIATE_TEST_SUITE_P(AudioStreamInTest, AudioStreamIn,
                         testing::ValuesIn(android::getAidlHalInstanceNames(IModule::descriptor)),
                         android::PrintInstanceNameToString);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioStreamIn);
INSTANTIATE_TEST_SUITE_P(AudioStreamOutTest, AudioStreamOut,
                         testing::ValuesIn(android::getAidlHalInstanceNames(IModule::descriptor)),
                         android::PrintInstanceNameToString);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioStreamOut);

static const NamedCommandSequence kReadOrWriteSeq = std::make_pair(
        std::string("ReadOrWrite"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE)});
static const NamedCommandSequence kDrainInSeq = std::make_pair(
        std::string("Drain"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::DRAIN,
                               StreamDescriptor::State::DRAINING),
                std::make_pair(StreamDescriptor::CommandCode::START,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::DRAIN,
                               StreamDescriptor::State::DRAINING),
                // TODO: This will need to be changed once DRAIN starts taking time.
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::STANDBY)});
static const NamedCommandSequence kDrainOutSeq = std::make_pair(
        std::string("Drain"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                // TODO: This will need to be changed once DRAIN starts taking time.
                std::make_pair(StreamDescriptor::CommandCode::DRAIN,
                               StreamDescriptor::State::IDLE)});
// TODO: This will need to be changed once DRAIN starts taking time so we can pause it.
static const NamedCommandSequence kDrainPauseOutSeq = std::make_pair(
        std::string("DrainPause"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::DRAIN,
                               StreamDescriptor::State::IDLE)});
static const NamedCommandSequence kStandbySeq = std::make_pair(
        std::string("Standby"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::STANDBY,
                               StreamDescriptor::State::STANDBY),
                // Perform a read or write in order to advance observable position
                // (this is verified by tests).
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE)});
static const NamedCommandSequence kPauseInSeq = std::make_pair(
        std::string("Pause"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::PAUSE,
                               StreamDescriptor::State::PAUSED),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::PAUSE,
                               StreamDescriptor::State::PAUSED),
                std::make_pair(StreamDescriptor::CommandCode::FLUSH,
                               StreamDescriptor::State::STANDBY)});
static const NamedCommandSequence kPauseOutSeq = std::make_pair(
        std::string("Pause"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::PAUSE,
                               StreamDescriptor::State::PAUSED),
                std::make_pair(StreamDescriptor::CommandCode::START,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::PAUSE,
                               StreamDescriptor::State::PAUSED),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::PAUSED),
                std::make_pair(StreamDescriptor::CommandCode::START,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::PAUSE,
                               StreamDescriptor::State::PAUSED)});
static const NamedCommandSequence kFlushInSeq = std::make_pair(
        std::string("Flush"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::PAUSE,
                               StreamDescriptor::State::PAUSED),
                std::make_pair(StreamDescriptor::CommandCode::FLUSH,
                               StreamDescriptor::State::STANDBY)});
static const NamedCommandSequence kFlushOutSeq = std::make_pair(
        std::string("Flush"),
        std::vector<CommandAndState>{
                std::make_pair(StreamDescriptor::CommandCode::START, StreamDescriptor::State::IDLE),
                std::make_pair(StreamDescriptor::CommandCode::BURST,
                               StreamDescriptor::State::ACTIVE),
                std::make_pair(StreamDescriptor::CommandCode::PAUSE,
                               StreamDescriptor::State::PAUSED),
                std::make_pair(StreamDescriptor::CommandCode::FLUSH,
                               StreamDescriptor::State::IDLE)});
std::string GetStreamIoTestName(const testing::TestParamInfo<StreamIoTestParameters>& info) {
    return android::PrintInstanceNameToString(
                   testing::TestParamInfo<std::string>{std::get<PARAM_MODULE_NAME>(info.param),
                                                       info.index})
            .append("_")
            .append(std::get<PARAM_CMD_SEQ>(info.param).first)
            .append("_SetupSeq")
            .append(std::get<PARAM_SETUP_SEQ>(info.param) ? "2" : "1");
}
INSTANTIATE_TEST_SUITE_P(
        AudioStreamIoInTest, AudioStreamIoIn,
        testing::Combine(testing::ValuesIn(android::getAidlHalInstanceNames(IModule::descriptor)),
                         testing::Values(kReadOrWriteSeq, kDrainInSeq, kStandbySeq, kPauseInSeq,
                                         kFlushInSeq),
                         testing::Values(false, true)),
        GetStreamIoTestName);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioStreamIoIn);
INSTANTIATE_TEST_SUITE_P(
        AudioStreamIoOutTest, AudioStreamIoOut,
        testing::Combine(testing::ValuesIn(android::getAidlHalInstanceNames(IModule::descriptor)),
                         testing::Values(kReadOrWriteSeq, kDrainOutSeq, kDrainPauseOutSeq,
                                         kStandbySeq, kPauseOutSeq, kFlushOutSeq),
                         testing::Values(false, true)),
        GetStreamIoTestName);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioStreamIoOut);

INSTANTIATE_TEST_SUITE_P(AudioPatchTest, AudioModulePatch,
                         testing::ValuesIn(android::getAidlHalInstanceNames(IModule::descriptor)),
                         android::PrintInstanceNameToString);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioModulePatch);

class TestExecutionTracer : public ::testing::EmptyTestEventListener {
  public:
    void OnTestStart(const ::testing::TestInfo& test_info) override {
        TraceTestState("Started", test_info);
    }

    void OnTestEnd(const ::testing::TestInfo& test_info) override {
        TraceTestState("Completed", test_info);
    }

  private:
    static void TraceTestState(const std::string& state, const ::testing::TestInfo& test_info) {
        LOG(INFO) << state << " " << test_info.test_suite_name() << "::" << test_info.name();
    }
};

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestExecutionTracer());
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();
    return RUN_ALL_TESTS();
}
