package app.ggtg

import java.util.Calendar
import java.util.TimeZone

// ggtg: estimates approximate Telegram account registration date based on sequentially issued user_id
object GgtgIdDate {

    private class Anchor(val id: Long, val timestamp: Long)

    private val anchors = listOf(
        Anchor(1L, 1376438400L),          // 2013-08-14 (Telegram launch)
        Anchor(2768409L, 1383264000L),    // 2013-11-01
        Anchor(7679610L, 1388448000L),    // 2013-12-31
        Anchor(15835244L, 1392940000L),   // 2014-02-21
        Anchor(44634663L, 1399334000L),   // 2014-05-06
        Anchor(63263518L, 1414454000L),   // 2014-10-28
        Anchor(101260938L, 1425600000L),  // 2015-03-06
        Anchor(157242073L, 1446768000L),  // 2015-11-06
        Anchor(222021233L, 1465344000L),  // 2016-06-08
        Anchor(297621225L, 1481846000L),  // 2016-12-16
        Anchor(400169472L, 1501459000L),  // 2017-07-31
        Anchor(500000000L, 1515000000L),  // 2018-01-03
        Anchor(700000000L, 1545000000L),  // 2018-12-17
        Anchor(805158066L, 1563208000L),  // 2019-07-15
        Anchor(1000000000L, 1577836800L), // 2020-01-01
        Anchor(1300000000L, 1600000000L), // 2020-09-13
        Anchor(1600000000L, 1615000000L), // 2021-03-06
        Anchor(1974255900L, 1634000000L), // 2021-10-12
        Anchor(2100000000L, 1640995200L), // 2022-01-01
        Anchor(5000000000L, 1643673600L), // 2022-02-01 (64-bit jump)
        Anchor(5795034000L, 1662076800L), // 2022-09-02
        Anchor(6227468000L, 1679270400L), // 2023-03-20
        Anchor(6600000000L, 1695000000L), // 2023-09-18
        Anchor(7000000000L, 1712000000L), // 2024-04-01
        Anchor(7583599300L, 1739664000L), // 2025-02-16
        Anchor(7947063900L, 1754092800L), // 2025-08-02
        Anchor(8235679900L, 1758758400L), // 2025-09-25
        Anchor(8600000000L, 1775000000L)  // 2026-04-01
    )

    private val months = arrayOf(
        "январь", "февраль", "март", "апрель",
        "май", "июнь", "июль", "август",
        "сентябрь", "октябрь", "ноябрь", "декабрь"
    )

    @JvmStatic
    fun estimate(userId: Long): String {
        if (userId <= 0) return ""
        val t: Long = when {
            userId <= anchors.first().id -> anchors.first().timestamp
            userId >= anchors.last().id -> anchors.last().timestamp
            else -> {
                var found: Long = anchors.first().timestamp
                for (i in 0 until anchors.size - 1) {
                    val p1 = anchors[i]
                    val p2 = anchors[i + 1]
                    if (userId in p1.id..p2.id) {
                        val ratio = (userId - p1.id).toDouble() / (p2.id - p1.id).toDouble()
                        found = p1.timestamp + (ratio * (p2.timestamp - p1.timestamp)).toLong()
                        break
                    }
                }
                found
            }
        }
        val cal = Calendar.getInstance(TimeZone.getTimeZone("UTC")).apply {
            timeInMillis = t * 1000L
        }
        val monthIdx = cal.get(Calendar.MONTH)
        val year = cal.get(Calendar.YEAR)
        val monthName = if (monthIdx in 0..11) months[monthIdx] else ""
        return if (monthName.isNotEmpty()) "~$monthName $year" else ""
    }
}

