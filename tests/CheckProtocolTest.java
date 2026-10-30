package org.llcast;

final class CheckProtocolTest {
    private static void require(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) {
        require(CheckProtocol.root("uid=0(root) gid=0(root) groups=0(root)\n"));
        require(!CheckProtocol.root("uid=10000(u0_a0) gid=0(root)\n"));
        require(!CheckProtocol.root("uid=00(root)"));
        require(CheckProtocol.passed(CheckProtocol.parse("noise\nCHECK\tcard\tPASS\t/dev/dri/card0\n"), "card"));
        require(!CheckProtocol.passed(CheckProtocol.parse("CHECK\tcard\tPASSING\tpath\n"), "card"));
        require(!CheckProtocol.passed(CheckProtocol.parse("CHECK\tcard\tPASS\tpath\nCHECK\tcard\tPASS\tpath\n"), "card"));
        require(!CheckProtocol.passed(CheckProtocol.parse("CHECK\tcard\tPASS"), "card"));
        require(!CheckProtocol.passed(CheckProtocol.parse(""), "plane"));
        require("SELinux context u:r:untrusted_app".equals(CheckProtocol.details(
            "CHECK\tdecoder\tFAIL\tSELinux context u:r:untrusted_app\n").get("decoder")));
        require("no usable HDMI display".equals(CheckProtocol.sessionFailure("noise\nSESSION\tFAIL\tno usable HDMI display\n")));
        require(CheckProtocol.sessionFailure("SESSION\tPASS\t42\n") == null);
        require(CheckProtocol.nativeCommand("/data/app/~~abc==/io.github.tcchen2026.llcast-xyz==/lib/arm64", "--start")
            .endsWith("libllcast_session.so' --start"));
        for (String path : new String[]{"/data/local/tmp", "/data/app/../tmp", "/data/app/a'; id", "/data/app/a$(id)"}) {
            try { CheckProtocol.nativeCommand(path, "--start"); throw new AssertionError(path); }
            catch (IllegalArgumentException expected) { }
        }
        try { CheckProtocol.nativeCommand("/data/app/valid", "--start; id"); throw new AssertionError(); }
        catch (IllegalArgumentException expected) { }
        // Tablet landscape 1080p60: 1728x1080 exceeds 432000 blocks/s.
        int[] fit = EncoderSize.fit(1920, 1200, 1920, 1080, 160, 160, 2560, 1440, 2, 2,
            (w, h) -> ((w + 15) / 16) * ((h + 15) / 16) * 60 <= 432000);
        require(fit != null && fit[0] == 1712 && fit[1] == 1070);
        // Supported geometry stays unchanged; do not enlarge small screens.
        fit = EncoderSize.fit(1200, 1920, 1920, 1080, 160, 160, 2560, 1440, 2, 2, (w, h) -> true);
        require(fit[0] == 674 && fit[1] == 1080);
        fit = EncoderSize.fit(640, 480, 1920, 1080, 160, 160, 2560, 1440, 16, 16, (w, h) -> true);
        require(fit[0] == 640 && fit[1] == 480);
        // Cap dimensions first, retain aspect, and handle unsupported/minimum sizes.
        fit = EncoderSize.fit(1200, 1920, 3840, 2160, 160, 160, 2560, 1440, 2, 2, (w, h) -> true);
        require(fit[0] == 900 && fit[1] == 1440);
        require(EncoderSize.fit(1920, 1200, 1920, 1080, 160, 160, 2560, 1440, 2, 2, (w, h) -> false) == null);
        require(EncoderSize.fit(100, 100, 1920, 1080, 160, 160, 2560, 1440, 2, 2, (w, h) -> true) == null);
        System.out.println("APK protocol/command and encoder-fit tests passed");
    }
}
