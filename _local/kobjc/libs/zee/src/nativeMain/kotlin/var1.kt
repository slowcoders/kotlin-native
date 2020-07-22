
package runtime.memory.var1

import kotlin.test.*

class Integer(val value: Int) {
    operator fun inc() = Integer(value + 1)
}

fun foo(x: Any, y: Any) {
    x.use()
    y.use()
}

@Test fun runTest1() {
    var x = Integer(0)
    println("Hello")

    for (i in 0..1) {
        val c = Integer(0)
        if (i == 0) x = c
    }

    // x refcount is 1.
print(x.toString())
    foo(x, ++x)
}

fun Any?.use() {
    var x = this
}

@kotlin.native.internal.CanBePrecreated
object CompileTime {

    const val int = Int.MIN_VALUE
    const val byte = Byte.MIN_VALUE
    const val short = Short.MIN_VALUE
    const val long = Long.MIN_VALUE
    const val boolean = true
    const val float = 1.0f
    const val double = 1.0
    const val char = Char.MIN_VALUE
}

class AClass {
    companion object {}
}

@Test fun runTest2() {
    assertEquals(Int.MIN_VALUE, CompileTime.int)
    assertEquals(Byte.MIN_VALUE, CompileTime.byte)
    assertEquals(Short.MIN_VALUE, CompileTime.short)
    assertEquals(Long.MIN_VALUE, CompileTime.long)
    assertEquals(true, CompileTime.boolean)
    assertEquals(1.0f, CompileTime.float)
    assertEquals(1.0, CompileTime.double)
    assertEquals(Char.MIN_VALUE, CompileTime.char)
}


fun runVar1Test() {
    runTest1()
    runTest2()
}

