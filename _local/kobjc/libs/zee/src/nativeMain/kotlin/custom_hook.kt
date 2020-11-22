
/*
 * Copyright 2010-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */
package custom_hook

import kotlin.test.*

import kotlin.native.concurrent.*

fun testCustomHook() {
    assertFailsWith<InvalidMutabilityException> {
        setUnhandledExceptionHook { _ ->
            println("wrong")
    
        }
    }

    val x = 42
    val old = setUnhandledExceptionHook({
        throwable: Throwable ->
            println("value $x: ${throwable::class.simpleName}")
    }.freeze())

    assertNull(old)

    throw Error("an error")
}
