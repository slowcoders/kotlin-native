find test.ouput -name "*\.kexe" | xargs rm
./gradlew --continue backend.native:tests:run