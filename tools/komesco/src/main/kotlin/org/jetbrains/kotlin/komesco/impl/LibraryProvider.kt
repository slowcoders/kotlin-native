package org.jetbrains.kotlin.komesco.impl

import kotlinx.metadata.*
import kotlinx.metadata.klib.*
import org.jetbrains.kotlin.komesco.*
import org.jetbrains.kotlin.library.MetadataLibrary

class LibraryProvider(
        private val library: MetadataLibrary
) : KlibModuleMetadata.MetadataLibraryProvider {

    override val moduleHeaderData: ByteArray
        get() = library.moduleHeaderData

    override fun packageMetadata(fqName: String, partName: String): ByteArray =
            library.packageMetadata(fqName, partName)

    override fun packageMetadataParts(fqName: String): Set<String> =
            library.packageMetadataParts(fqName)
}

private fun joinPackages(pkg1: KmPackage, pkg2: KmPackage) = KmPackage().apply {
    functions += (pkg1.functions + pkg2.functions).sortedBy(KmFunction::name)
    properties += (pkg1.properties + pkg2.properties).sortedBy(KmProperty::name)
    typeAliases += (pkg1.typeAliases + pkg2.typeAliases).sortedBy(KmTypeAlias::name)
}

private fun joinFragments(frag1: KmModuleFragment, frag2: KmModuleFragment) = KmModuleFragment().apply {
    pkg = when {
        frag1.pkg != null && frag2.pkg != null -> joinPackages(frag1.pkg!!, frag2.pkg!!)
        frag1.pkg != null -> joinPackages(frag1.pkg!!, KmPackage())
        frag2.pkg != null -> joinPackages(KmPackage(), frag2.pkg!!)
        else -> null
    }
    fqName = when {
        frag1.fqName != null -> frag1.fqName
        frag2.fqName != null -> frag2.fqName
        else -> null
    }
    classes += frag1.classes.sortedBy { it.name } + frag2.classes.sortedBy { it.name }
}

class SortedMergeStrategy : KlibModuleFragmentReadStrategy {

    override fun processModuleParts(parts: List<KmModuleFragment>): List<KmModuleFragment> =
            parts.fold(KmModuleFragment(), ::joinFragments).let(::listOf)
}

private inline fun <reified T : Any> compareElements(
        e1: List<T>, e2: List<T>,
        crossinline comparator: (T, T) -> CompareResult
): CompareResult {
    if (e1.size != e2.size) {
        return Fail("Sizes mismatch: ${e1.size}, ${e2.size}")
    }
    return e1.asSequence().zip(e2.asSequence())
            .map { comparator(it.first, it.second) }
            .toList()
            .wrap()
}

/**
 * TODO: Return lists without missing elements
 */
private fun <T> validateMissingElements(e1: List<T>, e2: List<T>, key: T.() -> String): CompareResult {
    val m1 = e1.associateBy { it.key() }
    val m2 = e2.associateBy { it.key() }
    val secondMissings = e1
            .filter { it.key() !in m2 }
            .map { Fail("${it.key()} is missing in second list") }
    val firstMissings = e2
            .filter { it.key() !in m1 }
            .map { Fail("${it.key()} is missing in first list") }
    return (firstMissings + secondMissings).wrap()
}

fun <T> processFragment(fragment: KmModuleFragment, cb: (List<KmClass>, List<KmFunction>, List<KmProperty>, List<KmTypeAlias>) -> T): T {
    val classes = fragment.classes
    val pkg = fragment.pkg
    return when {
        pkg != null -> cb(classes, pkg.functions, pkg.properties, pkg.typeAliases)
        else -> cb(classes, emptyList(), emptyList(), emptyList())
    }
}

fun compareMetadata(
        config: Config,
        metadataModuleA: KlibModuleMetadata,
        metadataModuleB: KlibModuleMetadata
): CompareResult {
    val fragmentA = metadataModuleA.fragments.fold(KmModuleFragment(), ::joinFragments)
    val fragmentB = metadataModuleB.fragments.fold(KmModuleFragment(), ::joinFragments)

    processFragment(fragmentA) { classesA, functionsA, propertiesA, typeAliasesA ->
        processFragment(fragmentB) { classesB, functionsB, propertiesB, typeAliasesB ->
            listOf(
                    validateMissingElements(classesA, classesB, KmClass::name),
                    validateMissingElements(functionsA, functionsB, KmFunction::name),
                    validateMissingElements(propertiesA, propertiesB, KmProperty::name),
                    validateMissingElements(typeAliasesA, typeAliasesB, KmTypeAlias::name)
            ).wrap()
        }
    }.let { if (it is Fail) return it }

    return processFragment(fragmentA) { classesA, functionsA, propertiesA, typeAliasesA ->
        processFragment(fragmentB) { classesB, functionsB, propertiesB, typeAliasesB ->
            val comparator = KmComparator(config)
            listOf(
                    compareElements(classesA, classesB, comparator::compare),
                    compareElements(functionsA, functionsB, comparator::compare),
                    compareElements(propertiesA, propertiesB, comparator::compare),
                    compareElements(typeAliasesA, typeAliasesB, comparator::compare)
            ).wrap()
        }
    }
}