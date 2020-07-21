package kotlin.test.leakmem;

import kotlin.native.concurrent.*
import kotlinx.cinterop.autoreleasepool
import objclib.*

fun runIlleagalSharingWithWeakTest() {
    autoreleasepool {
        run()
    }
}

private class NSObjectImpl : NSObject() {
    var x = 111
}

fun run() = withWorker {
    val obj = NSObjectImpl()
    setObject(obj)

    println("Before")
    val isAlive = try {
        execute(TransferMode.SAFE, {}) {
            isObjectAliveShouldCrash()
        }.result
    } catch (e: Throwable) {
        false
    }
    println("After $isAlive")
}
