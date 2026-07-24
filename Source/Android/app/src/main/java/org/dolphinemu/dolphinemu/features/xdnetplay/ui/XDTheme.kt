// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * The Pokémon Box "Sea" design language, carried over from PokéBridge:
 * blue-family palette, floating navy panels with title chips, and a single
 * yellow accent reserved for selection, headings, warnings, and shiny.
 */
object XDColors {
    val BgTop = Color(0xFF4488CC)
    val BgBottom = Color(0xFF1E4D7B)
    val Panel = Color(0xEB183454)
    val PanelLight = Color(0xFF6FA3D1)
    val Accent = Color(0xFFF4D858)
    val TextPrimary = Color(0xFFFFFFFF)
    val TextDim = Color(0xFFC8DDF5)
    val TextAccent = Color(0xFFFFE872)
    val Shiny = Color(0xFFFFD800)
    val ChipText = Color(0xFF183454)

    private val pastels = listOf(
        Color(0xFF6FA3D1), Color(0xFFE56B6F), Color(0xFF88BB66), Color(0xFF77CCEE),
        Color(0xFFFFD86B), Color(0xFFBB99DD), Color(0xFFCCBB99), Color(0xFFAA8855)
    )

    fun pastelFor(species: Int): Color = pastels[species.mod(pastels.size)]
}

@Composable
fun SeaBackground(content: @Composable () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(XDColors.BgTop, XDColors.BgBottom)))
    ) {
        content()
    }
}

@Composable
fun SeaPanel(
    title: String?,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit
) {
    Box(modifier = modifier.padding(top = if (title != null) 10.dp else 0.dp)) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(XDColors.Panel, RoundedCornerShape(12.dp))
                .border(1.dp, XDColors.PanelLight, RoundedCornerShape(12.dp))
                .padding(start = 14.dp, end = 14.dp, top = if (title != null) 18.dp else 12.dp, bottom = 12.dp),
            content = content
        )
        if (title != null) {
            Text(
                text = title.uppercase(),
                color = XDColors.ChipText,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .offset(x = 16.dp, y = (-10).dp)
                    .background(XDColors.PanelLight, RoundedCornerShape(8.dp))
                    .padding(horizontal = 10.dp, vertical = 3.dp)
            )
        }
    }
}
