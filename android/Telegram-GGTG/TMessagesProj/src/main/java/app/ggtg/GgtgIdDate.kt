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

    // ---- the estimate of when an account was registered, by its id (mirrors Ggtg::EstimateIdInfo of the desktop app) ----
    //
    // Between the anchors the date follows a smooth monotone curve; past the last anchor it goes on at the
    // recent rate, up to now. Every account registered by GGTG adds an exact (id, time) point to
    // ggtg_bulk_db/id_calibration.json. The first profile photo bounds the answer from above.

    class IdEstimate(val time: Long, val uncertaintyDays: Int, val extrapolated: Boolean)

    private class Point(val id: Double, val time: Double, val exact: Boolean)

    private val calibration: MutableList<Pair<Long, Long>> by lazy {
        val list = ArrayList<Pair<Long, Long>>()
        try {
            val file = java.io.File(GgtgDb.dir(), "id_calibration.json")
            if (file.exists()) {
                val array = org.json.JSONArray(file.readText())
                for (i in 0 until array.length()) {
                    val o = array.optJSONObject(i) ?: continue
                    val id = o.optLong("id"); val t = o.optLong("t")
                    if (id > 0 && t > 0) list.add(id to t)
                }
            }
        } catch (_: Throwable) {}
        list
    }

    /** A fresh account: its id and the real time are an exact point for every other id. */
    @JvmStatic
    @Synchronized
    fun calibrate(userId: Long, time: Long) {
        if (userId <= 0 || time <= 0 || calibration.any { it.first == userId }) return
        calibration.add(userId to time)
        try {
            val array = org.json.JSONArray()
            for ((id, t) in calibration) array.put(org.json.JSONObject().put("id", id).put("t", t))
            java.io.File(GgtgDb.dir(), "id_calibration.json").writeText(array.toString())
        } catch (_: Throwable) {}
    }

    /** Anchors and exact points, sorted by id and kept monotone: an exact point wins over a contradicting anchor. */
    @Synchronized
    private fun points(): List<Point> {
        val all = ArrayList<Point>()
        for (a in anchors) all.add(Point(a.id.toDouble(), a.timestamp.toDouble(), false))
        for ((id, t) in calibration) all.add(Point(id.toDouble(), t.toDouble(), true))
        all.sortWith(compareBy<Point>({ it.id }, { if (it.exact) 0 else 1 }))
        val result = ArrayList<Point>()
        for (p in all) {
            if (result.isNotEmpty() && p.id == result.last().id) continue
            if (result.isNotEmpty() && p.time < result.last().time) {
                if (!p.exact) continue
                while (result.isNotEmpty() && !result.last().exact && result.last().time > p.time) result.removeAt(result.size - 1)
                if (result.isNotEmpty() && result.last().time > p.time) continue
            }
            result.add(p)
        }
        return result
    }

    /** Fritsch-Carlson slopes of a monotone cubic through the points. */
    private fun slopes(p: List<Point>): DoubleArray {
        val n = p.size
        val delta = DoubleArray(n - 1) { (p[it + 1].time - p[it].time) / (p[it + 1].id - p[it].id) }
        val m = DoubleArray(n)
        m[0] = delta[0]; m[n - 1] = delta[n - 2]
        for (i in 1 until n - 1) {
            if (delta[i - 1] * delta[i] <= 0.0) { m[i] = 0.0; continue }
            val h0 = p[i].id - p[i - 1].id
            val h1 = p[i + 1].id - p[i].id
            val w1 = 2 * h1 + h0
            val w2 = h1 + 2 * h0
            m[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i])
        }
        return m
    }

    /** [upperBound]: the account is not younger than this (unix seconds, 0 - unknown), e.g. its first profile photo. */
    @JvmStatic
    fun estimateInfo(userId: Long, upperBound: Long = 0L): IdEstimate? {
        if (userId <= 0) return null
        val p = points()
        if (p.size < 2) return null
        val now = System.currentTimeMillis() / 1000L
        val x = userId.toDouble()
        var time: Long
        var uncertainty: Int
        var extrapolated = false
        if (x <= p.first().id) {
            time = p.first().time.toLong(); uncertainty = 30
        } else if (x >= p.last().id) {
            val last = p.last()
            var from = p.size - 2
            while (from > 0 && last.time - p[from].time < 180.0 * 86400.0) from--
            val slope = (last.time - p[from].time) / (last.id - p[from].id)
            time = minOf(last.time + (x - last.id) * slope, now.toDouble()).toLong()
            extrapolated = true
            uncertainty = (30 + (time - last.time.toLong()) / 86400 * 0.15).toInt()
        } else {
            val m = slopes(p)
            var i = 0
            while (i + 2 < p.size && x > p[i + 1].id) i++
            val a = p[i]; val b = p[i + 1]
            val h = b.id - a.id
            val t = (x - a.id) / h
            val t2 = t * t; val t3 = t2 * t
            time = ((2 * t3 - 3 * t2 + 1) * a.time + (t3 - 2 * t2 + t) * h * m[i] +
                (-2 * t3 + 3 * t2) * b.time + (t3 - t2) * h * m[i + 1]).toLong()
            val gapDays = ((b.time - a.time) / 86400.0).toInt()
            uncertainty = if (a.exact && b.exact) maxOf(3, gapDays / 20) else gapDays.div(4).coerceIn(14, 120)
        }
        if (upperBound > 0 && time > upperBound) {
            time = upperBound
            uncertainty = minOf(uncertainty, 30)
        }
        return IdEstimate(time, uncertainty, extrapolated)
    }

    // The estimate as unix seconds (0 when the id is unknown): the age of an account is counted from it.
    @JvmStatic
    @JvmOverloads
    fun estimateTime(userId: Long, upperBound: Long = 0L): Long = estimateInfo(userId, upperBound)?.time ?: 0L

    // "около 2 лет 3 мес." for the account of this id.
    @JvmStatic
    @JvmOverloads
    fun ageText(userId: Long, upperBound: Long = 0L): String {
        val created = estimateTime(userId, upperBound)
        val now = System.currentTimeMillis() / 1000L
        if (created <= 0 || now <= created) return ""
        val months = ((now - created) / (30L * 24 * 3600)).toInt()
        val years = months / 12
        val rest = months % 12
        if (years <= 0) return if (rest <= 1) "меньше месяца" else "около $rest мес."
        val m100 = years % 100
        val m10 = years % 10
        val word = when {
            m100 in 11..14 -> "лет"
            m10 == 1 -> "год"
            m10 in 2..4 -> "года"
            else -> "лет"
        }
        return "около $years $word" + if (rest > 0) " $rest мес." else ""
    }

    // "~сентябрь 2026 (±1 мес.)": the month of the estimate and how far it can be off.
    @JvmStatic
    @JvmOverloads
    fun estimate(userId: Long, upperBound: Long = 0L): String {
        val info = estimateInfo(userId, upperBound) ?: return ""
        val cal = Calendar.getInstance(TimeZone.getTimeZone("UTC")).apply { timeInMillis = info.time * 1000L }
        val monthIdx = cal.get(Calendar.MONTH)
        val year = cal.get(Calendar.YEAR)
        val monthName = if (monthIdx in 0..11) months[monthIdx] else ""
        if (monthName.isEmpty()) return ""
        var result = "~$monthName $year"
        if (info.uncertaintyDays >= 20) result += " (±${maxOf(1, Math.round(info.uncertaintyDays / 30.0).toInt())} мес.)"
        return result
    }
}
