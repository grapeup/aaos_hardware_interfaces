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

package android.hardware.media.c2;

import android.hardware.media.c2.SettingResult;
import android.hardware.media.c2.Status;
import android.hardware.media.c2.WorkBundle;

/**
 * Callback interface for handling notifications from @ref IComponent.
 */
@VintfStability
oneway interface IComponentListener {
    /**
     * Identifying information for an input buffer previously queued to the
     * component via IComponent::queue().
     */
    @VintfStability
    parcelable InputBuffer {
        /**
         * This value comes from `Work::input.ordinal.frameIndex` in a `Work`
         * object that was previously queued.
         */
        long frameIndex;
        /**
         * This value is an index into `Work::input.buffers` (which is an array)
         * in a `Work` object that was previously queued.
         */
        int arrayIndex;
    }
    /**
     * Information about rendering of a frame to a `Surface`.
     */
    @VintfStability
    parcelable RenderedFrame {
        /**
         * Id of the `BufferQueue` containing the rendered buffer.
         *
         * This value must have been obtained by an earlier call to
         * IGraphicBufferProducer::getUniqueId().
         */
        long bufferQueueId;
        /**
         * Id of the slot of the rendered buffer.
         *
         * This value must have been obtained by an earlier call to
         * IGraphicBufferProducer::dequeueBuffer() or
         * IGraphicBufferProducer::attachBuffer().
         */
        int slotId;
        /**
         * Timestamp the rendering happened.
         *
         * The reference point for the timestamp is determined by the
         * `BufferQueue` that performed the rendering.
         */
        long timestampNs;
    }
    /**
     * Notify the listener of an error.
     *
     * @param status Error type. @p status may be `OK`, which means that an
     *     error has occurred, but the error type does not fit into the type
     *     `Status`. In this case, additional information is provided by
     *     @p errorCode.
     * @param errorCode Additional error information. The framework may not
     *     recognize the meaning of this value.
     */
    void onError(in Status status, in int errorCode);

    /**
     * Notify the listener that frames have been rendered.
     *
     * @param renderedFrames List of @ref RenderedFrame objects.
     */
    void onFramesRendered(in RenderedFrame[] renderedFrames);

    /**
     * Notify the listener that some input buffers are no longer needed by the
     * component, and hence can be released or reused by the client.
     *
     * Input buffers that are contained in a `Work` object returned by an
     * earlier onWorkDone() call are assumed released, so they must not appear
     * in any onInputBuffersReleased() calls. That means
     * onInputBuffersReleased() must only report input buffers that are released
     * before the output in the same `Work` item is produced. However, it is
     * possible for an input buffer to be returned by onWorkDone() after it has
     * been reported by onInputBuffersReleased().
     *
     * @note onWorkDone() and onInputBuffersReleased() both notify the client
     * that input buffers are no longer needed. However, in order to minimize
     * IPC calls, onInputBuffersReleased() should be called only when
     * onWorkDone() cannot be called, e.g., the component needs more input
     * before an output can be produced.
     *
     * @param inputBuffers List of `InputBuffer` objects, identifying input
     * buffers that are no longer needed by the component.
     */
    void onInputBuffersReleased(in InputBuffer[] inputBuffers);

    /**
     * Notify the listener that the component is tripped.
     *
     * @param settingResults List of failures.
     */
    void onTripped(in SettingResult[] settingResults);

    /**
     * Notify the listener that some `Work` items have been completed.
     *
     * All the input buffers in the returned `Work` objects must not be used by
     * the component after onWorkDone() is called.
     *
     * @param workBundle List of completed `Work` objects.
     */
    void onWorkDone(in WorkBundle workBundle);
}
