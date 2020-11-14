../../../../dist/bin/kotlinc src/nativeMain/kotlin/ -memory-model relaxed \
  -library objcSmoke -library objc_illegal_sharing_with_weak \
  -verbose -nomain -p framework -Xembed-bitcode \
  -tr -Xopt-in=kotlin.RequiresOptIn,kotlin.ExperimentalStdlibApi -friend-modules /Users/zeedh/slowcoders/kotlin-native/dist/klib/common/stdlib \
  -opt -linker-option -L. -linker-option -lobjcsmoke -o build/objs/Test2

# -opt : release
# -g : debug