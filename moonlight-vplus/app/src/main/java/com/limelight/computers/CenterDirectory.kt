package com.limelight.computers

import com.limelight.nvstream.http.ComputerDetails
import com.limelight.nvstream.http.LimelightCryptoProvider
import com.limelight.nvstream.http.NvHTTP
import com.limelight.nvstream.http.NvApp
import java.io.IOException
import java.net.InetAddress
import java.net.Proxy
import java.security.MessageDigest
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
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
    data class Connection(val computer: ComputerDetails, val app: NvApp?)

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

    fun connect(device: Device, uniqueId: String, name: String, crypto: LimelightCryptoProvider): Connection {
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

        for (index in 0 until addresses.length()) {
            val address = addresses.getString(index)
            if (!(address.matches(Regex("[0-9.]+")) && address.contains('.') ||
                    address.matches(Regex("[a-fA-F0-9:.]+")) && address.contains(':'))) continue
            val ip = runCatching { InetAddress.getByName(address) }.getOrNull() ?: continue
            if (ip.isAnyLocalAddress || ip.isLoopbackAddress || ip.isLinkLocalAddress || ip.isMulticastAddress) continue
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
                return Connection(computer, app)
            } catch (_: IOException) {
                // Another advertised address may be reachable.
            }
        }
        throw IOException("The host is unreachable")
    }

    private fun request(deviceId: String? = null, body: JSONObject? = null): String {
        val endpoint = url.newBuilder()
            .encodedPath(url.encodedPath.trimEnd('/') + "/v1/devices")
            .apply { if (deviceId != null) addPathSegment(deviceId).addPathSegment("pair") }
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
