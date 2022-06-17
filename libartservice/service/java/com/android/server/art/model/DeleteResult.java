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

package com.android.server.art.model;

import android.annotation.SystemApi;

/** @hide */
@SystemApi(client = SystemApi.Client.SYSTEM_SERVER)
public class DeleteResult {
    private long mFreedBytes;

    /** @hide */
    public DeleteResult(long freedBytes) {
        mFreedBytes = freedBytes;
    }

    /** The amount of the disk space freed by the deletion, in bytes. */
    public long getFreedBytes() {
        return mFreedBytes;
    }
}
