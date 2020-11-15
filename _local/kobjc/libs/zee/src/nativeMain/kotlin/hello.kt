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
import illegal_sharing.*;
import runtime.basic.initializers6.*;
import test.utils.*
import kotlin.*
import runtime.workers.worker2.*;
import runtime.workers.worker6.*;
import runtime.workers.worker10.*;

interface Interface {
    fun iMember(clazz: Interface) : Interface { return clazz; }
    fun test2(ifc: Interface?) : Interface? { return null; }
}
fun Any?.asAny(): Any? = this


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

runPattern2Test()
printTime("runPattern2Test")

runPatternTest()
printTime("runPatternTest")


runEnumIdentityTest();
printTime("runEnumIdentityTest")


testWorker10();
printTime("testWorker10")

runWorker2Test()
printTime("runWorker2Test")


testWorker6();
printTime("testWorker6")


val test_gc_bug_in_worker_thread = true;
if (test_gc_bug_in_worker_thread) {
    runWorkerBoundReference0Test()
    printTime("runWorkerBoundReference0Test")

    runAtomicTest();
    printTime("runAtomicTest")
}



val test_deep_recursive = false;
if (test_deep_recursive) {
    testDeepRecusive()
    printTime("testDeepRecusive")
}

testInitializer6();
printTime("testInitializer6")

runFreezeStressTest();
printTime("runFreezeStressTest")

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

val test_without_memory_leak_check = false;
if (test_without_memory_leak_check) {
    runLeakMemoryTest()
    printTime("runLeakMemoryTest")

    runLeakMemoryWithWorkerTerminationTest()
    printTime("runLeakMemoryWithWorkerTerminationTest")
}

val test_without_illegal_sharing_check = false;
if (test_without_illegal_sharing_check) {
    runIlleagalSharingWithWeakTest()
    printTime("runIlleagalSharingWithWeakTest")
}

//AllCodePointsTest().test()
//printTime("AllCodePointsTest")

testObjCSmoke();
printTime("testObjCSmoke")


runHashSet0Test();
printTime("runHashSet0Test")


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
