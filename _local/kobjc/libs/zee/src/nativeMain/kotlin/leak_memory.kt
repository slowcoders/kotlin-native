package kotlin.test.leakmem;

import kotlinx.cinterop.*

fun runLeakMemoryTest() {
    StableRef.create(Any())
}

