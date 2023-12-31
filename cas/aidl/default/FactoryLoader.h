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

#include <android-base/strings.h>
#include <dirent.h>
#include <dlfcn.h>
#include <media/cas/CasAPI.h>
#include <utils/KeyedVector.h>
#include <utils/Mutex.h>
#include "SharedLibrary.h"

using namespace std;

namespace aidl {
namespace android {
namespace hardware {
namespace cas {

using namespace ::android;

template <class T>
class FactoryLoader {
  public:
    FactoryLoader(const char* name) : mFactory(NULL), mCreateFactoryFuncName(name) {}

    virtual ~FactoryLoader() { closeFactory(); }

    bool findFactoryForScheme(int32_t CA_system_id, shared_ptr<SharedLibrary>* library = NULL,
                              T** factory = NULL);

    bool enumeratePlugins(vector<AidlCasPluginDescriptor>* results);

  private:
    typedef T* (*CreateFactoryFunc)();

    Mutex mMapLock;
    T* mFactory;
    const char* mCreateFactoryFuncName;
    shared_ptr<SharedLibrary> mLibrary;
    KeyedVector<int32_t, String8> mCASystemIdToLibraryPathMap;
    KeyedVector<String8, shared_ptr<SharedLibrary>> mLibraryPathToOpenLibraryMap;

    bool loadFactoryForSchemeFromPath(const String8& path, int32_t CA_system_id,
                                      shared_ptr<SharedLibrary>* library, T** factory);

    bool queryPluginsFromPath(const String8& path, vector<AidlCasPluginDescriptor>* results);

    bool openFactory(const String8& path);
    void closeFactory();
};

template <class T>
bool FactoryLoader<T>::findFactoryForScheme(int32_t CA_system_id,
                                            shared_ptr<SharedLibrary>* library, T** factory) {
    if (library != NULL) {
        library->reset();
    }
    if (factory != NULL) {
        *factory = NULL;
    }

    Mutex::Autolock autoLock(mMapLock);

    // first check cache
    ssize_t index = mCASystemIdToLibraryPathMap.indexOfKey(CA_system_id);
    if (index >= 0) {
        return loadFactoryForSchemeFromPath(mCASystemIdToLibraryPathMap[index], CA_system_id,
                                            library, factory);
    }

    // no luck, have to search
#ifdef __LP64__
    String8 dirPath("/vendor/lib64/mediacas");
#else
    String8 dirPath("/vendor/lib/mediacas");
#endif
    DIR* pDir = opendir(dirPath.c_str());

    if (pDir == NULL) {
        ALOGE("Failed to open plugin directory %s", dirPath.c_str());
        return false;
    }

    struct dirent* pEntry;
    while ((pEntry = readdir(pDir))) {
        String8 pluginPath = dirPath + "/" + pEntry->d_name;
        if (base::EndsWith(pluginPath.c_str(), ".so")) {
            if (loadFactoryForSchemeFromPath(pluginPath, CA_system_id, library, factory)) {
                mCASystemIdToLibraryPathMap.add(CA_system_id, pluginPath);
                closedir(pDir);

                return true;
            }
        }
    }

    closedir(pDir);

    ALOGE("Failed to find plugin");
    return false;
}

template <class T>
bool FactoryLoader<T>::enumeratePlugins(vector<AidlCasPluginDescriptor>* results) {
    ALOGI("enumeratePlugins");

    results->clear();

#ifdef __LP64__
    String8 dirPath("/vendor/lib64/mediacas");
#else
    String8 dirPath("/vendor/lib/mediacas");
#endif
    DIR* pDir = opendir(dirPath.c_str());

    if (pDir == NULL) {
        ALOGE("Failed to open plugin directory %s", dirPath.c_str());
        return false;
    }

    Mutex::Autolock autoLock(mMapLock);

    struct dirent* pEntry;
    while ((pEntry = readdir(pDir))) {
        String8 pluginPath = dirPath + "/" + pEntry->d_name;
        if (base::EndsWith(pluginPath.c_str(), ".so")) {
            queryPluginsFromPath(pluginPath, results);
        }
    }
    closedir(pDir);
    return true;
}

template <class T>
bool FactoryLoader<T>::loadFactoryForSchemeFromPath(const String8& path, int32_t CA_system_id,
                                                    shared_ptr<SharedLibrary>* library,
                                                    T** factory) {
    closeFactory();

    if (!openFactory(path) || !mFactory->isSystemIdSupported(CA_system_id)) {
        closeFactory();
        return false;
    }

    if (library != NULL) {
        *library = mLibrary;
    }
    if (factory != NULL) {
        *factory = mFactory;
    }
    return true;
}

template <class T>
bool FactoryLoader<T>::queryPluginsFromPath(const String8& path,
                                            vector<AidlCasPluginDescriptor>* results) {
    closeFactory();

    vector<CasPluginDescriptor> descriptors;
    if (!openFactory(path) || mFactory->queryPlugins(&descriptors) != OK) {
        closeFactory();
        return false;
    }

    for (auto it = descriptors.begin(); it != descriptors.end(); it++) {
        results->push_back(
                AidlCasPluginDescriptor{.caSystemId = it->CA_system_id, .name = it->name.c_str()});
    }
    return true;
}

template <class T>
bool FactoryLoader<T>::openFactory(const String8& path) {
    // get strong pointer to open shared library
    ssize_t index = mLibraryPathToOpenLibraryMap.indexOfKey(path);
    if (index >= 0) {
        mLibrary = mLibraryPathToOpenLibraryMap[index];
    } else {
        index = mLibraryPathToOpenLibraryMap.add(path, NULL);
    }

    if (!mLibrary.get()) {
        mLibrary = ::ndk::SharedRefBase::make<SharedLibrary>(path);
        if (!*mLibrary) {
            return false;
        }

        mLibraryPathToOpenLibraryMap.replaceValueAt(index, mLibrary);
    }

    CreateFactoryFunc createFactory = (CreateFactoryFunc)mLibrary->lookup(mCreateFactoryFuncName);
    if (createFactory == NULL || (mFactory = createFactory()) == NULL) {
        return false;
    }
    return true;
}

template <class T>
void FactoryLoader<T>::closeFactory() {
    delete mFactory;
    mFactory = NULL;
    mLibrary.reset();
}

}  // namespace cas
}  // namespace hardware
}  // namespace android
}  // namespace aidl
