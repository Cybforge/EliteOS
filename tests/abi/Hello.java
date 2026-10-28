public class Hello {
    public static void main(String[] a) throws Exception {
        System.out.println("Java " + System.getProperty("java.version") + " on " + System.getProperty("os.name") + " " + System.getProperty("os.arch"));
        Thread[] ts = new Thread[4];
        final long[] sum = new long[1];
        for (int i = 0; i < 4; i++) { final int n = i; ts[i] = new Thread(() -> { for (int k = 0; k < 100000; k++) synchronized (sum) { sum[0] += n; } }); ts[i].start(); }
        for (Thread t : ts) t.join();
        System.out.println("threads sum " + sum[0] + " cpus " + Runtime.getRuntime().availableProcessors());
        java.util.HashMap<Integer,String> m = new java.util.HashMap<>();
        for (int i = 0; i < 100000; i++) m.put(i, "v" + i);
        System.out.println("map " + m.size());
    }
}
