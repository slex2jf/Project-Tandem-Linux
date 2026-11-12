package com.ethannicholas.mkdd;

import android.app.NativeActivity;
import android.content.Intent;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.provider.Settings;

public class TandemActivity extends NativeActivity {
    static {
        System.loadLibrary("mkdd");
    }

    private static final int ISO_REQUEST = 18001;
    private static final int EOS_CONFIG_REQUEST = 18002;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        ensurePublicIsoFolder();
    }

    /** Create the user-visible ISO directory without storage permissions on Android Q+. */
    private void ensurePublicIsoFolder() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            android.util.Log.i("TANDEM-ISO", "Pasta publica requer selecao SAF neste Android");
            return;
        }
        try {
            ContentResolver resolver = getContentResolver();
            Uri collection = MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
            String relative = Environment.DIRECTORY_DOCUMENTS + "/Project Tandem/ISOs";
            String[] projection = { MediaStore.MediaColumns._ID };
            String selection = MediaStore.MediaColumns.RELATIVE_PATH + "=? AND "
                    + MediaStore.MediaColumns.DISPLAY_NAME + "=?";
            String[] args = { relative + "/", ".nomedia" };
            try (android.database.Cursor cursor = resolver.query(collection, projection, selection, args, null)) {
                if (cursor != null && cursor.moveToFirst()) return;
            }
            ContentValues values = new ContentValues();
            values.put(MediaStore.MediaColumns.DISPLAY_NAME, ".nomedia");
            values.put(MediaStore.MediaColumns.MIME_TYPE, "application/octet-stream");
            values.put(MediaStore.MediaColumns.RELATIVE_PATH, relative);
            Uri created = resolver.insert(collection, values);
            android.util.Log.i("TANDEM-ISO", created != null
                    ? "Pasta publica pronta: Documents/Project Tandem/ISOs"
                    : "Nao foi possivel criar pasta publica; SAF continua disponivel");
        } catch (Exception e) {
            android.util.Log.i("TANDEM-ISO", "Pasta publica indisponivel; SAF continua disponivel");
        }
    }

    public void openEosConfigPicker() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/json");
                intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/json", "text/plain", "*/*" });
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                startActivityForResult(intent, EOS_CONFIG_REQUEST);
            }
        });
    }

    public void openIsoPicker() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/octet-stream");
                intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/octet-stream", "application/x-iso9660-image", "*/*" });
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                startActivityForResult(intent, ISO_REQUEST);
            }
        });
    }

    /** Reopen the last ISO only while its SAF grant and descriptor are valid. */
    public boolean openPersistedIso() {
        String saved = getSharedPreferences("tandem", MODE_PRIVATE)
                .getString("iso_uri", null);
        if (saved == null || saved.length() == 0) return false;
        ParcelFileDescriptor pfd = null;
        try {
            Uri uri = Uri.parse(saved);
            pfd = getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) throw new IllegalStateException("persisted ISO unavailable");
            nativeIsoSelected(pfd.detachFd(), saved);
            return true;
        } catch (Exception e) {
            if (pfd != null) { try { pfd.close(); } catch (java.io.IOException ignored) {} }
            getSharedPreferences("tandem", MODE_PRIVATE).edit().remove("iso_uri").apply();
            android.util.Log.i("TANDEM-ISO", "ISO persistida indisponivel; abrindo seletor");
            return false;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == EOS_CONFIG_REQUEST) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                nativeEosConfigCancelled();
                return;
            }
            Uri uri = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
                // A transient grant remains valid while the detached FD is consumed.
            }
            ParcelFileDescriptor pfd = null;
            try {
                pfd = getContentResolver().openFileDescriptor(uri, "r");
                if (pfd == null) throw new IllegalStateException("openFileDescriptor returned null");
                nativeEosConfigSelected(pfd.detachFd());
            } catch (Exception e) {
                if (pfd != null) { try { pfd.close(); } catch (java.io.IOException ignored) {} }
                nativeEosConfigCancelled();
            }
            return;
        }
        if (requestCode != ISO_REQUEST) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            nativeIsoCancelled();
            return;
        }
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // Some providers grant a transient read permission only; keep the FD alive.
        }
        ParcelFileDescriptor pfd = null;
        try {
            pfd = getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) throw new IllegalStateException("openFileDescriptor returned null");
            getSharedPreferences("tandem", MODE_PRIVATE).edit()
                    .putString("iso_uri", uri.toString()).apply();
            int fd = pfd.detachFd();
            nativeIsoSelected(fd, uri.toString());
        } catch (Exception e) {
            if (pfd != null) { try { pfd.close(); } catch (java.io.IOException closeError) { android.util.Log.e("TANDEM-ISO", "Erro ao fechar PFD", closeError); } }
            nativeIsoCancelled();
        }
    }

    private static native void nativeIsoSelected(int fd, String displayName);
    private static native void nativeIsoCancelled();
    private static native void nativeEosConfigSelected(int fd);
    private static native void nativeEosConfigCancelled();
}
