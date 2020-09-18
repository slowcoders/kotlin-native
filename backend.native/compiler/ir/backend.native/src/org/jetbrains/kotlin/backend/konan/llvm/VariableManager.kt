/*
 * Copyright 2010-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

package org.jetbrains.kotlin.backend.konan.llvm

import llvm.*
import org.jetbrains.kotlin.backend.common.ir.ir2string
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.declarations.IrValueDeclaration
import org.jetbrains.kotlin.ir.declarations.IrVariable
import org.jetbrains.kotlin.ir.declarations.IrValueParameter
import org.jetbrains.kotlin.ir.declarations.impl.IrValueParameterImpl
import org.jetbrains.kotlin.name.Name

internal fun IrElement.needDebugInfo(context: Context) = context.shouldContainDebugInfo() || (this is IrVariable && this.isVar)

internal class VariableManager(val functionGenerationContext: FunctionGenerationContext) {
    internal interface Record {
        fun load() : LLVMValueRef
        fun store(value: LLVMValueRef)
        fun address() : LLVMValueRef
        fun attachReturnValue(value: LLVMValueRef) { throw Error("writing to immutable: ") }
        fun getAttachedReturnValue() : LLVMValueRef? { throw Error("no attached value: ") }
    }

    inner class SlotRecord(val address: LLVMValueRef, val refSlot: Boolean, val isVar: Boolean) : Record {
        var loadedValues = mutableListOf<Pair<Int, LLVMValueRef>>();
        var attachedRetValue: LLVMValueRef? = null
        override fun load() : LLVMValueRef {
            if (!functionGenerationContext.ENABLE_ALTER_ARGS) {
                return functionGenerationContext.loadSlot(address, !functionGenerationContext.RTGC && isVar)
            }
            val layer = functionGenerationContext.vars.argLists.size;
            val value = functionGenerationContext.loadSlot(address, !functionGenerationContext.RTGC && isVar);
            if (layer > 0) {
                loadedValues.add(layer to value);
            }
            // println("*** load " + address + " -> " + value);
            return value;
        }
        override fun store(value: LLVMValueRef) {
            if (functionGenerationContext.ENABLE_ALTER_ARGS && loadedValues.size > 0) alterPushedVariable(loadedValues);
            functionGenerationContext.storeAny(value, address, true)
        }
        override fun address() : LLVMValueRef = this.address
        override fun toString() = (if (refSlot) "refslot" else "slot") + " for ${address}"
        override fun attachReturnValue(value: LLVMValueRef) { attachedRetValue = value }
        override fun getAttachedReturnValue() : LLVMValueRef? = attachedRetValue
    }

    inner class ParameterRecord(val address: LLVMValueRef, val refSlot: Boolean) : Record {
        override fun load() : LLVMValueRef = functionGenerationContext.loadSlot(address, false)
        override fun store(value: LLVMValueRef) = throw Error("writing to parameter")
        override fun address() : LLVMValueRef = this.address
        override fun toString() = (if (refSlot) "refslot" else "slot") + " for ${address}"
    }

    class ValueRecord(val value: LLVMValueRef, val name: Name) : Record {
        override fun load() : LLVMValueRef = value
        override fun store(value: LLVMValueRef) = throw Error("writing to immutable: ${name}")
        override fun address() : LLVMValueRef = throw Error("no address for: ${name}")
        override fun toString() = "value of ${value} from ${name}"
    }

    val variables: ArrayList<Record> = arrayListOf()
    val contextVariablesToIndex: HashMap<IrValueDeclaration, Int> = hashMapOf()
    val argLists: ArrayList<MutableList<Pair<IrValueParameter, LLVMValueRef>>> = arrayListOf()

    fun pushArgList(argList: MutableList<Pair<IrValueParameter, LLVMValueRef>>) {
        argLists.add(argList)
    }

    fun popArgList(argList: MutableList<Pair<IrValueParameter, LLVMValueRef>>) {
        val layer = argLists.size;
        for (v in variables) {
            if (!(v is SlotRecord)) continue;

            val values = v.loadedValues;
            var i = values.size;
            while ( --i >= 0 ) {
                val vr = values.get(i);
                if (vr.first == layer) {
                    values.removeAt(i);
                }
            }
        }
        argLists.remove(argList)
    }
    
    fun alterPushedVariable(values : List<Pair<Int, LLVMValueRef>>) {
        var alterVariable: LLVMValueRef? = null;
        for (argList in argLists) {
            for (i in 0 until argList.size) {
                val arg = argList[i];
                for (v in values) {
                    if (arg.second == v.second) {
                        if (alterVariable == null) {
                            val slot_addr = createAnonymousSlot(v.second);
                            alterVariable = functionGenerationContext.loadSlot(slot_addr, false);
                            println("****************** " + alterVariable);
                        }
                        argList[i] = (arg.first to alterVariable)
                    }
                }
            }
        }
    }

    // Clears inner state of variable manager.
    fun clear() {
        skipSlots = 0
        variables.clear()
        contextVariablesToIndex.clear()
    }

    fun createVariable(valueDeclaration: IrValueDeclaration, value: LLVMValueRef? = null, variableLocation: VariableDebugLocation?) : Int {
        val isVar = valueDeclaration is IrVariable && valueDeclaration.isVar
        // Note that we always create slot for object references for memory management.
        if (!functionGenerationContext.context.shouldContainDebugInfo() && !isVar && value != null)
            return createImmutable(valueDeclaration, value)
        else
            // Unfortunately, we have to create mutable slots here,
            // as even vals can be assigned on multiple paths. However, we use varness
            // knowledge, as anonymous slots are created only for true vars (for vals
            // their single assigner already have slot).
            return createMutable(valueDeclaration, isVar, value, variableLocation)
    }

    internal fun createMutable(valueDeclaration: IrValueDeclaration,
                               isVar: Boolean, value: LLVMValueRef? = null, variableLocation: VariableDebugLocation?) : Int {
        assert(!contextVariablesToIndex.contains(valueDeclaration))
        val index = variables.size
        val type = functionGenerationContext.getLLVMType(valueDeclaration.type)
        val slot = functionGenerationContext.alloca(type, valueDeclaration.name.asString(), variableLocation)
        if (value != null)
            functionGenerationContext.storeAny(value, slot, true)
        variables.add(SlotRecord(slot, functionGenerationContext.isObjectType(type), isVar))
        contextVariablesToIndex[valueDeclaration] = index
        return index
    }

    internal var skipSlots = 0
    internal fun createParameter(valueDeclaration: IrValueDeclaration, variableLocation: VariableDebugLocation?) : Int {
        assert(!contextVariablesToIndex.contains(valueDeclaration))
        val index = variables.size
        val type = functionGenerationContext.getLLVMType(valueDeclaration.type)
        val slot = functionGenerationContext.alloca(
                type, "p-${valueDeclaration.name.asString()}", variableLocation)
        val isObject = functionGenerationContext.isObjectType(type)
        variables.add(ParameterRecord(slot, isObject))
        contextVariablesToIndex[valueDeclaration] = index
        if (isObject)
            skipSlots++
        return index
    }

    // Creates anonymous mutable variable.
    // Think of slot reuse.
    fun createAnonymousSlot(value: LLVMValueRef? = null) : LLVMValueRef {
        val index = createAnonymousMutable(functionGenerationContext.kObjHeaderPtr, value)
        return addressOf(index)
    }

    private fun createAnonymousMutable(type: LLVMTypeRef, value: LLVMValueRef? = null) : Int {
        val index = variables.size
        val slot = functionGenerationContext.alloca(type, variableLocation = null)
        if (value != null)
            functionGenerationContext.storeAny(value, slot, true)
        variables.add(SlotRecord(slot, functionGenerationContext.isObjectType(type), true))
        return index
    }

    internal fun createImmutable(valueDeclaration: IrValueDeclaration, value: LLVMValueRef) : Int {
        if (contextVariablesToIndex.containsKey(valueDeclaration))
            throw Error("${ir2string(valueDeclaration)} is already defined")
        val index = variables.size
        variables.add(ValueRecord(value, valueDeclaration.name))
        contextVariablesToIndex[valueDeclaration] = index
        return index
    }

    fun indexOf(valueDeclaration: IrValueDeclaration) : Int {
        return contextVariablesToIndex.getOrElse(valueDeclaration) { -1 }
    }

    fun addressOf(index: Int): LLVMValueRef {
        return variables[index].address()
    }

    fun load(index: Int): LLVMValueRef {
        return variables[index].load()
    }

    fun store(value: LLVMValueRef, index: Int) {
        variables[index].store(value)
    }

    fun attachReturnValue(value: LLVMValueRef, index: Int) { variables[index].attachReturnValue(value) }
    fun getAttachedReturnValue(index: Int) : LLVMValueRef? { return variables[index].getAttachedReturnValue() }
}

internal data class VariableDebugLocation(val localVariable: DILocalVariableRef, val location:DILocationRef?, val file:DIFileRef, val line:Int)

internal fun debugInfoLocalVariableLocation(builder: DIBuilderRef?,
        functionScope: DIScopeOpaqueRef, diType: DITypeOpaqueRef, name:Name, file: DIFileRef, line: Int,
        location: DILocationRef?): VariableDebugLocation {
    val variableDeclaration = DICreateAutoVariable(
            builder = builder,
            scope = functionScope,
            name = name.asString(),
            file = file,
            line = line,
            type = diType)

    return VariableDebugLocation(localVariable = variableDeclaration!!, location = location, file = file, line = line)
}

internal fun debugInfoParameterLocation(builder: DIBuilderRef?,
                                        functionScope: DIScopeOpaqueRef, diType: DITypeOpaqueRef,
                                        name:Name, argNo: Int, file: DIFileRef, line: Int,
                                        location: DILocationRef?): VariableDebugLocation {
    val variableDeclaration = DICreateParameterVariable(
            builder = builder,
            scope = functionScope,
            name = name.asString(),
            argNo = argNo,
            file = file,
            line = line,
            type = diType)

    return VariableDebugLocation(localVariable = variableDeclaration!!, location = location, file = file, line = line)
}
