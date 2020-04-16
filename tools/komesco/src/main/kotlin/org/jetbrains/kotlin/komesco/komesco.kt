package org.jetbrains.kotlin.komesco

import kotlinx.metadata.KmType
import kotlinx.metadata.klib.KlibModuleMetadata
import org.jetbrains.kotlin.komesco.impl.LibraryProvider
import org.jetbrains.kotlin.komesco.impl.SortedMergeStrategy
import org.jetbrains.kotlin.komesco.impl.compareMetadata
import org.jetbrains.kotlin.konan.file.File
import org.jetbrains.kotlin.library.CompilerSingleFileKlibResolveAllowingIrProvidersStrategy
import org.jetbrains.kotlin.library.resolveSingleFileKlib


sealed class CompareResult

object Ok : CompareResult()

class Fail(
        val children: List<Fail>, val message: String? = null
) : CompareResult() {
    constructor(message: String, child: Fail? = null)
            : this(listOfNotNull(child), message)
}

fun expandFail(fail: Fail, output: (String) -> Unit, padding: String = "") {
    fail.message?.let { output("$padding$it") }
    fail.children.forEach {
        expandFail(it, output, "$padding ")
    }
}

interface Config {
    fun <T, R> shouldCheck(property: T.() -> R): Boolean

    companion object {
        val Default: Config = object : Config {
            override fun <T, R> shouldCheck(property: T.() -> R): Boolean =
                    true
        }
    }
}

class InteropConfig : Config {
    override fun <T, R> shouldCheck(property: T.() -> R): Boolean = when (property) {
        KmType::abbreviatedType -> false
        else -> true
    }
}

fun compare(
        config: Config,
        pathToFirstLibrary: String,
        pathToSecondLibrary: String
): CompareResult {
    val resolveStrategy = CompilerSingleFileKlibResolveAllowingIrProvidersStrategy(knownIrProviders = listOf("kotlin.native.cinterop"))
    val klib1 = resolveSingleFileKlib(File(pathToFirstLibrary), strategy = resolveStrategy)
    val klib2 = resolveSingleFileKlib(File(pathToSecondLibrary), strategy = resolveStrategy)
    val metadata1 = KlibModuleMetadata.read(LibraryProvider(klib1), SortedMergeStrategy())
    val metadata2 = KlibModuleMetadata.read(LibraryProvider(klib2), SortedMergeStrategy())
    return compareMetadata(config, metadata1, metadata2)
}

fun main(args: Array<String>) {
    when (val result = compare(InteropConfig(), args[0], args[1])) {
        is Ok -> {
        }
        is Fail -> {
            expandFail(result, System.err::println)
        }
    }
}