package com.krtuworks.sdk;

import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import javax.crypto.AEADBadTagException;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * The Krtuworks SDK's sealing key on Android (the Unity package's AndroidKeystoreMasterKey, the Unreal plugin's FKeystoreMasterKey):
 * encrypted with an AES-256-GCM key that never leaves the Android Keystore. Restored onto another phone,
 * the sealed key does not open (the Keystore key stays on the phone that made it).
 */
public final class KrtuworksKeystore {
    private static final String ALIAS = "com.krtuworks.sdk.sealing";
    private static final String PROVIDER = "AndroidKeyStore";
    private static final int IV_BYTES = 12;

    private KrtuworksKeystore() {}

    private static SecretKey key(boolean create) throws Exception {
        KeyStore store = KeyStore.getInstance(PROVIDER);
        store.load(null);
        KeyStore.Entry entry = store.getEntry(ALIAS, null);
        if (entry instanceof KeyStore.SecretKeyEntry) return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        if (!create) return null;
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, PROVIDER);
        generator.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build());
        return generator.generateKey();
    }

    /**
     * base64(iv | ciphertext) of `secret`, or null when the Keystore cannot (Android before 6.0, or a refusal). A null is
     * not final: the caller keeps the key as plain text meanwhile and calls this again on every launch until it seals.
     */
    public static String seal(String secret) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return null;
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key(true));
            byte[] iv = cipher.getIV();
            byte[] sealed = cipher.doFinal(secret.getBytes(StandardCharsets.UTF_8));
            byte[] out = new byte[iv.length + sealed.length];
            System.arraycopy(iv, 0, out, 0, iv.length);
            System.arraycopy(sealed, 0, out, iv.length, sealed.length);
            return Base64.encodeToString(out, Base64.NO_WRAP);
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * The secret `seal` made here; "" when it can never open on this phone (no Keystore key here, made
     * with another key, or not sealed text); null when the Keystore failed just now (try again later:
     * nothing may be thrown away for it).
     */
    public static String open(String sealed) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return null;
        try {
            SecretKey key = key(false);
            if (key == null) return "";
            byte[] in = Base64.decode(sealed, Base64.NO_WRAP);
            if (in.length <= IV_BYTES) return "";
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, in, 0, IV_BYTES));
            byte[] plain = cipher.doFinal(in, IV_BYTES, in.length - IV_BYTES);
            return new String(plain, StandardCharsets.UTF_8);
        } catch (AEADBadTagException | IllegalArgumentException e) {
            return "";
        } catch (Exception e) {
            return null;
        }
    }
}
