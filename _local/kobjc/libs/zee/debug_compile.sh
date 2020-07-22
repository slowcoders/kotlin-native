kotlinc -J-agentlib:jdwp=transport=dt_socket,server=y,suspend=y,address=5006 src/nativeMain/kotlin/var1.kt -memory-model relaxed -g -nomain -verbose -p framework
