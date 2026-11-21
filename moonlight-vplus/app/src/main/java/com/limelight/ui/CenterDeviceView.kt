package com.limelight.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
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
import kotlin.math.roundToInt

internal class CenterDeviceView(
    context: Context,
    private val refresh: () -> Unit,
    private val centerSettings: () -> Unit,
    private val localNetwork: () -> Unit,
    private val streamSettings: () -> Unit,
    private val deviceAction: (CenterDirectory.Device, Action) -> Unit,
) : FrameLayout(context) {
    enum class Action { DESKTOP, APPS, SECONDARY_SCREEN, DEVICE_OPTIONS }

    private val ink = Color.rgb(25, 43, 65)
    private val muted = Color.rgb(83, 105, 129)
    private val line = Color.rgb(219, 228, 237)
    private val accent = Color.rgb(36, 101, 202)
    private val sky = Color.rgb(232, 241, 253)
    private val paper = Color.rgb(248, 250, 252)
    private val green = Color.rgb(36, 116, 82)
    private val root = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private var devices = emptyList<CenterDirectory.Device>()
    private var connected = false
    private var selectedId: String? = null
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
        render()
    }

    fun backToDevices(): Boolean {
        if (selectedId == null) return false
        selectedId = null
        render()
        return true
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
        val selected = devices.firstOrNull { it.id == selectedId }
        if (selected == null) {
            selectedId = null
            showDevices(content)
        } else {
            showDetails(content, selected)
        }
    }

    private fun showDevices(content: LinearLayout) {
        val header = row()
        header.addView(label(R.string.center_my_devices, 26, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
        header.addView(button(R.string.center_refresh, enabled = !loading, listener = refresh))
        content.addView(header)
        gap(content, 19)

        val center = row().apply {
            setPadding(dp(12), 0, dp(12), 0)
            action(sky, centerSettings)
        }
        center.addView(icon(R.drawable.ic_network_control))
        center.addView(label(R.string.center_server, 15), LinearLayout.LayoutParams(0, -2, 1f))
        center.addView(label(if (connected) R.string.center_connected else R.string.center_sign_in, 13,
            color = if (connected) green else accent))
        content.addView(center, LinearLayout.LayoutParams(-1, dp(50)))
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
                    action(Color.WHITE) {
                        selectedId = device.id
                        render()
                    }
                }
                tile.addView(icon(R.drawable.ic_computer))
                val name = column()
                name.addView(label(0, 15, bold = true).apply { text = device.name })
                name.addView(label(if (device.online) R.string.center_online else R.string.center_offline_short, 13,
                    color = if (device.online) green else muted))
                tile.addView(name, LinearLayout.LayoutParams(0, -2, 1f))
                tile.addView(label(0, 23, color = muted).apply { text = "›" })
                content.addView(tile, LinearLayout.LayoutParams(-1, dp(74)))
                gap(content, 9)
            }
        }
        gap(content, 14)
        content.addView(button(R.string.center_local_network, listener = localNetwork))
        val tabs = row().apply {
            setBackgroundColor(Color.WHITE)
            setPadding(dp(8), dp(6), dp(8), dp(6))
        }
        tabs.addView(tab(R.string.center_nav_devices, R.drawable.ic_computer, active = true, listener = refresh),
            LinearLayout.LayoutParams(0, -2, 1f))
        tabs.addView(tab(R.string.center_nav_center, R.drawable.ic_network_control, listener = centerSettings),
            LinearLayout.LayoutParams(0, -2, 1f))
        tabs.addView(tab(R.string.center_settings, android.R.drawable.ic_menu_preferences, listener = streamSettings),
            LinearLayout.LayoutParams(0, -2, 1f))
        root.addView(tabs)
    }

    private fun showDetails(content: LinearLayout, device: CenterDirectory.Device) {
        content.addView(button(R.string.center_my_devices) { backToDevices() })
        gap(content, 16)
        val heading = row()
        heading.addView(label(0, 26, bold = true).apply { text = device.name }, LinearLayout.LayoutParams(0, -2, 1f))
        heading.addView(label(if (device.online) R.string.center_online else R.string.center_offline_short, 13,
            color = if (device.online) green else muted))
        content.addView(heading)
        gap(content, 16)
        val desktop = column().apply { background = shape(Color.WHITE, line) }
        desktop.addView(ImageView(context).apply {
            scaleType = ImageView.ScaleType.CENTER_CROP
            setImageResource(R.drawable.center_device_cover)
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }, LinearLayout.LayoutParams(-1, dp(171)))
        desktop.addView(row().apply {
            setPadding(dp(12), 0, dp(12), 0)
            addView(icon(R.drawable.ic_computer))
            addView(label(R.string.center_remote_desktop, 15, bold = true))
        }, LinearLayout.LayoutParams(-1, dp(49)))
        content.addView(desktop)
        gap(content, 20)
        content.addView(label(R.string.center_common_actions, 13, color = muted))
        gap(content, 10)
        val ready = device.online && !loading
        addDetailAction(content, R.string.center_app_list, ready) { deviceAction(device, Action.APPS) }
        addDetailAction(content, R.string.pcview_menu_secondary_screen, ready) { deviceAction(device, Action.SECONDARY_SCREEN) }
        addDetailAction(content, R.string.center_stream_settings, true, streamSettings)
        addDetailAction(content, R.string.center_device_options, ready) { deviceAction(device, Action.DEVICE_OPTIONS) }
        val bar = row().apply { setPadding(dp(20), dp(9), dp(20), dp(12)) }
        bar.addView(button(R.string.center_connect_desktop, primary = true, enabled = ready) {
            deviceAction(device, Action.DESKTOP)
        }, LinearLayout.LayoutParams(-1, dp(47)))
        root.addView(bar)
    }

    private fun addDetailAction(content: LinearLayout, name: Int, enabled: Boolean, listener: () -> Unit) {
        content.addView(button(name, enabled = enabled, listener = listener).apply {
            gravity = Gravity.CENTER_VERTICAL or Gravity.START
        }, LinearLayout.LayoutParams(-1, dp(48)))
        gap(content, 8)
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
        label(textId, 14, bold = true, color = if (primary && enabled) Color.WHITE else if (enabled) ink else muted)
            .apply {
                gravity = Gravity.CENTER
                minHeight = dp(42)
                setPadding(dp(10), 0, dp(10), 0)
                if (enabled) action(if (primary) accent else Color.WHITE, listener)
                else background = shape(Color.rgb(238, 242, 246), line)
                isEnabled = enabled
            }

    private fun tab(textId: Int, drawable: Int, active: Boolean = false, listener: () -> Unit) =
        label(textId, 12, bold = active, color = if (active) accent else muted).apply {
            gravity = Gravity.CENTER
            setPadding(dp(6), dp(4), dp(6), dp(4))
            val icon = ContextCompat.getDrawable(context, drawable)?.mutate()
            icon?.setBounds(0, 0, dp(20), dp(20))
            icon?.setTint(if (active) accent else muted)
            compoundDrawablePadding = dp(4)
            setCompoundDrawablesRelative(null, icon, null, null)
            action(Color.WHITE, listener)
        }

    private fun icon(drawable: Int) = ImageView(context).apply {
        layoutParams = LinearLayout.LayoutParams(dp(38), dp(38)).apply { marginEnd = dp(10) }
        setPadding(dp(8), dp(8), dp(8), dp(8))
        background = shape(sky)
        setImageResource(drawable)
        imageTintList = ColorStateList.valueOf(accent)
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    private fun View.action(fill: Int, listener: () -> Unit) {
        background = RippleDrawable(ColorStateList.valueOf(0x272465ca), shape(fill, line), null)
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

    private fun shape(fill: Int, stroke: Int? = null) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(10).toFloat()
        if (stroke != null) setStroke(dp(1), stroke)
    }

    private fun gap(parent: LinearLayout, size: Int) {
        parent.addView(View(context), LinearLayout.LayoutParams(1, dp(size)))
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).roundToInt()
}
