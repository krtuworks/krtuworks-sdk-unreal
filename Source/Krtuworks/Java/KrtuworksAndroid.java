package com.krtuworks.sdk;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;

/**
 * The Krtuworks SDK's Android helpers for the Unreal plugin (Private/Android/KrtuworksAndroid.cpp): the
 * no-backup folder, the browser, and the sign-in's return on the app's scheme (com.krtuworks.<slug>, added to
 * GameActivity by Krtuworks_UPL.xml, which also calls received() from GameActivity.onNewIntent).
 */
public final class KrtuworksAndroid {
    private static final String SCHEME_PREFIX = "com.krtuworks.";

    private KrtuworksAndroid() {}

    private static native void nativeAddress(String address);

    /** Context.getNoBackupFilesDir(): never in Auto Backup or a device-to-device transfer. */
    public static String noBackupDir(Activity activity) {
        return activity.getNoBackupFilesDir().getAbsolutePath();
    }

    /** Opens `url` in the browser. */
    public static void openBrowser(final Activity activity, final String url) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            }
        });
    }

    private static String ours(Intent intent) {
        if (intent == null) return null;
        Uri data = intent.getData();
        if (data == null || data.getScheme() == null || !data.getScheme().startsWith(SCHEME_PREFIX)) return null;
        return data.toString();
    }

    /** The sign-in's return while the game runs: handed to the native side, which keeps it if a sign-in waits for it. */
    public static void received(Intent intent) {
        String address = ours(intent);
        if (address != null) nativeAddress(address);
    }

    /** The address that started the app (the system closed it while the sign-in page was open), or null. */
    public static String launchAddress(Activity activity) {
        return ours(activity.getIntent());
    }
}
