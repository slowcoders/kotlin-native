package runtime.workers.worker6

import kotlin.test.*

import kotlin.native.concurrent.*

@Test fun runTest1() {
    withWorker {
        val future = execute(TransferMode.SAFE, { 42 }) { input ->
            input.toString()
        }
        future.consume { result ->
            println("Got $result")
        }
    }
    println("OK")
}

var int1 = 1
val int2 = 77

@Test fun runTest2() {
    int1++
    withWorker {
        executeAfter(0, {
            assertFailsWith<IncorrectDereferenceException> {
                int1++
                println("This line should not be printed.")
            }
            assertEquals(2, int1)
            assertEquals(77, int2)
        }.freeze())
    }
}

fun testWorker6() {
   runTest1();
   runTest2();
}
