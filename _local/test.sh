find test.output -name "$1\.kexe" | xargs rm
rm test.output/local/macos_x64/localTest.kexe
./gradlew backend.native:tests:$1