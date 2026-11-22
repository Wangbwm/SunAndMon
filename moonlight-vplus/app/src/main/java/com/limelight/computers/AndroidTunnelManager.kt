package com.limelight.computers

import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.content.edit
import com.easytier.jni.EasyTierJNI
import com.limelight.LimeLog
import dalvik.system.BaseDexClassLoader
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.net.InetSocketAddress
import java.net.Proxy
import java.net.ServerSocket
import java.security.SecureRandom
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject

internal class AndroidTunnelManager private constructor(context: Context) {
    private val appContext = context.applicationContext
    private val prefs = appContext.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val executor = Executors.newScheduledThreadPool(2) { r ->
        Thread(r, "SunAndMon-Tunnel").apply { isDaemon = true }
    }
    private val httpClient = OkHttpClient.Builder()
        .proxy(Proxy.NO_PROXY)
        .connectTimeout(3, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .writeTimeout(10, TimeUnit.SECONDS)
        .build()

    private val listeners = CopyOnWriteArraySet<() -> Unit>()
    private var pollFuture: ScheduledFuture<*>? = null

    // Shared settings
    @Volatile var routePolicy: String = normalizeRoutePolicy(prefs.getString(KEY_ROUTE_POLICY, "auto").orEmpty())
        private set
    @Volatile var hostname: String = prefs.getString(KEY_HOSTNAME, "").orEmpty().trim()
        .ifEmpty { defaultHostname() }
        private set
    @Volatile var centerDefaults: JSONObject? = prefs.getString(KEY_CENTER_DEFAULTS, null)
        ?.takeIf { it.isNotBlank() }
        ?.let { runCatching { JSONObject(it) }.getOrNull() }
        private set

    // Tailscale settings & state
    @Volatile var tsAutoConnect: Boolean = prefs.getBoolean(KEY_TS_AUTO, false)
        private set
    @Volatile var tsControlUrl: String = prefs.getString(KEY_TS_CONTROL_URL, "").orEmpty().trim()
        private set
    @Volatile var tsAuthKey: String = prefs.getString(KEY_TS_AUTH_KEY, "").orEmpty().trim()
        private set
    @Volatile var tsUseWebOAuth: Boolean = prefs.getBoolean(KEY_TS_WEB_OAUTH, true)
        private set

    @Volatile var tsState: String = "stopped"
        private set
    @Volatile var tsVirtualIp: String = ""
        private set
    @Volatile var tsAuthUrl: String = ""
        private set
    @Volatile var tsError: String = ""
        private set
    @Volatile var tsPeersOnline: Int = 0
        private set

    // EasyTier settings & state
    @Volatile var etAutoConnect: Boolean = prefs.getBoolean(KEY_ET_AUTO, false)
        private set
    @Volatile var etNetworkName: String = prefs.getString(KEY_ET_NETWORK_NAME, "").orEmpty().trim()
        private set
    @Volatile var etNetworkSecret: String = prefs.getString(KEY_ET_NETWORK_SECRET, "").orEmpty().trim()
        private set
    @Volatile var etPeers: String = prefs.getString(KEY_ET_PEERS, DEFAULT_ET_PEERS).orEmpty().trim()
        .ifEmpty { DEFAULT_ET_PEERS }
        private set
    @Volatile var etStaticIp: String = prefs.getString(KEY_ET_STATIC_IP, "").orEmpty().trim()
        private set

    @Volatile var etState: String = "stopped"
        private set
    @Volatile var etVirtualIp: String = ""
        private set
    @Volatile var etError: String = ""
        private set
    @Volatile var etPeersOnline: Int = 0
        private set

    // Tailscale daemon process state
    private val daemonLock = Any()
    private var daemonProcess: Process? = null
    @Volatile private var daemonPort: Int = 0
    @Volatile private var daemonToken: String = ""
    @Volatile private var daemonReady: Boolean = false
    @Volatile private var pendingStartTs: Boolean = false

    // EasyTier in-process userspace state & port forwards
    private val etLock = Any()
    @Volatile private var etRunning: Boolean = false
    private data class EtPortSet(val httpPort: Int, val httpsPort: Int) {
        fun tcpPorts(): List<Int> {
            val http = if (httpPort > 0) httpPort else 47989
            val https = if (httpsPort > 0) httpsPort else if (http > 5) http - 5 else 47984
            val rtsp = http + 21
            val clip = http + 1
            return listOf(http, https, rtsp, clip).distinct()
        }
        fun udpPorts(): List<Int> {
            val http = if (httpPort > 0) httpPort else 47989
            return listOf(http + 9, http + 10, http + 11, http + 13).distinct()
        }
    }
    private data class EtForwardSpec(
        val remoteIp: String,
        val localHost: String,
        val portSet: EtPortSet
    )
    private val etForwards = LinkedHashMap<String, EtForwardSpec>() // key = "$remoteIp:${portSet.httpPort}:${portSet.httpsPort}"
    private val etLoopbackByRemote = LinkedHashMap<String, String>()
    private var etNextLoopbackSlot = 10
    private var etAppliedToml: String = ""

    init {
        startPoller()
    }

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyChanged() {
        mainHandler.post {
            for (l in listeners) {
                runCatching { l() }
            }
        }
    }

    val tsAvailable: Boolean
        get() = findTunnelDaemonExecutable() != null

    val etAvailable: Boolean
        get() {
            val loader = EasyTierJNI::class.java.classLoader as? BaseDexClassLoader
            return loader?.findLibrary("easytier_android_jni") != null &&
                loader.findLibrary("easytier_ffi") != null
        }

    fun isProviderConnected(provider: String): Boolean = when (provider.lowercase()) {
        "tailscale" -> tsState == "connected"
        "easytier" -> etState == "connected"
        else -> false
    }

    fun preferTunnelFirst(): Boolean =
        routePolicy == "prefer_ts" || routePolicy == "prefer_et"

    fun orderedTunnelProviders(): List<String> =
        if (routePolicy == "auto_ts" || routePolicy == "prefer_ts") {
            listOf("tailscale", "easytier")
        } else {
            listOf("easytier", "tailscale")
        }

    fun summaryBadge(): String {
        val tsUp = tsState == "connected"
        val etUp = etState == "connected"
        if (tsUp && etUp) return "TS + ET"
        if (tsUp) return "Tailscale"
        if (etUp) return "EasyTier"
        if (tsState == "needs_auth") return "Tailscale 待登录"
        if (tsState == "starting" || etState == "starting") return "连接中…"
        if (tsState == "error" || etState == "error") return "错误"
        return ""
    }

    fun isAnyConnected(): Boolean = tsState == "connected" || etState == "connected"

    fun updateRoutePolicy(policy: String) {
        val norm = normalizeRoutePolicy(policy)
        if (norm != routePolicy) {
            routePolicy = norm
            saveSettings()
            notifyChanged()
        }
    }

    fun startOnLaunch() {
        centerDefaults?.let { cached ->
            val p = cached.optString("provider").trim().lowercase()
            if ((p == "easytier" && etNetworkName.isEmpty()) ||
                (p == "tailscale" && tsControlUrl.isEmpty() && tsAuthKey.isEmpty())
            ) {
                applyCenterDefaults(cached, autoStartIfIdle = false)
            }
        }
        if (tsAutoConnect && tsAvailable && tsState == "stopped") {
            tsState = "starting"
            tsError = ""
            pendingStartTs = true
            notifyChanged()
            executor.execute { ensureDaemonAndStartTailscale() }
        }
        if (etAutoConnect && etAvailable && etNetworkName.isNotEmpty() && etState == "stopped") {
            etState = "starting"
            etError = ""
            notifyChanged()
            executor.execute { startOrReloadEasyTier(forceRestart = true) }
        }
    }

    fun setCachedCenterDefaults(defaults: JSONObject) {
        centerDefaults = defaults
        prefs.edit { putString(KEY_CENTER_DEFAULTS, defaults.toString()) }
        notifyChanged()
    }

    fun applyCenterDefaults(defaults: JSONObject, autoStartIfIdle: Boolean = false) {
        centerDefaults = defaults
        prefs.edit { putString(KEY_CENTER_DEFAULTS, defaults.toString()) }
        val provider = defaults.optString("provider").trim().lowercase()
        if (provider == "tailscale") {
            if (defaults.has("controlUrl")) {
                tsControlUrl = defaults.optString("controlUrl").trim()
            }
            val key = defaults.optString("authKey").trim()
            if (key.isNotEmpty()) {
                tsAuthKey = key
                tsUseWebOAuth = false
            }
            if (autoStartIfIdle && tsAuthKey.isNotEmpty()) {
                tsAutoConnect = true
            }
            saveSettings()
            notifyChanged()
            if (autoStartIfIdle && tsState == "stopped" && tsAvailable && tsAuthKey.isNotEmpty()) {
                saveAndConnectTailscale(
                    autoConnect = true,
                    newHostname = hostname,
                    controlUrl = tsControlUrl,
                    authKey = tsAuthKey,
                    useWebOAuth = false
                )
            }
        } else if (provider == "easytier") {
            val netName = defaults.optString("networkName").trim()
            val netSecret = defaults.optString("networkSecret").trim()
            if (netName.isNotEmpty()) etNetworkName = netName
            if (netSecret.isNotEmpty()) etNetworkSecret = netSecret
            val peersArr = defaults.optJSONArray("peers")
            if (peersArr != null && peersArr.length() > 0) {
                val list = mutableListOf<String>()
                for (i in 0 until peersArr.length()) {
                    val p = peersArr.optString(i).trim()
                    if (p.isNotEmpty()) list.add(p)
                }
                if (list.isNotEmpty()) {
                    etPeers = list.joinToString("\n")
                }
            }
            if (autoStartIfIdle && etNetworkName.isNotEmpty()) {
                etAutoConnect = true
            }
            saveSettings()
            notifyChanged()
            if (autoStartIfIdle && etState == "stopped" && etAvailable && etNetworkName.isNotEmpty()) {
                saveAndConnectEasyTier(
                    autoConnect = true,
                    newHostname = hostname,
                    networkName = etNetworkName,
                    networkSecret = etNetworkSecret,
                    peers = etPeers,
                    staticIp = etStaticIp
                )
            }
        }
    }

    fun saveAndConnectTailscale(
        autoConnect: Boolean,
        newHostname: String,
        controlUrl: String,
        authKey: String,
        useWebOAuth: Boolean
    ) {
        tsAutoConnect = autoConnect
        if (newHostname.trim().isNotEmpty()) hostname = newHostname.trim()
        tsControlUrl = controlUrl.trim()
        tsAuthKey = authKey.trim()
        tsUseWebOAuth = useWebOAuth
        saveSettings()

        if (!tsAvailable) {
            tsState = "error"
            tsError = "内置 Tailscale 组件不可用"
            notifyChanged()
            return
        }

        tsState = "starting"
        tsError = ""
        tsAuthUrl = ""
        pendingStartTs = true
        notifyChanged()

        executor.execute {
            ensureDaemonAndStartTailscale()
        }
    }

    fun stopTailscale() {
        pendingStartTs = false
        tsState = "stopped"
        tsVirtualIp = ""
        tsAuthUrl = ""
        tsError = ""
        tsPeersOnline = 0
        notifyChanged()
        executor.execute {
            if (daemonReady) {
                runCatching {
                    val body = JSONObject().put("provider", "tailscale")
                    daemonRequest("POST", "/v1/stop", body)
                }
            }
        }
    }

    fun openTailscaleAuthUrl(context: Context) {
        val url = tsAuthUrl.trim()
        if (url.isEmpty()) return
        runCatching {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url)).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            context.startActivity(intent)
        }
    }

    fun saveAndConnectEasyTier(
        autoConnect: Boolean,
        newHostname: String,
        networkName: String,
        networkSecret: String,
        peers: String,
        staticIp: String
    ) {
        etAutoConnect = autoConnect
        if (newHostname.trim().isNotEmpty()) hostname = newHostname.trim()
        etNetworkName = networkName.trim()
        etNetworkSecret = networkSecret.trim()
        etPeers = peers.trim().ifEmpty { DEFAULT_ET_PEERS }
        etStaticIp = staticIp.trim()
        saveSettings()

        if (!etAvailable) {
            etState = "error"
            etError = "当前设备架构不支持内置 EasyTier (需 arm64-v8a)"
            notifyChanged()
            return
        }
        if (etNetworkName.isEmpty()) {
            etState = "error"
            etError = "请输入 EasyTier 网络名称"
            notifyChanged()
            return
        }

        etState = "starting"
        etError = ""
        notifyChanged()

        executor.execute {
            startOrReloadEasyTier(forceRestart = true)
        }
    }

    fun stopEasyTier() {
        synchronized(etLock) {
            etRunning = false
            etAppliedToml = ""
            if (etAvailable) {
                runCatching { EasyTierJNI.stopAllInstances() }
            }
        }
        etState = "stopped"
        etVirtualIp = ""
        etError = ""
        etPeersOnline = 0
        notifyChanged()
    }

    /**
     * Blocking helper called on Dispatchers.IO inside CenterDirectory.connect().
     * Returns ordered list of Pair(localLoopbackHost, provider) for connected tunnel providers.
     */
    fun ensureForwards(
        endpoints: List<Pair<String, String>>,
        httpPort: Int,
        httpsPort: Int
    ): List<Pair<String, String>> {
        val out = mutableListOf<Pair<String, String>>()
        for (preferred in orderedTunnelProviders()) {
            val ep = endpoints.firstOrNull {
                it.first.equals(preferred, ignoreCase = true) && it.second.isNotBlank()
            } ?: continue
            val remoteIp = ep.second.trim()
            if (preferred == "easytier" && isProviderConnected("easytier")) {
                val localHost = ensureEasyTierForward(remoteIp, httpPort, httpsPort)
                if (!localHost.isNullOrEmpty()) {
                    out.add(localHost to "easytier")
                }
            } else if (preferred == "tailscale" && isProviderConnected("tailscale")) {
                val localHost = ensureTailscaleForward(remoteIp, httpPort, httpsPort)
                if (!localHost.isNullOrEmpty()) {
                    out.add(localHost to "tailscale")
                }
            }
        }
        return out
    }

    private fun ensureTailscaleForward(remoteIp: String, httpPort: Int, httpsPort: Int): String? {
        if (!daemonReady) return null
        return runCatching {
            val body = JSONObject()
                .put("provider", "tailscale")
                .put("remoteIp", remoteIp)
                .put(
                    "ports",
                    JSONObject()
                        .put("httpPort", httpPort)
                        .put("httpsPort", httpsPort)
                )
            val resp = daemonRequest("POST", "/v1/forward", body) ?: return null
            resp.optString("localHost").trim().takeIf { it.isNotEmpty() }
        }.getOrNull()
    }

    private fun allocateEasyTierLoopback(remoteIp: String): String {
        etLoopbackByRemote[remoteIp]?.let { return it }
        val slot = etNextLoopbackSlot++
        val hi = (slot / 250) and 0xFF
        val lo = (slot % 250) + 1
        val candidate = "127.18.1.$lo".takeIf { hi == 0 } ?: "127.18.${hi + 1}.$lo"
        val bindable = runCatching {
            ServerSocket().use { ss ->
                ss.reuseAddress = true
                ss.bind(InetSocketAddress(candidate, 0))
                true
            }
        }.getOrDefault(false)
        val chosen = if (bindable) candidate else "127.0.0.1"
        etLoopbackByRemote[remoteIp] = chosen
        return chosen
    }

    private fun ensureEasyTierForward(remoteIp: String, httpPort: Int, httpsPort: Int): String? {
        if (!etAvailable) return null
        val portSet = EtPortSet(httpPort, httpsPort)
        val key = "$remoteIp:${portSet.httpPort}:${portSet.httpsPort}"
        var needReload = false
        val localHost: String
        synchronized(etLock) {
            if (!etRunning) return null
            val existing = etForwards[key]
            if (existing != null) {
                localHost = existing.localHost
            } else {
                localHost = allocateEasyTierLoopback(remoteIp)
                etForwards[key] = EtForwardSpec(remoteIp, localHost, portSet)
                needReload = true
            }
        }
        if (needReload) {
            startOrReloadEasyTier(forceRestart = true)
            // Wait up to 3.5s for EasyTier mesh routes to settle after reload
            val deadline = System.currentTimeMillis() + 3500L
            while (System.currentTimeMillis() < deadline) {
                val infos = runCatching { EasyTierJNI.collectNetworkInfos() }.getOrNull()
                if (!infos.isNullOrEmpty() && hasAnyEasyTierRoute(infos)) {
                    Thread.sleep(250)
                    break
                }
                Thread.sleep(200)
            }
        }
        return localHost
    }

    private fun hasAnyEasyTierRoute(infosJson: String): Boolean {
        return runCatching {
            val root = JSONObject(infosJson)
            val map = root.optJSONObject("map") ?: return false
            val keys = map.keys()
            while (keys.hasNext()) {
                val inst = map.optJSONObject(keys.next()) ?: continue
                val routes = inst.optJSONArray("routes")
                if (routes != null && routes.length() > 0) return true
            }
            false
        }.getOrDefault(false)
    }

    private fun startOrReloadEasyTier(forceRestart: Boolean) {
        if (!etAvailable || etNetworkName.isEmpty()) return
        synchronized(etLock) {
            val toml = buildEasyTierUserspaceToml()
            if (!forceRestart && etRunning && toml == etAppliedToml) {
                return
            }
            runCatching { EasyTierJNI.stopAllInstances() }
            val rc = runCatching { EasyTierJNI.runNetworkInstance(toml) }.getOrElse { e ->
                LimeLog.warning("EasyTierJNI.runNetworkInstance failed: ${e.message}")
                -1
            }
            if (rc == 0) {
                etRunning = true
                etAppliedToml = toml
                if (etState != "connected") {
                    etState = "starting"
                }
                etError = ""
            } else {
                etRunning = false
                etState = "error"
                etError = runCatching { EasyTierJNI.getLastError() }.getOrNull()
                    ?.takeIf { it.isNotBlank() } ?: "启动 EasyTier 失败"
            }
        }
        notifyChanged()
        pollEasyTierStatus()
    }

    private fun buildEasyTierUserspaceToml(): String {
        val sb = StringBuilder()
        val host = hostname.ifBlank { defaultHostname() }
        appendTomlString(sb, "hostname", host)
        appendTomlString(sb, "instance_name", ET_INSTANCE_NAME)
        if (etStaticIp.isNotEmpty()) {
            sb.append("dhcp = false\n")
            val ipWithCidr = if ("/" in etStaticIp) etStaticIp else "$etStaticIp/24"
            appendTomlString(sb, "ipv4", ipWithCidr)
        } else {
            sb.append("dhcp = true\n")
        }
        appendTomlString(sb, "rpc_portal", "127.0.0.1:0")

        sb.append("\n[network_identity]\n")
        appendTomlString(sb, "network_name", etNetworkName)
        appendTomlString(sb, "network_secret", etNetworkSecret, writeEmpty = true)

        val peerList = splitPeers(etPeers).ifEmpty { listOf(DEFAULT_ET_PEERS) }
        for (peer in peerList) {
            sb.append("\n[[peer]]\n")
            appendTomlString(sb, "uri", peer)
        }

        for (spec in etForwards.values) {
            for (tcpPort in spec.portSet.tcpPorts()) {
                sb.append("\n[[port_forward]]\n")
                appendTomlString(sb, "bind_addr", "${spec.localHost}:$tcpPort")
                appendTomlString(sb, "dst_addr", "${spec.remoteIp}:$tcpPort")
                appendTomlString(sb, "proto", "tcp")
            }
            for (udpPort in spec.portSet.udpPorts()) {
                sb.append("\n[[port_forward]]\n")
                appendTomlString(sb, "bind_addr", "${spec.localHost}:$udpPort")
                appendTomlString(sb, "dst_addr", "${spec.remoteIp}:$udpPort")
                appendTomlString(sb, "proto", "udp")
            }
        }

        sb.append("\n[flags]\n")
        sb.append("no_tun = true\n")
        sb.append("use_smoltcp = true\n")
        sb.append("latency_first = true\n")
        sb.append("enable_ipv6 = true\n")
        return sb.toString()
    }

    private fun pollEasyTierStatus() {
        val running = synchronized(etLock) { etRunning }
        if (!running || !etAvailable) return

        val infosJson = runCatching { EasyTierJNI.collectNetworkInfos() }.getOrNull()
        if (infosJson.isNullOrEmpty()) return

        runCatching {
            val root = JSONObject(infosJson)
            val map = root.optJSONObject("map") ?: return
            val inst = map.optJSONObject(ET_INSTANCE_NAME) ?: run {
                val k = map.keys()
                if (k.hasNext()) map.optJSONObject(k.next()) else null
            } ?: return

            var vip = ""
            val myNode = inst.optJSONObject("my_node_info")
            val v4 = myNode?.optJSONObject("virtual_ipv4")
            if (v4 != null) {
                val addrObj = v4.optJSONObject("address")
                if (addrObj != null) {
                    vip = ipFromInt(addrObj.optInt("addr"))
                }
            }

            val routes = inst.optJSONArray("routes")
            val peers = inst.optJSONArray("peers")
            val routeCount = routes?.length() ?: 0
            val peerCount = maxOf(routeCount, peers?.length() ?: 0)

            // Collect discovered peer virtual IPs and /32 proxy CIDRs to pre-warm standard Sunshine port forwards
            val discoveredIps = mutableListOf<String>()
            if (routes != null) {
                for (i in 0 until routes.length()) {
                    val r = routes.optJSONObject(i) ?: continue
                    val rV4 = r.optJSONObject("ipv4_addr")?.optJSONObject("address")
                    if (rV4 != null) {
                        val ip = ipFromInt(rV4.optInt("addr"))
                        if (ip.isNotEmpty() && ip != "0.0.0.0" && ip != vip) {
                            discoveredIps.add(ip)
                        }
                    }
                    val cidrs = r.optJSONArray("proxy_cidrs")
                    if (cidrs != null) {
                        for (j in 0 until cidrs.length()) {
                            val cidr = cidrs.optString(j).trim()
                            val slash = cidr.indexOf('/')
                            val hostIp = if (slash > 0) cidr.substring(0, slash) else cidr
                            val prefix = if (slash > 0) cidr.substring(slash + 1).toIntOrNull() ?: 32 else 32
                            if (prefix == 32 && hostIp.count { it == '.' } == 3 && hostIp != vip) {
                                discoveredIps.add(hostIp)
                            }
                        }
                    }
                }
            }

            var addedPrewarm = false
            if (discoveredIps.isNotEmpty()) {
                synchronized(etLock) {
                    val defaultPorts = EtPortSet(47989, 47984)
                    for (ip in discoveredIps.distinct()) {
                        val key = "$ip:47989:47984"
                        if (!etForwards.containsKey(key) && etForwards.size < 16) {
                            val lh = allocateEasyTierLoopback(ip)
                            etForwards[key] = EtForwardSpec(ip, lh, defaultPorts)
                            addedPrewarm = true
                        }
                    }
                }
            }
            if (addedPrewarm) {
                startOrReloadEasyTier(forceRestart = true)
            }

            val newState = if (vip.isNotEmpty() || peerCount > 0) "connected" else "starting"
            val changed = (newState != etState) || (vip != etVirtualIp) || (peerCount != etPeersOnline)
            etState = newState
            etVirtualIp = vip
            etPeersOnline = peerCount
            if (newState == "connected") etError = ""
            if (changed) notifyChanged()
        }
    }

    private fun ensureDaemonAndStartTailscale() {
        if (!ensureDaemonRunning()) {
            tsState = "error"
            if (tsError.isEmpty()) tsError = "无法启动内置 Tailscale 守护进程"
            notifyChanged()
            return
        }
        if (pendingStartTs) {
            pendingStartTs = false
            sendStartTailscale()
        }
    }

    private fun findTunnelDaemonExecutable(): File? {
        val nativeDir = appContext.applicationInfo.nativeLibraryDir?.let(::File)
        if (nativeDir != null) {
            val soFile = File(nativeDir, "libsunandmon_tunnel.so")
            if (soFile.isFile) return soFile
        }
        return null
    }

    private fun resolveActiveDnsServers(): String {
        return runCatching {
            val cm = appContext.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
                ?: return ""
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                val net = cm.activeNetwork ?: return ""
                val lp = cm.getLinkProperties(net) ?: return ""
                lp.dnsServers.mapNotNull { it.hostAddress?.takeIf { a -> '.' in a } }
                    .joinToString(",")
            } else {
                ""
            }
        }.getOrDefault("")
    }

    private fun ensureDaemonRunning(): Boolean {
        synchronized(daemonLock) {
            if (daemonReady && daemonProcess?.isAliveCompat() == true && daemonPort > 0) {
                return true
            }
            stopDaemonLocked()
            val exe = findTunnelDaemonExecutable() ?: return false
            val tokenBytes = ByteArray(24)
            SecureRandom().nextBytes(tokenBytes)
            val token = tokenBytes.joinToString("") { "%02x".format(it.toInt() and 0xFF) }
            val workDir = File(appContext.filesDir, "tunnel").apply { mkdirs() }

            val pb = ProcessBuilder(exe.absolutePath, "-listen", "127.0.0.1:0", "-watch-stdin")
                .directory(workDir)
            val env = pb.environment()
            env["SUNANDMON_TUNNEL_TOKEN"] = token
            env["HOME"] = workDir.absolutePath
            env["TMPDIR"] = appContext.cacheDir.absolutePath
            val dns = resolveActiveDnsServers()
            if (dns.isNotEmpty()) {
                env["SUNANDMON_DNS"] = dns
            }

            val proc = runCatching { pb.start() }.getOrElse { e ->
                LimeLog.warning("Failed to start libsunandmon_tunnel.so: ${e.message}")
                tsError = "启动隧道进程失败: ${e.message}"
                return false
            }

            val reader = BufferedReader(InputStreamReader(proc.inputStream, Charsets.UTF_8))
            var port = 0
            val deadline = System.currentTimeMillis() + 5000L
            while (System.currentTimeMillis() < deadline) {
                val line = runCatching { reader.readLine() }.getOrNull() ?: break
                if (line.startsWith("READY ")) {
                    port = line.removePrefix("READY ").trim().toIntOrNull() ?: 0
                    break
                }
            }
            if (port <= 0) {
                runCatching { proc.destroy() }
                return false
            }

            daemonProcess = proc
            daemonPort = port
            daemonToken = token
            daemonReady = true

            // Drain stdout/stderr in background so pipe never blocks
            Thread({
                runCatching {
                    val buf = CharArray(512)
                    while (reader.read(buf) >= 0) { /* drain */ }
                }
            }, "SunAndMon-Tunnel-Stdout").apply { isDaemon = true; start() }
            Thread({
                runCatching {
                    proc.errorStream.bufferedReader().forEachLine { line ->
                        LimeLog.info("sunandmon-tunnel: $line")
                    }
                }
            }, "SunAndMon-Tunnel-Stderr").apply { isDaemon = true; start() }

            return true
        }
    }

    private fun stopDaemonLocked() {
        daemonReady = false
        daemonPort = 0
        daemonToken = ""
        daemonProcess?.let { proc ->
            runCatching { proc.outputStream.close() }
            runCatching { proc.destroy() }
        }
        daemonProcess = null
    }

    private fun sendStartTailscale() {
        if (!daemonReady) return
        val stateDir = File(appContext.filesDir, "tunnel").apply { mkdirs() }.absolutePath
        val cfg = JSONObject()
            .put("provider", "tailscale")
            .put("stateDir", stateDir)
            .put("hostname", hostname.ifBlank { defaultHostname() })
            .put("controlUrl", tsControlUrl)
            .put("useWebOAuth", tsUseWebOAuth)
        if (!tsUseWebOAuth && tsAuthKey.isNotEmpty()) {
            cfg.put("authKey", tsAuthKey)
        }
        val resp = runCatching { daemonRequest("POST", "/v1/start", cfg) }.getOrElse { e ->
            tsState = "error"
            tsError = e.message ?: "启动 Tailscale 失败"
            notifyChanged()
            return
        }
        if (resp != null) {
            applyDaemonStatus(resp)
        }
    }

    private fun pollTailscaleStatus() {
        if (!daemonReady) return
        val resp = runCatching { daemonRequest("GET", "/v1/status", null) }.getOrNull() ?: return
        applyDaemonStatus(resp)
    }

    private fun applyDaemonStatus(root: JSONObject) {
        val providers = root.optJSONObject("providers") ?: return
        val ts = providers.optJSONObject("tailscale") ?: return
        val newState = ts.optString("state", "stopped")
        val newVip = ts.optString("virtualIp", "")
        val newAuth = ts.optString("authUrl", "")
        val newErr = ts.optString("error", "")
        val newPeers = ts.optInt("peersOnline", 0)

        val changed = (newState != tsState) || (newVip != tsVirtualIp) ||
            (newAuth != tsAuthUrl) || (newErr != tsError) || (newPeers != tsPeersOnline)
        tsState = newState
        tsVirtualIp = newVip
        tsAuthUrl = newAuth
        tsError = newErr
        tsPeersOnline = newPeers
        if (changed) notifyChanged()
    }

    private fun daemonRequest(method: String, path: String, body: JSONObject?): JSONObject? {
        val port = daemonPort
        val token = daemonToken
        if (port <= 0 || token.isEmpty()) return null
        val url = "http://127.0.0.1:$port$path"
        val reqBuilder = Request.Builder()
            .url(url)
            .header("Authorization", "Bearer $token")
        if (method == "POST") {
            val payload = (body?.toString() ?: "{}").toRequestBody("application/json".toMediaType())
            reqBuilder.post(payload)
        } else {
            reqBuilder.get()
        }
        httpClient.newCall(reqBuilder.build()).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            val json = runCatching { JSONObject(text) }.getOrNull()
            if (resp.code >= 400) {
                val msg = json?.optString("error")?.takeIf { it.isNotEmpty() }
                    ?: "HTTP ${resp.code}"
                throw IllegalStateException(msg)
            }
            return json
        }
    }

    private fun startPoller() {
        pollFuture?.cancel(false)
        pollFuture = executor.scheduleWithFixedDelay({
            runCatching {
                if (daemonReady && (tsState == "starting" || tsState == "needs_auth" || tsState == "connected")) {
                    pollTailscaleStatus()
                }
                if (etRunning) {
                    pollEasyTierStatus()
                }
            }
        }, 1200, 1200, TimeUnit.MILLISECONDS)
    }

    private fun saveSettings() {
        prefs.edit {
            putString(KEY_ROUTE_POLICY, routePolicy)
            putString(KEY_HOSTNAME, hostname)
            putBoolean(KEY_TS_AUTO, tsAutoConnect)
            putString(KEY_TS_CONTROL_URL, tsControlUrl)
            putString(KEY_TS_AUTH_KEY, tsAuthKey)
            putBoolean(KEY_TS_WEB_OAUTH, tsUseWebOAuth)
            putBoolean(KEY_ET_AUTO, etAutoConnect)
            putString(KEY_ET_NETWORK_NAME, etNetworkName)
            putString(KEY_ET_NETWORK_SECRET, etNetworkSecret)
            putString(KEY_ET_PEERS, etPeers)
            putString(KEY_ET_STATIC_IP, etStaticIp)
        }
    }

    private fun Process.isAliveCompat(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) isAlive
        else runCatching { exitValue(); false }.getOrDefault(true)

    companion object {
        private const val PREF_NAME = "sunandmon_tunnel"
        private const val KEY_ROUTE_POLICY = "routePolicy"
        private const val KEY_HOSTNAME = "hostname"
        private const val KEY_CENTER_DEFAULTS = "centerDefaultsJson"
        private const val KEY_TS_AUTO = "tsAutoConnect"
        private const val KEY_TS_CONTROL_URL = "tsControlUrl"
        private const val KEY_TS_AUTH_KEY = "tsAuthKey"
        private const val KEY_TS_WEB_OAUTH = "tsUseWebOAuth"
        private const val KEY_ET_AUTO = "etAutoConnect"
        private const val KEY_ET_NETWORK_NAME = "etNetworkName"
        private const val KEY_ET_NETWORK_SECRET = "etNetworkSecret"
        private const val KEY_ET_PEERS = "etPeers"
        private const val KEY_ET_STATIC_IP = "etVirtualIp"

        private const val ET_INSTANCE_NAME = "sunandmon"
        const val DEFAULT_ET_PEERS = "tcp://public.easytier.top:11010"

        @Volatile
        private var instance: AndroidTunnelManager? = null

        fun get(context: Context): AndroidTunnelManager {
            return instance ?: synchronized(this) {
                instance ?: AndroidTunnelManager(context).also { instance = it }
            }
        }

        private fun normalizeRoutePolicy(p: String): String {
            val v = p.trim().lowercase()
            return if (v in setOf("auto", "auto_ts", "prefer_ts", "prefer_et")) v else "auto"
        }

        private fun defaultHostname(): String {
            val raw = (Build.MODEL ?: "android").lowercase()
                .replace(Regex("[^a-z0-9-]+"), "-")
                .trim('-')
            return "sunandmon-${raw.ifEmpty { "android" }}"
        }

        private fun splitPeers(text: String): List<String> =
            text.split(Regex("[\\r\\n,;\\s]+"))
                .map { it.trim() }
                .filter { it.isNotEmpty() }
                .distinct()

        private fun appendTomlString(
            sb: StringBuilder,
            key: String,
            value: String,
            writeEmpty: Boolean = false
        ) {
            if (writeEmpty || value.isNotEmpty()) {
                val escaped = value.replace("\\", "\\\\").replace("\"", "\\\"")
                sb.append(key).append(" = \"").append(escaped).append("\"\n")
            }
        }

        private fun ipFromInt(addr: Int): String =
            "${(addr ushr 24) and 0xFF}.${(addr ushr 16) and 0xFF}.${(addr ushr 8) and 0xFF}.${addr and 0xFF}"
    }
}
