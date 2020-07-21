
package test.text.harmony_regex

import kotlin.text.*
import kotlin.test.*

class AllCodePointsTest {

    fun assertTrue(msg: String, value: Boolean) = assertTrue(value, msg)
    fun assertFalse(msg: String, value: Boolean) = assertFalse(value, msg)

    fun codePointToString(codePoint: Int): String {
        val charArray = Char.toChars(codePoint)
        return String(charArray, 0, charArray.size)
    }

    // TODO: Here is a performance problem: an execution of this test requires much more time than it in Kotlin/JVM.
    @Test fun test() {
        // Regression for HARMONY-3145
        var p = Regex("(\\p{all})+")
        var res = true
        var cnt = 0
        var s: String
        for (i in 0..1114111) {
    if (i == 4092) {
    println("at debug point");
    }
            //if (i % 3 == 0)
            //kotlin.native.internal.GC.collect();
            s = codePointToString(i)
            if (!s.matches(p)) {
                cnt++
                res = false
            }
//if (i % 3 == 0)
            //kotlin.native.internal.GC.collect();
        }
        assertTrue(res)
        assertEquals(0, cnt)

        p = Regex("(\\P{all})+")
        res = true
        cnt = 0

        for (i in 0..1114111) {
            s = codePointToString(i)
            if (!s.matches(p)) {
                cnt++
                res = false
            }
        }

        assertFalse(res)
        assertEquals(0x110000, cnt)
    }
}
