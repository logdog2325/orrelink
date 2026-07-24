// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

import android.content.Context
import java.io.InputStream
import org.json.JSONArray
import org.json.JSONObject

/**
 * Static Gen 3 (Emerald) game data loaded from the bundled asset
 * "xdnetplay/gen3data.json".
 *
 * Schema (verified against the generated file):
 *   species:   { name: { id, natDex, baseStats [hp,atk,def,speed,spatk,spdef],
 *                        abilities [a1,a2], genderRatio, expGroup, types } }
 *   moves:     { name: id }
 *   items:     { name: id }
 *   abilities: { name: id }
 *   natures:   { name: { id, plus, minus } }   plus/minus: -1 = neutral,
 *              else stat index 0=atk 1=def 2=speed 3=spatk 4=spdef
 *   expTables: { group: { max, cumulative [101] } }  cumulative indexed by level
 *              (a plain 101-element array is also accepted for compatibility
 *              with the originally specified schema)
 *   charset:   { char: byte }  (pokeemerald charmap; note it maps '$' to 0xFF
 *              because '$' is the EOS marker in pokeemerald source strings —
 *              use Gen3Text for actual name encoding)
 *   typeNames: { typeId: name }  (optional)
 *
 * All name keys are normalized: lowercase with non-alphanumerics stripped.
 * The lookup helpers apply the same normalization to their inputs.
 */
class Gen3Data private constructor(
    val species: Map<String, Species>,
    val moves: Map<String, Int>,
    val items: Map<String, Int>,
    val abilities: Map<String, Int>,
    val natures: Map<String, Nature>,
    val expTables: Map<String, IntArray>,
    val charset: Map<Char, Int>,
    val typeNames: Map<Int, String>
) {
    data class Species(
        val name: String,
        val id: Int,          // internal (Hoenn) species id used in save data
        val natDex: Int,      // National Dex number
        val baseStats: IntArray,  // hp, atk, def, speed, spatk, spdef
        val abilities: IntArray,  // slot 1, slot 2 (0 = none)
        val genderRatio: Int, // 0 = always male, 254 = always female,
                              // 255 = genderless, else female iff (PID & 0xFF) < ratio
        val expGroup: String,
        val types: IntArray
    )

    data class Nature(
        val name: String,
        val id: Int,
        val plus: Int,   // -1 = neutral, else 0=atk 1=def 2=speed 3=spatk 4=spdef
        val minus: Int
    )

    // -- lookup helpers (input names are normalized) -------------------------

    fun species(name: String): Species? = species[normalizeName(name)]

    fun moveId(name: String): Int? = moves[normalizeName(name)]

    fun itemId(name: String): Int? = items[normalizeName(name)]

    fun abilityId(name: String): Int? = abilities[normalizeName(name)]

    fun nature(name: String): Nature? = natures[normalizeName(name)]

    /**
     * Total experience at [level] (1..100) for growth group [expGroup]
     * (e.g. "MEDIUM_SLOW"), from the cumulative table.
     */
    fun expForLevel(expGroup: String, level: Int): Int {
        val table = expTables[expGroup]
            ?: throw IllegalArgumentException("unknown experience group: $expGroup")
        require(level in 1..100) { "level out of range: $level" }
        return table[level]
    }

    companion object {
        const val ASSET_PATH = "xdnetplay/gen3data.json"

        /** Load the bundled asset. */
        fun load(context: Context): Gen3Data =
            context.assets.open(ASSET_PATH).use { load(it) }

        fun load(stream: InputStream): Gen3Data =
            fromJson(stream.readBytes().toString(Charsets.UTF_8))

        /**
         * Normalize a display name to a lookup key: lowercase, ASCII letters
         * and digits only ("Mr. Mime" -> "mrmime", "Nidoran-F" -> "nidoranf").
         */
        fun normalizeName(name: String): String {
            val sb = StringBuilder(name.length)
            for (c in name.lowercase()) {
                if (c in 'a'..'z' || c in '0'..'9') {
                    sb.append(c)
                }
            }
            return sb.toString()
        }

        fun fromJson(text: String): Gen3Data {
            val root = JSONObject(text)

            val species = HashMap<String, Species>()
            val speciesObj = root.getJSONObject("species")
            for (key in speciesObj.keys()) {
                val s = speciesObj.getJSONObject(key)
                val name = normalizeName(key)
                species[name] = Species(
                    name = name,
                    id = s.getInt("id"),
                    natDex = s.getInt("natDex"),
                    baseStats = intArray(s.getJSONArray("baseStats")),
                    abilities = intArray(s.getJSONArray("abilities")),
                    genderRatio = s.getInt("genderRatio"),
                    expGroup = s.getString("expGroup"),
                    types = intArray(s.getJSONArray("types"))
                )
            }

            val expTables = HashMap<String, IntArray>()
            val expObj = root.getJSONObject("expTables")
            for (key in expObj.keys()) {
                // Actual generated shape: { "max": n, "cumulative": [101] }.
                // Also accept a bare [101] array (the originally stated schema).
                val value = expObj.get(key)
                val cumulative = when (value) {
                    is JSONObject -> value.getJSONArray("cumulative")
                    is JSONArray -> value
                    else -> throw IllegalArgumentException("bad expTables entry: $key")
                }
                expTables[key] = intArray(cumulative)
            }

            val natures = HashMap<String, Nature>()
            val naturesObj = root.getJSONObject("natures")
            for (key in naturesObj.keys()) {
                val n = naturesObj.getJSONObject(key)
                val name = normalizeName(key)
                natures[name] = Nature(
                    name = name,
                    id = n.getInt("id"),
                    plus = n.getInt("plus"),
                    minus = n.getInt("minus")
                )
            }

            val charset = HashMap<Char, Int>()
            val charsetObj = root.getJSONObject("charset")
            for (key in charsetObj.keys()) {
                if (key.length == 1) {
                    charset[key[0]] = charsetObj.getInt(key)
                }
            }

            val typeNames = HashMap<Int, String>()
            val typeNamesObj = root.optJSONObject("typeNames")
            if (typeNamesObj != null) {
                for (key in typeNamesObj.keys()) {
                    val id = key.toIntOrNull() ?: continue
                    typeNames[id] = typeNamesObj.getString(key)
                }
            }

            return Gen3Data(
                species = species,
                moves = idMap(root.getJSONObject("moves")),
                items = idMap(root.getJSONObject("items")),
                abilities = idMap(root.getJSONObject("abilities")),
                natures = natures,
                expTables = expTables,
                charset = charset,
                typeNames = typeNames
            )
        }

        private fun intArray(arr: JSONArray): IntArray {
            val out = IntArray(arr.length())
            for (i in 0 until arr.length()) {
                out[i] = arr.getInt(i)
            }
            return out
        }

        private fun idMap(obj: JSONObject): Map<String, Int> {
            val out = HashMap<String, Int>()
            for (key in obj.keys()) {
                out[normalizeName(key)] = obj.getInt(key)
            }
            return out
        }
    }
}
