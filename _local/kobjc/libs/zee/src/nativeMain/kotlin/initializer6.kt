package runtime.basic.initializers6

import kotlin.test.*

import kotlin.native.concurrent.*

val aWorkerId = AtomicInt(0)
val bWorkersCount = 3

val aWorkerUnlocker = AtomicInt(0)
val bWorkerUnlocker = AtomicInt(0)

object A {
    init {
        // Must be called by aWorker only.
        println("### AAAA");
        assertEquals(aWorkerId.value, Worker.current.id)
        println("### AAAA---");
        // Only allow b workers to run, when a worker has started initialization.
        bWorkerUnlocker.increment()
        // Only proceed with initialization, when all b workers have started executing.
        while (aWorkerUnlocker.value < bWorkersCount) {}
        // And now wait a bit, to increase probability of races.
        Worker.current.park(100 * 1000L)
    }
    val a = produceA()
    val b = produceB()
}

fun produceA(): String {
    // Must've been called by aWorker only.
    println("### A");
    assertEquals(aWorkerId.value, Worker.current.id)
    println("### A--");
    return "A"
}

fun produceB(): String {
    // Must've been called by aWorker only.
    println("### B");
    assertEquals(aWorkerId.value, Worker.current.id)
    println("### B--");
    // Also check that it's ok to get A.a while initializing A.b.
    return "B+${A.a}"
}

@Test fun testInitializer6() {
    val aWorker = Worker.start()
    aWorkerId.value = aWorker.id
    val bWorkers = Array(bWorkersCount, { _ -> Worker.start() })

    val aFuture = aWorker.execute(TransferMode.SAFE, {}, {
        A.b
    })
    val bFutures = Array(bWorkers.size, {
        bWorkers[it].execute(TransferMode.SAFE, {}, {
            // Wait until A has started to initialize.
            while (bWorkerUnlocker.value < 1) {}
            // Now allow A initialization to continue.
            aWorkerUnlocker.increment()
            // And this should not've tried to init A itself.
            A.a + A.b
        })
    })

    for (future in bFutures) {
        assertEquals("AB+A", future.result)
    }
    assertEquals("B+A", aFuture.result)

    for (worker in bWorkers) {
        worker.requestTermination().result
    }
    aWorker.requestTermination().result
}
