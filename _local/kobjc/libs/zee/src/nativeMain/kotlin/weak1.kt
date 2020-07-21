
/*
 * Copyright 2010-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

package runtime.memory.weak1

import kotlin.test.*
import kotlin.native.ref.*

class Node(var next: Node?)

@Test fun runWeakTest1() {
    val node1 = Node(null)
    val node2 = Node(node1)
    node1.next = node2

    kotlin.native.ref.WeakReference(node1)
    println("OK")
}

@Test fun runWeakTest2() {
    val string = "Hello"
    val refString = WeakReference(string)
    assertEquals(string, refString.value)
    val zero = 0
    val refZero = WeakReference(zero)
    assertEquals(0, refZero.value)
    val _long:Any = Long.MAX_VALUE
    val refLong = WeakReference(_long)
    assertEquals(Long.MAX_VALUE, refLong.value)
}
