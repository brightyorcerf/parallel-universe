import jdk.jfr.consumer.RecordingFile;
import java.nio.file.Path;
public class CountParks {
    public static void main(String[] a) throws Exception {
        long n = 0;
        for (var e : RecordingFile.readAllEvents(Path.of(a[0])))
            if (e.getEventType().getName().equals("jdk.ThreadPark")) n++;
        System.out.println(n);
    }
}
