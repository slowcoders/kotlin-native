find test.output -name "*\.kexe" | xargs rm
./gradlew --continue backend.native:tests:run