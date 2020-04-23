/*
 * Copyright 2010-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.common.atMostOne
import org.jetbrains.kotlin.backend.common.pop
import org.jetbrains.kotlin.backend.common.push
import org.jetbrains.kotlin.backend.konan.DirectedGraphCondensationBuilder
import org.jetbrains.kotlin.backend.konan.DirectedGraphMultiNode
import org.jetbrains.kotlin.backend.konan.llvm.Lifetime
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.backend.konan.descriptors.isArray
import org.jetbrains.kotlin.ir.util.file
import kotlin.math.min

internal object EscapeAnalysis {

    /*
     * The goal of escape analysis is to estimate lifetimes of all expressions in a program.
     * Possible lifetimes are:
     * 1. Local        - an object is used only within a function.
     * 2. Return value - an object is either returned or set to a field of an object being returned.
     * 3. Parameter    - an object is set to a field of exactly one parameter of a function.
     * 4. Global       - otherwise.
     *
     * The analysis is performed in two main steps - intraprocedural and interprocedural.
     * During intraprocedural analysis we remove all control flow related expressions and compute all possible
     * values of all variables within a function.
     * The goal of interprocedural analysis is to build points-to graph (an edge is created from A to B iff A holds
     * a reference to B). This is done by building call graph (using devirtualization for more precise result).
     *
     * How do exactly we build the points-to graph out of the call graph?
     * 1. Build condensation of the call graph.
     * 2. Handle vertices of the resulting DAG in topological order (ensuring that all functions being called
     *    are already handled).
     * 3. For a strongly connected component build the points-to graph iteratively starting with empty graph
     *    (since edges can only be added the process will end eventually).
     *
     * When we have the points-to graph it is easy to compute lifetimes - using DFS compute the graph's closure.
     */

    private val DEBUG = 0

    private inline fun DEBUG_OUTPUT(severity: Int, block: () -> Unit) {
        if (DEBUG > severity) block()
    }

    // TODO: elaborate on the following comment.
    // A special marker field for external types implemented in C++ (mainly, arrays).
    // The types are not used in the analysis - put there anything.
    private val intestinesField = DataFlowIR.Field(null, DataFlowIR.Type.Virtual, 1L, "inte\$tines")

    // A special marker field for return values.
    // Basically we substitute [return x] with [ret.v@lue = x].
    // We do this in order not to handle return parameter somewhat specially.
    private val returnsValueField = DataFlowIR.Field(null, DataFlowIR.Type.Virtual, 2L, "v@lue")

    // TODO: Kill.
    // Roles in which particular object reference is being used. Lifetime is computed from all roles reference.
    private enum class Role {
        // If reference is being returned.
        RETURN_VALUE,
        // If reference is being thrown.
        THROW_VALUE,
        // If reference's field is being written to.
        WRITE_FIELD,
        READ_FIELD,
        // If reference is being written to the global.
        WRITTEN_TO_GLOBAL,
        ASSIGNED,
    }

    // The less the higher an object escapes.
    object Depths {
        val INFINITY = 1_000_000
        val RETURN_VALUE = -1
        val PARAMETER = -2
        val ESCAPES = -3
    }

    private class RoleInfoEntry(val node: DataFlowIR.Node? = null, val field: DataFlowIR.Field?)

    private open class RoleInfo {
        val entries = mutableListOf<RoleInfoEntry>()

        open fun add(entry: RoleInfoEntry) = entries.add(entry)
    }

    private class NodeInfo(val depth: Int = Depths.INFINITY) {
        val data = HashMap<Role, RoleInfo>()

        fun add(role: Role, info: RoleInfoEntry?) {
            val entry = data.getOrPut(role, { RoleInfo() })
            if (info != null) entry.add(info)
        }

        fun has(role: Role): Boolean = data[role] != null

        fun escapes() = has(Role.WRITTEN_TO_GLOBAL) || has(Role.THROW_VALUE)

        override fun toString() =
                data.keys.joinToString(separator = "; ", prefix = "Roles: ") { it.toString() }
    }

    private class FunctionAnalysisResult(val function: DataFlowIR.Function,
                                         val nodesRoles: Map<DataFlowIR.Node, NodeInfo>)

    private class IntraproceduralAnalysis(val context: Context,
                                          val moduleDFG: ModuleDFG, val externalModulesDFG: ExternalModulesDFG,
                                          val callGraph: CallGraph) {

        val functions = moduleDFG.functions// TODO: use for cross-module analysis: + externalModulesDFG.functionDFGs

        private fun DataFlowIR.Type.resolved(): DataFlowIR.Type.Declared {
            if (this is DataFlowIR.Type.Declared) return this
            val hash = (this as DataFlowIR.Type.External).hash
            return externalModulesDFG.publicTypes[hash] ?: error("Unable to resolve exported type $hash")
        }

        fun analyze(): Map<DataFlowIR.FunctionSymbol, FunctionAnalysisResult> {
            val nothing = moduleDFG.symbolTable.mapClassReferenceType(context.ir.symbols.nothing.owner).resolved()
            return callGraph.nodes.filter { functions[it.symbol] != null }.associateBy({ it.symbol }) { callGraphNode ->
                val function = functions[callGraphNode.symbol]!!
                val body = function.body
                val nodesRoles = mutableMapOf<DataFlowIR.Node, NodeInfo>()

                fun computeDepths(node: DataFlowIR.Node, depth: Int) {
                    if (node is DataFlowIR.Node.Scope)
                        node.nodes.forEach { computeDepths(it, depth + 1) }
                    else
                        nodesRoles[node] = NodeInfo(depth)
                }
                computeDepths(body.rootScope, -1)

                fun assignRole(node: DataFlowIR.Node, role: Role, infoEntry: RoleInfoEntry?) {
                    if (nodesRoles[node] == null)
                        println("BUGBUGBUG 1: $node ${(node as DataFlowIR.Node.Variable).kind}")
                    nodesRoles[node]!!.add(role, infoEntry)
                }

//                fun checkScopeEscape(from: DataFlowIR.Node, to: DataFlowIR.Node) {
//                    // An edge from -> to.
//                    if (nodeDepths[from] == null)
//                        println("BUGBUGBUG 2: $from")
//                    if (nodeDepths[to] == null)
//                        println("BUGBUGBUG 3: $to")
//                    val fromDepth = nodeDepths[from]!!
//                    val toDepth = nodeDepths[to]!!
//                    if (fromDepth < toDepth)
//                        assignRole(to, Role.WRITTEN_TO_OUTER_SCOPE, null)
//                }

                body.returns.values.forEach { assignRole(it.node, Role.RETURN_VALUE, null /* TODO */) }
                body.throws.values.forEach  { assignRole(it.node, Role.THROW_VALUE,  null /* TODO */) }

                body.forEachNonScopeNode { node ->
                    when (node) {
                        is DataFlowIR.Node.FieldWrite -> {
                            val receiver = node.receiver
                            if (receiver == null) {
                                // Global field.
                                assignRole(node.value.node, Role.WRITTEN_TO_GLOBAL, null /* TODO */)
                            } else {
                                assignRole(receiver.node, Role.WRITE_FIELD, RoleInfoEntry(node.value.node, node.field))
//                                checkScopeEscape(receiver.node, node.value.node)

//                                // TODO: make more precise analysis and differentiate fields from receivers.
//                                // See test escape2.kt, why we need these edges.
//                                assignRole(node.value.node, Role.FIELD_WRITTEN, RoleInfoEntry(receiver.node))
                            }
                        }

                        is DataFlowIR.Node.Singleton -> {
                            val type = node.type.resolved()
                            if (type != nothing)
                                assignRole(node, Role.WRITTEN_TO_GLOBAL, null /* TODO */)
                        }

                        is DataFlowIR.Node.FieldRead -> {
                            val receiver = node.receiver
                            if (receiver == null) {
                                // Global field.
                                assignRole(node, Role.WRITTEN_TO_GLOBAL, null /* TODO */)
                            } else {
                                // Receiver holds reference to all its fields.
                                assignRole(receiver.node, Role.READ_FIELD, RoleInfoEntry(node, node.field))
//                                checkScopeEscape(receiver.node, node)

//                                /*
//                                 * The opposite (a field points to its receiver) is also kind of true.
//                                 * Here is an example why we need these edges:
//                                 *
//                                 * class B
//                                 * class A { val b = B() }
//                                 * fun foo(): B {
//                                 *     val a = A() <- here [a] is created and so does [a.b], therefore they have the same lifetime.
//                                 *     return a.b  <- a.b escapes to return value. If there were no edge from [a.b] to [a],
//                                 *                    then [a] would've been considered local and since [a.b] has the same lifetime as [a],
//                                 *                    [a.b] would be local as well.
//                                 * }
//                                 *
//                                 */
//                                assignRole(node, Role.FIELD_WRITTEN, RoleInfoEntry(receiver.node))
                            }
                        }

                        is DataFlowIR.Node.ArrayWrite -> {
                            assignRole(node.array.node, Role.WRITE_FIELD, RoleInfoEntry(node.value.node, intestinesField))
//                            checkScopeEscape(node.array.node, node.value.node)
                            //assignRole(node.value.node, Role.FIELD_WRITTEN, RoleInfoEntry(node.array.node))
                        }

                        is DataFlowIR.Node.ArrayRead -> {
                            assignRole(node.array.node, Role.READ_FIELD, RoleInfoEntry(node, intestinesField))
//                            checkScopeEscape(node.array.node, node)
                            //assignRole(node, Role.FIELD_WRITTEN, RoleInfoEntry(node.array.node))
                        }

                        is DataFlowIR.Node.Variable -> {
                            for (value in node.values) {
                                assignRole(node, Role.ASSIGNED, RoleInfoEntry(value.node, null))
//                                checkScopeEscape(node, value.node)
                                // A variable is an alias for its possible values - so the edges are undirected.
//                                assignRole(value.node, Role.ALIAS, RoleInfoEntry(node, null))
                            }
                        }
                    }
                }
                FunctionAnalysisResult(function, nodesRoles)
            }
        }
    }

    private inline fun <reified T: Comparable<T>> Array<T>.sortedAndDistinct(): Array<T> {
        this.sort()
        if (this.isEmpty()) return this
        val unique = mutableListOf(this[0])
        for (i in 1 until this.size)
            if (this[i] != this[i - 1])
                unique.add(this[i])
        return unique.toTypedArray()
    }

    private class CompressedPointsToGraph(edges: Array<Edge>) {
        val edges = edges.sortedAndDistinct()

        sealed class NodeKind {
            abstract val absoluteIndex: Int

            object Return : NodeKind() {
                override val absoluteIndex = 0

                override fun equals(other: Any?): Boolean {
                    return other === this
                }

                override fun toString() = "RET"
            }

            class Param(val index: Int) : NodeKind() {
                override val absoluteIndex: Int
                    get() = -1_000_000 + index

                override fun equals(other: Any?): Boolean {
                    return index == (other as? Param)?.index
                }

                override fun toString() = "P$index"
            }

            class Drain(val index: Int) : NodeKind() {
                override val absoluteIndex: Int
                    get() = index + 1

                override fun equals(other: Any?): Boolean {
                    return index == (other as? Drain)?.index
                }

                override fun toString() = "D$index"
            }

            companion object {
                fun parameter(index: Int, total: Int) =
                        if (index == total - 1)
                            Return
                        else
                            Param(index)
            }
        }

        class Node(val kind: NodeKind, val path: Array<DataFlowIR.Field>) : Comparable<Node> {
            override fun compareTo(other: Node): Int {
                if (kind.absoluteIndex != other.kind.absoluteIndex)
                    return kind.absoluteIndex.compareTo(other.kind.absoluteIndex)
                for (i in path.indices) {
                    if (i >= other.path.size)
                        return 1
                    if (path[i].hash != other.path[i].hash)
                        return path[i].hash.compareTo(other.path[i].hash)
                }
                if (path.size < other.path.size) return -1
                return 0
            }

            override fun equals(other: Any?): Boolean {
                if (this === other) return true
                if (other !is Node) return false
                if (kind != other.kind || path.size != other.path.size)
                    return false
                for (i in path.indices)
                    if (path[i] != other.path[i])
                        return false
                return true
            }

            override fun toString() = debugOutput(null)

            fun debugOutput(root: String?): String {
                val result = StringBuilder()
                result.append(root ?: kind.toString())
                path.forEach {
                    result.append('.')
                    result.append(it.name ?: "<no_name@${it.hash}>")
                }
                return result.toString()
            }

            companion object {
                fun parameter(index: Int, total: Int) = Node(NodeKind.parameter(index, total), path = emptyArray())
                fun drain(index: Int) = Node(NodeKind.Drain(index), path = emptyArray())
            }
        }

        class Edge(val from: Node, val to: Node) : Comparable<Edge> {
            override fun compareTo(other: Edge): Int {
                val fromCompareResult = from.compareTo(other.from)
                if (fromCompareResult != 0)
                    return fromCompareResult
                return to.compareTo(other.to)
            }

            override fun equals(other: Any?): Boolean {
                if (other === this) return true
                if (other !is Edge) return false
                return from == other.from && to == other.to
            }

            override fun toString(): String {
                return "$from -> $to"
            }

            companion object {
                fun pointsTo(param1: Int, param2: Int, totalParams: Int, kind: Int): Edge {
                    /*
                     *  kind            edge
                     *   1      p1            -> p2
                     *   2      p1            -> p2.intestines
                     *   3      p1.intestines -> p2
                     *   4      p1.intestines -> p2.intestines
                     */
                    if (kind <= 0 || kind > 4)
                        error("Invalid pointsTo kind: $kind")
                    val from = if (kind < 3)
                        Node.parameter(param1, totalParams)
                    else
                        Node(NodeKind.parameter(param1, totalParams), Array(1) { intestinesField })
                    val to = if (kind % 2 == 1)
                        Node.parameter(param2, totalParams)
                    else
                        Node(NodeKind.parameter(param2, totalParams), Array(1) { intestinesField })
                    return Edge(from, to)
                }
            }
        }
    }

    private class FunctionEscapeAnalysisRezult(
            val numberOfDrains: Int,
            val pointsTo: CompressedPointsToGraph,
            escapes: Array<CompressedPointsToGraph.Node>
    ) {
        val escapes = escapes.sortedAndDistinct()

        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is FunctionEscapeAnalysisRezult) return false

            if (escapes.size != other.escapes.size) return false
            for (i in escapes.indices)
                if (escapes[i] != other.escapes[i]) return false

            if (pointsTo.edges.size != other.pointsTo.edges.size)
                return false
            for (i in pointsTo.edges.indices)
                if (pointsTo.edges[i] != other.pointsTo.edges[i])
                    return false
            return true
        }

        override fun toString(): String {
            val result = StringBuilder()
            result.appendln("PointsTo:")
            pointsTo.edges.forEach { result.appendln("    $it") }
            result.append("Escapes:")
            escapes.forEach {
                result.append(' ')
                result.append(it)
            }
            return result.toString()
        }

        companion object {
            fun fromBits(escapesMask: Int, pointsToMasks: List<Int>): FunctionEscapeAnalysisRezult {
                val paramCount = pointsToMasks.size
                val edges = mutableListOf<CompressedPointsToGraph.Edge>()
                val escapes = mutableListOf<CompressedPointsToGraph.Node>()
                for (param1 in pointsToMasks.indices) {
                    if (escapesMask and (1 shl param1) != 0)
                        escapes.add(CompressedPointsToGraph.Node.parameter(param1, paramCount))
                    val curPointsToMask = pointsToMasks[param1]
                    for (param2 in pointsToMasks.indices) {
                        // Read a nibble at position [param2].
                        val pointsTo = (curPointsToMask shr (4 * param2)) and 15
                        if (pointsTo != 0)
                            edges.add(CompressedPointsToGraph.Edge.pointsTo(param1, param2, paramCount, pointsTo))
                    }
                }
                return FunctionEscapeAnalysisRezult(
                        0, CompressedPointsToGraph(edges.toTypedArray()), escapes.toTypedArray())
            }

            fun optimistic() =
                    FunctionEscapeAnalysisRezult(0, CompressedPointsToGraph(emptyArray()), emptyArray())

            fun pessimistic(numberOfParameters: Int) =
                    FunctionEscapeAnalysisRezult(0, CompressedPointsToGraph(emptyArray()),
                            Array(numberOfParameters + 1) { CompressedPointsToGraph.Node.parameter(it, numberOfParameters + 1) })
        }
    }

    private class InterproceduralAnalysis(
            val context: Context,
            val callGraph: CallGraph,
            val intraproceduralAnalysisResults: Map<DataFlowIR.FunctionSymbol, FunctionAnalysisResult>,
            val externalModulesDFG: ExternalModulesDFG,
            val lifetimes: MutableMap<IrElement, Lifetime>
    ) {

        private fun DataFlowIR.Type.resolved(): DataFlowIR.Type.Declared {
            if (this is DataFlowIR.Type.Declared) return this
            val hash = (this as DataFlowIR.Type.External).hash
            return externalModulesDFG.publicTypes[hash] ?: error("Unable to resolve exported type $hash")
        }

        val escapeAnalysisResults = mutableMapOf<DataFlowIR.FunctionSymbol, FunctionEscapeAnalysisRezult>()

        fun analyze() {
            DEBUG_OUTPUT(0) {
                println("CALL GRAPH")
                callGraph.directEdges.forEach { t, u ->
                    println("    FUN $t")
                    u.callSites.forEach {
                        val label = when {
                            it.isVirtual -> "VIRTUAL"
                            callGraph.directEdges.containsKey(it.actualCallee) -> "LOCAL"
                            else -> "EXTERNAL"
                        }
                        println("        CALLS $label ${it.actualCallee}")
                    }
                    callGraph.reversedEdges[t]!!.forEach {
                        println("        CALLED BY $it")
                    }
                }
            }

            val condensation = DirectedGraphCondensationBuilder(callGraph).build()

            val priorities = mutableMapOf<DataFlowIR.FunctionSymbol.Declared, Int>()
            condensation.topologicalOrder.forEachIndexed { index, multiNode ->
                multiNode.nodes.forEach { priorities[it] = index }
            }

            callGraph.directEdges.forEach { symbol, node ->
                for (callSite in node.callSites) {
                    if (callSite.isVirtual) continue
                    val callee = callSite.actualCallee as? DataFlowIR.FunctionSymbol.Declared ?: continue
                    if (!callGraph.directEdges.containsKey(callee)) continue
                    if (priorities[symbol]!! > priorities[callee]!!)
                        println("BUGBUGBUG: $symbol -> $callee")
                }
            }

            DEBUG_OUTPUT(0) {
                println("CONDENSATION")
                condensation.topologicalOrder.forEach { multiNode ->
                    println("    MULTI-NODE")
                    multiNode.nodes.forEach {
                        println("        $it")
                    }
                }
                println("CONDENSATION(DETAILED)")
                condensation.topologicalOrder.forEach { multiNode ->
                    println("    MULTI-NODE")
                    multiNode.nodes.forEach {
                        println("        $it")
                        callGraph.directEdges[it]!!.callSites
                                .filter { callGraph.directEdges.containsKey(it.actualCallee) }
                                .forEach { println("            CALLS ${it.actualCallee}") }
                        callGraph.reversedEdges[it]!!.forEach {
                            println("            CALLED BY $it")
                        }
                    }
                }
            }

            for (functionSymbol in callGraph.directEdges.keys) {
                if (!intraproceduralAnalysisResults.containsKey(functionSymbol)) continue
//                val numberOfParameters = functionSymbol.parameters.size
//                escapeAnalysisResults[functionSymbol] =
//                        FunctionEscapeAnalysisResult.fromBits(
//                                functionSymbol.escapes ?: 0,
//                                (0..functionSymbol.parameters.size).map { functionSymbol.pointsTo?.elementAtOrNull(it) ?: 0 }
//                        )
//                        FunctionEscapeAnalysisResult(
//                        // Assume no edges at the beginning.
//                        // Then iteratively add needed.
//                        Array(numberOfParameters + 1) { ParameterEscapeAnalysisResult(false, IntArray(0)) }
//                )
                // Assume trivial result at the beginning - then iteratively specify it.
                // Since edges are never deleted once they've been added, this process will end eventually.
                escapeAnalysisResults[functionSymbol] = FunctionEscapeAnalysisRezult.optimistic()
            }

            for (multiNode in condensation.topologicalOrder.reversed())
                analyze(callGraph, multiNode)
        }

        private fun analyze(callGraph: CallGraph, multiNode: DirectedGraphMultiNode<DataFlowIR.FunctionSymbol>) {
            val nodes = multiNode.nodes.filter { intraproceduralAnalysisResults.containsKey(it) }.toMutableSet()
            DEBUG_OUTPUT(0) {
                println("Analyzing multiNode:\n    ${nodes.joinToString("\n   ") { it.toString() }}")
                nodes.forEach { from ->
                    println("DataFlowIR")
                    intraproceduralAnalysisResults[from]!!.function.debugOutput()
                    callGraph.directEdges[from]!!.callSites.forEach { to ->
                        println("CALL")
                        println("   from $from")
                        println("   to ${to.actualCallee}")
                    }
                }
            }

            val pointsToGraphs = nodes.associateBy({ it }, { PointsToGraph(it) })
            val toAnalyze = mutableSetOf<DataFlowIR.FunctionSymbol>()
            toAnalyze.addAll(nodes)
            val numberOfRuns = mutableMapOf<DataFlowIR.FunctionSymbol, Int>()
            nodes.forEach { numberOfRuns[it] = 0 }
            while (toAnalyze.isNotEmpty()) {
                val function = toAnalyze.first()
                toAnalyze.remove(function)
                numberOfRuns[function] = numberOfRuns[function]!! + 1

                DEBUG_OUTPUT(0) { println("Processing function $function") }

                val startResult = escapeAnalysisResults[callGraph.directEdges[function]!!.symbol]!!

                DEBUG_OUTPUT(0) { println("Start escape analysis result:\n$startResult") }

                analyze(callGraph, pointsToGraphs[function]!!, function)
                val endResult = escapeAnalysisResults[callGraph.directEdges[function]!!.symbol]!!
                if (startResult == endResult) {
                    DEBUG_OUTPUT(0) { println("Escape analysis is not changed") }
                } else {
                    DEBUG_OUTPUT(0) { println("Escape analysis was refined:\n$endResult") }

                    if (numberOfRuns[function]!! > 1) {
                        DEBUG_OUTPUT(0) {
                            println("WARNING: Escape analysis for $function seems not to be converging." +
                                    " Assuming conservative results.")
                        }
                        escapeAnalysisResults[callGraph.directEdges[function]!!.symbol] =
                                FunctionEscapeAnalysisRezult.pessimistic(function.parameters.size)
                        nodes.remove(function)
                    }

                    callGraph.reversedEdges[function]?.forEach {
                        if (nodes.contains(it))
                            toAnalyze.add(it)
                    }
                }
            }
//            nodes.forEach {
//                val escapeAnalysisResult = escapeAnalysisResults[it]!!
//                var escapes = 0
//                val pointsTo = escapeAnalysisResult.parameters.withIndex().map { (index, parameterEAResult) ->
//                    if (parameterEAResult.escapes)
//                        escapes = escapes or (1 shl index)
//                    var pointsToMask = 0
//                    parameterEAResult.pointsTo.forEach {
//                        pointsToMask = pointsToMask or (1 shl it)
//                    }
//                    pointsToMask
//                }.toIntArray()
//                it.escapes = escapes
//                it.pointsTo = pointsTo
//            }
            for (graph in pointsToGraphs.values) {
                for (node in graph.nodes.keys) {
                    val ir = when (node) {
                        is DataFlowIR.Node.Call -> node.irCallSite
                        is DataFlowIR.Node.ArrayRead -> node.irCallSite
                        is DataFlowIR.Node.FieldRead -> node.ir
                        else -> null
                    }
                    ir?.let {
                        var lifetime = graph.lifetimeOf(node)
                        if (lifetime == Lifetime.STACK && node is DataFlowIR.Node.NewObject) {
                            val constructedType = node.constructedType.resolved()
                            constructedType.irClass?.let { irClass ->
                                if (irClass.isArray) {
                                    lifetime = Lifetime.GLOBAL
//                                    if (!irClass.isArrayWithFixedSizeItems) // TODO: Support array of references.
//                                        lifetime = Lifetime.LOCAL
//                                    else {
//                                        val sizeArgument = node.arguments.first().node
//                                        val arraySize = findOutArraySize(sizeArgument)
//                                        if (arraySize == null || arraySize > stackAllocationArraySizeLimit)
//                                            lifetime = Lifetime.LOCAL
//                                    }
                                }
                            }
                        }
                        if (lifetime == Lifetime.LOCAL) {
                            // TODO: Support Lifetime.LOCAL - requires arenas.
                            lifetime = Lifetime.GLOBAL
                        }
//                        if (graph.functionSymbol.name?.contains("bar") != true
//                                && graph.functionSymbol.name?.contains("foo") != true)
//                            lifetime = Lifetime.GLOBAL
                        lifetimes.put(it, lifetime)
                    }
                }
            }
        }

        // TODO: replace into KonanConfigKeys?
        private val stackAllocationArraySizeLimit = 64

        private fun findOutArraySize(node: DataFlowIR.Node): Int? {
            if (node is DataFlowIR.Node.SimpleConst<*>) {
                return node.value as? Int
            }
            if (node is DataFlowIR.Node.Variable) {
                // In case of several possible values, it's unknown what is used.
                // TODO: if all values are constants which are less limit?
                if (node.values.size == 1) {
                    return findOutArraySize(node.values.first().node)
                }
            }
            return null
        }

        private fun analyze(callGraph: CallGraph, pointsToGraph: PointsToGraph, function: DataFlowIR.FunctionSymbol) {
//            if (function.irFunction?.file != context.ir.symbols.entryPoint?.owner?.file)
//                return
            DEBUG_OUTPUT(0) {
                println("Before calls analysis")
                pointsToGraph.print()
                //if (function.name?.contains("foo") == true)
                    pointsToGraph.print_digraph()
            }

            callGraph.directEdges[function]!!.callSites.forEach {
                val callee = it.actualCallee
                val calleeEAResult = if (it.isVirtual)
                                         getExternalFunctionEAResult(it)
                                     else
                                         callGraph.directEdges[callee]?.let { escapeAnalysisResults[it.symbol]!! }
                                             ?: getExternalFunctionEAResult(it)
                pointsToGraph.processCall(it, calleeEAResult)
            }

            DEBUG_OUTPUT(0) {
                println("After calls analysis")
                pointsToGraph.print()
                //if (function.name?.contains("foo") == true)
                    pointsToGraph.print_digraph()
            }

            // Build transitive closure.
            val eaResult = pointsToGraph.buildClosure()

            DEBUG_OUTPUT(0) {
                println("After closure building")
                pointsToGraph.print()
                //if (function.name?.contains("foo") == true)
                    pointsToGraph.print_digraph()
            }

            escapeAnalysisResults[callGraph.directEdges[function]!!.symbol] = eaResult
        }

        private fun DataFlowIR.FunctionSymbol.resolved(): DataFlowIR.FunctionSymbol {
            if (this is DataFlowIR.FunctionSymbol.External)
                return externalModulesDFG.publicFunctions[this.hash] ?: this
            return this
        }

        private fun getExternalFunctionEAResult(callSite: CallGraphNode.CallSite): FunctionEscapeAnalysisRezult {
            val callee = callSite.actualCallee.resolved()

            val calleeEAResult = if (callSite.isVirtual) {

                DEBUG_OUTPUT(0) { println("A virtual call: $callee") }

                FunctionEscapeAnalysisRezult.pessimistic(callee.parameters.size)
            } else {

                DEBUG_OUTPUT(0) { println("An external call: $callee") }

                FunctionEscapeAnalysisRezult.fromBits(
                        callee.escapes ?: 0,
                        (0..callee.parameters.size).map { callee.pointsTo?.elementAtOrNull(it) ?: 0 }
                )
            }

            DEBUG_OUTPUT(0) {
                println("Escape analysis result")
                println(calleeEAResult.toString())
                println()
            }

            return calleeEAResult
        }

        private enum class PointsToGraphNodeKind {
            STACK,
            LOCAL,
            PARAMETER,
            RETURN_VALUE,
            ESCAPES
        }

        private class PointsToGraphEdge(var node: PointsToGraphNode, val field: DataFlowIR.Field?) {
            val isAssignment get() = field == null
        }

        private class PointsToGraphNode(val nodeInfo: NodeInfo, val node: DataFlowIR.Node?) {
            val edges = mutableListOf<PointsToGraphEdge>()
            val reversedEdges = mutableListOf<PointsToGraphEdge>()

            fun addAssignmentEdge(to: PointsToGraphNode) {
                edges += PointsToGraphEdge(to, null)
                to.reversedEdges += PointsToGraphEdge(this, null)
            }

            private val fields = mutableMapOf<DataFlowIR.Field, PointsToGraphNode>()

            fun gotoField(field: DataFlowIR.Field, graph: PointsToGraph) = fields.getOrPut(field) {
                val node = PointsToGraphNode(NodeInfo(), null)
                edges += PointsToGraphEdge(node, field)
                graph.allNodes += node
                node
            }

            var depth = when {
                nodeInfo.escapes() -> Depths.ESCAPES
                node is DataFlowIR.Node.Parameter -> Depths.PARAMETER
                nodeInfo.has(Role.RETURN_VALUE) -> Depths.RETURN_VALUE
                else -> nodeInfo.depth
            }

            val kind get() = when {
                depth == Depths.ESCAPES -> PointsToGraphNodeKind.ESCAPES
                depth == Depths.PARAMETER -> PointsToGraphNodeKind.PARAMETER
                depth == Depths.RETURN_VALUE -> PointsToGraphNodeKind.RETURN_VALUE
                depth != nodeInfo.depth -> PointsToGraphNodeKind.LOCAL
                else -> PointsToGraphNodeKind.STACK
            }

            var drain: PointsToGraphNode? = null

            val actualDrain: PointsToGraphNode?
                get() {
                    var result = drain ?: return null
                    while (result != result.drain)
                        result = result.drain!!
                    return result
                }

            val beingReturned get() = nodeInfo.has(Role.RETURN_VALUE)

//            val parametersPointingOnUs = mutableSetOf<Int>()
//
//            fun addIncomingParameter(parameter: Int) {
//                if (depth == ESCAPES) return
//                parametersPointingOnUs += parameter
//            }

        }

        private inner class PointsToGraph(val functionSymbol: DataFlowIR.FunctionSymbol) {

            val functionAnalysisResult = intraproceduralAnalysisResults[functionSymbol]!!
            val nodes = mutableMapOf<DataFlowIR.Node, PointsToGraphNode>()

            val returnsNode = PointsToGraphNode(NodeInfo().also { it.data[Role.RETURN_VALUE] = RoleInfo() }, null)
            val allNodes = mutableListOf(returnsNode)

            val ids = if (DEBUG > 0)
                (listOf(functionAnalysisResult.function.body.rootScope)
                        + functionAnalysisResult.function.body.allScopes.flatMap { it.nodes }
                        )
                        .withIndex().associateBy({ it.value }, { it.index })
            else null

            fun lifetimeOf(node: DataFlowIR.Node) = lifetimeOf(nodes[node]!!)

            fun lifetimeOf(node: PointsToGraphNode) =
                when (node.kind) {
                    PointsToGraphNodeKind.ESCAPES -> Lifetime.GLOBAL
                    PointsToGraphNodeKind.PARAMETER -> Lifetime.GLOBAL

                    PointsToGraphNodeKind.STACK -> {
                        // A value doesn't escape from its scope - it can be allocated on the stack.
                        Lifetime.STACK
                    }

                    PointsToGraphNodeKind.LOCAL -> {
                        // A value is neither stored into a global nor into any parameter nor into the return value -
                        // it can be allocated locally.
                        Lifetime.LOCAL
                    }

                    PointsToGraphNodeKind.RETURN_VALUE -> {
                        when {
                            // If a value is explicitly returned.
                            node.node?.let { it in returnValues } == true -> Lifetime.RETURN_VALUE

                            else -> Lifetime.GLOBAL
//                            // A value is stored into a field of the return value.
//                            else -> Lifetime.INDIRECT_RETURN_VALUE
                        }
                    }
                }

            private val returnValues: Set<DataFlowIR.Node>

            init {
                DEBUG_OUTPUT(0) {
                    println("Building points-to graph for function $functionSymbol")
                    println("Results of preliminary function analysis")
                }

                functionAnalysisResult.nodesRoles.forEach { (node, roles) ->
                    DEBUG_OUTPUT(0) { println("NODE ${nodeToString(node)}: $roles") }

                    nodes[node] = PointsToGraphNode(roles, node).also { allNodes += it }
                }

                val returnValues = mutableListOf<DataFlowIR.Node>()
                functionAnalysisResult.nodesRoles.forEach { (node, roles) ->
                    val ptgNode = nodes[node]!!
                    addEdges(ptgNode, roles)
                    if (ptgNode.beingReturned) {
                        returnsNode.gotoField(returnsValueField, this).addAssignmentEdge(ptgNode)
                        returnValues += node
                    }
                }

                this.returnValues = returnValues.toSet()

                val escapes = functionSymbol.escapes
                if (escapes != null) {
                    // Parameters are declared in the root scope
                    val parameters = functionAnalysisResult.function.body.rootScope.nodes.filterIsInstance<DataFlowIR.Node.Parameter>()
                    for (parameter in parameters)
                        if (escapes and (1 shl parameter.index) != 0)
                            nodes[parameter]!!.depth = Depths.ESCAPES
                    if (escapes and (1 shl parameters.size) != 0)
                        returnsNode.depth = Depths.ESCAPES
                }
            }

//            private val returnValues = nodes.filter { it.value.beingReturned }
//                                            .map { it.key }
//                                            .toSet()

            private fun addEdges(from: PointsToGraphNode, roles: NodeInfo) {
                val assigned = roles.data[Role.ASSIGNED]
//                if (assigned != null && (from.node as? DataFlowIR.Node.Variable == null))
//                    error("Assigning is expected only for variables but was: ${from.node}")
                assigned?.entries?.forEach {
                    val to = nodes[it.node!!]!!
                    from.addAssignmentEdge(to)
                }
                roles.data[Role.WRITE_FIELD]?.entries?.forEach { roleInfo ->
                    val value = nodes[roleInfo.node!!]!!
                    val field = roleInfo.field!!
                    from.gotoField(field, this).addAssignmentEdge(value)
                }
                roles.data[Role.READ_FIELD]?.entries?.forEach { roleInfo ->
                    val result = nodes[roleInfo.node!!]!!
                    val field = roleInfo.field!!
                    result.addAssignmentEdge(from.gotoField(field, this))
                }
            }

            private fun nodeToStringWhole(node: DataFlowIR.Node) = DataFlowIR.Function.nodeToString(node, ids!!)

            private fun nodeToString(node: DataFlowIR.Node) = ids!![node].toString()

            fun print() {
                println("POINTS-TO GRAPH")
                println("NODES")
                val tempIds = mutableMapOf<PointsToGraphNode, Int>()
                var tempIndex = 0
                allNodes.forEach {
                    if (it.node == null)
                        tempIds[it] = tempIndex++
                }
                allNodes.forEach {
                    val tempId = tempIds[it]
                    println("    ${lifetimeOf(it)} ${it.depth} ${it.node?.let { nodeToString(it) } ?: "t$tempId"}")
                    print(it.node?.let { nodeToStringWhole(it) } ?: "        t$tempId\n")
                }
//                println("EDGES")
//                allNodes.forEach {
//                    it.edgez.forEach {
//                        println("    FROM ${nodeToString(t)}")
//                        println("    TO ${nodeToString(it)}")
//                    }
//                }
            }

            fun print_digraph(
                    nodeFilter: (PointsToGraphNode) -> Boolean = { true },
                    nodeLabel: ((PointsToGraphNode) -> String)? = null
            ) {
                println("digraph {")
                val ids = ids!!
                val tempIds = mutableMapOf<PointsToGraphNode, Int>()
                var tempIndex = 0
                allNodes.forEach {
                    if (it.node == null)
                        tempIds[it] = tempIndex++
                }

                fun PointsToGraphNode.format() =
                        (nodeLabel?.invoke(this) ?:
                        (if (drain == this) "d" else "") + (node?.let { "n${ids[it]!!}" } ?: "t${tempIds[this]}")) +
                                "[d=$depth]"

                for (from in allNodes) {
                    if (!nodeFilter(from)) continue
                    for (it in from.edges) {
                        val to = it.node
                        if (!nodeFilter(to)) continue
                        val field = it.field
                        if (field == null)
                            println("    \"${from.format()}\" -> \"${to.format()}\";")
                        else
                            println("    \"${from.format()}\" -> \"${to.format()}\" [ label=\"${field.name}\"];")
                    }
                }
                println("}")
            }

            fun processCall(callSite: CallGraphNode.CallSite, calleeEscapeAnalysisResult: FunctionEscapeAnalysisRezult) {
                val call = callSite.call
                DEBUG_OUTPUT(0) {
                    println("Processing callSite")
                    println(nodeToStringWhole(call))
                    println("Actual callee: ${callSite.actualCallee}")
                    println("Callee escape analysis result:")
                    println(calleeEscapeAnalysisResult.toString())
                }

                val arguments = if (call is DataFlowIR.Node.NewObject) {
                                    (0..call.arguments.size).map {
                                        if (it == 0) call else call.arguments[it - 1].node
                                    }
                                } else {
                                    (0..call.arguments.size).map {
                                        if (it < call.arguments.size) call.arguments[it].node else call
                                    }
                                }

                val drains = Array(calleeEscapeAnalysisResult.numberOfDrains) {
                    PointsToGraphNode(NodeInfo(), null).also { allNodes += it }
                }

                fun mapNode(compressedNode: CompressedPointsToGraph.Node): Pair<DataFlowIR.Node?, PointsToGraphNode?> {
                    val (arg, rootNode) = when (val kind = compressedNode.kind) {
                        CompressedPointsToGraph.NodeKind.Return -> arguments.last() to nodes[arguments.last()]
                        is CompressedPointsToGraph.NodeKind.Param -> arguments[kind.index] to nodes[arguments[kind.index]]
                        is CompressedPointsToGraph.NodeKind.Drain -> null to drains[kind.index]
                    }
                    if (rootNode == null)
                        return arg to rootNode
                    val path = compressedNode.path
                    var node: PointsToGraphNode = rootNode
                    for (i in path.indices) {
                        val field = path[i]
                        node = when (field) {
                            returnsValueField -> node
                            else -> node.gotoField(field, this)
                        }
                    }
                    return arg to node
                }

                calleeEscapeAnalysisResult.escapes.forEach { escapingNode ->
                    val (arg, node) = mapNode(escapingNode)
                    if (node == null) {
                        DEBUG_OUTPUT(0) { println("WARNING: There is no node ${nodeToString(arg!!)}") }
                        return@forEach
                    }
                    node.depth = Depths.ESCAPES

                    DEBUG_OUTPUT(0) {
                        println("Node ${escapingNode.debugOutput(arg?.let { nodeToString(it) })} escapes")
                    }
                }

                calleeEscapeAnalysisResult.pointsTo.edges.forEach { edge ->
                    val (fromArg, fromNode) = mapNode(edge.from)
                    if (fromNode == null) {
                        DEBUG_OUTPUT(0) { println("WARNING: There is no node ${nodeToString(fromArg!!)}") }
                        return@forEach
                    }
                    val (toArg, toNode) = mapNode(edge.to)
                    if (toNode == null) {
                        DEBUG_OUTPUT(0) { println("WARNING: There is no node ${nodeToString(toArg!!)}") }
                        return@forEach
                    }
                    fromNode.addAssignmentEdge(toNode)

                    DEBUG_OUTPUT(0) {
                        println("Adding edge")
                        println("    FROM ${edge.from.debugOutput(fromArg?.let { nodeToString(it) })}")
                        println("    TO ${edge.to.debugOutput(toArg?.let { nodeToString(it) })}")
                    }
                }
            }

            fun buildClosure(): FunctionEscapeAnalysisRezult {
                DEBUG_OUTPUT(0) {
                    println("BUILDING CLOSURE")
                    println("Return values:")
                    returnValues.forEach {
                        println("    ${nodeToString(it)}")
                    }
                }

                val visited = mutableSetOf<PointsToGraphNode>()
                val drains = mutableListOf<PointsToGraphNode>()
                val createdDrains = mutableSetOf<PointsToGraphNode>()
                // First phase.
                for (node in allNodes) {
                    if (node in visited) continue
                    val component = mutableListOf<PointsToGraphNode>()
                    buildComponent(node, visited, component)
                    val drain = trySelectDrain(component)
                            ?: PointsToGraphNode(NodeInfo(), null).also { createdDrains += it }
                    drains += drain
                    drain.drain = drain
                    component.forEach {
                        if (it == drain) return@forEach
                        it.drain = drain
                        val assignmentEdges = mutableListOf<PointsToGraphEdge>()
                        for (edge in it.edges) {
                            if (edge.isAssignment)
                                assignmentEdges += edge
                            else
                                drain.edges += edge
                        }
                        it.edges.clear()
                        it.edges += assignmentEdges
                    }
                }

                // Second phase.
                // TODO: This looks very similar to the system of disjoint sets algorithm.
                while (true) {
                    val toMerge = mutableListOf<Pair<PointsToGraphNode, PointsToGraphNode>>()
                    for (drain in drains) {
                        val fields = drain.edges.groupBy { edge ->
                            edge.field ?: error("A drain cannot have outgoing assignment edges")
                        }
                        for (nodes in fields.values) {
                            if (nodes.size == 1) continue
                            for (i in nodes.indices) {
                                val firstNode = nodes[i].node
                                val secondNode = if (i == nodes.size - 1) nodes[0].node else nodes[i + 1].node
                                if (firstNode.actualDrain != secondNode.actualDrain)
                                    toMerge += Pair(firstNode, secondNode)
//                                firstNode.addAssignmentEdge(secondNode)
                            }
//                            for (i in nodes.indices) {
//                                for (j in i + 1 until nodes.size) {
//                                    val firstNode = nodes[i].node
//                                    val secondNode = nodes[j].node
//                                    if (firstNode.actualDrain != secondNode.actualDrain)
//                                        toMerge += Pair(firstNode, secondNode)
////                                firstNode.addAssignmentEdge(secondNode)
//                                }
//                            }
                        }
//                        val fields = mutableMapOf<DataFlowIR.Field, PointsToGraphNode>()
//                        for (edge in drain.edgez) {
//                            val field = edge.field ?: error("A drain cannot have outgoing assignment edges")
//                            val fieldNode = fields[field]
//                            val node = edge.node
//                            if (fieldNode == null)
//                                fields[field] = node
//                            else {
//                                if (fieldNode.actualDrain != node.actualDrain) {
//                                    toMerge += Pair(fieldNode, node)
////                                    fieldNode.addAssignmentEdge(node)
////                                    node.addAssignmentEdge(fieldNode)
//                                }
//                            }
//                        }
                    }
                    if (toMerge.isEmpty()) break
                    val possibleDrains = mutableListOf<PointsToGraphNode>()
                    for ((first, second) in toMerge) {
                        val firstDrain = first.actualDrain!!
                        val secondDrain = second.actualDrain!!
                        when {
                            firstDrain == secondDrain -> continue
                            firstDrain in createdDrains -> {
                                secondDrain.drain = firstDrain
                                firstDrain.edges += secondDrain.edges
                                secondDrain.edges.clear()
                                possibleDrains += firstDrain
                            }
                            secondDrain in createdDrains -> {
                                firstDrain.drain = secondDrain
                                secondDrain.edges += firstDrain.edges
                                firstDrain.edges.clear()
                                possibleDrains += secondDrain
                            }
                            else -> {
                                // Create a new drain in order to not create false constraints.
                                val newDrain = PointsToGraphNode(NodeInfo(), null).also { createdDrains += it }
                                firstDrain.drain = newDrain
                                secondDrain.drain = newDrain
                                newDrain.drain = newDrain
                                newDrain.edges += firstDrain.edges
                                newDrain.edges += secondDrain.edges
                                firstDrain.edges.clear()
                                secondDrain.edges.clear()
                                possibleDrains += newDrain
                            }
                        }
                    }
                    drains.clear()
                    for (drain in possibleDrains)
                        if (drain.drain == drain)
                            drains += drain
                }
                allNodes += createdDrains

                // Third phase.
                drains.clear()
                for (node in allNodes) {
                    if (node.actualDrain == node)
                        drains += node
                }

                // TODO: remove.
                for (drain in drains) {
                    val fields = mutableMapOf<DataFlowIR.Field, PointsToGraphNode>()
                    for (edge in drain.edges) {
                        val field = edge.field ?: error("A drain cannot have outgoing assignment edges!")
                        val fieldNode = fields[field]
                        val node = edge.node.actualDrain!!
                        if (fieldNode == null)
                            fields[field] = node
                        else {
                            if (fieldNode != node)
                                error("BUGBUGBUG")
                        }
                    }
                }

                // Merge together multi-edges.
                for (drain in drains) {
                    val actualDrain = drain.actualDrain!!
                    val fields = actualDrain.edges.groupBy { edge ->
                        edge.field ?: error("A drain cannot have outgoing assignment edges")
                    }
                    actualDrain.edges.clear()
                    for (nodes in fields.values) {
                        if (nodes.size == 1) {
                            actualDrain.edges += nodes[0]
                            continue
                        }
                        val nextDrain = nodes.atMostOne { it.node.actualDrain == it.node }?.node?.actualDrain
                        if (nextDrain != null) {
                            val newDrain = PointsToGraphNode(NodeInfo(), null).also { allNodes += it }
                            nextDrain.drain = newDrain
                            newDrain.drain = newDrain
                            newDrain.edges += nextDrain.edges
                            nextDrain.edges.clear()
                        }
                        val mergedNode = PointsToGraphNode(NodeInfo(), null).also { allNodes += it }
                        nodes.forEach {
                            mergedNode.addAssignmentEdge(it.node)
                            it.node.addAssignmentEdge(mergedNode)
                        }
                        actualDrain.edges += PointsToGraphEdge(mergedNode, nodes[0].field)
                        mergedNode.drain = nodes[0].node.drain

//                        for (i in nodes.indices) {
//                            val firstNode = nodes[i].node
//                            val secondNode = if (i == nodes.size - 1) nodes[0].node else nodes[i + 1].node
//                            firstNode.addAssignmentEdge(secondNode)
//                        }
                    }
                }

                // Third phase.
                drains.clear()
                for (node in allNodes) {
                    val drain = node.actualDrain!!
                    node.drain = drain
                    if (node == drain)
                        drains += drain
                    else
                        node.addAssignmentEdge(drain)
                }

                DEBUG_OUTPUT(0) {
                    print_digraph()
                }

                propagateLifetimes()

                val parameters = arrayOfNulls<PointsToGraphNode>(functionSymbol.parameters.size + 1)
                // Parameters are declared in the root scope.
                functionAnalysisResult.function.body.rootScope.nodes
                        .filterIsInstance<DataFlowIR.Node.Parameter>()
                        .forEach { parameters[it.index] = nodes[it]!! }
                parameters[functionSymbol.parameters.size] = returnsNode

                val interestingDrains = mutableSetOf<PointsToGraphNode>()
                for (param in parameters) {
                    val drain = param!!.drain!!
                    if (drain !in interestingDrains)
                        findReachableDrains(drain, interestingDrains)
                }
                val reversedEdges = interestingDrains.associateWith {
                    mutableListOf<Pair<PointsToGraphNode, PointsToGraphEdge>>()
                }
                for (drain in interestingDrains) {
                    for (edge in drain.edges)
                        reversedEdges[edge.node.drain!!]!! += drain to edge
                }
                val parameterDrains = parameters.map { it!!.drain!! }.toSet()
                // TODO: rewrite using priority queue.
                while (true) {
                    val nonInterestingDrains = mutableListOf<PointsToGraphNode>()
                    for (drain in interestingDrains) {
                        if (drain.edges.any { it.node.drain in interestingDrains }) continue
                        val incomingEdges = reversedEdges[drain]!!
                        if (incomingEdges.isEmpty()) {
                            if (drain !in parameterDrains)
                                error("A drain with no incoming edges")
                            if (parameters.all { it!!.drain != drain || it.depth != Depths.ESCAPES })
                                nonInterestingDrains += drain
                            continue
                        }
                        if (drain in parameterDrains)
                            continue
                        //if (incomingEdges.size == 1 && incomingEdges[0].node.depth != Depths.ESCAPES)
                        if (incomingEdges.size == 1
                                && incomingEdges[0].let { (node, edge) ->
                                    node.depth == Depths.ESCAPES || edge.node.depth != Depths.ESCAPES
                                }
                        ) {
                            nonInterestingDrains += drain
                        }
                    }
                    if (nonInterestingDrains.isEmpty()) break
                    for (drain in nonInterestingDrains)
                        interestingDrains.remove(drain)
                }
                //println("ZZZ: ${interestingDrains.size}")
//                val parameterDrains = parameters.map { it!!.drain!! }.toSet()
//                val visitedDrains = mutableSetOf<PointsToGraphNode>()
//                val interestingDrains = mutableSetOf<PointsToGraphNode>()
//                for (param in parameters) {
//                    val drain = param!!.drain!!
//                    if (drain !in visitedDrains)
//                        findInterestingDrains(drain, parameterDrains, visitedDrains, interestingDrains)
//                }

                val nodeIds = mutableMapOf<PointsToGraphNode, CompressedPointsToGraph.Node>()
                for (index in parameters.indices)
                    nodeIds[parameters[index]!!] = CompressedPointsToGraph.Node.parameter(index, parameters.size)

                var drainIndex = 0
                var front = parameters.map { it!! }
                while (front.isNotEmpty()) {
                    //println("QZZ: ${front.size}")
                    val nextFront = mutableSetOf<PointsToGraphNode>()
                    for (node in front) {
                        paintNodes(node, nodeIds[node]!!.kind, mutableListOf(),
                                interestingDrains, nodeIds, nextFront)
                    }
                    front = nextFront.filter { nodeIds[it] == null }.toList()
                    for (node in front)
                        nodeIds[node] = CompressedPointsToGraph.Node.drain(drainIndex++)
                }
//                val additionalDrains = mutableListOf<PointsToGraphNode>()
//                for (node in allNodes) {
//                    if (nodeIds[node] != null) continue
//                    val drain = node.drain!!
//                    if (drain !in interestingDrains) continue
//                    if (nodeIds[drain] != null) continue
//
//                    if (findReferencing(node).count { nodeIds[it] != null } > 1) {
//                        // The drain has been optimized out but a node referenced by some interesting nodes
//                        // has been found - save it as an additional drain.
//                        nodeIds[node] = CompressedPointsToGraph.Node.drain(drainIndex++)
//                        additionalDrains += node
//                    }
//                }

                buildComponentsClosures(nodeIds)
//                // Turn them into drains.
//                for (node in additionalDrains)
//                    node.edges.clear()

                // Here we try to find this subgraph within one component: [v -> d; w -> d; v !-> w; w !-> v].
                // In most cases such a node [d] is just the drain of the component,
                // but it may have been optimized away.
                val connectedNodes = mutableSetOf<Pair<PointsToGraphNode, PointsToGraphNode>>()
                val additionalDrains = mutableListOf<PointsToGraphNode>()
                for (node in allNodes) {
                    if (nodeIds[node] != null) continue
                    val drain = node.drain!!
                    if (drain !in interestingDrains) continue
                    if (nodeIds[drain] != null) continue
                    val referencingNodes = findReferencing(node).toList()
                    for (i in referencingNodes.indices) {
                        val firstNode = referencingNodes[i]
                        if (nodeIds[firstNode] == null) continue
                        for (j in i + 1 until referencingNodes.size) {
                            val secondNode = referencingNodes[j]
                            if (nodeIds[secondNode] == null) continue
                            val pair = Pair(firstNode, secondNode)
                            if (pair in connectedNodes) continue
                            if (firstNode.edges.any { it.node == secondNode }
                                    || secondNode.edges.any { it.node == firstNode })
                                continue
                            val additionalDrain = PointsToGraphNode(NodeInfo(), null).also { additionalDrains += it }
                            // For consistency.
                            additionalDrain.depth = min(firstNode.depth, secondNode.depth)
                            firstNode.addAssignmentEdge(additionalDrain)
                            secondNode.addAssignmentEdge(additionalDrain)
                            nodeIds[additionalDrain] = CompressedPointsToGraph.Node.drain(drainIndex++)
                            connectedNodes.add(pair)
                            connectedNodes.add(Pair(secondNode, firstNode))
                        }
                    }
                }
                allNodes += additionalDrains

                DEBUG_OUTPUT(0) {
                    //if (functionSymbol.name?.contains("foo") == true)
                    print_digraph({ nodeIds[it] != null }, { nodeIds[it].toString() })
                }

                // TODO: Remove redundant edges.
                val compressedEdges = mutableListOf<CompressedPointsToGraph.Edge>()
                val escapingNodes = mutableListOf<CompressedPointsToGraph.Node>()
                for (from in allNodes) {
                    val fromCompressedNode = nodeIds[from] ?: continue
                    if (from.depth == Depths.ESCAPES)
                        escapingNodes += fromCompressedNode
                    for (edge in from.edges) {
                        if (!edge.isAssignment) continue
                        val toCompressedNode = nodeIds[edge.node] ?: continue
                        compressedEdges += CompressedPointsToGraph.Edge(fromCompressedNode, toCompressedNode)
                    }
                }

                return FunctionEscapeAnalysisRezult(
                        drainIndex,
                        CompressedPointsToGraph(compressedEdges.toTypedArray()),
                        //emptyArray()
                        escapingNodes.toTypedArray()
                )
            }

            private fun findReferencing(node: PointsToGraphNode, visited: MutableSet<PointsToGraphNode>) {
                visited += node
                for (edge in node.reversedEdges) {
                    val nextNode = edge.node
                    if (edge.isAssignment && nextNode !in visited)
                        findReferencing(nextNode, visited)
                }
            }

            private fun findReferencing(node: PointsToGraphNode): Set<PointsToGraphNode> {
                val visited = mutableSetOf<PointsToGraphNode>()
                findReferencing(node, visited)
                return visited
            }

            private fun trySelectDrain(component: MutableList<PointsToGraphNode>) =
                    component.firstOrNull { node ->
                        if (node.edges.any { edge -> edge.isAssignment })
                            false
                        else
                            findReferencing(node).size == component.size
                    }

            private fun buildComponent(
                    node: PointsToGraphNode,
                    visited: MutableSet<PointsToGraphNode>,
                    component: MutableList<PointsToGraphNode>
            ) {
                visited += node
                component += node
                for (edge in node.edges) {
                    if (edge.isAssignment && edge.node !in visited)
                        buildComponent(edge.node, visited, component)
                }
                for (edge in node.reversedEdges) {
                    if (edge.isAssignment && edge.node !in visited)
                        buildComponent(edge.node, visited, component)
                }
            }

            private fun findReachable(node: PointsToGraphNode, visited: MutableSet<PointsToGraphNode>) {
                visited += node
                node.edges.forEach {
                    val next = it.node
                    if (it.isAssignment && next !in visited)
                        findReachable(next, visited)
                }
            }

            private fun buildComponentsClosures(nodeIds: MutableMap<PointsToGraphNode, CompressedPointsToGraph.Node>) {
                for (node in allNodes) {
                    if (node !in nodeIds) continue
                    val visited = mutableSetOf<PointsToGraphNode>()
                    findReachable(node, visited)
                    for (edge in node.edges) {
                        if (edge.isAssignment)
                            visited.remove(edge.node)
                    }
                    visited.remove(node)
                    for (reachable in visited)
                        if (reachable in nodeIds)
                            node.addAssignmentEdge(reachable)
                }
            }

            private fun propagateLifetimes() {
                val visited = mutableSetOf<PointsToGraphNode>()

                fun propagate(node: PointsToGraphNode) {
                    visited += node
                    val depth = node.depth
                    for (edge in node.edges) {
                        val nextNode = edge.node
                        if (nextNode !in visited && nextNode.depth >= depth) {
                            nextNode.depth = depth
                            propagate(nextNode)
                        }
                    }
                }

                for (node in allNodes.sortedBy { it.depth }) {
                    if (node !in visited)
                        propagate(node)
                }
            }

            private fun findReachableDrains(drain: PointsToGraphNode, visitedDrains: MutableSet<PointsToGraphNode>) {
                visitedDrains += drain
                for (edge in drain.edges) {
                    if (edge.isAssignment)
                        error("A drain cannot have outgoing assignment edges")
                    val nextDrain = edge.node.drain!!
                    if (nextDrain !in visitedDrains)
                        findReachableDrains(nextDrain, visitedDrains)
                }
            }

            private fun paintNodes(
                    node: PointsToGraphNode, kind: CompressedPointsToGraph.NodeKind,
                    path: MutableList<DataFlowIR.Field>,
                    interestingDrains: MutableSet<PointsToGraphNode>,
                    nodeIds: MutableMap<PointsToGraphNode, CompressedPointsToGraph.Node>,
                    seenNotPaintedDrains: MutableSet<PointsToGraphNode>
            ) {
                val drain = node.drain!!
                if (node != drain) {
                    if (nodeIds[drain] == null
                            // A little optimization.
                            && (drain.edges.any { it.node.drain in interestingDrains }
                                    || drain.node != null))
                        seenNotPaintedDrains += drain
                    return
                }
//                val multiEdgeCounts = mutableMapOf<DataFlowIR.Field, Int>()
//                for (edge in drain.edgez) {
//                    // A drain only has outgoing field edges.
//                    val field = edge.field ?: error("A drain cannot have outgoing assignment edges")
//                    multiEdgeCounts[field] = 1 + (multiEdgeCounts[field] ?: 0)
//                }
//                val multiEdgeIndices = mutableMapOf<DataFlowIR.Field, Int>()
//                for (field in multiEdgeCounts.keys)
//                    multiEdgeIndices[field] = 0
                for (edge in drain.edges) {
                    val field = edge.field!!
                    val nextNode = edge.node
                    val nextDrain = nextNode.drain!!
                    if (nextDrain in interestingDrains) {
                        if (nodeIds[nextNode] != null)
                            error("Expected only one incoming field edge")
//                        val multiEdgeIndex =
//                                if (nextNode == nextDrain || multiEdgeCounts[field] == 1)
//                                    -1
//                                else
//                                    multiEdgeIndices[field]!!.also { multiEdgeIndices[field] = it + 1 }
                        path.push(field)
                        nodeIds[nextNode] = CompressedPointsToGraph.Node(kind, path.toTypedArray())
                        paintNodes(nextNode, kind, path, interestingDrains, nodeIds, seenNotPaintedDrains)
                        path.pop()
                    }
                }
            }

        }
    }

    fun computeLifetimes(context: Context, moduleDFG: ModuleDFG, externalModulesDFG: ExternalModulesDFG,
                         callGraph: CallGraph, lifetimes: MutableMap<IrElement, Lifetime>) {
        assert(lifetimes.isEmpty())

        val intraproceduralAnalysisResult =
                IntraproceduralAnalysis(context, moduleDFG, externalModulesDFG, callGraph).analyze()
        InterproceduralAnalysis(context, callGraph, intraproceduralAnalysisResult, externalModulesDFG, lifetimes).analyze()
    }
}