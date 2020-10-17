package example


/*
 * Copyright 2010-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */


import kotlin.native.concurrent.*
import kotlin.native.ref.*
import kotlin.test.*
import runtime.memory.var1.*
import kotlin.native.internal.test.*
import runtime.collections.AbstractMutableCollection.*
import runtime.collections.SortWith.*;
import runtime.workers.atomic0.*;
import runtime.workers.freeze6.*;
import runtime.collections.array0.*;
import runtime.workers.worker1.*;
import runtime.collections.hash_map0.*;
import runtime.workers.freeze_stress.*;
import runtime.lazy.*;
import runtime.workers.enum_identity.*;
import kotlinx.cinterop.*;
import objcSmoke.*;
import runtime.collections.hash_set0.*;
import kotlin.test.smoke.*;
import test.text.harmony_regex.*;
import runtime.concurrent.worker_bound_reference0.*;
import runtime.memory.weak1.*;
import kotlin.test.leakmem.*;
import test.text.harmony_regex.*;

interface Interface {
    fun iMember(clazz: Interface) : Interface { return clazz; }
    fun test2(ifc: Interface?) : Interface? { return null; }
}


fun Any?.asAny(): Any? = this

@Suppress("CONFLICTING_OVERLOADS")
class MutablePairImpl(first: Int, second: Int) : NSObject(), MutablePairProtocol {
    private var elements = intArrayOf(first, second)

    override fun first() = elements.first()
    override fun second() = elements.last()

    override fun update(index: Int, add: Int) {
        elements[index] += add
    }

    override fun update(index: Int, sub: Int) {
        elements[index] -= sub
    }

    constructor() : this(123, 321)
}


fun runArray0Test() {
    // Create instances of all array types.
    val byteArray = ByteArray(5)
    println(byteArray.size.toString())

    val charArray = CharArray(6)
    println(charArray.size.toString())

    val shortArray = ShortArray(7)
    println(shortArray.size.toString())

    val intArray = IntArray(8)
    println(intArray.size.toString())

    val longArray = LongArray(9)
    println(longArray.size.toString())

    val floatArray = FloatArray(10)
    println(floatArray.size.toString())

    val doubleArray = FloatArray(11)
    println(doubleArray.size.toString())

    val booleanArray = BooleanArray(12)
    println(booleanArray.size.toString())

    val stringArray = Array<String>(13, { i -> ""})
    println(stringArray.size.toString())
}

fun testTypes() : MutablePairProtocol {
val v = MutablePairImpl(1, 2).asAny();
val v2 = v  as MutablePairProtocol;
return v;
}

var t1 = kotlin.system.getTimeMillis();
fun printTime(test:String) {
    kotlin.native.internal.GC.collect()
    val t2 = kotlin.system.getTimeMillis();
    println(test + ": " + (t2 - t1) + "ms");
    kotlin.native.internal.GC.rtgcLog();
    t1 = kotlin.system.getTimeMillis();
}

fun test(str: String) {
    try {
printTime("start")


runFreezeStressTest();
printTime("runFreezeStressTest")


runArray0Test()
runWeakTest1()
runWeakTest2()
printTime("runWeakTest1,2")

runHashMap0Test();
printTime("runHashMap0Test")




val test_ref_count_bug_in_param_side_effect = true;
if (test_ref_count_bug_in_param_side_effect) {
    runVar1Test()
    printTime("runVar1Test")
}

runLeakMemoryTest()
printTime("runLeakMemoryTest")

runLeakMemoryWithWorkerTerminationTest()
printTime("runLeakMemoryWithWorkerTerminationTest")

runIlleagalSharingWithWeakTest()
printTime("runIlleagalSharingWithWeakTest")

val test_gc_bug_in_worker_thread = true;
if (test_gc_bug_in_worker_thread) {
    runWorkerBoundReference0Test()
    printTime("runWorkerBoundReference0Test")

    runAtomicTest();
    printTime("runAtomicTest")
}

runPatternTest()
printTime("runPatternTest")

runPattern2Test()
printTime("runPattern2Test")

//AllCodePointsTest().test()
//printTime("AllCodePointsTest")

testObjCSmoke();
printTime("testObjCSmoke")

testTypes();
printTime("testTypes")


runHashSet0Test();
printTime("runHashSet0Test")

runEnumIdentityTest();
printTime("runEnumIdentityTest")


runWorker1Test();
printTime("runWorker1Test")

testFreeze6();
printTime("testFreeze6")

runArrayTest();
printTime("runArrayTest")

runSortTest();
printTime("runSortTest")

runTestRunner()
printTime("runTestRunner")

//runTest1()
        //runTest2()
    }
    catch (e: Exception){
        println(e)
    }
}



fun forIntegers(b: Byte, s: UShort, i: Int, l: ULong?) {
    test("forIntegers!!!");
}
fun forFloats(ifc: Interface, d: Double?) {
    test("forIntegers!!!")
}

fun strings(str: String?) : String {
  return "That is '$str' from C"
}

fun acceptFun(f: (String) -> String?) = {
    f("Kotlin/Native rocks!")
}
fun supplyFun() : (String) -> String? = {
    "$it is cool!"
}

fun main(args: Array<String>) {
  println("Hello kotlin")
}
