
public class TestMain {
    public static void main(String args0[]) {
        String command = "konanc /Users/zeedh/Desktop/ktest/kobjc/libs/zee/src/nativeMain/kotlin/hello.kt" +
                " -memory-model relaxed -verbose -nomain -p framework -g -o build/objs/Test2";
        String[] args2 = command.split(" ");
        org.jetbrains.kotlin.cli.utilities.MainKt.main(args2);
    }
}
