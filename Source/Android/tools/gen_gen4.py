#!/usr/bin/env python3
"""
Regenerate the Gen 4 data the Android PBR team editor ships:

  app/src/main/assets/xdnetplay/gen4data.json
  app/src/main/java/.../features/xdnetplay/pbr/Gen4Glyphs.kt
  app/src/main/java/.../features/xdnetplay/pbr/Gen4Growth.kt

Every byte of input comes from PKHeX, pinned at commit 757a297f1463. Nothing is
hand-typed; the C# tables are scanned out of the source. Fetch the inputs into
./ref first:

  PIN=757a297f1463
  for p in \\
    PKHeX.Core/Resources/text/other/en/text_Species_en.txt \\
    PKHeX.Core/Resources/text/other/en/text_Moves_en.txt \\
    PKHeX.Core/Resources/text/items/text_Items_en.txt \\
    PKHeX.Core/Resources/text/other/en/text_Abilities_en.txt \\
    PKHeX.Core/Resources/text/other/en/text_Natures_en.txt \\
    PKHeX.Core/Resources/byte/personal/personal_hgss \\
    PKHeX.Core/PersonalInfo/Info/PersonalInfo4.cs \\
    PKHeX.Core/PKM/Util/Experience.cs \\
    PKHeX.Core/PKM/Strings/StringConverter4Util.cs ; do
    curl -sfLO --output-dir ref "https://raw.githubusercontent.com/kwsch/PKHeX/$PIN/$p"
  done

then

  python3 tools/gen_gen4.py \\
      app/src/main/assets/xdnetplay/gen4data.json \\
      app/src/main/java/org/dolphinemu/dolphinemu/features/xdnetplay/pbr/

What each input supplies:
  text_Species_en.txt    species names (index 0 is "Egg")
  text_Moves_en.txt      move names
  text_Items_en.txt      item names
  text_Abilities_en.txt  ability names
  text_Natures_en.txt    nature names
  personal_hgss          PersonalInfo4 records, SIZE 0x2C
  PersonalInfo4.cs       the field offsets mirrored below
  Experience.cs          Growth0..Growth5 experience curves
  StringConverter4Util.cs TableINT, the Gen 4 name glyph table
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "ref")

# Gen 4 index ceilings (inclusive). Anything above these never appears in a
# DP/Pt/HGSS/PBR record, so the asset stops there instead of shipping Gen 9 names.
MAX_SPECIES = 493      # Arceus
MAX_MOVE = 467         # Shadow Force
MAX_ITEM = 536         # HGSS item ceiling
MAX_ABILITY = 123      # Bad Dreams

# PersonalInfo4.cs
PERSONAL_SIZE = 0x2C
P_HP, P_ATK, P_DEF, P_SPE, P_SPA, P_SPD = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05
P_TYPE1, P_TYPE2 = 0x06, 0x07
P_GENDER = 0x10
P_GROWTH = 0x13
P_ABILITY1, P_ABILITY2 = 0x16, 0x17

# Gen 4 internal type ids. Unlike Gen 3 there is no unused "???" slot at 9, so
# everything from Fire up is shifted down by one -- verified against
# personal_hgss below (Bulbasaur 11/3, Charmander 9, Mewtwo 13, Rayquaza 15/2).
TYPE_NAMES = {
    0: "Normal", 1: "Fighting", 2: "Flying", 3: "Poison", 4: "Ground",
    5: "Rock", 6: "Bug", 7: "Ghost", 8: "Steel", 9: "Fire", 10: "Water",
    11: "Grass", 12: "Electric", 13: "Psychic", 14: "Ice", 15: "Dragon",
    16: "Dark",
}


def read_names(fn):
    with open(os.path.join(REF, fn), encoding="utf-8-sig") as f:
        return f.read().split("\n")


def normalize(name):
    """Must stay identical to Gen4Data.normalizeName in Kotlin.

    The gender symbols are folded to f/m first so Nidoran(F) and Nidoran(M)
    stay distinct keys and line up with Showdown's "Nidoran-F"/"Nidoran-M".
    """
    name = name.lower().replace("♀", "f").replace("♂", "m")
    return "".join(c for c in name if c.isascii() and c.isalnum())


# ---------------------------------------------------------------- glyph table
def parse_table_int():
    """Scan StringConverter4Util.TableINT out of the C# source (never retyped)."""
    text = open(os.path.join(REF, "StringConverter4Util.cs"), encoding="utf-8").read()
    consts = {"NUL": 0xFFFF, "EMP": 0xFFFF, "HGM": 0x246D, "HGF": 0x246E}
    start = text.index("public static ReadOnlySpan<char> TableINT =>")
    body = text[text.index("[", start) + 1: text.index("\n    ];", start)]
    body = re.sub(r"//[^\n]*", "", body)
    esc = {"\\": 0x5C, "'": 0x27, '"': 0x22, "0": 0, "n": 10, "r": 13,
           "t": 9, "a": 7, "b": 8, "f": 12, "v": 11}
    vals, i, n = [], 0, len(body)
    while i < n:
        c = body[i]
        if c in " \t\r\n,":
            i += 1
            continue
        if c == "'":
            i += 1
            if body[i] == "\\":
                i += 1
                e = body[i]
                if e == "u":
                    vals.append(int(body[i + 1:i + 5], 16))
                    i += 5
                else:
                    vals.append(esc[e])
                    i += 1
            else:
                vals.append(ord(body[i]))
                i += 1
            assert body[i] == "'", f"expected closing quote at {i}"
            i += 1
            continue
        m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", body[i:])
        assert m, f"unparsed at {i}: {body[i:i+20]!r}"
        vals.append(consts[m.group(0)])
        i += len(m.group(0))
    # Spot-checks against the row comments in the C# source.
    assert vals[0] == 0xFFFF
    assert chr(vals[0x121]) == "0" and chr(vals[0x12B]) == "A" and chr(vals[0x145]) == "a"
    assert vals[0x1BB] == 0x246D and vals[0x1BC] == 0x246E
    return vals


# ------------------------------------------------------------- growth tables
def parse_growth():
    src = open(os.path.join(REF, "Experience.cs"), encoding="utf-8").read()
    tables = []
    for g in range(6):
        m = re.search(rf"ReadOnlySpan<uint> Growth{g} =>\s*\[(.*?)\];", src, re.S)
        assert m, f"Growth{g} not found"
        body = re.sub(r"//[^\n]*", "", m.group(1))
        vals = [int(v.strip()) for v in body.split(",") if v.strip()]
        assert len(vals) == 100, f"Growth{g}: {len(vals)} entries"
        # PKHeX indexes [level-1]; re-index so element[level] is the EXP for that
        # level and element[0] is unused.
        tables.append([0] + vals)
    return tables


def main():
    personal = open(os.path.join(REF, "personal_hgss"), "rb").read()
    entries = len(personal) // PERSONAL_SIZE
    assert len(personal) % PERSONAL_SIZE == 0
    print(f"personal_hgss: {len(personal)} bytes = {entries} entries", file=sys.stderr)

    def pfield(idx, ofs):
        return personal[idx * PERSONAL_SIZE + ofs]

    species_names = read_names("text_Species_en.txt")
    move_names = read_names("text_Moves_en.txt")
    item_names = read_names("text_Items_en.txt")
    ability_names = read_names("text_Abilities_en.txt")
    nature_names = read_names("text_Natures_en.txt")
    assert len(nature_names) >= 25

    growth = parse_growth()
    # Sanity: Bulbasaur Medium Slow -> 1059860, Rattata Medium Fast -> 1000000.
    assert growth[pfield(1, P_GROWTH)][100] == 1059860, "Bulbasaur growth"
    assert growth[pfield(19, P_GROWTH)][100] == 1000000, "Rattata growth"
    # Sanity: the Gen 4 type ordering (Bulbasaur Grass/Poison, Mewtwo Psychic,
    # Rayquaza Dragon/Flying) -- catches a Gen 3 table being pasted in.
    assert (pfield(1, P_TYPE1), pfield(1, P_TYPE2)) == (11, 3), "Bulbasaur type"
    assert (pfield(150, P_TYPE1), pfield(150, P_TYPE2)) == (13, 13), "Mewtwo type"
    assert (pfield(384, P_TYPE1), pfield(384, P_TYPE2)) == (15, 2), "Rayquaza type"

    species = {}
    for sid in range(1, MAX_SPECIES + 1):
        name = species_names[sid]
        species[normalize(name)] = {
            "id": sid,
            "name": name,
            "baseStats": [pfield(sid, o) for o in
                          (P_HP, P_ATK, P_DEF, P_SPE, P_SPA, P_SPD)],
            "abilities": [pfield(sid, P_ABILITY1), pfield(sid, P_ABILITY2)],
            "genderRatio": pfield(sid, P_GENDER),
            "growth": pfield(sid, P_GROWTH),
            "types": [pfield(sid, P_TYPE1), pfield(sid, P_TYPE2)],
        }

    def id_map(names, hi):
        out = {}
        for i in range(1, hi + 1):
            key = normalize(names[i])
            if key and key not in out:
                out[key] = i
        return out

    natures = {}
    for i in range(25):
        plus, minus = i // 5, i % 5
        natures[normalize(nature_names[i])] = {
            "id": i,
            "name": nature_names[i],
            "plus": -1 if plus == minus else plus,
            "minus": -1 if plus == minus else minus,
        }

    data = {
        "_provenance": {
            "source": "kwsch/PKHeX",
            "commit": "757a297f1463",
            "files": [
                "PKHeX.Core/Resources/text/other/en/text_Species_en.txt",
                "PKHeX.Core/Resources/text/other/en/text_Moves_en.txt",
                "PKHeX.Core/Resources/text/items/text_Items_en.txt",
                "PKHeX.Core/Resources/text/other/en/text_Abilities_en.txt",
                "PKHeX.Core/Resources/text/other/en/text_Natures_en.txt",
                "PKHeX.Core/Resources/byte/personal/personal_hgss",
            ],
            "generator": "gen_gen4.py",
            "note": "baseStats order hp/atk/def/spe/spa/spd; nature plus/minus "
                    "index 0=atk 1=def 2=spe 3=spa 4=spd, -1 = neutral",
        },
        "species": species,
        "moves": id_map(move_names, MAX_MOVE),
        "items": id_map(item_names, MAX_ITEM),
        "abilities": id_map(ability_names, MAX_ABILITY),
        "natures": natures,
        # Display names indexed by id, so the UI never has to reverse a
        # normalized key back into something readable ("Shadow Force", not
        # "Shadowforce"). Index 0 is the games' own "none" placeholder.
        "moveNames": move_names[:MAX_MOVE + 1],
        "itemNames": item_names[:MAX_ITEM + 1],
        "abilityNames": ability_names[:MAX_ABILITY + 1],
        "typeNames": {str(k): v for k, v in TYPE_NAMES.items()},
    }

    out_json = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "gen4data.json")
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, separators=(",", ":"), sort_keys=False)
        f.write("\n")
    print(f"wrote {out_json} ({os.path.getsize(out_json)} bytes): "
          f"{len(species)} species, {len(data['moves'])} moves, "
          f"{len(data['items'])} items, {len(data['abilities'])} abilities",
          file=sys.stderr)

    # ---------------------------------------------------------- Kotlin tables
    ktdir = sys.argv[2] if len(sys.argv) > 2 else HERE

    glyphs = parse_table_int()
    with open(os.path.join(ktdir, "Gen4Glyphs.kt"), "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

/**
 * GENERATED FILE - do not edit by hand.
 *
 * PKHeX StringConverter4Util.TableINT, scanned out of the C# source by
 * gen_gen4.py (kwsch/PKHeX @ 757a297f1463). Index = the Gen 4 glyph value
 * stored in a BK4 nickname/OT field; value = the UTF-16 code unit it renders
 * as, or 0xFFFF when the slot is unmapped.
 */
internal object Gen4Glyphs {
    const val UNMAPPED = 0xFFFF

    /** Half-width male/female markers PKHeX maps to U+246D / U+246E. */
    const val HALF_MALE = 0x246D
    const val HALF_FEMALE = 0x246E

    val TABLE_INT = intArrayOf(
""")
        for i in range(0, len(glyphs), 12):
            row = ", ".join(f"0x{v:04X}" for v in glyphs[i:i + 12])
            f.write(f"        {row},\n")
        f.write("    )\n}\n")
    print(f"wrote Gen4Glyphs.kt ({len(glyphs)} entries)", file=sys.stderr)

    with open(os.path.join(ktdir, "Gen4Growth.kt"), "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

/**
 * GENERATED FILE - do not edit by hand.
 *
 * Gen 4 experience curves, extracted by gen_gen4.py from PKHeX
 * (kwsch/PKHeX @ 757a297f1463): Experience.Growth0..Growth5 re-indexed so
 * TABLES[rate][level] is the total EXP at that level (element 0 unused), and
 * the per-species growth rate byte at PersonalInfo4 +0x13 of personal_hgss.
 *
 * Kept in Kotlin rather than in gen4data.json so the pure save-format layer
 * has no Android/JSON dependency and can be unit-tested on a plain JVM.
 */
internal object Gen4Growth {
    val TABLES = arrayOf(
""")
        for t in growth:
            f.write("        intArrayOf(\n")
            for i in range(0, len(t), 10):
                f.write("            " + ", ".join(str(v) for v in t[i:i + 10]) + ",\n")
            f.write("        ),\n")
        f.write("    )\n\n")
        f.write("    /** Index = personal-table entry (species id for 1..493). */\n")
        f.write("    val SPECIES_GROWTH = intArrayOf(\n")
        rates = [pfield(i, P_GROWTH) for i in range(entries)]
        assert all(0 <= r <= 5 for r in rates)
        for i in range(0, len(rates), 25):
            f.write("        " + ", ".join(str(v) for v in rates[i:i + 25]) + ",\n")
        f.write("""    )

    private fun tableFor(species: Int): IntArray? {
        if (species < 0 || species >= SPECIES_GROWTH.size) {
            return null
        }
        return TABLES[SPECIES_GROWTH[species]]
    }

    /** Level 1..100 for [exp]; 0 when [species] is out of range. */
    fun levelFor(exp: Long, species: Int): Int {
        val table = tableFor(species) ?: return 0
        var level = 1
        while (level < 100 && exp >= table[level + 1]) {
            level++
        }
        return level
    }

    /** Total EXP at [level] (1..100); 0 when [species] is out of range. */
    fun expFor(level: Int, species: Int): Long {
        val table = tableFor(species) ?: return 0L
        return table[level.coerceIn(1, 100)].toLong()
    }
}
""")
    print(f"wrote Gen4Growth.kt ({entries} species rates)", file=sys.stderr)


if __name__ == "__main__":
    main()
