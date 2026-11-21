package com.limelight.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.limelight.R
import com.limelight.computers.CenterDirectory
import com.limelight.nvstream.http.ComputerDetails
import com.limelight.nvstream.http.PairingManager.PairState
import com.limelight.utils.AppTheme
import kotlin.math.roundToInt

internal class CenterDeviceView(
    context: Context,
    private val refresh: () -> Unit,
    private val centerSettings: () -> Unit,
    private val addLanComputer: () -> Unit,
    private val refreshLan: () -> Unit,
    private val streamSettings: () -> Unit,
    private val deviceAction: (CenterDirectory.Device, Action) -> Unit,
    private val lanDeviceAction: (ComputerDetails, LanAction) -> Unit,
) : FrameLayout(context) {
    enum class Page { CENTER, LAN }
    enum class Action { DESKTOP, APPS, SECONDARY_SCREEN, DEVICE_OPTIONS }
    enum class LanAction { DESKTOP, APPS, SECONDARY_SCREEN, PAIR, WAKE, DEVICE_OPTIONS }

    private val palette = AppTheme.configurationContext(context)
    private val ink = color(R.color.center_ink)
    private val muted = color(R.color.center_muted)
    private val line = color(R.color.center_line)
    private val accent = color(R.color.center_accent)
    private val sky = color(R.color.center_selected)
    private val paper = color(R.color.center_canvas)
    private val surface = color(R.color.center_surface)
    private val green = color(R.color.center_online_ink)
    private val greenFill = color(R.color.center_online_fill)
    private val onAccent = color(R.color.center_on_accent)
    private val rippleColor = ColorStateList.valueOf(color(R.color.center_ripple))
    private val root = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }

    private var currentPage = Page.CENTER
    private var devices = emptyList<CenterDirectory.Device>()
    private var lanDevices = emptyList<ComputerDetails>()
    private var lanSnapshot: List<String> = emptyList()
    private var connected = false
    private var selectedId: String? = null
    private var selectedLanUuid: String? = null
    private var loading = false

    init {
        setBackgroundColor(paper)
        addView(root, LayoutParams(-1, -1))
        ViewCompat.setOnApplyWindowInsetsListener(this) { view, insets ->
            val safe = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(safe.left, safe.top, safe.right, safe.bottom)
            insets
        }
    }

    fun update(devices: List<CenterDirectory.Device>, connected: Boolean, loading: Boolean = false) {
        this.devices = devices
        this.connected = connected
        this.loading = loading
        if (currentPage == Page.CENTER) {
            render()
        }
    }

    fun updateLanDevices(computers: List<ComputerDetails>) {
        val snapshot = computers.map { c ->
            val addr = bestLanAddress(c)
            "${c.uuid}|${c.name}|${c.state}|${c.pairState}|${c.runningGameId}|${c.macAddress}|$addr"
        }
        val changed = snapshot != lanSnapshot
        this.lanDevices = computers
        this.lanSnapshot = snapshot
        if (currentPage == Page.LAN && changed) {
            render()
        }
    }

    fun backToDevices(): Boolean {
        if (currentPage == Page.CENTER && selectedId != null) {
            selectedId = null
            render()
            return true
        }
        if (currentPage == Page.LAN && selectedLanUuid != null) {
            selectedLanUuid = null
            render()
            return true
        }
        if (currentPage == Page.LAN) {
            currentPage = Page.CENTER
            render()
            return true
        }
        return false
    }

    private fun switchPage(target: Page) {
        if (currentPage == target) {
            if (target == Page.CENTER) {
                selectedId = null
                refresh()
            } else {
                selectedLanUuid = null
                refreshLan()
                render()
            }
            return
        }
        currentPage = target
        selectedId = null
        selectedLanUuid = null
        if (target == Page.LAN) {
            refreshLan()
        }
        render()
    }

    private fun render() {
        root.removeAllViews()
        val content = column().apply { setPadding(dp(20), dp(12), dp(20), dp(18)) }
        val scroll = ScrollView(context).apply {
            isFillViewport = true
            clipToPadding = false
            addView(content, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        root.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))

        when (currentPage) {
            Page.CENTER -> {
                val selected = devices.firstOrNull { it.id == selectedId }
                if (selected == null) {
                    selectedId = null
                    showDevices(content)
                } else {
                    showDetails(content, selected)
                }
            }
            Page.LAN -> {
                val selected = lanDevices.firstOrNull { it.uuid != null && it.uuid == selectedLanUuid }
                if (selected == null) {
                    selectedLanUuid = null
                    showLanDevices(content)
                } else {
                    showLanDetails(content, selected)
                }
            }
        }
    }

    private fun showDevices(content: LinearLayout) {
        val header = row()
        header.addView(label(R.string.center_my_devices, 26, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
        header.addView(button(R.string.center_refresh, enabled = !loading, listener = refresh))
        content.addView(header)
        gap(content, 14)

        val center = row().apply {
            setPadding(dp(12), 0, dp(12), 0)
            action(sky, stroke = null, listener = centerSettings)
        }
        center.addView(icon(R.drawable.ic_center_server))
        center.addView(label(R.string.center_server, 15), LinearLayout.LayoutParams(0, -2, 1f))
        center.addView(if (connected) status(R.string.center_connected, online = true)
            else label(R.string.center_sign_in, 13, color = accent))
        center.addView(chevron())
        content.addView(center, LinearLayout.LayoutParams(-1, dp(52)))
        gap(content, 22)

        if (devices.isEmpty()) {
            val emptyText = when {
                loading -> R.string.center_loading
                connected -> R.string.center_empty
                else -> R.string.center_sign_in_hint
            }
            content.addView(label(emptyText, 14, color = muted))
        } else {
            val caption = row()
            caption.addView(label(R.string.center_all_devices, 13, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
            val online = devices.count { it.online }
            caption.addView(label(0, 13, color = muted).apply {
                text = resources.getQuantityString(R.plurals.center_online_count, online, online)
            })
            content.addView(caption)
            gap(content, 10)
            devices.forEach { device ->
                val tile = row().apply {
                    setPadding(dp(12), 0, dp(12), 0)
                    action(surface) {
                        selectedId = device.id
                        render()
                    }
                }
                tile.addView(icon(R.drawable.ic_center_monitor, tile = true))
                val name = column()
                name.addView(label(0, 15, bold = true).apply { text = device.name })
                name.addView(status(deviceStatus(device.online), device.online))
                tile.addView(name, LinearLayout.LayoutParams(0, -2, 1f))
                tile.addView(chevron())
                content.addView(tile, LinearLayout.LayoutParams(-1, -2))
                tile.minimumHeight = dp(68)
                gap(content, 9)
            }
        }
        footer(bottomTabs())
    }

    private fun showLanDevices(content: LinearLayout) {
        val header = row()
        header.addView(label(R.string.center_local_network, 26, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
        header.addView(button(R.string.center_refresh, listener = refreshLan))
        content.addView(header)
        gap(content, 14)

        val addStrip = row().apply {
            setPadding(dp(12), 0, dp(12), 0)
            action(sky, stroke = null, listener = addLanComputer)
        }
        addStrip.addView(icon(R.drawable.ic_center_add))
        addStrip.addView(label(R.string.center_add_pc, 15), LinearLayout.LayoutParams(0, -2, 1f))
        addStrip.addView(label(R.string.center_add_action, 13, color = accent))
        addStrip.addView(chevron())
        content.addView(addStrip, LinearLayout.LayoutParams(-1, dp(52)))
        gap(content, 22)

        if (lanDevices.isEmpty()) {
            content.addView(label(R.string.center_lan_empty, 14, color = muted))
        } else {
            val caption = row()
            caption.addView(label(R.string.center_all_devices, 13, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
            val online = lanDevices.count { it.state == ComputerDetails.State.ONLINE }
            caption.addView(label(0, 13, color = muted).apply {
                text = resources.getQuantityString(R.plurals.center_online_count, online, online)
            })
            content.addView(caption)
            gap(content, 10)
            lanDevices.forEach { computer ->
                val tile = row().apply {
                    setPadding(dp(12), 0, dp(12), 0)
                    action(surface) {
                        selectedLanUuid = computer.uuid
                        render()
                    }
                }
                tile.addView(icon(R.drawable.ic_center_monitor, tile = true))
                val info = column()
                info.addView(label(0, 15, bold = true).apply { text = computer.name.orEmpty() })
                val (statusRes, dotColor) = lanStatusSpec(computer)
                info.addView(statusWithColor(statusRes, dotColor))
                tile.addView(info, LinearLayout.LayoutParams(0, -2, 1f))
                tile.addView(chevron())
                content.addView(tile, LinearLayout.LayoutParams(-1, -2))
                tile.minimumHeight = dp(68)
                gap(content, 9)
            }
        }
        footer(bottomTabs())
    }

    private fun showDetails(content: LinearLayout, device: CenterDirectory.Device) {
        content.addView(backLink(R.string.center_my_devices) { backToDevices() }, LinearLayout.LayoutParams(-2, -2))
        gap(content, 8)
        val heading = row()
        heading.addView(label(0, 26, bold = true).apply { text = device.name }, LinearLayout.LayoutParams(0, -2, 1f))
        heading.addView(status(deviceStatus(device.online), device.online).apply {
            setPadding(dp(9), dp(4), dp(9), dp(4))
            background = shape(if (device.online) greenFill else sky, radius = 7)
        }, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(10) })
        content.addView(heading)
        gap(content, 16)
        content.addView(desktopCoverCard())
        gap(content, 20)
        content.addView(label(R.string.center_common_actions, 13, color = muted))
        gap(content, 10)
        val ready = device.online && !loading
        addDetailAction(content, R.string.center_app_list, R.drawable.ic_center_apps, ready) { deviceAction(device, Action.APPS) }
        addDetailAction(content, R.string.center_secondary_screen, R.drawable.ic_screen_mode_secondary, ready) {
            deviceAction(device, Action.SECONDARY_SCREEN)
        }
        addDetailAction(content, R.string.center_stream_settings, R.drawable.ic_center_sliders, true, streamSettings)
        addDetailAction(content, R.string.center_device_options, R.drawable.ic_more_vert, ready) {
            deviceAction(device, Action.DEVICE_OPTIONS)
        }
        val bar = row().apply { setPadding(dp(20), dp(10), dp(20), dp(10)) }
        bar.addView(button(R.string.center_connect_desktop, primary = true, enabled = ready) {
            deviceAction(device, Action.DESKTOP)
        }, LinearLayout.LayoutParams(-1, -2))
        footer(bar)
    }

    private fun showLanDetails(content: LinearLayout, computer: ComputerDetails) {
        content.addView(backLink(R.string.center_local_network) { backToDevices() }, LinearLayout.LayoutParams(-2, -2))
        gap(content, 8)
        val online = computer.state == ComputerDetails.State.ONLINE
        val paired = computer.pairState == PairState.PAIRED
        val (statusRes, dotColor) = lanStatusSpec(computer)

        val heading = row()
        heading.addView(label(0, 26, bold = true).apply { text = computer.name.orEmpty() }, LinearLayout.LayoutParams(0, -2, 1f))
        heading.addView(statusWithColor(statusRes, dotColor).apply {
            setPadding(dp(9), dp(4), dp(9), dp(4))
            background = shape(if (online && paired) greenFill else sky, radius = 7)
        }, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(10) })
        content.addView(heading)

        val addressText = bestLanAddress(computer)
        if (addressText.isNotEmpty()) {
            gap(content, 4)
            content.addView(label(0, 13, color = muted).apply { text = addressText })
        }

        gap(content, 16)
        content.addView(desktopCoverCard())
        gap(content, 20)
        content.addView(label(R.string.center_common_actions, 13, color = muted))
        gap(content, 10)

        val canStream = online && paired
        if (canStream) {
            addDetailAction(content, R.string.center_app_list, R.drawable.ic_center_apps, true) {
                lanDeviceAction(computer, LanAction.APPS)
            }
            addDetailAction(content, R.string.center_secondary_screen, R.drawable.ic_screen_mode_secondary, true) {
                lanDeviceAction(computer, LanAction.SECONDARY_SCREEN)
            }
        } else if (!online) {
            val canWake = !computer.macAddress.isNullOrBlank()
            addDetailAction(content, R.string.center_wol_action, R.drawable.ic_center_monitor, canWake) {
                lanDeviceAction(computer, LanAction.WAKE)
            }
        }
        addDetailAction(content, R.string.center_stream_settings, R.drawable.ic_center_sliders, true, streamSettings)
        addDetailAction(content, R.string.center_device_options, R.drawable.ic_more_vert, true) {
            lanDeviceAction(computer, LanAction.DEVICE_OPTIONS)
        }

        val bar = row().apply { setPadding(dp(20), dp(10), dp(20), dp(10)) }
        when {
            canStream -> bar.addView(button(R.string.center_connect_desktop, primary = true, enabled = true) {
                lanDeviceAction(computer, LanAction.DESKTOP)
            }, LinearLayout.LayoutParams(-1, -2))
            online && !paired -> bar.addView(button(R.string.center_pair_pc, primary = true, enabled = true) {
                lanDeviceAction(computer, LanAction.PAIR)
            }, LinearLayout.LayoutParams(-1, -2))
            else -> {
                val canWake = !computer.macAddress.isNullOrBlank()
                bar.addView(button(R.string.center_wake_pc, primary = true, enabled = canWake) {
                    lanDeviceAction(computer, LanAction.WAKE)
                }, LinearLayout.LayoutParams(-1, -2))
            }
        }
        footer(bar)
    }

    private fun bottomTabs(): LinearLayout = row().apply {
        setPadding(dp(8), 0, dp(8), 0)
        addView(
            tab(R.string.center_nav_devices, R.drawable.ic_center_monitor, active = currentPage == Page.CENTER) {
                switchPage(Page.CENTER)
            },
            LinearLayout.LayoutParams(0, -2, 1f)
        )
        addView(
            tab(R.string.center_nav_lan, R.drawable.ic_center_lan, active = currentPage == Page.LAN) {
                switchPage(Page.LAN)
            },
            LinearLayout.LayoutParams(0, -2, 1f)
        )
        addView(
            tab(R.string.center_nav_center, R.drawable.ic_center_server, listener = centerSettings),
            LinearLayout.LayoutParams(0, -2, 1f)
        )
        addView(
            tab(R.string.center_settings, R.drawable.ic_center_sliders, listener = streamSettings),
            LinearLayout.LayoutParams(0, -2, 1f)
        )
    }

    private fun backLink(textId: Int, onClick: () -> Unit) = label(textId, 14, bold = true, color = accent).apply {
        gravity = Gravity.CENTER_VERTICAL
        minHeight = dp(48)
        setPadding(dp(4), 0, dp(10), 0)
        compoundDrawablePadding = dp(5)
        setCompoundDrawablesRelative(glyph(R.drawable.ic_arrow_back_24, 16, accent), null, null, null)
        action(Color.TRANSPARENT, stroke = null, listener = onClick)
    }

    private fun desktopCoverCard(): LinearLayout = column().apply {
        background = shape(surface, line, radius = 14)
        clipToOutline = true
        addView(ImageView(context).apply {
            scaleType = ImageView.ScaleType.CENTER_CROP
            setImageResource(R.drawable.center_device_cover)
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }, LinearLayout.LayoutParams(-1, dp(171)))
        addView(row().apply {
            setPadding(dp(12), 0, dp(12), 0)
            addView(icon(R.drawable.ic_center_monitor))
            addView(label(R.string.center_remote_desktop, 15, bold = true))
        }, LinearLayout.LayoutParams(-1, dp(49)))
    }

    private fun addDetailAction(content: LinearLayout, name: Int, icon: Int, enabled: Boolean, listener: () -> Unit) {
        content.addView(button(name, enabled = enabled, listener = listener).apply {
            gravity = Gravity.CENTER_VERTICAL or Gravity.START
            setPadding(dp(12), dp(7), dp(12), dp(7))
            compoundDrawablePadding = dp(10)
            setCompoundDrawablesRelative(glyph(icon, 20, if (enabled) accent else muted), null,
                glyph(R.drawable.ic_arrow_right, 16, muted), null)
        }, LinearLayout.LayoutParams(-1, -2))
        gap(content, 8)
    }

    private fun footer(content: View) {
        root.addView(column().apply {
            setBackgroundColor(surface)
            addView(View(context).apply { setBackgroundColor(line) }, LinearLayout.LayoutParams(-1, dp(1)))
            addView(content, LinearLayout.LayoutParams(-1, -2))
        })
    }

    private fun deviceStatus(online: Boolean) = if (online) R.string.center_online else R.string.center_offline_short

    private fun lanStatusSpec(computer: ComputerDetails): Pair<Int, Int> = when (computer.state) {
        ComputerDetails.State.ONLINE -> when {
            computer.pairState != PairState.PAIRED -> R.string.center_unpaired to accent
            computer.runningGameId != 0 -> R.string.center_streaming to green
            else -> R.string.center_online to green
        }
        ComputerDetails.State.OFFLINE -> R.string.center_offline_short to muted
        else -> R.string.center_checking to muted
    }

    private fun bestLanAddress(computer: ComputerDetails): String {
        val tuple = computer.activeAddress
            ?: computer.localAddress
            ?: computer.manualAddress
            ?: computer.remoteAddress
            ?: computer.ipv6Address
            ?: return ""
        return tuple.address.orEmpty()
    }

    private fun status(textId: Int, online: Boolean) = statusWithColor(textId, if (online) green else muted)

    private fun statusWithColor(textId: Int, color: Int) = label(textId, 13, color = color).apply {
        val dot = shape(color).apply { setBounds(0, 0, dp(6), dp(6)) }
        compoundDrawablePadding = dp(6)
        setCompoundDrawablesRelative(dot, null, null, null)
    }

    private fun column() = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private fun row() = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun label(textId: Int, size: Int, bold: Boolean = false, color: Int = ink) = TextView(context).apply {
        if (textId != 0) setText(textId)
        textSize = size.toFloat()
        setTextColor(color)
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }

    private fun button(textId: Int, primary: Boolean = false, enabled: Boolean = true, listener: () -> Unit) =
        label(textId, 14, bold = true, color = if (primary && enabled) onAccent else if (enabled) ink else muted)
            .apply {
                gravity = Gravity.CENTER
                minHeight = dp(48)
                setPadding(dp(10), 0, dp(10), 0)
                if (enabled) action(if (primary) accent else surface, stroke = if (primary) null else line, listener = listener)
                else background = shape(sky, line)
                isEnabled = enabled
            }

    private fun tab(textId: Int, drawable: Int, active: Boolean = false, listener: () -> Unit) =
        label(textId, 12, bold = active, color = if (active) accent else muted).apply {
            gravity = Gravity.CENTER
            minHeight = dp(54)
            setPadding(dp(6), dp(6), dp(6), dp(6))
            compoundDrawablePadding = dp(4)
            setCompoundDrawablesRelative(null, glyph(drawable, 22, if (active) accent else muted), null, null)
            isSelected = active
            action(Color.TRANSPARENT, stroke = null, listener = listener)
        }

    private fun icon(drawable: Int, tile: Boolean = false) = ImageView(context).apply {
        val size = if (tile) 40 else 22
        layoutParams = LinearLayout.LayoutParams(dp(size), dp(size)).apply { marginEnd = dp(10) }
        if (tile) {
            setPadding(dp(9), dp(9), dp(9), dp(9))
            background = shape(sky)
        }
        setImageResource(drawable)
        imageTintList = ColorStateList.valueOf(accent)
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    private fun chevron() = ImageView(context).apply {
        layoutParams = LinearLayout.LayoutParams(dp(16), dp(16)).apply { marginStart = dp(8) }
        setImageDrawable(glyph(R.drawable.ic_arrow_right, 16, muted))
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    private fun glyph(drawable: Int, size: Int, tint: Int): Drawable? =
        ContextCompat.getDrawable(context, drawable)?.mutate()?.apply {
            setBounds(0, 0, dp(size), dp(size))
            setTint(tint)
        }

    private fun View.action(fill: Int, stroke: Int? = line, listener: () -> Unit) {
        background = RippleDrawable(rippleColor, shape(fill, stroke), shape(Color.WHITE))
        isFocusable = true
        stateListAnimator = null
        setOnClickListener { listener() }
        accessibilityDelegate = object : View.AccessibilityDelegate() {
            override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo) {
                super.onInitializeAccessibilityNodeInfo(host, info)
                info.className = Button::class.java.name
            }
        }
    }

    private fun shape(fill: Int, stroke: Int? = null, radius: Int = 10) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(radius).toFloat()
        if (stroke != null) setStroke(dp(1), stroke)
    }

    private fun gap(parent: LinearLayout, size: Int) {
        parent.addView(View(context), LinearLayout.LayoutParams(1, dp(size)))
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).roundToInt()

    private fun color(resource: Int) = ContextCompat.getColor(palette, resource)
}
