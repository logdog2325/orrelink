// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

import android.content.Context

/**
 * The one Android-aware entry point for [Gen4Data].
 *
 * It lives in its own file so Gen4Data.kt itself pulls in nothing from the
 * platform, which is what lets the whole save-format layer be compiled and
 * round-trip-tested on a plain JVM.
 */
fun loadGen4Data(context: Context): Gen4Data =
    context.assets.open(Gen4Data.ASSET_PATH).use { Gen4Data.load(it) }
