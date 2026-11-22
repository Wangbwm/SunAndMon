package com.limelight.ui

import android.app.AlertDialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.content.ContextCompat
import com.limelight.R
import com.limelight.computers.AndroidTunnelManager
import com.limelight.utils.AppTheme
import kotlin.math.roundToInt
import org.json.JSONObject

internal class TunnelSettingsDialog(
    private val context: Context,
    private val tunnel: AndroidTunnelManager,
    private val centerDefaultsProvider: () -> JSONObject?
) {
    private val palette = AppTheme.configurationContext(context)
    private val ink = color(R.color.center_ink)
    private val muted = color(R.color.center_muted)
    private val line = color(R.color.center_line)
    private val accent = color(R.color.center_accent)
    private val sky = color(R.color.center_selected)
    private val paper = color(R.color.center_canvas)
    private val surface = color(R.color.center_surface)
    private val green = color(R.color.center_online_ink)
    private val onAccent = color(R.color.center_on_accent)
    private val danger = Color.parseColor("#E5534B")
    private val rippleColor = ColorStateList.valueOf(color(R.color.center_ripple))

    private var activeTab: String = run {
        val defP = (centerDefaultsProvider() ?: tunnel.centerDefaults)
            ?.optString("provider")?.trim()?.lowercase().orEmpty()
        when {
            tunnel.etState == "connected" && tunnel.tsState != "connected" -> "easytier"
            tunnel.tsState == "connected" && tunnel.etState != "connected" -> "tailscale"
            defP == "easytier" || tunnel.etNetworkName.isNotEmpty() -> "easytier"
            else -> "tailscale"
        }
    }
    private var useWebOAuth: Boolean = tunnel.tsUseWebOAuth
    private var hadBannerOnBuild: Boolean = false

    private var dialog: AlertDialog? = null
    private lateinit var rootContainer: LinearLayout

    // Input fields preserved across live status refreshes
    private var hostnameEdit: EditText? = null
    private var tsAutoCheck: CheckBox? = null
    private var tsControlEdit: EditText? = null
    private var tsAuthKeyEdit: EditText? = null
    private var etAutoCheck: CheckBox? = null
    private var etNameEdit: EditText? = null
    private var etSecretEdit: EditText? = null
    private var etPeersEdit: EditText? = null
    private var etStaticIpEdit: EditText? = null

    // Live status views updated in-place without losing focus on inputs
    private var tsTabBtn: TextView? = null
    private var etTabBtn: TextView? = null
    private var tsStatusDot: View? = null
    private var tsStatusTitle: TextView? = null
    private var tsPeersLabel: TextView? = null
    private var tsVipLabel: TextView? = null
    private var tsErrorLabel: TextView? = null
    private var tsAuthRow: LinearLayout? = null
    private var tsAuthUrlLabel: TextView? = null
    private var tsDisconnectBtn: TextView? = null

    private var etStatusDot: View? = null
    private var etStatusTitle: TextView? = null
    private var etPeersLabel: TextView? = null
    private var etVipLabel: TextView? = null
    private var etErrorLabel: TextView? = null
    private var etDisconnectBtn: TextView? = null

    private val tunnelListener: () -> Unit = {
        if (dialog?.isShowing == true) {
            val defaults = centerDefaultsProvider() ?: tunnel.centerDefaults
            val hasBannerNow = defaults != null &&
                defaults.optString("provider").trim().lowercase() in setOf("easytier", "tailscale")
            val etJustPopulated = etNameEdit?.text?.toString()?.isBlank() == true && tunnel.etNetworkName.isNotEmpty()
            if ((hasBannerNow && !hadBannerOnBuild) || etJustPopulated) {
                rebuildContent(syncFieldsFromManager = true)
            } else {
                refreshStatusViews()
            }
        }
    }

    fun show() {
        rootContainer = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(paper)
            setPadding(dp(18), dp(14), dp(18), dp(14))
        }
        val scroll = ScrollView(context).apply {
            isFillViewport = true
            setBackgroundColor(paper)
            addView(
                rootContainer,
                ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            )
        }

        rebuildContent(syncFieldsFromManager = true)

        val dlg = AlertDialog.Builder(context)
            .setView(scroll)
            .create()
        dlg.setOnDismissListener {
            tunnel.removeListener(tunnelListener)
        }
        tunnel.addListener(tunnelListener)
        dlg.show()
        dlg.window?.clearFlags(
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM
        )
        dlg.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        dialog = dlg
    }

    private fun rebuildContent(syncFieldsFromManager: Boolean) {
        val savedHostname = if (syncFieldsFromManager) tunnel.hostname else hostnameEdit?.text?.toString().orEmpty()
        val savedTsAuto = if (syncFieldsFromManager) tunnel.tsAutoConnect else (tsAutoCheck?.isChecked ?: tunnel.tsAutoConnect)
        val savedTsControl = if (syncFieldsFromManager) tunnel.tsControlUrl else tsControlEdit?.text?.toString().orEmpty()
        val savedTsAuthKey = if (syncFieldsFromManager) tunnel.tsAuthKey else tsAuthKeyEdit?.text?.toString().orEmpty()
        val savedEtAuto = if (syncFieldsFromManager) tunnel.etAutoConnect else (etAutoCheck?.isChecked ?: tunnel.etAutoConnect)
        val savedEtName = if (syncFieldsFromManager) tunnel.etNetworkName else etNameEdit?.text?.toString().orEmpty()
        val savedEtSecret = if (syncFieldsFromManager) tunnel.etNetworkSecret else etSecretEdit?.text?.toString().orEmpty()
        val savedEtPeers = if (syncFieldsFromManager) tunnel.etPeers else etPeersEdit?.text?.toString().orEmpty()
        val savedEtStaticIp = if (syncFieldsFromManager) tunnel.etStaticIp else etStaticIpEdit?.text?.toString().orEmpty()
        if (syncFieldsFromManager) {
            useWebOAuth = tunnel.tsUseWebOAuth
        }

        rootContainer.removeAllViews()

        // Title header + close button
        val header = row()
        header.addView(
            label(context.getString(R.string.center_builtin_tunnel), 20, bold = true),
            LinearLayout.LayoutParams(0, -2, 1f)
        )
        header.addView(
            pillButton(context.getString(android.R.string.cancel), primary = false) {
                dialog?.dismiss()
            }
        )
        rootContainer.addView(header)
        gap(rootContainer, 6)

        rootContainer.addView(
            label(context.getString(R.string.center_tunnel_subtitle), 12, color = muted)
        )
        gap(rootContainer, 12)

        // 1. Route priority policy card
        val policyCard = card()
        policyCard.addView(
            label(context.getString(R.string.center_tunnel_route_policy), 13, bold = true)
        )
        gap(policyCard, 8)
        val row1 = row()
        row1.addView(
            policyBtn(
                context.getString(R.string.center_route_auto_et),
                tunnel.routePolicy == "auto"
            ) {
                tunnel.updateRoutePolicy("auto")
                rebuildContent(syncFieldsFromManager = false)
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(4) }
        )
        row1.addView(
            policyBtn(
                context.getString(R.string.center_route_auto_ts),
                tunnel.routePolicy == "auto_ts"
            ) {
                tunnel.updateRoutePolicy("auto_ts")
                rebuildContent(syncFieldsFromManager = false)
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(4) }
        )
        policyCard.addView(row1)
        gap(policyCard, 6)
        val row2 = row()
        row2.addView(
            policyBtn(
                context.getString(R.string.center_route_prefer_et),
                tunnel.routePolicy == "prefer_et"
            ) {
                tunnel.updateRoutePolicy("prefer_et")
                rebuildContent(syncFieldsFromManager = false)
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(4) }
        )
        row2.addView(
            policyBtn(
                context.getString(R.string.center_route_prefer_ts),
                tunnel.routePolicy == "prefer_ts"
            ) {
                tunnel.updateRoutePolicy("prefer_ts")
                rebuildContent(syncFieldsFromManager = false)
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(4) }
        )
        policyCard.addView(row2)
        rootContainer.addView(policyCard)
        gap(rootContainer, 10)

        // 2. Center defaults banner if available
        val defaults = centerDefaultsProvider() ?: tunnel.centerDefaults
        val defProvider = defaults?.optString("provider")?.trim()?.lowercase().orEmpty()
        hadBannerOnBuild = defaults != null && (defProvider == "easytier" || defProvider == "tailscale")
        if (hadBannerOnBuild && defaults != null) {
            val banner = row().apply {
                setPadding(dp(10), dp(8), dp(10), dp(8))
                background = shape(sky, accent, radius = 10)
            }
            val providerName = if (defProvider == "easytier") "EasyTier" else "Tailscale / Headscale"
            banner.addView(
                label(
                    context.getString(R.string.center_tunnel_defaults_banner, providerName),
                    12,
                    color = ink
                ),
                LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(8) }
            )
            banner.addView(
                pillButton(context.getString(R.string.center_tunnel_load_defaults), primary = true) {
                    tunnel.applyCenterDefaults(defaults, autoStartIfIdle = false)
                    activeTab = defProvider
                    rebuildContent(syncFieldsFromManager = true)
                }
            )
            rootContainer.addView(banner)
            gap(rootContainer, 10)
        }

        // 3. Shared hostname
        rootContainer.addView(label(context.getString(R.string.center_tunnel_hostname), 12, color = muted))
        gap(rootContainer, 4)
        hostnameEdit = inputField(
            savedHostname,
            context.getString(R.string.center_tunnel_hostname_hint)
        )
        rootContainer.addView(hostnameEdit)
        gap(rootContainer, 12)

        // 4. Dual provider tab strip (Tailscale / Headscale | EasyTier)
        val tabRow = row()
        tsTabBtn = policyBtn(
            tsTabText(),
            activeTab == "tailscale"
        ) {
            activeTab = "tailscale"
            rebuildContent(syncFieldsFromManager = false)
        }
        etTabBtn = policyBtn(
            etTabText(),
            activeTab == "easytier"
        ) {
            activeTab = "easytier"
            rebuildContent(syncFieldsFromManager = false)
        }
        tabRow.addView(tsTabBtn, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(4) })
        tabRow.addView(etTabBtn, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(4) })
        rootContainer.addView(tabRow)
        gap(rootContainer, 10)

        if (activeTab == "tailscale") {
            buildTailscalePanel(savedTsAuto, savedTsControl, savedTsAuthKey)
        } else {
            buildEasyTierPanel(savedEtAuto, savedEtName, savedEtSecret, savedEtPeers, savedEtStaticIp)
        }

        refreshStatusViews()
    }

    private fun buildTailscalePanel(savedAuto: Boolean, savedControl: String, savedAuthKey: String) {
        val statusCard = card()
        val topRow = row()
        tsStatusDot = View(context).apply {
            layoutParams = LinearLayout.LayoutParams(dp(9), dp(9)).apply { marginEnd = dp(8) }
        }
        tsStatusTitle = label("", 13, bold = true)
        tsPeersLabel = label("", 12, color = muted)
        topRow.addView(tsStatusDot)
        topRow.addView(tsStatusTitle, LinearLayout.LayoutParams(0, -2, 1f))
        topRow.addView(tsPeersLabel)
        statusCard.addView(topRow)

        tsVipLabel = label("", 12, color = muted).apply {
            setPadding(0, dp(4), 0, 0)
        }
        statusCard.addView(tsVipLabel)

        tsErrorLabel = label("", 12, color = danger).apply {
            setPadding(0, dp(4), 0, 0)
        }
        statusCard.addView(tsErrorLabel)

        tsAuthRow = column().apply {
            setPadding(0, dp(8), 0, 0)
            val btnRow = row()
            btnRow.addView(
                pillButton(context.getString(R.string.center_ts_open_browser), primary = true) {
                    tunnel.openTailscaleAuthUrl(context)
                },
                LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(4) }
            )
            btnRow.addView(
                pillButton("复制授权链接", primary = false) {
                    if (tunnel.copyTailscaleAuthUrl(context)) {
                        Toast.makeText(context, "已复制授权链接", Toast.LENGTH_SHORT).show()
                    }
                },
                LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(4) }
            )
            addView(btnRow)
            tsAuthUrlLabel = label("", 11, color = muted).apply {
                setPadding(dp(8), dp(6), dp(8), dp(6))
                setTextIsSelectable(true)
                setOnClickListener {
                    if (tunnel.copyTailscaleAuthUrl(context)) {
                        Toast.makeText(context, "已复制授权链接", Toast.LENGTH_SHORT).show()
                    }
                }
            }
            addView(tsAuthUrlLabel)
        }
        statusCard.addView(tsAuthRow)
        rootContainer.addView(statusCard)
        gap(rootContainer, 10)

        tsAutoCheck = CheckBox(context).apply {
            text = context.getString(R.string.center_ts_auto_connect)
            setTextColor(ink)
            textSize = 13f
            isChecked = savedAuto
        }
        rootContainer.addView(tsAutoCheck)
        gap(rootContainer, 8)

        rootContainer.addView(label(context.getString(R.string.center_ts_control_url), 12, color = muted))
        gap(rootContainer, 4)
        tsControlEdit = inputField(
            savedControl,
            context.getString(R.string.center_ts_control_hint)
        )
        rootContainer.addView(tsControlEdit)
        gap(rootContainer, 10)

        rootContainer.addView(label(context.getString(R.string.center_ts_auth_method), 12, color = muted))
        gap(rootContainer, 4)
        val authModeRow = row()
        authModeRow.addView(
            policyBtn(context.getString(R.string.center_ts_web_oauth), useWebOAuth) {
                useWebOAuth = true
                rebuildContent(syncFieldsFromManager = false)
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(4) }
        )
        authModeRow.addView(
            policyBtn(context.getString(R.string.center_ts_auth_key), !useWebOAuth) {
                useWebOAuth = false
                rebuildContent(syncFieldsFromManager = false)
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(4) }
        )
        rootContainer.addView(authModeRow)
        gap(rootContainer, 8)

        tsAuthKeyEdit = inputField(
            savedAuthKey,
            "tskey-auth-…",
            password = true
        ).apply {
            visibility = if (useWebOAuth) View.GONE else View.VISIBLE
        }
        rootContainer.addView(tsAuthKeyEdit)
        if (!useWebOAuth) gap(rootContainer, 10) else gap(rootContainer, 4)

        val actions = row()
        actions.addView(
            pillButton(context.getString(R.string.center_ts_connect_btn), primary = true) {
                tunnel.saveAndConnectTailscale(
                    autoConnect = tsAutoCheck?.isChecked ?: false,
                    newHostname = hostnameEdit?.text?.toString().orEmpty(),
                    controlUrl = tsControlEdit?.text?.toString().orEmpty(),
                    authKey = tsAuthKeyEdit?.text?.toString().orEmpty(),
                    useWebOAuth = useWebOAuth
                )
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(6) }
        )
        tsDisconnectBtn = pillButton(context.getString(R.string.center_tunnel_disconnect), primary = false) {
            tunnel.stopTailscale()
        }
        actions.addView(tsDisconnectBtn)
        rootContainer.addView(actions)
    }

    private fun buildEasyTierPanel(
        savedAuto: Boolean,
        savedName: String,
        savedSecret: String,
        savedPeers: String,
        savedStaticIp: String
    ) {
        val statusCard = card()
        val topRow = row()
        etStatusDot = View(context).apply {
            layoutParams = LinearLayout.LayoutParams(dp(9), dp(9)).apply { marginEnd = dp(8) }
        }
        etStatusTitle = label("", 13, bold = true)
        etPeersLabel = label("", 12, color = muted)
        topRow.addView(etStatusDot)
        topRow.addView(etStatusTitle, LinearLayout.LayoutParams(0, -2, 1f))
        topRow.addView(etPeersLabel)
        statusCard.addView(topRow)

        etVipLabel = label("", 12, color = muted).apply {
            setPadding(0, dp(4), 0, 0)
        }
        statusCard.addView(etVipLabel)

        etErrorLabel = label("", 12, color = danger).apply {
            setPadding(0, dp(4), 0, 0)
        }
        statusCard.addView(etErrorLabel)
        rootContainer.addView(statusCard)
        gap(rootContainer, 10)

        etAutoCheck = CheckBox(context).apply {
            text = context.getString(R.string.center_et_auto_connect)
            setTextColor(ink)
            textSize = 13f
            isChecked = savedAuto
        }
        rootContainer.addView(etAutoCheck)
        gap(rootContainer, 8)

        rootContainer.addView(label(context.getString(R.string.center_et_network_name), 12, color = muted))
        gap(rootContainer, 4)
        etNameEdit = inputField(savedName, "sunandmon-mesh")
        rootContainer.addView(etNameEdit)
        gap(rootContainer, 8)

        rootContainer.addView(label(context.getString(R.string.center_et_network_secret), 12, color = muted))
        gap(rootContainer, 4)
        etSecretEdit = inputField(
            savedSecret,
            context.getString(R.string.center_et_secret_hint),
            password = true
        )
        rootContainer.addView(etSecretEdit)
        gap(rootContainer, 8)

        rootContainer.addView(label(context.getString(R.string.center_et_peers), 12, color = muted))
        gap(rootContainer, 4)
        etPeersEdit = inputField(
            savedPeers.ifEmpty { AndroidTunnelManager.DEFAULT_ET_PEERS },
            AndroidTunnelManager.DEFAULT_ET_PEERS,
            multiLine = true
        )
        rootContainer.addView(etPeersEdit)
        gap(rootContainer, 8)

        rootContainer.addView(label(context.getString(R.string.center_et_static_ip), 12, color = muted))
        gap(rootContainer, 4)
        etStaticIpEdit = inputField(
            savedStaticIp,
            context.getString(R.string.center_et_static_ip_hint)
        )
        rootContainer.addView(etStaticIpEdit)
        gap(rootContainer, 12)

        val actions = row()
        actions.addView(
            pillButton(context.getString(R.string.center_et_connect_btn), primary = true) {
                tunnel.saveAndConnectEasyTier(
                    autoConnect = etAutoCheck?.isChecked ?: false,
                    newHostname = hostnameEdit?.text?.toString().orEmpty(),
                    networkName = etNameEdit?.text?.toString().orEmpty(),
                    networkSecret = etSecretEdit?.text?.toString().orEmpty(),
                    peers = etPeersEdit?.text?.toString().orEmpty(),
                    staticIp = etStaticIpEdit?.text?.toString().orEmpty()
                )
            },
            LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(6) }
        )
        etDisconnectBtn = pillButton(context.getString(R.string.center_tunnel_disconnect), primary = false) {
            tunnel.stopEasyTier()
        }
        actions.addView(etDisconnectBtn)
        rootContainer.addView(actions)
    }

    private fun refreshStatusViews() {
        tsTabBtn?.text = tsTabText()
        etTabBtn?.text = etTabText()

        // Tailscale status views
        val tsDotColor = when (tunnel.tsState) {
            "connected" -> green
            "needs_auth", "starting" -> accent
            "error" -> danger
            else -> muted
        }
        tsStatusDot?.background = shape(tsDotColor, stroke = null, radius = 99)
        tsStatusTitle?.text = when {
            !tunnel.tsAvailable -> context.getString(R.string.center_tunnel_unavailable)
            tunnel.tsState == "connected" -> context.getString(R.string.center_ts_connected)
            tunnel.tsState == "needs_auth" -> context.getString(R.string.center_ts_waiting_auth)
            tunnel.tsState == "starting" -> context.getString(R.string.center_tunnel_connecting)
            tunnel.tsState == "error" -> context.getString(R.string.center_tunnel_error)
            else -> context.getString(R.string.center_tunnel_disconnected)
        }
        tsPeersLabel?.apply {
            visibility = if (tunnel.tsState == "connected") View.VISIBLE else View.GONE
            text = context.getString(R.string.center_tunnel_peers_count, tunnel.tsPeersOnline)
        }
        tsVipLabel?.apply {
            visibility = if (tunnel.tsVirtualIp.isNotEmpty()) View.VISIBLE else View.GONE
            text = context.getString(R.string.center_tunnel_virtual_ip, tunnel.tsVirtualIp)
        }
        tsErrorLabel?.apply {
            visibility = if (tunnel.tsError.isNotEmpty()) View.VISIBLE else View.GONE
            text = tunnel.tsError
        }
        tsAuthRow?.visibility = if (tunnel.tsAuthUrl.isNotEmpty()) View.VISIBLE else View.GONE
        tsAuthUrlLabel?.text = tunnel.tsAuthUrl

        // EasyTier status views
        val etDotColor = when (tunnel.etState) {
            "connected" -> green
            "starting" -> accent
            "error" -> danger
            else -> muted
        }
        etStatusDot?.background = shape(etDotColor, stroke = null, radius = 99)
        etStatusTitle?.text = when {
            !tunnel.etAvailable -> context.getString(R.string.center_tunnel_unavailable)
            tunnel.etState == "connected" -> context.getString(R.string.center_et_connected)
            tunnel.etState == "starting" -> context.getString(R.string.center_tunnel_connecting)
            tunnel.etState == "error" -> context.getString(R.string.center_tunnel_error)
            else -> context.getString(R.string.center_tunnel_disconnected)
        }
        etPeersLabel?.apply {
            visibility = if (tunnel.etState == "connected") View.VISIBLE else View.GONE
            text = context.getString(R.string.center_tunnel_peers_count, tunnel.etPeersOnline)
        }
        etVipLabel?.apply {
            visibility = if (tunnel.etVirtualIp.isNotEmpty()) View.VISIBLE else View.GONE
            text = context.getString(R.string.center_tunnel_virtual_ip, tunnel.etVirtualIp)
        }
        etErrorLabel?.apply {
            visibility = if (tunnel.etError.isNotEmpty()) View.VISIBLE else View.GONE
            text = tunnel.etError
        }
    }

    private fun tsTabText(): String =
        "Tailscale / Headscale" + if (tunnel.tsState == "connected") "  ●" else ""

    private fun etTabText(): String =
        "EasyTier" + if (tunnel.etState == "connected") "  ●" else ""

    private fun card(): LinearLayout = column().apply {
        setPadding(dp(12), dp(10), dp(12), dp(10))
        background = shape(surface, line, radius = 10)
    }

    private fun inputField(
        initialValue: String,
        hintText: String,
        password: Boolean = false,
        multiLine: Boolean = false
    ): EditText = EditText(context).apply {
        setText(initialValue)
        hint = hintText
        setTextColor(ink)
        setHintTextColor(muted)
        textSize = 13f
        setPadding(dp(11), dp(9), dp(11), dp(9))
        background = shape(surface, line, radius = 8)
        inputType = when {
            password -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            multiLine -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or InputType.TYPE_TEXT_VARIATION_URI
            else -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
        }
        if (multiLine) {
            minLines = 2
            maxLines = 4
            gravity = Gravity.TOP or Gravity.START
        } else {
            setSingleLine(true)
        }
    }

    private fun policyBtn(text: String, selected: Boolean, onClick: () -> Unit): TextView =
        label(text, 12, bold = selected, color = if (selected) accent else ink).apply {
            gravity = Gravity.CENTER
            minHeight = dp(38)
            setPadding(dp(10), dp(6), dp(10), dp(6))
            val fill = if (selected) sky else surface
            val border = if (selected) accent else line
            background = RippleDrawable(rippleColor, shape(fill, border, radius = 8), shape(Color.WHITE))
            isClickable = true
            isFocusable = true
            setOnClickListener { onClick() }
        }

    private fun pillButton(text: String, primary: Boolean, onClick: () -> Unit): TextView =
        label(text, 13, bold = true, color = if (primary) onAccent else ink).apply {
            gravity = Gravity.CENTER
            minHeight = dp(42)
            setPadding(dp(14), dp(8), dp(14), dp(8))
            val fill = if (primary) accent else surface
            val border = if (primary) null else line
            background = RippleDrawable(rippleColor, shape(fill, border, radius = 9), shape(Color.WHITE))
            isClickable = true
            isFocusable = true
            setOnClickListener { onClick() }
        }

    private fun column() = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private fun row() = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun label(text: String, size: Int, bold: Boolean = false, color: Int = ink) = TextView(context).apply {
        this.text = text
        textSize = size.toFloat()
        setTextColor(color)
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }

    private fun shape(fill: Int, stroke: Int? = line, radius: Int = 10) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(radius).toFloat()
        if (stroke != null) setStroke(dp(1), stroke)
    }

    private fun gap(parent: LinearLayout, size: Int) {
        parent.addView(View(context), LinearLayout.LayoutParams(1, dp(size)))
    }

    private fun dp(value: Int) = (value * context.resources.displayMetrics.density).roundToInt()

    private fun color(resource: Int) = ContextCompat.getColor(palette, resource)
}
