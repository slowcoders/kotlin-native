package org.jetbrains.kotlin.komesco.impl

import kotlinx.metadata.*
import kotlinx.metadata.klib.KlibEnumEntry
import kotlinx.metadata.klib.annotations
import org.jetbrains.kotlin.komesco.CompareResult
import org.jetbrains.kotlin.komesco.Config
import org.jetbrains.kotlin.komesco.Fail
import org.jetbrains.kotlin.komesco.Ok


class KmComparator(private val configuration: Config) {

    private fun <T, R> shouldCheck(prop: T.() -> R): Boolean =
            configuration.shouldCheck(prop)

    fun compare(kmClass1: KmClass, kmClass2: KmClass): CompareResult = serialComparator(
            compare(KmClass::name, ::compare) to "Different names: ${kmClass1.name}, ${kmClass2.name}",
            compare(KmClass::flags, ::compareClassFlags) to "Different flags for ${kmClass1.name}",
            compare(KmClass::constructors, ::compareConstructors) to "Constructors mismatch for ${kmClass1.name}",
    )(kmClass1, kmClass2)

    fun compare(typealias1: KmTypeAlias, typealias2: KmTypeAlias): CompareResult = serialComparator(
            compare(KmTypeAlias::name, ::compare) to "Different names",
            compare(KmTypeAlias::underlyingType, ::compareTypes) to "Underlying types mismatch",
            compare(KmTypeAlias::expandedType, ::compareTypes) to "Expanded types mismatch",
            compare(KmTypeAlias::typeParameters, ::compareTypeParams) to "Type parameters mismatch"
    )(typealias1, typealias2)

    fun compare(function1: KmFunction, function2: KmFunction): CompareResult = serialComparator(
            compare(KmFunction::name, ::compare) to "Different names",
            compare(KmFunction::returnType, ::compareTypes) to "Return type mismatch",
            compare(KmFunction::valueParameters, ::compareValueParameters) to "Value parameters mismatch"
    )(function1, function2)

    fun compare(property1: KmProperty, property2: KmProperty): CompareResult = serialComparator(
            compare(KmProperty::name, ::compare) to "Different names",
            compare(KmProperty::returnType, ::compareTypes) to "Return type mismatch",
            compare(KmProperty::flags, ::comparePropertyFlags) to "Flags mismatch",
            compare(KmProperty::getterFlags, ::comparePropertyAccessorFlags) to "Getter flags mismatch",
            compare(KmProperty::setterFlags, ::comparePropertyAccessorFlags) to "Setter flags mismatch"
    )(property1, property2)

    private fun compare(entry1: KlibEnumEntry, entry2: KlibEnumEntry): CompareResult = serialComparator(
            compare(KlibEnumEntry::annotations, ::compareAnnotations) to "Different annotations",
            compare(KlibEnumEntry::name, ::compare) to "Different names",
            compare(KlibEnumEntry::ordinal, compareNullable(::compare)) to "Different ordinals"
    )(entry1, entry2)

    private fun checkFlag(flag: Flag, flagName: String? = null): (Flags, Flags) -> CompareResult = { f1, f2 ->
        when {
            flag(f1) != flag(f2) -> {
                Fail("Flags mismatch: ${flag(f1)}, ${flag(f2)} for $flagName")
            }
            else -> Ok
        }
    }

    private fun compare(i1: Int, i2: Int): CompareResult = when {
        i1 == i2 -> Ok
        else -> Fail("$i1 != $i2")
    }

    private fun compare(s1: String, s2: String): CompareResult = when {
        s1 == s2 -> Ok
        else -> Fail("$s1 != $s2")
    }

    private fun comparePropertyFlags(flags1: Flags, flags2: Flags): CompareResult =
            serialComparator(
                    "Flags mismatch",
                    checkFlag(Flag.Property.IS_CONST, "IS_CONST"),
                    checkFlag(Flag.Property.HAS_SETTER, "HAS_SETTER"),
                    checkFlag(Flag.Property.HAS_GETTER, "HAS_GETTER"),
                    checkFlag(Flag.Property.IS_VAR, "IS_VAR"),
                    checkFlag(Flag.Property.HAS_CONSTANT, "HAS_CONSTANT"),
                    checkFlag(Flag.Property.IS_DECLARATION, "IS_DECLARATION"),
                    checkFlag(Flag.Property.IS_EXTERNAL, "IS_EXTERNAL")
            )(flags1, flags2)

    private fun comparePropertyAccessorFlags(flags1: Flags, flags2: Flags): CompareResult = serialComparator(
            "Accessor flags mismatch",
            checkFlag(Flag.PropertyAccessor.IS_NOT_DEFAULT, "IS_NOT_DEFAULT"),
            checkFlag(Flag.PropertyAccessor.IS_INLINE, "IS_INLINE"),
            checkFlag(Flag.PropertyAccessor.IS_EXTERNAL, "IS_EXTERNAL"),
            ::compareVisibilityFlags,
            ::compareModalityFlags
    )(flags1, flags2)


    private fun compareClassFlags(flags1: Flags, flags2: Flags): CompareResult = serialComparator(
            "Flags mismatch",
            checkFlag(Flag.Class.IS_CLASS, "IS_CLASS"),
            checkFlag(Flag.Class.IS_COMPANION_OBJECT, "IS_COMPANION_OBJECT"),
            checkFlag(Flag.Class.IS_ENUM_CLASS, "IS_ENUM_CLASS"),
            checkFlag(Flag.Class.IS_OBJECT, "IS_OBJECT"),
            checkFlag(Flag.IS_FINAL, "IS_FINAL"),
            checkFlag(Flag.IS_OPEN, "IS_OPEN"),
            checkFlag(Flag.HAS_ANNOTATIONS, "HAS_ANNOTATIONS"),
            ::compareVisibilityFlags,
            ::compareModalityFlags
    )(flags1, flags2)

    private fun compareVisibilityFlags(flags1: Flags, flags2: Flags): CompareResult = serialComparator(
            "Visibility flags mismatch",
            checkFlag(Flag.IS_PUBLIC, "IS_PUBLIC"),
            checkFlag(Flag.IS_PRIVATE_TO_THIS, "IS_PRIVATE_TO_THIS"),
            checkFlag(Flag.IS_PRIVATE, "IS_PRIVATE"),
            checkFlag(Flag.IS_PROTECTED, "IS_PROTECTED"),
            checkFlag(Flag.IS_INTERNAL, "IS_INTERNAL")
    )(flags1, flags2)

    private fun compareModalityFlags(flags1: Flags, flags2: Flags): CompareResult = serialComparator(
            "Flags mismatch",
            checkFlag(Flag.IS_FINAL, "IS_FINAL"),
            checkFlag(Flag.IS_ABSTRACT, "IS_ABSTRACT"),
            checkFlag(Flag.IS_OPEN, "IS_OPEN"),
            checkFlag(Flag.IS_SEALED, "IS_SEALED")
    )(flags1, flags2)

    private fun compareValueParameters(p1: List<KmValueParameter>, p2: List<KmValueParameter>): CompareResult =
            compareLists(p1, p2, ::compare)

    private fun compare(a1: KmAnnotation, a2: KmAnnotation): CompareResult {
        if (a1.className != a2.className) return Fail("${a1.className} != ${a2.className}")
        // TODO: compare values
        return Ok
    }

    fun compareAnnotations(a1: List<KmAnnotation>, a2: List<KmAnnotation>): CompareResult =
            compareLists(a1, a2, ::compare)

    fun compare(p1: KmValueParameter, p2: KmValueParameter): CompareResult = serialComparator(
            compare(KmValueParameter::name, ::compare) to "Different names",
            compare(KmValueParameter::type, compareNullable(::compareTypes)) to "Type mismatch",
            compare(KmValueParameter::annotations, ::compareAnnotations) to "Annotations mismatch"
    )(p1, p2)

    fun compareConstructors(cl1: List<KmConstructor>, cl2: List<KmConstructor>): CompareResult =
            compareLists(cl1, cl2, ::compare)

    fun compareTypeParams(tp1: List<KmTypeParameter>, tp2: List<KmTypeParameter>): CompareResult =
            compareLists(tp1, tp2, ::compare)

    fun compareArgs(args1: List<KmTypeProjection>, args2: List<KmTypeProjection>): CompareResult =
            compareLists(args1, args2, ::compare)

    fun compareTypeFlags(f1: Flags, f2: Flags): CompareResult = serialComparator(
            checkFlag(Flag.Type.IS_NULLABLE) to "Nullable flag mismatch",
            checkFlag(Flag.Type.IS_SUSPEND) to "Suspend flag mismatch"
    )(f1, f2)

    fun compareConstructorFlags(f1: Flags, f2: Flags): CompareResult = serialComparator(
            checkFlag(Flag.Constructor.IS_PRIMARY) to "IS_PRIMARY mismatch"
    )(f1, f2)

    fun compare(c1: KmConstructor, c2: KmConstructor): CompareResult = serialComparator(
            "Flags mismatch",
            ::compareVisibilityFlags,
            ::compareConstructorFlags
    )(c1.flags, c2.flags)

    fun compare(arg1: KmTypeProjection, arg2: KmTypeProjection): CompareResult = serialComparator(
            "Different projections ${render(arg1.type)} ${render(arg2.type)}",
            compare(KmTypeProjection::type, compareNullable(::compareTypes))
    )(arg1, arg2)

    fun compare(tp1: KmTypeParameter, tp2: KmTypeParameter): CompareResult =
            when {
                tp1.variance != tp2.variance -> Fail("Different variance")
                tp1.name != tp2.name -> Fail("${tp1.name}, ${tp2.name}")
                else -> Ok
            }

    private fun compareTypes(ty1: KmType, ty2: KmType): CompareResult = serialComparator(
            compare(KmType::classifier, ::compare) to "Classifiers mismatch",
            compare(KmType::arguments, ::compareArgs) to "Type arguments mismatch",
            compare(KmType::flags, ::compareTypeFlags) to "Type flags mismatch for",
            compare(KmType::abbreviatedType, compareNullable(::compareTypes)) to "Abbreviated types mismatch"
    )(ty1, ty2)

    private fun compare(class1: KmClassifier, class2: KmClassifier): CompareResult = when {
        class1 is KmClassifier.TypeAlias && class2 is KmClassifier.TypeAlias -> {
            if (class1.name == class2.name) Ok else Fail("Different type aliases: ${class1.name}, ${class2.name}")
        }
        class1 is KmClassifier.Class && class2 is KmClassifier.Class -> {
            if (class1.name == class2.name) Ok else Fail("Different classes: ${class1.name}, ${class2.name}")
        }
        class1 is KmClassifier.TypeParameter && class2 is KmClassifier.TypeParameter -> {
            if (class1.id == class2.id) Ok else Fail("Different type params: ${class1.id}, ${class2.id}")
        }
        else -> Fail("class1 is $class1 and class2 is $class2")
    }

    private fun <T> compareNullable(comparator: (T, T) -> CompareResult): (T?, T?) -> CompareResult = { a, b ->
        when {
            a != null && b != null -> comparator(a, b)
            a == null && b == null -> Ok
            else -> Fail("${render(a)} ${render(b)}")
        }
    }

    private fun <T, R> compare(
            prop: T.() -> R,
            comparator: (R, R) -> CompareResult
    ): (T, T) -> CompareResult = { o1, o2 ->
        if (shouldCheck(prop)) comparator(o1.prop(), o2.prop()) else Ok
    }
}