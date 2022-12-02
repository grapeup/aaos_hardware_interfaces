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

package android.hardware.audio.effect;

import android.hardware.audio.effect.VendorExtension;

/**
 * Reverb specific definitions.
 *
 * All parameters defined in union Reverb must be gettable and settable. The capabilities defined in
 * Reverb.Capability can only acquired with IEffect.getDescriptor() and not settable.
 */
@VintfStability
union Reverb {
    /**
     * Effect parameter tag to identify the parameters for getParameter().
     */
    @VintfStability
    union Id {
        int vendorExtensionTag;
        Reverb.Tag commonTag;
    }

    /**
     * Vendor Reverb implementation definition for additional parameters.
     */
    VendorExtension vendor;

    /**
     * Capability supported by effect implementation.
     */
    @VintfStability
    parcelable Capability {
        VendorExtension extension;

        /**
         * Max decay time supported in millisecond.
         */
        int maxDecayTimeMs;
    }

    /**
     * Room level apply to the reverb effect in millibels.
     */
    int roomLevelMb;
    /**
     * Room HF level apply to the reverb effect in millibels.
     */
    int roomHfLevelMb;
    /**
     * Delay time apply to the reverb effect in milliseconds.
     */
    int decayTimeMs;
    /**
     * HF decay ratio in permilles.
     */
    int decayHfRatioPm;
    /**
     * Reverb level in millibels.
     */
    int levelMb;
    /**
     * Reverb delay in milliseconds.
     */
    int delayMs;
    /**
     * Diffusion in permilles.
     */
    int diffusionPm;
    /**
     * Density in permilles.
     */
    int densityPm;
    /**
     * Bypass reverb and copy input to output if set to true.
     */
    boolean bypass;
}
