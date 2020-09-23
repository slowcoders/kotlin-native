# kotlin v1.4.20
../../../../dist/bin/kotlinc src/nativeMain/kotlin/ -memory-model relaxed \
  -library objcSmoke -library objc_illegal_sharing_with_weak \
  -verbose -nomain -p framework -Xembed-bitcode \
  -g -linker-option -L. -linker-option -lobjcsmoke -o build/objs/Test2

# -opt : release
# -g : debug