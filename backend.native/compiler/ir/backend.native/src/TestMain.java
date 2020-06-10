
public class TestMain {
    /**
     * Setup
     * dist/konan/lib/kotlin-native 를 풀고,
     * org/jetbrains/kotlin/backend/konarn 을 konan22로 rename
     *   konan 폴더를 생성하고,
     *   konan22/files 와 konan22/hash 를 새로 생성한 konan 폴더로 옮김.
     *
     * Java 실행 옵션에 아래 내용 모두 추가.
     * -Djava.library.path=/Users/zeedh/slowcoders/kotlin-native/dist/konan/nativelib
     * -Dkonan.home=/Users/zeedh/slowcoders/kotlin-native/dist
     * -Dfile.encoding=UTF-8
     */
    public static void main(String args0[]) {
        String command = "konanc /Users/zeedh/Desktop/ktest/kobjc/libs/zee/src/nativeMain/kotlin/hello.kt" +
                " -memory-model relaxed -verbose -nomain -p framework -g " +
                " -o /Users/zeedh/Desktop/ktest/kobjc/libs/zee/build/objs/Test2";
        String[] args2 = command.split(" ");
        org.jetbrains.kotlin.cli.utilities.MainKt.main(args2);
    }
}
