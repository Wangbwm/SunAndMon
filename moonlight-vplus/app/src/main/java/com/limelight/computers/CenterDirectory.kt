package com.limelight.computers

import android.content.Context
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.core.content.edit
import com.limelight.nvstream.http.ComputerDetails
import com.limelight.nvstream.http.LimelightCryptoProvider
import com.limelight.nvstream.http.NvHTTP
import com.limelight.nvstream.http.NvApp
import java.io.IOException
import java.net.InetAddress
import java.net.Proxy
import java.security.KeyStore
import java.security.MessageDigest
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import okhttp3.Dns
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject

internal class CenterDirectory(url: String, private val key: String, directIp: String = "") {
    data class Device(val id: String, val name: String, val online: Boolean)
    data class Connection(val computer: ComputerDetails, val app: NvApp?, val routeLabel: String = "")
    class TunnelRequiredException(val missingProviders: List<String>) :
        IOException("Missing built-in tunnel providers: ${missingProviders.joinToString(", ")}")

    internal val accessKey: String get() = key

    val url: HttpUrl = requireNotNull(url.trim().toHttpUrlOrNull()).also {
        require(it.username.isEmpty() && it.password.isEmpty() && it.query == null && it.fragment == null)
        require(it.isHttps || it.host in setOf("localhost", "127.0.0.1", "::1"))
        require(key.matches(Regex("[a-f0-9]{64}")))
    }
    val directIp = directIp.trim()
    private val directAddress = this.directIp.takeIf(String::isNotEmpty)?.let { ip ->
        require(this.url.isHttps)
        val address = requireNotNull(numericAddress(ip))
        require(!address.isAnyLocalAddress && !address.isMulticastAddress)
        numericAddress(this.url.host)?.let { require(it == address) }
        address
    }
    private val client = OkHttpClient.Builder()
        .followRedirects(false)
        .followSslRedirects(false)
        .callTimeout(5, TimeUnit.SECONDS)
        .apply {
            val centerUrl = this@CenterDirectory.url
            if (!centerUrl.isHttps || directAddress != null) proxy(Proxy.NO_PROXY)
            directAddress?.let { address ->
                // Keep the HTTPS URL's hostname for SNI and certificate validation.
                dns { host -> if (host == centerUrl.host) listOf(address) else Dns.SYSTEM.lookup(host) }
            }
        }
        .build()

    fun devices(): List<Device> {
        val array = JSONArray(request())
        return (0 until array.length()).mapNotNull { index ->
            val item = array.optJSONObject(index) ?: return@mapNotNull null
            val id = item.optString("id")
            if (!id.matches(Regex("[a-f0-9]{32}"))) return@mapNotNull null
            Device(id, item.getString("name"), item.optBoolean("online"))
        }
    }

    fun tunnelDefaults(): JSONObject? = runCatching {
        val obj = JSONObject(requestPath("/v1/tunnel-config"))
        val p = obj.optString("provider").trim().lowercase()
        if (p == "tailscale" || p == "easytier") obj else null
    }.getOrNull()

    fun connect(
        device: Device,
        uniqueId: String,
        name: String,
        crypto: LimelightCryptoProvider,
        tunnelManager: AndroidTunnelManager? = null
    ): Connection {
        val fingerprint = MessageDigest.getInstance("SHA-256").digest(crypto.getClientCertificate().encoded)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val grant = JSONObject(request(device.id, JSONObject().put("fingerprint", fingerprint)))
        val ticket = grant.getString("ticket")
        require(grant.getString("id") == device.id && ticket.matches(Regex("[0-9]{1,12}\\.[A-Z2-7]{26}\\.[a-f0-9]{64}")))
        val port = grant.getInt("port")
        val httpsPort = grant.getInt("httpsPort")
        require(port in 1..65535 && httpsPort in 1..65535)
        val cert = CertificateFactory.getInstance("X.509")
            .generateCertificate(grant.getString("certificate").byteInputStream()) as X509Certificate
        val addresses = grant.getJSONArray("addresses")
        require(addresses.length() in 1..16)

        // Collect all tunnel endpoints advertised by the host (supports Tailscale + EasyTier simultaneously).
        val hostTunnels = mutableListOf<Pair<String, String>>()
        val hostProviders = mutableListOf<String>()
        val tunnelsArr = grant.optJSONArray("tunnels")
        if (tunnelsArr != null) {
            for (i in 0 until tunnelsArr.length()) {
                val obj = tunnelsArr.optJSONObject(i) ?: continue
                val p = obj.optString("provider").trim().lowercase()
                val a = obj.optString("address").trim()
                if (p.isNotEmpty() && a.isNotEmpty()) {
                    hostTunnels.add(p to a)
                    if (p !in hostProviders) hostProviders.add(p)
                }
            }
        }
        if (hostTunnels.isEmpty()) {
            val lp = grant.optString("tunnelProvider").trim().lowercase()
            val la = grant.optString("tunnelAddress").trim()
            if (lp.isNotEmpty() && la.isNotEmpty()) {
                hostTunnels.add(lp to la)
                hostProviders.add(lp)
            }
        }

        val directCandidates = mutableListOf<Pair<String, String>>()
        for (index in 0 until addresses.length()) {
            val address = addresses.getString(index)
            if (!(address.matches(Regex("[0-9.]+")) && address.contains('.') ||
                    address.matches(Regex("[a-fA-F0-9:.]+")) && address.contains(':'))) continue
            val ip = runCatching { InetAddress.getByName(address) }.getOrNull() ?: continue
            if (ip.isAnyLocalAddress || ip.isLoopbackAddress || ip.isLinkLocalAddress || ip.isMulticastAddress) continue
            val routeLabel = classifyDirectRoute(ip)
            directCandidates.add(address to routeLabel)
        }

        val tunnelCandidates = mutableListOf<Pair<String, String>>()
        if (tunnelManager != null && hostTunnels.isNotEmpty()) {
            val forwards = tunnelManager.ensureForwards(hostTunnels, port, httpsPort)
            for ((localHost, provider) in forwards) {
                val label = if (provider.equals("easytier", ignoreCase = true)) "EasyTier" else "Tailscale"
                tunnelCandidates.add(localHost to label)
            }
        }

        val orderedCandidates = mutableListOf<Pair<String, String>>()
        if (tunnelManager?.preferTunnelFirst() == true) {
            orderedCandidates.addAll(tunnelCandidates)
            orderedCandidates.addAll(directCandidates)
        } else {
            orderedCandidates.addAll(directCandidates)
            orderedCandidates.addAll(tunnelCandidates)
        }

        for ((address, routeLabel) in orderedCandidates) {
            val endpoint = ComputerDetails.AddressTuple(address, port)
            try {
                val http = NvHTTP(endpoint, httpsPort, uniqueId, name, cert, crypto)
                val computer = http.connectViaCenter(ticket, name).apply {
                    activeAddress = endpoint
                    manualAddress = endpoint
                    serverCert = cert
                }
                val app = if (computer.runningGameId == 0 && computer.supportsDesktopSpecialApp) {
                    NvApp(NvApp.DESKTOP_APP_NAME, NvApp.DESKTOP_APP_ID, false)
                } else {
                    runCatching { http.getAppList() }.getOrNull()?.firstOrNull {
                        if (computer.runningGameId != 0) it.appId == computer.runningGameId
                        else it.appName == "Desktop" || it.appName == "桌面"
                    }
                }
                return Connection(computer, app, routeLabel)
            } catch (_: IOException) {
                // Another advertised address may be reachable.
            }
        }

        if (hostProviders.isNotEmpty() && tunnelManager != null) {
            val missing = hostProviders.filter { !tunnelManager.isProviderConnected(it) }
                .map { if (it == "easytier") "EasyTier" else "Tailscale" }
            if (missing.isNotEmpty()) {
                throw TunnelRequiredException(missing)
            }
        }
        throw IOException("The host is unreachable")
    }

    private fun classifyDirectRoute(ip: InetAddress): String {
        val raw = ip.address
        if (raw.size == 4) {
            val b0 = raw[0].toInt() and 0xFF
            val b1 = raw[1].toInt() and 0xFF
            // 100.64.0.0/10 (system Tailscale / CGNAT)
            if (b0 == 100 && (b1 and 0xC0) == 64) return "Tailscale"
        }
        return if (ip.isSiteLocalAddress || ip.isLoopbackAddress) "局域网直连" else "公网直连"
    }

    private fun request(deviceId: String? = null, body: JSONObject? = null): String {
        val path = if (deviceId != null) "/v1/devices/$deviceId/pair" else "/v1/devices"
        return requestPath(path, body)
    }

    private fun requestPath(subPath: String, body: JSONObject? = null): String {
        val endpoint = url.newBuilder()
            .encodedPath(url.encodedPath.trimEnd('/') + subPath)
            .build()
        val request = Request.Builder().url(endpoint)
            .header("Authorization", "Bearer $key")
            .apply { if (body != null) post(body.toString().toRequestBody("application/json".toMediaType())) }
            .build()
        return client.newCall(request).execute().use { response ->
            if (response.code != 200) throw IOException("Center returned HTTP ${response.code}")
            val source = response.body.source()
            if (source.request(MAX_RESPONSE + 1)) throw IOException("Center response is too large")
            source.readUtf8()
        }
    }

    private companion object {
        const val MAX_RESPONSE = 2L * 1024 * 1024

        fun numericAddress(value: String): InetAddress? {
            if (':' in value) {
                if (!value.matches(Regex("[0-9a-fA-F:.]+"))) return null
                return runCatching { InetAddress.getByName(value) }.getOrNull()
            }
            val octets = value.split('.')
            if (octets.size != 4) return null
            val bytes = ByteArray(4)
            for ((index, octet) in octets.withIndex()) {
                if (octet.isEmpty() || octet.length > 3 || octet.any { it !in '0'..'9' }) return null
                val number = octet.toInt()
                if (number > 255) return null
                bytes[index] = number.toByte()
            }
            return InetAddress.getByAddress(bytes)
        }
    }
}

internal object CenterCredentialStore {
    private const val PREF_NAME = "center"
    private const val KEY_URL = "url"
    private const val KEY_DIRECT_IP = "directIp"
    private const val KEY_ENC = "accessKeyEnc"
    private const val KEY_IV = "accessKeyIv"
    private const val KEY_FALLBACK = "accessKeyFallback"
    private const val ANDROID_KEYSTORE = "AndroidKeyStore"
    private const val KEYSTORE_ALIAS = "sunandmon_center_access_key_v1"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"

    fun savedUrl(context: Context): String =
        prefs(context).getString(KEY_URL, "").orEmpty()

    fun savedDirectIp(context: Context): String =
        prefs(context).getString(KEY_DIRECT_IP, "").orEmpty()

    fun save(context: Context, directory: CenterDirectory) {
        val rawBytes = directory.accessKey.toByteArray(Charsets.UTF_8)
        val encrypted = encryptWithKeystore(rawBytes)
        prefs(context).edit {
            putString(KEY_URL, directory.url.toString())
            putString(KEY_DIRECT_IP, directory.directIp)
            if (encrypted != null) {
                putString(KEY_ENC, Base64.encodeToString(encrypted.first, Base64.NO_WRAP))
                putString(KEY_IV, Base64.encodeToString(encrypted.second, Base64.NO_WRAP))
                remove(KEY_FALLBACK)
            } else {
                remove(KEY_ENC)
                remove(KEY_IV)
                putString(KEY_FALLBACK, Base64.encodeToString(rawBytes, Base64.NO_WRAP))
            }
        }
    }

    fun load(context: Context): CenterDirectory? {
        val p = prefs(context)
        val url = p.getString(KEY_URL, "").orEmpty().trim()
        if (url.isEmpty()) return null
        val directIp = p.getString(KEY_DIRECT_IP, "").orEmpty().trim()
        val key = loadKey(p) ?: return null
        return runCatching { CenterDirectory(url, key, directIp) }.getOrNull()
    }

    fun clearKey(context: Context) {
        prefs(context).edit {
            remove(KEY_ENC)
            remove(KEY_IV)
            remove(KEY_FALLBACK)
        }
    }

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)

    private fun loadKey(p: android.content.SharedPreferences): String? {
        val enc = p.getString(KEY_ENC, null)
        val iv = p.getString(KEY_IV, null)
        if (!enc.isNullOrEmpty() && !iv.isNullOrEmpty()) {
            val plain = runCatching {
                val cipherBytes = Base64.decode(enc, Base64.NO_WRAP)
                val ivBytes = Base64.decode(iv, Base64.NO_WRAP)
                decryptWithKeystore(cipherBytes, ivBytes)
            }.getOrNull()
            if (plain != null) return String(plain, Charsets.UTF_8)
        }
        val fallback = p.getString(KEY_FALLBACK, null) ?: return null
        return runCatching { String(Base64.decode(fallback, Base64.NO_WRAP), Charsets.UTF_8) }.getOrNull()
    }

    private fun encryptWithKeystore(plain: ByteArray): Pair<ByteArray, ByteArray>? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return null
        return runCatching {
            val secretKey = getOrCreateKey() ?: return null
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, secretKey)
            cipher.doFinal(plain) to cipher.iv
        }.getOrNull()
    }

    private fun decryptWithKeystore(cipherBytes: ByteArray, ivBytes: ByteArray): ByteArray? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return null
        return runCatching {
            val secretKey = getExistingKey() ?: return null
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, secretKey, GCMParameterSpec(128, ivBytes))
            cipher.doFinal(cipherBytes)
        }.getOrNull()
    }

    private fun getExistingKey(): SecretKey? {
        val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        return ks.getKey(KEYSTORE_ALIAS, null) as? SecretKey
    }

    private fun getOrCreateKey(): SecretKey? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return null
        getExistingKey()?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE)
        val spec = KeyGenParameterSpec.Builder(
            KEYSTORE_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
        )
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build()
        generator.init(spec)
        return generator.generateKey()
    }
}
