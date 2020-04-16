package org.jetbrains.kotlin.komesco.impl

import kotlinx.metadata.*
import org.jetbrains.kotlin.komesco.CompareResult
import org.jetbrains.kotlin.komesco.Fail
import org.jetbrains.kotlin.komesco.Ok

internal fun render(element: Any?): String = when (element) {
    is KmTypeAlias -> "typealias ${element.name}"
    is KmFunction -> "function ${element.name}"
    is KmProperty -> "property ${element.name}"
    is KmClass -> "class ${element.name}"
    is KmType -> "`type ${element.classifier}`"
    else -> element.toString()
}

internal fun <T> serialComparator(
        vararg comparators: Pair<(T, T) -> CompareResult, String>
): (T, T) -> CompareResult = { o1, o2 ->
    comparators.map { (comparator, message) ->
                comparator(o1, o2).let { result ->
                    if (result is Fail) Fail(message, result) else result
                }
            }
            .wrap()
}

internal fun <T> serialComparator(
        message: String,
        vararg comparators: (T, T) -> CompareResult
): (T, T) -> CompareResult = { o1, o2 ->
    comparators
            .map { comparator ->
                comparator(o1, o2).let { result ->
                    if (result is Fail) Fail(message, result) else result
                }
            }
            .wrap()
}

internal fun List<CompareResult>.wrap(): CompareResult =
        filterIsInstance<Fail>().let { fails ->
            when (fails.size) {
                0 -> Ok
                else -> Fail(fails)
            }
        }



internal fun <T> compareLists(l1: List<T>, l2: List<T>, comparator: (T, T) -> CompareResult): CompareResult {
    if (l1.size != l2.size) return Fail("${l1.size} != ${l2.size}")
    return l1.zip(l2)
            .map { comparator(it.first, it.second) }
            .wrap()
}