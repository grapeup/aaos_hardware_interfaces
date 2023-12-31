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

package android.hardware.graphics.common;

/**
 * Supported HDR formats. Must be kept in sync with equivalents in Display.java.
 * @hide
 */
@VintfStability
@Backing(type="int")
enum Hdr {
    /**
     * Device supports Dolby Vision HDR
     */
    DOLBY_VISION = 1,
    /**
     * Device supports HDR10
     */
    HDR10 = 2,
    /**
     * Device supports hybrid log-gamma HDR
     */
    HLG = 3,
    /**
     * Device supports HDR10+
     */
    HDR10_PLUS = 4,
    /**
     * If present, indicates that device supports Dolby Vision only up to 4k30hz graphics mode
     */
    DOLBY_VISION_4K30 = 5,
}
