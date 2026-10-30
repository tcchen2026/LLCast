package org.llcast;

import java.util.HashMap;
import java.util.Map;

/** Strict, fail-closed parsing of the native check protocol. No text is executed. */
final class CheckProtocol {
    static Map<String, Boolean> parse(String output) {
        Map<String, Boolean> checks = new HashMap<>();
        for (String line : output.split("\n")) {
            String[] fields = line.trim().split("\t", -1);
            if (fields.length != 4 || !fields[0].equals("CHECK")) continue;
            if (!fields[2].equals("PASS") && !fields[2].equals("FAIL")) continue;
            // A duplicate, even a later PASS, must not hide a failure.
            boolean pass = fields[2].equals("PASS") && !checks.containsKey(fields[1]);
            checks.put(fields[1], pass);
        }
        return checks;
    }

    /** The detail field of each CHECK line (the last one wins); for showing why a check failed. */
    static Map<String, String> details(String output) {
        Map<String, String> details = new HashMap<>();
        for (String line : output.split("\n")) {
            String[] fields = line.trim().split("\t", -1);
            if (fields.length == 4 && fields[0].equals("CHECK")) details.put(fields[1], fields[3]);
        }
        return details;
    }

    /** The reason after "SESSION\tFAIL\t", or null. */
    static String sessionFailure(String output) {
        for (String line : output.split("\n")) {
            String[] fields = line.trim().split("\t", 3);
            if (fields.length == 3 && fields[0].equals("SESSION") && fields[1].equals("FAIL")) return fields[2];
        }
        return null;
    }

    static boolean passed(Map<String, Boolean> checks, String key) {
        return Boolean.TRUE.equals(checks.get(key));
    }

    static boolean root(String output) {
        return output.matches("(?s).*\\buid=0(?:\\(|\\s).*" );
    }

    static String nativeCommand(String directory, String operation) {
        if (!directory.startsWith("/data/app/") || directory.contains("..") ||
            !directory.matches("[a-zA-Z0-9_/=+.~-]+")) throw new IllegalArgumentException("nativeLibraryDir");
        if (!operation.equals("--probe") && !operation.equals("--start"))
            throw new IllegalArgumentException("operation");
        return "exec '" + directory + "/libllcast_session.so' " + operation;
    }
}
